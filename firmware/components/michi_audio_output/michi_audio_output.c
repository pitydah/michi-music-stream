/*
 * Audio output pipeline (phase 4). P0-fixed by construction:
 *  - P0-5: every failure (malloc/i2s/bind/task) is propagated and cleaned
 *    up; no ESP_ERROR_CHECK, no swallowed errors.
 *  - P0-6: start() returns an error on any failure; the phase-12 session
 *    layer only marks the session active when start() returned ESP_OK.
 *  - P0-7: cooperative shutdown only. stop() = run flag + xTaskNotify +
 *    i2s_channel_disable (unblocks in-flight writes) + join on a done
 *    flag with timeout. The I2S task self-deletes after releasing its
 *    resources; no external vTaskDelete ever, no dangling handles. The
 *    channel is created (NOT enabled) at init(), enabled at start() and
 *    DISABLED (not deleted) at stop(); i2s_del_channel happens only in
 *    deinit(), once no task may be alive. start() after stop() re-enables
 *    the live channel (no UAF under a live task); deinit() after
 *    init()-without-start() deletes the channel.
 *  - P0-8: SPSC ring buffer; every head/tail/used update runs under a
 *    portMUX critical section (see header for why not atomics).
 *  - P0-12: digital volume applied by the consumer task via
 *    michi_volume_apply() before every i2s_channel_write.
 */

#include <inttypes.h>
#include <string.h>

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "soc/gpio_num.h"

#include "michi_audio_output.h"
#include "michi_volume.h"

#define TAG "michi_audio"

#define MICHI_AUDIO_CHUNK_BYTES 4092  /* max bytes under one critical section;
                                       * multiple of 2 AND 3 (4092/3 = 1364)
                                       * so any 16/24-bit chunk is sample-aligned;
                                       * the consumer chunk is used as-is */
#define MICHI_AUDIO_TASK_STACK  4096
#define MICHI_AUDIO_TASK_PRIO   8
#define MICHI_AUDIO_I2S_WRITE_TIMEOUT_MS 100  /* bounded: task always wakes */
#define MICHI_AUDIO_JOIN_TIMEOUT_MS 2000

typedef struct {
    uint8_t *buf;         /* ring buffer (PSRAM) */
    size_t   size;        /* ring size in bytes, power of two (masked) */
    size_t   head;        /* producer index, masked, written by the producer only */
    size_t   tail;        /* consumer index, masked, written by the consumer only */
    size_t   used;        /* bytes in the ring, a plain counter */
} ring_t;

static ring_t s_ring = {0};
static portMUX_TYPE s_ring_lock = portMUX_INITIALIZER_UNLOCKED;

static i2s_chan_handle_t s_tx = NULL;
static volatile TaskHandle_t s_task = NULL;
static volatile bool s_inited = false;
static volatile bool s_running = false;
static volatile bool s_run = false;       /* cooperative: read by the task */
static volatile bool s_task_done = false; /* set by the task before self-delete */
static volatile bool s_consumer_sleeping = false; /* set by the task before sleep,
                                                   * read by the producer (F9) */

static size_t s_prefill_bytes = 0;
static uint8_t s_bit_depth = 16;
static uint8_t s_chunk[MICHI_AUDIO_CHUNK_BYTES] __attribute__((aligned(4)));

/* F14 diagnostics: I2S error counter, incremented where the failures are
 * logged (i2s_channel_write in the consumer task, i2s_channel_disable in
 * stop()). Written from two contexts (task + caller), read from any task:
 * every access runs under s_err_lock. */
static volatile uint32_t s_error_count = 0;
static portMUX_TYPE s_err_lock = portMUX_INITIALIZER_UNLOCKED;

static void err_count_inc(void)
{
    portENTER_CRITICAL(&s_err_lock);
    s_error_count++;
    portEXIT_CRITICAL(&s_err_lock);
}

/* ------------------------------------------------------------------
 * Ring (SPSC: producer writes head, consumer writes tail; all access
 * under s_ring_lock). Indices are masked to the (power-of-two) size;
 * `used` is a plain counter that never wraps.
 * ------------------------------------------------------------------ */

static size_t ring_used(const ring_t *r)
{
    return r->used;
}

