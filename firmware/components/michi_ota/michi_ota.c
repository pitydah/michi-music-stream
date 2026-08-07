/*
 * Signed OTA updates (phase 13): manifest + signature verification +
 * streaming download + A/B swap with boot-time rollback.
 *
 * Design (see include/michi_ota.h for the full contract):
 *  - michi_ota_start() validates the URL synchronously (fast, no network)
 *    and spawns the OTA task; everything else runs in that task. The
 *    state/percent/error snapshot is mutex-protected for any-task reads
 *    (the HTTP diagnostics handler polls it).
 *  - The manifest is the trust anchor: the binary URL and its SHA-256
 *    live INSIDE the signed JSON, so a tampered binary is rejected before
 *    esp_ota_end - nothing is ever marked bootable that was not signed.
 *  - TLS: https:// is enforced at URL validation (scheme, no userinfo,
 *    non-empty host) AND at download (the signed binary URL is re-
 *    validated the same way); the CA bundle is always attached, the CN
 *    check is never skipped.
 *  - The FSM is requested to MICHI_STATE_UPDATING via michi_state_request
 *    (IDLE/PLAYING/PAUSED are in the transition table); an active session
 *    is force-closed with michi_session_abort() BEFORE the request so the
 *    SESSION_CLOSED event lands first and the UPDATING request maps.
 *    UPDATE_STARTED/FAILED/DONE are posted for observers (broadcast-only).
 *  - Boot-time rollback: michi_ota_boot_selftest_done() checks the
 *    RUNNING partition state with esp_ota_get_state_partition() (note:
 *    esp_ota_get_state_partition_name() does NOT exist in IDF 5.3 - the
 *    name/state pair is resolved by the caller) and marks valid or
 *    restarts to let the bootloader roll back.
 *  - Logs are key=value; URLs are logged as host + path-length only
 *    (query strings may carry tokens and are never logged).
 */

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"

#include "cJSON.h"

#include "mbedtls/base64.h"
#include "mbedtls/md.h"
#include "mbedtls/pk.h"

#include "michi_led.h"
#include "michi_ota.h"
#include "michi_ota_pubkey.h"
#include "michi_product_profile.h"
#include "michi_session.h"
#include "michi_state.h"

#define TAG "michi_ota"

/* Task priority: below the FSM (5) and the display render (4) - the OTA
 * task blocks on network reads for seconds and must never delay the event
 * bus or the screens; above app_main (1). */
#define MICHI_OTA_TASK_PRIORITY 3
#define MICHI_OTA_TASK_NAME "michi_ota"

/* Streaming chunk size: aligned to the partition erase sector (4 KB). */
#define MICHI_OTA_CHUNK_BYTES 4096

/* Progress anchors (documented in the header): manifest 5, validating 10,
 * download 10..85, verifying 90, applying 95, done 100. */
#define MICHI_OTA_PCT_MANIFEST 5
#define MICHI_OTA_PCT_VALIDATING 10
#define MICHI_OTA_PCT_DOWNLOAD_MIN 10
#define MICHI_OTA_PCT_DOWNLOAD_MAX 85
#define MICHI_OTA_PCT_VERIFYING 90
#define MICHI_OTA_PCT_APPLYING 95
#define MICHI_OTA_PCT_DONE 100

/* Manifest field bounds (mirror the API contract; the signature payload
 * uses these exact strings). */
#define MICHI_OTA_FIELD_VERSION_MAX 32
#define MICHI_OTA_FIELD_BOARD_MAX 64
#define MICHI_OTA_FIELD_MIN_VERSION_MAX 32
#define MICHI_OTA_SHA256_HEX_LEN 64
#define MICHI_OTA_SIGNATURE_B64_MAX 1024
#define MICHI_OTA_SIG_BYTES_MAX 512
/* MICHI_OTA_ERR_MAX comes from michi_ota.h (single source of truth). */

/* RSA-2048 signature length (PKCS#1 v1.5, verified with mbedtls_pk_verify
 * against the embedded DER public key). */
#define MICHI_OTA_EXPECTED_SIG_BYTES 256

typedef struct {
    char version[MICHI_OTA_FIELD_VERSION_MAX];
    char board[MICHI_OTA_FIELD_BOARD_MAX];
    char min_version[MICHI_OTA_FIELD_MIN_VERSION_MAX];
    char url[CONFIG_MICHI_OTA_URL_MAX + 1];
    char sha256[MICHI_OTA_SHA256_HEX_LEN + 1];
    char signature[MICHI_OTA_SIGNATURE_B64_MAX];
} michi_ota_manifest_t;

