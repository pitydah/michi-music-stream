/* Deterministic button gesture contract (P1-07): the pure decision
 * logic + the factory-reset orchestration, both host-testable without
 * GPIO/task plumbing. The debounce task (michi_button.c) feeds the
 * classification with the press duration, the FSM state snapshots and
 * the boot elapsed; this file owns the contract.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "michi_button_gesture.h"
#include "michi_identity.h"
#include "michi_pairing.h"

#define TAG "michi_button"

static bool is_protected_state(michi_state_t st)
{
    return st == MICHI_STATE_BOOTING || st == MICHI_STATE_SELF_TEST ||
           st == MICHI_STATE_UPDATING;
}

michi_button_action_t michi_button_gesture_classify(
    uint32_t press_ms, michi_state_t press_state, michi_state_t release_state,
    int64_t press_boot_elapsed_ms, uint32_t recovery_ms, uint32_t factory_ms,
    uint32_t arm_ms)
{
    /* Hard protection on BOTH flanks: the press must not have started in
     * a protected state (held through boot, or started during OTA) AND
     * must not be released in one - otherwise a press that began
     * protected would fire its action once the FSM reached a stable
     * state. */
    if (is_protected_state(press_state) || is_protected_state(release_state)) {
        return MICHI_BUTTON_ACTION_IGNORED_PROTECTED;
    }

    if (press_ms >= factory_ms) {
        if (press_boot_elapsed_ms < (int64_t)arm_ms) {
            return MICHI_BUTTON_ACTION_IGNORED_ARM;
        }
        return MICHI_BUTTON_ACTION_FACTORY_RESET;
    }

    if (press_ms >= recovery_ms) {
        /* Recovery is a retry gesture: it fires only when the device is
         * actually in RECOVERABLE_ERROR at the release (the press state
         * does not gate it - a press started in IDLE that ends after an
         * error landed is exactly the gesture that should work). */
        if (release_state != MICHI_STATE_RECOVERABLE_ERROR) {
            return MICHI_BUTTON_ACTION_IGNORED_STATE;
        }
        return MICHI_BUTTON_ACTION_RECOVERY;
    }

    return MICHI_BUTTON_ACTION_PAIRING;
}

esp_err_t michi_button_factory_reset_run(void)
{
    /* Component-side wipes FIRST, full partition erase LAST:
     * michi_identity_factory_reset() erases its own key and wipes the
     * in-RAM keys - if the full NVS erase ran first, the key would
     * already be gone and the RAM wipe would be skipped (the identity
     * contract keeps RAM consistent with the store). The full erase
     * covers everything without a dedicated hook (wifi credentials, DAC
     * override, the discovery server_id, boot_seq); the discovery
     * component has no factory-reset hook of its own. */
    esp_err_t err = michi_identity_factory_reset();
    if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_NVS_NOT_FOUND) {
        /* Fresh device: no persisted identity - nothing to erase, the
         * RAM state is already UNINITIALIZED. Benign. */
        ESP_LOGI(TAG, "button: identity not persisted (fresh device)");
    } else if (err != ESP_OK) {
        ESP_LOGW(TAG, "button: identity factory reset failed: %s - the "
                 "full NVS erase below still clears the store",
                 esp_err_to_name(err));
    }

    err = michi_pairing_erase_all();
    if (err == ESP_ERR_INVALID_STATE) {
        /* Pairing never initialized: no in-RAM registry to wipe; the
         * full NVS erase below removes any persisted registry. */
        ESP_LOGI(TAG, "button: pairing not initialized - nothing in RAM "
                 "to wipe");
    } else if (err != ESP_OK) {
        ESP_LOGW(TAG, "button: pairing erase failed: %s - the full NVS "
                 "erase below still clears the registry",
                 esp_err_to_name(err));
    }

    err = nvs_flash_erase();
    if (err != ESP_OK) {
        /* Honest abort: without the full erase the reset did not achieve
         * its purpose, so the device keeps running (no restart) with the
         * identity/pairing state already wiped - degraded but never
         * bricked; the log says exactly what happened. */
        ESP_LOGE(TAG, "button: nvs_flash_erase failed: %s - factory reset "
                 "aborted", esp_err_to_name(err));
        return err;
    }

    /* Restart immediately, no log-flush delay: the factory-reset log is
     * already in the UART FIFO and survives the reset, and the extra
     * delay only widened the window in which a concurrent shutdown could
     * hand the still-running debounce task a stale join handle. */
    ESP_LOGW(TAG, "button: factory reset complete - restarting");
    esp_restart();
    return ESP_OK; /* unreachable: esp_restart() never returns */
}
