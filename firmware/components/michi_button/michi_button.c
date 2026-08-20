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

#define TAG "michi_button"

/* Gesture contract (P1-07): the factory-reset band must sit strictly
 * above the recovery band - otherwise every recovery hold would become a
 * destructive reset. Enforced at build time on top of the Kconfig range. */
_Static_assert(CONFIG_MICHI_BUTTON_FACTORY_RESET_PRESS_MS >
                   CONFIG_MICHI_BUTTON_RECOVERY_PRESS_MS,
               "MICHI_BUTTON_FACTORY_RESET_PRESS_MS must be greater than "
               "MICHI_BUTTON_RECOVERY_PRESS_MS");

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
/* FSM state at the press confirmation (F1: the release action requires the
 * press to have started OUTSIDE the protected states). */
static volatile michi_state_t s_press_state = MICHI_STATE_BOOTING;
/* Boot elapsed at the press confirmation, ms (F4: factory-reset arm).
 * int64_t: esp_timer_get_time() is int64_t, and a uint32_t cast would wrap
 * at 49.7 days of uptime (F8 follow-up). */
static int64_t s_press_boot_elapsed;

static void IRAM_ATTR button_isr(void *arg)
{
    const int level = gpio_get_level(CONFIG_MICHI_BUTTON_GPIO);

    portENTER_CRITICAL_ISR(&s_edge_mux);
    s_edge.level = level;
    s_edge.t_us = esp_timer_get_time();
    portEXIT_CRITICAL_ISR(&s_edge_mux);
}

static void take_edge_snapshot(michi_button_edge_t *out)
{
    portENTER_CRITICAL(&s_edge_mux);
    *out = s_edge;
    portEXIT_CRITICAL(&s_edge_mux);
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

static void handle_short_press(uint32_t press_ms, michi_state_t st)
{
    if (st == MICHI_STATE_IDLE || st == MICHI_STATE_UNPROVISIONED ||
        st == MICHI_STATE_PAIRING) {
        ESP_LOGI(TAG, "button: press_ms=%u action=pairing", (unsigned)press_ms);
        /* The physical press is the ONLY authority that opens the pairing
         * window. From IDLE/UNPROVISIONED, PAIRING_STARTED is posted ONLY
         * when the window actually opened: posting it anyway would strand
         * the FSM in PAIRING. From PAIRING the press REPLACES the open
         * window (contract 2.3: "abrir de nuevo reemplaza la ventana
         * previa y elimina sesiones pendientes") - the FSM already sits
         * in PAIRING, no event is posted. On failure the press is a no-op
         * (logged): the next press retries. */
        const esp_err_t err = michi_pairing_open_window();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "button: pairing window open failed err=%s",
                     esp_err_to_name(err));
            return;
        }
        if (st != MICHI_STATE_PAIRING) {
            post_with_retry(MICHI_EVENT_PAIRING_STARTED, 0);
        }
        return;
    }
    ESP_LOGW(TAG, "button: press_ms=%u action=pairing state=%s "
             "(expected IDLE, UNPROVISIONED or PAIRING)",
             (unsigned)press_ms, michi_state_name(st));
}

/* Deterministic gestures (P1-07, contract in michi_button_gesture.h):
 *   - short press  (< RECOVERY_PRESS_MS):               pairing window
 *   - long press   (>= RECOVERY_PRESS_MS, < FACTORY_RESET_PRESS_MS):
 *     recovery, only when the FSM is in RECOVERABLE_ERROR at the release
 *   - very long    (>= FACTORY_RESET_PRESS_MS):         factory reset,
 *     armed (press started >= FACTORY_ARM_MS after boot)
 * Hard protection: a press that STARTED or ENDED in BOOTING, SELF_TEST
 * or UPDATING is ignored - a factory reset during OTA could brick the
 * unit. The classification lives in michi_button_gesture.c (pure, host-
 * tested); this file executes the chosen action. */
