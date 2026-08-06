/*
 * HTTP API layer (phase 4). Read-only migrated endpoints only:
 * GET /api/v1/receiver/info and GET /api/v1/receiver/firmware, no auth
 * (same as the legacy prototype). Pairing (phase 10) and session/volume
 * (phase 12) handlers follow the handler contract documented in
 * michi_http.h: copy ALL values out of the cJSON tree BEFORE delete,
 * never return pointers into the tree.
 *
 * /info is built from the dynamic product profile (michi_product_profile),
 * the single source of truth - no product string is duplicated here.
 */

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "michi_http.h"
#include "michi_product_profile.h"

#define TAG "michi_http"

#define MICHI_HTTP_PORT 80
#define MICHI_HTTP_BODY_MAX 2048    /* body limit for phases 10/12 bodies */
#define MICHI_HTTP_RECV_TIMEOUT_RETRIES 1  /* single timeout retry */
#define MICHI_HTTP_BODY_TOTAL_TIMEOUT_MS 2000 /* anti-slowloris: total body deadline */

static httpd_handle_t s_server = NULL;

/* ------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------ */

static void status_to_str(int status, const char **out)
{
    switch (status) {
    case 200: *out = "200 OK"; break;
    case 400: *out = "400 Bad Request"; break;
    case 401: *out = "401 Unauthorized"; break;
    case 404: *out = "404 Not Found"; break;
    case 409: *out = "409 Conflict"; break;
    case 500: *out = "500 Internal Server Error"; break;
    default:  *out = "500 Internal Server Error"; break;
    }
}

esp_err_t michi_http_send_json(httpd_req_t *req, int status, const cJSON *root)
{
    if (req == NULL || root == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    char *json = cJSON_PrintUnformatted(root);
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }
    const char *status_str = NULL;
    status_to_str(status, &status_str);
    httpd_resp_set_status(req, status_str);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, json);
    free(json);
    return err;
}

