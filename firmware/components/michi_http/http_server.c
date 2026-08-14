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
 * canonical_json.c). The receiver-button pairing flow (MS-06) is
 * implemented: pair/start (physical 120 s window + Ed25519 challenge +
 * local PIN), pair/status and pair/confirm (receiver-issued token,
 * SHA-256 digest only in NVS, expires_in 0). The canonical RTP session
 * lifecycle (MS-07) is implemented: POST/GET/PATCH/DELETE
 * /receiver-lite/session with strict body gates, the receiver-picked UDP
 * stream port, the RAM-only 43-char base64url session token and the
 * exact state body. The heartbeat lease (MS-08) is implemented: POST
 * /receiver-lite/heartbeat renews the 30 s monotonic lease (strictly
 * increasing sequence; replay answers 409 without renewing; the session
 * watchdog closes at 30 s even while RTP flows). The certified
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
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "cJSON.h"

#include "michi_audio.h"
#include "michi_audio_output.h"
#include "michi_dac.h"
#include "michi_discovery.h"
#include "michi_http.h"
#include "michi_identity.h"
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

#define MICHI_HTTP_AUTH_HEADER_MAX 96   /* "Bearer " + 43 base64url + NUL */

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
             (unsigned int)(b & 0xFFFFu),          /* time_mid */
             (unsigned int)((b >> 16) & 0xFFFu),   /* version 4 + time_hi */
             (unsigned int)((c & 0x3FFFu) | 0x8000u), /* variant 10 + clock_seq */
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
        /* httpd_req_recv reports socket failures as positive sentinels
         * (HTTPD_SOCK_ERR_INVALID = 0x1002, HTTPD_SOCK_ERR_FAIL = 0x1003);
         * anything >= HTTPD_SOCK_ERR_TIMEOUT is an error, never a byte
         * count. Accepting them as bytes would corrupt the stack buffer
         * terminator below. */
        if (ret <= 0 || ret >= HTTPD_SOCK_ERR_TIMEOUT) {
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

/* Source IP of the request (pair/start rate-limit key). IPv4 only (the
 * device stack is IPv4); unknown/unparseable -> empty string (the caller
 * passes NULL and only the global limit applies). */
static void client_ip_str(httpd_req_t *req, char *out, size_t out_len)
{
    out[0] = '\0';
    struct sockaddr_storage ss;
    socklen_t ss_len = (socklen_t)sizeof(ss);
    const int fd = httpd_req_to_sockfd(req);
    if (fd < 0 || getpeername(fd, (struct sockaddr *)&ss, &ss_len) != 0) {
        return;
    }
    if (ss.ss_family != AF_INET) {
        return;
    }
    const struct sockaddr_in *s4 = (const struct sockaddr_in *)&ss;
    const uint32_t a = ntohl(s4->sin_addr.s_addr);
    snprintf(out, out_len, "%u.%u.%u.%u", (unsigned)((a >> 24) & 0xFFu),
             (unsigned)((a >> 16) & 0xFFu), (unsigned)((a >> 8) & 0xFFu),
             (unsigned)(a & 0xFFu));
}

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

/* POST /api/v1/pair/start (no auth; physical window): the canonical
 * RECEIVER_BUTTON flow (MS-06, contract section 2.3). The signature is
 * verified over the DECODED nonce and michi_id must correspond to
 * public_key (michi_pairing_start / michi_identity). The PIN is created
 * and shown locally by the pairing component - it is NEVER part of the
 * response. */
static esp_err_t pair_start_handler(httpd_req_t *req)
{
    char michi_id[MICHI_IDENTITY_MICHI_ID_LEN] = {0};
    char public_key[MICHI_IDENTITY_PUBLIC_KEY_B64_LEN] = {0};
    char nonce[MICHI_PAIRING_NONCE_B64_MAX] = {0};
    char signature[MICHI_IDENTITY_SIGNATURE_B64_LEN] = {0};
    char field[20] = {0};

    cJSON *root = read_json_body(req);
    if (root == NULL) {
        return ESP_OK; /* 400 already sent (P0-5) */
    }
    const bool body_ok = michi_http_json_get_pair_start(
        root, michi_id, sizeof(michi_id), public_key, sizeof(public_key),
        nonce, sizeof(nonce), signature, sizeof(signature), field,
        sizeof(field));
    cJSON_Delete(root);
    if (!body_ok) {
        /* Includes the explicit rejection of the legacy initiator_id /
         * client_token fields (400, details.field names them). */
        return michi_http_send_error(req, 400,
                                     "invalid pair/start request body",
                                     field);
    }

    michi_pairing_peer_t peer;
    strlcpy(peer.michi_id, michi_id, sizeof(peer.michi_id));
    strlcpy(peer.public_key, public_key, sizeof(peer.public_key));
    strlcpy(peer.challenge_nonce, nonce, sizeof(peer.challenge_nonce));
    strlcpy(peer.challenge_signature, signature,
            sizeof(peer.challenge_signature));

    char ip[MICHI_PAIRING_IP_MAX] = {0};
    client_ip_str(req, ip, sizeof(ip));

    char session_id[MICHI_PAIRING_SESSION_ID_LEN] = {0};
    char expires_at[MICHI_PAIRING_EXPIRES_AT_LEN] = {0};
    uint32_t attempts = 0;
    const michi_pairing_start_result_t result = michi_pairing_start(
        &peer, ip[0] != '\0' ? ip : NULL, session_id, sizeof(session_id),
        expires_at, sizeof(expires_at), &attempts);
    switch (result) {
    case MICHI_PAIRING_START_WINDOW_CLOSED:
        return michi_http_send_error(req, 403,
                                     "the physical pairing window is closed",
                                     NULL);
    case MICHI_PAIRING_START_INVALID:
        return michi_http_send_error(
            req, 400,
            "challenge signature or identity validation failed", NULL);
    case MICHI_PAIRING_START_RATE_LIMITED:
        return michi_http_send_error(
            req, 429, "too many pair/start requests for this window", NULL);
    case MICHI_PAIRING_START_INTERNAL:
        return michi_http_send_error(req, 500,
                                     "pairing is not available", NULL);
    case MICHI_PAIRING_START_OK:
        break;
    }

    /* The server identity group: required by pair-start-response. */
    char server_michi_id[MICHI_IDENTITY_MICHI_ID_LEN] = {0};
    uint8_t server_pk_raw[MICHI_IDENTITY_KEY_BYTES] = {0};
    char server_public_key[MICHI_IDENTITY_PUBLIC_KEY_B64_LEN] = {0};
    if (michi_identity_michi_id(server_michi_id, sizeof(server_michi_id)) !=
            ESP_OK ||
        michi_identity_public_key(server_pk_raw) != ESP_OK ||
        michi_identity_base64url_encode(server_pk_raw, sizeof(server_pk_raw),
                                        server_public_key,
                                        sizeof(server_public_key)) != ESP_OK) {
        return michi_http_send_error(req, 500,
                                     "server identity is not available",
                                     NULL);
    }

    cJSON *resp = cJSON_CreateObject();
    if (resp == NULL) {
        return michi_http_send_error(req, 500,
                                     "out of memory while building response",
                                     NULL);
    }
    esp_err_t err = ESP_OK;
    if (cJSON_AddStringToObject(resp, "session_id", session_id) == NULL ||
        cJSON_AddStringToObject(resp, "expires_at", expires_at) == NULL ||
        cJSON_AddNumberToObject(resp, "attempts_remaining",
                                (double)attempts) == NULL ||
        cJSON_AddStringToObject(resp, "server_michi_id",
                                server_michi_id) == NULL ||
        cJSON_AddStringToObject(resp, "server_public_key",
                                server_public_key) == NULL) {
        err = ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK) {
        err = michi_http_send_json(req, 201, resp);
    }
    cJSON_Delete(resp);
    if (err != ESP_OK) {
        return michi_http_send_error(req, 500,
                                     "failed to build pair/start response",
                                     NULL);
    }
    return ESP_OK;
}

/* GET /api/v1/pair/status?session_id=<uuid> (no auth): status is
 * pending/confirmed/expired/locked (MS-06, section 2.3). A locked
 * session reports the schema floor (1) for attempts_remaining. */
static esp_err_t pair_status_handler(httpd_req_t *req)
{
    char query[96] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        query[0] == '\0') {
        return michi_http_send_error(req, 400,
                                     "missing session_id query parameter",
                                     "session_id");
    }
    char session_id[MICHI_PAIRING_SESSION_ID_LEN] = {0};
    if (httpd_query_key_value(query, "session_id", session_id,
                              sizeof(session_id)) != ESP_OK ||
        !michi_pairing_uuid_valid(session_id)) {
        return michi_http_send_error(req, 400,
                                     "missing or malformed session_id query "
                                     "parameter",
                                     "session_id");
    }

    char status[12] = {0};
    char expires_at[MICHI_PAIRING_EXPIRES_AT_LEN] = {0};
    uint32_t attempts = 0;
    const michi_pairing_status_result_t result = michi_pairing_status(
        session_id, status, sizeof(status), expires_at, sizeof(expires_at),
        &attempts);
    if (result == MICHI_PAIRING_STATUS_NOT_FOUND) {
        return michi_http_send_error(req, 404,
                                     "the pairing session was not found",
                                     NULL);
    }

    cJSON *resp = cJSON_CreateObject();
    if (resp == NULL) {
        return michi_http_send_error(req, 500,
                                     "out of memory while building response",
                                     NULL);
    }
    /* pair-status.schema.json pins attempts_remaining to 1..5: a locked
     * (consumed) session reports the schema floor. */
    const double reported_attempts =
        attempts == 0 ? 1.0 : (double)attempts;
    esp_err_t err = ESP_OK;
    if (cJSON_AddStringToObject(resp, "session_id", session_id) == NULL ||
        cJSON_AddStringToObject(resp, "status", status) == NULL ||
        cJSON_AddStringToObject(resp, "expires_at", expires_at) == NULL ||
        cJSON_AddNumberToObject(resp, "attempts_remaining",
                                reported_attempts) == NULL) {
        err = ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK) {
        err = michi_http_send_json(req, 200, resp);
    }
    cJSON_Delete(resp);
    if (err != ESP_OK) {
        return michi_http_send_error(req, 500,
                                     "failed to build pair/status response",
                                     NULL);
    }
    return ESP_OK;
}

