#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "nvs.h"

#include "cJSON.h"
#include "michi_home.h"
#include "michi_identity.h"

#ifndef ESP_ERR_INVALID_CRC
#define ESP_ERR_INVALID_CRC 0x109
#endif

#ifndef MICHI_HOME_TESTING
#include "esp_partition.h"
#else
static const char *s_test_factory_partition_data = NULL;
static const uint8_t *s_test_factory_partition_bytes = NULL;
static size_t s_test_factory_partition_len = 0;

void michi_factory_cfg_set_test_partition_data(const char *data)
{
    s_test_factory_partition_data = data;
    s_test_factory_partition_bytes = NULL;
    s_test_factory_partition_len = 0;
}

void michi_factory_cfg_set_test_partition_bytes(const uint8_t *data, size_t len)
{
    s_test_factory_partition_bytes = data;
    s_test_factory_partition_len = len;
    s_test_factory_partition_data = NULL;
}
#endif

#define TAG "michi_factory_cfg"

/* IEEE 802.3 CRC32 implementation */
static uint32_t compute_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (-(uint32_t)(crc & 1)));
        }
    }
    return crc ^ 0xFFFFFFFF;
}

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

static esp_err_t parse_inner_payload(const char *payload, size_t len, michi_factory_cfg_t *out_cfg)
{
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

        /* Check for device private identity seed */
        const cJSON *seed_item = cJSON_GetObjectItem(json, "device_seed");
        if (seed_item == NULL) {
            seed_item = cJSON_GetObjectItem(json, "device_private_key");
        }
        if (seed_item == NULL) {
            seed_item = cJSON_GetObjectItem(json, "device_identity");
        }
        if (seed_item != NULL && cJSON_IsString(seed_item) && seed_item->valuestring != NULL) {
            size_t dec_len = 0;
            if (michi_identity_base64url_decode(seed_item->valuestring, out_cfg->device_seed, sizeof(out_cfg->device_seed), &dec_len) == ESP_OK &&
                dec_len == MICHI_IDENTITY_KEY_BYTES) {
                out_cfg->has_device_seed = true;
            }
        }

        const cJSON *mem_item = cJSON_GetObjectItem(json, "device_membership");
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
                } else if (str_case_eq(key, "device_seed") || str_case_eq(key, "device_private_key")) {
                    size_t dec_len = 0;
                    if (michi_identity_base64url_decode(val, out_cfg->device_seed, sizeof(out_cfg->device_seed), &dec_len) == ESP_OK &&
                        dec_len == MICHI_IDENTITY_KEY_BYTES) {
                        out_cfg->has_device_seed = true;
                    }
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

esp_err_t michi_factory_cfg_parse(const char *payload, size_t len, michi_factory_cfg_t *out_cfg)
{
    if (payload == NULL || len == 0 || out_cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_cfg, 0, sizeof(*out_cfg));

    /* Attempt 1: Byte-exact binary MICHI-F1 container */
    if (len >= sizeof(michi_f1_header_t) + 4 &&
        memcmp(payload, MICHI_F1_MAGIC, MICHI_F1_MAGIC_LEN) == 0 &&
        payload[8] != '\n' && payload[8] != '\r' && payload[8] != '=') {

        const michi_f1_header_t *hdr = (const michi_f1_header_t *)payload;
        if (hdr->version != MICHI_F1_VERSION) {
            ESP_LOGE(TAG, "factory_cfg: unsupported version %u", hdr->version);
            return ESP_ERR_NOT_SUPPORTED;
        }
        if (hdr->nonce_len < MICHI_F1_NONCE_MIN_LEN || hdr->nonce_len > MICHI_F1_NONCE_MAX_LEN) {
            ESP_LOGE(TAG, "factory_cfg: invalid nonce length %u", hdr->nonce_len);
            return ESP_ERR_INVALID_SIZE;
        }
        if (hdr->payload_len == 0 || sizeof(michi_f1_header_t) + hdr->payload_len + 4 > len) {
            ESP_LOGE(TAG, "factory_cfg: invalid payload length %" PRIu32 " (container len %zu)", hdr->payload_len, len);
            return ESP_ERR_INVALID_SIZE;
        }

        uint32_t stored_crc = 0;
        memcpy(&stored_crc, payload + sizeof(michi_f1_header_t) + hdr->payload_len, 4);
        uint32_t calc_crc = compute_crc32((const uint8_t *)payload, sizeof(michi_f1_header_t) + hdr->payload_len);
        if (stored_crc != calc_crc) {
            ESP_LOGE(TAG, "factory_cfg: CRC32 mismatch (stored=0x%08" PRIx32 ", calc=0x%08" PRIx32 ")",
                     stored_crc, calc_crc);
            return ESP_ERR_INVALID_CRC;
        }

        const char *inner_payload = payload + sizeof(michi_f1_header_t);
        size_t inner_len = hdr->payload_len;
        return parse_inner_payload(inner_payload, inner_len, out_cfg);
    }

    /* Fallback: Legacy line-delimited or plain JSON */
    return parse_inner_payload(payload, len, out_cfg);
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

    /* Verify root_public_key derives home_id: blake3(root_pk) == home_id */
    char derived_home_id[MICHI_HOME_ID_LEN];
    if (michi_identity_derive_michi_id(root_pk, derived_home_id, sizeof(derived_home_id)) != ESP_OK ||
        strcmp(derived_home_id, cfg.home_id) != 0) {
        ESP_LOGE(TAG, "factory_cfg: home_id does not derive from root_public_key");
        return ESP_ERR_INVALID_ARG;
    }

    /* Pre-verify device membership and identity coherence before modifying durable state */
    if (cfg.has_device_membership) {
        if (strcmp(cfg.device_membership.home_id, cfg.home_id) != 0) {
            ESP_LOGE(TAG, "factory_cfg: device membership home_id mismatch");
            return ESP_ERR_INVALID_ARG;
        }

        if (!michi_home_verify_membership(&cfg.device_membership, root_pk)) {
            ESP_LOGE(TAG, "factory_cfg: device membership signature verification failed");
            return ESP_ERR_INVALID_ARG;
        }

        if (cfg.has_device_seed) {
            /* If factory configuration provides device private identity seed,
             * verify it derives the exact device_michi_id and device_public_key in membership */
            uint8_t dev_pk[MICHI_IDENTITY_KEY_BYTES];
            if (michi_identity_derive_public_key(cfg.device_seed, dev_pk) != ESP_OK) {
                return ESP_FAIL;
            }

            char seed_michi_id[MICHI_IDENTITY_MICHI_ID_LEN];
            char seed_pk_b64[MICHI_IDENTITY_PUBLIC_KEY_B64_LEN];
            if (michi_identity_derive_michi_id(dev_pk, seed_michi_id, sizeof(seed_michi_id)) != ESP_OK ||
                michi_identity_base64url_encode(dev_pk, sizeof(dev_pk), seed_pk_b64, sizeof(seed_pk_b64)) != ESP_OK) {
                return ESP_FAIL;
            }
            if (strcmp(seed_michi_id, cfg.device_membership.device_michi_id) != 0 ||
                strcmp(seed_pk_b64, cfg.device_membership.device_public_key) != 0) {
                ESP_LOGE(TAG, "factory_cfg: device seed does not match membership (%s vs %s)",
                         seed_michi_id, cfg.device_membership.device_michi_id);
                return ESP_ERR_INVALID_ARG;
            }
        } else if (michi_identity_get_state() == MICHI_IDENTITY_READY) {
            char local_michi_id[MICHI_IDENTITY_MICHI_ID_LEN] = {0};
            if (michi_identity_michi_id(local_michi_id, sizeof(local_michi_id)) == ESP_OK) {
                if (strcmp(local_michi_id, cfg.device_membership.device_michi_id) != 0) {
                    ESP_LOGE(TAG, "factory_cfg: device membership does not match local device identity (%s vs %s)",
                             cfg.device_membership.device_michi_id, local_michi_id);
                    return ESP_ERR_INVALID_ARG;
                }
            }
        }
    }

    /* Atomic commit with rollback */
    if (cfg.has_device_seed) {
        err = michi_identity_import_seed(cfg.device_seed);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "factory_cfg: failed to import device seed: %s", esp_err_to_name(err));
            return err;
        }
        ESP_LOGI(TAG, "factory_cfg: device private identity imported successfully");
    }

    err = michi_home_set_credentials(cfg.home_id, root_pk);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "factory_cfg: failed to store home credentials: %s", esp_err_to_name(err));
        if (cfg.has_device_seed) (void)michi_identity_factory_reset();
        return err;
    }

    if (cfg.has_device_membership) {
        err = michi_home_set_device_membership(&cfg.device_membership);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "factory_cfg: failed to store device membership: %s (rolling back)", esp_err_to_name(err));
            (void)michi_home_erase();
            if (cfg.has_device_seed) (void)michi_identity_factory_reset();
            return err;
        }
        ESP_LOGI(TAG, "factory_cfg: device membership stored and verified");
    }

    /* If Wi-Fi credentials provided, save to NVS "wifi" */
    if (cfg.wifi_ssid[0] != '\0') {
        nvs_handle_t wh;
        err = nvs_open("wifi", NVS_READWRITE, &wh);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "factory_cfg: failed to open wifi nvs: %s (rolling back)", esp_err_to_name(err));
            (void)michi_home_erase();
            if (cfg.has_device_seed) (void)michi_identity_factory_reset();
            return err;
        }
        err = nvs_set_str(wh, "ssid", cfg.wifi_ssid);
        if (err == ESP_OK) {
            err = nvs_set_str(wh, "password", cfg.wifi_password);
        }
        if (err == ESP_OK) {
            err = nvs_commit(wh);
        }
        nvs_close(wh);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "factory_cfg: failed to write wifi credentials: %s (rolling back)", esp_err_to_name(err));
            (void)michi_home_erase();
            if (cfg.has_device_seed) (void)michi_identity_factory_reset();
            return err;
        }
        ESP_LOGI(TAG, "factory_cfg: Wi-Fi credentials imported (SSID: %s)", cfg.wifi_ssid);
    }

    /* Verification: confirm home_id is readable and valid */
    char verify_id[MICHI_HOME_ID_LEN] = {0};
    if (michi_home_get_id(verify_id, sizeof(verify_id)) != ESP_OK ||
        strcmp(verify_id, cfg.home_id) != 0) {
        ESP_LOGE(TAG, "factory_cfg: verification of committed credentials failed (rolling back)");
        (void)michi_home_erase();
        if (cfg.has_device_seed) (void)michi_identity_factory_reset();
        return ESP_FAIL;
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
    const char *buf = NULL;
    size_t to_read = 0;
    if (s_test_factory_partition_bytes != NULL && s_test_factory_partition_len > 0) {
        buf = (const char *)s_test_factory_partition_bytes;
        to_read = s_test_factory_partition_len;
    } else if (s_test_factory_partition_data != NULL) {
        buf = s_test_factory_partition_data;
        to_read = strlen(s_test_factory_partition_data);
    } else {
        return ESP_OK;
    }

    if ((uint8_t)buf[0] == 0xFF || buf[0] == '\0') {
        ESP_LOGD(TAG, "factory_cfg: partition empty or unprogrammed");
        return ESP_OK;
    }

    size_t content_len = 0;
    if (to_read >= sizeof(michi_f1_header_t) + 4 && memcmp(buf, MICHI_F1_MAGIC, MICHI_F1_MAGIC_LEN) == 0) {
        const michi_f1_header_t *hdr = (const michi_f1_header_t *)buf;
        size_t total_len = sizeof(michi_f1_header_t) + hdr->payload_len + 4;
        if (total_len > to_read) {
            ESP_LOGE(TAG, "factory_cfg: MICHI-F1 container length %zu exceeds available %zu", total_len, to_read);
            return ESP_ERR_INVALID_SIZE;
        }
        content_len = total_len;
    } else {
        while (content_len < to_read && (uint8_t)buf[content_len] != 0xFF && buf[content_len] != '\0') {
            content_len++;
        }
    }
    if (content_len == 0) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "factory_cfg: found config in test partition (%zu bytes), importing...", content_len);
    esp_err_t res = michi_home_import_factory_cfg(buf, content_len);
    if (res == ESP_OK) {
        s_test_factory_partition_bytes = NULL;
        s_test_factory_partition_len = 0;
        s_test_factory_partition_data = NULL;
    }
    return res;
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
    if (to_read >= sizeof(michi_f1_header_t) + 4 && memcmp(buf, MICHI_F1_MAGIC, MICHI_F1_MAGIC_LEN) == 0) {
        const michi_f1_header_t *hdr = (const michi_f1_header_t *)buf;
        size_t total_len = sizeof(michi_f1_header_t) + hdr->payload_len + 4;
        if (total_len > to_read || total_len > part->size) {
            ESP_LOGE(TAG, "factory_cfg: MICHI-F1 container length %zu exceeds available %zu", total_len, to_read);
            return ESP_ERR_INVALID_SIZE;
        }
        content_len = total_len;
    } else {
        while (content_len < to_read && (uint8_t)buf[content_len] != 0xFF && buf[content_len] != '\0') {
            content_len++;
        }
        if (content_len == 0) {
            return ESP_OK;
        }
        buf[content_len] = '\0';
    }

    ESP_LOGI(TAG, "factory_cfg: found config in partition (%zu bytes), importing...", content_len);
    err = michi_home_import_factory_cfg(buf, content_len);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "factory_cfg: one-shot import completed successfully, erasing partition");
        /* Securely erase partition to avoid retaining credentials */
        esp_err_t erase_err = esp_partition_erase_range(part, 0, part->size);
        if (erase_err != ESP_OK) {
            ESP_LOGE(TAG, "factory_cfg: critical error, erase failed: %s (initiating rollback to prevent credential retention)", esp_err_to_name(erase_err));
            (void)michi_home_erase();
            return erase_err;
        }
    } else {
        ESP_LOGW(TAG, "factory_cfg: one-shot import failed: %s (retaining partition for retry)", esp_err_to_name(err));
    }
    return err;
#endif
}
