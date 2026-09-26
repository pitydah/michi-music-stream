#pragma once

/* Deterministic button gesture contract (P0-01 + P1-07):
 *
 * NEW CONTRACT — hold-on-threshold (P0-01):
 *   The gesture action fires when the HOLD DURATION CROSSES the threshold
 *   while the button is still pressed, NOT on release.  Once fired, the
 *   action is CONSUMED (action_fired = true): the same press cannot
 *   escalate to a more destructive action silently.
 *
 * THRESHOLDS (Kconfig defaults):
 *   0 – PAIRING_HOLD_MS-1         hold zone (feedback only)
 *   PAIRING_HOLD_MS (5000 ms)     → PAIRING fired (threshold crossing)
 *   FACTORY_WARN_MS (10000 ms)    → visual warning: impending factory reset
 *   FACTORY_RESET_MS (15000 ms)   → FACTORY_RESET fired
 *
 * In RECOVERABLE_ERROR state, the 5 s crossing fires RECOVERY instead
 * of pairing (same threshold, context-sensitive).
 *
 * GESTURE CONSUMPTION:
 *   Once any action fires, action_fired is set.  A subsequent crossing of
 *   a higher threshold DOES NOT fire a new action — the gesture is consumed.
 *   The only exception is the factory-reset warning (visual only, not an
 *   action, consumed flag not set).
 *
 * HARD PROTECTION:
 *   A press that STARTED or is currently in BOOTING/SELF_TEST/UPDATING
 *   is IGNORED entirely — a factory reset during OTA could brick the unit.
 *
 * RELEASE:
 *   On release, the task clears visual feedback and the press context.
 *   If action_fired is true, NO action is executed on release.
 *   If the press was shorter than MIN_PRESS_MS, it is treated as noise.
 *   Any release of a non-consumed press shorter than PAIRING_HOLD_MS is
 *   silently discarded (the user did not hold long enough for any action).
 */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "michi_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Action enum ---- */

typedef enum {
    /* Fired on threshold crossing while pressed. */
    MICHI_BUTTON_ACTION_PAIRING        = 0, /*!< 5 s hold: open pairing window */
    MICHI_BUTTON_ACTION_RECOVERY,           /*!< 5 s hold in RECOVERABLE_ERROR */
    MICHI_BUTTON_ACTION_FACTORY_WARN,       /*!< 10 s: visual warning only */
    MICHI_BUTTON_ACTION_FACTORY_RESET,      /*!< 15 s: NVS wipe + restart */
    /* No action at this elapsed. */
    MICHI_BUTTON_ACTION_NONE,
    /* Protected state: press ignored entirely. */
    MICHI_BUTTON_ACTION_IGNORED_PROTECTED,
    /* Factory-reset arm window not elapsed (boot-hold protection). */
    MICHI_BUTTON_ACTION_IGNORED_ARM,
} michi_button_action_t;

/* ---- Hold context (owned by button_task, not by this module) ---- */

/* The press context captures everything the task needs to manage a single
 * button press lifecycle:
 *   pressed        : true between debounce-confirmed press and release
 *   action_fired   : pairing OR recovery has been executed; blocks escalation
 *   factory_warned : warning visual displayed; factory reset pending
 *   press_state    : FSM state at press confirmation
 *   pressed_at_us  : esp_timer_get_time() at press confirmation
 *   press_boot_ms  : boot elapsed at press confirmation (arm gate)
 */
typedef struct {
    bool           pressed;
    bool           action_fired;
    bool           factory_warned;
    michi_state_t  press_state;
    int64_t        pressed_at_us;
    int64_t        press_boot_ms;
} michi_button_press_ctx_t;

/* ---- Pure classification API ---- */

/**
 * @brief Classify the action that should fire at the given hold duration.
 *        Called by the task loop while the button is pressed.
 *        Pure: no I/O, no state mutation.
 *
 * @param elapsed_ms         Time held so far (now_us - pressed_at_us) / 1000.
 * @param press_state        FSM state at the confirmed press edge.
 * @param current_state      FSM state right now (for protected-state check).
 * @param press_boot_ms      Boot elapsed at press confirmation (arm gate).
 * @param ctx                Current press context (action_fired, factory_warned).
 * @param pairing_ms         MICHI_BUTTON_PAIRING_HOLD_MS.
 * @param factory_warn_ms    MICHI_BUTTON_FACTORY_WARN_MS.
 * @param factory_ms         MICHI_BUTTON_FACTORY_RESET_PRESS_MS.
 * @param arm_ms             MICHI_BUTTON_FACTORY_ARM_MS.
 * @return                   Action to take right now, or NONE / IGNORED_*.
 */
michi_button_action_t michi_button_hold_classify(
    int64_t               elapsed_ms,
    michi_state_t         press_state,
    michi_state_t         current_state,
    int64_t               press_boot_ms,
    const michi_button_press_ctx_t *ctx,
    uint32_t              pairing_ms,
    uint32_t              factory_warn_ms,
    uint32_t              factory_ms,
    uint32_t              arm_ms);

/**
 * @brief Execute the factory reset: wipe the component-side state
 *        (identity keys + pairing registry), erase the WHOLE NVS
 *        partition and restart.
 *
 * Order is deliberate: michi_identity_factory_reset() erases its own
 * key and wipes the in-RAM keys FIRST - if the full NVS erase ran first,
 * the key would already be gone and the RAM wipe would be skipped (the
 * identity contract keeps RAM consistent with the store). The full
 * partition erase then covers everything without a dedicated hook.
 *
 * @return ESP_OK (esp_restart never returns); the NVS error when the
 *         full partition erase fails (reset aborted, no restart).
 */
esp_err_t michi_button_factory_reset_run(void);

#ifdef __cplusplus
}
#endif
