#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"

#include "led_strip.h"

#include "michi_led.h"
#include "michi_state.h"

#define TAG "michi_led"

/* Animation task priority: below the FSM task (5) and the display task (4)
 * so the event bus and the screen are never starved, above app_main (1).
 * The tick is 50 ms and the per-tick work is trivial (3 pixels from the
 * sine LUT + one queued RMT refresh), so the priority keeps the strip
 * fresh without ever delaying audio/display work. */
#define MICHI_LED_TASK_PRIORITY 3

/* Animation tick: 50 ms. Every pattern period is expressed in ticks. */
#define MICHI_LED_TICK_MS 50

/* RMT resolution for the led_strip encoder: 10 MHz = 0.1 us per tick,
 * the recommended default for 800 kHz SK6812 signaling. */
#define MICHI_LED_RMT_RESOLUTION_HZ 10000000

/* Join timeout: the task ticks every 50 ms and clears the strip before
 * exiting, so 200 ms covers a full tick plus the clear/refresh. */
#define MICHI_LED_SHUTDOWN_TIMEOUT_MS 200

/* Envelope for "dim" fixed patterns: 90/255 ~= 0.35 of the brightness cap
 * (the cap itself is MICHI_LED_MAX_BRIGHTNESS_PCT/100, so a dim solid
 * pattern is ~9% of full output - comfortably visible, never blinding). */
#define MICHI_LED_DIM_ENV 90

/* Precomputed 64-entry sine LUT: lut[i] = (sin(2*pi*i/64)+1)/2 * 255,
 * generated offline so the animation tick needs no math.h at all. A pulse
 * of period P (ticks) samples lut[((t%P)*64/P + PHASE) & 63]; one full
 * period covers the 64 entries. The phase is computed modulo the period,
 * so no uint32 overflow occurs at long uptimes. The PULSE pattern adds a
 * fixed +48 offset (lut[48]=0x00, the trough): entering a pulsing state
 * begins dim instead of flashing full brightness (see envelope_for). */
#define MICHI_LED_LUT_SIZE 64
#define MICHI_LED_LUT_MASK (MICHI_LED_LUT_SIZE - 1)
#define MICHI_LED_LUT_PHASE_SHIFT 21 /* ~120 deg of 64: sweep step per LED */
#define MICHI_LED_LUT_PULSE_OFFSET 48 /* lut[48]=0x00: start pulses at the trough */

static const uint8_t s_sin_lut[MICHI_LED_LUT_SIZE] = {
    0x80, 0x8C, 0x98, 0xA5, 0xB0, 0xBC, 0xC6, 0xD0,
    0xDA, 0xE2, 0xEA, 0xF0, 0xF5, 0xFA, 0xFD, 0xFE,
    0xFF, 0xFE, 0xFD, 0xFA, 0xF5, 0xF0, 0xEA, 0xE2,
    0xDA, 0xD0, 0xC6, 0xBC, 0xB0, 0xA5, 0x98, 0x8C,
    0x80, 0x73, 0x67, 0x5A, 0x4F, 0x43, 0x39, 0x2F,
    0x25, 0x1D, 0x15, 0x0F, 0x0A, 0x05, 0x02, 0x01,
    0x00, 0x01, 0x02, 0x05, 0x0A, 0x0F, 0x15, 0x1D,
    0x25, 0x2F, 0x39, 0x43, 0x4F, 0x5A, 0x67, 0x73,
};

/* Animation patterns. The observer picks one per state; the task renders
 * it. param is a period in ticks (pattern-specific meaning). */
typedef enum {
    MICHI_LED_PATTERN_OFF = 0, /*!< All LEDs dark */
    MICHI_LED_PATTERN_SOLID,   /*!< Fixed color at dim envelope */
    MICHI_LED_PATTERN_PULSE,   /*!< Sine pulse of the base color; param = period in ticks */
    MICHI_LED_PATTERN_SWEEP,   /*!< Sine pulse phase-shifted per LED (traveling wave); param = period in ticks */
    MICHI_LED_PATTERN_BLINK,   /*!< Square blink: dim on / off; param = half period in ticks */
    MICHI_LED_PATTERN_PROGRESS,/*!< Sawtooth brightness ramp (OTA progress); param = period in ticks */
} michi_led_pattern_t;

typedef struct {
    michi_led_pattern_t pattern;
    uint32_t param; /* period in ticks (0 = unused) */
    uint8_t r;      /* base color, full scale 0..255 */
    uint8_t g;
    uint8_t b;
} michi_led_pat_state_t;

