/*
 * HTTP API layer (phases 4 + 12): the Michi Link Receiver API.
 *
 * Phase 4: read-only migrated endpoints (GET /api/v1/receiver/info and
 * GET /api/v1/receiver/firmware, no auth - same surface as the legacy
 * prototype). /info is built from the dynamic product profile
 * (michi_product_profile), the single source of truth - no product string
 * is duplicated here.
 *
 * Phase 12: the full receiver API - pairing (challenge/confirm strictly
 * INSIDE the button-opened window, controller list/revoke), sessions
 * (start/current/patch/stop), now-playing, diagnostics, updates (phase
 * 13: signed OTA), plus the v1-lite compatibility layer mapping the
 * legacy paths onto the SAME handlers with the SAME security. The bearer
 * token is validated by michi_pairing_validate_token (constant-time
 * registry scan) and the endpoint's permission bit is checked before any
 * action; the token VALUE is never logged.
 *
 * Handler contract (P0-1/P0-2 fixed by construction, shared with
 * michi_http.h): copy ALL values out of the cJSON tree BEFORE delete,
 * never return pointers into the tree; parse -> copy -> delete ->
 * process -> respond. The session token (a credential) is only ever
 * copied into local buffers for michi_session_* calls; it never appears
 * in a log format string, an error message or a response (except the
 * start response, where it is issued ONCE).
 */

#include <limits.h>
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
#include "michi_display.h"
#include "michi_http.h"
#include "michi_ota.h"
#include "michi_pairing.h"
#include "michi_product_profile.h"
#include "michi_session.h"
#include "michi_state.h"
#include "michi_volume.h"
#include "michi_wifi.h"

#define TAG "michi_http"

#define MICHI_HTTP_PORT 80
#define MICHI_HTTP_BODY_MAX 2048    /* body limit for phases 10/12 bodies */
#define MICHI_HTTP_RECV_TIMEOUT_RETRIES 1  /* single timeout retry */
#define MICHI_HTTP_BODY_TOTAL_TIMEOUT_MS 2000 /* anti-slowloris: total body deadline */

#define MICHI_HTTP_AUTH_HEADER_MAX 80   /* "Bearer " + 64 hex + NUL */
#define MICHI_HTTP_SESSION_TOKEN_HEADER "X-Michi-Session"
#define MICHI_HTTP_CONTROLLERS_PREFIX "/api/v1/receiver/controllers/"
/* OTA error text buffer for the diagnostics snapshot: MICHI_OTA_ERR_MAX
 * is exported by michi_ota.h (the component's internal bound - single
 * source of truth, no local copy to drift). */

static httpd_handle_t s_server = NULL;

/* ------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------ */

