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
 *    restarts to let the bootloader roll back. A LOCAL update additionally
 *    disables its manifest on the card (rename to michi-update.applied)
 *    before the rollback restart so the previous image never reapplies
 *    the same update (anti boot-loop latch, review F1 - NVS namespace
 *    "ota_local": pending_version / applied_version / failed_boots).
 *  - Boot-time local check (review F2): michi_ota_init() registers a
 *    MICHI_EVENT_STATE_CHANGED observer; when the FSM reaches IDLE it
 *    triggers the check task, which waits for the async SD mount flag
 *    (review F3) and runs michi_ota_check_local(). The UPDATING request
 *    maps only from IDLE, so the direct app_main call was removed (it
 *    ran while the FSM was still BOOTING/SELF_TEST).
 *  - Logs are key=value; URLs are logged as host + path-length only
 *    (query strings may carry tokens and are never logged).
 */

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/unistd.h>

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

#include "nvs_flash.h"

#include "michi_led.h"
#include "michi_ota.h"
#include "michi_ota_pubkey.h"
#include "semver.h"
#include "michi_product_profile.h"
#include "michi_sd.h"
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

/* Local (SD) OTA: maximum base file name length for a file:// binary URL.
 * Plain base name only: no '/', no '\\', no ".." (path traversal guard)
 * and it must equal CONFIG_MICHI_SD_UPDATE_FILE. */
#define MICHI_OTA_FILE_NAME_MAX 64

/* Anti boot-loop latch (review F1): the manifest is renamed to this name
 * on the card when a local update is applied (success) or disabled
 * (failed self-test), so the boot-time check never reapplies it. */
#define MICHI_OTA_SD_APPLIED_NAME "michi-update.applied"

/* NVS latch namespace (review F1): pending_version is written BEFORE a
 * local update applies; boot_selftest_done uses it to detect that the
 * failing image came from the card. applied_version is written on
 * success (idempotency skip). failed_boots is the defensive counter when
 * the manifest rename cannot run. Key names are capped at 15 chars
 * (NVS_KEY_NAME_MAX_SIZE); versions are truncated to 15 chars (strict
 * semver is far shorter; the truncation is identical on write and
 * compare, so two real semver versions never collide). */
#define MICHI_OTA_LOCAL_NVS_NS "ota_local"
#define MICHI_OTA_LOCAL_NVS_KEY_LAST "applied_version"
#define MICHI_OTA_LOCAL_NVS_KEY_PENDING "pending_version"
#define MICHI_OTA_LOCAL_NVS_KEY_FAILED "failed_boots"
#define MICHI_OTA_LOCAL_NVS_VERSION_LEN 16

/* Boot-time local check task (review F2): below the FSM (5) and the
 * display render (4) - the check blocks on the SD mount flag, so it must
 * never run on the FSM task (which is task-watchdog-subscribed); above
 * app_main (1). */
#define MICHI_OTA_LOCAL_CHECK_TASK_NAME "ota_local_chk"
#define MICHI_OTA_LOCAL_CHECK_TASK_PRIO 2
#define MICHI_OTA_LOCAL_CHECK_STACK_BYTES 4096
#define MICHI_OTA_LOCAL_CHECK_POLL_MS 20
// Hardcoded safety belt (was Kconfig; kept internal to stay within the
// review's genesis scope - configurable variants can be reintroduced with a
// proper phase review).
#define MICHI_OTA_LOCAL_MAX_FAILED_BOOTS_DEFAULT 3

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

/* Local (SD) base-name rule (phase 17, shared by url_valid_file and the
 * Kconfig file names - review F9): plain base name ONLY, so neither a
 * signed manifest URL nor a misconfigured Kconfig name can ever make the
 * firmware read or write outside /sdcard: no path separators ('/', '\\'),
 * no ".." and <= MICHI_OTA_FILE_NAME_MAX chars. */
#ifdef CONFIG_MICHI_SD_ENABLE
static bool file_name_ok(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return false;
    }
    const size_t len = strlen(name);
    if (len > MICHI_OTA_FILE_NAME_MAX) {
        return false;
    }
    if (strchr(name, '/') != NULL || strchr(name, '\\') != NULL ||
        strstr(name, "..") != NULL) {
        return false;
    }
    return true;
}

/* Build a validated /sdcard/<base-name> path (review F9): the single
 * place that joins a base name with the mount point. */