typedef struct {
    SemaphoreHandle_t mutex;
    volatile michi_ota_state_t state;
    volatile int percent;
    char err[MICHI_OTA_ERR_MAX];
    TaskHandle_t task;
} michi_ota_ctx_t;

static michi_ota_ctx_t s_ctx;

/* ------------------------------------------------------------------
 * State snapshot (any-task reads; the task owns the writes)
 * ------------------------------------------------------------------ */

static void set_state(michi_ota_state_t state, int percent)
{
    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
    s_ctx.state = state;
    s_ctx.percent = percent;
    xSemaphoreGive(s_ctx.mutex);
}

static void set_failed(esp_err_t err, const char *detail)
{
    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
    /* Terminal state: FAILED implies not busy. The task clears its own
     * handle here (under the same lock that guards get_state/busy/start),
     * so the strict invariant 'MICHI_OTA_FAILED is not busy' holds
     * atomically - no window where FAILED is visible while busy(). */
    s_ctx.task = NULL;
    s_ctx.state = MICHI_OTA_FAILED;
    s_ctx.percent = 0;
    snprintf(s_ctx.err, sizeof(s_ctx.err), "%s%s%s",
             esp_err_to_name(err), detail != NULL ? ": " : "",
             detail != NULL ? detail : "");
    xSemaphoreGive(s_ctx.mutex);
    ESP_LOGE(TAG, "ota: state=failed err=%s detail=%s",
             esp_err_to_name(err), detail != NULL ? detail : "-");
}

/* ------------------------------------------------------------------
 * URL validation (rule 1: https:// only, no userinfo, host non-empty)
 * ------------------------------------------------------------------ */

static bool url_valid_https(const char *url)
{
    if (url == NULL || url[0] == '\0') {
        return false;
    }
    if (strncmp(url, "https://", 8) != 0) {
        char scheme[16];
        const size_t n = strlen(url) < sizeof(scheme) - 1
                             ? strlen(url) : sizeof(scheme) - 1;
        memcpy(scheme, url, n);
        scheme[n] = '\0';
        ESP_LOGW(TAG, "ota: url_scheme_rejected scheme=%s", scheme);
        return false;
    }
    const char *host_start = url + 8;
    const char *host_end = strchr(host_start, '/');
    const size_t host_len =
        host_end != NULL ? (size_t)(host_end - host_start) : strlen(host_start);
    if (host_len == 0) {
        ESP_LOGW(TAG, "ota: url_rejected reason=empty_host");
        return false;
    }
    /* userinfo: an '@' before the first '/' after the scheme is
     * credentials in the URL - rejected (never sent, never logged). */
    if (memchr(host_start, '@', host_len) != NULL) {
        ESP_LOGW(TAG, "ota: url_rejected reason=userinfo");
        return false;
    }
    return true;
}

/* Host for logs: the URL is not a secret, but query strings may carry
 * tokens - logs carry scheme://host and the path length only. */
static void log_url_safe(const char *url, const char *what)
{
    char host[64];
    const char *p = strchr(url + 8, '/');
    const size_t host_len = p != NULL ? (size_t)(p - (url + 8)) : strlen(url + 8);
    const size_t cap = host_len < sizeof(host) - 1 ? host_len : sizeof(host) - 1;
    memcpy(host, url + 8, cap);
    host[cap] = '\0';
    const size_t path_len = strlen(url) - (8 + host_len);
    ESP_LOGI(TAG, "ota: %s host=%s path_len=%zu", what, host, path_len);
}

/* ------------------------------------------------------------------
 * Strict semver (x.y.z numeric only; used for downgrade prevention)
 * ------------------------------------------------------------------ */

/* Strict semver (x.y.z numeric only; used for downgrade prevention).
 * Mirrors the signer's validation: every component is digit-only (no
 * leading whitespace, no sign, no leading zeros - '01.2.3' is rejected)
 * and fits in uint16_t. */
