#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "michi_button.h"
#include "michi_button_debounce.h"
#include "michi_button_gesture.h"
#include "michi_pairing.h"
#include "michi_state.h"
#include "michi_display.h"

#define TAG "michi_button"

#define MICHI_BUTTON_MIN_PRESS_MS CONFIG_MICHI_BUTTON_MIN_PRESS_MS

/* Build-time contract (P0-01): pairing threshold < factory-warn < factory-reset.
 * This is enforced by the Kconfig range; the assert catches an invalid
 * manual override of the defaults. */
_Static_assert(CONFIG_MICHI_BUTTON_PAIRING_HOLD_MS <
                   CONFIG_MICHI_BUTTON_FACTORY_WARN_MS,
               "PAIRING_HOLD_MS must be less than FACTORY_WARN_MS");
_Static_assert(CONFIG_MICHI_BUTTON_FACTORY_WARN_MS <
                   CONFIG_MICHI_BUTTON_FACTORY_RESET_PRESS_MS,
               "FACTORY_WARN_MS must be less than FACTORY_RESET_PRESS_MS");

/* Debounce task priority: below the FSM task (5) so the event bus is never
 * delayed by button work; the task only polls a GPIO and posts events. */
#define MICHI_BUTTON_TASK_PRIORITY 2

/* Confirmed-edge debounce window (ms) - the time-based single-authority
 * debouncer (michi_button_debounce.c). The poll period no longer gates the
 * accuracy: the debouncer measures a stable window on the monotonic clock,
 * so POLL >= DEBOUNCE no longer silently disables it. */
#define MICHI_BUTTON_DEBOUNCE_MS CONFIG_MICHI_BUTTON_DEBOUNCE_MS

/* Join timeout: the task ticks every POLL_MS, so 200 ms covers a full tick
 * plus the shutdown exit. */
#define MICHI_BUTTON_SHUTDOWN_TIMEOUT_MS 200

/* Hold thresholds (P0-01): all in milliseconds. */
#define MICHI_BUTTON_PAIRING_HOLD_MS      CONFIG_MICHI_BUTTON_PAIRING_HOLD_MS
#define MICHI_BUTTON_FACTORY_WARN_MS      CONFIG_MICHI_BUTTON_FACTORY_WARN_MS
#define MICHI_BUTTON_FACTORY_RESET_MS     CONFIG_MICHI_BUTTON_FACTORY_RESET_PRESS_MS
#define MICHI_BUTTON_FACTORY_ARM_MS_CFG   CONFIG_MICHI_BUTTON_FACTORY_ARM_MS

/* ISR record: the latest GPIO edge (level + timestamp). Written by the ISR,
 * read by the debounce task. The ISR contains NO logic - it only records;
 * everything else (debounce, duration, actions) happens in the task. */
typedef struct {
    int level;    /* 0 = pressed (active low), 1 = released */
    int64_t t_us; /* esp_timer_get_time() at the edge */
} michi_button_edge_t;

static portMUX_TYPE s_edge_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile michi_button_edge_t s_edge;
static volatile bool s_stop;
static TaskHandle_t s_task;
/* Task handle that the debounce task must notify right before it deletes
 * itself (join); NULL when nobody waits. */
static TaskHandle_t s_done_notify;
/* True only when THIS component installed the GPIO ISR service: shutdown
 * must never uninstall a service installed by another component (it would
 * silently kill that component's handlers). */
static bool s_isr_service_installed;
static volatile bool s_initialized;
/* Serializes shutdown: a second caller while one is in progress gets
 * ESP_ERR_INVALID_STATE instead of racing on the join target and teardown. */
static volatile bool s_shutdown_in_progress;
/* Boot reference (esp_timer_get_time() at init, a few ms after power-on):
 * the factory-reset arm window measures the press start against it. */
static int64_t s_boot_time;