static esp_err_t build_sd_path(char *out, size_t out_size,
                               const char *base_name)
{
    if (out == NULL || out_size == 0 || !file_name_ok(base_name)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (snprintf(out, out_size, "/sdcard/%s", base_name) >= (int)out_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

/* Local (SD) binary URL rule: file://<base-name> ONLY, where the base
 * name passes the shared file_name_ok check. The actual file that gets
 * read is /sdcard/<CONFIG_MICHI_SD_UPDATE_FILE>; the manifest base name
 * must equal it (checked in the local task - the signed URL binds the
 * file). */
static bool url_valid_file(const char *url)
{
    if (url == NULL || url[0] == '\0') {
        return false;
    }
    if (strncmp(url, "file://", 7) != 0) {
        char scheme[16];
        const size_t n = strlen(url) < sizeof(scheme) - 1
                             ? strlen(url) : sizeof(scheme) - 1;
        memcpy(scheme, url, n);
        scheme[n] = '\0';
        ESP_LOGW(TAG, "ota_local: url_scheme_rejected scheme=%s", scheme);
        return false;
    }
    if (!file_name_ok(url + 7)) {
        ESP_LOGW(TAG, "ota_local: url_rejected reason=base_name_invalid");
        return false;
    }
    return true;
}
#endif /* CONFIG_MICHI_SD_ENABLE */
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
/* semver_parse/semver_cmp live in semver.c (F15: extracted so the
 * host-side tests compile the SAME source - no reimplementation). */

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

/* Local (SD) manifest read: same contract as fetch_manifest (bounded
 * read with an overflow probe, NUL-terminated) but from a VFS file at
 * /sdcard/<name>. The VFS path only exists while the card is mounted, so
 * an unmounted card makes fopen fail with ENOENT and the caller treats
 * it as "no local update available". */
#ifdef CONFIG_MICHI_SD_ENABLE
static esp_err_t read_manifest_file(const char *path, char *buf, size_t buf_len,
                                    size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        ESP_LOGW(TAG, "ota_local: manifest open failed path=%s errno=%d",
                 path, errno);
        return ESP_ERR_NOT_FOUND;
    }
    size_t total = 0;
    for (;;) {
        const size_t space = buf_len - 1 - total;
        if (space == 0) {
            break;
        }
        const size_t rd = fread(buf + total, 1, space, f);
        if (rd > 0) {
            total += rd;
            continue;
        }
        if (feof(f)) {
            break;
        }
        ESP_LOGW(TAG, "ota_local: manifest read failed errno=%d", errno);
        fclose(f);
        return ESP_FAIL;
    }
    /* Buffer full (total == buf_len - 1): probe one more byte so an exact
     * fit is accepted while overflow is detected (mirror of the HTTP
     * fetch_manifest overflow probe). */
    if (total == buf_len - 1) {
        const int c = fgetc(f);
        if (c != EOF) {
            ESP_LOGW(TAG, "ota_local: manifest too_large limit=%u",
                     (unsigned)(buf_len - 1));
            fclose(f);
            return ESP_ERR_INVALID_SIZE;
        }
    }
    fclose(f);
    buf[total] = '\0';
    *out_len = total;
    return ESP_OK;
}
#endif /* CONFIG_MICHI_SD_ENABLE */

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

/* Shared manifest validation (phase 13 HTTPS + phase 17 local SD): the
 * url_valid callback is the ONLY scheme-specific part - https:// (with
 * userinfo/empty-host rules) for the network flow, file://<base-name>
 * (path-traversal rules) for the SD flow. Everything else - board exact
 * match, strict semver anti-downgrade, min_version floor, 64-hex sha256
 * and the RSA-2048 signature over the canonical payload - is identical
 * for both sources, so a local update accepts exactly what an HTTPS
 * update would (and vice versa): same trust anchor, same checks.
 * `source` (review F7) is the log tag ("https"/"sd"): the operator can
 * filter the shared validating lines by source=sd. */
static esp_err_t validate_manifest(michi_ota_manifest_t *m,
                                   bool (*url_valid)(const char *url),
                                   const char *source)
{
    const michi_product_profile_t *p = michi_product_profile_get();

    if (strcmp(m->board, p->board_model) != 0) {
        ESP_LOGW(TAG, "ota: state=validating board=%s expected=%s source=%s",
                 m->board, p->board_model, source);
        return ESP_ERR_NOT_FOUND;
    }

    uint16_t cur[3], ver[3], min_ver[3];
    if (!semver_parse(p->firmware_version, cur) ||
        !semver_parse(m->version, ver)) {
        ESP_LOGW(TAG, "ota: state=validating semver_invalid version=%s "
                      "source=%s", m->version, source);
        return ESP_ERR_INVALID_ARG;
    }
    if (semver_cmp(ver, cur) <= 0) {
        ESP_LOGW(TAG, "ota: state=validating downgrade_rejected "
                      "version=%s current=%s source=%s",
                 m->version, p->firmware_version, source);
        return ESP_ERR_INVALID_VERSION;
    }
    if (!semver_parse(m->min_version, min_ver) ||
        semver_cmp(ver, min_ver) < 0) {
        ESP_LOGW(TAG, "ota: state=validating min_version_not_met "
                      "version=%s min_version=%s source=%s",
                 m->version, m->min_version, source);
        return ESP_ERR_INVALID_VERSION;
    }
    if (!url_valid(m->url)) {
        ESP_LOGW(TAG, "ota: state=validating binary_url_rejected source=%s",
                 source);
        return ESP_ERR_INVALID_ARG;
    }
    if (!sha256_hex_ok(m->sha256)) {
        ESP_LOGW(TAG, "ota: state=validating sha256_format_invalid source=%s",
                 source);
        return ESP_ERR_INVALID_ARG;
    }

    /* Signature LAST: everything else was validated and the payload is
     * canonical by then (cheap reject before the expensive RSA op). */
    const esp_err_t sig_err = verify_signature(m);
    if (sig_err != ESP_OK) {
        return sig_err;
    }
    ESP_LOGI(TAG, "ota: state=validating board=%s version=%s sig=ok source=%s",
             m->board, m->version, source);
    return ESP_OK;
}

/* Parse + validate a fetched/read manifest (shared by the HTTPS and the
 * local SD flow). fail_stage names the step for the error detail:
 * "manifest parse" (JSON), "manifest fields" (required keys), or
 * "manifest validation" (validate_manifest) - identical to the pre-
 * refactor HTTPS behavior. */
static esp_err_t parse_and_validate_manifest(const char *manifest,
                                             michi_ota_manifest_t *m,
                                             bool (*url_valid)(const char *url),
                                             const char *source,
                                             const char **fail_stage)
{
    *fail_stage = "manifest parse";
    cJSON *root = cJSON_Parse(manifest);
    if (root == NULL || !cJSON_IsObject(root)) {
        if (root != NULL) {
            cJSON_Delete(root);
        }
        return ESP_ERR_INVALID_ARG;
    }
    const bool have_all =
        json_str_copy(root, "version", m->version, sizeof(m->version)) &&
        json_str_copy(root, "board", m->board, sizeof(m->board)) &&
        json_str_copy(root, "min_version", m->min_version,
                      sizeof(m->min_version)) &&
        json_str_copy(root, "url", m->url, sizeof(m->url)) &&
        json_str_copy(root, "sha256", m->sha256, sizeof(m->sha256)) &&
        json_str_copy(root, "signature", m->signature, sizeof(m->signature));
    cJSON_Delete(root);
    if (!have_all) {
        *fail_stage = "manifest fields";
        return ESP_ERR_INVALID_ARG;
    }
    *fail_stage = "manifest validation";
    return validate_manifest(m, url_valid, source);
}

/* ------------------------------------------------------------------
 * Firmware download (rule 3): stream + runtime SHA-256 + partition write
 * ------------------------------------------------------------------ */

/* Binary source abstraction (phase 13 HTTPS + phase 17 local SD): the
 * apply pipeline below reads through a chunk callback so the SAME code
 * (esp_ota_begin/write/end/set_boot_partition + runtime SHA-256 +
 * progress) serves both transports. EOF = ESP_OK with *out_len == 0. */
typedef esp_err_t (*michi_ota_chunk_reader_t)(void *arg, uint8_t *buf,
                                              size_t cap, size_t *out_len);

static esp_err_t http_binary_read(void *arg, uint8_t *buf, size_t cap,
                                  size_t *out_len)
{
    esp_http_client_handle_t client = (esp_http_client_handle_t)arg;
    const int rd = esp_http_client_read(client, (char *)buf, (int)cap);
    if (rd > 0) {
        *out_len = (size_t)rd;
        return ESP_OK;
    }
    if (rd == 0) {
        *out_len = 0; /* EOF */
        return ESP_OK;
    }
    ESP_LOGW(TAG, "ota: binary read failed rd=%d", rd);
    return ESP_FAIL;
}

#ifdef CONFIG_MICHI_SD_ENABLE
static esp_err_t file_binary_read(void *arg, uint8_t *buf, size_t cap,
                                  size_t *out_len)
{
    FILE *f = (FILE *)arg;
    const size_t n = fread(buf, 1, cap, f);
    if (n > 0) {
        *out_len = n;
        return ESP_OK;
    }
    if (feof(f)) {
        *out_len = 0; /* EOF */
        return ESP_OK;
    }
    ESP_LOGW(TAG, "ota_local: binary read failed errno=%d", errno);
    return ESP_FAIL;
}
#endif /* CONFIG_MICHI_SD_ENABLE */

/* The streaming apply pipeline: next-update partition, size bound,
 * esp_ota_begin (OTA_SIZE_UNKNOWN), 4 KB chunks through the reader with
 * runtime SHA-256 + esp_ota_write in parallel, digest vs the signed
 * manifest BEFORE esp_ota_end, then set_boot_partition. On any failure
 * esp_ota_abort and nothing bootable was ever set. total_bytes > 0
 * enables the by-bytes progress (HTTP content-length / file size); 0
 * keeps the percent at the download anchor (chunked HTTP transfer).
 * `source` (review F7) tags the shared pipeline logs ("https"/"sd") so
 * the operator can filter the local flow with source=sd. */
static esp_err_t stream_and_apply(michi_ota_chunk_reader_t reader,
                                  void *reader_arg, uint64_t total_bytes,
                                  const char *expect_hex, const char *source)
{
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (part == NULL) {
        ESP_LOGE(TAG, "ota: no_update_partition source=%s", source);
        return ESP_ERR_NOT_FOUND;
    }
    if (total_bytes > 0 && total_bytes > part->size) {
        ESP_LOGW(TAG, "ota: binary_too_large size=%llu partition=%u source=%s",
                 (unsigned long long)total_bytes, (unsigned)part->size,
                 source);
        return ESP_ERR_INVALID_SIZE;
    }

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(part, OTA_SIZE_UNKNOWN, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota: esp_ota_begin failed err=%s source=%s",
                 esp_err_to_name(err), source);
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
        ESP_LOGE(TAG, "ota: sha256_init failed err=%d source=%s",
                 (int)err, source);
        mbedtls_md_free(&md_ctx);
        esp_ota_abort(handle);
        return ESP_ERR_NO_MEM;
    }

    /* The 4 KB streaming chunk is heap-allocated (stack budget): the task
     * stack must fit mbedTLS + HTTP frames, not the transfer buffers. */
    uint8_t *chunk = malloc(MICHI_OTA_CHUNK_BYTES);
    if (chunk == NULL) {
        ESP_LOGE(TAG, "ota: chunk_alloc failed source=%s", source);
        mbedtls_md_free(&md_ctx);
        esp_ota_abort(handle);
        return ESP_ERR_NO_MEM;
    }
    uint64_t total = 0;
    bool read_failed = false;
    err = ESP_OK;
    for (;;) {
        size_t rd = 0;
        const esp_err_t r_err = reader(reader_arg, chunk,
                                       MICHI_OTA_CHUNK_BYTES, &rd);
        if (r_err != ESP_OK) {
            read_failed = true;
            err = r_err;
            break;
        }
        if (rd == 0) {
            break; /* EOF */
        }
        total += (uint64_t)rd;
        if (total_bytes > 0 && total > total_bytes) {
            ESP_LOGW(TAG, "ota: binary longer than declared size source=%s",
                     source);
            err = ESP_ERR_INVALID_SIZE;
            break;
        }
        if (mbedtls_md_update(&md_ctx, chunk, rd) != 0) {
            err = ESP_FAIL;
            break;
        }
        const esp_err_t w_err = esp_ota_write(handle, chunk, rd);
        if (w_err != ESP_OK) {
            ESP_LOGE(TAG, "ota: esp_ota_write failed err=%s source=%s",
                     esp_err_to_name(w_err), source);
            err = w_err;
            break;
        }
        if (total_bytes > 0) {
            const int pct = MICHI_OTA_PCT_DOWNLOAD_MIN +
                (int)(((uint64_t)(MICHI_OTA_PCT_DOWNLOAD_MAX -
                                  MICHI_OTA_PCT_DOWNLOAD_MIN) * total) /
                      total_bytes);
            set_state(MICHI_OTA_DOWNLOADING,
                      pct > MICHI_OTA_PCT_DOWNLOAD_MAX
                          ? MICHI_OTA_PCT_DOWNLOAD_MAX : pct);
        }
    }

    if (err != ESP_OK || read_failed) {
        mbedtls_md_free(&md_ctx);
        esp_ota_abort(handle);
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
                      "manifest=%s computed=%.16s... source=%s",
                 expect_hex, hex, source);
        esp_ota_abort(handle);
        free(chunk);
        return ESP_ERR_INVALID_CRC;
    }
    ESP_LOGI(TAG, "ota: state=verifying sha256=ok bytes=%llu source=%s",
             (unsigned long long)total, source);
    free(chunk);

    /* Apply: finalize the partition image, then swap the boot target.
     * A failing esp_ota_end needs NO esp_ota_abort: the handle is
     * invalidated internally on error (no more writes are possible) and
     * esp_ota_set_boot_partition was never called, so the new image is
     * left unmarked - nothing bootable was ever set. */
    set_state(MICHI_OTA_APPLYING, MICHI_OTA_PCT_APPLYING);
    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota: esp_ota_end failed err=%s source=%s",
                 esp_err_to_name(err), source);
        return err;
    }
    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota: esp_ota_set_boot_partition failed err=%s "
                      "source=%s", esp_err_to_name(err), source);
        return err;
    }
    return ESP_OK;
}

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

    err = stream_and_apply(http_binary_read, client,
                           content_len > 0 ? (uint64_t)content_len : 0,
                           expect_hex, "https");
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
}