static size_t ring_write(ring_t *r, const uint8_t *data, size_t len)
{
    size_t written = 0;
    bool wake = false;
    portENTER_CRITICAL(&s_ring_lock);
    size_t free = r->size - ring_used(r);
    if (free > len) {
        free = len;
    }
    if (free > MICHI_AUDIO_CHUNK_BYTES) {
        free = MICHI_AUDIO_CHUNK_BYTES; /* bounded critical section */
    }
    size_t first = r->size - r->head;
    size_t n = free < first ? free : first;
    memcpy(r->buf + r->head, data, n);
    if (free > n) {
        memcpy(r->buf, data + n, free - n);
    }
    r->head = (r->head + free) & (r->size - 1);
    r->used += free;
    written = free;
    /* F9: if the consumer is asleep, wake it right away (cheap; the
     * 100 ms notification timeout stays as a backstop). */
    wake = free > 0 && s_task != NULL && s_consumer_sleeping;
    portEXIT_CRITICAL(&s_ring_lock);
    if (wake) {
        xTaskNotifyGive(s_task);
    }
    return written;
}

static size_t ring_read(ring_t *r, uint8_t *out, size_t len)
{
    size_t got = 0;
    portENTER_CRITICAL(&s_ring_lock);
    size_t used = ring_used(r);
    if (used > len) {
        used = len;
    }
    if (used > MICHI_AUDIO_CHUNK_BYTES) {
        used = MICHI_AUDIO_CHUNK_BYTES;
    }
    size_t first = r->size - r->tail;
    size_t n = used < first ? used : first;
    memcpy(out, r->buf + r->tail, n);
    if (used > n) {
        memcpy(out + n, r->buf, used - n);
    }
    r->tail = (r->tail + used) & (r->size - 1);
    r->used -= used;
    got = used;
    portEXIT_CRITICAL(&s_ring_lock);
    return got;
}

/* ------------------------------------------------------------------
 * I2S consumer task (the only reader; self-deletes on shutdown)
 * ------------------------------------------------------------------ */

