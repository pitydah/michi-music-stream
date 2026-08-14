#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"
#include "cJSON.h"

#include "michi_product_profile.h"
#include "michi_session.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief HTTP API layer: the canonical Michi Link receiver v1-lite
 *        surface (MS-03).
 *
 * Serves on port 80 ONLY the canonical routes of the vendored bundle
 * (contracts/michi-link/): /api/v1/server/info, the /api/v1/pair routes,
 * /api/v1/receiver-lite/session, /heartbeat, /now-playing,
 * /diagnostics and /firmware. No legacy route is kept.
 *
 * /server/info emits the exact receiver v1-lite profile (build_info_json;
 * the identity group is NOT emitted yet - it requires the persistent
 * Ed25519 identity of michi_identity, MS-04). Receiver-button pairing
 * (MS-06), the canonical RTP session/lease (MS-07/MS-08), the certified
 * now-playing payload and the OTA flow answer 501 NOT_IMPLEMENTED after
 * the route-table auth and the strict JSON body gate; the matching
 * feature flag in /server/info is false. Diagnostics is implemented (its
 * response shape is not frozen by the contract).
 *
 * Every error response uses the single canonical envelope
 * {error:{code,message,request_id,details}} built by
 * michi_http_build_error() and sent by michi_http_send_error(), which
 * derives the code from the HTTP status per the section 2.7 map. Unknown
 * routes answer the canonical 404 envelope.
 *
 * ------------------------------------------------------------------
 * Handler contract (fixed by construction)
 * ------------------------------------------------------------------
 * Every handler that parses a JSON body MUST follow this exact order:
 *
 *   1. read the body: michi_http_read_body() (full Content-Length,
 *      bounded size, anti-slowloris total deadline with a single timeout
 *      retry - never a partial read);
 *   2. parse with cJSON_Parse();
 *   3. copy EVERY value into local buffers with the checked helpers
 *      michi_http_json_get_string/int/bool() - exact JSON type plus
 *      length/range limit; a value that does not fit is a 400, never a
 *      silent truncation;
 *   4. cJSON_Delete(root) - after this call NO pointer into the tree is
 *      valid;
 *   5. process using only the copied local values;
 *   6. respond with michi_http_send_json()/michi_http_send_error().
 *
 * PROHIBITED: returning, storing or dereferencing a pointer obtained
 * from cJSON_GetObjectItem() after cJSON_Delete() (use-after-free) and
 * any macro that yields a cJSON pointer. GET handlers build a fresh
 * response tree, send it, then delete it; the tree never outlives the
 * handler.
 * ------------------------------------------------------------------
 */

/**
 * @brief Start the HTTP server and register the canonical endpoints.
 *
 * Idempotent: a second call while running returns ESP_OK.
 *
 * @return ESP_OK; httpd_start/httpd_register_uri_handler errors are
 *         propagated (no ESP_ERROR_CHECK, no halt).
 */
esp_err_t michi_http_init(void);

/**
 * @brief Stop the HTTP server (all handlers unregistered).
 *
 * @return ESP_OK; ESP_ERR_INVALID_STATE when never started.
 */
esp_err_t michi_http_stop(void);

/**
 * @brief Read a request body completely.
 *
 * Reads exactly Content-Length bytes (rejecting a missing or malformed
 * header - strict parse, no trailing junk), never more than buf_len - 1
 * bytes (the caller's size IS the limit). Anti-slowloris contract: a
 * socket timeout is retried at most once AND the whole body must arrive
 * within MICHI_HTTP_BODY_TOTAL_TIMEOUT_MS of wall time, so a stalled or
 * trickling client cannot block the httpd task forever. The buffer is
 * NUL-terminated.
 *
 * @param req     Request.
 * @param buf     Output buffer (at least buf_len bytes).
 * @param buf_len Buffer size; the enforced body limit is buf_len - 1.
 * @param out_len Set to the number of body bytes read.
 * @return ESP_OK; ESP_ERR_INVALID_ARG on NULL args/zero buffer;
 *         ESP_ERR_NOT_FOUND when Content-Length is missing;
 *         ESP_ERR_INVALID_SIZE when the body exceeds the buffer;
 *         ESP_ERR_TIMEOUT when the client stalls (socket timeout retried
 *         or total deadline exceeded);
 *         ESP_ERR_INVALID_STATE on a socket/parse error (caller answers
 *         400).
 */
esp_err_t michi_http_read_body(httpd_req_t *req, char *buf, size_t buf_len,
                               size_t *out_len);

/**
 * @brief Checked JSON string getter.
 *
 * Requires an exact JSON string type AND a value that fits in out (the
 * caller's size IS the limit); on any violation returns false and leaves
 * out untouched - no truncation, no type coercion.
 *
 * @return true and out NUL-terminated on success.
 */
bool michi_http_json_get_string(const cJSON *obj, const char *key,
                                char *out, size_t out_len);