/* POST /api/v1/pair/confirm (no auth; pairing session): identity must be
 * exactly the pair/start one; five failed PIN attempts are allowed, the
 * sixth answers 429 and consumes the session; success returns the
 * receiver-issued token ONCE with expires_in 0 (MS-06, section 2.3).
 * The token and PIN are never logged. */
static esp_err_t pair_confirm_handler(httpd_req_t *req)
{
    char session_id[MICHI_PAIRING_SESSION_ID_LEN] = {0};
    char pin[MICHI_PAIRING_PIN_BUF_LEN] = {0};
    char michi_id[MICHI_IDENTITY_MICHI_ID_LEN] = {0};
    char public_key[MICHI_IDENTITY_PUBLIC_KEY_B64_LEN] = {0};
    char field[20] = {0};

    cJSON *root = read_json_body(req);
    if (root == NULL) {
        return ESP_OK; /* 400 already sent (P0-5) */
    }
    const bool body_ok = michi_http_json_get_pair_confirm(
        root, session_id, sizeof(session_id), pin, sizeof(pin), michi_id,
        sizeof(michi_id), public_key, sizeof(public_key), field,
        sizeof(field));
    cJSON_Delete(root);
    if (!body_ok) {
        return michi_http_send_error(req, 400,
                                     "invalid pair/confirm request body",
                                     field);
    }

    /* Fetch the server_id BEFORE confirming: the token is returned
     * exactly once, so a successful confirm must never be followed by a
     * response-building failure. */
    char server_id[MICHI_DISCOVERY_UUID_LEN] = {0};
    if (michi_discovery_get_server_id(server_id, sizeof(server_id)) !=
        ESP_OK) {
        return michi_http_send_error(req, 500,
                                     "server identity is not available",
                                     NULL);
    }

    char token[MICHI_PAIRING_TOKEN_B64_LEN] = {0};
    char device_id[MICHI_PAIRING_DEVICE_ID_LEN] = {0};
    const michi_pairing_confirm_result_t result = michi_pairing_confirm(
        session_id, pin, michi_id, public_key, token, sizeof(token),
        device_id, sizeof(device_id));
    switch (result) {
    case MICHI_PAIRING_CONFIRM_NOT_FOUND:
        return michi_http_send_error(
            req, 404, "the pairing session was not found or has expired",
            NULL);
    case MICHI_PAIRING_CONFIRM_INVALID:
        return michi_http_send_error(
            req, 400, "controller identity does not match the pairing "
                      "session",
            NULL);
    case MICHI_PAIRING_CONFIRM_PIN_MISMATCH:
        return michi_http_send_error(
            req, 401, "the PIN does not match this pairing session", NULL);
    case MICHI_PAIRING_CONFIRM_LOCKED:
        return michi_http_send_error(
            req, 429, "PIN attempts exceeded; the pairing session is "
                      "consumed",
            NULL);
    case MICHI_PAIRING_CONFIRM_CONFLICT:
        return michi_http_send_error(req, 409,
                                     "this pairing session has already "
                                     "been used",
                                     NULL);
    case MICHI_PAIRING_CONFIRM_INTERNAL:
        return michi_http_send_error(req, 500,
                                     "pairing is not available", NULL);
    case MICHI_PAIRING_CONFIRM_OK:
        break;
    }

    cJSON *resp = cJSON_CreateObject();
    if (resp == NULL) {
        return michi_http_send_error(req, 500,
                                     "out of memory while building response",
                                     NULL);
    }
    esp_err_t err = ESP_OK;
    if (cJSON_AddStringToObject(resp, "token", token) == NULL ||
        cJSON_AddNumberToObject(resp, "expires_in", 0) == NULL ||
        cJSON_AddStringToObject(resp, "device_id", device_id) == NULL ||
        cJSON_AddStringToObject(resp, "server_id", server_id) == NULL) {
        err = ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK) {
        err = michi_http_send_json(req, 200, resp);
    }
    cJSON_Delete(resp);
    if (err != ESP_OK) {
        return michi_http_send_error(req, 500,
                                     "failed to build pair/confirm response",
                                     NULL);
    }
    return ESP_OK;
}