static void IRAM_ATTR button_isr(void *arg)
{
    const int level = gpio_get_level(CONFIG_MICHI_BUTTON_GPIO);

    portENTER_CRITICAL_ISR(&s_edge_mux);
    s_edge.level = level;
    s_edge.t_us = esp_timer_get_time();
    portEXIT_CRITICAL_ISR(&s_edge_mux);
}


/* Hard protection (inside the classifier, michi_button_gesture.c): while
 * the firmware is booting, self-testing or updating, NO button action
 * runs - a factory reset during OTA could brick the unit. The pairing
 * gate (IDLE/UNPROVISIONED/PAIRING) and the recovery gate
 * (RECOVERABLE_ERROR at the release) sit on top of it: the FSM would
 * drop the out-of-contract events anyway, the gates keep the logs
 * honest. */
/* Post with one bounded retry: ESP_ERR_TIMEOUT means the event queue is
 * full (transient - the FSM task drains it), so a 50 ms wait + a second
 * attempt covers the usual spike. If the second post also fails the event
 * is dropped and logged: the button never blocks on the bus. */
static esp_err_t post_with_retry(michi_event_id_t id, uint32_t data)
{
    esp_err_t err = michi_state_post(id, data);
    if (err == ESP_ERR_TIMEOUT) {
        vTaskDelay(pdMS_TO_TICKS(50));
        err = michi_state_post(id, data);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "button: post_failed event=%d err=%s", (int)id,
                 esp_err_to_name(err));
    }
    return err;
}

static void handle_pairing_action(michi_state_t st, int64_t elapsed_ms)
{
    /* Pairing fires from any non-protected state.  The pairing window is the
     * ONLY authority that opens the physical pairing flow. */
    if (st == MICHI_STATE_IDLE || st == MICHI_STATE_UNPROVISIONED ||
        st == MICHI_STATE_PAIRING || st == MICHI_STATE_SESSION_PENDING ||
        st == MICHI_STATE_BUFFERING || st == MICHI_STATE_PLAYING ||
        st == MICHI_STATE_PAUSED) {
        ESP_LOGI(TAG, "button: hold=%" PRId64 "ms action=pairing (threshold crossed)",
                 elapsed_ms);
        const esp_err_t err = michi_pairing_open_window();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "button: pairing window open failed err=%s",
                     esp_err_to_name(err));
            return;
        }
        /* Post PAIRING_STARTED only from states where the FSM safely
         * transitions to PAIRING without tearing down an active session. */
        if (st == MICHI_STATE_IDLE || st == MICHI_STATE_UNPROVISIONED) {
            post_with_retry(MICHI_EVENT_PAIRING_STARTED, 0);
        }
        michi_display_set_pairing_overlay(MICHI_DISPLAY_PAIRING_OVERLAY_WAITING);
        return;
    }
    ESP_LOGW(TAG, "button: hold=%" PRId64 "ms action=pairing state=%s "
             "(ignored: non-pairable state)", elapsed_ms, michi_state_name(st));
}

/* Evaluate the hold-on-threshold contract while the button is pressed.
 * Called every poll tick. Mutates ctx (action_fired, factory_warned).
 * Returns true if a destructive action (factory reset) was fired and the
 * task should restart its loop. */