/* Pattern per state (dim = MICHI_LED_DIM_ENV fixed patterns; pulses reach
 * the full brightness cap at their peak). */
static const michi_led_pat_state_t s_patterns[MICHI_STATE_COUNT] = {
    [MICHI_STATE_BOOTING] = { MICHI_LED_PATTERN_SOLID, 0, 255, 255, 255 },
    [MICHI_STATE_SELF_TEST] = { MICHI_LED_PATTERN_SOLID, 0, 255, 255, 255 },
    [MICHI_STATE_UNPROVISIONED] = { MICHI_LED_PATTERN_PULSE, 48, 0, 90, 255 },   /* 2.4 s */
    [MICHI_STATE_PROVISIONING] = { MICHI_LED_PATTERN_PULSE, 16, 0, 90, 255 },    /* 0.8 s */
    [MICHI_STATE_WIFI_CONNECTING] = { MICHI_LED_PATTERN_PULSE, 32, 255, 140, 0 },/* 1.6 s */
    [MICHI_STATE_IDLE] = { MICHI_LED_PATTERN_SOLID, 0, 0, 90, 255 },
    [MICHI_STATE_PAIRING] = { MICHI_LED_PATTERN_SWEEP, 12, 0, 90, 255 },         /* 0.6 s per wave */
    [MICHI_STATE_SESSION_PENDING] = { MICHI_LED_PATTERN_SOLID, 0, 255, 200, 0 },
    [MICHI_STATE_BUFFERING] = { MICHI_LED_PATTERN_SOLID, 0, 255, 200, 0 },
    [MICHI_STATE_PLAYING] = { MICHI_LED_PATTERN_SOLID, 0, 0, 255, 60 },
    [MICHI_STATE_PAUSED] = { MICHI_LED_PATTERN_BLINK, 24, 0, 255, 60 },          /* 1.2 s on / off */
    [MICHI_STATE_UPDATING] = { MICHI_LED_PATTERN_PROGRESS, 60, 255, 255, 255 },  /* 3.0 s per ramp */
    [MICHI_STATE_RECOVERABLE_ERROR] = { MICHI_LED_PATTERN_SOLID, 0, 255, 40, 0 },
    [MICHI_STATE_FATAL_ERROR] = { MICHI_LED_PATTERN_SOLID, 0, 255, 40, 0 },
};

static led_strip_handle_t s_strip;
static TaskHandle_t s_task;
static volatile bool s_initialized;

/* Pattern + shutdown flag: written by the observer (FSM task) and by
 * shutdown(), read by the animation task every tick. All access under a
 * short portMUX critical section - the observer MUST NOT block. */
static portMUX_TYPE s_pat_mux = portMUX_INITIALIZER_UNLOCKED;
static michi_led_pat_state_t s_pattern;
static volatile bool s_stop;
/* Task handle that the animation task must notify right before it deletes
 * itself (join); NULL when nobody waits. */
static TaskHandle_t s_done_notify;
/* Consecutive failed refresh ticks (led_strip_set_pixel/refresh errors).
 * Errors are logged only on the first failure and on recovery - no per-
 * tick spam while the visual behavior keeps retrying every tick. */
static uint32_t s_refresh_failures;

/* Observer contract (invoked from the FSM task): update the volatile
 * pattern ONLY, never touch the strip (RMT writes are slow; the strip is
 * owned by the animation task). The animation task renders. */
static void on_state_event(const michi_event_t *ev)
{
    if (ev->id != MICHI_EVENT_STATE_CHANGED) {
        return;
    }
    const michi_state_t st = (michi_state_t)ev->data;
    const michi_led_pat_state_t *pat =
        (st < MICHI_STATE_COUNT) ? &s_patterns[st] : NULL;

    portENTER_CRITICAL(&s_pat_mux);
    if (pat != NULL) {
        s_pattern = *pat;
    } else {
        /* Unknown/out-of-range state: dark (defensive; STATE_CHANGED data
         * is always a valid state). */
        s_pattern.pattern = MICHI_LED_PATTERN_OFF;
        s_pattern.param = 0;
        s_pattern.r = s_pattern.g = s_pattern.b = 0;
    }
    portEXIT_CRITICAL(&s_pat_mux);
}

/* Per-LED envelope (0..255): pattern-dependent brightness multiplier.
 * `idx` is the LED index (0..count-1), `t` the tick counter. The phase is
 * computed modulo the period ((t % param) * LUT_SIZE / param): t * LUT_SIZE
 * alone would overflow uint32 after ~38.9 days at a 50 ms tick (2^32 * 50
 * ms), wrapping the phase; modulo first keeps the product below param *
 * LUT_SIZE. */