/* Session state name for the canonical state body (contract 2.5). */
static const char *session_state_name(michi_session_state_t st)
{
    switch (st) {
    case MICHI_SESSION_STATE_STARTING: return "starting";
    case MICHI_SESSION_STATE_PLAYING:  return "playing";
    case MICHI_SESSION_STATE_PAUSED:   return "paused";
    case MICHI_SESSION_STATE_STOPPING: return "stopping";
    default:                           return "stopping";
    }
}

/* The canonical GET/PATCH session state body (receiver-session
 * .schema.json, state form): the session_token NEVER appears here. */
static esp_err_t send_session_state(httpd_req_t *req,
                                    const michi_session_info_t *info)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return michi_http_send_error(req, 500,
                                     "out of memory while building session "
                                     "state", NULL);
    }
    esp_err_t err = ESP_OK;
    if (cJSON_AddStringToObject(root, "session_id", info->session_id) == NULL ||
        cJSON_AddStringToObject(root, "state",
                                session_state_name(info->state)) == NULL ||
        cJSON_AddNumberToObject(root, "lease_remaining_ms",
                                (double)info->lease_remaining_ms) == NULL ||
        cJSON_AddNumberToObject(root, "volume", (double)info->volume) == NULL ||
        cJSON_AddBoolToObject(root, "paused", info->paused) == NULL ||
        cJSON_AddNumberToObject(root, "stream_port",
                                (double)info->stream_port) == NULL ||
        cJSON_AddNumberToObject(root, "ssrc", (double)info->ssrc) == NULL ||
        cJSON_AddNumberToObject(root, "packets_received",
                                (double)info->packets_received) == NULL ||
        cJSON_AddNumberToObject(root, "packets_rejected",
                                (double)info->packets_rejected) == NULL ||
        cJSON_AddNumberToObject(root, "packets_lost",
                                (double)info->packets_lost) == NULL ||
        cJSON_AddNumberToObject(root, "underruns",
                                (double)info->underruns) == NULL) {
        err = ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK) {
        err = michi_http_send_json(req, 200, root);
    }
    cJSON_Delete(root);
    if (err != ESP_OK) {
        return michi_http_send_error(req, 500,
                                     "failed to build session state", NULL);
    }
    return ESP_OK;
}