static bool semver_parse(const char *s, uint16_t out[3])
{
    uint16_t v[3] = {0};
    for (int i = 0; i < 3; i++) {
        const char *p = s;
        if (*p == '\0' || !isdigit((unsigned char)*p)) {
            return false;
        }
        if (*p == '0' && isdigit((unsigned char)p[1])) {
            return false; /* leading zero: '01' rejected */
        }
        unsigned long part = 0;
        while (isdigit((unsigned char)*p)) {
            part = part * 10 + (unsigned long)(*p - '0');
            if (part > 65535) {
                return false;
            }
            p++;
        }
        v[i] = (uint16_t)part;
        if (i < 2) {
            if (*p != '.') {
                return false;
            }
            s = p + 1;
        } else if (*p != '\0') {
            return false;
        }
    }
    memcpy(out, v, sizeof(v));
    return true;
}

static int semver_cmp(const uint16_t a[3], const uint16_t b[3])
{
    for (int i = 0; i < 3; i++) {
        if (a[i] != b[i]) {
            return a[i] < b[i] ? -1 : 1;
        }
    }
    return 0;
}

/* ESP-IDF 5.3 has no esp_ota_img_state_name() helper (verified: only
 * esp_ota_get_state_partition in app_update); the name mapping is local. */
static const char *image_state_name(esp_ota_img_states_t st)
{
    switch (st) {
    case ESP_OTA_IMG_NEW:           return "NEW";
    case ESP_OTA_IMG_PENDING_VERIFY: return "PENDING_VERIFY";
    case ESP_OTA_IMG_VALID:         return "VALID";
    case ESP_OTA_IMG_INVALID:       return "INVALID";
    case ESP_OTA_IMG_ABORTED:       return "ABORTED";
    case ESP_OTA_IMG_UNDEFINED:     return "UNDEFINED";
    default:                        return "UNKNOWN";
    }
}

/* ------------------------------------------------------------------
 * Manifest fetch (HTTP GET with CA-verified TLS)
 * ------------------------------------------------------------------ */

static esp_err_t fetch_manifest(const char *url, char *buf, size_t buf_len,
                                size_t *out_len)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = CONFIG_MICHI_OTA_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        /* skip_cert_common_name_check stays false: the CN is validated. */
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        ESP_LOGE(TAG, "ota: manifest client_init failed");
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota: manifest open failed err=%s",
                 esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }
    esp_http_client_fetch_headers(client);
    const int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGW(TAG, "ota: manifest status=%d", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_RESPONSE;
    }
    size_t total = 0;
    while (total < buf_len - 1) {
        const int rd = esp_http_client_read(client, buf + total,
                                            (int)(buf_len - 1 - total));
        if (rd > 0) {
            total += (size_t)rd;
            continue;
        }
        if (rd == 0) {
            break; /* EOF */
        }
        ESP_LOGW(TAG, "ota: manifest read failed rd=%d", rd);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }
    /* Buffer full (total == buf_len - 1): probe one more byte so an exact
     * fit (up to buf_len - 1 content bytes = 2047 with the default 2048
     * bound, the NUL byte lives outside the limit) is accepted while
     * overflow is still detected instead of silently truncating. */
    if (total == buf_len - 1) {
        char probe;
        const int rd = esp_http_client_read(client, &probe, 1);
        if (rd > 0) {
            ESP_LOGW(TAG, "ota: manifest too_large limit=%u",
                     (unsigned)(buf_len - 1));
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_ERR_INVALID_SIZE;
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    buf[total] = '\0';
    *out_len = total;
    return ESP_OK;
}

/* ------------------------------------------------------------------
 * Manifest validation (rules 2): fields, semver, sha256 hex, signature
 * ------------------------------------------------------------------ */

static bool json_str_copy(const cJSON *root, const char *key, char *out,
                          size_t out_len)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        ESP_LOGW(TAG, "ota: manifest_field_missing key=%s", key);
        return false;
    }
    const size_t len = strlen(item->valuestring);
    if (len == 0 || len >= out_len) {
        ESP_LOGW(TAG, "ota: manifest_field_invalid key=%s len=%u",
                 key, (unsigned)len);
        return false;
    }
    memcpy(out, item->valuestring, len + 1);
    return true;
}