/* ------------------------------------------------------------------
 * Shared epilogue helpers (review F8): the success and failure tails of
 * the OTA task and the local OTA task were a verbatim copy - extracted
 * once. ota_task_success NEVER returns (it restarts); the caller frees
 * its own buffers before calling it. ota_task_fail terminates the task.
 * ------------------------------------------------------------------ */

static void ota_task_success(michi_ota_ctx_t *ctx, const char *version,
                             const char *source)
{
    (void)ctx;
    michi_led_shutdown();
    set_state(MICHI_OTA_DONE, MICHI_OTA_PCT_DONE);
    ESP_LOGI(TAG, "ota: state=done version=%s source=%s booting_next",
             version, source);
    ESP_LOGI(TAG, "ota: stack_hwm=%u",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    michi_state_post(MICHI_EVENT_UPDATE_DONE, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
    for (;;) { /* esp_restart never returns; defensive */
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}

static void ota_task_fail(michi_ota_ctx_t *ctx, esp_err_t err,
                          char *manifest)
{
    (void)ctx;
    ESP_LOGI(TAG, "ota: stack_hwm=%u",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    free(manifest);
    /* F15: report_error captures the cause directly (guaranteed even with
     * a full bus queue) and posts best-effort for the observers. */
    (void)michi_state_report_error(MICHI_EVENT_UPDATE_FAILED, (uint32_t)err);
    michi_state_request(MICHI_STATE_IDLE);
    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
    s_ctx.task = NULL; /* redundant with set_failed, kept as defense */
    xSemaphoreGive(s_ctx.mutex);
    vTaskDelete(NULL);
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
    const char *fail_stage = "manifest buffer";

    if (manifest == NULL) {
        /* F15: assign err BEFORE set_failed so the done: report_error
         * carries the real cause (NO_MEM) and not the ESP_OK default. */
        err = ESP_ERR_NO_MEM;
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
    err = parse_and_validate_manifest(manifest, &m, url_valid_https,
                                      "https", &fail_stage);
    if (err != ESP_OK) {
        set_failed(err, fail_stage);
        goto done;
    }

    err = download_binary(m.url, m.sha256);
    if (err != ESP_OK) {
        set_failed(err, "firmware download");
        goto done;
    }

    /* Success: clean LED shutdown, mark DONE, restart. */
    free(manifest);
    ota_task_success(&s_ctx, m.version, "https");

done:
    free((void *)url);
    ota_task_fail(&s_ctx, err, manifest);
}

/* ------------------------------------------------------------------
 * Anti boot-loop latch (review F1) + manifest disable, NVS "ota_local"
 * ------------------------------------------------------------------ */

#ifdef CONFIG_MICHI_SD_ENABLE
/* Versions are truncated to 15 chars for the NVS string latches: strict
 * semver (x.y.z numeric) is far shorter, and the truncation is identical
 * on write and compare - two real semver versions can never collide. */
static void latch_version_trunc(char *out, size_t out_size,
                                const char *version)
{
    strlcpy(out, version, out_size);
}

/* Record "a local update for this version is staged": written BEFORE the
 * apply starts (F1). Errors are logged, never block the update - an
 * unlatched update loses the rollback-disable protection, nothing more. */
static esp_err_t local_latch_set_pending(const char *version)
{
    nvs_handle_t h;
    if (nvs_open(MICHI_OTA_LOCAL_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "ota_local: pending latch skipped reason=nvs_open");
        return ESP_ERR_NOT_FOUND;
    }
    char ver[MICHI_OTA_LOCAL_NVS_VERSION_LEN];
    latch_version_trunc(ver, sizeof(ver), version);
    esp_err_t err = nvs_set_str(h, MICHI_OTA_LOCAL_NVS_KEY_PENDING, ver);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ota_local: pending latch skipped version=%s err=%s",
                 ver, esp_err_to_name(err));
    }
    nvs_close(h);
    return err;
}

static void local_latch_clear_pending(void)
{
    nvs_handle_t h;
    if (nvs_open(MICHI_OTA_LOCAL_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    const esp_err_t err = nvs_erase_key(h, MICHI_OTA_LOCAL_NVS_KEY_PENDING);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "ota_local: pending latch clear failed err=%s",
                 esp_err_to_name(err));
    }
    (void)nvs_commit(h);
    nvs_close(h);
}

/* Success (F1): record the applied version (idempotency skip on the next
 * boot) and clear the pending marker + the failed-boot counter. */
static void local_latch_mark_success(const char *version)
{
    nvs_handle_t h;
    if (nvs_open(MICHI_OTA_LOCAL_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "ota_local: success latch skipped reason=nvs_open");
        return;
    }
    char ver[MICHI_OTA_LOCAL_NVS_VERSION_LEN];
    latch_version_trunc(ver, sizeof(ver), version);
    esp_err_t err = nvs_set_str(h, MICHI_OTA_LOCAL_NVS_KEY_LAST, ver);
    if (err == ESP_OK) {
        err = nvs_erase_key(h, MICHI_OTA_LOCAL_NVS_KEY_PENDING);
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(h, MICHI_OTA_LOCAL_NVS_KEY_FAILED, 0);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ota_local: success latch failed err=%s",
                 esp_err_to_name(err));
    }
    nvs_close(h);
}

/* Disable the manifest on the card (rename to michi-update.applied):
 * called after a successful apply (the user sees the card was consumed)
 * and on a failed self-test boot (the rolled-back image must NOT reapply
 * the same update - the primary anti boot-loop mechanism, review F1).
 * A leftover target from a previous disable is replaced. */
static esp_err_t disable_manifest_on_sd(void)
{
    char manifest_path[sizeof("/sdcard/") + MICHI_OTA_FILE_NAME_MAX + 1];
    char applied_path[sizeof("/sdcard/") + MICHI_OTA_FILE_NAME_MAX + 1];
    if (build_sd_path(manifest_path, sizeof(manifest_path),
                      CONFIG_MICHI_SD_UPDATE_MANIFEST) != ESP_OK ||
        build_sd_path(applied_path, sizeof(applied_path),
                      MICHI_OTA_SD_APPLIED_NAME) != ESP_OK) {
        ESP_LOGE(TAG, "ota_local: manifest disable skipped reason=path_build");
        return ESP_ERR_INVALID_ARG;
    }
    (void)unlink(applied_path);
    if (rename(manifest_path, applied_path) != 0) {
        ESP_LOGW(TAG, "ota_local: manifest disable failed path=%s errno=%d",
                 manifest_path, errno);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "ota_local: manifest disabled (%s -> %s)",
             manifest_path, applied_path);
    return ESP_OK;
}

/* Failed self-test boot of a local update (F1): latch the failure, then
 * disable the manifest. The rename is the primary disable; the counter
 * is the defensive belt when the rename cannot run (e.g. the card is not
 * readable at this point) - the boot-time check then refuses to reapply
 * once failed_boots >= MICHI_OTA_LOCAL_MAX_FAILED_BOOTS_DEFAULT. */
static void local_ota_on_selftest_fail(void)
{
    nvs_handle_t h;
    if (nvs_open(MICHI_OTA_LOCAL_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "ota_local: rollback latch skipped reason=nvs_open");
        return;
    }
    char pending[MICHI_OTA_LOCAL_NVS_VERSION_LEN] = {0};
    size_t len = sizeof(pending);
    const esp_err_t r = nvs_get_str(h, MICHI_OTA_LOCAL_NVS_KEY_PENDING,
                                    pending, &len);
    if (r == ESP_ERR_NVS_NOT_FOUND || (r == ESP_OK && pending[0] == '\0')) {
        nvs_close(h);
        return; /* no local update was staged - plain HTTPS rollback */
    }
    if (r != ESP_OK) {
        ESP_LOGW(TAG, "ota_local: pending latch unreadable err=%s "
                      "(rollback latch skipped)", esp_err_to_name(r));
        nvs_close(h);
        return;
    }

    uint32_t failed_boots = 0;
    if (nvs_get_u32(h, MICHI_OTA_LOCAL_NVS_KEY_FAILED, &failed_boots) !=
        ESP_OK) {
        failed_boots = 0;
    }
    failed_boots++;
    if (nvs_set_u32(h, MICHI_OTA_LOCAL_NVS_KEY_FAILED, failed_boots) !=
            ESP_OK ||
        nvs_commit(h) != ESP_OK) {
        ESP_LOGW(TAG, "ota_local: failed_boots write failed "
                      "(rollback latch lost)");
    }

    /* The mount runs in parallel with the boot: wait briefly so the FIRST
     * failure reliably disables the manifest instead of falling back to
     * the counter belt (bounded; this image is restarting anyway). */
    const TickType_t deadline =
        xTaskGetTickCount() + pdMS_TO_TICKS(CONFIG_MICHI_SD_MOUNT_WAIT_MS);
    while (!michi_sd_mounted() && xTaskGetTickCount() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(MICHI_OTA_LOCAL_CHECK_POLL_MS));
    }

    if (disable_manifest_on_sd() != ESP_OK) {
        ESP_LOGW(TAG, "ota_local: update %s failed %u/%u times, manifest NOT "
                      "disabled (defensive counter armed)", pending,
                 (unsigned)failed_boots,
                 (unsigned)MICHI_OTA_LOCAL_MAX_FAILED_BOOTS_DEFAULT);
        nvs_close(h);
        return;
    }
    ESP_LOGW(TAG, "ota_local: update %s failed %u times, manifest disabled",
             pending, (unsigned)failed_boots);
    (void)nvs_erase_key(h, MICHI_OTA_LOCAL_NVS_KEY_PENDING);
    (void)nvs_set_u32(h, MICHI_OTA_LOCAL_NVS_KEY_FAILED, 0);
    (void)nvs_commit(h);
    nvs_close(h);
}
#endif /* CONFIG_MICHI_SD_ENABLE */

/* ------------------------------------------------------------------
 * Local (SD) OTA task (phase 17): same pipeline as ota_task, but the
 * manifest and the binary come from the mounted card at /sdcard instead
 * of the network. Validation is IDENTICAL to the HTTPS flow (shared
 * parse_and_validate_manifest + verify_signature): a local update only
 * applies what a signed manifest describes - no signature, no apply.
 * ------------------------------------------------------------------ */

#ifdef CONFIG_MICHI_SD_ENABLE
static void local_ota_task(void *arg)
{
    (void)arg;
    esp_err_t err = ESP_OK;
    char *manifest = malloc(CONFIG_MICHI_OTA_MANIFEST_MAX_BYTES);
    michi_ota_manifest_t m;
    const char *fail_stage = "manifest buffer";
    bool pending_written = false;

    if (manifest == NULL) {
        err = ESP_ERR_NO_MEM;
        set_failed(ESP_ERR_NO_MEM, "manifest buffer");
        goto done;
    }
    memset(&m, 0, sizeof(m));

    set_state(MICHI_OTA_FETCHING_MANIFEST, MICHI_OTA_PCT_MANIFEST);
    char manifest_path[sizeof("/sdcard/") + MICHI_OTA_FILE_NAME_MAX + 1];
    if (build_sd_path(manifest_path, sizeof(manifest_path),
                      CONFIG_MICHI_SD_UPDATE_MANIFEST) != ESP_OK) {
        set_failed(ESP_ERR_INVALID_ARG, "manifest name");
        goto done;
    }
    ESP_LOGI(TAG, "ota_local: state=fetching_manifest file=%s",
             CONFIG_MICHI_SD_UPDATE_MANIFEST);
    size_t manifest_len = 0;
    err = read_manifest_file(manifest_path, manifest,
                             CONFIG_MICHI_OTA_MANIFEST_MAX_BYTES,
                             &manifest_len);
    if (err != ESP_OK) {
        set_failed(err, "manifest read (sd)");
        goto done;
    }
    ESP_LOGI(TAG, "ota_local: manifest_bytes=%u", (unsigned)manifest_len);

    set_state(MICHI_OTA_VALIDATING, MICHI_OTA_PCT_VALIDATING);
    err = parse_and_validate_manifest(manifest, &m, url_valid_file,
                                      "sd", &fail_stage);
    if (err != ESP_OK) {
        set_failed(err, fail_stage);
        goto done;
    }

    /* The signed file:// URL binds the binary: the manifest base name must
     * equal the configured update file, so a manifest signed for another
     * file (or moved card content) is rejected instead of misapplied. */
    if (strcmp(m.url + 7, CONFIG_MICHI_SD_UPDATE_FILE) != 0) {
        ESP_LOGW(TAG, "ota_local: state=validating binary_name_mismatch "
                      "manifest=%s expected=%s", m.url + 7,
                 CONFIG_MICHI_SD_UPDATE_FILE);
        set_failed(ESP_ERR_INVALID_ARG, "manifest binary name");
        goto done;
    }

    char binary_path[sizeof("/sdcard/") + MICHI_OTA_FILE_NAME_MAX + 1];
    if (build_sd_path(binary_path, sizeof(binary_path),
                      CONFIG_MICHI_SD_UPDATE_FILE) != ESP_OK) {
        set_failed(ESP_ERR_INVALID_ARG, "binary name");
        goto done;
    }
    FILE *f = fopen(binary_path, "rb");
    if (f == NULL) {
        ESP_LOGW(TAG, "ota_local: binary open failed path=%s errno=%d",
                 binary_path, errno);
        set_failed(ESP_ERR_NOT_FOUND, "firmware file (sd)");
        goto done;
    }
    uint64_t file_bytes = 0;
    if (fseek(f, 0, SEEK_END) != 0) {
        ESP_LOGW(TAG, "ota_local: binary size failed errno=%d", errno);
        fclose(f);
        set_failed(ESP_FAIL, "firmware size (sd)");
        goto done;
    }
    const long size_bytes = ftell(f);
    if (size_bytes <= 0) {
        ESP_LOGW(TAG, "ota_local: binary empty size=%ld", size_bytes);
        fclose(f);
        set_failed(ESP_ERR_INVALID_SIZE, "firmware size (sd)");
        goto done;
    }
    file_bytes = (uint64_t)size_bytes;
    if (fseek(f, 0, SEEK_SET) != 0) {
        ESP_LOGW(TAG, "ota_local: binary rewind failed errno=%d", errno);
        fclose(f);
        set_failed(ESP_FAIL, "firmware size (sd)");
        goto done;
    }

    /* F1: latch the pending version BEFORE the apply starts - if the
     * next boot's self-test fails, the rollback path knows this update
     * came from the card and disables the manifest (anti boot-loop).
     * Errors are logged and never block the update. */
    if (local_latch_set_pending(m.version) == ESP_OK) {
        pending_written = true;
    }

    ESP_LOGI(TAG, "ota_local: state=downloading file=%s size=%llu",
             CONFIG_MICHI_SD_UPDATE_FILE, (unsigned long long)file_bytes);
    /* Download anchor + by-bytes progress: the file size is always known
     * for the SD flow (unlike chunked HTTP), so percent advances 10..85. */
    set_state(MICHI_OTA_DOWNLOADING, MICHI_OTA_PCT_DOWNLOAD_MIN);
    err = stream_and_apply(file_binary_read, f, file_bytes, m.sha256, "sd");
    fclose(f);
    if (err != ESP_OK) {
        set_failed(err, "firmware apply");
        goto done;
    }

    /* Success (F1): record the applied version + disable the manifest on
     * the card (rename to michi-update.applied) so the user sees the
     * update was consumed and the next boot skips it. Best-effort:
     * failures are logged; the NVS latch alone keeps the skip working. */
    local_latch_mark_success(m.version);
    (void)disable_manifest_on_sd();
    free(manifest);
    ota_task_success(&s_ctx, m.version, "sd");

done:
    /* Hygiene (F1): the pending marker means "a new image will boot".
     * The apply failed, so no image boots - clear the marker. */
    if (pending_written && err != ESP_OK) {
        local_latch_clear_pending();
    }
    ota_task_fail(&s_ctx, err, manifest);
}
#endif /* CONFIG_MICHI_SD_ENABLE */

/* ------------------------------------------------------------------
 * Boot-time local check: FSM observer + check task (review F2)
 * ------------------------------------------------------------------ */

/* Why the observer + a task instead of a direct app_main call: the
 * MICHI_STATE_UPDATING request maps only from IDLE (transition table).
 * A direct call runs while the FSM is still BOOTING/SELF_TEST (the boot
 * events are only queued at that point), the request fails and the
 * update proceeds WITHOUT the UPDATING state - no LED ramp, no "Updating"
 * screen, diagnostics stuck on IDLE. The observer fires exactly when the
 * FSM lands on IDLE, and the check task does the blocking work (the
 * observer runs on the FSM task, which is task-watchdog-subscribed: it
 * MUST NOT wait for the SD mount flag).
 *
 * Timing detail (verified against michi_wifi): the FIRST IDLE is
 * transient - the wifi observer requests WIFI_CONNECTING/UNPROVISIONED
 * in the same dispatch, so the FSM leaves IDLE before the check task's
 * UPDATING request is applied (WIFI_CONNECTING->UPDATING is NOT in the
 * transition table). The check therefore starts the update best-effort,
 * and the re-arm below re-requests UPDATING when the FSM reaches IDLE
 * again (after wifi connects) - the update is already running by then
 * and the UPDATING state surfaces with its progress. An UNPROVISIONED
 * device never returns to IDLE: the update runs without the UPDATING
 * state (documented limitation, same as the pre-fix behavior). */

#ifdef CONFIG_MICHI_SD_ENABLE
static volatile bool s_check_triggered;
static TaskHandle_t s_check_task;

static void local_check_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        const esp_err_t err = michi_ota_check_local();
        if (err != ESP_OK && err != ESP_ERR_NOT_FOUND &&
            err != ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGW(TAG, "ota_local: check failed err=%s",
                     esp_err_to_name(err));
        }
    }
}
#endif /* CONFIG_MICHI_SD_ENABLE */