/* The X-Michi-Session header (contract 2.4): PATCH/DELETE require it.
 * Missing/malformed answers 401 UNAUTHORIZED - the header value is a
 * credential, never logged. */
static esp_err_t read_session_token(httpd_req_t *req, char *out,
                                    size_t out_len)
{
    if (httpd_req_get_hdr_value_str(req, "X-Michi-Session", out,
                                    out_len) != ESP_OK ||
        !michi_session_token_valid(out)) {
        return michi_http_send_error(req, 401,
                                     "missing or invalid session token",
                                     NULL);
    }
    return ESP_OK;
}

/* POST /api/v1/receiver-lite/session (Bearer): the canonical session
 * creation (contract 2.5, MS-07). Strict body gate (400 INVALID_REQUEST
 * with details.field) -> one-session rule (409 CONFLICT) -> the RTP
 * source IP is the HTTP request peer -> michi_session_start (the
 * receiver picks the UDP port in 49152..65535; all-or-nothing start).
 * 201 returns the session_token ONCE (RAM-only). */
static esp_err_t session_start_handler(httpd_req_t *req)
{
    char controller_id[MICHI_PAIRING_DEVICE_ID_LEN] = {0};
    if (!auth_gate(req, MICHI_PERM_PLAYBACK, controller_id,
                   sizeof(controller_id))) {
        return ESP_OK; /* 401/403 already sent (P0-5) */
    }

    cJSON *root = read_json_body(req);
    if (root == NULL) {
        return ESP_OK; /* 400 already sent (P0-5) */
    }
    michi_http_session_create_body_t body;
    char field[32] = {0};
    const bool body_ok = michi_http_json_get_session_create(
        root, &body, field, sizeof(field));
    cJSON_Delete(root);
    if (!body_ok) {
        return michi_http_send_error(req, 400,
                                     "invalid session create request body",
                                     field);
    }

    /* One session per receiver: a second POST while one is active
     * answers 409 CONFLICT (validation wins over the conflict check, as
     * in the simulator reference). */
    if (michi_session_active()) {
        return michi_http_send_error(req, 409,
                                     "a session already exists", NULL);
    }

    /* The RTP source IP is the TCP peer of THIS request - never JSON. */
    char source_ip[16] = {0};
    client_ip_str(req, source_ip, sizeof(source_ip));
    if (source_ip[0] == '\0') {
        return michi_http_send_error(req, 500,
                                     "could not determine the client "
                                     "address", NULL);
    }

    const michi_session_start_params_t params = {
        .owner_controller_id = controller_id,
        .codec = body.codec,
        .sample_rate = (uint32_t)body.sample_rate,
        .bit_depth = (uint8_t)body.bit_depth,
        .channels = (uint8_t)body.channels,
        .packet_ms = (uint8_t)body.packet_ms,
        .buffer_ms = (uint16_t)body.buffer_ms,
        .payload_type = (uint8_t)body.payload_type,
        .ssrc = body.ssrc,
        .volume = (uint8_t)body.volume,
        .source_ip = source_ip,
    };
    char token[MICHI_SESSION_TOKEN_LEN];
    const esp_err_t start_err = michi_session_start(&params, token,
                                                    sizeof(token));
    if (start_err != ESP_OK) {
        ESP_LOGW(TAG, "session start failed: %s",
                 esp_err_to_name(start_err));
        /* All-or-nothing start: bind/buffer/pipeline failures already
         * rolled back - no session exists, the client retries. */
        return michi_http_send_error(req, 500,
                                     "the audio session could not be "
                                     "started", NULL);
    }
    michi_session_info_t info;
    if (michi_session_get_info(&info) != ESP_OK) {
        /* Defensive: a session that was just started must be readable;
         * tear it down instead of answering half a contract. */
        (void)michi_session_stop(token);
        return michi_http_send_error(req, 500,
                                     "the audio session could not be "
                                     "started", NULL);
    }

    cJSON *resp = cJSON_CreateObject();
    if (resp == NULL) {
        /* The one-shot token was never delivered; tear the session down
         * instead of leaving an unreachable ghost session. */
        (void)michi_session_stop(token);
        return michi_http_send_error(req, 500,
                                     "out of memory while building "
                                     "response", NULL);
    }
    esp_err_t err = ESP_OK;
    cJSON *effective = cJSON_AddObjectToObject(resp, "effective");
    if (cJSON_AddStringToObject(resp, "session_id", info.session_id) == NULL ||
        cJSON_AddStringToObject(resp, "session_token", token) == NULL ||
        cJSON_AddNumberToObject(resp, "lease_seconds",
                                MICHI_SESSION_LEASE_SECONDS) == NULL ||
        effective == NULL ||
        cJSON_AddStringToObject(effective, "transport",
                                body.transport) == NULL ||
        cJSON_AddStringToObject(effective, "codec", body.codec) == NULL ||
        cJSON_AddNumberToObject(effective, "sample_rate",
                                (double)body.sample_rate) == NULL ||
        cJSON_AddNumberToObject(effective, "bit_depth",
                                (double)body.bit_depth) == NULL ||
        cJSON_AddNumberToObject(effective, "channels",
                                (double)body.channels) == NULL ||
        cJSON_AddNumberToObject(effective, "packet_ms",
                                (double)body.packet_ms) == NULL ||
        cJSON_AddNumberToObject(effective, "buffer_ms",
                                (double)info.buffer_ms) == NULL ||
        cJSON_AddNumberToObject(effective, "payload_type",
                                (double)body.payload_type) == NULL ||
        cJSON_AddNumberToObject(effective, "ssrc",
                                (double)info.ssrc) == NULL ||
        cJSON_AddNumberToObject(effective, "stream_port",
                                (double)info.stream_port) == NULL ||
        cJSON_AddNumberToObject(effective, "volume",
                                (double)info.volume) == NULL) {
        err = ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK) {
        err = michi_http_send_json(req, 201, resp);
    }
    cJSON_Delete(resp);
    if (err != ESP_OK) {
        /* Response build/send failed after a successful start: the token
         * was never delivered, so stop the session (no ghost session). */
        (void)michi_session_stop(token);
        return michi_http_send_error(req, 500,
                                     "failed to build session response",
                                     NULL);
    }
    return ESP_OK;
}