static bool sha256_hex_ok(const char *hex)
{
    if (strlen(hex) != MICHI_OTA_SHA256_HEX_LEN) {
        return false;
    }
    for (size_t i = 0; i < MICHI_OTA_SHA256_HEX_LEN; i++) {
        const char c = hex[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    return true;
}

/* Verify the RSA-2048 PKCS#1 v1.5 SHA-256 signature over
 * "version|board|min_version|url|sha256" with the embedded public key. */
static esp_err_t verify_signature(const michi_ota_manifest_t *m)
{
    char payload[CONFIG_MICHI_OTA_URL_MAX + 1 + MICHI_OTA_FIELD_VERSION_MAX +
                  MICHI_OTA_FIELD_BOARD_MAX + MICHI_OTA_FIELD_MIN_VERSION_MAX +
                  MICHI_OTA_SHA256_HEX_LEN + 4];
    const int plen = snprintf(payload, sizeof(payload), "%s|%s|%s|%s|%s",
                              m->version, m->board, m->min_version, m->url,
                              m->sha256);
    if (plen <= 0 || (size_t)plen >= sizeof(payload)) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t sig[MICHI_OTA_SIG_BYTES_MAX];
    size_t sig_len = 0;
    const int b64_err = mbedtls_base64_decode(sig, sizeof(sig), &sig_len,
                                              (const unsigned char *)m->signature,
                                              strlen(m->signature));
    if (b64_err != 0 || sig_len != MICHI_OTA_EXPECTED_SIG_BYTES) {
        ESP_LOGW(TAG, "ota: signature_base64_invalid err=%d len=%u",
                 b64_err, (unsigned)sig_len);
        return ESP_ERR_INVALID_ARG;
    }

    unsigned char hash[32];
    const int md_err = mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                                  (const unsigned char *)payload,
                                  (size_t)plen, hash);
    if (md_err != 0) {
        ESP_LOGE(TAG, "ota: sha256_failed err=%d", md_err);
        return ESP_FAIL;
    }

    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    const int parse_err = mbedtls_pk_parse_public_key(
        &pk, michi_ota_pubkey_der, michi_ota_pubkey_der_len);
    if (parse_err != 0) {
        ESP_LOGE(TAG, "ota: pubkey_parse_failed err=%d", parse_err);
        mbedtls_pk_free(&pk);
        return ESP_FAIL;
    }
    const int verify_err = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, hash,
                                             sizeof(hash), sig, sig_len);
    mbedtls_pk_free(&pk);
    if (verify_err != 0) {
        ESP_LOGW(TAG, "ota: signature_invalid err=%d", verify_err);
        return ESP_ERR_INVALID_CRC;
    }
    return ESP_OK;
}

static esp_err_t validate_manifest(michi_ota_manifest_t *m)
{
    const michi_product_profile_t *p = michi_product_profile_get();

    if (strcmp(m->board, p->board_model) != 0) {
        ESP_LOGW(TAG, "ota: state=validating board=%s expected=%s",
                 m->board, p->board_model);
        return ESP_ERR_NOT_FOUND;
    }

    uint16_t cur[3], ver[3], min_ver[3];
    if (!semver_parse(p->firmware_version, cur) ||
        !semver_parse(m->version, ver)) {
        ESP_LOGW(TAG, "ota: state=validating semver_invalid version=%s",
                 m->version);
        return ESP_ERR_INVALID_ARG;
    }
    if (semver_cmp(ver, cur) <= 0) {
        ESP_LOGW(TAG, "ota: state=validating downgrade_rejected "
                      "version=%s current=%s", m->version, p->firmware_version);
        return ESP_ERR_INVALID_VERSION;
    }
    if (!semver_parse(m->min_version, min_ver) ||
        semver_cmp(ver, min_ver) < 0) {
        ESP_LOGW(TAG, "ota: state=validating min_version_not_met "
                      "version=%s min_version=%s", m->version, m->min_version);
        return ESP_ERR_INVALID_VERSION;
    }
    if (!url_valid_https(m->url)) {
        ESP_LOGW(TAG, "ota: state=validating binary_url_rejected");
        return ESP_ERR_INVALID_ARG;
    }
    if (!sha256_hex_ok(m->sha256)) {
        ESP_LOGW(TAG, "ota: state=validating sha256_format_invalid");
        return ESP_ERR_INVALID_ARG;
    }

    /* Signature LAST: everything else was validated and the payload is
     * canonical by then (cheap reject before the expensive RSA op). */
    const esp_err_t sig_err = verify_signature(m);
    if (sig_err != ESP_OK) {
        return sig_err;
    }
    ESP_LOGI(TAG, "ota: state=validating board=%s version=%s sig=ok",
             m->board, m->version);
    return ESP_OK;
}