static uint32_t envelope_for(const michi_led_pat_state_t *pat, uint32_t idx,
                             uint32_t t)
{
    switch (pat->pattern) {
    case MICHI_LED_PATTERN_SOLID:
        return MICHI_LED_DIM_ENV;
    case MICHI_LED_PATTERN_PULSE:
        /* Phase modulo period, then the fixed trough offset (+48): the wrap
         * index = (((t % param) * 64) / param + 48) & 63 stays correct
         * because the modulo keeps the product in range and the mask folds
         * the offset back into [0, 63]. */
        return s_sin_lut[(((t % pat->param) * MICHI_LED_LUT_SIZE) /
                          pat->param + MICHI_LED_LUT_PULSE_OFFSET) &
                         MICHI_LED_LUT_MASK];
    case MICHI_LED_PATTERN_SWEEP:
        return s_sin_lut[(((t % pat->param) * MICHI_LED_LUT_SIZE) /
                          pat->param +
                          idx * MICHI_LED_LUT_PHASE_SHIFT) &
                         MICHI_LED_LUT_MASK];
    case MICHI_LED_PATTERN_BLINK:
        return ((t / pat->param) & 1u) ? MICHI_LED_DIM_ENV : 0;
    case MICHI_LED_PATTERN_PROGRESS:
        /* Rising sawtooth: dim floor to full cap, then jump back. param 0
         * would divide by zero below; the dim floor is the safe output. */
        if (pat->param == 0) {
            return 30u;
        }
        return 30u + ((t % pat->param) * 225u) / pat->param;
    case MICHI_LED_PATTERN_OFF:
    default:
        return 0;
    }
}

static void led_task(void *arg)
{
    uint32_t tick = 0;

    /* Initial pattern: the current state at task start (BOOTING -> white
     * dim). Late observers miss the first STATE_CHANGED only if it raced
     * task creation; rendering michi_state_get() covers that. */
    {
        const michi_state_t st = michi_state_get();
        const michi_led_pat_state_t *pat =
            (st < MICHI_STATE_COUNT) ? &s_patterns[st] : NULL;
        portENTER_CRITICAL(&s_pat_mux);
        if (pat != NULL) {
            s_pattern = *pat;
        }
        portEXIT_CRITICAL(&s_pat_mux);
    }

    for (;;) {
        portENTER_CRITICAL(&s_pat_mux);
        const bool stop = s_stop;
        const michi_led_pat_state_t pat = s_pattern;
        portEXIT_CRITICAL(&s_pat_mux);

        if (stop) {
            /* The task owns the strip: clear it (strip stays fully dark)
             * BEFORE signalling shutdown() - no race on the handle. A
             * failed clear/refresh is logged once; the strip is still
             * released by shutdown(). */
            if (s_strip != NULL) {
                esp_err_t err = led_strip_clear(s_strip);
                if (err == ESP_OK) {
                    err = led_strip_refresh(s_strip);
                }
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "refresh_failed err=%s",
                             esp_err_to_name(err));
                }
            }
            if (s_done_notify != NULL) {
                xTaskNotifyGive(s_done_notify);
            }
            vTaskDelete(NULL);
        }

        if (s_strip != NULL) {
            const uint32_t pct = CONFIG_MICHI_LED_MAX_BRIGHTNESS_PCT;
            esp_err_t err = ESP_OK;
            for (uint32_t i = 0; i < CONFIG_MICHI_LED_COUNT; i++) {
                const uint32_t env = envelope_for(&pat, i, tick);
                /* base * (env/255) * (pct/100), all in software. */
                const uint32_t r = ((uint32_t)pat.r * env * pct) / (255u * 100u);
                const uint32_t g = ((uint32_t)pat.g * env * pct) / (255u * 100u);
                const uint32_t b = ((uint32_t)pat.b * env * pct) / (255u * 100u);
                err = led_strip_set_pixel(s_strip, i, r, g, b);
                if (err != ESP_OK) {
                    break;
                }
            }
            if (err == ESP_OK) {
                err = led_strip_refresh(s_strip);
            }
            if (err != ESP_OK) {
                /* First failure only; recovery logs the summary. The strip
                 * keeps being retried every tick (visual behavior
                 * unchanged). */
                if (s_refresh_failures == 0) {
                    ESP_LOGE(TAG, "refresh_failed err=%s",
                             esp_err_to_name(err));
                }
                s_refresh_failures++;
            } else if (s_refresh_failures > 0) {
                ESP_LOGI(TAG, "refresh recovered after %u failed ticks",
                         (unsigned)s_refresh_failures);
                s_refresh_failures = 0;
            }
        }

        tick++;
        /* 50 ms tick; a shutdown notification wakes the task immediately
         * instead of waiting for the next tick. */
        ulTaskNotifyTake(pdFALSE, pdMS_TO_TICKS(MICHI_LED_TICK_MS));
    }
}