/* GET /api/v1/receiver-lite/session (Bearer): the canonical state body.
 * Requires Bearer but NOT X-Michi-Session (contract 2.4). 404 without a
 * session. The session_token NEVER appears. */
static esp_err_t session_current_get_handler(httpd_req_t *req)
{
    char controller_id[MICHI_PAIRING_DEVICE_ID_LEN] = {0};
    if (!auth_gate(req, MICHI_PERM_STATUS, controller_id,
                   sizeof(controller_id))) {
        return ESP_OK; /* 401/403 already sent (P0-5) */
    }
    michi_session_info_t info;
    if (michi_session_get_info(&info) != ESP_OK) {
        return michi_http_send_error(req, 404, "no active session", NULL);
    }
    return send_session_state(req, &info);
}

/* PATCH /api/v1/receiver-lite/session (Bearer + X-Michi-Session):
 * canonical patch (volume 0..100 and/or paused). Guard order matches
 * the simulator reference: bearer -> session exists (404) -> session
 * token (401) -> body (400). Answers the SAME state body as GET after
 * applying. */
static esp_err_t session_patch_handler(httpd_req_t *req)
{
    char controller_id[MICHI_PAIRING_DEVICE_ID_LEN] = {0};
    if (!auth_gate(req, MICHI_PERM_PLAYBACK, controller_id,
                   sizeof(controller_id))) {
        return ESP_OK; /* 401/403 already sent (P0-5) */
    }
    if (!michi_session_active()) {
        return michi_http_send_error(req, 404, "no active session", NULL);
    }
    char token[MICHI_SESSION_TOKEN_LEN];
    const esp_err_t token_err = read_session_token(req, token,
                                                   sizeof(token));
    if (token_err != ESP_OK) {
        return token_err; /* 401 already sent (P0-5) */
    }

    cJSON *root = read_json_body(req);
    if (root == NULL) {
        return ESP_OK; /* 400 already sent (P0-5) */
    }
    michi_http_session_patch_body_t body;
    char field[32] = {0};
    const bool body_ok = michi_http_json_get_session_patch(
        root, &body, field, sizeof(field));
    cJSON_Delete(root);
    if (!body_ok) {
        return michi_http_send_error(req, 400,
                                     "invalid session patch request body",
                                     field);
    }

    const esp_err_t err = michi_session_patch(
        token, body.has_volume, (uint8_t)(body.has_volume ? body.volume : 0),
        body.has_paused, body.paused);
    if (err == ESP_ERR_NOT_FOUND) {
        return michi_http_send_error(req, 401,
                                     "missing or invalid session token",
                                     NULL);
    }
    if (err == ESP_ERR_INVALID_STATE) {
        return michi_http_send_error(req, 404, "no active session", NULL);
    }
    if (err != ESP_OK) {
        return michi_http_send_error(req, 500,
                                     "the session patch failed", NULL);
    }

    michi_session_info_t info;
    if (michi_session_get_info(&info) != ESP_OK) {
        return michi_http_send_error(req, 404, "no active session", NULL);
    }
    return send_session_state(req, &info);
}