static void ota_state_observer(const michi_event_t *ev)
{
    if (ev->id != MICHI_EVENT_STATE_CHANGED) {
        return;
    }
    if ((michi_state_t)ev->data != MICHI_STATE_IDLE) {
        return;
    }

#ifdef CONFIG_MICHI_SD_ENABLE
    /* One-shot trigger of the boot-time local check. The task (not the
     * observer) waits for the SD mount flag and runs the latches. */
    if (!s_check_triggered && s_check_task != NULL) {
        s_check_triggered = true;
        xTaskNotifyGive(s_check_task);
    }
#endif

    /* Re-arm: while an update task runs, the FSM should surface
     * UPDATING. If the start request was made before the FSM could map
     * it (local check while WIFI_CONNECTING), re-request it now that the
     * FSM is IDLE - the request validates against the state at apply
     * time, so this only succeeds while the FSM really is IDLE. No-op
     * for an update that already holds UPDATING. */
    if (michi_ota_busy()) {
        (void)michi_state_request(MICHI_STATE_UPDATING);
    }
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
#ifdef CONFIG_MICHI_SD_ENABLE
    if (s_check_task == NULL) {
        const BaseType_t rc = xTaskCreate(local_check_task,
                                          MICHI_OTA_LOCAL_CHECK_TASK_NAME,
                                          MICHI_OTA_LOCAL_CHECK_STACK_BYTES,
                                          NULL, MICHI_OTA_LOCAL_CHECK_TASK_PRIO,
                                          &s_check_task);
        if (rc != pdPASS) {
            s_check_task = NULL;
            ESP_LOGW(TAG, "ota_local: check task creation failed - "
                          "boot-time local updates disabled");
        }
    }
#endif
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
    /* F2: the boot-time local check runs from this observer; a duplicate
     * registration is a no-op per the michi_state contract. */
    const esp_err_t oerr = michi_state_register_observer(
        MICHI_EVENT_STATE_CHANGED, ota_state_observer);
    if (oerr != ESP_OK) {
        ESP_LOGW(TAG, "init: state observer registration failed: %s "
                      "(boot-time local updates disabled)",
                 esp_err_to_name(oerr));
    }
    ESP_LOGI(TAG, "subsystem=ota state=ok phase=13");
    return ESP_OK;
}