static bool evaluate_hold(michi_button_press_ctx_t *ctx, int64_t now_us)
{
    const int64_t elapsed_ms = (now_us - ctx->pressed_at_us) / 1000;
    const michi_state_t cur_state = michi_state_get();

    const michi_button_action_t action = michi_button_hold_classify(
        elapsed_ms,
        ctx->press_state,
        cur_state,
        ctx->press_boot_ms,
        ctx,
        MICHI_BUTTON_PAIRING_HOLD_MS,
        MICHI_BUTTON_FACTORY_WARN_MS,
        MICHI_BUTTON_FACTORY_RESET_MS,
        MICHI_BUTTON_FACTORY_ARM_MS_CFG);

    switch (action) {
    case MICHI_BUTTON_ACTION_PAIRING:
        ctx->action_fired = true;
        handle_pairing_action(cur_state, elapsed_ms);
        break;

    case MICHI_BUTTON_ACTION_RECOVERY:
        ctx->action_fired = true;
        ESP_LOGI(TAG, "button: hold=%" PRId64 "ms action=recovery (threshold crossed)",
                 elapsed_ms);
        post_with_retry(MICHI_EVENT_RECOVER, 0);
        break;

    case MICHI_BUTTON_ACTION_FACTORY_WARN:
        ctx->factory_warned = true;
        ESP_LOGW(TAG, "button: hold=%" PRId64 "ms factory_reset_imminent "
                 "(release to cancel)", elapsed_ms);
        /* Display a warning overlay — keep NONE for now (display TBD). */
        break;

    case MICHI_BUTTON_ACTION_FACTORY_RESET:
        ctx->action_fired = true;
        ESP_LOGW(TAG, "button: hold=%" PRId64 "ms action=factory_reset "
                 "(threshold crossed)", elapsed_ms);
        (void)michi_button_factory_reset_run(); /* never returns */
        return true; /* unreachable but keeps compiler happy */

    case MICHI_BUTTON_ACTION_IGNORED_PROTECTED:
        /* Became protected mid-press (e.g. OTA started) — ignore. */
        break;

    case MICHI_BUTTON_ACTION_IGNORED_ARM:
        /* Boot-hold: already logged at hold init. */
        break;

    case MICHI_BUTTON_ACTION_NONE:
        /* Below threshold or already consumed. */
        break;
    }
    return false;
}

static void button_task(void *arg)
{
    /* The debouncer is the SINGLE AUTHORITY for edge confirmation: it owns
     * the raw-level → stable-level state machine and emits exactly one
     * event per confirmed transition. GPIO, time and the FSM never bypass it.
     * The ISR (button_isr) records the edge timestamp that anchors the
     * duration; the task only reads it AFTER the debouncer has confirmed a
     * stable transition. */
    michi_button_debounce_t deb;
    michi_button_debounce_init(&deb, MICHI_BUTTON_DEBOUNCE_MS);

    /* Press context: holds the state of the current (or last) press. */
    michi_button_press_ctx_t ctx = {0};

    for (;;) {
        /* Stop check + join notify (shutdown coordination). */
        portENTER_CRITICAL(&s_edge_mux);
        const bool stop = s_stop;
        if (stop) {
            if (s_done_notify != NULL) {
                xTaskNotifyGive(s_done_notify);
            }
            portEXIT_CRITICAL(&s_edge_mux);
            vTaskDelete(NULL);
        }
        portEXIT_CRITICAL(&s_edge_mux);

        const int level = gpio_get_level(CONFIG_MICHI_BUTTON_GPIO);
        const int64_t now_us = esp_timer_get_time();
        const michi_button_debounce_evt_t evt =
            michi_button_debounce_feed(&deb, level, now_us);

        if (evt == MICHI_BTN_DEBOUNCE_PRESS) {
            /* Confirmed press: initialise the press context. */
            ctx.pressed       = true;
            ctx.action_fired  = false;
            ctx.factory_warned= false;
            ctx.pressed_at_us = now_us;
            portENTER_CRITICAL(&s_edge_mux);
            ctx.press_state   = michi_state_get();
            ctx.press_boot_ms = (now_us - s_boot_time) / 1000;
            portEXIT_CRITICAL(&s_edge_mux);

            ESP_LOGD(TAG, "button: press confirmed state=%s boot_ms=%" PRId64,
                     michi_state_name(ctx.press_state), ctx.press_boot_ms);
            /* Immediate visual feedback on confirmed press. */
            michi_display_set_pairing_overlay(
                MICHI_DISPLAY_PAIRING_OVERLAY_BTN_PRESS);

        } else if (evt == MICHI_BTN_DEBOUNCE_RELEASE) {
            /* Release: clear feedback regardless of whether action fired. */
            michi_display_set_pairing_overlay(MICHI_DISPLAY_PAIRING_OVERLAY_NONE);

            if (ctx.pressed && !ctx.action_fired) {
                /* Press was NOT consumed: check if it's a noise pulse. */
                const int64_t held_ms = (now_us - ctx.pressed_at_us) / 1000;
                if (held_ms < MICHI_BUTTON_MIN_PRESS_MS) {
                    ESP_LOGD(TAG, "button: release after %" PRId64 "ms < MIN_PRESS "
                             "%d ms, discarded as noise", held_ms,
                             MICHI_BUTTON_MIN_PRESS_MS);
                } else {
                    /* Valid press but too short to trigger any action (< 5 s).
                     * Silently discard: the user did not hold long enough. */
                    ESP_LOGD(TAG, "button: release after %" PRId64 "ms "
                             "(below pairing threshold %d ms, no action)",
                             held_ms, MICHI_BUTTON_PAIRING_HOLD_MS);
                }
            }
            /* Clear context. */
            ctx = (michi_button_press_ctx_t){0};

        } else if (ctx.pressed) {
            /* Button is still held: evaluate hold thresholds this tick. */
            evaluate_hold(&ctx, now_us);
        }

        /* Poll period; a shutdown notification wakes the task immediately. */
        ulTaskNotifyTake(pdFALSE, pdMS_TO_TICKS(CONFIG_MICHI_BUTTON_POLL_MS));
    }
}

