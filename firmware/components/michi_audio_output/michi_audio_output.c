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
#include <stdatomic.h>
#include <string.h>

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

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
#define MICHI_AUDIO_I2S_WRITE_TIMEOUT_MS 100  /* bounded: task always wakes (ms) */
#define MICHI_AUDIO_I2S_SILENCE_TIMEOUT_MS 50 /* explicit ms (not ticks) */
#define MICHI_AUDIO_JOIN_TIMEOUT_MS 2000
/* H9: producer must never block forever if the I2S worker stops consuming.
 * The bound is spent across the whole write (not reset on occasional progress).
 * It does not impose a timeout on OTA, pairing, or other control operations. */
#define MICHI_AUDIO_BACKPRESSURE_MAX_MS 1500
/* H9: transient DMA errors are counted, consecutive failures are fatal.
 * A stream that cannot reach the DAC must not advertise active output. */
#define MICHI_AUDIO_DMA_FATAL_STREAK 3

typedef enum {
    MICHI_AUDIO_CMD_NONE = 0,
    MICHI_AUDIO_CMD_QUIESCE,
    MICHI_AUDIO_CMD_RESUME,
    MICHI_AUDIO_CMD_STOP,
} michi_audio_cmd_t;

typedef struct {
    uint8_t *buf;         /* ring buffer (PSRAM) */
    size_t   size;        /* ring size in bytes, power of two (masked) */
    size_t   head;        /* producer index, masked, written by the producer only */
    size_t   tail;        /* consumer index, masked, written by the consumer only */
    size_t   used;        /* bytes in the ring, a plain counter */
} ring_t;

static ring_t s_ring = {0};
static portMUX_TYPE s_ring_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_ring_write_barrier = false;

static i2s_chan_handle_t s_tx = NULL;
static TaskHandle_t s_task = NULL;
static bool s_inited = false;
static bool s_run = false;       /* cooperative: read by the task */
static bool s_task_done = false; /* set by the task before self-delete */
/* H10: if channel deletion fails, the channel and ring are still owned here.
 * Do not allow start() until a retry deinit successfully releases them. */
static bool s_channel_delete_failed = false;
/* H11: Lifecycle operations are serialized by a non-blocking lease. This
 * protects driver ownership across calls that release s_state_lock while
 * invoking ESP-IDF (enable/disable/delete), without holding a spinlock during
 * potentially blocking driver calls. A concurrent lifecycle request fails
 * explicitly instead of racing the I2S handle or semaphores. */
static portMUX_TYPE s_lifecycle_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_lifecycle_busy = false;
/* Accessed only by the current lifecycle lease holder. Distinguishes a
 * stopped worker from a genuinely DISABLED hardware channel, including
 * STOP timeout/retry and start rollback paths. */
static bool s_channel_enabled = false;
/* Producers can outlive a STOP request. Prevent DEINIT or a new START
 * from freeing/reusing the ring while a previously admitted write still
 * owns its memory. Protected by s_state_lock, not the lifecycle lease. */
static uint32_t s_write_inflight = 0;
/* H13: a control caller may be between checking a command semaphore and
 * acquiring it. STOP can finish while such a caller still holds a reference.
 * Deinit must never delete either semaphore until every admitted caller has
 * returned. Protected by s_state_lock; no locks are held while waiting. */
static uint32_t s_control_inflight = 0;

static bool lifecycle_try_enter(void)
{
    portENTER_CRITICAL(&s_lifecycle_lock);
    bool acquired = !s_lifecycle_busy;
    if (acquired) {
        s_lifecycle_busy = true;
    }
    portEXIT_CRITICAL(&s_lifecycle_lock);
    return acquired;
}

static void lifecycle_leave(void)
{
    portENTER_CRITICAL(&s_lifecycle_lock);
    s_lifecycle_busy = false;
    portEXIT_CRITICAL(&s_lifecycle_lock);
}

static michi_audio_output_state_t s_state = MICHI_AUDIO_STATE_UNINITIALIZED;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_notify_inflight = 0;
static bool s_notify_closed = false;

static michi_audio_cmd_t s_pending_cmd = MICHI_AUDIO_CMD_NONE;
static esp_err_t s_cmd_result = ESP_OK;
static uint32_t s_cmd_generation = 0;
static uint32_t s_cmd_ack_generation = 0;
static SemaphoreHandle_t s_cmd_mux = NULL;
static SemaphoreHandle_t s_cmd_ack_sem = NULL;

#ifdef MICHI_HOST_TEST
static uint32_t s_cmd_timeout_override_ms = 0;
static uint32_t s_join_timeout_override_ms = 0;
static _Atomic bool s_test_hold_worker = false;
static _Atomic bool s_test_hold_producer = false;
/* Deterministic race gate: retain control ownership after command ACK and
 * before wrapper releases its reference (not in production firmware). */
static _Atomic bool s_test_hold_control_exit = false;
static _Atomic bool s_test_control_exit_entered = false;
static _Atomic bool s_test_producer_in_hold = false;
static _Atomic uint32_t s_test_quiesce_barrier_race_count = 0;
#endif

static size_t s_prefill_bytes = 0;
static uint8_t s_bit_depth = 16;
static size_t s_frame_bytes = 4; /* interleaved PCM frame (sample bytes * channels) */

/* Single I2S owner contract (Wave B):
 * s_chunk is strictly owned by the i2s_task worker.
 * No outside function or task may touch s_chunk, zero it, or call i2s_channel_write().
 */
static uint8_t s_chunk[MICHI_AUDIO_CHUNK_BYTES] __attribute__((aligned(4)));

/* F14 diagnostics: I2S error counter, incremented where the failures are
 * logged (i2s_channel_write in the consumer task, i2s_channel_disable in
 * stop()). Written from two contexts (task + caller), read from any task:
 * every access runs under s_err_lock. */
static uint32_t s_error_count = 0;
static uint32_t s_short_write_count = 0;
static uint32_t s_backpressure_timeout_count = 0;
static uint32_t s_dma_fault_count = 0;
static portMUX_TYPE s_err_lock = portMUX_INITIALIZER_UNLOCKED;

static void err_count_inc(void)
{
    portENTER_CRITICAL(&s_err_lock);
    s_error_count++;
    portEXIT_CRITICAL(&s_err_lock);
}

static void short_write_inc(void)
{
    portENTER_CRITICAL(&s_err_lock);
    s_short_write_count++;
    portEXIT_CRITICAL(&s_err_lock);
}

static void dma_fault_inc(void)
{
    portENTER_CRITICAL(&s_err_lock);
    s_dma_fault_count++;
    portEXIT_CRITICAL(&s_err_lock);
}