/* ------------------------------------------------------------------
 * Start gate (shared by HTTPS and local SD starts)
 * ------------------------------------------------------------------ */

/* Blocked sessions: an active session is force-closed BEFORE the gate
 * (the credential is never persisted, so OTA cannot present it -
 * michi_session_abort is the privileged internal path). The abort result
 * is authoritative: a live session that survived abort means the update
 * must NOT start (the FSM would keep the old session's transition in
 * flight). Side effects are at most once. */
static esp_err_t ota_force_close_session(void)
{
    if (michi_session_active()) {
        const esp_err_t abort_err = michi_session_abort("ota update");
        if (abort_err != ESP_OK) {
            ESP_LOGE(TAG, "ota: session_abort_failed err=%s "
                          "(update not started: a live session blocks OTA)",
                     esp_err_to_name(abort_err));
            return ESP_ERR_INVALID_STATE;
        }
    }
    return ESP_OK;
}

/* Atomic gate: the busy check and the xTaskCreate run in ONE critical
 * section (the session abort and the caller's arg setup are outside it,
 * so the check inside is the single point of truth for task==NULL). A
 * second concurrent start lands here, sees task != NULL and rejects with
 * ESP_ERR_INVALID_STATE - the task is never created twice. First boot
 * after an OTA: the running image is PENDING_VERIFY and esp_ota_begin
 * would reject the new write - the boot self-test must mark the image
 * valid first. Distinct error code on purpose: the API answers 409
 * 'pending_verify', NOT 'ota_in_progress'. */