static void handle_release(uint32_t press_ms)
{
    const michi_state_t st = michi_state_get();

    const michi_button_action_t action = michi_button_gesture_classify(
        press_ms, s_press_state, st, s_press_boot_elapsed,
        CONFIG_MICHI_BUTTON_RECOVERY_PRESS_MS,
        CONFIG_MICHI_BUTTON_FACTORY_RESET_PRESS_MS,
        CONFIG_MICHI_BUTTON_FACTORY_ARM_MS);

    switch (action) {
    case MICHI_BUTTON_ACTION_PAIRING:
        handle_short_press(press_ms, st);
        break;
    case MICHI_BUTTON_ACTION_RECOVERY:
        ESP_LOGI(TAG, "button: press_ms=%u action=recovery",
                 (unsigned)press_ms);
        post_with_retry(MICHI_EVENT_RECOVER, 0);
        break;
    case MICHI_BUTTON_ACTION_FACTORY_RESET:
        ESP_LOGW(TAG, "button: press_ms=%u action=factory_reset state=%s",
                 (unsigned)press_ms, michi_state_name(st));
        (void)michi_button_factory_reset_run();
        break;
    case MICHI_BUTTON_ACTION_IGNORED_PROTECTED:
        ESP_LOGW(TAG, "button: action=ignored press_state=%s release_state=%s",
                 michi_state_name(s_press_state), michi_state_name(st));
        break;
    case MICHI_BUTTON_ACTION_IGNORED_ARM:
        ESP_LOGW(TAG, "button: factory_reset ignored arm_window=%d ms "
                 "(press_elapsed=%" PRId64 " ms)",
                 CONFIG_MICHI_BUTTON_FACTORY_ARM_MS, s_press_boot_elapsed);
        break;
    case MICHI_BUTTON_ACTION_IGNORED_STATE:
        ESP_LOGW(TAG, "button: press_ms=%u action=recovery state=%s "
                 "(expected RECOVERABLE_ERROR)",
                 (unsigned)press_ms, michi_state_name(st));
        break;
    }
}

static void button_task(void *arg)
{
    /* The debouncer is the SINGLE AUTHORITY for edge confirmation: it owns
     * the raw-level -> stable-level state machine and emits exactly one
     * event per confirmed transition. GPIO, time and the FSM never bypass it.
     * The ISR (button_isr) records the edge timestamp that anchors the
     * duration; the task only reads it AFTER the debouncer has confirmed a
     * stable transition - never as a validity re-check (the old raw
     * gpio_get_level() abort on release was a TOCTOU that dropped valid
     * releases, PAIR-BTN-01 P0). */
    michi_button_debounce_t deb;
    michi_button_debounce_init(&deb, MICHI_BUTTON_DEBOUNCE_MS);
    /* ISR timestamp of the confirmed press edge (0 = no confirmed press). */
    int64_t press_t_us = 0;

    for (;;) {
        /* Stop check + join notify under the mux (F8 follow-up): the
         * shutdown caller registers s_done_notify and clears it under the
         * same mux before returning, so a notify issued while holding the
         * mux can never hit a stale (freed) handle - the joiner is either
         * still waiting or has already cleared the target. */
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
            michi_button_edge_t edge;
            take_edge_snapshot(&edge);
            /* Stable press (active low): anchor the duration with the ISR edge
             * timestamp (sub-tick accuracy) - the duration is measured
             * edge-to-edge on the release. The ISR record must still say
             * "pressed": a stale or coalesced record (edge.level != 0) cannot
             * anchor the duration - discard it (no press_t_us => the release
             * does nothing, safer than a bogus long press). */
            if (edge.level == 0) {
                press_t_us = edge.t_us;
                /* Snapshot the FSM state and the boot elapsed under the mux:
                 * the release gates on both (F1: the press must not have
                 * STARTED in a protected state; F4: factory reset needs the
                 * arm window). */
                portENTER_CRITICAL(&s_edge_mux);
                s_press_state = michi_state_get();
                s_press_boot_elapsed = (now_us - s_boot_time) / 1000;
                portEXIT_CRITICAL(&s_edge_mux);
            } else {
                ESP_LOGD(TAG, "button: press anchor discarded (edge "
                         "level=%d, stale or coalesced record)",
                         edge.level);
            }
        } else if (evt == MICHI_BTN_DEBOUNCE_RELEASE) {
            michi_button_edge_t edge;
            take_edge_snapshot(&edge);
            /* Stable release: duration between the two confirmed edges, both
             * ISR-timestamped (sub-tick, debounce window excluded). Symmetric
             * gate: the release requires a REAL release edge (edge.level == 1),
             * mirroring the press path's edge.level == 0 anchor check - a stale
             * or coalesced record cannot fire an action.
             *
             * The debouncer is authoritative here: a confirmed stable release
             * fires the action directly. The old code re-read the pin with
             * gpio_get_level() right before firing and aborted if it read 0
             * (a rebound) - that re-introduced a TOCTOU race that dropped
             * valid releases (PAIR-BTN-01 P0). The debouncer's stable
             * confirmation is the only gate the release needs. */
            if (press_t_us != 0 && edge.level == 1 && edge.t_us > press_t_us) {
                const uint32_t press_ms =
                    (uint32_t)((edge.t_us - press_t_us) / 1000);
                handle_release(press_ms);
            }
            press_t_us = 0;
        }

        /* 10 ms poll; a shutdown notification wakes the task immediately
         * instead of waiting for the next tick. */
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