esp_err_t michi_button_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    /* The debounce is a pure, time-based single-authority state machine
     * (michi_button_debounce.c, MICHI_BUTTON_DEBOUNCE_MS passed in at init).
     * The poll period no longer gates the debounce accuracy, so the
     * POLL >= DEBOUNCE clamping that used to live here is gone - the
     * window is enforced on the wall clock. */

    /* GPIO input with internal pull-up: the button shorts the pin to GND
     * (active low); the pull-up guarantees a defined idle level. The
     * internal pull-up is NOT a substitute for the physical validation of
     * continuity/pull-up on the real unit (Kconfig help + README). */
    gpio_config_t cfg = {
        .pin_bit_mask = BIT64(CONFIG_MICHI_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init: gpio_config failed: %s (button unavailable)",
                 esp_err_to_name(err));
        return err;
    }

    /* GPIO ISR service with ESP_INTR_FLAG_IRAM: button_isr is IRAM-safe
     * (it only calls gpio_get_level, esp_timer_get_time and
     * portENTER_CRITICAL_ISR/portEXIT_CRITICAL_ISR - no flash access), so
     * it can run with the cache disabled. If another component already
     * installed the service (ESP_ERR_INVALID_STATE), the shared service is
     * reused: the handler makes no flash calls, so a non-IRAM dispatcher is
     * equally safe, and shutdown MUST NOT uninstall it (tracked with
     * s_isr_service_installed). */
    err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (err == ESP_OK) {
        s_isr_service_installed = true;
    } else if (err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "init: gpio_install_isr_service failed: %s (button "
                 "unavailable)", esp_err_to_name(err));
        return err;
    }

    err = gpio_isr_handler_add(CONFIG_MICHI_BUTTON_GPIO, button_isr, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init: gpio_isr_handler_add failed: %s (button "
                 "unavailable)", esp_err_to_name(err));
        if (s_isr_service_installed) {
            gpio_uninstall_isr_service();
            s_isr_service_installed = false;
        }
        return err;
    }

    /* Seed the ISR record with the CURRENT pin level and a real timestamp:
     * if the button is already held when the ISR is registered, no edge
     * fires and the record must still be a valid time anchor (otherwise a
     * press held through boot would measure a huge duration). s_boot_time
     * is the arm-window reference: a boot-hold press anchors here, i.e.
     * with ~0 elapsed since "boot". */
    portENTER_CRITICAL(&s_edge_mux);
    s_edge.level = gpio_get_level(CONFIG_MICHI_BUTTON_GPIO);
    s_edge.t_us = esp_timer_get_time();
    s_boot_time = s_edge.t_us;
    s_stop = false;
    s_done_notify = NULL;
    s_shutdown_in_progress = false;
    portEXIT_CRITICAL(&s_edge_mux);

    BaseType_t rc = xTaskCreate(button_task, "michi_button",
                                CONFIG_MICHI_BUTTON_TASK_STACK_BYTES, NULL,
                                MICHI_BUTTON_TASK_PRIORITY, &s_task);
    if (rc != pdPASS) {
        gpio_isr_handler_remove(CONFIG_MICHI_BUTTON_GPIO);
        if (s_isr_service_installed) {
            gpio_uninstall_isr_service();
            s_isr_service_installed = false;
        }
        ESP_LOGE(TAG, "init: task creation failed");
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "subsystem=button state=ok phase=8");
    return ESP_OK;
}

