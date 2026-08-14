#pragma once

/* Internal (not public API): persistence of the server_id (== the
 * announce device_id). discovery_nvs.c is the ONLY place of the
 * component that talks to NVS; the store is one string in namespace
 * "michi_discovery" (key "server_id").
 *
 * Policy (contract sections 2.1/2.2 + section 4):
 * - UUID v4, generated exactly once with esp_fill_random (hardware RNG)
 *   and persisted on first boot; never regenerated while the store is
 *   usable.
 * - A store that exists but is structurally invalid (wrong length /
 *   non-hex / bad hyphen layout) is NOT silently regenerated: the call
 *   fails and the caller disables announces until an explicit factory
 *   reset (the physical factory reset wipes ALL of NVS).
 */

#include <stddef.h>

#include "esp_err.h"

/**
 * @brief Get the persisted server_id, generating + persisting it exactly
 *        once when the store is empty (first boot).
 *
 * @param out     Buffer (>= MICHI_DISCOVERY_UUID_LEN bytes).
 * @param out_len Size of out.
 * @return ESP_OK; ESP_ERR_INVALID_SIZE when the buffer is too small;
 *         ESP_ERR_NOT_FOUND when the store was empty AND the generated
 *         value could not be persisted (retryable next boot);
 *         ESP_ERR_INVALID_RESPONSE when the stored value is structurally
 *         invalid (factory reset required); NVS errors propagated.
 */
esp_err_t michi_discovery_nvs_get_or_create_server_id(char *out,
                                                      size_t out_len);
