#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Audio output pipeline (phase 4: P0-5/P0-6/P0-7/P0-8/P0-12 fixed).
 *
 * I2S master (standard mode) feeding the external DAC through a large
 * SPSC ring buffer. NOT started at boot: a session (phase 12) calls
 * start() and only marks itself active when start() returns ESP_OK
 * (P0-6).
 *
 * ------------------------------------------------------------------
 * Threading model (P0-8 fixed)
 * ------------------------------------------------------------------
 * The ring buffer is SPSC: exactly ONE producer task (the session socket
 * task of phase 12, via michi_audio_output_write()) and ONE consumer
 * task (the internal I2S task). All head/tail/used updates happen under
 * a FreeRTOS portMUX critical section.
 *
 * Why critical sections instead of C11 atomics: on Xtensa the pair of
 * indices (head written by the producer, tail by the consumer) needs
 * both ordering AND mutual exclusion for the wrap-around copy; a
 * portENTER_CRITICAL/portEXIT_CRITICAL pair provides both with the
 * FreeRTOS-idiomatic pattern and is held only for a bounded ~4 KiB
 * memcpy (~microseconds), negligible against the millisecond-scale DMA
 * buffers. C11 atomics would be marginally faster but add memory-order
 * reasoning with no measurable benefit at this data rate. Correctness
 * first; phase 11 may optimize.
 *
 * ------------------------------------------------------------------
 * Lifecycle (P0-7 fixed by construction)
 * ------------------------------------------------------------------
 *   init():   allocate the ring, create the I2S channel and init standard
 *             mode. The channel is CREATED but NOT enabled; s_tx stays
 *             valid until deinit().
 *   start():  flush the ring (stale audio never plays), set the run
 *             flags, enable the LIVE channel, then create the I2S task.
 *             Repeatable: start() after a successful stop() re-enables
 *             the same channel (no delete/recreate, no use-after-free).
 *   stop():   run flag + xTaskNotify + i2s_channel_disable (unblocks
 *             in-flight writes) + join on a done flag with a timeout.
 *             The channel is DISABLED but NOT deleted. On ESP_OK the
 *             task handle/done flag are reset. On ESP_ERR_TIMEOUT
 *             nothing is reset - the task may still be alive - and the
 *             subsystem requires deinit after timeout: retry stop()
 *             until ESP_OK, then deinit().
 *   deinit(): refuses (ESP_ERR_INVALID_STATE) while the I2S task may
 *             still be alive (task handle or done flag still set) -
 *             never free under a live task; otherwise deletes the
 *             channel (also after init() without start()), frees the
 *             ring and returns to the uninitialized state.
 *
 * Tasks are NEVER deleted from outside. The I2S task self-deletes AFTER
 * releasing its resources; no external vTaskDelete ever, no dangling
 * handles (s_task is only NULLed once the done flag was observed).
 *
 * ------------------------------------------------------------------
 * Error propagation (P0-5 fixed)
 * ------------------------------------------------------------------
 * Every failure (malloc, i2s_new_channel, i2s_channel_enable, task
 * creation) is propagated to the caller as an esp_err_t and cleaned up -
 * no ESP_ERROR_CHECK, no swallowed errors, no half-initialized state
 * behind a "success".
 *
 * ------------------------------------------------------------------
 * Volume (P0-12 delegated)
 * ------------------------------------------------------------------
 * The I2S task applies michi_volume_apply() to every dequeued chunk
 * before i2s_channel_write() (digital gain, or no-op when the DAC has
 * hardware volume). The API layer must answer with michi_volume_get().
 */

typedef struct {
    uint32_t sample_rate;    /*!< I2S sample rate in Hz (8000..96000) */
    uint8_t  bit_depth;      /*!< 16 or 24 */
    uint8_t  channels;       /*!< 1 or 2 */
    uint16_t buffer_ms;      /*!< Ring prefill before the first I2S write (10..1000) */
    uint16_t ring_buffer_kb; /*!< Ring size in KiB, allocated in PSRAM.
                              *   0 = CONFIG_MICHI_AUDIO_RING_BUFFER_KB default;
                              *   else 16..16384, power of two */
    int bclk;                /*!< I2S BCLK pin, from michi_board_get_external_pins() */
    int lrck;                /*!< I2S LRCK pin */
    int din;                 /*!< I2S DIN pin */
    int mclk;                /*!< I2S MCLK pin, -1 when unused */
} michi_audio_output_config_t;