static void i2s_task(void *arg)
{
    /* Prefill: do not start DMA until buffer_ms of audio is buffered
     * (avoids the first underruns on session start). s_run is already
     * true when the task starts (start() sets it first); if stop()
     * happened before this task ran, the notification is pending and
     * the take below returns immediately -> shutdown. */
    for (;;) {
        size_t used = 0;
        portENTER_CRITICAL(&s_ring_lock);
        used = ring_used(&s_ring);
        portEXIT_CRITICAL(&s_ring_lock);
        if (used >= s_prefill_bytes) {
            break;
        }
        s_consumer_sleeping = true;
        uint32_t notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
        s_consumer_sleeping = false;
        if (notified > 0 && !s_run) {
            goto shutdown;
        }
    }

    while (s_run) {
        size_t n = ring_read(&s_ring, s_chunk, sizeof(s_chunk));
        if (n == 0) {
            /* Ring empty: sleep on the notification (stop() uses it too;
             * the producer also notifies when it writes to a sleeping
             * consumer - F9). */
            s_consumer_sleeping = true;
            uint32_t notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
            s_consumer_sleeping = false;
            if (notified > 0 && !s_run) {
                break;
            }
            continue;
        }
        /* P0-12: digital gain here (no-op when DAC hardware volume). */
        michi_volume_apply(s_chunk, n, s_bit_depth);
        size_t written = 0;
        esp_err_t err = i2s_channel_write(s_tx, s_chunk, n, &written,
                                          MICHI_AUDIO_I2S_WRITE_TIMEOUT_MS);
        if (err != ESP_OK) {
            if (!s_run) {
                break; /* stop() disabled the channel: silent teardown */
            }
            /* Data was dequeued but could not be written: drop it and
             * keep the pipeline moving. */
            ESP_LOGW(TAG, "i2s_channel_write failed: %s", esp_err_to_name(err));
            err_count_inc();
        }
        if (!s_run) {
            break;
        }
    }

shutdown:
    /* Cooperative: the task releases its own resources (stack) and
     * signals, then self-deletes. stop() never vTaskDelete()s from the
     * outside. */
    s_task_done = true;
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------ */

static bool pin_ok(int pin)
{
    return pin >= 0 && pin < GPIO_NUM_MAX;
}

esp_err_t michi_audio_output_init(const michi_audio_output_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_inited) {
        return ESP_ERR_INVALID_STATE; /* init -> deinit -> init required */
    }
    if (cfg->sample_rate < 8000 || cfg->sample_rate > 96000) {
        ESP_LOGE(TAG, "init: sample_rate %" PRIu32 " out of range (8000..96000)",
                 cfg->sample_rate);
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->bit_depth != 16 && cfg->bit_depth != 24) {
        ESP_LOGE(TAG, "init: bit_depth %u not supported (16, 24)", cfg->bit_depth);
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->channels < 1 || cfg->channels > 2) {
        ESP_LOGE(TAG, "init: channels %u out of range (1..2)", cfg->channels);
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->buffer_ms < 10 || cfg->buffer_ms > 1000) {
        ESP_LOGE(TAG, "init: buffer_ms %u out of range (10..1000)", cfg->buffer_ms);
        return ESP_ERR_INVALID_ARG;
    }
    uint16_t ring_kb = cfg->ring_buffer_kb;
    if (ring_kb == 0) {
        /* 0 = the Kconfig default (the option would be dead weight
         * otherwise; documented in the Kconfig help). */
        ring_kb = CONFIG_MICHI_AUDIO_RING_BUFFER_KB;
    }
    if (ring_kb < 16 || ring_kb > 16384 || (ring_kb & (ring_kb - 1)) != 0) {
        /* Power of two is required: the ring indices are masked with
         * (size - 1) (F3). The Kconfig range is 16..16384 KiB. */
        ESP_LOGE(TAG, "init: ring_buffer_kb %u out of range "
                      "(16..16384 KiB, power of two required)",
                 (unsigned)ring_kb);
        return ESP_ERR_INVALID_ARG;
    }
    if (!pin_ok(cfg->bclk) || !pin_ok(cfg->lrck) || !pin_ok(cfg->din) ||
        (cfg->mclk != -1 && !pin_ok(cfg->mclk))) {
        ESP_LOGE(TAG, "init: invalid I2S pin (bclk=%d lrck=%d din=%d mclk=%d)",
                 cfg->bclk, cfg->lrck, cfg->din, cfg->mclk);
        return ESP_ERR_INVALID_ARG;
    }

    size_t ring_size = (size_t)ring_kb * 1024;
    size_t bytes_per_ms = (size_t)cfg->sample_rate * cfg->channels *
                          (cfg->bit_depth / 8) / 1000;
    s_prefill_bytes = bytes_per_ms * cfg->buffer_ms;
    if (s_prefill_bytes >= ring_size) {
        ESP_LOGE(TAG, "init: prefill %u bytes (%ums) does not fit the ring (%u bytes)",
                 (unsigned)s_prefill_bytes, cfg->buffer_ms, (unsigned)ring_size);
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t *ring = heap_caps_malloc(ring_size, MALLOC_CAP_SPIRAM);
    if (ring == NULL) {
        ESP_LOGE(TAG, "init: ring allocation failed (%u bytes in PSRAM)",
                 (unsigned)ring_size);
        return ESP_ERR_NO_MEM;
    }
    s_ring.buf = ring;
    s_ring.size = ring_size;
    s_ring.head = 0;
    s_ring.tail = 0;
    s_ring.used = 0;
    s_bit_depth = cfg->bit_depth;

    /* P0-5: every error below is propagated AND cleaned up. */
    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = 6,
        .dma_frame_num = 240,
        .auto_clear = true,
        .auto_clear_before_cb = false,
    };
    i2s_chan_handle_t tx = NULL;
    esp_err_t err = i2s_new_channel(&chan_cfg, &tx, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init: i2s_new_channel failed: %s", esp_err_to_name(err));
        goto fail_ring;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(cfg->sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            cfg->bit_depth == 24 ? I2S_DATA_BIT_WIDTH_24BIT : I2S_DATA_BIT_WIDTH_16BIT,
            cfg->channels == 2 ? I2S_SLOT_MODE_STEREO : I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = cfg->mclk < 0 ? I2S_GPIO_UNUSED : (gpio_num_t)cfg->mclk,
            .bclk = (gpio_num_t)cfg->bclk,
            .ws = (gpio_num_t)cfg->lrck,
            .dout = (gpio_num_t)cfg->din,
            .invert_flags = {0},
        },
    };
    err = i2s_channel_init_std_mode(tx, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init: i2s_channel_init_std_mode failed: %s",
                 esp_err_to_name(err));
        i2s_del_channel(tx);
        goto fail_ring;
    }

    s_tx = tx;
    s_inited = true;
    ESP_LOGI(TAG, "init: ring=%u bytes (PSRAM) prefill=%u bytes "
                  "rate=%" PRIu32 " depth=%u ch=%u",
             (unsigned)ring_size, (unsigned)s_prefill_bytes,
             cfg->sample_rate, cfg->bit_depth, cfg->channels);
    return ESP_OK;

fail_ring:
    heap_caps_free(ring);
    s_ring.buf = NULL;
    s_ring.size = 0;
    return err;
}