esp_err_t michi_button_shutdown(void)
{
    if (!s_initialized) {
        /* Already shut down (or never initialized): idempotent, not an
         * error - a second call must report ESP_OK. */
        return ESP_OK;
    }
    if (xTaskGetCurrentTaskHandle() == s_task) {
        ESP_LOGE(TAG, "shutdown: called from the debounce task");
        return ESP_ERR_INVALID_STATE;
    }

    /* Serialize concurrent shutdowns: a second caller while one is in
     * progress would race on the join target (s_done_notify) and on the
     * ISR/state teardown. Callers must serialize externally. */
    portENTER_CRITICAL(&s_edge_mux);
    if (s_shutdown_in_progress) {
        portEXIT_CRITICAL(&s_edge_mux);
        ESP_LOGW(TAG, "shutdown: already in progress");
        return ESP_ERR_INVALID_STATE;
    }
    s_shutdown_in_progress = true;
    portEXIT_CRITICAL(&s_edge_mux);

    /* Cooperative stop: the caller registers as the join target, then the
     * task is notified. Order matters: the target handle must be visible
     * before the task can observe s_stop (it notifies and self-deletes). */
    portENTER_CRITICAL(&s_edge_mux);
    s_done_notify = xTaskGetCurrentTaskHandle();
    s_stop = true;
    portEXIT_CRITICAL(&s_edge_mux);
    xTaskNotifyGive(s_task);

    const uint32_t waited = ulTaskNotifyTake(pdFALSE,
                                             pdMS_TO_TICKS(MICHI_BUTTON_SHUTDOWN_TIMEOUT_MS));
    if (waited == 0) {
        /* The task may still be alive and sampling: removing the ISR
         * handler under it would only break its time source, not crash the
         * unit, but the join was requested - report the timeout and leave
         * everything registered (honest degraded). Clear the join target
         * under the mux BEFORE returning: the live task must never notify
         * a stale handle once the caller is gone. */
        portENTER_CRITICAL(&s_edge_mux);
        s_done_notify = NULL;
        portEXIT_CRITICAL(&s_edge_mux);
        ESP_LOGW(TAG, "shutdown: debounce task did not stop within %d ms",
                 (int)MICHI_BUTTON_SHUTDOWN_TIMEOUT_MS);
        s_shutdown_in_progress = false;
        return ESP_ERR_TIMEOUT;
    }

    gpio_isr_handler_remove(CONFIG_MICHI_BUTTON_GPIO);
    /* Only uninstall the ISR service if THIS component installed it:
     * gpio_uninstall_isr_service is global - a shared service must stay
     * installed for the other components using it. */
    if (s_isr_service_installed) {
        gpio_uninstall_isr_service();
        s_isr_service_installed = false;
    }

    portENTER_CRITICAL(&s_edge_mux);
    s_done_notify = NULL;
    portEXIT_CRITICAL(&s_edge_mux);
    s_initialized = false;
    s_shutdown_in_progress = false;
    ESP_LOGI(TAG, "subsystem=button state=off phase=8");
    return ESP_OK;
}