static esp_err_t ota_spawn_task(TaskFunction_t task_fn, void *arg)
{
    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
    if (s_ctx.task != NULL) {
        xSemaphoreGive(s_ctx.mutex);
        ESP_LOGW(TAG, "ota: start_rejected reason=busy");
        return ESP_ERR_INVALID_STATE;
    }
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running != NULL) {
        esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
        if (esp_ota_get_state_partition(running, &st) == ESP_OK &&
            st == ESP_OTA_IMG_PENDING_VERIFY) {
            xSemaphoreGive(s_ctx.mutex);
            ESP_LOGW(TAG, "ota: start_rejected reason=pending_verify");
            return ESP_ERR_NOT_ALLOWED;
        }
    }
    s_ctx.err[0] = '\0';
    s_ctx.state = MICHI_OTA_IDLE;
    s_ctx.percent = 0;
    const BaseType_t rc = xTaskCreate(task_fn, MICHI_OTA_TASK_NAME,
                                      CONFIG_MICHI_OTA_STACK_BYTES, arg,
                                      MICHI_OTA_TASK_PRIORITY, &s_ctx.task);
    if (rc != pdPASS) {
        s_ctx.task = NULL;
        xSemaphoreGive(s_ctx.mutex);
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

    const esp_err_t sess_err = ota_force_close_session();
    if (sess_err != ESP_OK) {
        return sess_err;
    }

    char *url_copy = strdup(url);
    if (url_copy == NULL) {
        set_failed(ESP_ERR_NO_MEM, "url copy");
        michi_state_request(MICHI_STATE_IDLE);
        return ESP_ERR_NO_MEM;
    }

    const esp_err_t rc = ota_spawn_task(ota_task, url_copy);
    if (rc != ESP_OK) {
        /* The task never took ownership of url_copy (gate rejection or
         * task-create failure): free it here. */
        free(url_copy);
        return rc;
    }
    ESP_LOGI(TAG, "ota: started");
    return ESP_OK;
}