/**
 * @brief Checked JSON integer getter.
 *
 * Requires an exact JSON number type AND an integer value that fits in
 * int: strings are NOT coerced, fractional or out-of-range values fail
 * (never truncated).
 *
 * @return true and *out set on success.
 */
bool michi_http_json_get_int(const cJSON *obj, const char *key, int *out);

/**
 * @brief Checked JSON bool getter.
 *
 * Requires an exact JSON boolean type.
 *
 * @return true and *out set on success.
 */
bool michi_http_json_get_bool(const cJSON *obj, const char *key, bool *out);

/**
 * @brief Build the canonical error envelope (contract section 2.7):
 *
 *   { "error": { "code": <code>, "message": <message>,
 *                "request_id": <request_id>, "details": {...} } }
 *
 * details always exists; when field is non-NULL it carries
 * {"field": <field>}, otherwise it is empty. Pure cJSON (no ESP-IDF
 * runtime dependency): compiled and tested by the host-side tests.
 *
 * @param out_root   Output tree (caller owns it; cJSON_Delete()).
 * @param code       Canonical error code (e.g. "INVALID_REQUEST").
 * @param message    Human-readable message.
 * @param request_id Correlation id (UUID v4 from the caller).
 * @param field      Optional offending field name (NULL for none).
 * @return ESP_OK; ESP_ERR_NO_MEM on allocation failure (nothing is
 *         emitted - the envelope is all-or-nothing);
 *         ESP_ERR_INVALID_ARG on NULL args.
 */
esp_err_t michi_http_build_error(cJSON **out_root, const char *code,
                                 const char *message, const char *request_id,
                                 const char *field);

/**
 * @brief Build the exact receiver v1-lite info profile into root
 *        (contract section 2.1).
 *
 * service is derived from the profile tier (michi-stream-standard or
 * michi-stream-hifi); name, version, api_version ("v1-lite"), roles
 * (["audio_receiver"]), auth (RECEIVER_BUTTON), the truthful feature
 * flags and the certified audio block follow. The identity group
 * (server_id/identity_scheme/michi_id/public_key) is NOT emitted: it
 * requires the persistent Ed25519 identity (MS-04). Pure cJSON +
 * michi_product_profile_t: compiled and tested by the host-side tests.
 *
 * @param root Target object (fresh, empty).
 * @param p    Current product profile snapshot.
 * @return ESP_OK; ESP_ERR_NO_MEM on allocation failure;
 *         ESP_ERR_INVALID_ARG on NULL args.
 */
esp_err_t build_info_json(cJSON *root, const michi_product_profile_t *p);

/**
 * @brief Send the single canonical error response.
 *
 * The code is derived from the HTTP status per the section 2.7 map
 * (400 INVALID_REQUEST, 401 UNAUTHORIZED, 403 FORBIDDEN, 404 NOT_FOUND,
 * 409 CONFLICT, 429 RATE_LIMITED, 500 INTERNAL_ERROR, 501
 * NOT_IMPLEMENTED - anything else maps to INTERNAL_ERROR), so no handler
 * can emit a local code. A fresh UUID v4 request_id is generated per
 * response.
 *
 * @param status  HTTP status (see status_to_code map).
 * @param message Human-readable message.
 * @param field   Optional offending field name (NULL for none).
 * @return ESP_OK when the response was sent (never ESP_FAIL after
 *         responding); the httpd send error otherwise.
 */
esp_err_t michi_http_send_error(httpd_req_t *req, int status,
                                const char *message, const char *field);

/**
 * @brief Serialize and send a JSON response tree.
 *
 * The tree is serialized to a string first (so it is safe to delete the
 * tree right after this returns) and is NOT consumed: the caller still
 * owns it and MUST call cJSON_Delete().
 *
 * @return ESP_OK when the response was sent; the httpd send error
 *         otherwise.
 */
esp_err_t michi_http_send_json(httpd_req_t *req, int status,
                               const cJSON *root);

/**
 * @brief Parse + copy the canonical pair/start body (POST /pair/start).
 *
 * Validates the schema shape (device_name/device_type/roles/auth_strategy
 * presence, types and enums), REJECTS the legacy fields initiator_id and
 * client_token (spec 2.3 + MS-06: they are not part of the
 * receiver-button flow), and copies the four identity fields
 * (michi_id/public_key/challenge_nonce/challenge_signature) into the
 * caller's buffers. On failure the offending field name ("initiator_id",
 * "client_token", "michi_id", ...) is written to err_field - the caller
 * answers 400 INVALID_REQUEST with details.field. Pure cJSON + pure
 * validators: compiled and tested by the host-side tests.
 *
 * @param obj                Parsed request object.
 * @param michi_id           Buffer (>= 44 bytes).
 * @param public_key         Buffer (>= 44 bytes).
 * @param challenge_nonce    Buffer (>= 22 + NUL bytes; the schema allows
 *                           up to 63 chars here).
 * @param challenge_signature Buffer (>= 87 bytes).
 * @param err_field          Buffer (>= 20 bytes) for the field name.
 * @return true when the body is canonical and everything was copied.
 */
