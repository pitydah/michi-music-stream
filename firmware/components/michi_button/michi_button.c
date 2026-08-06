#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "michi_button.h"
#include "michi_state.h"

#define TAG "michi_button"

/* Debounce task priority: below the FSM task (5) so the event bus is never
 * delayed by button work; the task only polls a GPIO and posts events. */
#define MICHI_BUTTON_TASK_PRIORITY 2

/* Consecutive stable samples required to confirm an edge: N = DEBOUNCE/POLL
 * rounded up. The stable window is (N-1) poll periods (first-to-last
 * sample), so a glitch shorter than the debounce window cannot confirm an
 * edge (needs N consecutive samples). POLL >= DEBOUNCE would make N = 1
 * (debounce disabled): init clamps N to 2 and logs a warning. */
#define MICHI_BUTTON_DEBOUNCE_SAMPLES \
    ((CONFIG_MICHI_BUTTON_DEBOUNCE_MS + CONFIG_MICHI_BUTTON_POLL_MS - 1) / \
     CONFIG_MICHI_BUTTON_POLL_MS)

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
/* Boot elapsed at the press confirmation, ms (F4: factory-reset arm). */
static uint32_t s_press_boot_elapsed;
/* Consecutive samples required to confirm an edge; 2 when the Kconfig
 * values make the debounce impossible (POLL >= DEBOUNCE, see init). */
static int s_debounce_samples = MICHI_BUTTON_DEBOUNCE_SAMPLES;

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

/* Hard protection: while the firmware is booting, self-testing or updating,
 * NO button action runs - a factory reset during OTA could brick the unit.
 * The pairing gate (IDLE/UNPROVISIONED only) and the recovery gate
 * (RECOVERABLE_ERROR only) sit on top of it: the FSM would drop the
 * out-of-contract events anyway, the gates keep the logs honest. */
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
    if (st == MICHI_STATE_IDLE || st == MICHI_STATE_UNPROVISIONED) {
        ESP_LOGI(TAG, "button: press_ms=%u action=pairing", (unsigned)press_ms);
        post_with_retry(MICHI_EVENT_PAIRING_STARTED, 0);
        return;
    }
    ESP_LOGW(TAG, "button: press_ms=%u action=pairing state=%s "
             "(expected IDLE or UNPROVISIONED)",
             (unsigned)press_ms, michi_state_name(st));
}

static void handle_long_press(uint32_t press_ms, michi_state_t st)
{
#ifdef CONFIG_MICHI_BUTTON_LONG_PRESS_FACTORY_RESET
    ESP_LOGW(TAG, "button: press_ms=%u action=factory_reset state=%s",
             (unsigned)press_ms, michi_state_name(st));
    /* Arm window: the press must have STARTED at least
     * MICHI_BUTTON_FACTORY_ARM_MS after boot (boot-hold, where the ISR
     * record is seeded at init, and a stuck pin both trip a press with
     * ~0 elapsed). Recovery is deliberately NOT armed. */
    if (s_press_boot_elapsed < CONFIG_MICHI_BUTTON_FACTORY_ARM_MS) {
        ESP_LOGW(TAG, "button: factory_reset ignored arm_window=%u ms "
                 "(press_elapsed=%u ms)",
                 (unsigned)CONFIG_MICHI_BUTTON_FACTORY_ARM_MS,
                 (unsigned)s_press_boot_elapsed);
        return;
    }
    const esp_err_t err = nvs_flash_erase();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "button: nvs_flash_erase failed: %s - factory reset "
                 "aborted", esp_err_to_name(err));
        return;
    }
    /* Restart immediately, no log-flush delay: the factory-reset log above
     * is already in the UART FIFO and survives the reset, and the extra
     * 200 ms only widened the window in which a concurrent shutdown could
     * hand the still-running debounce task a stale join handle. */
    esp_restart();
#else
    if (st == MICHI_STATE_RECOVERABLE_ERROR) {
        ESP_LOGI(TAG, "button: press_ms=%u action=recovery", (unsigned)press_ms);
        post_with_retry(MICHI_EVENT_RECOVER, 0);
        return;
    }
    ESP_LOGW(TAG, "button: press_ms=%u action=recovery state=%s "
             "(expected RECOVERABLE_ERROR)",
             (unsigned)press_ms, michi_state_name(st));
#endif
}