esp_err_t michi_audio_output_start(void)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_running) {
        ESP_LOGE(TAG, "start: already running");
        return ESP_ERR_INVALID_STATE;
    }
    /* 1) Flush the ring: stale audio from a previous session must never
     *    play. */
    portENTER_CRITICAL(&s_ring_lock);
    s_ring.head = 0;
    s_ring.tail = 0;
    s_ring.used = 0;
    portEXIT_CRITICAL(&s_ring_lock);
    /* 2) Flags BEFORE creating the task: the task reads s_run at its
     *    very first wake. */
    s_run = true;
    s_running = true;
    /* 3) Enable the LIVE channel (created at init, deleted only at
     *    deinit): start() after stop() re-enables it, no delete, no UAF. */
    esp_err_t err = i2s_channel_enable(s_tx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start: i2s_channel_enable failed: %s", esp_err_to_name(err));
        s_run = false;
        s_running = false;
        return err; /* P0-6: session layer only marks active on ESP_OK */
    }
    /* 4) Create the consumer task. */
    TaskHandle_t task = NULL;
    err = xTaskCreate(i2s_task, "michi_i2s", MICHI_AUDIO_TASK_STACK,
                      NULL, MICHI_AUDIO_TASK_PRIO, &task);
    if (err != pdPASS) {
        ESP_LOGE(TAG, "start: task creation failed");
        i2s_channel_disable(s_tx);
        s_run = false;
        s_running = false;
        return ESP_ERR_NO_MEM;
    }
    s_task = task;
    ESP_LOGI(TAG, "start: pipeline running");
    return ESP_OK;
}

esp_err_t michi_audio_output_write(const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_running) {
        return ESP_ERR_INVALID_STATE;
    }
    size_t off = 0;
    while (off < len) {
        if (!s_running) {
            return ESP_ERR_INVALID_STATE; /* stopped mid-write: no partial
                                           * success lies */
        }
        size_t n = ring_write(&s_ring, data + off, len - off);
        off += n;
        if (off < len) {
            /* Ring full: wait for the consumer to drain. Polling is the
             * simple correct SPSC flow control; phase 11 may switch to
             * notification-based pacing. */
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
    return ESP_OK;
}

esp_err_t michi_audio_output_stop(void)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_running) {
        return ESP_OK; /* idempotent */
    }

    /* 1) Stop the flag (the task checks it on every wake). */
    s_run = false;
    /* 2) Wake the task (ring-empty sleep AND prefill wait both use the
     *    notification). */
    if (s_task != NULL) {
        xTaskNotifyGive(s_task);
    }
    /* 3) Unblock any in-flight i2s_channel_write immediately. */
    esp_err_t err = i2s_channel_disable(s_tx);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "stop: i2s_channel_disable failed: %s", esp_err_to_name(err));
        err_count_inc();
    }
    /* 4) Join on the done flag with a timeout. The task sets it right
     *    before self-deleting; after observing it, s_task is stale and is
     *    never touched again (no dangling handle). */
    int waited_ms = 0;
    while (!s_task_done && waited_ms < MICHI_AUDIO_JOIN_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(10));
        waited_ms += 10;
    }
    if (!s_task_done) {
        /* The task may still be alive: the channel is NOT deleted and
         * s_task/s_task_done are NOT reset - the subsystem requires
         * deinit after timeout. deinit() refuses while a task may be
         * alive, so the caller must retry stop() until ESP_OK, then
         * deinit(). */
        ESP_LOGE(TAG, "stop: task did not self-delete within %d ms - "
                      "subsystem requires deinit after timeout",
                 MICHI_AUDIO_JOIN_TIMEOUT_MS);
        return ESP_ERR_TIMEOUT;
    }
    s_task = NULL;
    s_task_done = false;
    s_running = false;
    s_run = false;
    ESP_LOGI(TAG, "stop: pipeline stopped");
    return ESP_OK;
}

esp_err_t michi_audio_output_deinit(void)
{
    /* Never free under a live task: if the handle or the done flag is
     * still set, the I2S task may exist (or have just exited) - only
     * stop() with ESP_OK clears both. */
    if (s_task != NULL || s_task_done) {
        ESP_LOGE(TAG, "deinit: I2S task may still be alive - call stop() "
                      "until it returns ESP_OK first");
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_inited) {
        return ESP_OK; /* idempotent */
    }
    /* The channel is deleted here and only here (also after
     * init()-without-start(): it was created but never enabled). */
    esp_err_t err = i2s_del_channel(s_tx);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "deinit: i2s_del_channel failed: %s", esp_err_to_name(err));
    }
    s_tx = NULL;
    if (s_ring.buf != NULL) {
        heap_caps_free(s_ring.buf);
        s_ring.buf = NULL;
        s_ring.size = 0;
        s_ring.used = 0;
    }
    s_inited = false;
    ESP_LOGI(TAG, "deinit: resources released");
    return ESP_OK;
}

bool michi_audio_output_is_running(void)
{
    return s_running;
}

esp_err_t michi_audio_output_get_error_count(uint32_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_err_lock);
    *out = s_error_count;
    portEXIT_CRITICAL(&s_err_lock);
    return ESP_OK;
}
