#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Physical pairing button (phase 8): debounced edge detection on
 * MICHI_BUTTON_GPIO (default GPIO17, camera PWDN, active low).
 *
 * Architecture (see firmware/README.md, Button section):
 * - The GPIO ISR service handler ONLY records the edge level + timestamp
 *   (esp_timer_get_time) in a volatile struct under a portMUX critical
 *   section. It contains NO logic: no debounce, no pairing, no state posts.
 * - The debounce task (priority 2, MICHI_BUTTON_POLL_MS tick) polls
 *   gpio_get_level and requires N consecutive equal samples to confirm a
 *   press (stable low) or a release (stable high), N = DEBOUNCE/POLL
 *   rounded up (the stable window is (N-1) poll periods; init clamps N to
 *   2 and logs a warning when POLL >= DEBOUNCE). The press duration is
 *   measured edge-to-edge from the ISR-recorded timestamps (sub-tick
 *   accuracy); the pin level is re-checked right before an action fires,
 *   so a mid-window bounce aborts the release.
 * - All actions run in the task, NEVER in the ISR. The gesture
 *   classification is deterministic (contract in michi_button_gesture.h,
 *   pure + host-tested):
 *     - short press (< MICHI_BUTTON_RECOVERY_PRESS_MS, default 5000 ms)
 *       opens the pairing window (michi_pairing_open_window, phase 10 -
 *       the physical press is the ONLY authority that opens it, there is
 *       no network-visible way) and posts MICHI_EVENT_PAIRING_STARTED
 *       when the FSM is in IDLE or UNPROVISIONED;
 *     - long press (>= MICHI_BUTTON_RECOVERY_PRESS_MS and
 *       < MICHI_BUTTON_FACTORY_RESET_PRESS_MS, default 10000 ms) posts
 *       MICHI_EVENT_RECOVER ONLY when the FSM is in RECOVERABLE_ERROR at
 *       the release (ignored otherwise);
 *     - very long press (>= MICHI_BUTTON_FACTORY_RESET_PRESS_MS) runs the
 *       factory reset (michi_button_factory_reset_run: identity wipe +
 *       pairing registry wipe + full NVS erase + esp_restart), armed by
 *       MICHI_BUTTON_FACTORY_ARM_MS (the press must have STARTED at
 *       least that long after boot). Both gestures are always compiled -
 *       the physical factory reset must never disappear behind a Kconfig
 *       choice. Posts are retried once after 50 ms on ESP_ERR_TIMEOUT,
 *       then dropped and logged; a window-open failure is logged without
 *       breaking the post.
 * - Anti-accidental gates: actions are ignored unless the FSM state at
 *   the press confirmation AND at the release are both outside BOOTING,
 *   SELF_TEST and UPDATING (a press held through boot, or started during
 *   OTA, must never fire on release). The factory reset additionally
 *   requires the press to have started at least
 *   MICHI_BUTTON_FACTORY_ARM_MS after boot (boot-hold / stuck-pin
 *   protection); recovery is not armed. A corrupt identity does not
 *   change the FSM state, so the factory-reset gesture stays available
 *   as the only physical recovery path for a corrupt identity store.
 */

/**
 * @brief Initialize the button subsystem: GPIO input with pull-up, both-
 *        edge ISR handler (records edge + timestamp only) and the debounce
 *        task.
 *
 * The ISR handler is IRAM-safe (it only calls gpio_get_level,
 * esp_timer_get_time and portENTER_CRITICAL_ISR/portEXIT_CRITICAL_ISR - no
 * flash access), so the GPIO ISR service is installed with
 * ESP_INTR_FLAG_IRAM; if another component already installed the service,
 * it is reused - the handler stays safe under either service, and shutdown
 * will NOT uninstall it.
 *
 * Must be called after michi_state_init(). Safe to call once; repeated
 * calls return ESP_OK (idempotent). On failure app_main continues degraded:
 * no pairing button, everything else keeps working.
 *
 * @return ESP_OK; raw driver errors from gpio_config/gpio_install_isr_service
 *         are propagated unchanged (e.g. ESP_ERR_INVALID_ARG for an invalid
 *         GPIO); ESP_ERR_NO_MEM if the task cannot be created.
 */
esp_err_t michi_button_init(void);

/**
 * @brief Shut the button subsystem down: cooperative debounce-task stop
 *        (flag + notification + join with timeout), ISR handler removed,
 *        GPIO ISR service uninstalled ONLY if this component installed it.
 *
 * Idempotent: a second call when the subsystem is already off returns
 * ESP_OK. Safe to call from any task; the caller must not be the debounce
 * task. NOT safe for concurrent shutdown callers: a second shutdown while
 * one is in progress returns ESP_ERR_INVALID_STATE - serialize externally.
 * On join timeout the ISR handler is left registered, the join target is
 * cleared (the live task never notifies a stale handle) and the task may
 * still be running and sampling.
 *
 * @return ESP_OK; ESP_ERR_TIMEOUT if the debounce task did not stop within
 *         the join timeout; ESP_ERR_INVALID_STATE if called from the
 *         debounce task itself or while another shutdown is in progress.
 */
esp_err_t michi_button_shutdown(void);

#ifdef __cplusplus
}
#endif