static void status_to_str(int status, const char **out)
{
    switch (status) {
    case 200: *out = "200 OK"; break;
    case 202: *out = "202 Accepted"; break;
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

/* JSON access helpers (michi_http_json_get_string/int/bool) live in
 * json_helpers.c (F15: extracted for host-side testing - the component
 * and tests/host compile the SAME source). */

/* Distinguish "field absent" from "field present but does not fit":
 * michi_http_json_get_string fails on BOTH, and an oversize VALUE is a
 * client error (400) - never a silent fallback or a silent 200 (F3). */
static bool json_value_too_long(const cJSON *obj, const char *key, size_t cap)
{
    if (obj == NULL || key == NULL || cap == 0) {
        return false;
    }
    const cJSON *item = cJSON_GetObjectItem(obj, key);
    return item != NULL && cJSON_IsString(item) &&
           item->valuestring != NULL && strlen(item->valuestring) >= cap;
}

/* Send the standard auth failures. The token is NEVER part of any log or
 * error message (only its VALIDATION outcome is). */
static esp_err_t send_auth_error(httpd_req_t *req, bool forbidden)
{
    if (forbidden) {
        return michi_http_send_error(req, 403, "insufficient_permissions",
                                     "the controller token lacks the required "
                                     "permission for this endpoint");
    }
    return michi_http_send_error(req, 401, "invalid_token",
                                 "missing, malformed or unknown bearer token");
}

/* Bearer auth + permission gate (phase 12). Never logs the token; on
 * success the owning controller id (not secret) is copied out. Returns:
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

/* Read + parse a JSON body; on failure answers 400 and returns NULL.
 * The caller owns the returned tree. */
static cJSON *read_json_body(httpd_req_t *req)
{
    char body[MICHI_HTTP_BODY_MAX];
    size_t body_len = 0;
    const esp_err_t err = michi_http_read_body(req, body, sizeof(body),
                                               &body_len);
    if (err != ESP_OK) {
        michi_http_send_error(req, 400, "invalid_request",
                              "a JSON request body is required");
        return NULL;
    }
    cJSON *root = cJSON_Parse(body);
    if (root == NULL || !cJSON_IsObject(root)) {
        if (root != NULL) {
            cJSON_Delete(root);
        }
        michi_http_send_error(req, 400, "invalid_json",
                              "the request body is not a JSON object");
        return NULL;
    }
    return root;
}

static bool id_valid_chars(const char *id)
{
    if (id == NULL || id[0] == '\0') {
        return false;
    }
    for (const char *c = id; *c != '\0'; c++) {
        const bool alnum = (*c >= 'a' && *c <= 'z') ||
                           (*c >= 'A' && *c <= 'Z') ||
                           (*c >= '0' && *c <= '9');
        if (!alnum && *c != '-') {
            return false;
        }
    }
    return true;
}

static const char *state_name_safe(void)
{
    return michi_state_name(michi_state_get());
}

/* ------------------------------------------------------------------
 * Endpoints
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

/* GET /api/v1/receiver/status (no auth): FSM state + session + tier +
 * uptime. Short and read-only: no internal state is exposed beyond what
 * /info already announces. */
static esp_err_t status_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    const uint64_t uptime_s = (uint64_t)(esp_timer_get_time() / 1000000);
    /* F8/F2: get_info reconciles (dead engine cleaned, stuck FSM
     * re-driven) so the reported flag matches reality. */
    michi_session_info_t info;
    const bool session_active = michi_session_get_info(&info) == ESP_OK;
    esp_err_t err = ESP_OK;
    if (cJSON_AddStringToObject(root, "state", state_name_safe()) == NULL ||
        cJSON_AddBoolToObject(root, "session_active", session_active) == NULL ||
        cJSON_AddStringToObject(root, "tier",
                                michi_product_profile_tier_name()) == NULL ||
        cJSON_AddNumberToObject(root, "uptime_seconds",
                                (double)uptime_s) == NULL) {
        err = ESP_ERR_NO_MEM;
    } else {
        err = michi_http_send_json(req, 200, root);
    }
    cJSON_Delete(root);
    return err;
}

/* POST /api/v1/receiver/pairing/challenge (no auth) and the v1-lite
 * POST /api/v1/receiver/pair/start: issue a challenge for an initiator
 * id, STRICTLY inside a button-opened window. The endpoint NEVER opens
 * the window - with no button press the pairing component answers
 * ESP_ERR_INVALID_STATE and the API answers 409 pairing_window_closed
 * (the phase-10 security rule: the physical button is the only
 * authority). The challenge is returned; the confirm step proves it. */
static esp_err_t pairing_challenge_handler(httpd_req_t *req)
{
    cJSON *root = read_json_body(req);
    if (root == NULL) {
        return ESP_OK; /* 400 already sent (P0-5) */
    }
    char initiator_id[32] = {0};
    const bool have_id = michi_http_json_get_string(root, "initiator_id",
                                                    initiator_id,
                                                    sizeof(initiator_id));
    cJSON_Delete(root);
    if (!have_id) {
        return michi_http_send_error(req, 400, "invalid_request",
                                     "initiator_id (1..31 chars, "
                                     "alphanumeric + '-') is required");
    }

    /* 16 random bytes = 32 hex chars + NUL (pairing challenge format). */
    char challenge[33];
    const esp_err_t err = michi_pairing_get_challenge(initiator_id, challenge,
                                                      sizeof(challenge));
    switch (err) {
    case ESP_OK: {
        cJSON *resp = cJSON_CreateObject();
        if (resp == NULL) {
            return ESP_ERR_NO_MEM;
        }
        esp_err_t send_err = ESP_OK;
        if (cJSON_AddStringToObject(resp, "challenge", challenge) == NULL ||
            cJSON_AddStringToObject(resp, "initiator_id", initiator_id) == NULL ||
            cJSON_AddNumberToObject(resp, "pairing_window_seconds",
                                    CONFIG_MICHI_PAIRING_WINDOW_SECONDS) == NULL) {
            send_err = ESP_ERR_NO_MEM;
        } else {
            send_err = michi_http_send_json(req, 200, resp);
        }
        cJSON_Delete(resp);
        return send_err;
    }
    case ESP_ERR_INVALID_STATE:
        /* Window closed (never opened, expired, or the button was not
         * pressed): the ONLY way to open it is the physical button. */
        return michi_http_send_error(req, 409, "pairing_window_closed",
                                     "no pairing window open; press the "
                                     "physical pairing button");
    case ESP_ERR_INVALID_ARG:
        return michi_http_send_error(req, 400, "invalid_request",
                                     "malformed initiator_id (1..31 chars, "
                                     "alphanumeric + '-')");
    case ESP_ERR_TIMEOUT:
        return michi_http_send_error(req, 429, "challenge_limit_exceeded",
                                     "challenge issue limit reached for "
                                     "this window");
    default:
        return michi_http_send_error(req, 500, "internal_error",
                                     esp_err_to_name(err));
    }
}

/* POST /api/v1/receiver/pairing/confirm (no auth) and the v1-lite
 * POST /api/v1/receiver/pair/confirm: prove the challenge (the request
 * accepts BOTH the phase-12 key "challenge" and the v1-lite alias
 * "nonce") and register the controller + its token. The token is stored
 * as a SHA-256 digest by the pairing component and is NEVER echoed back
 * (the legacy v1-lite response did echo it - deviation, see README). */
static esp_err_t pairing_confirm_handler(httpd_req_t *req)
{
    cJSON *root = read_json_body(req);
    if (root == NULL) {
        return ESP_OK; /* 400 already sent (P0-5) */
    }
    char challenge[65] = {0};
    char initiator_id[32] = {0};
    char token_hex[65] = {0};
    const bool have_challenge =
        michi_http_json_get_string(root, "challenge", challenge,
                                   sizeof(challenge)) ||
        michi_http_json_get_string(root, "nonce", challenge,
                                   sizeof(challenge));
    const bool have_id = michi_http_json_get_string(root, "initiator_id",
                                                    initiator_id,
                                                    sizeof(initiator_id));
    const bool have_token = michi_http_json_get_string(root, "token",
                                                       token_hex,
                                                       sizeof(token_hex));
    cJSON_Delete(root);
    if (!have_challenge || !have_id || !have_token) {
        return michi_http_send_error(req, 400, "invalid_request",
                                     "challenge/nonce, initiator_id and "
                                     "token (64 hex chars) are required");
    }

    char controller_id[32] = {0};
    uint32_t permissions = 0;
    const esp_err_t err = michi_pairing_confirm(challenge, initiator_id,
                                                token_hex, controller_id,
                                                sizeof(controller_id),
                                                &permissions);
    switch (err) {
    case ESP_OK: {
        cJSON *resp = cJSON_CreateObject();
        if (resp == NULL) {
            return ESP_ERR_NO_MEM;
        }
        esp_err_t send_err = ESP_OK;
        if (cJSON_AddStringToObject(resp, "status", "paired") == NULL ||
            cJSON_AddStringToObject(resp, "controller_id", controller_id) == NULL ||
            cJSON_AddNumberToObject(resp, "permissions", permissions) == NULL) {
            send_err = ESP_ERR_NO_MEM;
        } else {
            send_err = michi_http_send_json(req, 200, resp);
        }
        cJSON_Delete(resp);
        return send_err;
    }
    case ESP_ERR_INVALID_STATE:
        /* Window closed / no challenge issued / initiator != owner. */
        return michi_http_send_error(req, 409, "pairing_window_closed",
                                     "no pairing window open, no challenge "
                                     "issued, or initiator_id mismatch");
    case ESP_ERR_INVALID_ARG:
        return michi_http_send_error(req, 400, "invalid_request",
                                     "malformed challenge, initiator_id or "
                                     "token");
    case ESP_ERR_NOT_FOUND:
        return michi_http_send_error(req, 409, "pairing_proof_mismatch",
                                     "the challenge proof did not match");
    case ESP_ERR_TIMEOUT:
        return michi_http_send_error(req, 409, "pairing_attempts_exhausted",
                                     "confirmation attempts exhausted for "
                                     "this window");
    default:
        /* NO_MEM (registry full) and NVS errors. */
        return michi_http_send_error(req, 500, "internal_error",
                                     esp_err_to_name(err));
    }
}

/* GET /api/v1/receiver/controllers (Bearer STATUS): the registry as a
 * JSON array of ids - no secrets, no digests (michi_pairing_list
 * contract). */
static esp_err_t controllers_get_handler(httpd_req_t *req)
{
    char controller_id[32] = {0};
    if (!auth_gate(req, MICHI_PERM_STATUS, controller_id,
                     sizeof(controller_id))) {
        return ESP_OK; /* 401/403 already sent (P0-5) */
    }
    char list[CONFIG_MICHI_PAIRING_MAX_CONTROLLERS * 32 + 8] = {0};
    const esp_err_t err = michi_pairing_list(list, sizeof(list));
    if (err != ESP_OK) {
        return michi_http_send_error(req, 500, "internal_error",
                                     esp_err_to_name(err));
    }
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON *arr = cJSON_AddArrayToObject(root, "controllers");
    if (arr == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    char *save = NULL;
    for (char *tok = strtok_r(list, ",", &save); tok != NULL;
         tok = strtok_r(NULL, ",", &save)) {
        cJSON *item = cJSON_CreateString(tok);
        if (item == NULL || !cJSON_AddItemToArray(arr, item)) {
            if (item != NULL) {
                cJSON_Delete(item);
            }
            cJSON_Delete(root);
            return ESP_ERR_NO_MEM;
        }
    }
    esp_err_t send_err = michi_http_send_json(req, 200, root);
    cJSON_Delete(root);
    return send_err;
}

/* DELETE /api/v1/receiver/controllers/{id} (Bearer CONTROLLER_ADMIN):
 * individual revocation. The id is parsed from the matched URI
 * (wildcard endpoint); anything but a bare id after the prefix is
 * rejected. */
static esp_err_t controller_delete_handler(httpd_req_t *req)
{
    char controller_id[32] = {0};
    if (!auth_gate(req, MICHI_PERM_CONTROLLER_ADMIN, controller_id,
                     sizeof(controller_id))) {
        return ESP_OK; /* 401/403 already sent (P0-5) */
    }
    /* req->uri is a fixed array (HTTPD_MAX_URI_LEN + 1): never NULL. */
    const char *uri = req->uri;
    const size_t prefix_len = strlen(MICHI_HTTP_CONTROLLERS_PREFIX);
    if (strncmp(uri, MICHI_HTTP_CONTROLLERS_PREFIX, prefix_len) != 0) {
        return michi_http_send_error(req, 400, "invalid_request",
                                     "malformed controller path");
    }
    const char *id = uri + prefix_len;
    if (!id_valid_chars(id)) {
        return michi_http_send_error(req, 400, "invalid_request",
                                     "controller id must be 1..31 chars, "
                                     "alphanumeric + '-'");
    }
    const esp_err_t err = michi_pairing_revoke(id);
    switch (err) {
    case ESP_OK: {
        cJSON *root = cJSON_CreateObject();
        if (root == NULL) {
            return ESP_ERR_NO_MEM;
        }
        esp_err_t send_err = ESP_OK;
        if (cJSON_AddStringToObject(root, "status", "revoked") == NULL ||
            cJSON_AddStringToObject(root, "controller_id", id) == NULL) {
            send_err = ESP_ERR_NO_MEM;
        } else {
            send_err = michi_http_send_json(req, 200, root);
        }
        cJSON_Delete(root);
        return send_err;
    }
    case ESP_ERR_NOT_FOUND:
        return michi_http_send_error(req, 404, "controller_not_found",
                                     "no controller with that id");
    case ESP_ERR_INVALID_ARG:
        return michi_http_send_error(req, 400, "invalid_request",
                                     "malformed controller id");
    default:
        return michi_http_send_error(req, 500, "internal_error",
                                     esp_err_to_name(err));
    }
}

/* Append the session info (WITHOUT the session token: it is a credential
 * returned only at creation) to an existing response tree. */
static esp_err_t add_session_info_json(cJSON *root,
                                       const michi_session_info_t *info)
{
    if (cJSON_AddStringToObject(root, "session_id", info->session_id) == NULL ||
        cJSON_AddStringToObject(root, "owner_controller_id",
                                info->owner_controller_id) == NULL ||
        cJSON_AddNumberToObject(root, "ssrc", (double)info->ssrc) == NULL ||
        cJSON_AddStringToObject(root, "source_addr", info->source_addr) == NULL ||
        cJSON_AddStringToObject(root, "codec", info->codec) == NULL ||
        cJSON_AddNumberToObject(root, "sample_rate", (double)info->sample_rate) == NULL ||
        cJSON_AddNumberToObject(root, "bit_depth", info->bit_depth) == NULL ||
        cJSON_AddNumberToObject(root, "channels", info->channels) == NULL ||
        cJSON_AddNumberToObject(root, "stream_port", info->stream_port) == NULL ||
        cJSON_AddNumberToObject(root, "buffer_ms", info->buffer_ms) == NULL ||
        cJSON_AddNumberToObject(root, "volume", info->volume) == NULL ||
        cJSON_AddBoolToObject(root, "paused", info->paused) == NULL) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* POST /api/v1/receiver/sessions (Bearer PLAYBACK) and the v1-lite
 * POST /api/v1/receiver/session/start. Body: codec, sample_rate,
 * bit_depth, channels, stream_port (required); buffer_ms (default 250),
 * volume (default: current) optional; the v1-lite "transport" must be
 * "udp" when present; a client-provided "session_id" is IGNORED (the
 * receiver generates the id - single-session identity). The owner is
 * ALWAYS the authenticated controller id: a body owner claim would be
 * spoofable, so it is overridden. The response carries the session token
 * - the ONLY time it is ever transmitted. */
static esp_err_t session_start_handler(httpd_req_t *req)
{
    char owner[32] = {0};
    if (!auth_gate(req, MICHI_PERM_PLAYBACK, owner, sizeof(owner))) {
        return ESP_OK; /* 401/403 already sent (P0-5) */
    }
    /* OTA gate (phase 13): no session may start while an update is in
     * flight (the update task force-closes the active session and the
     * FSM lands on UPDATING; the session layer's own defensive gate is
     * a second line). */
    if (michi_ota_busy()) {
        return michi_http_send_error(req, 409, "ota_in_progress",
                                     "a firmware update is in progress");
    }
    cJSON *root = read_json_body(req);
    if (root == NULL) {
        return ESP_OK; /* 400 already sent (P0-5) */
    }
    char codec[16] = {0};
    char transport[8] = {0};
    int sample_rate = 0, bit_depth = 0, channels = 0, stream_port = 0;
    int buffer_ms = 250, volume = -1;
    const bool have_codec = michi_http_json_get_string(root, "codec", codec,
                                                       sizeof(codec));
    const bool have_rate = michi_http_json_get_int(root, "sample_rate",
                                                   &sample_rate);
    const bool have_depth = michi_http_json_get_int(root, "bit_depth",
                                                    &bit_depth);
    const bool have_channels = michi_http_json_get_int(root, "channels",
                                                       &channels);
    const bool have_port = michi_http_json_get_int(root, "stream_port",
                                                   &stream_port);
    /* buffer_ms: optional, default 250 (read into the initialized value). */
    const bool have_buffer_ms = michi_http_json_get_int(root, "buffer_ms",
                                                        &buffer_ms);
    const bool have_volume = michi_http_json_get_int(root, "volume", &volume);
    /* v1-lite compatibility: "transport" must be "udp" when present. */
    const bool have_transport = michi_http_json_get_string(root, "transport",
                                                           transport,
                                                           sizeof(transport));
    cJSON_Delete(root);

    if (!have_codec || !have_rate || !have_depth || !have_channels ||
        !have_port) {
        return michi_http_send_error(req, 400, "invalid_request",
                                     "codec, sample_rate, bit_depth, "
                                     "channels and stream_port are required");
    }
    if (have_transport && strcmp(transport, "udp") != 0) {
        return michi_http_send_error(req, 400, "unsupported_transport",
                                     "only udp is supported (meta 1)");
    }
    /* F1: range-validate the ints BEFORE the narrowing casts below -
     * stream_port 70000 or -1 must never wrap into a uint16_t. Each
     * out-of-range field answers 400 naming it. bit_depth/channels are
     * allowed beyond meta 1 (24-bit/1ch are declared metas): the engine
     * rejects them downstream as unsupported_format. */
    if (stream_port < 1024 || stream_port > 65535) {
        return michi_http_send_error(req, 400, "invalid_request",
                                     "stream_port must be in 1024-65535");
    }
    if (bit_depth != 16 && bit_depth != 24) {
        return michi_http_send_error(req, 400, "invalid_request",
                                     "bit_depth must be 16 or 24");
    }
    if (channels != 1 && channels != 2) {
        return michi_http_send_error(req, 400, "invalid_request",
                                     "channels must be 1 or 2");
    }
    if (have_volume && (volume < 0 || volume > 100)) {
        return michi_http_send_error(req, 400, "invalid_request",
                                     "volume must be in 0-100");
    }
    if (have_buffer_ms &&
        (buffer_ms < 50 || buffer_ms > CONFIG_MICHI_AUDIO_JITTER_MAX_MS)) {
        char msg[48];
        snprintf(msg, sizeof(msg), "buffer_ms must be in 50-%d",
                 CONFIG_MICHI_AUDIO_JITTER_MAX_MS);
        return michi_http_send_error(req, 400, "invalid_request", msg);
    }
    if (!have_volume) {
        volume = michi_volume_get(); /* default: keep the current level */
    }

    char session_token[MICHI_SESSION_TOKEN_LEN] = {0};
    const esp_err_t err = michi_session_start(
        owner, codec, (uint32_t)sample_rate, (uint8_t)bit_depth,
        (uint8_t)channels, (uint16_t)stream_port, (uint16_t)buffer_ms,
        (uint8_t)volume, session_token, sizeof(session_token));
    switch (err) {
    case ESP_OK: {
        michi_session_info_t info;
        if (michi_session_get_info(&info) != ESP_OK) {
            return michi_http_send_error(req, 500, "internal_error",
                                         "session started but info is "
                                         "unavailable");
        }
        cJSON *resp = cJSON_CreateObject();
        if (resp == NULL) {
            return ESP_ERR_NO_MEM;
        }
        esp_err_t send_err = ESP_OK;
        if (cJSON_AddStringToObject(resp, "status", "session_started") == NULL ||
            cJSON_AddStringToObject(resp, "session_token", session_token) == NULL ||
            add_session_info_json(resp, &info) != ESP_OK) {
            send_err = ESP_ERR_NO_MEM;
        } else {
            send_err = michi_http_send_json(req, 200, resp);
        }
        cJSON_Delete(resp);
        return send_err;
    }
    case ESP_ERR_INVALID_STATE:
        if (michi_session_active()) {
            return michi_http_send_error(req, 409, "session_active",
                                         "a session is already active; stop "
                                         "it first");
        }
        if (!michi_session_is_initialized()) {
            /* F7: never blame the DAC for a missing session layer (init
             * failed at boot -> app_main continued degraded). */
            ESP_LOGW(TAG, "session/start: session layer unavailable "
                          "(michi_session_init failed)");
            return michi_http_send_error(req, 409, "audio_unavailable",
                                         "the session layer is unavailable");
        }
        return michi_http_send_error(req, 409, "audio_unavailable",
                                     "the audio pipeline is not running "
                                     "(no DAC)");
    case ESP_ERR_NOT_SUPPORTED:
        return michi_http_send_error(req, 400, "unsupported_format",
                                     "codec/sample_rate/bit_depth/channels "
                                     "outside meta 1 (pcm_s16le 48000/16/2)");
    case ESP_ERR_INVALID_ARG:
        return michi_http_send_error(req, 400, "invalid_request",
                                     "malformed session parameters "
                                     "(stream_port must be 1024-65535)");
    default:
        return michi_http_send_error(req, 500, "internal_error",
                                     esp_err_to_name(err));
    }
}

/* GET /api/v1/receiver/sessions/current (Bearer STATUS): the session
 * snapshot WITHOUT the session token (a credential: issued once at
 * creation, never re-exposed). */
static esp_err_t session_current_get_handler(httpd_req_t *req)
{
    char controller_id[32] = {0};
    if (!auth_gate(req, MICHI_PERM_STATUS, controller_id,
                     sizeof(controller_id))) {
        return ESP_OK; /* 401/403 already sent (P0-5) */
    }
    michi_session_info_t info;
    const esp_err_t err = michi_session_get_info(&info);
    if (err == ESP_ERR_INVALID_STATE) {
        return michi_http_send_error(req, 404, "no_active_session",
                                     "no session is active");
    }
    if (err != ESP_OK) {
        return michi_http_send_error(req, 500, "internal_error",
                                     esp_err_to_name(err));
    }
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t send_err = add_session_info_json(root, &info);
    if (send_err == ESP_OK) {
        send_err = michi_http_send_json(req, 200, root);
    }
    cJSON_Delete(root);
    return send_err;
}

/* Extract the session token: body "session_token" (PATCH) first, else the
 * X-Michi-Session header (PATCH/DELETE). Empty on absence. */
static void get_session_token(httpd_req_t *req, const cJSON *body,
                              char *out, size_t out_len)
{
    out[0] = '\0';
    if (body != NULL) {
        char from_body[MICHI_SESSION_TOKEN_LEN] = {0};
        if (michi_http_json_get_string(body, "session_token", from_body,
                                       sizeof(from_body))) {
            strlcpy(out, from_body, out_len);
            return;
        }
    }
    char from_header[MICHI_SESSION_TOKEN_LEN] = {0};
    if (httpd_req_get_hdr_value_str(req, MICHI_HTTP_SESSION_TOKEN_HEADER,
                                    from_header,
                                    sizeof(from_header)) == ESP_OK) {
        strlcpy(out, from_header, out_len);
    }
}

/* PATCH /api/v1/receiver/sessions/current (Bearer PLAYBACK): volume
 * and/or pause. The session token comes from the body or the
 * X-Michi-Session header. The response carries the CURRENT info with the
 * APPLIED volume (P0-12: never the request value). */
static esp_err_t session_patch_handler(httpd_req_t *req)
{
    char controller_id[32] = {0};
    if (!auth_gate(req, MICHI_PERM_PLAYBACK, controller_id,
                     sizeof(controller_id))) {
        return ESP_OK; /* 401/403 already sent (P0-5) */
    }
    cJSON *root = read_json_body(req);
    if (root == NULL) {
        return ESP_OK; /* 400 already sent (P0-5) */
    }
    char session_token[MICHI_SESSION_TOKEN_LEN] = {0};
    get_session_token(req, root, session_token, sizeof(session_token));
    int volume = -1;
    bool paused = false;
    bool have_paused = michi_http_json_get_bool(root, "paused", &paused);
    const bool have_volume = michi_http_json_get_int(root, "volume", &volume);
    cJSON_Delete(root);

    if (session_token[0] == '\0') {
        return michi_http_send_error(req, 401, "invalid_session_token",
                                     "the session token (body or "
                                     "X-Michi-Session header) is required");
    }
    if (!michi_session_active()) {
        return michi_http_send_error(req, 404, "no_active_session",
                                     "no session is active");
    }

    const esp_err_t err = michi_session_patch(
        session_token, have_volume ? volume : -1,
        have_paused ? &paused : NULL);
    switch (err) {
    case ESP_OK: {
        michi_session_info_t info;
        if (michi_session_get_info(&info) != ESP_OK) {
            return michi_http_send_error(req, 500, "internal_error",
                                         "session info unavailable");
        }
        cJSON *resp = cJSON_CreateObject();
        if (resp == NULL) {
            return ESP_ERR_NO_MEM;
        }
        esp_err_t send_err = add_session_info_json(resp, &info);
        if (send_err == ESP_OK) {
            send_err = michi_http_send_json(req, 200, resp);
        }
        cJSON_Delete(resp);
        return send_err;
    }
    case ESP_ERR_NOT_FOUND:
        return michi_http_send_error(req, 401, "invalid_session_token",
                                     "the session token does not match the "
                                     "active session");
    case ESP_ERR_INVALID_ARG:
        return michi_http_send_error(req, 400, "invalid_request",
                                     "malformed session token");
    case ESP_ERR_INVALID_STATE:
        if (michi_session_active()) {
            return michi_http_send_error(req, 500, "session_operation_failed",
                                         "the audio engine rejected the "
                                         "operation");
        }
        return michi_http_send_error(req, 404, "no_active_session",
                                     "no session is active");
    default:
        return michi_http_send_error(req, 500, "session_operation_failed",
                                     esp_err_to_name(err));
    }
}

/* DELETE /api/v1/receiver/sessions/current (Bearer PLAYBACK): stop the
 * session. The session token arrives in the X-Michi-Session header
 * (DELETE bodies are not part of the contract). */
static esp_err_t session_delete_handler(httpd_req_t *req)
{
    char controller_id[32] = {0};
    if (!auth_gate(req, MICHI_PERM_PLAYBACK, controller_id,
                     sizeof(controller_id))) {
        return ESP_OK; /* 401/403 already sent (P0-5) */
    }
    char session_token[MICHI_SESSION_TOKEN_LEN] = {0};
    get_session_token(req, NULL, session_token, sizeof(session_token));
    if (session_token[0] == '\0') {
        return michi_http_send_error(req, 401, "invalid_session_token",
                                     "the X-Michi-Session header is required");
    }
    if (!michi_session_active()) {
        return michi_http_send_error(req, 404, "no_active_session",
                                     "no session is active");
    }
    const esp_err_t err = michi_session_stop(session_token);
    switch (err) {
    case ESP_OK: {
        cJSON *root = cJSON_CreateObject();
        if (root == NULL) {
            return ESP_ERR_NO_MEM;
        }
        esp_err_t send_err = ESP_OK;
        if (cJSON_AddStringToObject(root, "status", "session_stopped") == NULL) {
            send_err = ESP_ERR_NO_MEM;
        } else {
            send_err = michi_http_send_json(req, 200, root);
        }
        cJSON_Delete(root);
        return send_err;
    }
    case ESP_ERR_NOT_FOUND:
        return michi_http_send_error(req, 401, "invalid_session_token",
                                     "the session token does not match the "
                                     "active session");
    case ESP_ERR_INVALID_ARG:
        return michi_http_send_error(req, 400, "invalid_request",
                                     "malformed session token");
    case ESP_ERR_TIMEOUT:
        return michi_http_send_error(req, 500, "session_stop_failed",
                                     "the audio engine did not stop in "
                                     "time; retry");
    default:
        return michi_http_send_error(req, 500, "internal_error",
                                     esp_err_to_name(err));
    }
}

/* v1-lite POST /api/v1/receiver/session/stop (Bearer PLAYBACK): the
 * legacy path mapped onto michi_session_stop with the SAME security. The
 * body carries the server-issued session_id (validated against the
 * active session, 404 on mismatch) and the session token (body or
 * X-Michi-Session header). */
static esp_err_t v1lite_session_stop_handler(httpd_req_t *req)
{
    char controller_id[32] = {0};
    if (!auth_gate(req, MICHI_PERM_PLAYBACK, controller_id,
                     sizeof(controller_id))) {
        return ESP_OK; /* 401/403 already sent (P0-5) */
    }
    cJSON *root = read_json_body(req);
    if (root == NULL) {
        return ESP_OK; /* 400 already sent (P0-5) */
    }
    char session_id[32] = {0};
    char session_token[MICHI_SESSION_TOKEN_LEN] = {0};
    get_session_token(req, root, session_token, sizeof(session_token));
    const bool have_id = michi_http_json_get_string(root, "session_id",
                                                    session_id,
                                                    sizeof(session_id));
    cJSON_Delete(root);

    if (session_token[0] == '\0') {
        return michi_http_send_error(req, 401, "invalid_session_token",
                                     "the session token (body or "
                                     "X-Michi-Session header) is required");
    }
    if (!michi_session_active()) {
        return michi_http_send_error(req, 404, "no_active_session",
                                     "no session is active");
    }
    if (have_id) {
        michi_session_info_t info;
        const esp_err_t info_err = michi_session_get_info(&info);
        if (info_err != ESP_OK) {
            /* F6: an unreadable session (dead-engine cleanup just ran)
             * cannot be validated - answer 404 explicitly instead of
             * swallowing the failure and proceeding to stop(). */
            ESP_LOGW(TAG, "session/stop: cannot validate session_id "
                          "(get_info=%s)", esp_err_to_name(info_err));
            return michi_http_send_error(req, 404, "session_not_found",
                                         "the active session cannot be "
                                         "validated");
        }
        if (strcmp(session_id, info.session_id) != 0) {
            return michi_http_send_error(req, 404, "session_not_found",
                                         "the session_id does not match the "
                                         "active session");
        }
    }
    const esp_err_t err = michi_session_stop(session_token);
    switch (err) {
    case ESP_OK: {
        cJSON *resp = cJSON_CreateObject();
        if (resp == NULL) {
            return ESP_ERR_NO_MEM;
        }
        esp_err_t send_err = ESP_OK;
        if (cJSON_AddStringToObject(resp, "status", "session_stopped") == NULL) {
            send_err = ESP_ERR_NO_MEM;
        } else {
            send_err = michi_http_send_json(req, 200, resp);
        }
        cJSON_Delete(resp);
        return send_err;
    }
    case ESP_ERR_NOT_FOUND:
        return michi_http_send_error(req, 401, "invalid_session_token",
                                     "the session token does not match the "
                                     "active session");
    case ESP_ERR_INVALID_ARG:
        return michi_http_send_error(req, 400, "invalid_request",
                                     "malformed session token");
    case ESP_ERR_TIMEOUT:
        return michi_http_send_error(req, 500, "session_stop_failed",
                                     "the audio engine did not stop in "
                                     "time; retry");
    default:
        return michi_http_send_error(req, 500, "internal_error",
                                     esp_err_to_name(err));
    }
}

/* v1-lite POST /api/v1/receiver/heartbeat (Bearer STATUS): liveness
 * probe. The active session_id is the SERVER truth (a client-provided id
 * that does not match is not an error - the response lets the client
 * discover the real id). No heartbeat timeout exists in phase 12 (the
 * v1-lite 90 s auto-stop is NOT implemented): a session stays until
 * explicitly stopped or the device reboots. */
static esp_err_t v1lite_heartbeat_handler(httpd_req_t *req)
{
    char controller_id[32] = {0};
    if (!auth_gate(req, MICHI_PERM_STATUS, controller_id,
                     sizeof(controller_id))) {
        return ESP_OK; /* 401/403 already sent (P0-5) */
    }
    /* The body is optional: tolerate a missing Content-Length. */
    char body[MICHI_HTTP_BODY_MAX];
    size_t body_len = 0;
    if (michi_http_read_body(req, body, sizeof(body), &body_len) == ESP_OK &&
        body_len > 0) {
        /* Parsed and discarded on purpose: heartbeat bodies carry no
         * authoritative data (the session_id in the response is the
         * server's). */
        cJSON *parsed = cJSON_Parse(body);
        if (parsed != NULL) {
            cJSON_Delete(parsed);
        }
    }
    char session_id[MICHI_SESSION_ID_LEN] = "";
    michi_session_info_t info;
    if (michi_session_get_info(&info) == ESP_OK) {
        strlcpy(session_id, info.session_id, sizeof(session_id));
    }
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t send_err = ESP_OK;
    if (cJSON_AddStringToObject(root, "status", "alive") == NULL ||
        cJSON_AddStringToObject(root, "session_id", session_id) == NULL ||
        cJSON_AddNumberToObject(root, "uptime_seconds",
                                (double)(uint64_t)(esp_timer_get_time() / 1000000)) == NULL) {
        send_err = ESP_ERR_NO_MEM;
    } else {
        send_err = michi_http_send_json(req, 200, root);
    }
    cJSON_Delete(root);
    return send_err;
}

/* v1-lite POST /api/v1/receiver/volume (Bearer VOLUME): clamp and report
 * the REAL applied value (P0-12 - never the request payload). */
static esp_err_t v1lite_volume_handler(httpd_req_t *req)
{
    char controller_id[32] = {0};
    if (!auth_gate(req, MICHI_PERM_VOLUME, controller_id,
                     sizeof(controller_id))) {
        return ESP_OK; /* 401/403 already sent (P0-5) */
    }
    cJSON *root = read_json_body(req);
    if (root == NULL) {
        return ESP_OK; /* 400 already sent (P0-5) */
    }
    int volume = 0;
    const bool have_volume = michi_http_json_get_int(root, "volume", &volume);
    cJSON_Delete(root);
    if (!have_volume) {
        return michi_http_send_error(req, 400, "invalid_request",
                                     "volume (integer) is required");
    }
    if (volume < 0) {
        volume = 0;
    } else if (volume > 100) {
        volume = 100;
    }
    michi_volume_set((uint8_t)volume);
    cJSON *resp = cJSON_CreateObject();
    if (resp == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t send_err = ESP_OK;
    if (cJSON_AddStringToObject(resp, "status", "volume_set") == NULL ||
        cJSON_AddNumberToObject(resp, "volume", michi_volume_get()) == NULL) {
        send_err = ESP_ERR_NO_MEM;
    } else {
        send_err = michi_http_send_json(req, 200, resp);
    }
    cJSON_Delete(resp);
    return send_err;
}

/* PUT /api/v1/receiver/now-playing (Bearer PLAYBACK): display metadata.
 * The display subsystem copies/truncates to its own limits; the API
 * validates the SAME limits and rejects oversize values with 400 (never
 * silently truncate, P0-4 philosophy). All fields are optional; the
 * display renders "--" for missing ones. No session is required: the
 * metadata is accepted and shown whenever the display is up. */
static esp_err_t now_playing_put_handler(httpd_req_t *req)
{
    char controller_id[32] = {0};
    if (!auth_gate(req, MICHI_PERM_PLAYBACK, controller_id,
                     sizeof(controller_id))) {
        return ESP_OK; /* 401/403 already sent (P0-5) */
    }
    cJSON *root = read_json_body(req);
    if (root == NULL) {
        return ESP_OK; /* 400 already sent (P0-5) */
    }
    char source[MICHI_DISPLAY_SOURCE_MAX + 1] = "";
    char title[MICHI_DISPLAY_TITLE_MAX + 1] = "";
    char artist[MICHI_DISPLAY_ARTIST_MAX + 1] = "";
    const bool have_source = michi_http_json_get_string(root, "source",
                                                        source,
                                                        sizeof(source));
    const bool have_title = michi_http_json_get_string(root, "title",
                                                       title, sizeof(title));
    const bool have_artist = michi_http_json_get_string(root, "artist",
                                                        artist,
                                                        sizeof(artist));
    /* F3: an oversize value is a 400 (the display limits ARE the
     * contract), never a silent 200 - checked before the tree is
     * deleted because get_string fails for BOTH missing and oversize. */
    if (json_value_too_long(root, "source", sizeof(source)) ||
        json_value_too_long(root, "title", sizeof(title)) ||
        json_value_too_long(root, "artist", sizeof(artist))) {
        cJSON_Delete(root);
        return michi_http_send_error(req, 400, "invalid_request",
                                     "field too long");
    }
    cJSON_Delete(root);

    /* A value that does not fit its display limit is a 400 (checked
     * helper: get_string fails instead of truncating). */
    const esp_err_t err = michi_display_update_now_playing(
        have_source ? source : NULL,
        have_title ? title : NULL,
        have_artist ? artist : NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "now-playing: display update failed: %s "
                      "(metadata accepted anyway)",
                 esp_err_to_name(err));
    }
    cJSON *resp = cJSON_CreateObject();
    if (resp == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t send_err = ESP_OK;
    if (cJSON_AddStringToObject(resp, "status", "now_playing_updated") == NULL) {
        send_err = ESP_ERR_NO_MEM;
    } else {
        send_err = michi_http_send_json(req, 200, resp);
    }
    cJSON_Delete(resp);
    return send_err;
}

/* OTA state name for the API responses (diagnostics + updates 202). */
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

/* GET /api/v1/receiver/diagnostics (Bearer STATUS): uptime, heap, PSRAM,
 * Wi-Fi link, audio metrics, DAC state. Diagnostic data only - no
 * secrets (the SSID is a network name, fine; the password is never
 * touched). */
static esp_err_t diagnostics_get_handler(httpd_req_t *req)
{
    char controller_id[32] = {0};
    if (!auth_gate(req, MICHI_PERM_STATUS, controller_id,
                     sizeof(controller_id))) {
        return ESP_OK; /* 401/403 already sent (P0-5) */
    }
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
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
    return send_err;
}

/* POST /api/v1/receiver/updates (Bearer OTA, phase 13): body {url} where
 * url points at a SIGNED manifest. michi_ota_start() validates the URL
 * synchronously and spawns the OTA task; the handler answers 202 with the
 * resulting OTA state. Errors: 409 ota_in_progress when an update is
 * already running (checked in the handler before the body is read, plus
 * the atomic defensive gate inside michi_ota_start), 409 pending_verify
 * when the running image is awaiting the boot self-test, 400 for a
 * rejected URL/body, 500 on allocation or internal failures. */
static esp_err_t updates_post_handler(httpd_req_t *req)
{
    char controller_id[32] = {0};
    if (!auth_gate(req, MICHI_PERM_OTA, controller_id,
                     sizeof(controller_id))) {
        return ESP_OK; /* 401/403 already sent (P0-5) */
    }
    if (michi_ota_busy()) {
        return michi_http_send_error(req, 409, "ota_in_progress",
                                     "a firmware update is already in "
                                     "progress");
    }
    cJSON *root = read_json_body(req);
    if (root == NULL) {
        return ESP_OK; /* 400 already sent (P0-5) */
    }
    char url[CONFIG_MICHI_OTA_URL_MAX + 1] = {0};
    const bool have_url = michi_http_json_get_string(root, "url", url,
                                                     sizeof(url));
    cJSON_Delete(root);
    if (!have_url) {
        return michi_http_send_error(req, 400, "invalid_request",
                                     "url is required");
    }
    const esp_err_t err = michi_ota_start(url);
    switch (err) {
    case ESP_OK: {
        michi_ota_state_t st;
        int percent = 0;
        if (michi_ota_get_state(&st, &percent, NULL, 0) != ESP_OK) {
            st = MICHI_OTA_IDLE;
        }
        cJSON *resp = cJSON_CreateObject();
        if (resp == NULL) {
            return ESP_ERR_NO_MEM;
        }
        esp_err_t send_err = ESP_OK;
        if (cJSON_AddStringToObject(resp, "status", "ota_started") == NULL ||
            cJSON_AddStringToObject(resp, "state", ota_state_name(st)) == NULL ||
            cJSON_AddNumberToObject(resp, "percent", percent) == NULL) {
            send_err = ESP_ERR_NO_MEM;
        } else {
            send_err = michi_http_send_json(req, 202, resp);
        }
        cJSON_Delete(resp);
        return send_err;
    }
    case ESP_ERR_INVALID_STATE:
        return michi_http_send_error(req, 409, "ota_in_progress",
                                     "an update is already in progress");
    case ESP_ERR_NOT_ALLOWED:
        return michi_http_send_error(req, 409, "pending_verify",
                                     "firmware is awaiting the boot "
                                     "self-test before the next update");
    case ESP_ERR_INVALID_ARG:
        return michi_http_send_error(req, 400, "invalid_request",
                                     "url must be https:// with a non-empty "
                                     "host and no userinfo");
    case ESP_ERR_NO_MEM:
        return michi_http_send_error(req, 500, "internal_error",
                                     "out of memory starting the update");
    default:
        return michi_http_send_error(req, 500, "internal_error",
                                     esp_err_to_name(err));
    }
}

static const httpd_uri_t s_endpoints[] = {
    /* Phase 4 (no auth) */
    {.uri = "/api/v1/receiver/info",     .method = HTTP_GET, .handler = info_get_handler},
    {.uri = "/api/v1/receiver/firmware", .method = HTTP_GET, .handler = firmware_get_handler},
    /* Phase 12 */
    {.uri = "/api/v1/receiver/status",   .method = HTTP_GET, .handler = status_get_handler},
    {.uri = "/api/v1/receiver/pairing/challenge", .method = HTTP_POST, .handler = pairing_challenge_handler},
    {.uri = "/api/v1/receiver/pairing/confirm",   .method = HTTP_POST, .handler = pairing_confirm_handler},
    {.uri = "/api/v1/receiver/controllers",       .method = HTTP_GET,  .handler = controllers_get_handler},
    {.uri = "/api/v1/receiver/controllers/*",     .method = HTTP_DELETE, .handler = controller_delete_handler},
    {.uri = "/api/v1/receiver/sessions",          .method = HTTP_POST,  .handler = session_start_handler},
    {.uri = "/api/v1/receiver/sessions/current",  .method = HTTP_GET,   .handler = session_current_get_handler},
    {.uri = "/api/v1/receiver/sessions/current",  .method = HTTP_PATCH, .handler = session_patch_handler},
    {.uri = "/api/v1/receiver/sessions/current",  .method = HTTP_DELETE, .handler = session_delete_handler},
    {.uri = "/api/v1/receiver/now-playing",       .method = HTTP_PUT,   .handler = now_playing_put_handler},
    {.uri = "/api/v1/receiver/diagnostics",       .method = HTTP_GET,   .handler = diagnostics_get_handler},
    {.uri = "/api/v1/receiver/updates",           .method = HTTP_POST,  .handler = updates_post_handler},
    /* v1-lite compatibility (transition layer, same handlers/security) */
    {.uri = "/api/v1/receiver/pair/start",    .method = HTTP_POST, .handler = pairing_challenge_handler},
    {.uri = "/api/v1/receiver/pair/confirm",  .method = HTTP_POST, .handler = pairing_confirm_handler},
    {.uri = "/api/v1/receiver/session/start", .method = HTTP_POST, .handler = session_start_handler},
    {.uri = "/api/v1/receiver/session/stop",  .method = HTTP_POST, .handler = v1lite_session_stop_handler},
    {.uri = "/api/v1/receiver/heartbeat",     .method = HTTP_POST, .handler = v1lite_heartbeat_handler},
    {.uri = "/api/v1/receiver/volume",        .method = HTTP_POST, .handler = v1lite_volume_handler},
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
    /* F10: explicit stack size - the default 4096 is tight for the
     * 2048-byte body buffers plus the nested cJSON frames built by the
     * session/now-playing handlers. */
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
