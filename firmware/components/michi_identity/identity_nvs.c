/* Thin NVS wrapper for the persistent identity seed (MS-04).
 *
 * This is the single place in the component that talks to NVS. The store
 * is one versioned blob in namespace "michi_identity" (key "seed"),
 * layout defined in identity_storage.h. NVS guarantees CRC-checked blob
 * reads: a torn flash write surfaces as a read error, which the caller
 * maps to the CORRUPT state (factory reset required - never silently
 * regenerated).
 */

#include <string.h>

#include "esp_log.h"
#include "nvs.h"

#include "identity_storage.h"

#define TAG "michi_identity"

_Static_assert(sizeof(michi_identity_blob_t) == 40,
               "identity blob layout changed; the persisted blob format breaks");

esp_err_t michi_identity_nvs_load(michi_identity_blob_t *out)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(MICHI_IDENTITY_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t len = sizeof(*out);
    err = nvs_get_blob(handle, MICHI_IDENTITY_NVS_KEY, out, &len);
    nvs_close(handle);

    if (err == ESP_OK && len != sizeof(*out)) {
        /* Structurally wrong blob: treated as corruption. */
        ESP_LOGE(TAG, "identity: store_blob_wrong_length");
        return ESP_ERR_INVALID_SIZE;
    }
    return err;
}

esp_err_t michi_identity_nvs_store(const michi_identity_blob_t *blob)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(MICHI_IDENTITY_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_blob(handle, MICHI_IDENTITY_NVS_KEY, blob, sizeof(*blob));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t michi_identity_nvs_erase(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(MICHI_IDENTITY_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_erase_key(handle, MICHI_IDENTITY_NVS_KEY);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}
