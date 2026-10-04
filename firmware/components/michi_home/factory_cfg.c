#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "nvs.h"

#include "cJSON.h"
#include "michi_home.h"
#include "michi_identity.h"

#define TAG "michi_factory_cfg"

static void trim_whitespace(char *str)
{
    if (str == NULL) return;
    char *end = str + strlen(str) - 1;
    while (end >= str && isspace((unsigned char)*end)) {
        *end = '\0';
        end--;
    }
}

static bool str_case_eq(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        a++;
        b++;
    }
    return *a == *b;
}

esp_err_t michi_factory_cfg_parse(const char *payload, size_t len, michi_factory_cfg_t *out_cfg)
{
    if (payload == NULL || len == 0 || out_cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_cfg, 0, sizeof(*out_cfg));

    /* Attempt 1: Parse as JSON */
    cJSON *json = cJSON_ParseWithLength(payload, len);
    if (json != NULL) {
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, json) {
            if (item->string == NULL || !cJSON_IsString(item)) continue;
            if (str_case_eq(item->string, "home_id")) {
                snprintf(out_cfg->home_id, sizeof(out_cfg->home_id), "%s", item->valuestring);
            } else if (str_case_eq(item->string, "root_public_key")) {
                snprintf(out_cfg->root_public_key, sizeof(out_cfg->root_public_key), "%s", item->valuestring);
            } else if (str_case_eq(item->string, "wifi_ssid")) {
                snprintf(out_cfg->wifi_ssid, sizeof(out_cfg->wifi_ssid), "%s", item->valuestring);
            } else if (str_case_eq(item->string, "wifi_password")) {
                snprintf(out_cfg->wifi_password, sizeof(out_cfg->wifi_password), "%s", item->valuestring);
            }
        }
        cJSON_Delete(json);
        return ESP_OK;
    }

    /* Attempt 2: Parse line-delimited key=value format */
    const char *cursor = payload;
    const char *end = payload + len;

    while (cursor < end) {
        const char *next = memchr(cursor, '\n', (size_t)(end - cursor));
        size_t line_len = next ? (size_t)(next - cursor) : (size_t)(end - cursor);
        if (line_len > 0) {
            char line[256];
            if (line_len >= sizeof(line)) line_len = sizeof(line) - 1;
            memcpy(line, cursor, line_len);
            line[line_len] = '\0';
            trim_whitespace(line);

            char *eq = strchr(line, '=');
            if (eq != NULL) {
                *eq = '\0';
                char *key = line;
                char *val = eq + 1;
                trim_whitespace(key);
                trim_whitespace(val);
                if (str_case_eq(key, "home_id")) {
                    snprintf(out_cfg->home_id, sizeof(out_cfg->home_id), "%s", val);
                } else if (str_case_eq(key, "root_public_key")) {
                    snprintf(out_cfg->root_public_key, sizeof(out_cfg->root_public_key), "%s", val);
                } else if (str_case_eq(key, "wifi_ssid")) {
                    snprintf(out_cfg->wifi_ssid, sizeof(out_cfg->wifi_ssid), "%s", val);
                } else if (str_case_eq(key, "wifi_password")) {
                    snprintf(out_cfg->wifi_password, sizeof(out_cfg->wifi_password), "%s", val);
                }
            }
        }
        if (next == NULL) break;
        cursor = next + 1;
    }

    return ESP_OK;
}

esp_err_t michi_home_import_factory_cfg(const char *payload, size_t len)
{
    michi_factory_cfg_t cfg;
    esp_err_t err = michi_factory_cfg_parse(payload, len, &cfg);
    if (err != ESP_OK) {
        return err;
    }

    if (strlen(cfg.home_id) != 43 || strlen(cfg.root_public_key) != 43) {
        ESP_LOGE(TAG, "factory_cfg: missing or invalid home_id or root_public_key");
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t root_pk[MICHI_HOME_KEY_BYTES];
    size_t decoded_len = 0;
    if (michi_identity_base64url_decode(cfg.root_public_key, root_pk, sizeof(root_pk), &decoded_len) != ESP_OK ||
        decoded_len != MICHI_HOME_KEY_BYTES) {
        ESP_LOGE(TAG, "factory_cfg: root_public_key decoding failed");
        return ESP_ERR_INVALID_ARG;
    }

    err = michi_home_set_credentials(cfg.home_id, root_pk);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "factory_cfg: failed to store home credentials: %s", esp_err_to_name(err));
        return err;
    }

    /* If Wi-Fi credentials provided, save to NVS "wifi" */
    if (cfg.wifi_ssid[0] != '\0') {
        nvs_handle_t wh;
        if (nvs_open("wifi", NVS_READWRITE, &wh) == ESP_OK) {
            (void)nvs_set_str(wh, "ssid", cfg.wifi_ssid);
            (void)nvs_set_str(wh, "password", cfg.wifi_password);
            (void)nvs_commit(wh);
            nvs_close(wh);
            ESP_LOGI(TAG, "factory_cfg: Wi-Fi credentials imported (SSID: %s)", cfg.wifi_ssid);
        }
    }

    ESP_LOGI(TAG, "factory_cfg: Home credentials imported successfully (home_id: %s)", cfg.home_id);
    return ESP_OK;
}