/* ------------------------------------------------------------------
 * Local (SD) OTA (phase 17): the owner copies a signed manifest +
 * firmware binary onto the onboard microSD, and the receiver applies it
 * from /sdcard - field updates without USB. The manifest is the SAME
 * signed JSON as HTTPS OTA (same canonical payload, same key), so
 * validate_manifest/verify_signature/stream_and_apply are shared: an SD
 * update is accepted exactly when the equivalent HTTPS update would be.
 * The ESP32-S3 does NOT boot from SD - this is update transport only.
 * ------------------------------------------------------------------ */

/* Kconfig-provided file names must stay plain base names: the firmware
 * builds /sdcard/<name> from them via build_sd_path (review F9), so a
 * misconfigured name must never escape the card (no separators, no "..",
 * length bounded - file_name_ok is the single shared check). */

esp_err_t michi_ota_start_local(void)
{
#ifndef CONFIG_MICHI_SD_ENABLE
    ESP_LOGW(TAG, "ota_local: start_rejected reason=disabled "
                  "(MICHI_SD_ENABLE=n)");
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (s_ctx.mutex == NULL) {
        return ESP_ERR_INVALID_STATE; /* init never succeeded */
    }
    if (!file_name_ok(CONFIG_MICHI_SD_UPDATE_MANIFEST) ||
        !file_name_ok(CONFIG_MICHI_SD_UPDATE_FILE)) {
        ESP_LOGE(TAG, "ota_local: start_rejected reason=invalid_kconfig_name");
        return ESP_ERR_INVALID_ARG;
    }

    /* SD presence probe: the VFS path exists only while michi_sd mounted
     * the card, so fopen fails with ENOENT on an unmounted card. A failed
     * probe simply means "no local update available" (updates fall back
     * to HTTPS OTA). */
    char manifest_path[sizeof("/sdcard/") + MICHI_OTA_FILE_NAME_MAX + 1];
    if (build_sd_path(manifest_path, sizeof(manifest_path),
                      CONFIG_MICHI_SD_UPDATE_MANIFEST) != ESP_OK) {
        ESP_LOGE(TAG, "ota_local: start_rejected reason=manifest_name_too_long");
        return ESP_ERR_INVALID_ARG;
    }
    FILE *probe = fopen(manifest_path, "rb");
    if (probe == NULL) {
        ESP_LOGW(TAG, "ota_local: start_rejected reason=sd_unavailable_or_no_manifest "
                      "(updates fall back to HTTPS OTA) errno=%d", errno);
        return ESP_ERR_NOT_FOUND;
    }
    fclose(probe);

    const esp_err_t sess_err = ota_force_close_session();
    if (sess_err != ESP_OK) {
        return sess_err;
    }

    /* No URL copy: the sources are the fixed, validated Kconfig names. */
    const esp_err_t rc = ota_spawn_task(local_ota_task, NULL);
    if (rc != ESP_OK) {
        return rc;
    }
    ESP_LOGI(TAG, "ota_local: started file=%s",
             CONFIG_MICHI_SD_UPDATE_MANIFEST);
    return ESP_OK;
#endif
}