esp_err_t michi_led_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    /* SK6812 (RGB, 3-channel) driven by one RMT channel; the driver owns
     * the RMT resource and frees it on del(). */
    led_strip_config_t strip_config = {
        .strip_gpio_num = CONFIG_MICHI_LED_GPIO,
        .max_leds = CONFIG_MICHI_LED_COUNT,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model = LED_MODEL_SK6812,
    };
    const led_strip_rmt_config_t rmt_config = {
        .resolution_hz = MICHI_LED_RMT_RESOLUTION_HZ,
        /* 64 words is the led_strip component default for ESP32-S3; the
         * frame (3 LEDs = 73 words) fits the 96-word channel (2 blocks) in
         * one encode session; larger strips stream via ping-pong refill
         * (adjudicated against rmt_tx.c). */
        .mem_block_symbols = 64,
    };

    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config,
                                             &s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init: led_strip_new_rmt_device failed (%s), "
                 "status LEDs unavailable", esp_err_to_name(err));
        return err;
    }

    err = michi_state_register_observer(MICHI_EVENT_STATE_CHANGED,
                                        on_state_event);
    if (err != ESP_OK) {
        led_strip_del(s_strip);
        s_strip = NULL;
        ESP_LOGE(TAG, "init: observer registration failed (%s)",
                 esp_err_to_name(err));
        return err;
    }

    portENTER_CRITICAL(&s_pat_mux);
    s_stop = false;
    s_done_notify = NULL;
    s_pattern.pattern = MICHI_LED_PATTERN_OFF;
    s_pattern.param = 0;
    s_pattern.r = s_pattern.g = s_pattern.b = 0;
    portEXIT_CRITICAL(&s_pat_mux);

    BaseType_t rc = xTaskCreate(led_task, "michi_led",
                                CONFIG_MICHI_LED_TASK_STACK_BYTES, NULL,
                                MICHI_LED_TASK_PRIORITY, &s_task);
    if (rc != pdPASS) {
        led_strip_del(s_strip);
        s_strip = NULL;
        ESP_LOGE(TAG, "init: task creation failed");
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "subsystem=led state=ok phase=7");
    return ESP_OK;
}

esp_err_t michi_led_shutdown(void)
{
    if (!s_initialized) {
        /* Already shut down (or never initialized): idempotent, not an
         * error - a second call must report ESP_OK. */
        return ESP_OK;
    }
    if (xTaskGetCurrentTaskHandle() == s_task) {
        ESP_LOGE(TAG, "shutdown: called from the animation task");
        return ESP_ERR_INVALID_STATE;
    }

    /* Cooperative stop: the caller registers as the join target, then the
     * task is notified. Order matters: the target handle must be visible
     * before the task can observe s_stop (it clears the strip, notifies
     * and self-deletes). */
    portENTER_CRITICAL(&s_pat_mux);
    s_done_notify = xTaskGetCurrentTaskHandle();
    s_stop = true;
    portEXIT_CRITICAL(&s_pat_mux);
    xTaskNotifyGive(s_task);

    const uint32_t waited = ulTaskNotifyTake(pdFALSE,
                                             pdMS_TO_TICKS(MICHI_LED_SHUTDOWN_TIMEOUT_MS));
    if (waited == 0) {
        /* The task may still be alive: deleting the strip under it would
         * be a use-after-free, so it is leaked instead (honest degraded). */
        ESP_LOGW(TAG, "shutdown: animation task did not stop within %d ms, "
                 "strip left allocated", (int)MICHI_LED_SHUTDOWN_TIMEOUT_MS);
        return ESP_ERR_TIMEOUT;
    }

    if (s_strip != NULL) {
        esp_err_t err = led_strip_del(s_strip);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "shutdown: led_strip_del failed (%s)",
                     esp_err_to_name(err));
        }
        s_strip = NULL;
    }

    s_initialized = false;
    ESP_LOGI(TAG, "subsystem=led state=off phase=7");
    return ESP_OK;
}