/**
 * @brief Validate the config, allocate the SPSC ring buffer (PSRAM) and
 *        create + init (NOT enable) the I2S TX channel (standard mode).
 *
 * Does NOT start any task and does NOT touch the DAC. The channel stays
 * created (disabled) until deinit().
 *
 * @return ESP_OK; ESP_ERR_INVALID_ARG on a bad config (rate/depth/
 *         channels/pins/ring size/prefill vs ring size); ESP_ERR_NO_MEM
 *         when the ring cannot be allocated in PSRAM; I2S errors
 *         propagated with the partially created resources already
 *         cleaned up.
 */
esp_err_t michi_audio_output_init(const michi_audio_output_config_t *cfg);

/**
 * @brief Start the pipeline: flush the ring, enable the live channel
 *        and create the I2S task. Only valid after a successful init();
 *        only ONE running instance (double start is an error).
 *
 * Repeatable: after a successful stop() the same (still created) channel
 * is re-enabled - no delete/recreate. On any failure the flags are
 * reverted and the error is propagated.
 *
 * @return ESP_OK; ESP_ERR_INVALID_STATE when not initialized or already
 *         running; i2s_channel_enable/task errors propagated.
 */
esp_err_t michi_audio_output_start(void);

/**
 * @brief Enqueue PCM into the ring buffer (SPSC producer side).
 *
 * Blocks (polling the ring) until the whole buffer is enqueued or the
 * pipeline stops. MUST be called from a single task (see threading
 * model). Applies no volume here: gain is applied by the consumer.
 *
 * @param data Raw PCM (little-endian samples, standard I2S order).
 * @param len  Byte count.
 * @return ESP_OK; ESP_ERR_INVALID_ARG on NULL/zero; ESP_ERR_INVALID_STATE
 *         when not running (or stopped mid-write).
 */
esp_err_t michi_audio_output_write(const uint8_t *data, size_t len);

/**
 * @brief Cooperative stop: flag + notify + i2s_channel_disable + join
 *        with timeout. The channel is DISABLED but NOT deleted (it is
 *        deleted only by deinit()). Idempotent when not running.
 *
 * @return ESP_OK; ESP_ERR_INVALID_STATE when never initialized;
 *         ESP_ERR_TIMEOUT when the task did not self-delete within the
 *         join window - the channel is NOT deleted and the task handle
 *         is NOT reset: the subsystem requires deinit after timeout
 *         (retry stop() until ESP_OK, then deinit()).
 */
esp_err_t michi_audio_output_stop(void);

/**
 * @brief Delete the I2S channel and free the ring buffer.
 *
 * Refuses while the I2S task may still be alive (task handle or done
 * flag set) - never free under a live task. Works after init() without
 * start() and after a successful stop().
 *
 * @return ESP_OK; ESP_ERR_INVALID_STATE when a task may still be alive
 *         (stop() must have returned ESP_OK first).
 */
esp_err_t michi_audio_output_deinit(void);

/**
 * @return true while the pipeline is running (started, not stopped).
 */
bool michi_audio_output_is_running(void);

/**
 * @brief Get the I2S error counter (phase 14 diagnostics).
 *
 * Counts the failures that today are only logged: i2s_channel_write
 * failures in the consumer task (transient: the chunk is dropped and the
 * pipeline keeps moving) and i2s_channel_disable failures in stop().
 * Session-ending pipeline rejections are NOT counted here - the RTP
 * engine posts MICHI_EVENT_ERROR for those (michi_state last-error slot).
 *
 * @param out Receives the error count.
 * @return ESP_OK; ESP_ERR_INVALID_ARG on NULL out.
 */
esp_err_t michi_audio_output_get_error_count(uint32_t *out);

#ifdef __cplusplus
}
#endif
