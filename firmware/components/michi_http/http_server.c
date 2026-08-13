/*
 * HTTP API layer: the canonical Michi Link receiver v1-lite surface
 * (MS-03).
 *
 * Only the canonical routes of the vendored Michi Link bundle
 * (contracts/michi-link/) are registered - no legacy route is kept:
 *
 *   GET    /api/v1/server/info
 *   POST   /api/v1/pair/start
 *   GET    /api/v1/pair/status
 *   POST   /api/v1/pair/confirm
 *   POST   /api/v1/receiver-lite/session
 *   GET    /api/v1/receiver-lite/session
 *   PATCH  /api/v1/receiver-lite/session
 *   DELETE /api/v1/receiver-lite/session
 *   POST   /api/v1/receiver-lite/heartbeat
 *   PUT    /api/v1/receiver-lite/now-playing
 *   GET    /api/v1/receiver-lite/diagnostics
 *   GET    /api/v1/receiver-lite/firmware
 *   POST   /api/v1/receiver-lite/firmware
 *
 * /server/info emits the exact receiver v1-lite profile (build_info_json,
 * canonical_json.c). The receiver-button pairing flow (MS-06), the
 * canonical RTP session and lease (MS-07/MS-08), the certified
 * now-playing payload and the OTA flow are NOT implemented yet: their
 * handlers still enforce the route-table auth and the strict JSON body
 * gate, then answer 501 NOT_IMPLEMENTED, and the matching feature flag
 * in /server/info is false. Diagnostics is the only optional extension
 * implemented (its response shape is not frozen by the contract, so the
 * existing snapshot conforms). Unknown routes answer the canonical 404
 * error envelope via the registered httpd 404 handler.
 *
 * Every error response uses the single canonical envelope
 * (michi_http_send_error): {error:{code,message,request_id,details}},
 * with the code derived from the HTTP status per the section 2.7 map -
 * no local lowercase codes exist. JSON bodies are read completely and
 * parsed strictly (malformed/oversized/non-object -> 400 INVALID_REQUEST)
 * before any execution, so parse/validation stays separate from the
 * deferred execution.
 *
 * Handler contract (shared with michi_http.h): copy ALL values out of
 * the cJSON tree BEFORE delete, never return pointers into the tree;
 * parse -> copy -> delete -> process -> respond. No handler may deviate
 * from it. Bearer tokens are validated by michi_pairing_validate_token
 * (constant-time registry scan) and their VALUE is never logged.
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "cJSON.h"

#include "michi_audio.h"
#include "michi_audio_output.h"
#include "michi_dac.h"
#include "michi_http.h"
#include "michi_ota.h"
#include "michi_pairing.h"
#include "michi_product_profile.h"
#include "michi_sd.h"
#include "michi_session.h"
#include "michi_state.h"
#include "michi_wifi.h"

#define TAG "michi_http"

#define MICHI_HTTP_PORT 80
#define MICHI_HTTP_BODY_MAX 2048    /* body limit for canonical bodies */
#define MICHI_HTTP_RECV_TIMEOUT_RETRIES 1  /* single timeout retry */
#define MICHI_HTTP_BODY_TOTAL_TIMEOUT_MS 2000 /* anti-slowloris: total body deadline */

#define MICHI_HTTP_AUTH_HEADER_MAX 80   /* "Bearer " + 64 hex + NUL */

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
    case 403: *out = "403 Forbidden"; break;
    case 404: *out = "404 Not Found"; break;
    case 409: *out = "409 Conflict"; break;
    case 429: *out = "429 Too Many Requests"; break;
    case 500: *out = "500 Internal Server Error"; break;
    case 501: *out = "501 Not Implemented"; break;
    default:  *out = "500 Internal Server Error"; break;
    }
}

/* Section 2.7 error map: the canonical error code is derived from the
 * HTTP status - there is exactly one code per status, so a handler can
 * never invent a local lowercase code. */
static const char *status_to_code(int status)
{
    switch (status) {
    case 400: return "INVALID_REQUEST";
    case 401: return "UNAUTHORIZED";
    case 403: return "FORBIDDEN";
    case 404: return "NOT_FOUND";
    case 409: return "CONFLICT";
    case 429: return "RATE_LIMITED";
    case 501: return "NOT_IMPLEMENTED";
    case 500: /* fall through */
    default:  return "INTERNAL_ERROR";
    }
}