esp_err_t michi_http_send_error(httpd_req_t *req, int status,
                                const char *code, const char *message)
{
    if (req == NULL || code == NULL || message == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON *e = cJSON_AddObjectToObject(root, "error");
    if (e == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    /* F8: never emit an incomplete error envelope with ESP_OK. */
    if (cJSON_AddStringToObject(e, "code", code) == NULL ||
        cJSON_AddStringToObject(e, "message", message) == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    if (cJSON_AddObjectToObject(e, "details") == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = michi_http_send_json(req, status, root);
    cJSON_Delete(root);
    /* P0-5 (shared with audio_output: error propagation): return the
     * SEND result (ESP_OK once responded), never ESP_FAIL after a
     * response was already sent. */
    return err;
}

esp_err_t michi_http_read_body(httpd_req_t *req, char *buf, size_t buf_len,
                               size_t *out_len)
{
    if (req == NULL || buf == NULL || out_len == NULL || buf_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    char clen_str[16] = {0};
    if (httpd_req_get_hdr_value_str(req, "Content-Length", clen_str,
                                    sizeof(clen_str)) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    /* Strict parse: no trailing junk, no negatives. A malformed header is
     * a client error - the caller MUST answer 400 Bad Request. */
    char *endp = NULL;
    long content_len = strtol(clen_str, &endp, 10);
    if (endp == clen_str || *endp != '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    if (content_len < 0 || (size_t)content_len >= buf_len) {
        /* The caller's buffer size IS the limit: a body that does not fit
         * (or a missing NUL byte) is rejected, never truncated. */
        return ESP_ERR_INVALID_SIZE;
    }
    size_t received = 0;
    int timeouts = 0;
    /* Anti-slowloris contract: the whole body must arrive within
     * MICHI_HTTP_BODY_TOTAL_TIMEOUT_MS of wall time (checked before every
     * recv) AND a socket timeout is retried at most once - a client that
     * trickles bytes cannot hold the httpd task indefinitely. */
    const int64_t deadline_us = esp_timer_get_time() +
                                MICHI_HTTP_BODY_TOTAL_TIMEOUT_MS * 1000LL;
    while (received < (size_t)content_len) {
        if (esp_timer_get_time() >= deadline_us) {
            return ESP_ERR_TIMEOUT;
        }
        int ret = httpd_req_recv(req, buf + received,
                                 (size_t)content_len - received);
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            /* Bounded retries: a stalled client cannot block the httpd
             * task forever. */
            if (++timeouts > MICHI_HTTP_RECV_TIMEOUT_RETRIES) {
                return ESP_ERR_TIMEOUT;
            }
            continue;
        }
        if (ret <= 0) {
            return ESP_ERR_INVALID_STATE;
        }
        received += (size_t)ret;
    }
    buf[received] = '\0';
    *out_len = received;
    return ESP_OK;
}

bool michi_http_json_get_string(const cJSON *obj, const char *key,
                                char *out, size_t out_len)
{
    if (obj == NULL || key == NULL || out == NULL || out_len == 0) {
        return false;
    }
    const cJSON *item = cJSON_GetObjectItem(obj, key);
    if (item == NULL || !cJSON_IsString(item) ||
        item->valuestring == NULL) {
        return false;
    }
    size_t len = strlen(item->valuestring);
    if (len >= out_len) {
        return false; /* value does not fit: fail, do not truncate */
    }
    memcpy(out, item->valuestring, len + 1);
    return true;
}

bool michi_http_json_get_int(const cJSON *obj, const char *key, int *out)
{
    if (obj == NULL || key == NULL || out == NULL) {
        return false;
    }
    const cJSON *item = cJSON_GetObjectItem(obj, key);
    if (item == NULL || !cJSON_IsNumber(item)) {
        return false; /* exact type: no strings, no bools, no coercion */
    }
    /* Exact type PLUS range: fractional or out-of-int-range numbers fail
     * (never truncated); the (int) cast is safe after the range checks. */
    const double d = item->valuedouble;
    if (d < (double)INT_MIN || d > (double)INT_MAX ||
        d != (double)(int)d) {
        return false;
    }
    *out = item->valueint;
    return true;
}

bool michi_http_json_get_bool(const cJSON *obj, const char *key, bool *out)
{
    if (obj == NULL || key == NULL || out == NULL) {
        return false;
    }
    const cJSON *item = cJSON_GetObjectItem(obj, key);
    if (item == NULL || !cJSON_IsBool(item)) {
        return false;
    }
    *out = cJSON_IsTrue(item);
    return true;
}

/* ------------------------------------------------------------------
 * Endpoints (GET only, no auth - same surface as the legacy prototype)
 * ------------------------------------------------------------------ */

static esp_err_t build_info_json(cJSON *root, const michi_product_profile_t *p)
{
    if (cJSON_AddStringToObject(root, "service", "michi-link") == NULL ||
        cJSON_AddStringToObject(root, "name", p->product_name) == NULL ||
        cJSON_AddStringToObject(root, "device_id", CONFIG_MICHI_DEVICE_ID) == NULL ||
        cJSON_AddStringToObject(root, "type", michi_product_profile_tier_name()) == NULL) {
        return ESP_ERR_NO_MEM;
    }
    /* v1-lite kept for compatibility with the legacy API surface;
     * phase 12 aligns it with the michi-link contract. */
    if (cJSON_AddStringToObject(root, "api_version", "v1-lite") == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON *fw = cJSON_AddObjectToObject(root, "firmware");
    if (fw == NULL ||
        cJSON_AddStringToObject(fw, "version", p->firmware_version) == NULL ||
        cJSON_AddStringToObject(fw, "build_date", p->build_date) == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON *out = cJSON_AddObjectToObject(root, "output");
    if (out == NULL ||
        cJSON_AddStringToObject(out, "connector", p->output_connector) == NULL ||
        (p->dac_model[0] != '\0' &&
         cJSON_AddStringToObject(out, "dac", p->dac_model) == NULL) ||
        cJSON_AddNumberToObject(out, "max_sample_rate", p->max_sample_rate) == NULL ||
        cJSON_AddNumberToObject(out, "max_bit_depth", p->max_bit_depth) == NULL ||
        cJSON_AddNumberToObject(out, "channels", p->channels) == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON *cc = cJSON_AddArrayToObject(root, "supported_codecs");
    if (cc == NULL) {
        return ESP_ERR_NO_MEM;
    }
    for (uint8_t i = 0; i < p->supported_codecs_count; i++) {
        cJSON *codec = cJSON_CreateString(p->supported_codecs[i]);
        if (codec == NULL) {
            return ESP_ERR_NO_MEM;
        }
        /* cJSON_AddItemToArray returns cJSON_bool: check the item first. */
        if (!cJSON_AddItemToArray(cc, codec)) {
            cJSON_Delete(codec);
            return ESP_ERR_NO_MEM;
        }
    }
    /* Features: capabilities of the v1-lite service contract. Pairing,
     * heartbeat and volume handlers land in phases 10/12. */
    cJSON *feat = cJSON_AddObjectToObject(root, "features");
    if (feat == NULL ||
        cJSON_AddBoolToObject(feat, "pairing_button", true) == NULL ||
        cJSON_AddBoolToObject(feat, "volume", true) == NULL ||
        cJSON_AddBoolToObject(feat, "heartbeat", true) == NULL ||
        cJSON_AddBoolToObject(feat, "ota_update", p->ota_supported) == NULL) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t info_get_handler(httpd_req_t *req)
{
    const michi_product_profile_t *p = michi_product_profile_get();
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = build_info_json(root, p);
    if (err == ESP_OK) {
        err = michi_http_send_json(req, 200, root);
    }
    cJSON_Delete(root);
    return err;
}

static esp_err_t firmware_get_handler(httpd_req_t *req)
{
    const michi_product_profile_t *p = michi_product_profile_get();
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = ESP_OK;
    if (cJSON_AddStringToObject(root, "device_id", CONFIG_MICHI_DEVICE_ID) == NULL ||
        cJSON_AddStringToObject(root, "current_version", p->firmware_version) == NULL ||
        cJSON_AddStringToObject(root, "build_date", p->build_date) == NULL ||
        cJSON_AddBoolToObject(root, "ota_supported", p->ota_supported) == NULL) {
        err = ESP_ERR_NO_MEM;
    } else {
        err = michi_http_send_json(req, 200, root);
    }
    cJSON_Delete(root);
    return err;
}

static const httpd_uri_t s_endpoints[] = {
    {.uri = "/api/v1/receiver/info",     .method = HTTP_GET, .handler = info_get_handler},
    {.uri = "/api/v1/receiver/firmware", .method = HTTP_GET, .handler = firmware_get_handler},
};

/* ------------------------------------------------------------------
 * Server lifecycle
 * ------------------------------------------------------------------ */

esp_err_t michi_http_init(void)
{
    if (s_server != NULL) {
        return ESP_OK; /* idempotent */
    }
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = MICHI_HTTP_PORT;
    cfg.lru_purge_enable = true;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }
    for (size_t i = 0; i < sizeof(s_endpoints) / sizeof(s_endpoints[0]); i++) {
        err = httpd_register_uri_handler(server, &s_endpoints[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "httpd_register_uri_handler('%s') failed: %s",
                     s_endpoints[i].uri, esp_err_to_name(err));
            httpd_stop(server);
            return err;
        }
    }
    s_server = server;
    ESP_LOGI(TAG, "http server listening on port %d (%u endpoints)",
             MICHI_HTTP_PORT, (unsigned)(sizeof(s_endpoints) / sizeof(s_endpoints[0])));
    return ESP_OK;
}

esp_err_t michi_http_stop(void)
{
    if (s_server == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = httpd_stop(s_server);
    s_server = NULL;
    return err;
}
