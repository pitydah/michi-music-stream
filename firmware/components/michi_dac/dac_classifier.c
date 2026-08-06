/*
 * DAC capability classifier.
 *
 * Builds the runtime michi_dac_caps_t from the driver's silicon template plus
 * REAL evidence gathered by the manager (probe result, lifecycle state). No
 * capability is announced without that evidence: tier HiFi requires the DAC
 * to be detected AND initialized (PLL lock + clock status verified), and
 * STANDARD also degrades to DIAGNOSTIC when not detected+initialized. The
 * product tier (standard/hifi/diagnostic) is only exposed here, never
 * announced on the boot screen (that is phase 3).
 *
 * The caps template lives IN the driver descriptor (drv->template): the
 * classifier never matches drivers by name.
 */

#include <string.h>

#include "esp_log.h"

#include "michi_dac_types.h"

#include "dac_internal.h"

static const char *TAG = "michi_dac_classifier";

esp_err_t michi_dac_classifier_build(const michi_dac_driver_t *drv,
                                     michi_dac_state_t state,
                                     bool board_verified,
                                     michi_dac_caps_t *out)
{
    memset(out, 0, sizeof(*out));
    if (drv == NULL) {
        return ESP_OK; /* no DAC bound: zeroed caps, not an error */
    }
    if (drv->template == NULL) {
        ESP_LOGE(TAG, "driver=%s has no caps template (descriptor bug, "
                 "fix the driver registration)", drv->name);
        return ESP_ERR_INVALID_STATE;
    }
    *out = *drv->template;

    out->detected = (state >= MICHI_DAC_STATE_DETECTED);
    out->initialized = (state >= MICHI_DAC_STATE_INITIALIZED);
    out->board_verified = board_verified;

    switch (out->tier) {
    case MICHI_PRODUCT_STANDARD:
        /* PCM5102A stays STANDARD only when detected AND initialized: no
         * register control and no PLL/clock verification possible, so it can
         * never earn the HiFi tier - but before init evidence it degrades to
         * DIAGNOSTIC like HiFi does. */
        if (!(out->detected && out->initialized)) {
            out->tier = MICHI_PRODUCT_DIAGNOSTIC;
        }
        break;
    case MICHI_PRODUCT_HIFI:
        /* PCM512x is HiFi only when the init verification gate passed
         * (PLL locked, clocks valid, config read back). */
        if (!(out->detected && out->initialized)) {
            out->tier = MICHI_PRODUCT_DIAGNOSTIC;
        }
        break;
    default:
        out->tier = MICHI_PRODUCT_DIAGNOSTIC;
        break;
    }
    return ESP_OK;
}