/* Correlation id for the canonical error envelope: a fresh UUID v4 per
 * error (format uuid in the vendored error schema). */
static void request_id_generate(char *out, size_t out_len)
{
    if (out == NULL || out_len < 37) {
        return;
    }
    const uint32_t a = esp_random();
    const uint32_t b = esp_random();
    const uint32_t c = esp_random();
    const uint32_t d = esp_random();
    /* UUID v4: time_low - time_mid - 4xxx - (10xx variant) - node. */
    snprintf(out, out_len, "%08" PRIx32 "-%04x-4%03x-%04x-%08" PRIx32,
             a,
             b & 0xFFFFu,                          /* time_mid */
             (b >> 16) & 0xFFFu,                   /* version 4 + time_hi */
             (uint32_t)((c & 0x3FFFu) | 0x8000u),  /* variant 10 + clock_seq */
             d);
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
                                const char *message, const char *field)
{
    if (req == NULL || message == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    char request_id[37];
    request_id_generate(request_id, sizeof(request_id));
    cJSON *root = NULL;
    esp_err_t err = michi_http_build_error(&root, status_to_code(status),
                                           message, request_id, field);
    if (err != ESP_OK) {
        return err;
    }
    err = michi_http_send_json(req, status, root);
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
     * a client error - the caller MUST answer 400. */
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

/* JSON access helpers (michi_http_json_get_string/int/bool) live in
 * json_helpers.c (F15: extracted for host-side testing - the component
 * and tests/host compile the SAME source). */

/* Send the standard auth failures. The token is NEVER part of any log or
 * error message (only its VALIDATION outcome is). */
static esp_err_t send_auth_error(httpd_req_t *req, bool forbidden)
{
    if (forbidden) {
        return michi_http_send_error(req, 403,
                                     "the controller token lacks the "
                                     "required permission for this endpoint",
                                     NULL);
    }
    return michi_http_send_error(req, 401,
                                 "missing, malformed or unknown bearer token",
                                 NULL);
}

/* Bearer auth + permission gate. Never logs the token; on success the
 * owning controller id (not secret) is copied out. Returns:
 *  ESP_OK                 - authorized; out_controller_id (>= 32) filled.
 *  ESP_ERR_NOT_FOUND      - missing/malformed/unknown token -> 401.
 *  ESP_ERR_INVALID_STATE  - token valid but lacks the permission -> 403. */
static esp_err_t require_auth(httpd_req_t *req, uint32_t perm,
                              char *out_controller_id, size_t id_len)
{
    char auth[MICHI_HTTP_AUTH_HEADER_MAX] = {0};
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth,
                                    sizeof(auth)) != ESP_OK) {
        return ESP_ERR_NOT_FOUND; /* missing or oversized header */
    }
    const char *prefix = "Bearer ";
    if (strncmp(auth, prefix, strlen(prefix)) != 0) {
        return ESP_ERR_NOT_FOUND; /* wrong scheme: treated as absent */
    }
    const char *token = auth + strlen(prefix);
    uint32_t permissions = 0;
    esp_err_t err = michi_pairing_validate_token(token, out_controller_id,
                                                 id_len, &permissions);
    if (err != ESP_OK) {
        return ESP_ERR_NOT_FOUND; /* malformed/unknown/before-init */
    }
    if ((permissions & perm) == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

/* Auth gate for handlers: true = authorized (id filled); false = the
 * 401/403 response was ALREADY SENT and the handler MUST return
 * immediately - with ESP_OK (P0-5: once a response was sent the handler
 * returns the send outcome, never ESP_FAIL). */
static bool auth_gate(httpd_req_t *req, uint32_t perm,
                      char *out_controller_id, size_t id_len)
{
    const esp_err_t err = require_auth(req, perm, out_controller_id, id_len);
    if (err == ESP_OK) {
        return true;
    }
    send_auth_error(req, err == ESP_ERR_INVALID_STATE);
    return false;
}

/* Read + parse a JSON body; on failure answers 400 INVALID_REQUEST and
 * returns NULL. The caller owns the returned tree. */
static cJSON *read_json_body(httpd_req_t *req)
{
    char body[MICHI_HTTP_BODY_MAX];
    size_t body_len = 0;
    const esp_err_t err = michi_http_read_body(req, body, sizeof(body),
                                               &body_len);
    if (err != ESP_OK) {
        michi_http_send_error(req, 400,
                              "a JSON request body is required", NULL);
        return NULL;
    }
    cJSON *root = cJSON_Parse(body);
    if (root == NULL || !cJSON_IsObject(root)) {
        if (root != NULL) {
            cJSON_Delete(root);
        }
        michi_http_send_error(req, 400,
                              "the request body is not a JSON object", NULL);
        return NULL;
    }
    return root;
}

/* 501 NOT_IMPLEMENTED for a canonical route whose semantics are deferred
 * to a later package (MS-03 deferral: Ed25519 pairing, RTP session,
 * lease, certified now-playing and OTA). */
static esp_err_t send_not_implemented(httpd_req_t *req, const char *message)
{
    return michi_http_send_error(req, 501, message, NULL);
}

/* A deferred route that takes a JSON body still runs the strict body
 * gate: a missing/malformed/oversized/non-object body answers 400
 * INVALID_REQUEST (parse/validation is separated from execution - the
 * execution itself is what is deferred). */
static esp_err_t not_implemented_with_body(httpd_req_t *req,
                                           const char *message)
{
    cJSON *root = read_json_body(req);
    if (root == NULL) {
        return ESP_OK; /* 400 already sent (P0-5) */
    }
    cJSON_Delete(root);
    return send_not_implemented(req, message);
}

/* ------------------------------------------------------------------
 * Endpoints
 * ------------------------------------------------------------------ */

/* GET /api/v1/server/info (no auth): the exact receiver v1-lite info
 * profile (build_info_json in canonical_json.c - same source compiled by
 * the host tests). */
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

/* POST /api/v1/pair/start (no auth): deferred - the canonical
 * RECEIVER_BUTTON flow (Ed25519 challenge, PIN, 120 s physical window)
 * lands in MS-06. */
static esp_err_t pair_start_handler(httpd_req_t *req)
{
    return not_implemented_with_body(req,
                                     "canonical receiver-button pairing is "
                                     "not implemented");
}

/* GET /api/v1/pair/status (no auth; session_id query): deferred (MS-06).
 */
static esp_err_t pair_status_handler(httpd_req_t *req)
{
    return send_not_implemented(req,
                                "canonical receiver-button pairing is "
                                "not implemented");
}

/* POST /api/v1/pair/confirm (no auth): deferred (MS-06). */
static esp_err_t pair_confirm_handler(httpd_req_t *req)
{
    return not_implemented_with_body(req,
                                     "canonical receiver-button pairing is "
                                     "not implemented");
}

/* POST /api/v1/receiver-lite/session (Bearer): deferred - the canonical
 * RTP session lifecycle lands in MS-07/MS-08. */
static esp_err_t session_start_handler(httpd_req_t *req)
{
    char controller_id[32] = {0};
    if (!auth_gate(req, MICHI_PERM_PLAYBACK, controller_id,
                     sizeof(controller_id))) {
        return ESP_OK; /* 401/403 already sent (P0-5) */
    }
    return not_implemented_with_body(req,
                                     "canonical receiver-lite audio sessions "
                                     "are not implemented");
}

/* GET /api/v1/receiver-lite/session (Bearer): deferred (MS-07). */
static esp_err_t session_current_get_handler(httpd_req_t *req)
{
    char controller_id[32] = {0};
    if (!auth_gate(req, MICHI_PERM_STATUS, controller_id,
                     sizeof(controller_id))) {
        return ESP_OK; /* 401/403 already sent (P0-5) */
    }
    return send_not_implemented(req,
                                "canonical receiver-lite audio sessions are "
                                "not implemented");
}

/* PATCH /api/v1/receiver-lite/session (Bearer): deferred (MS-07). */
static esp_err_t session_patch_handler(httpd_req_t *req)
{
    char controller_id[32] = {0};
    if (!auth_gate(req, MICHI_PERM_PLAYBACK, controller_id,
                     sizeof(controller_id))) {
        return ESP_OK; /* 401/403 already sent (P0-5) */
    }
    return not_implemented_with_body(req,
                                     "canonical receiver-lite audio sessions "
                                     "are not implemented");
}

/* DELETE /api/v1/receiver-lite/session (Bearer, no body): deferred
 * (MS-07). */
static esp_err_t session_delete_handler(httpd_req_t *req)
{
    char controller_id[32] = {0};
    if (!auth_gate(req, MICHI_PERM_PLAYBACK, controller_id,
                     sizeof(controller_id))) {
        return ESP_OK; /* 401/403 already sent (P0-5) */
    }
    return send_not_implemented(req,
                                "canonical receiver-lite audio sessions are "
                                "not implemented");
}

/* POST /api/v1/receiver-lite/heartbeat (Bearer): deferred - the
 * monotonic lease renewal lands in MS-08. */
static esp_err_t v1lite_heartbeat_handler(httpd_req_t *req)
{
    char controller_id[32] = {0};
    if (!auth_gate(req, MICHI_PERM_STATUS, controller_id,
                     sizeof(controller_id))) {
        return ESP_OK; /* 401/403 already sent (P0-5) */
    }
    return not_implemented_with_body(req,
                                     "the canonical heartbeat lease is not "
                                     "implemented");
}

/* PUT /api/v1/receiver-lite/now-playing (Bearer): deferred - the payload
 * shape is not frozen by the contract until the extension is certified.
 */
static esp_err_t now_playing_put_handler(httpd_req_t *req)
{
    char controller_id[32] = {0};
    if (!auth_gate(req, MICHI_PERM_PLAYBACK, controller_id,
                     sizeof(controller_id))) {
        return ESP_OK; /* 401/403 already sent (P0-5) */
    }
    return not_implemented_with_body(req,
                                     "now-playing metadata is not "
                                     "implemented");
}

/* GET /api/v1/receiver-lite/firmware (Bearer): deferred - update
 * availability semantics are not implemented. */
static esp_err_t firmware_get_handler(httpd_req_t *req)
{
    char controller_id[32] = {0};
    if (!auth_gate(req, MICHI_PERM_STATUS, controller_id,
                     sizeof(controller_id))) {
        return ESP_OK; /* 401/403 already sent (P0-5) */
    }
    return send_not_implemented(req,
                                "firmware management is not implemented");
}

/* POST /api/v1/receiver-lite/firmware (Bearer + OTA permission):
 * deferred - the canonical OTA flow (url + checksum) is not implemented.
 */
static esp_err_t firmware_post_handler(httpd_req_t *req)
{
    char controller_id[32] = {0};
    if (!auth_gate(req, MICHI_PERM_OTA, controller_id,
                     sizeof(controller_id))) {
        return ESP_OK; /* 401/403 already sent (P0-5) */
    }
    return not_implemented_with_body(req,
                                     "firmware management is not implemented");
}

/* OTA state name for the diagnostics response. */
static const char *ota_state_name(michi_ota_state_t st)
{
    switch (st) {
    case MICHI_OTA_IDLE:             return "idle";
    case MICHI_OTA_FETCHING_MANIFEST: return "fetching_manifest";
    case MICHI_OTA_VALIDATING:       return "validating";
    case MICHI_OTA_DOWNLOADING:      return "downloading";
    case MICHI_OTA_VERIFYING:        return "verifying";
    case MICHI_OTA_APPLYING:         return "applying";
    case MICHI_OTA_DONE:             return "done";
    case MICHI_OTA_FAILED:           return "failed";
    default:                         return "unknown";
    }
}

/* F14: esp_reset_reason() -> stable diagnostic name (documented in the
 * README Diagnostics section). The mapping is explicit per enum value so
 * an IDF rename never silently renames a string. */
static const char *reset_reason_name(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:    return "POWERON";
    case ESP_RST_EXT:        return "EXT";
    case ESP_RST_SW:         return "SW";
    case ESP_RST_PANIC:      return "PANIC";
    case ESP_RST_INT_WDT:    return "INT_WDT";
    case ESP_RST_TASK_WDT:   return "TWDT";
    case ESP_RST_WDT:        return "OWDT";
    case ESP_RST_DEEPSLEEP:  return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:   return "BROWNOUT";
    case ESP_RST_SDIO:       return "SDIO";
    case ESP_RST_USB:        return "USB_UART";
    case ESP_RST_JTAG:       return "USB_JTAG";
    case ESP_RST_EFUSE:      return "EFUSE";
    case ESP_RST_PWR_GLITCH: return "PWR_GLITCH";
    case ESP_RST_CPU_LOCKUP: return "CPU_LOCKUP";
    case ESP_RST_UNKNOWN:
    default:                 return "UNKNOWN";
    }
}

/* F14: event ids captured by the michi_state last-error slot -> names. */
static const char *last_error_event_name(michi_event_id_t id)
{
    switch (id) {
    case MICHI_EVENT_ERROR:          return "error";
    case MICHI_EVENT_UPDATE_FAILED:  return "update_failed";
    default:                         return "unknown";
    }
}

/* F15: the error-state target captured with the last error -> names.
 * The default maps to "none" (not "unknown"): the diagnostics schema
 * reserves "unknown" and this branch is unreachable - any state not
 * listed is captured as an event without a request, i.e. no target. */
static const char *last_error_target_name(michi_state_t t)
{
    switch (t) {
    case MICHI_STATE_RECOVERABLE_ERROR: return "recoverable";
    case MICHI_STATE_FATAL_ERROR:       return "fatal";
    case MICHI_STATE_COUNT:             return "none"; /* event capture, no request */
    default:                            return "none";
    }
}

/* GET /api/v1/receiver-lite/diagnostics (Bearer): uptime, heap, PSRAM,
 * Wi-Fi link, audio metrics, DAC state. Diagnostic data only - no
 * secrets (the SSID is a network name, fine; the password is never
 * touched). The contract does not freeze the response shape, so this
 * snapshot conforms as-is. */
static esp_err_t diagnostics_get_handler(httpd_req_t *req)
{
    char controller_id[32] = {0};
    if (!auth_gate(req, MICHI_PERM_STATUS, controller_id,
                     sizeof(controller_id))) {
        return ESP_OK; /* 401/403 already sent (P0-5) */
    }
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        /* F9: an OOM must still produce a response (500), never a
         * connection dropped without an answer. */
        return michi_http_send_error(req, 500,
                                     "out of memory while building "
                                     "diagnostics response", NULL);
    }
    esp_err_t send_err = ESP_OK;
    if (cJSON_AddNumberToObject(root, "uptime_seconds",
                                (double)(uint64_t)(esp_timer_get_time() / 1000000)) == NULL ||
        cJSON_AddNumberToObject(root, "heap_free",
                                (double)esp_get_free_heap_size()) == NULL ||
        cJSON_AddNumberToObject(root, "heap_min_free",
                                (double)esp_get_minimum_free_heap_size()) == NULL ||
        cJSON_AddNumberToObject(root, "psram_free",
                                (double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM)) == NULL ||
        cJSON_AddNumberToObject(root, "psram_size",
                                (double)heap_caps_get_total_size(MALLOC_CAP_SPIRAM)) == NULL ||
        cJSON_AddStringToObject(root, "reset_reason",
                                reset_reason_name()) == NULL) {
        send_err = ESP_ERR_NO_MEM;
    }
    if (send_err == ESP_OK) {
        cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
        if (wifi == NULL) {
            send_err = ESP_ERR_NO_MEM;
        } else {
            int8_t rssi = 0;
            uint32_t reconnects = 0;
            const esp_err_t rssi_err = michi_wifi_get_rssi(&rssi);
            if (cJSON_AddBoolToObject(wifi, "connected",
                                      rssi_err == ESP_OK) == NULL ||
                cJSON_AddStringToObject(wifi, "ssid",
                                        michi_wifi_get_ssid()) == NULL ||
                (rssi_err == ESP_OK &&
                 cJSON_AddNumberToObject(wifi, "rssi_dbm", rssi) == NULL) ||
                (michi_wifi_get_reconnect_count(&reconnects) == ESP_OK &&
                 cJSON_AddNumberToObject(wifi, "reconnects",
                                         reconnects) == NULL)) {
                send_err = ESP_ERR_NO_MEM;
            }
        }
    }
    if (send_err == ESP_OK) {
        michi_audio_metrics_t m;
        if (michi_audio_get_metrics(&m) != ESP_OK) {
            send_err = ESP_ERR_NO_MEM; /* defensive: before init */
        } else {
            cJSON *audio = cJSON_AddObjectToObject(root, "audio");
            uint32_t ssrc = 0;
            const bool have_ssrc = michi_audio_session_get_ssrc(&ssrc) == ESP_OK;
            if (audio == NULL ||
                cJSON_AddBoolToObject(audio, "session_active",
                                      michi_audio_session_active()) == NULL ||
                (have_ssrc &&
                 cJSON_AddNumberToObject(audio, "ssrc", (double)ssrc) == NULL) ||
                cJSON_AddNumberToObject(audio, "received", m.received) == NULL ||
                cJSON_AddNumberToObject(audio, "lost", m.lost) == NULL ||
                cJSON_AddNumberToObject(audio, "late", m.late) == NULL ||
                cJSON_AddNumberToObject(audio, "duplicate", m.duplicate) == NULL ||
                cJSON_AddNumberToObject(audio, "reordered", m.reordered) == NULL ||
                cJSON_AddNumberToObject(audio, "underruns", m.underruns) == NULL ||
                cJSON_AddNumberToObject(audio, "overruns", m.overruns) == NULL ||
                cJSON_AddNumberToObject(audio, "drops_malformed", m.drops_malformed) == NULL ||
                cJSON_AddNumberToObject(audio, "drops_pt_s24le", m.drops_pt_s24le) == NULL ||
                cJSON_AddNumberToObject(audio, "drops_pt_other", m.drops_pt_other) == NULL ||
                cJSON_AddNumberToObject(audio, "drops_ssrc_filtered", m.drops_ssrc_filtered) == NULL ||
                cJSON_AddNumberToObject(audio, "drops_payload_geometry", m.drops_payload_geometry) == NULL ||
                cJSON_AddNumberToObject(audio, "jitter_us", m.jitter_us) == NULL ||
                cJSON_AddNumberToObject(audio, "buffer_ms", m.buffer_ms) == NULL ||
                cJSON_AddNumberToObject(audio, "packets_in_buffer", m.packets_in_buffer) == NULL ||
                cJSON_AddNumberToObject(audio, "last_seq", m.last_seq) == NULL ||
                cJSON_AddNumberToObject(audio, "last_timestamp", m.last_timestamp) == NULL) {
                send_err = ESP_ERR_NO_MEM;
            }
        }
    }
    if (send_err == ESP_OK) {
        const michi_dac_caps_t *caps = michi_dac_get_caps();
        const michi_product_profile_t *p = michi_product_profile_get();
        cJSON *dac = cJSON_AddObjectToObject(root, "dac");
        if (dac == NULL ||
            cJSON_AddBoolToObject(dac, "detected", caps->detected) == NULL ||
            cJSON_AddBoolToObject(dac, "initialized", caps->initialized) == NULL ||
            cJSON_AddStringToObject(dac, "model", caps->model) == NULL ||
            cJSON_AddStringToObject(dac, "tier",
                                    michi_product_profile_tier_name()) == NULL ||
            cJSON_AddNumberToObject(dac, "sample_rate",
                                    (double)p->validated_sample_rate) == NULL ||
            cJSON_AddNumberToObject(dac, "bit_depth",
                                    (double)p->validated_bit_depth) == NULL) {
            send_err = ESP_ERR_NO_MEM;
        }
    }
    if (send_err == ESP_OK) {
        /* F14 session block: the session layer snapshot (no token, ever).
         * On no active session only {"active": false} is emitted. */
        michi_session_info_t info;
        cJSON *session = cJSON_AddObjectToObject(root, "session");
        if (session == NULL) {
            send_err = ESP_ERR_NO_MEM;
        } else if (michi_session_get_info(&info) != ESP_OK) {
            if (cJSON_AddBoolToObject(session, "active", false) == NULL) {
                send_err = ESP_ERR_NO_MEM;
            }
        } else if (cJSON_AddBoolToObject(session, "active", true) == NULL ||
                   cJSON_AddStringToObject(session, "session_id",
                                           info.session_id) == NULL ||
                   cJSON_AddStringToObject(session, "codec",
                                           info.codec) == NULL ||
                   cJSON_AddNumberToObject(session, "sample_rate",
                                           (double)info.sample_rate) == NULL ||
                   cJSON_AddNumberToObject(session, "bit_depth",
                                           (double)info.bit_depth) == NULL ||
                   cJSON_AddNumberToObject(session, "channels",
                                           (double)info.channels) == NULL ||
                   cJSON_AddNumberToObject(session, "stream_port",
                                           (double)info.stream_port) == NULL ||
                   cJSON_AddNumberToObject(session, "buffer_ms",
                                           (double)info.buffer_ms) == NULL ||
                   cJSON_AddNumberToObject(session, "volume",
                                           (double)info.volume) == NULL ||
                   cJSON_AddBoolToObject(session, "paused",
                                         info.paused) == NULL ||
                   cJSON_AddNumberToObject(session, "ssrc",
                                           (double)info.ssrc) == NULL ||
                   cJSON_AddStringToObject(session, "source_addr",
                                           info.source_addr) == NULL) {
            send_err = ESP_ERR_NO_MEM;
        }
    }
    if (send_err == ESP_OK) {
        uint32_t i2s_errors = 0;
        const bool have_i2s = michi_audio_output_get_error_count(&i2s_errors) == ESP_OK;
        cJSON *le = cJSON_AddObjectToObject(root, "last_error");
        if ((have_i2s &&
             cJSON_AddNumberToObject(root, "i2s_errors",
                                     i2s_errors) == NULL) ||
            le == NULL) {
            send_err = ESP_ERR_NO_MEM;
        } else {
            michi_last_error_t last;
            const esp_err_t le_err = michi_state_get_last_error(&last);
            if (le_err == ESP_OK) {
                if (cJSON_AddStringToObject(le, "event",
                                            last_error_event_name(last.event)) == NULL ||
                    cJSON_AddNumberToObject(le, "data",
                                            (double)last.data) == NULL ||
                    cJSON_AddStringToObject(le, "target",
                                            last_error_target_name(last.target)) == NULL) {
                    send_err = ESP_ERR_NO_MEM;
                }
            } else if (cJSON_AddNullToObject(le, "event") == NULL ||
                       cJSON_AddNumberToObject(le, "data", 0) == NULL ||
                       cJSON_AddStringToObject(le, "target", "none") == NULL) {
                send_err = ESP_ERR_NO_MEM;
            }
        }
    }
    if (send_err == ESP_OK) {
        const michi_product_profile_t *p = michi_product_profile_get();
        cJSON *fw = cJSON_AddObjectToObject(root, "firmware");
        if (fw == NULL ||
            cJSON_AddStringToObject(fw, "version",
                                    p->firmware_version) == NULL ||
            cJSON_AddStringToObject(fw, "build_date",
                                    p->build_date) == NULL ||
            cJSON_AddStringToObject(fw, "board",
                                    p->board_model) == NULL) {
            send_err = ESP_ERR_NO_MEM;
        }
    }
    if (send_err == ESP_OK) {
#ifdef CONFIG_MICHI_SD_ENABLE
        /* SD (phase 17): card presence + FAT volume sizes. Zero-secret:
         * sizes only, no file listing, no contents. F6 (review): the
         * mounted flag is the REAL mount state (michi_sd_mounted, async
         * mount flag) and total/free fall back to 0 when get_info fails
         * - a stats failure must never be reported as "not mounted". */
        const bool sd_mounted = michi_sd_mounted();
        uint64_t sd_total = 0;
        uint64_t sd_free = 0;
        (void)michi_sd_get_info(&sd_total, &sd_free); /* 0/0 on failure */
        cJSON *sd = cJSON_AddObjectToObject(root, "sd");
        if (sd == NULL ||
            cJSON_AddBoolToObject(sd, "mounted", sd_mounted) == NULL ||
            cJSON_AddNumberToObject(sd, "total_bytes",
                                    (double)sd_total) == NULL ||
            cJSON_AddNumberToObject(sd, "free_bytes",
                                    (double)sd_free) == NULL) {
            send_err = ESP_ERR_NO_MEM;
        }
#endif /* CONFIG_MICHI_SD_ENABLE */
    }
    if (send_err == ESP_OK) {
        /* OTA (phase 13): lifecycle state + progress; the error text on
         * MICHI_OTA_FAILED is diagnostic data, never a credential. */
        michi_ota_state_t ota_state = MICHI_OTA_IDLE;
        int ota_percent = 0;
        char ota_err[MICHI_OTA_ERR_MAX] = {0};
        if (michi_ota_get_state(&ota_state, &ota_percent, ota_err,
                                sizeof(ota_err)) != ESP_OK) {
            send_err = ESP_ERR_NO_MEM; /* defensive */
        } else {
            cJSON *ota = cJSON_AddObjectToObject(root, "ota");
            if (ota == NULL ||
                cJSON_AddStringToObject(ota, "state",
                                        ota_state_name(ota_state)) == NULL ||
                cJSON_AddNumberToObject(ota, "percent", ota_percent) == NULL ||
                (ota_state == MICHI_OTA_FAILED &&
                 cJSON_AddStringToObject(ota, "error", ota_err) == NULL)) {
                send_err = ESP_ERR_NO_MEM;
            }
        }
    }
    if (send_err == ESP_OK) {
        send_err = michi_http_send_json(req, 200, root);
    }
    cJSON_Delete(root);
    if (send_err != ESP_OK) {
        /* F9: any allocation failure while building the payload must
         * still produce a canonical response, never a dropped
         * connection without an answer. */
        return michi_http_send_error(req, 500,
                                     "failed to build diagnostics response",
                                     NULL);
    }
    return ESP_OK;
}