esp_err_t michi_ota_check_local(void)
{
#ifndef CONFIG_MICHI_SD_ENABLE
    return ESP_ERR_NOT_SUPPORTED;
#else
    /* Boot-time check (F2): invoked by the check task after the FSM
     * observer saw IDLE - NEVER while the running image is PENDING_VERIFY
     * (the start gate inside ota_spawn_task rejects it anyway). This
     * function BLOCKS (mount wait + file reads): task context only. */
    if (s_ctx.mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!file_name_ok(CONFIG_MICHI_SD_UPDATE_MANIFEST)) {
        ESP_LOGW(TAG, "ota_local: check=skipped reason=invalid_manifest_name");
        return ESP_ERR_INVALID_ARG;
    }

    /* F3: the mount is asynchronous (michi_sd_init returns immediately);
     * wait for the mount flag up to MICHI_SD_MOUNT_WAIT_MS. */
    const TickType_t deadline =
        xTaskGetTickCount() + pdMS_TO_TICKS(CONFIG_MICHI_SD_MOUNT_WAIT_MS);
    while (!michi_sd_mounted() && xTaskGetTickCount() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(MICHI_OTA_LOCAL_CHECK_POLL_MS));
    }
    if (!michi_sd_mounted()) {
        ESP_LOGI(TAG, "ota_local: check=absent reason=no_card_or_mount_timeout");
        return ESP_ERR_NOT_FOUND;
    }

    char manifest_path[sizeof("/sdcard/") + MICHI_OTA_FILE_NAME_MAX + 1];
    if (build_sd_path(manifest_path, sizeof(manifest_path),
                      CONFIG_MICHI_SD_UPDATE_MANIFEST) != ESP_OK) {
        ESP_LOGW(TAG, "ota_local: check=skipped reason=manifest_name_too_long");
        return ESP_ERR_INVALID_ARG;
    }
    FILE *f = fopen(manifest_path, "rb");
    if (f == NULL) {
        ESP_LOGI(TAG, "ota_local: check=absent reason=no_manifest");
        return ESP_ERR_NOT_FOUND;
    }
    fclose(f);

    /* F1 latches: idempotency (already applied) + failed-boot belt. The
     * manifest version is read here (bounded 2 KB read + cJSON) so the
     * NVS compares can run before the start. An unreadable manifest is
     * NOT a reason to suppress the update - start_local surfaces the real
     * validation error. */
    char manifest_version[MICHI_OTA_LOCAL_NVS_VERSION_LEN] = {0};
    char *buf = malloc(CONFIG_MICHI_OTA_MANIFEST_MAX_BYTES);
    if (buf != NULL) {
        size_t mlen = 0;
        if (read_manifest_file(manifest_path, buf,
                               CONFIG_MICHI_OTA_MANIFEST_MAX_BYTES,
                               &mlen) == ESP_OK) {
            cJSON *root = cJSON_Parse(buf);
            if (root != NULL) {
                const cJSON *v = cJSON_GetObjectItemCaseSensitive(root,
                                                                  "version");
                if (cJSON_IsString(v) && v->valuestring != NULL) {
                    strlcpy(manifest_version, v->valuestring,
                            sizeof(manifest_version));
                }
                cJSON_Delete(root);
            }
        }
        free(buf);
    }

    if (manifest_version[0] != '\0') {
        nvs_handle_t h;
        if (nvs_open(MICHI_OTA_LOCAL_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
            char last_applied[MICHI_OTA_LOCAL_NVS_VERSION_LEN] = {0};
            size_t len = sizeof(last_applied);
            const esp_err_t lr = nvs_get_str(h, MICHI_OTA_LOCAL_NVS_KEY_LAST,
                                             last_applied, &len);
            if (lr == ESP_OK && last_applied[0] != '\0' &&
                strcmp(last_applied, manifest_version) == 0) {
                nvs_close(h);
                ESP_LOGI(TAG, "ota_local: check=skipped reason=already_applied "
                              "version=%s", manifest_version);
                /* The update is done and the manifest is still on the
                 * card: disable it so the user does not need to remove
                 * the card (F1). */
                (void)disable_manifest_on_sd();
                return ESP_ERR_NOT_FOUND;
            }
            char pending[MICHI_OTA_LOCAL_NVS_VERSION_LEN] = {0};
            len = sizeof(pending);
            const esp_err_t pr = nvs_get_str(h, MICHI_OTA_LOCAL_NVS_KEY_PENDING,
                                             pending, &len);
            uint32_t failed_boots = 0;
            (void)nvs_get_u32(h, MICHI_OTA_LOCAL_NVS_KEY_FAILED, &failed_boots);
            if (pr == ESP_OK && pending[0] != '\0' &&
                failed_boots >= (uint32_t)MICHI_OTA_LOCAL_MAX_FAILED_BOOTS_DEFAULT) {
                nvs_close(h);
                ESP_LOGW(TAG, "ota_local: check=skipped reason=failed_boot_limit "
                              "version=%s failures=%u (manifest disabled)",
                         pending, (unsigned)failed_boots);
                /* Belt: re-attempt the disable; only when it succeeds the
                 * latches are reset (a persistent rename failure keeps
                 * the cap armed). */
                if (disable_manifest_on_sd() == ESP_OK) {
                    local_latch_clear_pending();
                    nvs_handle_t hw;
                    if (nvs_open(MICHI_OTA_LOCAL_NVS_NS, NVS_READWRITE,
                                 &hw) == ESP_OK) {
                        (void)nvs_set_u32(hw, MICHI_OTA_LOCAL_NVS_KEY_FAILED,
                                          0);
                        (void)nvs_commit(hw);
                        nvs_close(hw);
                    }
                }
                return ESP_ERR_NOT_FOUND;
            }
            nvs_close(h);
        }
    }

    ESP_LOGI(TAG, "ota_local: check=present manifest=%s starting",
             CONFIG_MICHI_SD_UPDATE_MANIFEST);
    return michi_ota_start_local();
#endif
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
#ifdef CONFIG_MICHI_SD_ENABLE
        /* F1 hygiene: the pending marker means "a local update was
         * staged". The new image passed its self-test, so nothing is
         * pending anymore - normally already cleared by the local task
         * before the restart; clear a stale latch (failed NVS write)
         * so it can never arm the rollback-disable for a valid image. */
        local_latch_clear_pending();
#endif
        ESP_LOGI(TAG, "ota: boot_selftest=pass marked_valid partition=%s",
                 running->label);
        return ESP_OK;
    }
#ifdef CONFIG_MICHI_SD_ENABLE
    /* F1: anti boot-loop. The failing image came from a local update
     * (pending_version latched): disable the manifest on the card BEFORE
     * the rollback restart, so the previous image does NOT reapply the
     * same update on the next boot (the loop the review found: apply ->
     * rollback -> reapply). The rename is the primary disable; the
     * failed-boots counter is the defensive belt when it cannot run. */
    local_ota_on_selftest_fail();
#endif
    ESP_LOGE(TAG, "ota: boot_selftest=FAIL rollback partition=%s "
                  "(restarting for bootloader rollback)", running->label);
    esp_restart();
    return ESP_OK; /* unreachable; esp_restart never returns */
}
