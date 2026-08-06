/*
 * Internal interface shared by the michi_dac component sources.
 * Not installed: not part of the public API.
 */

#pragma once

#include <stddef.h>

#include "esp_err.h"

#include "michi_dac_types.h"

#define MICHI_DAC_REGISTRY_MAX 3 /* pcm512x, pcm5102a, mock */

const michi_dac_driver_t *michi_dac_registry_get(size_t index);
size_t michi_dac_registry_count(void);
const michi_dac_driver_t *michi_dac_registry_find_by_profile(const char *board_profile);

/* Build the runtime caps for a driver in a given lifecycle state. The caps
 * template comes from the driver descriptor itself (drv->template); a driver
 * without a template is a registration bug and returns ESP_ERR_INVALID_STATE
 * with an explicit error log. */
esp_err_t michi_dac_classifier_build(const michi_dac_driver_t *drv,
                                     michi_dac_state_t state,
                                     bool board_verified,
                                     michi_dac_caps_t *out);