/* ------------------------------------------------------------------
 * Firmware download (rule 3): stream + runtime SHA-256 + partition write
 * ------------------------------------------------------------------ */

static esp_err_t download_binary(const char *url, const char *expect_hex)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = CONFIG_MICHI_OTA_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        ESP_LOGE(TAG, "ota: binary client_init failed");
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota: binary open failed err=%s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }
    esp_http_client_fetch_headers(client);
    const int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGW(TAG, "ota: binary status=%d", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_RESPONSE;
    }
    const int64_t content_len = esp_http_client_get_content_length(client);
    if (content_len > 0) {
        ESP_LOGI(TAG, "ota: state=downloading size=%lld", (long long)content_len);
    }
    /* Download anchor: ALWAYS 10 on entry (also when content_len == -1,
     * i.e. chunked transfer - then percent stays at 10 until the verify
     * step, because there is no total to compute progress against). */
    set_state(MICHI_OTA_DOWNLOADING, MICHI_OTA_PCT_DOWNLOAD_MIN);

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (part == NULL) {
        ESP_LOGE(TAG, "ota: no_update_partition");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NOT_FOUND;
    }
    if (content_len > 0 && (uint64_t)content_len > part->size) {
        ESP_LOGW(TAG, "ota: binary_too_large size=%lld partition=%u",
                 (long long)content_len, (unsigned)part->size);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_SIZE;
    }

    esp_ota_handle_t handle = 0;
    err = esp_ota_begin(part, OTA_SIZE_UNKNOWN, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota: esp_ota_begin failed err=%s", esp_err_to_name(err));
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return err;
    }

    mbedtls_md_context_t md_ctx;
    mbedtls_md_init(&md_ctx);
    err = ESP_ERR_NO_MEM;
    if (mbedtls_md_setup(&md_ctx,
                         mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0) == 0) {
        err = mbedtls_md_starts(&md_ctx);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota: sha256_init failed err=%d", (int)err);
        mbedtls_md_free(&md_ctx);
        esp_ota_abort(handle);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    /* The 4 KB streaming chunk is heap-allocated (stack budget): the task
     * stack must fit mbedTLS + HTTP frames, not the transfer buffers. */
    uint8_t *chunk = malloc(MICHI_OTA_CHUNK_BYTES);
    if (chunk == NULL) {
        ESP_LOGE(TAG, "ota: chunk_alloc failed");
        mbedtls_md_free(&md_ctx);
        esp_ota_abort(handle);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }
    uint64_t total = 0;
    int read_err = 0;
    err = ESP_OK;
    for (;;) {
        const int rd = esp_http_client_read(client, (char *)chunk,
                                            MICHI_OTA_CHUNK_BYTES);
        if (rd > 0) {
            total += (uint64_t)rd;
            if (content_len > 0 && total > (uint64_t)content_len) {
                ESP_LOGW(TAG, "ota: binary longer than content-length");
                err = ESP_ERR_INVALID_SIZE;
                break;
            }
            if (mbedtls_md_update(&md_ctx, chunk, (size_t)rd) != 0) {
                err = ESP_FAIL;
                break;
            }
            const esp_err_t w_err = esp_ota_write(handle, chunk, (size_t)rd);
            if (w_err != ESP_OK) {
                ESP_LOGE(TAG, "ota: esp_ota_write failed err=%s",
                         esp_err_to_name(w_err));
                err = w_err;
                break;
            }
            if (content_len > 0) {
                const int pct = MICHI_OTA_PCT_DOWNLOAD_MIN +
                    (int)(((uint64_t)(MICHI_OTA_PCT_DOWNLOAD_MAX -
                                      MICHI_OTA_PCT_DOWNLOAD_MIN) * total) /
                          (uint64_t)content_len);
                set_state(MICHI_OTA_DOWNLOADING,
                          pct > MICHI_OTA_PCT_DOWNLOAD_MAX
                              ? MICHI_OTA_PCT_DOWNLOAD_MAX : pct);
            }
            continue;
        }
        if (rd == 0) {
            break; /* EOF */
        }
        read_err = rd;
        ESP_LOGW(TAG, "ota: binary read failed rd=%d", rd);
        err = ESP_FAIL;
        break;
    }

    if (err != ESP_OK || read_err != 0) {
        mbedtls_md_free(&md_ctx);
        esp_ota_abort(handle);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        free(chunk);
        return err != ESP_OK ? err : ESP_FAIL;
    }

    /* Runtime digest vs the signed manifest (rule 3: verify before end).
     * The md context is freed AFTER the final digest: finish must run on
     * a live context. */
    set_state(MICHI_OTA_VERIFYING, MICHI_OTA_PCT_VERIFYING);
    unsigned char digest[32];
    const int fin_err = mbedtls_md_finish(&md_ctx, digest);
    mbedtls_md_free(&md_ctx);
    if (fin_err != 0) {
        esp_ota_abort(handle);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        free(chunk);
        return ESP_FAIL;
    }
    char hex[65];
    for (size_t i = 0; i < sizeof(digest); i++) {
        snprintf(hex + 2 * i, 3, "%02x", digest[i]);
    }
    hex[64] = '\0';
    if (strcmp(hex, expect_hex) != 0) {
        ESP_LOGE(TAG, "ota: state=verifying sha256_mismatch "
                      "manifest=%s computed=%.16s...", expect_hex, hex);
        esp_ota_abort(handle);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        free(chunk);
        return ESP_ERR_INVALID_CRC;
    }
    ESP_LOGI(TAG, "ota: state=verifying sha256=ok bytes=%llu",
             (unsigned long long)total);

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(chunk);

    /* Apply: finalize the partition image, then swap the boot target.
     * A failing esp_ota_end needs NO esp_ota_abort: the handle is
     * invalidated internally on error (no more writes are possible) and
     * esp_ota_set_boot_partition was never called, so the new image is
     * left unmarked - nothing bootable was ever set. */
    set_state(MICHI_OTA_APPLYING, MICHI_OTA_PCT_APPLYING);
    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota: esp_ota_end failed err=%s", esp_err_to_name(err));
        return err;
    }
    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota: esp_ota_set_boot_partition failed err=%s",
                 esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------
 * OTA task: the whole pipeline, one sequential flow
 * ------------------------------------------------------------------ */

