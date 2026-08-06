#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Physical pairing button (phase 8): debounced edge detection on
 *        MICHI_BUTTON_GPIO (default GPIO17, camera PWDN, active low).
 *
 * Architecture (see firmware/README.md, Button section):
 * - The GPIO ISR service handler ONLY records the edge level + timestamp
 *   (esp_timer_get_time) in a volatile struct under a portMUX critical
 *   section. It contains NO logic: no debounce, no pairing, no state posts.
 * - The debounce task (priority 2, MICHI_BUTTON_POLL_MS tick) polls
 *   gpio_get_level, requires MICHI_BUTTON_DEBOUNCE_MS of stable samples
 *   (DEBOUNCE/POLL consecutive equal readings) to confirm a press (stable
 *   low) or a release (stable high), and measures the press duration
 *   edge-to-edge from the ISR-recorded timestamps (sub-tick accuracy).
 * - All actions run in the task, NEVER in the ISR: a short press
 *   (< MICHI_BUTTON_LONG_PRESS_MS) posts MICHI_EVENT_PAIRING_STARTED when
 *   the FSM is in IDLE or UNPROVISIONED (the pairing protocol itself is
 *   phase 10 - this component only posts events). A long press runs
 *   MICHI_BUTTON_LONG_PRESS_ACTION: recovery (post MICHI_EVENT_RECOVER,
 *   default) or factory_reset (nvs_flash_erase + esp_restart). Both
 *   actions are ignored while the FSM is in BOOTING, SELF_TEST or
 *   UPDATING (a factory reset during OTA could brick the unit).
 */

/**
 * @brief Initialize the button subsystem: GPIO input with pull-up, both-
 *        edge ISR handler (records edge + timestamp only) and the debounce
 *        task.
 *
 * Must be called after michi_state_init(). Safe to call once; repeated
 * calls return ESP_OK (idempotent). On failure app_main continues degraded:
 * no pairing button, everything else keeps working.
 *
 * @return ESP_OK; raw driver errors from gpio_config/gpio_install_isr_service
 *         are propagated unchanged (e.g. ESP_ERR_INVALID_ARG for an invalid
 *         GPIO); ESP_ERR_NO_MEM if the task cannot be created. If another
 *         component already installed the GPIO ISR service, it is reused
 *         (shutdown will NOT uninstall it).
 */
esp_err_t michi_button_init(void);

/**
 * @brief Shut the button subsystem down: cooperative debounce-task stop
 *        (flag + notification + join with timeout), ISR handler removed,
 *        GPIO ISR service uninstalled ONLY if this component installed it.
 *
 * Idempotent: a second call when the subsystem is already off returns
 * ESP_OK. Safe to call from any task; the caller must not be the debounce
 * task. If the task does not stop within the join timeout the ISR handler
 * is left registered and ESP_ERR_TIMEOUT is returned (the task may still be
 * running and sampling).
 *
 * @return ESP_OK; ESP_ERR_TIMEOUT if the debounce task did not stop within
 *         the join timeout; ESP_ERR_INVALID_STATE only if called from the
 *         debounce task itself.
 */
esp_err_t michi_button_shutdown(void);

#ifdef __cplusplus
}
#endif
