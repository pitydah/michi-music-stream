/* Thin NVS wrapper for the persistent server_id (MS-05). See
 * discovery_nvs.h for the policy (generate once, never silently
 * regenerate a corrupt store).
 *
 * The value is stored as the formatted UUID v4 string (36 chars + NUL);
 * the raw bytes are drawn from esp_fill_random with the RFC 4122
 * version/variant bits set before formatting, so the string is a valid
 * UUID v4 by construction. */

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"

#include "discovery_nvs.h"
#include "michi_discovery.h"

#define TAG "michi_discovery"

/* Strict structural check of a persisted UUID v4 string:
 * 36 chars, hyphens at 8/13/18/23, hex elsewhere, version nibble '4'
 * and variant nibble 8/9/a/b. Rejects junk without regenerating. */
static bool uuid_valid(const char *s)
{
    if (s == NULL || strlen(s) != 36) {
        return false;
    }
    static const char layout[36] = {
        'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x', '-',
        'x', 'x', 'x', 'x', '-',
        '4', 'x', 'x', 'x', '-',
        'x', 'x', 'x', 'x', '-',
        'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x',
    };
    for (size_t i = 0; i < sizeof(layout); i++) {
        if (layout[i] == '-') {
            if (s[i] != '-') {
                return false;
            }
        } else if (layout[i] == '4') {
            if (s[i] != '4') {
                return false;
            }
        } else if (!((s[i] >= '0' && s[i] <= '9') ||
                     (s[i] >= 'a' && s[i] <= 'f') ||
                     (s[i] >= 'A' && s[i] <= 'F'))) {
            return false;
        }
    }
    /* Variant nibble (byte 8, first char of the 4th group): 8/9/a/b. */
    const char v = s[19];
    return v == '8' || v == '9' || v == 'a' || v == 'b' || v == 'A' ||
           v == 'B';
}

static void uuid_generate(char *out, size_t out_len)
{
    uint8_t raw[16];
    esp_fill_random(raw, sizeof(raw));
    raw[6] = (uint8_t)((raw[6] & 0x0Fu) | 0x40u); /* version 4 */
    raw[8] = (uint8_t)((raw[8] & 0x3Fu) | 0x80u); /* variant 10xx */
    snprintf(out, out_len,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
             "%02x%02x%02x%02x%02x%02x",
             raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7],
             raw[8], raw[9], raw[10], raw[11], raw[12], raw[13], raw[14],
             raw[15]);
}

esp_err_t michi_discovery_nvs_get_or_create_server_id(char *out,
                                                      size_t out_len)
{
    if (out == NULL || out_len < MICHI_DISCOVERY_UUID_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }

    nvs_handle_t h;
    esp_err_t err =
        nvs_open(MICHI_DISCOVERY_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_OK) {
        size_t len = out_len;
        err = nvs_get_str(h, MICHI_DISCOVERY_NVS_KEY, out, &len);
        nvs_close(h);
        if (err == ESP_OK) {
            if (!uuid_valid(out)) {
                ESP_LOGE(TAG, "discovery: server_id store corrupt - "
                         "factory reset required (never regenerated)");
                return ESP_ERR_INVALID_RESPONSE;
            }
            return ESP_OK;
        }
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            /* Store exists but cannot be read: do NOT regenerate over it. */
            ESP_LOGE(TAG, "discovery: server_id read failed: %s",
                     esp_err_to_name(err));
            return err;
        }
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "discovery: server_id namespace open failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    /* Empty store: first boot - generate exactly once and persist. */
    uuid_generate(out, out_len);
    err = nvs_open(MICHI_DISCOVERY_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "discovery: server_id persist open failed: %s",
                 esp_err_to_name(err));
        return err;
    }
    err = nvs_set_str(h, MICHI_DISCOVERY_NVS_KEY, out);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "discovery: server_id persist failed: %s (the boot "
                 "will retry next time)", esp_err_to_name(err));
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "discovery: server_id minted");
    return ESP_OK;
}
