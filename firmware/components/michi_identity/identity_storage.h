#pragma once

/* Internal (not public API): the thin NVS persistence wrapper for the
 * identity seed. michi_identity.c owns state transitions and crypto;
 * identity_nvs.c is the ONLY place that touches nvs.h - the store layout
 * is a single versioned blob, mirroring the michi_pairing registry
 * conventions.
 *
 * Layout (MICHI_IDENTITY_BLOB_VERSION = 1):
 *   offset 0: u32 version
 *   offset 4: u8 seed[32]   (the Ed25519 seed, RFC 8032)
 *   offset 36: u8 reserved[4] (explicit tail padding: deterministic
 *               persisted bytes, no uninitialized padding on flash)
 * Total: 40 bytes, guarded by _Static_assert.
 */

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define MICHI_IDENTITY_NVS_NAMESPACE "michi_identity"
#define MICHI_IDENTITY_NVS_KEY "seed"
#define MICHI_IDENTITY_BLOB_VERSION 1u

#define MICHI_IDENTITY_SEED_BYTES 32

typedef struct {
    uint32_t version;
    uint8_t seed[MICHI_IDENTITY_SEED_BYTES];
    uint8_t reserved[4];
} michi_identity_blob_t;

/**
 * @brief Load the identity blob from NVS.
 *
 * @return ESP_OK; ESP_ERR_NOT_FOUND when the store is empty (first
 *         boot - the caller generates and persists); any other error
 *         when the store exists but cannot be read (corrupt NVS item:
 *         the caller enters CORRUPT).
 */
esp_err_t michi_identity_nvs_load(michi_identity_blob_t *out);

/**
 * @brief Persist the identity blob (set_blob + commit).
 *
 * @return ESP_OK; NVS errors propagated unchanged.
 */
esp_err_t michi_identity_nvs_store(const michi_identity_blob_t *blob);

/**
 * @brief Erase the persisted identity (factory reset).
 *
 * @return ESP_OK; NVS errors propagated unchanged.
 */
esp_err_t michi_identity_nvs_erase(void);