/* DELETE /api/v1/receiver-lite/session (Bearer + X-Michi-Session, no
 * body): canonical close (contract 2.5). 204 on success - the engine
 * stops accepting RTP, the buffers/socket are freed and the session
 * token is wiped from RAM. Guard order: bearer -> session exists (404)
 * -> session token (401). Idempotent for the authenticated session. */
static esp_err_t session_delete_handler(httpd_req_t *req)
{
    char controller_id[MICHI_PAIRING_DEVICE_ID_LEN] = {0};
    if (!auth_gate(req, MICHI_PERM_PLAYBACK, controller_id,
                   sizeof(controller_id))) {
        return ESP_OK; /* 401/403 already sent (P0-5) */
    }
    if (!michi_session_active()) {
        return michi_http_send_error(req, 404, "no active session", NULL);
    }
    char token[MICHI_SESSION_TOKEN_LEN];
    const esp_err_t token_err = read_session_token(req, token,
                                                   sizeof(token));
    if (token_err != ESP_OK) {
        return token_err; /* 401 already sent (P0-5) */
    }

    const esp_err_t err = michi_session_stop(token);
    if (err == ESP_ERR_NOT_FOUND) {
        return michi_http_send_error(req, 401,
                                     "missing or invalid session token",
                                     NULL);
    }
    if (err == ESP_ERR_INVALID_STATE) {
        return michi_http_send_error(req, 404, "no active session", NULL);
    }
    if (err == ESP_ERR_TIMEOUT) {
        return michi_http_send_error(req, 500,
                                     "the session did not stop - retry",
                                     NULL);
    }
    if (err != ESP_OK) {
        return michi_http_send_error(req, 500,
                                     "the session could not be closed",
                                     NULL);
    }
    /* 204: no body, no content type. */
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

/* POST /api/v1/receiver-lite/heartbeat (Bearer + X-Michi-Session): the
 * canonical lease renewal (contract 2.6, MS-08). Guard order mirrors
 * the simulator reference: bearer -> session exists (404) -> session
 * token (401) -> body (400) -> sequence replay/session mismatch (409/
 * 404). The PLAYBACK permission gates it (same as every session
 * mutation: the contract has no dedicated heartbeat permission). A
 * valid heartbeat renews the 30 s lease on the MONOTONIC clock; a
 * repeated/older sequence does NOT renew. sent_at_ms is validated
 * and then ignored (informational per the contract). */
static esp_err_t v1lite_heartbeat_handler(httpd_req_t *req)
{
    char controller_id[MICHI_PAIRING_DEVICE_ID_LEN] = {0};
    if (!auth_gate(req, MICHI_PERM_PLAYBACK, controller_id,
                     sizeof(controller_id))) {
        return ESP_OK; /* 401/403 already sent (P0-5) */
    }
    if (!michi_session_active()) {
        return michi_http_send_error(req, 404, "no active session", NULL);
    }
    char token[MICHI_SESSION_TOKEN_LEN];
    const esp_err_t token_err = read_session_token(req, token,
                                                   sizeof(token));
    if (token_err != ESP_OK) {
        return token_err; /* 401 already sent (P0-5) */
    }

    cJSON *root = read_json_body(req);
    if (root == NULL) {
        return ESP_OK; /* 400 already sent (P0-5) */
    }
    michi_http_heartbeat_body_t body;
    char field[32] = {0};
    const bool body_ok = michi_http_json_get_heartbeat(root, &body, field,
                                                       sizeof(field));
    cJSON_Delete(root);
    if (!body_ok) {
        return michi_http_send_error(req, 400,
                                     "invalid heartbeat request body",
                                     field);
    }

    const michi_session_heartbeat_result_t result =
        michi_session_heartbeat(token, body.session_id, body.sequence);
    switch (result) {
    case MICHI_SESSION_HEARTBEAT_TOKEN_MISMATCH:
        return michi_http_send_error(req, 401,
                                     "missing or invalid session token",
                                     NULL);
    case MICHI_SESSION_HEARTBEAT_NO_SESSION:
    case MICHI_SESSION_HEARTBEAT_SESSION_MISMATCH:
        return michi_http_send_error(req, 404, "no active session", NULL);
    case MICHI_SESSION_HEARTBEAT_SEQUENCE_REPLAY:
        return michi_http_send_error(req, 409,
                                     "heartbeat sequence already seen",
                                     NULL);
    case MICHI_SESSION_HEARTBEAT_OK:
        break;
    }

    /* 200 (contract 2.6): lease_seconds frozen at 30; the uptime is the
     * MONOTONIC receiver clock (the same source as diagnostics). */
    cJSON *resp = cJSON_CreateObject();
    if (resp == NULL) {
        return michi_http_send_error(req, 500,
                                     "out of memory while building "
                                     "heartbeat response", NULL);
    }
    esp_err_t err = ESP_OK;
    if (cJSON_AddStringToObject(resp, "session_id", body.session_id) == NULL ||
        cJSON_AddStringToObject(resp, "status", "alive") == NULL ||
        cJSON_AddNumberToObject(resp, "lease_seconds",
                                MICHI_SESSION_LEASE_SECONDS) == NULL ||
        cJSON_AddNumberToObject(resp, "receiver_uptime_ms",
                                (double)(uint64_t)(esp_timer_get_time() /
                                                   1000)) == NULL) {
        err = ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK) {
        err = michi_http_send_json(req, 200, resp);
    }
    cJSON_Delete(resp);
    if (err != ESP_OK) {
        return michi_http_send_error(req, 500,
                                     "failed to build heartbeat response",
                                     NULL);
    }
    return ESP_OK;
}

/* PUT /api/v1/receiver-lite/now-playing (Bearer): deferred - the payload
 * shape is not frozen by the contract until the extension is certified.
 */
static esp_err_t now_playing_put_handler(httpd_req_t *req)
{
    char controller_id[MICHI_PAIRING_DEVICE_ID_LEN] = {0};
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
    char controller_id[MICHI_PAIRING_DEVICE_ID_LEN] = {0};
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
    char controller_id[MICHI_PAIRING_DEVICE_ID_LEN] = {0};
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
    char controller_id[MICHI_PAIRING_DEVICE_ID_LEN] = {0};
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
                cJSON_AddNumberToObject(audio, "drops_pt_other", m.drops_pt_other) == NULL ||
                cJSON_AddNumberToObject(audio, "drops_ssrc_filtered", m.drops_ssrc_filtered) == NULL ||
                cJSON_AddNumberToObject(audio, "drops_source_ip", m.drops_source_ip) == NULL ||
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
        /* F14 session block: the session layer snapshot (no token, ever)
         * plus the lease-expiry counter (MS-08 metric). On no active
         * session {"active": false, "lease_expirations": <cumulative>}
         * is emitted - the counter survives session closes. */
        michi_session_info_t info;
        cJSON *session = cJSON_AddObjectToObject(root, "session");
        if (session == NULL) {
            send_err = ESP_ERR_NO_MEM;
        } else if (cJSON_AddNumberToObject(
                       session, "lease_expirations",
                       (double)michi_session_lease_expirations()) == NULL) {
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