static void ota_task(void *arg)
{
    const char *url = (const char *)arg;
    esp_err_t err = ESP_OK;
    /* The manifest buffer (up to CONFIG_MICHI_OTA_MANIFEST_MAX_BYTES) is
     * heap-allocated (stack budget, see the Kconfig help): the stack must
     * fit mbedTLS + HTTP frames, not the transfer buffers. Freed on every
     * exit path below (the esp_restart success path frees before booting
     * the new image). */
    char *manifest = malloc(CONFIG_MICHI_OTA_MANIFEST_MAX_BYTES);
    michi_ota_manifest_t m;

    if (manifest == NULL) {
        set_failed(ESP_ERR_NO_MEM, "manifest buffer");
        goto done;
    }
    memset(&m, 0, sizeof(m));
    log_url_safe(url, "state=fetching_manifest");

    set_state(MICHI_OTA_FETCHING_MANIFEST, MICHI_OTA_PCT_MANIFEST);
    size_t manifest_len = 0;
    err = fetch_manifest(url, manifest, CONFIG_MICHI_OTA_MANIFEST_MAX_BYTES,
                         &manifest_len);
    if (err != ESP_OK) {
        set_failed(err, "manifest fetch");
        goto done;
    }
    ESP_LOGI(TAG, "ota: manifest_bytes=%u", (unsigned)manifest_len);

    set_state(MICHI_OTA_VALIDATING, MICHI_OTA_PCT_VALIDATING);
    cJSON *root = cJSON_Parse(manifest);
    if (root == NULL || !cJSON_IsObject(root)) {
        if (root != NULL) {
            cJSON_Delete(root);
        }
        set_failed(ESP_ERR_INVALID_ARG, "manifest parse");
        goto done;
    }
    const bool have_all =
        json_str_copy(root, "version", m.version, sizeof(m.version)) &&
        json_str_copy(root, "board", m.board, sizeof(m.board)) &&
        json_str_copy(root, "min_version", m.min_version,
                      sizeof(m.min_version)) &&
        json_str_copy(root, "url", m.url, sizeof(m.url)) &&
        json_str_copy(root, "sha256", m.sha256, sizeof(m.sha256)) &&
        json_str_copy(root, "signature", m.signature, sizeof(m.signature));
    cJSON_Delete(root);
    if (!have_all) {
        set_failed(ESP_ERR_INVALID_ARG, "manifest fields");
        goto done;
    }

    err = validate_manifest(&m);
    if (err != ESP_OK) {
        set_failed(err, "manifest validation");
        goto done;
    }

    err = download_binary(m.url, m.sha256);
    if (err != ESP_OK) {
        set_failed(err, "firmware download");
        goto done;
    }

    /* Success: clean LED shutdown, mark DONE, restart. */
    michi_led_shutdown();
    set_state(MICHI_OTA_DONE, MICHI_OTA_PCT_DONE);
    ESP_LOGI(TAG, "ota: state=done version=%s booting_next", m.version);
    ESP_LOGI(TAG, "ota: stack_hwm=%u",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    free(manifest);
    michi_state_post(MICHI_EVENT_UPDATE_DONE, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
    for (;;) { /* esp_restart never returns; defensive */
        vTaskDelay(pdMS_TO_TICKS(60000));
    }

done:
    ESP_LOGI(TAG, "ota: stack_hwm=%u",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    free(manifest);
    michi_state_post(MICHI_EVENT_UPDATE_FAILED, (uint32_t)err);
    michi_state_request(MICHI_STATE_IDLE);
    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
    s_ctx.task = NULL; /* redundant with set_failed, kept as defense */
    xSemaphoreGive(s_ctx.mutex);
    free((void *)url);
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------ */

esp_err_t michi_ota_init(void)
{
    if (s_ctx.mutex == NULL) {
        s_ctx.mutex = xSemaphoreCreateMutex();
        if (s_ctx.mutex == NULL) {
            ESP_LOGE(TAG, "init: mutex creation failed");
            return ESP_ERR_NO_MEM;
        }
    }
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running != NULL) {
        esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
        if (esp_ota_get_state_partition(running, &st) != ESP_OK) {
            st = ESP_OTA_IMG_UNDEFINED;
        }
        ESP_LOGI(TAG, "ota: running=%s state=%s size=%u",
                 running->label, image_state_name(st), (unsigned)running->size);
    } else {
        ESP_LOGW(TAG, "ota: running_partition_unknown");
    }
    ESP_LOGI(TAG, "subsystem=ota state=ok phase=13");
    return ESP_OK;
}

esp_err_t michi_ota_start(const char *url)
{
    if (url == NULL || strlen(url) > CONFIG_MICHI_OTA_URL_MAX) {
        ESP_LOGW(TAG, "ota: start_rejected reason=url_length url_len=%u",
                 (unsigned)(url != NULL ? strlen(url) : 0));
        return ESP_ERR_INVALID_ARG;
    }
    if (!url_valid_https(url)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ctx.mutex == NULL) {
        return ESP_ERR_INVALID_STATE; /* init never succeeded */
    }

    /* Blocked sessions: an active session is force-closed BEFORE the gate
     * (the credential is never persisted, so OTA cannot present it -
     * michi_session_abort is the privileged internal path). The abort
     * result is authoritative: a live session that survived abort means
     * the update must NOT start (the FSM would keep the old session's
     * transition in flight). Side effects are at most once; the gate
     * below re-verifies task==NULL under the mutex, so a concurrent start
     * that already created a task rejects this one - no double task.
     * The state request + event post happen AFTER the gate passes: a
     * rejection (busy/pending_verify) must not leave the FSM on UPDATING
     * with no OTA task running. */
    if (michi_session_active()) {
        const esp_err_t abort_err = michi_session_abort("ota update");
        if (abort_err != ESP_OK) {
            ESP_LOGE(TAG, "ota: session_abort_failed err=%s "
                          "(update not started: a live session blocks OTA)",
                     esp_err_to_name(abort_err));
            return ESP_ERR_INVALID_STATE;
        }
    }

    char *url_copy = strdup(url);
    if (url_copy == NULL) {
        set_failed(ESP_ERR_NO_MEM, "url copy");
        michi_state_request(MICHI_STATE_IDLE);
        return ESP_ERR_NO_MEM;
    }

    /* Atomic gate: the busy check and the xTaskCreate run in ONE critical
     * section (the session abort and the strdup above are outside it, so
     * the check inside is the single point of truth for task==NULL). A
     * second concurrent start lands here, sees task != NULL and rejects
     * with ESP_ERR_INVALID_STATE - the task is never created twice. */
    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
    if (s_ctx.task != NULL) {
        xSemaphoreGive(s_ctx.mutex);
        free(url_copy);
        ESP_LOGW(TAG, "ota: start_rejected reason=busy");
        return ESP_ERR_INVALID_STATE;
    }
    /* First boot after an OTA: the running image is PENDING_VERIFY and
     * esp_ota_begin would reject the new write - the boot self-test must
     * mark the image valid first. Distinct error code on purpose: the API
     * answers 409 'pending_verify', NOT 'ota_in_progress'. */
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running != NULL) {
        esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
        if (esp_ota_get_state_partition(running, &st) == ESP_OK &&
            st == ESP_OTA_IMG_PENDING_VERIFY) {
            xSemaphoreGive(s_ctx.mutex);
            free(url_copy);
            ESP_LOGW(TAG, "ota: start_rejected reason=pending_verify");
            return ESP_ERR_NOT_ALLOWED;
        }
    }
    s_ctx.err[0] = '\0';
    s_ctx.state = MICHI_OTA_IDLE;
    s_ctx.percent = 0;
    const BaseType_t rc = xTaskCreate(ota_task, MICHI_OTA_TASK_NAME,
                                      CONFIG_MICHI_OTA_STACK_BYTES, url_copy,
                                      MICHI_OTA_TASK_PRIORITY, &s_ctx.task);
    if (rc != pdPASS) {
        s_ctx.task = NULL;
        xSemaphoreGive(s_ctx.mutex);
        free(url_copy);
        set_failed(ESP_ERR_NO_MEM, "task create");
        michi_state_request(MICHI_STATE_IDLE);
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(s_ctx.mutex);

    /* Gate passed: the OTA task exists, so the FSM may land on UPDATING
     * and observers get the started event. The SESSION_CLOSED post (from
     * the abort above) was queued BEFORE this request, so the FSM applies
     * IDLE first and the UPDATING request maps. */
    const esp_err_t req_err = michi_state_request(MICHI_STATE_UPDATING);
    if (req_err != ESP_OK) {
        ESP_LOGW(TAG, "ota: state_request failed err=%s state=%s",
                 esp_err_to_name(req_err), michi_state_name(michi_state_get()));
    }
    michi_state_post(MICHI_EVENT_UPDATE_STARTED, 0);

    ESP_LOGI(TAG, "ota: started");
    return ESP_OK;
}

esp_err_t michi_ota_get_state(michi_ota_state_t *state, int *percent,
                              char *err, size_t err_len)
{
    if (state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ctx.mutex == NULL) {
        *state = MICHI_OTA_IDLE;
        if (percent != NULL) {
            *percent = 0;
        }
        if (err != NULL && err_len > 0) {
            err[0] = '\0';
        }
        return ESP_OK;
    }
    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
    *state = s_ctx.state;
    if (percent != NULL) {
        *percent = s_ctx.percent;
    }
    if (err != NULL && err_len > 0) {
        strlcpy(err, s_ctx.err, err_len);
    }
    xSemaphoreGive(s_ctx.mutex);
    return ESP_OK;
}

bool michi_ota_busy(void)
{
    if (s_ctx.mutex == NULL) {
        return false;
    }
    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
    const bool busy = (s_ctx.task != NULL);
    xSemaphoreGive(s_ctx.mutex);
    return busy;
}

esp_err_t michi_ota_boot_selftest_done(bool selftest_ok)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running == NULL) {
        ESP_LOGW(TAG, "ota: boot_selftest skipped reason=running_unknown");
        return ESP_ERR_INVALID_STATE;
    }
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    if (esp_ota_get_state_partition(running, &st) != ESP_OK) {
        ESP_LOGW(TAG, "ota: boot_selftest skipped reason=state_unreadable");
        return ESP_ERR_INVALID_STATE;
    }
    if (st != ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "ota: boot_selftest noop state=%s (not pending verify)",
                 image_state_name(st));
        return ESP_OK;
    }
    if (selftest_ok) {
        const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ota: mark_valid failed err=%s", esp_err_to_name(err));
            return err;
        }
        ESP_LOGI(TAG, "ota: boot_selftest=pass marked_valid partition=%s",
                 running->label);
        return ESP_OK;
    }
    ESP_LOGE(TAG, "ota: boot_selftest=FAIL rollback partition=%s "
                  "(restarting for bootloader rollback)", running->label);
    esp_restart();
    return ESP_OK; /* unreachable; esp_restart never returns */
}
