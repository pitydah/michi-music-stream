/* Fake michi_dac for host-side tests: implements NVS DAC profile get/set
 * against fake NVS so factory reset SKU preservation can be tested.
 * TEST-ONLY. */

#include "michi_dac.h"
#include "nvs.h"

esp_err_t michi_dac_get_nvs_profile(char *profile, size_t buf_len)
{
    if (profile == NULL || buf_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(MICHI_DAC_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }
    size_t required = buf_len;
    err = nvs_get_str(handle, MICHI_DAC_NVS_KEY_PROFILE, profile, &required);
    nvs_close(handle);
    return err;
}

esp_err_t michi_dac_set_nvs_profile(const char *profile)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(MICHI_DAC_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    if (profile == NULL || profile[0] == '\0') {
        err = nvs_erase_key(handle, MICHI_DAC_NVS_KEY_PROFILE);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    } else {
        err = nvs_set_str(handle, MICHI_DAC_NVS_KEY_PROFILE, profile);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}