bool michi_http_json_get_pair_start(const cJSON *obj,
                                    char *michi_id, size_t michi_id_len,
                                    char *public_key, size_t public_key_len,
                                    char *challenge_nonce, size_t nonce_len,
                                    char *challenge_signature,
                                    size_t signature_len,
                                    char *err_field, size_t err_field_len);

/**
 * @brief Parse + copy the canonical pair/confirm body (POST /pair/confirm).
 *
 * Rejects the legacy fields initiator_id and client_token, validates
 * session_id (UUID v4), pin (6 digits), michi_id and public_key
 * (43-char base64url-nopad) and copies everything into the caller's
 * buffers. On failure err_field receives the offending field name.
 * Pure cJSON + pure validators: compiled and tested by the host tests.
 *
 * @param obj          Parsed request object.
 * @param session_id   Buffer (>= MICHI_PAIRING_SESSION_ID_LEN bytes).
 * @param pin          Buffer (>= MICHI_PAIRING_PIN_BUF_LEN bytes).
 * @param michi_id     Buffer (>= 44 bytes).
 * @param public_key   Buffer (>= 44 bytes).
 * @param err_field    Buffer (>= 20 bytes) for the field name.
 * @return true when the body is canonical and everything was copied.
 */
bool michi_http_json_get_pair_confirm(const cJSON *obj,
                                      char *session_id, size_t session_id_len,
                                      char *pin, size_t pin_len,
                                      char *michi_id, size_t michi_id_len,
                                      char *public_key, size_t public_key_len,
                                      char *err_field, size_t err_field_len);

/**
 * @brief Canonical session-create body (POST /receiver-lite/session),
 *        validated and copied (receiver-session-create.schema.json).
 */
typedef struct {
    char transport[10]; /*!< "rtp_udp" (exact) */
    char codec[MICHI_SESSION_CODEC_LEN]; /*!< "pcm_s16le" (exact) */
    int sample_rate;    /*!< 48000 (exact) */
    int bit_depth;      /*!< 16 (exact) */
    int channels;       /*!< 2 (exact) */
    int packet_ms;      /*!< 10 (exact) */
    int buffer_ms;      /*!< 50..500 */
    int payload_type;   /*!< 97 (exact) */
    uint32_t ssrc;      /*!< 1..4294967295 (exact negotiation) */
    int volume;         /*!< 0..100 */
} michi_http_session_create_body_t;

/**
 * @brief Parse + copy the canonical session-create body (MS-07).
 *
 * Strict validation per receiver-session-create.schema.json: every field
 * is required with its exact type and const/range value (transport
 * rtp_udp, codec pcm_s16le, 48000/16/2, packet_ms 10, payload_type 97,
 * buffer_ms 50..500, ssrc 1..4294967295, volume 0..100); ANY other
 * property (e.g. stream_port or source_ip - the receiver picks the port
 * and the RTP source IP is the HTTP request peer, never JSON) is
 * rejected with 400 details.field. No rounding, no correction: invalid
 * values are rejected, never clamped.
 *
 * On failure err_field receives the offending field name (or "body").
 * Pure cJSON: compiled and tested by the host-side tests.
 *
 * @param obj        Parsed request object.
 * @param out        Copied, validated body.
 * @param err_field  Buffer (>= 20 bytes) for the field name.
 * @param err_field_len Size of err_field.
 * @return true when the body is canonical and everything was copied.
 */
bool michi_http_json_get_session_create(const cJSON *obj,
                                        michi_http_session_create_body_t *out,
                                        char *err_field,
                                        size_t err_field_len);

/**
 * @brief Canonical session-patch body (PATCH /receiver-lite/session),
 *        validated and copied (receiver-session-patch.schema.json).
 */
typedef struct {
    bool has_volume; /*!< volume was present */
    int  volume;     /*!< 0..100 */
    bool has_paused; /*!< paused was present */
    bool paused;     /*!< target pause state */
} michi_http_session_patch_body_t;

/**
 * @brief Parse + copy the canonical session-patch body (MS-07).
 *
 * Strict validation per receiver-session-patch.schema.json: at least
 * one property; only volume (integer 0..100) and paused (boolean) are
 * accepted - any other property is rejected with 400 details.field.
 *
 * @param obj        Parsed request object.
 * @param out        Copied, validated body.
 * @param err_field  Buffer (>= 20 bytes) for the field name.
 * @param err_field_len Size of err_field.
 * @return true when the body is canonical and everything was copied.
 */
bool michi_http_json_get_session_patch(const cJSON *obj,
                                       michi_http_session_patch_body_t *out,
                                       char *err_field, size_t err_field_len);

#ifdef __cplusplus
}
#endif