static void handle_release(uint32_t press_ms)
{
    const michi_state_t st = michi_state_get();
    const michi_state_t press_st = s_press_state;

    /* Anti-accidental gate on BOTH flanks: the press must not have started
     * in a protected state (held through boot - the ISR record is seeded at
     * init - or started during UPDATING) AND must not be released in one;
     * otherwise a press that began protected would fire its action once the
     * FSM reached IDLE. */
    if (st == MICHI_STATE_BOOTING || st == MICHI_STATE_SELF_TEST ||
        st == MICHI_STATE_UPDATING || press_st == MICHI_STATE_BOOTING ||
        press_st == MICHI_STATE_SELF_TEST || press_st == MICHI_STATE_UPDATING) {
        ESP_LOGW(TAG, "button: action=ignored press_state=%s release_state=%s",
                 michi_state_name(press_st), michi_state_name(st));
        return;
    }
    if (press_ms >= CONFIG_MICHI_BUTTON_LONG_PRESS_MS) {
        handle_long_press(press_ms, st);
    } else {
        handle_short_press(press_ms, st);
    }
}

static void button_task(void *arg)
{
    /* Debounce state (task-owned, no lock needed): s_stable is the last
     * confirmed level; s_candidate accumulates consecutive identical
     * samples until it reaches s_debounce_samples, then becomes stable. */
    int stable = 1;
    int candidate = 1;
    uint32_t candidate_count = 0;
    /* ISR timestamp of the confirmed press edge (0 = no confirmed press). */
    int64_t press_t_us = 0;

    for (;;) {
        portENTER_CRITICAL(&s_edge_mux);
        const bool stop = s_stop;
        const TaskHandle_t done = s_done_notify;
        portEXIT_CRITICAL(&s_edge_mux);

        if (stop) {
            if (done != NULL) {
                xTaskNotifyGive(done);
            }
            vTaskDelete(NULL);
        }

        const int level = gpio_get_level(CONFIG_MICHI_BUTTON_GPIO);
        if (level == candidate) {
            candidate_count++;
        } else {
            candidate = level;
            candidate_count = 1;
        }

        if (candidate_count >= s_debounce_samples && candidate != stable) {
            michi_button_edge_t edge;
            take_edge_snapshot(&edge);

            stable = candidate;
            if (stable == 0) {
                /* Stable press (active low): keep the ISR edge timestamp;
                 * the duration is measured edge-to-edge on the release.
                 * The ISR record must still say "pressed": a stale or
                 * coalesced record (edge.level != 0) cannot anchor the
                 * duration - discard it (the release then does nothing,
                 * safer than a bogus long press). */
                if (edge.level == 0) {
                    press_t_us = edge.t_us;
                    /* Snapshot the FSM state and the boot elapsed under the
                     * mux: the release gates on both (F1: the press must
                     * not have STARTED in a protected state; F4: factory
                     * reset needs the arm window). */
                    portENTER_CRITICAL(&s_edge_mux);
                    s_press_state = michi_state_get();
                    s_press_boot_elapsed =
                        (uint32_t)((esp_timer_get_time() - s_boot_time) / 1000);
                    portEXIT_CRITICAL(&s_edge_mux);
                } else {
                    ESP_LOGD(TAG, "button: press anchor discarded (edge "
                             "level=%d, stale or coalesced record)",
                             edge.level);
                }
            } else {
                /* Stable release: duration between the two confirmed edges,
                 * both ISR-timestamped (sub-tick accuracy, debounce window
                 * excluded). */
                if (press_t_us != 0 && edge.t_us > press_t_us) {
                    const uint32_t press_ms =
                        (uint32_t)((edge.t_us - press_t_us) / 1000);
                    /* Re-check the pin right before firing: if it bounced
                     * back to the pressed level (active low = 0), the
                     * release is a mid-window rebound - abort the action
                     * and wait for the next stable release. */
                    if (gpio_get_level(CONFIG_MICHI_BUTTON_GPIO) == 0) {
                        ESP_LOGW(TAG, "button: action=aborted bounce");
                    } else {
                        handle_release(press_ms);
                    }
                }
                press_t_us = 0;
            }
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

    /* Debounce sanity: with POLL >= DEBOUNCE, N = ceil(DEBOUNCE/POLL) would
     * be 1 and every sample would confirm an edge - the debounce would be
     * a no-op. Clamp N to 2 (minimal stable window) and warn instead of
     * running unguarded. */
    if (CONFIG_MICHI_BUTTON_POLL_MS >= CONFIG_MICHI_BUTTON_DEBOUNCE_MS) {
        ESP_LOGW(TAG, "debounce disabled: poll >= debounce (poll_ms=%d "
                 "debounce_ms=%d, N forced to 2)",
                 CONFIG_MICHI_BUTTON_POLL_MS, CONFIG_MICHI_BUTTON_DEBOUNCE_MS);
        s_debounce_samples = 2;
    } else {
        s_debounce_samples = MICHI_BUTTON_DEBOUNCE_SAMPLES;
    }

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