static void backpressure_timeout_inc(void)
{
    portENTER_CRITICAL(&s_err_lock);
    s_backpressure_timeout_count++;
    portEXIT_CRITICAL(&s_err_lock);
}

/* ------------------------------------------------------------------
 * Ring (SPSC: producer writes head, consumer writes tail; all access
 * under s_ring_lock). Indices are masked to the (power-of-two) size;
 * `used` is a plain counter that never wraps.
 * ------------------------------------------------------------------ */

static void audio_task_notify(void)
{
    TaskHandle_t target = NULL;
    portENTER_CRITICAL(&s_state_lock);
    if (!s_notify_closed && s_task != NULL && (s_state == MICHI_AUDIO_STATE_RUNNING ||
                                               s_state == MICHI_AUDIO_STATE_QUIESCED ||
                                               s_state == MICHI_AUDIO_STATE_STOPPING)) {
        target = s_task;
        s_notify_inflight++;
    }
    portEXIT_CRITICAL(&s_state_lock);

    if (target != NULL) {
        xTaskNotifyGive(target);
        portENTER_CRITICAL(&s_state_lock);
        if (s_notify_inflight > 0) {
            s_notify_inflight--;
        }
        portEXIT_CRITICAL(&s_state_lock);
    }
}

static size_t ring_used(const ring_t *r)
{
    return r->used;
}

