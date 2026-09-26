/* Deterministic button gesture contract (P0-01 + P1-07):
 * Hold-on-threshold classification. Pure: no I/O, no state mutation.
 * See michi_button_gesture.h for the full contract documentation.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "michi_button_gesture.h"
#include "michi_dac.h"
#include "michi_identity.h"
#include "michi_pairing.h"

#define TAG "michi_button"

static bool is_protected_state(michi_state_t st)
{
    return st == MICHI_STATE_BOOTING || st == MICHI_STATE_SELF_TEST ||
           st == MICHI_STATE_UPDATING;
}

michi_button_action_t michi_button_hold_classify(
    int64_t                        elapsed_ms,
    michi_state_t                  press_state,
    michi_state_t                  current_state,
    int64_t                        press_boot_ms,
    const michi_button_press_ctx_t *ctx,
    uint32_t                       pairing_ms,
    uint32_t                       factory_warn_ms,
    uint32_t                       factory_ms,
    uint32_t                       arm_ms)
{
    /* Hard protection: press started or currently in a protected state. */
    if (is_protected_state(press_state) || is_protected_state(current_state)) {
        return MICHI_BUTTON_ACTION_IGNORED_PROTECTED;
    }

    /* Factory-reset band: elapsed >= factory_ms.
     * Only fires if:
     *   (a) action is not already consumed
     *   (b) arm window has elapsed (boot-hold protection)
     * Note: factory reset IGNORES action_fired from pairing/recovery —
     * the escalation from pairing → factory-reset requires gesture consumption,
     * so action_fired from pairing blocks it. */
    if ((int64_t)elapsed_ms >= (int64_t)factory_ms) {
        if (ctx->action_fired) {
            /* Gesture already consumed (pairing/recovery fired): block reset. */
            return MICHI_BUTTON_ACTION_NONE;
        }
        if (press_boot_ms < (int64_t)arm_ms) {
            return MICHI_BUTTON_ACTION_IGNORED_ARM;
        }
        return MICHI_BUTTON_ACTION_FACTORY_RESET;
    }

    /* Factory-warning band: elapsed >= factory_warn_ms.
     * Visual-only: does not consume the action, does not block escalation
     * to factory reset. Only fires once (factory_warned flag in ctx). */
    if ((int64_t)elapsed_ms >= (int64_t)factory_warn_ms) {
        if (!ctx->factory_warned && !ctx->action_fired) {
            return MICHI_BUTTON_ACTION_FACTORY_WARN;
        }
        return MICHI_BUTTON_ACTION_NONE;
    }

    /* Pairing/recovery band: elapsed >= pairing_ms. */
    if ((int64_t)elapsed_ms >= (int64_t)pairing_ms) {
        if (ctx->action_fired) {
            return MICHI_BUTTON_ACTION_NONE; /* already consumed */
        }
        /* Context-sensitive: in RECOVERABLE_ERROR → recovery, else pairing. */
        if (current_state == MICHI_STATE_RECOVERABLE_ERROR) {
            return MICHI_BUTTON_ACTION_RECOVERY;
        }
        return MICHI_BUTTON_ACTION_PAIRING;
    }

    /* Below threshold: no action yet. */
    return MICHI_BUTTON_ACTION_NONE;
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

    /* Preserve hardware SKU identity (e.g. PCM5102A profile) across factory reset:
     * Non-probeable DACs rely on NVS dac_profile binding. Preserve it so a factory
     * reset returns the device to unprovisioned state without breaking audio output. */
    char saved_dac[64] = {0};
    bool had_dac = (michi_dac_get_nvs_profile(saved_dac, sizeof(saved_dac)) == ESP_OK && saved_dac[0] != '\0');

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

    if (had_dac) {
        esp_err_t nvs_err = nvs_flash_init();
        if (nvs_err == ESP_OK) {
            esp_err_t set_err = michi_dac_set_nvs_profile(saved_dac);
            if (set_err == ESP_OK) {
                ESP_LOGI(TAG, "button: restored dac_profile=%s across factory reset", saved_dac);
            } else {
                ESP_LOGW(TAG, "button: failed to restore dac_profile: %s", esp_err_to_name(set_err));
            }
        } else {
            ESP_LOGW(TAG, "button: nvs_flash_init failed during dac_profile restore: %s",
                     esp_err_to_name(nvs_err));
        }
    }

    /* Restart immediately, no log-flush delay: the factory-reset log is
     * already in the UART FIFO and survives the reset, and the extra
     * delay only widened the window in which a concurrent shutdown could
     * hand the still-running debounce task a stale join handle. */
    ESP_LOGW(TAG, "button: factory reset complete - restarting");
    esp_restart();
    return ESP_OK; /* unreachable: esp_restart() never returns */
}
