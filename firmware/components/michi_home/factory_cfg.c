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

#ifndef MICHI_HOME_TESTING
#include "esp_partition.h"
#else
static const char *s_test_factory_partition_data = NULL;
void michi_factory_cfg_set_test_partition_data(const char *data)
{
    s_test_factory_partition_data = data;
}
#endif

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

static bool parse_membership_json(const cJSON *mem, michi_membership_t *out_membership)
{
    if (mem == NULL || !cJSON_IsObject(mem) || out_membership == NULL) {
        return false;
    }
    memset(out_membership, 0, sizeof(*out_membership));

    const cJSON *ver = cJSON_GetObjectItem(mem, "version");
    out_membership->version = (ver && cJSON_IsNumber(ver)) ? (uint32_t)ver->valueint : 1;

    const cJSON *m_hid = cJSON_GetObjectItem(mem, "home_id");
    if (m_hid && cJSON_IsString(m_hid) && strlen(m_hid->valuestring) < sizeof(out_membership->home_id)) {
        snprintf(out_membership->home_id, sizeof(out_membership->home_id), "%s", m_hid->valuestring);
    } else {
        return false;
    }

    const cJSON *m_did = cJSON_GetObjectItem(mem, "device_michi_id");
    if (m_did && cJSON_IsString(m_did) && strlen(m_did->valuestring) < sizeof(out_membership->device_michi_id)) {
        snprintf(out_membership->device_michi_id, sizeof(out_membership->device_michi_id), "%s", m_did->valuestring);
    } else {
        return false;
    }

    const cJSON *m_dpk = cJSON_GetObjectItem(mem, "device_public_key");
    if (m_dpk && cJSON_IsString(m_dpk) && strlen(m_dpk->valuestring) < sizeof(out_membership->device_public_key)) {
        snprintf(out_membership->device_public_key, sizeof(out_membership->device_public_key), "%s", m_dpk->valuestring);
    } else {
        return false;
    }

    const cJSON *m_dtype = cJSON_GetObjectItem(mem, "device_type");
    if (m_dtype && cJSON_IsString(m_dtype)) {
        snprintf(out_membership->device_type, sizeof(out_membership->device_type), "%s", m_dtype->valuestring);
    }

    const cJSON *m_roles = cJSON_GetObjectItem(mem, "roles");
    if (m_roles && cJSON_IsArray(m_roles)) {
        out_membership->role_count = 0;
        const cJSON *role_item = NULL;
        cJSON_ArrayForEach(role_item, m_roles) {
            if (cJSON_IsString(role_item) && role_item->valuestring != NULL &&
                out_membership->role_count < MICHI_MAX_MEMBERSHIP_ROLES) {
                snprintf(out_membership->roles[out_membership->role_count++],
                         MICHI_MAX_ROLE_NAME_LEN, "%s", role_item->valuestring);
            }
        }
    }

    const cJSON *m_issued = cJSON_GetObjectItem(mem, "issued_at");
    if (m_issued && cJSON_IsString(m_issued)) {
        snprintf(out_membership->issued_at, sizeof(out_membership->issued_at), "%s", m_issued->valuestring);
    }

    const cJSON *m_ser = cJSON_GetObjectItem(mem, "serial");
    if (m_ser && cJSON_IsNumber(m_ser)) {
        out_membership->serial = (uint64_t)m_ser->valuedouble;
    }

    const cJSON *m_sig = cJSON_GetObjectItem(mem, "signature");
    if (m_sig && cJSON_IsString(m_sig) && strlen(m_sig->valuestring) < sizeof(out_membership->signature)) {
        snprintf(out_membership->signature, sizeof(out_membership->signature), "%s", m_sig->valuestring);
    } else {
        return false;
    }

    return true;
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
            if (item->string == NULL) continue;
            if (cJSON_IsString(item)) {
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
        }

        cJSON *mem_item = cJSON_GetObjectItem(json, "device_membership");
        if (mem_item == NULL) {
            mem_item = cJSON_GetObjectItem(json, "membership");
        }
        if (mem_item != NULL) {
            if (cJSON_IsObject(mem_item)) {
                if (parse_membership_json(mem_item, &out_cfg->device_membership)) {
                    out_cfg->has_device_membership = true;
                }
            } else if (cJSON_IsString(mem_item) && mem_item->valuestring != NULL) {
                cJSON *sub = cJSON_Parse(mem_item->valuestring);
                if (sub != NULL) {
                    if (parse_membership_json(sub, &out_cfg->device_membership)) {
                        out_cfg->has_device_membership = true;
                    }
                    cJSON_Delete(sub);
                }
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
            char line[1024];
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
                } else if (str_case_eq(key, "device_membership") || str_case_eq(key, "membership")) {
                    cJSON *sub = cJSON_Parse(val);
                    if (sub != NULL) {
                        if (parse_membership_json(sub, &out_cfg->device_membership)) {
                            out_cfg->has_device_membership = true;
                        }
                        cJSON_Delete(sub);
                    }
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

    if (cfg.has_device_membership) {
        err = michi_home_set_device_membership(&cfg.device_membership);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "factory_cfg: failed to store device membership: %s", esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "factory_cfg: device membership stored and verified");
        }
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

esp_err_t michi_factory_cfg_check_and_import(void)
{
    /* 1. If already provisioned in NVS, nothing to do */
    nvs_handle_t h;
    if (nvs_open(MICHI_HOME_NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        char id[MICHI_HOME_ID_LEN];
        size_t id_len = sizeof(id);
        esp_err_t err = nvs_get_str(h, MICHI_HOME_NVS_KEY_ID, id, &id_len);
        nvs_close(h);
        if (err == ESP_OK && id_len > 1) {
            ESP_LOGI(TAG, "factory_cfg: device already provisioned (home_id=%s), skipping", id);
            return ESP_OK;
        }
    }

#ifdef MICHI_HOME_TESTING
    if (s_test_factory_partition_data != NULL) {
        return michi_home_import_factory_cfg(s_test_factory_partition_data, strlen(s_test_factory_partition_data));
    }
    return ESP_OK;
#else
    /* 2. Locate factory_cfg partition */
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA,
        0x99,
        "factory_cfg"
    );
    if (part == NULL) {
        esp_partition_iterator_t it = esp_partition_find(
            ESP_PARTITION_TYPE_DATA,
            ESP_PARTITION_SUBTYPE_ANY,
            "factory_cfg"
        );
        if (it != NULL) {
            part = esp_partition_get(it);
            esp_partition_iterator_release(it);
        }
    }

    if (part == NULL) {
        ESP_LOGD(TAG, "factory_cfg: partition 'factory_cfg' not found");
        return ESP_OK;
    }

    char buf[4096];
    size_t to_read = sizeof(buf) - 1;
    if (to_read > part->size) {
        to_read = part->size;
    }

    esp_err_t err = esp_partition_read(part, 0, buf, to_read);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "factory_cfg: read failed: %s", esp_err_to_name(err));
        return err;
    }
    buf[to_read] = '\0';

    if ((uint8_t)buf[0] == 0xFF || buf[0] == '\0') {
        ESP_LOGD(TAG, "factory_cfg: partition empty or unprogrammed");
        return ESP_OK;
    }

    size_t content_len = 0;
    while (content_len < to_read && (uint8_t)buf[content_len] != 0xFF && buf[content_len] != '\0') {
        content_len++;
    }
    if (content_len == 0) {
        return ESP_OK;
    }
    buf[content_len] = '\0';

    ESP_LOGI(TAG, "factory_cfg: found config in partition (%zu bytes), importing...", content_len);
    err = michi_home_import_factory_cfg(buf, content_len);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "factory_cfg: one-shot import completed successfully");
    } else {
        ESP_LOGW(TAG, "factory_cfg: one-shot import failed: %s", esp_err_to_name(err));
    }
    return err;
#endif
}
