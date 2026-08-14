#pragma once

/* Deterministic button gesture contract (P1-07): the pure decision logic
 * of the pairing button, separated from the debounce task so host tests
 * can prove the EXACT gesture boundaries (4999/5000/9999/10000 ms)
 * without GPIO hardware.
 *
 * The gesture table (Kconfig thresholds, defaults):
 *
 *   press_ms < MICHI_BUTTON_RECOVERY_PRESS_MS
 *       -> PAIRING (the physical press is the ONLY authority that opens
 *          the pairing window; the pairing execution additionally gates
 *          on IDLE/UNPROVISIONED/PAIRING)
 *   MICHI_BUTTON_RECOVERY_PRESS_MS <= press_ms
 *       < MICHI_BUTTON_FACTORY_RESET_PRESS_MS
 *       -> RECOVERY only when the FSM is in RECOVERABLE_ERROR at the
 *          RELEASE (post MICHI_EVENT_RECOVER; a press released after the
 *          FSM already recovered is ignored - the device fixed itself)
 *   press_ms >= MICHI_BUTTON_FACTORY_RESET_PRESS_MS
 *       -> FACTORY_RESET, armed: the press must have STARTED at least
 *          MICHI_BUTTON_FACTORY_ARM_MS after boot (boot-hold / stuck-pin
 *          protection). Recovery is NOT armed.
 *
 * Hard protection on top of every band: a press that STARTED or ENDED in
 * BOOTING, SELF_TEST or UPDATING is IGNORED - a press held through boot,
 * or started during OTA, must never fire on release (a factory reset
 * during OTA could brick the unit).
 *
 * A corrupt identity does NOT change the FSM state: the factory-reset
 * gesture stays available, which is the ONLY physical recovery path for
 * a corrupt identity store.
 */

#include <stdint.h>

#include "esp_err.h"
#include "michi_state.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MICHI_BUTTON_ACTION_PAIRING = 0, /*!< Short press: open the pairing window */
    MICHI_BUTTON_ACTION_RECOVERY,    /*!< Long press in RECOVERABLE_ERROR */
    MICHI_BUTTON_ACTION_FACTORY_RESET, /*!< Very long, armed press: NVS wipe + restart */
    /* Long press whose release state is not RECOVERABLE_ERROR. */
    MICHI_BUTTON_ACTION_IGNORED_STATE,
    /* The press started or ended in BOOTING/SELF_TEST/UPDATING. */
    MICHI_BUTTON_ACTION_IGNORED_PROTECTED,
    /* Factory-reset band, but the press started before the arm window. */
    MICHI_BUTTON_ACTION_IGNORED_ARM,
} michi_button_action_t;

/**
 * @brief Classify a confirmed press into the deterministic gesture
 *        action. Pure: no I/O, no state mutation, no time sources - the
 *        debounce task feeds it with the ISR-measured press duration,
 *        the FSM state snapshots (press confirmation / release) and the
 *        boot elapsed at the press confirmation.
 *
 * @param press_ms             Press duration (edge-to-edge, ISR timestamps).
 * @param press_state          FSM state at the press confirmation.
 * @param release_state        FSM state at the release.
 * @param press_boot_elapsed_ms Elapsed since the button init reference when
 *                             the press was confirmed.
 * @param recovery_ms          MICHI_BUTTON_RECOVERY_PRESS_MS.
 * @param factory_ms           MICHI_BUTTON_FACTORY_RESET_PRESS_MS
 *                             (must be > recovery_ms).
 * @param arm_ms               MICHI_BUTTON_FACTORY_ARM_MS.
 */
michi_button_action_t michi_button_gesture_classify(
    uint32_t press_ms, michi_state_t press_state, michi_state_t release_state,
    int64_t press_boot_elapsed_ms, uint32_t recovery_ms, uint32_t factory_ms,
    uint32_t arm_ms);

/**
 * @brief Execute the factory reset: wipe the component-side state
 *        (identity keys + pairing registry), erase the WHOLE NVS
 *        partition and restart.
 *
 * Order is deliberate: michi_identity_factory_reset() erases its own
 * key and wipes the in-RAM keys FIRST - if the full NVS erase ran first,
 * the key would already be gone and the RAM wipe would be skipped (the
 * identity contract keeps RAM consistent with the store). The full
 * partition erase then covers everything without a dedicated hook
 * (wifi credentials, DAC override, the discovery server_id, boot_seq).
 *
 * @return ESP_OK (esp_restart never returns); the NVS error when the
 *         full partition erase fails (reset aborted, no restart).
 */
esp_err_t michi_button_factory_reset_run(void);

#ifdef __cplusplus
}
#endif