static size_t ring_write(ring_t *r, const uint8_t *data, size_t len)
{
    size_t written = 0;

    portENTER_CRITICAL(&s_ring_lock);
    if (s_ring_write_barrier) {
        portEXIT_CRITICAL(&s_ring_lock);
        return 0;
    }
    size_t free = r->size - ring_used(r);
    if (free > len) {
        free = len;
    }
    if (free > MICHI_AUDIO_CHUNK_BYTES) {
        free = MICHI_AUDIO_CHUNK_BYTES; /* bounded critical section */
    }
    /* A ring write must never publish a fraction of one PCM frame. */
    free -= free % s_frame_bytes;
    if (free == 0) {
        portEXIT_CRITICAL(&s_ring_lock);
        return 0;
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
    bool was_empty = (r->used == free);
    portEXIT_CRITICAL(&s_ring_lock);

    if (free > 0 && was_empty) {
        audio_task_notify();
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
 * Control command dispatch and ACK protocol (Wave B)
 * ------------------------------------------------------------------ */

static void handle_pending_command_in_task(void)
{
    michi_audio_cmd_t cmd = MICHI_AUDIO_CMD_NONE;
    uint32_t gen = 0;

    portENTER_CRITICAL(&s_state_lock);
    cmd = s_pending_cmd;
    gen = s_cmd_generation;
    s_pending_cmd = MICHI_AUDIO_CMD_NONE;
    portEXIT_CRITICAL(&s_state_lock);

    if (cmd == MICHI_AUDIO_CMD_NONE) {
        return;
    }

    if (cmd == MICHI_AUDIO_CMD_QUIESCE) {
#ifdef MICHI_HOST_TEST
        portENTER_CRITICAL(&s_ring_lock);
        bool barrier_open = !s_ring_write_barrier;
        portEXIT_CRITICAL(&s_ring_lock);
        if (barrier_open) {
            s_test_quiesce_barrier_race_count++;
        }
#endif
        /* 1. Flush ring buffer so no stale PCM remains */
        portENTER_CRITICAL(&s_ring_lock);
        s_ring.head = 0;
        s_ring.tail = 0;
        s_ring.used = 0;
        portEXIT_CRITICAL(&s_ring_lock);

        /* 2. Worker is the sole owner of s_chunk: wipe with digital silence */
        memset(s_chunk, 0, sizeof(s_chunk));

        /* 3. Worker is the sole caller of i2s_channel_write: flush hardware FIFOs with silence */
        size_t written = 0;
        esp_err_t wr_err = i2s_channel_write(s_tx, s_chunk, sizeof(s_chunk), &written,
                                             MICHI_AUDIO_I2S_SILENCE_TIMEOUT_MS);
        /* H12: an ESP_OK with a short write is NOT a successful silence
         * barrier. Never report QUIESCED after an incomplete DMA delivery. */
        esp_err_t silence_err = wr_err;
        if (wr_err == ESP_OK && written != sizeof(s_chunk)) {
            silence_err = ESP_FAIL;
            short_write_inc();
        }

        portENTER_CRITICAL(&s_state_lock);
        s_cmd_ack_generation = gen;
        if (s_state == MICHI_AUDIO_STATE_FAULTED ||
            s_state == MICHI_AUDIO_STATE_STOPPING ||
            s_state == MICHI_AUDIO_STATE_STOPPED) {
            s_cmd_result = (silence_err != ESP_OK) ? silence_err : ESP_ERR_TIMEOUT;
        } else if (silence_err != ESP_OK) {
            s_state = MICHI_AUDIO_STATE_FAULTED;
            s_cmd_result = silence_err;
        } else {
            s_state = MICHI_AUDIO_STATE_QUIESCED;
            s_cmd_result = ESP_OK;
        }
        portEXIT_CRITICAL(&s_state_lock);

        if (silence_err != ESP_OK) {
            ESP_LOGW(TAG, "quiesce: silence DMA incomplete err=%s delivered=%u/%u",
                     esp_err_to_name(silence_err), (unsigned)written,
                     (unsigned)sizeof(s_chunk));
            err_count_inc();
        }

        if (s_cmd_ack_sem != NULL) {
            xSemaphoreGive(s_cmd_ack_sem);
        }
    } else if (cmd == MICHI_AUDIO_CMD_RESUME) {
        /* The worker is quiesced and owns the gain envelope. Reset before
         * publishing RUNNING, to fade the first resumed PCM from silence. */
        michi_volume_reset_envelope();
        portENTER_CRITICAL(&s_state_lock);
        s_cmd_ack_generation = gen;
        if (s_state == MICHI_AUDIO_STATE_FAULTED ||
            s_state == MICHI_AUDIO_STATE_STOPPING ||
            s_state == MICHI_AUDIO_STATE_STOPPED) {
            s_cmd_result = ESP_ERR_TIMEOUT;
        } else {
            s_state = MICHI_AUDIO_STATE_RUNNING;
            s_cmd_result = ESP_OK;
        }
        portEXIT_CRITICAL(&s_state_lock);

        if (s_cmd_result == ESP_OK) {
            portENTER_CRITICAL(&s_ring_lock);
            s_ring_write_barrier = false;
            portEXIT_CRITICAL(&s_ring_lock);
        }

        if (s_cmd_ack_sem != NULL) {
            xSemaphoreGive(s_cmd_ack_sem);
        }
    } else if (cmd == MICHI_AUDIO_CMD_STOP) {
        portENTER_CRITICAL(&s_state_lock);
        s_cmd_ack_generation = gen;
        s_state = MICHI_AUDIO_STATE_STOPPING;
        s_cmd_result = ESP_OK;
        portEXIT_CRITICAL(&s_state_lock);

        if (s_cmd_ack_sem != NULL) {
            xSemaphoreGive(s_cmd_ack_sem);
        }
    }
}

static esp_err_t send_cmd_and_wait_ack(michi_audio_cmd_t cmd, uint32_t timeout_ms)
{
#ifdef MICHI_HOST_TEST
    uint32_t override_ms = 0;
    portENTER_CRITICAL(&s_state_lock);
    override_ms = s_cmd_timeout_override_ms;
    portEXIT_CRITICAL(&s_state_lock);
    if (override_ms > 0) {
        timeout_ms = override_ms;
    }
#endif
    if (s_cmd_mux == NULL || s_cmd_ack_sem == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_cmd_mux, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        ESP_LOGE(TAG, "send_cmd %d: failed to take cmd mux (timeout)", cmd);
        return ESP_ERR_TIMEOUT;
    }

    /* Drain any stale ACK from semaphore before dispatch */
    while (xSemaphoreTake(s_cmd_ack_sem, 0) == pdTRUE) {
    }

    /* 1. Close write admission barrier BEFORE publishing QUIESCE so no PCM can enter the ring */
    if (cmd == MICHI_AUDIO_CMD_QUIESCE) {
        portENTER_CRITICAL(&s_ring_lock);
        s_ring_write_barrier = true;
        portEXIT_CRITICAL(&s_ring_lock);
    }

    uint32_t my_gen = 0;

    /* 2. Revalidate lifecycle AFTER closing the barrier before publishing command */
    portENTER_CRITICAL(&s_state_lock);
    if (s_task == NULL || s_state == MICHI_AUDIO_STATE_STOPPED || s_state == MICHI_AUDIO_STATE_STOPPING ||
        s_state == MICHI_AUDIO_STATE_FAULTED ||
        (cmd == MICHI_AUDIO_CMD_QUIESCE && s_state != MICHI_AUDIO_STATE_RUNNING) ||
        (cmd == MICHI_AUDIO_CMD_RESUME && s_state != MICHI_AUDIO_STATE_QUIESCED)) {
        bool may_reopen = (s_state == MICHI_AUDIO_STATE_RUNNING);
        if (cmd == MICHI_AUDIO_CMD_QUIESCE && may_reopen) {
            portENTER_CRITICAL(&s_ring_lock);
            s_ring_write_barrier = false;
            portEXIT_CRITICAL(&s_ring_lock);
        }
        portEXIT_CRITICAL(&s_state_lock);
        xSemaphoreGive(s_cmd_mux);
        return ESP_ERR_INVALID_STATE;
    }
    s_cmd_generation++;
    my_gen = s_cmd_generation;
    s_pending_cmd = cmd;
    portEXIT_CRITICAL(&s_state_lock);

    audio_task_notify();

    esp_err_t res = ESP_OK;
    if (xSemaphoreTake(s_cmd_ack_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        /* Timeout waiting for worker ACK */
        portENTER_CRITICAL(&s_state_lock);
        if (s_cmd_ack_generation != my_gen) {
            /* Command unacknowledged: transition to FAULTED */
            s_state = MICHI_AUDIO_STATE_FAULTED;
            s_pending_cmd = MICHI_AUDIO_CMD_NONE;
            res = ESP_ERR_TIMEOUT;
        } else {
            /* Worker finished right as timeout hit */
            res = s_cmd_result;
        }
        portEXIT_CRITICAL(&s_state_lock);

        if (res == ESP_ERR_TIMEOUT) {
            ESP_LOGE(TAG, "send_cmd %d: worker ACK timed out (%u ms), state -> FAULTED",
                     cmd, (unsigned)timeout_ms);
        }
    } else {
        portENTER_CRITICAL(&s_state_lock);
        if (s_cmd_ack_generation == my_gen) {
            res = s_cmd_result;
        } else {
            /* Stale generation ACK ignored */
            res = ESP_ERR_INVALID_RESPONSE;
        }
        portEXIT_CRITICAL(&s_state_lock);
    }

    if (cmd == MICHI_AUDIO_CMD_QUIESCE && res != ESP_OK) {
        /* H12: a failed command must not reopen PCM admission after STOP,
         * FAULTED or another lifecycle transition took ownership. */
        portENTER_CRITICAL(&s_state_lock);
        bool may_reopen = s_state == MICHI_AUDIO_STATE_RUNNING;
        if (may_reopen) {
            portENTER_CRITICAL(&s_ring_lock);
            s_ring_write_barrier = false;
            portEXIT_CRITICAL(&s_ring_lock);
        }
        portEXIT_CRITICAL(&s_state_lock);
    }

    xSemaphoreGive(s_cmd_mux);
    return res;
}

/* ------------------------------------------------------------------
 * I2S consumer task (the only reader; self-deletes on shutdown)
 * ------------------------------------------------------------------ */

static void i2s_task(void *arg)
{
    (void)arg;
    /* Prefill: do not start DMA until buffer_ms of audio is buffered
     * (avoids the first underruns on session start). */
    for (;;) {
        handle_pending_command_in_task();
        bool should_exit = false;
        bool is_quiesced = false;
        portENTER_CRITICAL(&s_state_lock);
        if (!s_run || s_state == MICHI_AUDIO_STATE_STOPPING) {
            should_exit = true;
        } else if (s_state == MICHI_AUDIO_STATE_QUIESCED) {
            is_quiesced = true;
        }
        portEXIT_CRITICAL(&s_state_lock);

        if (should_exit) {
            goto shutdown;
        }
        if (is_quiesced) {
            break;
        }

        size_t used = 0;
        portENTER_CRITICAL(&s_ring_lock);
        used = ring_used(&s_ring);
        portEXIT_CRITICAL(&s_ring_lock);
        if (used >= s_prefill_bytes) {
            break;
        }
        uint32_t notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
        portENTER_CRITICAL(&s_state_lock);
        if (notified > 0 && !s_run) {
            should_exit = true;
        }
        portEXIT_CRITICAL(&s_state_lock);
        if (should_exit) {
            goto shutdown;
        }
    }

    unsigned consecutive_dma_errors = 0;
    for (;;) {
        handle_pending_command_in_task();
        bool should_exit = false;
        bool wait_mode = false;

        portENTER_CRITICAL(&s_state_lock);
        if (!s_run || s_state == MICHI_AUDIO_STATE_STOPPING || s_state == MICHI_AUDIO_STATE_STOPPED) {
            should_exit = true;
        } else if (s_state == MICHI_AUDIO_STATE_QUIESCED || s_state == MICHI_AUDIO_STATE_FAULTED) {
            wait_mode = true;
        }
        portEXIT_CRITICAL(&s_state_lock);

        if (should_exit) {
            break;
        }
        if (wait_mode) {
            uint32_t notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
            portENTER_CRITICAL(&s_state_lock);
            if (notified > 0 && !s_run) {
                should_exit = true;
            }
            portEXIT_CRITICAL(&s_state_lock);
            if (should_exit) {
                break;
            }
            continue;
        }

        size_t n = ring_read(&s_ring, s_chunk, sizeof(s_chunk));
        if (n == 0) {
            /* Ring empty: sleep on the notification */
            uint32_t notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
            portENTER_CRITICAL(&s_state_lock);
            if (notified > 0 && !s_run) {
                should_exit = true;
            }
            portEXIT_CRITICAL(&s_state_lock);
            if (should_exit) {
                break;
            }
            continue;
        }

        /* P0-12: digital gain here (no-op when DAC hardware volume). */
        michi_volume_apply(s_chunk, n, s_bit_depth);

        size_t written = 0;
        esp_err_t err = i2s_channel_write(s_tx, s_chunk, n, &written,
                                          MICHI_AUDIO_I2S_WRITE_TIMEOUT_MS);
        /* A short write is a failure even if the driver returned ESP_OK.
         * Never replay the tail blindly: a prior prefix may already reside
         * inside DMA, and duplicates would introduce an audible discontinuity. */
        if (err != ESP_OK || written != n) {
            portENTER_CRITICAL(&s_state_lock);
            if (!s_run || s_state == MICHI_AUDIO_STATE_STOPPING) {
                should_exit = true;
            }
            portEXIT_CRITICAL(&s_state_lock);
            if (should_exit) {
                break; /* stop() disabled the channel: normal teardown */
            }
            if (written < n) {
                short_write_inc();
            }
            err_count_inc();
            consecutive_dma_errors++;
            ESP_LOGW(TAG, "I2S PCM delivery incomplete: err=%s submitted=%u/%u streak=%u",
                     esp_err_to_name(err), (unsigned)written, (unsigned)n,
                     consecutive_dma_errors);
            if (consecutive_dma_errors >= MICHI_AUDIO_DMA_FATAL_STREAK) {
                /* Fail closed: the session producer will reject further PCM.
                 * STOP must still disable the channel and join the worker. */
                portENTER_CRITICAL(&s_state_lock);
                s_state = MICHI_AUDIO_STATE_FAULTED;
                s_run = false;
                portEXIT_CRITICAL(&s_state_lock);
                dma_fault_inc();
                ESP_LOGE(TAG, "I2S output is faulted after %u consecutive failures",
                         consecutive_dma_errors);
                break;
            }
        } else {
            consecutive_dma_errors = 0;
        }
        portENTER_CRITICAL(&s_state_lock);
        if (!s_run) {
            should_exit = true;
        }
        portEXIT_CRITICAL(&s_state_lock);
        if (should_exit) {
            break;
        }
    }

shutdown:
#ifdef MICHI_HOST_TEST
    while (s_test_hold_worker) {
        /* One whole tick, not pdMS_TO_TICKS(5): at 100 Hz that macro is 0 and
         * this hold loop would burn the core instead of yielding. */
        vTaskDelay(1);
    }
#endif
    /* Cooperative: the task releases its own resources (stack) and
     * signals, then self-deletes.
     * 1. Atomically close admission of new notification leases under s_state_lock. */
    portENTER_CRITICAL(&s_state_lock);
    s_notify_closed = true;
    portEXIT_CRITICAL(&s_state_lock);

    /* 2. Drain any notification in flight. Since admission is closed,
     *    s_notify_inflight is strictly non-increasing and will reach zero. */
    while (1) {
        bool inflight = false;
        portENTER_CRITICAL(&s_state_lock);
        inflight = (s_notify_inflight > 0);
        portEXIT_CRITICAL(&s_state_lock);
        if (!inflight) {
            break;
        }
        vTaskDelay(1);
    }

    portENTER_CRITICAL(&s_state_lock);
    s_task_done = true;
    /* H11: the worker has no authority to declare STOPPED. Only stop()
     * may publish STOPPED after i2s_channel_disable actually succeeds.
     * Preserve STOPPING during ordinary cooperative termination; any
     * unexplained exit is FAULTED until the owner reconciles the channel. */
    if (s_state != MICHI_AUDIO_STATE_FAULTED &&
        s_state != MICHI_AUDIO_STATE_STOPPING) {
        s_state = MICHI_AUDIO_STATE_FAULTED;
    }
    s_task = NULL;
    portEXIT_CRITICAL(&s_state_lock);

    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------ */

static bool pin_ok(int pin)
{
    return pin >= 0 && pin < GPIO_NUM_MAX;
}

/* H11: an I2S deletion failure during INIT must not turn the live channel
 * into a leaked/unrecoverable handle. Keep ring ownership and mark the
 * partial init FAULTED; deinit() is the only legal recovery route. The
 * lifecycle lease is held throughout this helper. */
static esp_err_t quarantine_failed_init_channel(i2s_chan_handle_t tx,
                                                esp_err_t delete_err)
{
    portENTER_CRITICAL(&s_state_lock);
    s_tx = tx;
    s_inited = true;  /* ownership exists even though init failed */
    s_run = false;
    s_task = NULL;
    s_task_done = false;
    s_notify_closed = true;
    s_channel_delete_failed = true;
    s_state = MICHI_AUDIO_STATE_FAULTED;
    portEXIT_CRITICAL(&s_state_lock);

    portENTER_CRITICAL(&s_ring_lock);
    s_ring_write_barrier = true;
    portEXIT_CRITICAL(&s_ring_lock);
    err_count_inc();
    ESP_LOGE(TAG, "init: channel deletion failed (%s), keeping resources for deinit retry",
             esp_err_to_name(delete_err));
    return delete_err;
}

static esp_err_t michi_audio_output_init_impl(const michi_audio_output_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_state_lock);
    if (s_inited || s_state != MICHI_AUDIO_STATE_UNINITIALIZED) {
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE; /* init -> deinit -> init required */
    }
    portEXIT_CRITICAL(&s_state_lock);

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
    portENTER_CRITICAL(&s_ring_lock);
    s_ring.buf = ring;
    s_ring.size = ring_size;
    s_ring.head = 0;
    s_ring.tail = 0;
    s_ring.used = 0;
    portEXIT_CRITICAL(&s_ring_lock);
    s_bit_depth = cfg->bit_depth;
    s_frame_bytes = ((size_t)cfg->bit_depth / 8) * cfg->channels;

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
        goto fail_channel;
    }

    /* Create control synchronization primitives */
    if (s_cmd_mux == NULL) {
        s_cmd_mux = xSemaphoreCreateMutex();
    }
    if (s_cmd_ack_sem == NULL) {
        s_cmd_ack_sem = xSemaphoreCreateBinary();
    }
    if (s_cmd_mux == NULL || s_cmd_ack_sem == NULL) {
        if (s_cmd_mux != NULL) {
            vSemaphoreDelete(s_cmd_mux);
            s_cmd_mux = NULL;
        }
        if (s_cmd_ack_sem != NULL) {
            vSemaphoreDelete(s_cmd_ack_sem);
            s_cmd_ack_sem = NULL;
        }
        err = ESP_ERR_NO_MEM;
        goto fail_channel;
    }

    /* H8: bind frame shape/ramp before any I2S consumer can run. */
    err = michi_volume_configure_pcm(cfg->sample_rate, cfg->channels);
    if (err != ESP_OK) {
        vSemaphoreDelete(s_cmd_mux);
        s_cmd_mux = NULL;
        vSemaphoreDelete(s_cmd_ack_sem);
        s_cmd_ack_sem = NULL;
        goto fail_channel;
    }

    portENTER_CRITICAL(&s_state_lock);
    s_tx = tx;
    s_inited = true;
    s_run = false;
    s_task = NULL;
    s_task_done = false;
    s_notify_closed = false;
    s_channel_delete_failed = false;
    s_channel_enabled = false;
    s_control_inflight = 0;
    s_pending_cmd = MICHI_AUDIO_CMD_NONE;
    s_cmd_generation = 0;
    s_cmd_ack_generation = 0;
    s_state = MICHI_AUDIO_STATE_INITIALIZED;
    portEXIT_CRITICAL(&s_state_lock);

    portENTER_CRITICAL(&s_ring_lock);
    s_ring_write_barrier = false;
    portEXIT_CRITICAL(&s_ring_lock);

    ESP_LOGI(TAG, "init: ring=%u bytes (PSRAM) prefill=%u bytes "
                  "rate=%" PRIu32 " depth=%u ch=%u",
             (unsigned)ring_size, (unsigned)s_prefill_bytes,
             cfg->sample_rate, cfg->bit_depth, cfg->channels);
    return ESP_OK;

fail_channel:
    /* Keep the allocated ring when deletion failed: the channel still
     * belongs to this module and is recoverable by deinit(), not by init. */
    {
        esp_err_t del_err = i2s_del_channel(tx);
        if (del_err != ESP_OK) {
            return quarantine_failed_init_channel(tx, del_err);
        }
    }
fail_ring:
    heap_caps_free(ring);
    portENTER_CRITICAL(&s_ring_lock);
    s_ring.buf = NULL;
    s_ring.size = 0;
    s_ring.head = 0;
    s_ring.tail = 0;
    s_ring.used = 0;
    s_ring_write_barrier = false;
    portEXIT_CRITICAL(&s_ring_lock);
    return err;
}

static esp_err_t michi_audio_output_start_impl(void)
{
    portENTER_CRITICAL(&s_state_lock);
    if (!s_inited || s_channel_delete_failed || s_channel_enabled || s_write_inflight != 0 ||
        s_task != NULL || s_task_done ||
        (s_state != MICHI_AUDIO_STATE_INITIALIZED && s_state != MICHI_AUDIO_STATE_STOPPED)) {
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    portEXIT_CRITICAL(&s_state_lock);

    /* 1) Flush the ring: stale audio from a previous session must never play. */
    portENTER_CRITICAL(&s_ring_lock);
    s_ring.head = 0;
    s_ring.tail = 0;
    s_ring.used = 0;
    s_ring_write_barrier = false;
    portEXIT_CRITICAL(&s_ring_lock);

    /* New (or restarted) I2S session: avoid an initial full-scale step. */
    michi_volume_reset_envelope();

    /* 2) Enable the LIVE channel */
    esp_err_t err = i2s_channel_enable(s_tx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start: i2s_channel_enable failed: %s", esp_err_to_name(err));
        return err;
    }

    s_channel_enabled = true;

    /* 3) Flags BEFORE creating the task */
    portENTER_CRITICAL(&s_state_lock);
    s_run = true;
    s_state = MICHI_AUDIO_STATE_RUNNING;
    s_task_done = false;
    s_notify_closed = false;
    portEXIT_CRITICAL(&s_state_lock);

    /* 4) Create consumer task */
    TaskHandle_t task = NULL;
    BaseType_t task_ret = xTaskCreate(i2s_task, "michi_i2s", MICHI_AUDIO_TASK_STACK,
                                      NULL, MICHI_AUDIO_TASK_PRIO, &task);
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "start: task creation failed");
        /* H10: channel enable succeeded but task creation did not. A failed
         * rollback disable cannot be ignored: do not permit a second start
         * on an uncertain I2S channel. A STOP retry owns this recovery. */
        esp_err_t disable_err = i2s_channel_disable(s_tx);
        if (disable_err == ESP_OK) {
            s_channel_enabled = false;
        } else {
            err_count_inc();
            ESP_LOGE(TAG, "start: rollback disable failed: %s",
                     esp_err_to_name(disable_err));
        }
        portENTER_CRITICAL(&s_state_lock);
        s_run = false;
        s_state = (disable_err == ESP_OK) ? MICHI_AUDIO_STATE_INITIALIZED : MICHI_AUDIO_STATE_FAULTED;
        s_task = NULL;
        s_task_done = (disable_err != ESP_OK); /* no worker exists: retry STOP safely */
        s_notify_closed = (disable_err != ESP_OK);
        portEXIT_CRITICAL(&s_state_lock);
        return (disable_err == ESP_OK) ? ESP_ERR_NO_MEM : disable_err;
    }

    /* The scheduler may run the worker before xTaskCreate returns. Do not
     * resurrect a self-deleted task handle if it already reported FAULTED. */
    portENTER_CRITICAL(&s_state_lock);
    bool exited_during_create = s_task_done;
    if (!exited_during_create) {
        s_task = task;
    }
    portEXIT_CRITICAL(&s_state_lock);
    if (exited_during_create) {
        ESP_LOGE(TAG, "start: worker exited before task ownership was published");
        return ESP_FAIL; /* stop() must reconcile the still-enabled channel */
    }

    ESP_LOGI(TAG, "start: pipeline running");
    return ESP_OK;
}

static esp_err_t michi_audio_output_write_impl(const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_state_lock);
    michi_audio_output_state_t st = s_state;
    portEXIT_CRITICAL(&s_state_lock);

    if (st != MICHI_AUDIO_STATE_RUNNING) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Reject a torn sample before it reaches the PSRAM ring/I2S driver. */
    if ((len % s_frame_bytes) != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_ring_lock);
    bool barrier = s_ring_write_barrier;
    portEXIT_CRITICAL(&s_ring_lock);
    if (barrier) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t off = 0;
    /* H14: wall-clock deadline across the entire producer call, not only
     * ticks when the ring is completely full. Partial progress must not
     * reset the budget: otherwise a slowly draining DAC can keep this
     * producer occupied for arbitrarily long. Tick subtraction handles
     * wrap-around for intervals shorter than half the counter range. */
    const TickType_t started_at = xTaskGetTickCount();
    const TickType_t budget_ticks = pdMS_TO_TICKS(MICHI_AUDIO_BACKPRESSURE_MAX_MS) > 0
        ? pdMS_TO_TICKS(MICHI_AUDIO_BACKPRESSURE_MAX_MS) : 1;
    while (off < len) {
        if ((TickType_t)(xTaskGetTickCount() - started_at) >= budget_ticks) {
            backpressure_timeout_inc();
            ESP_LOGW(TAG, "PCM ring total write deadline reached (budget=%u ticks)",
                     (unsigned)budget_ticks);
            return ESP_ERR_TIMEOUT;
        }
        portENTER_CRITICAL(&s_state_lock);
        st = s_state;
        portEXIT_CRITICAL(&s_state_lock);
        if (st != MICHI_AUDIO_STATE_RUNNING) {
            return ESP_ERR_INVALID_STATE; /* stopped or quiesced mid-write */
        }

        portENTER_CRITICAL(&s_ring_lock);
        barrier = s_ring_write_barrier;
        portEXIT_CRITICAL(&s_ring_lock);
        if (barrier) {
            return ESP_ERR_INVALID_STATE; /* quiesce began during write */
        }

        size_t n = ring_write(&s_ring, data + off, len - off);
        if (n == 0) {
            portENTER_CRITICAL(&s_ring_lock);
            barrier = s_ring_write_barrier;
            portEXIT_CRITICAL(&s_ring_lock);
            if (barrier) {
                return ESP_ERR_INVALID_STATE;
            }
            /* Ring full: wait for the consumer to drain. One whole tick, NOT
             * pdMS_TO_TICKS(1): at CONFIG_FREERTOS_HZ=100 that macro is 0 and
             * vTaskDelay(0) is just taskYIELD(), so sustained backpressure
             * would spin here at 100% CPU and starve IDLE0, the task watchdog
             * subscriber - the same starvation the button debounce task hit. */
            vTaskDelay(1);
            continue;
        }
        off += n;
    }
    return ESP_OK;
}

esp_err_t michi_audio_output_flush(void)
{
    /* H12: flush is a producer-side ring operation. An already admitted
     * flush must keep its ring allocation alive across STOP/DEINIT. The
     * same counter also prevents START from reusing the ring during flush. */
    portENTER_CRITICAL(&s_state_lock);
    michi_audio_output_state_t st = s_state;
    if (st != MICHI_AUDIO_STATE_RUNNING && st != MICHI_AUDIO_STATE_QUIESCED) {
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_write_inflight++;
    portEXIT_CRITICAL(&s_state_lock);

    portENTER_CRITICAL(&s_ring_lock);
    bool barrier = s_ring_write_barrier;
    if (!barrier && s_ring.buf != NULL) {
        s_ring.head = 0;
        s_ring.tail = 0;
        s_ring.used = 0;
    }
    portEXIT_CRITICAL(&s_ring_lock);

    portENTER_CRITICAL(&s_state_lock);
    s_write_inflight--;
    portEXIT_CRITICAL(&s_state_lock);
    return barrier ? ESP_ERR_INVALID_STATE : ESP_OK;
}

/* Admission is atomic with respect to DEINIT's last-reference check.
 * A reference covers the entire command, including waits on the mutex/ACK.
 * STOP may run concurrently, but DEINIT refuses a live reference even after
 * STOP has completed. This closes the semaphore use-after-free window. */
static esp_err_t audio_control_command(michi_audio_cmd_t cmd)
{
    portENTER_CRITICAL(&s_state_lock);
    if (!s_inited) {
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if ((cmd == MICHI_AUDIO_CMD_QUIESCE && s_state == MICHI_AUDIO_STATE_QUIESCED) ||
        (cmd == MICHI_AUDIO_CMD_RESUME && s_state == MICHI_AUDIO_STATE_RUNNING)) {
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_OK;
    }
    if ((cmd == MICHI_AUDIO_CMD_QUIESCE && s_state != MICHI_AUDIO_STATE_RUNNING) ||
        (cmd == MICHI_AUDIO_CMD_RESUME && s_state != MICHI_AUDIO_STATE_QUIESCED)) {
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_control_inflight++;
    portEXIT_CRITICAL(&s_state_lock);

    esp_err_t err = send_cmd_and_wait_ack(cmd, 1000);
#ifdef MICHI_HOST_TEST
    while (atomic_load(&s_test_hold_control_exit)) {
        atomic_store(&s_test_control_exit_entered, true);
        vTaskDelay(1);
    }
    atomic_store(&s_test_control_exit_entered, false);
#endif
    portENTER_CRITICAL(&s_state_lock);
    s_control_inflight--;
    portEXIT_CRITICAL(&s_state_lock);
    return err;
}

esp_err_t michi_audio_output_quiesce(void)
{
    return audio_control_command(MICHI_AUDIO_CMD_QUIESCE);
}

esp_err_t michi_audio_output_resume(void)
{
    return audio_control_command(MICHI_AUDIO_CMD_RESUME);
}

bool michi_audio_output_is_quiesced(void)
{
    portENTER_CRITICAL(&s_state_lock);
    bool q = (s_state == MICHI_AUDIO_STATE_QUIESCED);
    portEXIT_CRITICAL(&s_state_lock);
    return q;
}

static esp_err_t michi_audio_output_stop_impl(void)
{
    portENTER_CRITICAL(&s_state_lock);
    if (!s_inited) {
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    /* A partial INIT that leaked its disabled channel needs DEINIT retry,
     * not a STOP on a channel that was never enabled. */
    if (s_channel_delete_failed && !s_channel_enabled) {
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if ((s_state == MICHI_AUDIO_STATE_INITIALIZED ||
         s_state == MICHI_AUDIO_STATE_STOPPED) && !s_channel_enabled &&
        !s_task_done && s_task == NULL) {
        /* Idempotence is legal only after hardware has been disabled. */
        s_task_done = false;
        s_task = NULL;
        s_notify_closed = false;
        portEXIT_CRITICAL(&s_state_lock);

        portENTER_CRITICAL(&s_ring_lock);
        s_ring_write_barrier = false;
        portEXIT_CRITICAL(&s_ring_lock);
        return ESP_OK; /* idempotent */
    }
    if (s_task == NULL && s_task_done) {
        /* Worker completed itself. An I2S fatal fault does NOT imply that
         * the hardware channel was disabled; explicitly disable it here. */
        portEXIT_CRITICAL(&s_state_lock);
        if (s_channel_enabled && s_tx != NULL) {
            esp_err_t disable_err = i2s_channel_disable(s_tx);
            if (disable_err != ESP_OK) {
                /* H10: preserve FAULTED and done for retry. A failed disable
                 * does not mean the hardware is safe to re-enable or delete. */
                err_count_inc();
                portENTER_CRITICAL(&s_state_lock);
                s_state = MICHI_AUDIO_STATE_FAULTED;
                s_notify_closed = true;
                portEXIT_CRITICAL(&s_state_lock);
                portENTER_CRITICAL(&s_ring_lock);
                s_ring_write_barrier = true;
                portEXIT_CRITICAL(&s_ring_lock);
                ESP_LOGE(TAG, "stop after fault: channel remains owned: %s",
                         esp_err_to_name(disable_err));
                return disable_err;
            }
            s_channel_enabled = false;
        }
        portENTER_CRITICAL(&s_state_lock);
        s_task_done = false;
        s_state = MICHI_AUDIO_STATE_STOPPED;
        s_notify_closed = false;
        portEXIT_CRITICAL(&s_state_lock);
        portENTER_CRITICAL(&s_ring_lock);
        s_ring_write_barrier = false;
        portEXIT_CRITICAL(&s_ring_lock);
        return ESP_OK;
    }

    /* 1) Stop the flag and set STOPPING state. */
    s_state = MICHI_AUDIO_STATE_STOPPING;
    s_run = false;
    portEXIT_CRITICAL(&s_state_lock);

    /* Wake any command waiter stuck on s_cmd_ack_sem */
    if (s_cmd_ack_sem != NULL) {
        xSemaphoreGive(s_cmd_ack_sem);
    }

    /* Serialize with any in-flight quiesce/resume command */
    if (s_cmd_mux != NULL) {
        xSemaphoreTake(s_cmd_mux, portMAX_DELAY);
    }

    /* 2) Wake the task (ring-empty sleep AND prefill wait both use the
     *    notification lease). */
    audio_task_notify();

    /* 3) Unblock any in-flight i2s_channel_write immediately. */
    esp_err_t err = ESP_OK;
    if (s_channel_enabled) {
        err = i2s_channel_disable(s_tx);
        if (err == ESP_OK) {
            s_channel_enabled = false;
        } else {
            ESP_LOGW(TAG, "stop: i2s_channel_disable failed: %s", esp_err_to_name(err));
            err_count_inc();
        }
    }
    /* 4) Join on the done flag with a timeout. The task sets it right
     *    before self-deleting; after observing it, s_task is stale and is
     *    never touched again (no dangling handle). */
    int join_timeout_ms = MICHI_AUDIO_JOIN_TIMEOUT_MS;
#ifdef MICHI_HOST_TEST
    portENTER_CRITICAL(&s_state_lock);
    if (s_join_timeout_override_ms > 0) {
        join_timeout_ms = (int)s_join_timeout_override_ms;
    }
    portEXIT_CRITICAL(&s_state_lock);
#endif
    int waited_ms = 0;
    bool done = false;
    while (waited_ms < join_timeout_ms) {
        portENTER_CRITICAL(&s_state_lock);
        done = s_task_done;
        portEXIT_CRITICAL(&s_state_lock);
        if (done) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
        waited_ms += 10;
    }
    if (!done) {
        if (s_cmd_mux != NULL) {
            xSemaphoreGive(s_cmd_mux);
        }
        /* The task may still be alive: the channel is NOT deleted and
         * s_task/s_task_done are NOT reset - the subsystem requires
         * deinit after timeout or retry stop. */
        ESP_LOGE(TAG, "stop: task did not self-delete within %d ms - "
                      "subsystem requires deinit or retry stop after timeout",
                 join_timeout_ms);
        return ESP_ERR_TIMEOUT;
    }

    /* H10: the worker has exited but a failed channel disable is still a
     * hardware fault. Keep a retryable terminal state and retain ownership.
     * Never advertise STOPPED or permit a new start on an enabled channel. */
    portENTER_CRITICAL(&s_state_lock);
    s_task = NULL;
    s_task_done = (err != ESP_OK);
    s_notify_closed = (err != ESP_OK);
    s_state = (err == ESP_OK) ? MICHI_AUDIO_STATE_STOPPED : MICHI_AUDIO_STATE_FAULTED;
    portEXIT_CRITICAL(&s_state_lock);

    portENTER_CRITICAL(&s_ring_lock);
    s_ring_write_barrier = (err != ESP_OK);
    portEXIT_CRITICAL(&s_ring_lock);

    if (s_cmd_mux != NULL) {
        while (xSemaphoreTake(s_cmd_ack_sem, 0) == pdTRUE) {
        }
        xSemaphoreGive(s_cmd_mux);
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "stop: channel disable failed, retry stop before deinit");
        return err;
    }
    ESP_LOGI(TAG, "stop: pipeline stopped");
    return ESP_OK;
}

static esp_err_t michi_audio_output_deinit_impl(void)
{
    /* Never free under a live task: if the handle or the done flag is
     * still set, the I2S task may exist (or have just exited) - only
     * stop() with ESP_OK clears both. */
    portENTER_CRITICAL(&s_state_lock);
    if (s_task == NULL && s_task_done && s_state == MICHI_AUDIO_STATE_STOPPED) {
        /* Reconcile late worker exit if caller called deinit directly after worker finished */
        s_task_done = false;
        s_notify_closed = false;
    }
    if (s_task != NULL || s_task_done || s_channel_enabled || s_write_inflight != 0 ||
        s_control_inflight != 0 ||
        s_state == MICHI_AUDIO_STATE_RUNNING ||
        s_state == MICHI_AUDIO_STATE_QUIESCED || s_state == MICHI_AUDIO_STATE_STOPPING ||
        (s_state == MICHI_AUDIO_STATE_FAULTED && !s_channel_delete_failed)) {
        portEXIT_CRITICAL(&s_state_lock);
        ESP_LOGE(TAG, "deinit: active task, producer or channel; "
                      "stop and drain all writes before releasing resources");
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_inited) {
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_OK; /* idempotent */
    }
    portEXIT_CRITICAL(&s_state_lock);

    /* Serialize with command channel */
    if (s_cmd_mux != NULL) {
        xSemaphoreTake(s_cmd_mux, portMAX_DELAY);
    }

    /* The channel is deleted here and only here (also after
     * init()-without-start(): it was created but never enabled). */
    if (s_tx != NULL) {
        esp_err_t err = i2s_del_channel(s_tx);
        if (err != ESP_OK) {
            /* H10: do NOT free the ring or command semaphores, and do NOT
             * discard s_tx. Retain ownership so deinit can be retried.
             * A start after this ambiguous driver failure is prohibited. */
            err_count_inc();
            portENTER_CRITICAL(&s_state_lock);
            s_channel_delete_failed = true;
            portEXIT_CRITICAL(&s_state_lock);
            ESP_LOGE(TAG, "deinit: channel deletion failed, resources retained: %s",
                     esp_err_to_name(err));
            if (s_cmd_mux != NULL) {
                xSemaphoreGive(s_cmd_mux);
            }
            return err;
        }
        s_tx = NULL;
    }

    void *buf_to_free = NULL;
    portENTER_CRITICAL(&s_ring_lock);
    if (s_ring.buf != NULL) {
        buf_to_free = s_ring.buf;
        s_ring.buf = NULL;
        s_ring.size = 0;
        s_ring.head = 0;
        s_ring.tail = 0;
        s_ring.used = 0;
        s_ring_write_barrier = false;
    }
    portEXIT_CRITICAL(&s_ring_lock);

    if (buf_to_free != NULL) {
        heap_caps_free(buf_to_free);
    }

    SemaphoreHandle_t mux_to_delete = s_cmd_mux;
    SemaphoreHandle_t sem_to_delete = s_cmd_ack_sem;
    s_cmd_mux = NULL;
    s_cmd_ack_sem = NULL;

    if (mux_to_delete != NULL) {
        xSemaphoreGive(mux_to_delete);
        vSemaphoreDelete(mux_to_delete);
    }
    if (sem_to_delete != NULL) {
        vSemaphoreDelete(sem_to_delete);
    }

    portENTER_CRITICAL(&s_state_lock);
    s_inited = false;
    s_run = false;
    s_notify_closed = false;
    s_channel_delete_failed = false;
    s_channel_enabled = false;
    s_write_inflight = 0;
    s_control_inflight = 0;
    s_state = MICHI_AUDIO_STATE_UNINITIALIZED;
    portEXIT_CRITICAL(&s_state_lock);

    ESP_LOGI(TAG, "deinit: resources released");
    return ESP_OK;
}

/* These public entry points enforce the H11 single-lifecycle-operation
 * invariant. No mutex is allocated/destroyed during INIT/DEINIT; the static
 * spinlock protects only a boolean and is NEVER held over driver calls. */
esp_err_t michi_audio_output_init(const michi_audio_output_config_t *cfg)
{
    if (!lifecycle_try_enter()) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = michi_audio_output_init_impl(cfg);
    lifecycle_leave();
    return err;
}

esp_err_t michi_audio_output_start(void)
{
    if (!lifecycle_try_enter()) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = michi_audio_output_start_impl();
    lifecycle_leave();
    return err;
}

esp_err_t michi_audio_output_stop(void)
{
    if (!lifecycle_try_enter()) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = michi_audio_output_stop_impl();
    lifecycle_leave();
    return err;
}

esp_err_t michi_audio_output_deinit(void)
{
    if (!lifecycle_try_enter()) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = michi_audio_output_deinit_impl();
    lifecycle_leave();
    return err;
}

esp_err_t michi_audio_output_write(const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_state_lock);
    if (s_state != MICHI_AUDIO_STATE_RUNNING) {
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_write_inflight++;
    portEXIT_CRITICAL(&s_state_lock);

#ifdef MICHI_HOST_TEST
    while (s_test_hold_producer) {
        s_test_producer_in_hold = true;
        vTaskDelay(1);
    }
    s_test_producer_in_hold = false;
#endif
    esp_err_t err = michi_audio_output_write_impl(data, len);
    portENTER_CRITICAL(&s_state_lock);
    s_write_inflight--;
    portEXIT_CRITICAL(&s_state_lock);
    return err;
}

bool michi_audio_output_is_running(void)
{
    portENTER_CRITICAL(&s_state_lock);
    bool r = (s_state == MICHI_AUDIO_STATE_RUNNING || s_state == MICHI_AUDIO_STATE_QUIESCED);
    portEXIT_CRITICAL(&s_state_lock);
    return r;
}

michi_audio_output_state_t michi_audio_output_get_state(void)
{
    michi_audio_output_state_t st;
    portENTER_CRITICAL(&s_state_lock);
    st = s_state;
    portEXIT_CRITICAL(&s_state_lock);
    return st;
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

#ifdef MICHI_HOST_TEST
void test_michi_audio_output_set_cmd_timeout_ms(uint32_t ms)
{
    portENTER_CRITICAL(&s_state_lock);
    s_cmd_timeout_override_ms = ms;
    portEXIT_CRITICAL(&s_state_lock);
}

void test_michi_audio_output_set_join_timeout_ms(uint32_t ms)
{
    portENTER_CRITICAL(&s_state_lock);
    s_join_timeout_override_ms = ms;
    portEXIT_CRITICAL(&s_state_lock);
}

void test_michi_audio_output_hold_worker(bool hold)
{
    s_test_hold_worker = hold;
}

void test_michi_audio_output_hold_producer(bool hold)
{
    s_test_hold_producer = hold;
}

void test_michi_audio_output_hold_control_exit(bool hold)
{
    atomic_store(&s_test_hold_control_exit, hold);
    if (hold) atomic_store(&s_test_control_exit_entered, false);
}

bool test_michi_audio_output_control_exit_is_held(void)
{
    return atomic_load(&s_test_control_exit_entered);
}

bool test_michi_audio_output_producer_is_held(void)
{
    return s_test_producer_in_hold;
}

uint32_t test_michi_audio_output_get_quiesce_barrier_race_count(void)
{
    return s_test_quiesce_barrier_race_count;
}

void test_michi_audio_output_reset_quiesce_barrier_race_count(void)
{
    s_test_quiesce_barrier_race_count = 0;
}
#endif

/* H9: bounded, concurrency-safe quality counters for future WebUI mapping.
 * Counters are cumulative for this boot and never imply analog fidelity. */
esp_err_t michi_audio_output_get_quality(michi_audio_output_quality_t *quality)
{
    if (quality == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_err_lock);
    quality->i2s_errors = s_error_count;
    quality->short_dma_writes = s_short_write_count;
    quality->backpressure_timeouts = s_backpressure_timeout_count;
    quality->fatal_dma_streaks = s_dma_fault_count;
    portEXIT_CRITICAL(&s_err_lock);
    return ESP_OK;
}