static const httpd_uri_t s_endpoints[] = {
    {.uri = "/api/v1/server/info",               .method = HTTP_GET,    .handler = info_get_handler},
    {.uri = "/api/v1/pair/start",                .method = HTTP_POST,   .handler = pair_start_handler},
    {.uri = "/api/v1/pair/status",               .method = HTTP_GET,    .handler = pair_status_handler},
    {.uri = "/api/v1/pair/confirm",              .method = HTTP_POST,   .handler = pair_confirm_handler},
    {.uri = "/api/v1/receiver-lite/session",     .method = HTTP_POST,   .handler = session_start_handler},
    {.uri = "/api/v1/receiver-lite/session",     .method = HTTP_GET,    .handler = session_current_get_handler},
    {.uri = "/api/v1/receiver-lite/session",     .method = HTTP_PATCH,  .handler = session_patch_handler},
    {.uri = "/api/v1/receiver-lite/session",     .method = HTTP_DELETE, .handler = session_delete_handler},
    {.uri = "/api/v1/receiver-lite/heartbeat",   .method = HTTP_POST,   .handler = v1lite_heartbeat_handler},
    {.uri = "/api/v1/receiver-lite/now-playing", .method = HTTP_PUT,    .handler = now_playing_put_handler},
    {.uri = "/api/v1/receiver-lite/diagnostics", .method = HTTP_GET,    .handler = diagnostics_get_handler},
    {.uri = "/api/v1/receiver-lite/firmware",    .method = HTTP_GET,    .handler = firmware_get_handler},
    {.uri = "/api/v1/receiver-lite/firmware",    .method = HTTP_POST,   .handler = firmware_post_handler},
};

/* Canonical 404 for any URI that matches no registered route: the
 * standard httpd 404 page is plain text, the contract requires the
 * canonical error envelope (section 2.7). */
static esp_err_t not_found_err_handler(httpd_req_t *req,
                                       httpd_err_code_t error)
{
    (void)error;
    return michi_http_send_error(req, 404,
                                 "The requested resource was not found",
                                 NULL);
}

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
    /* F10: explicit stack size - the default 4096 is tight for the
     * 2048-byte body buffers plus the nested cJSON frames built by the
     * diagnostics handler. */
    cfg.stack_size = 8192;

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
    err = httpd_register_err_handler(server, HTTPD_404_NOT_FOUND,
                                     not_found_err_handler);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_err_handler(404) failed: %s",
                 esp_err_to_name(err));
        httpd_stop(server);
        return err;
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
