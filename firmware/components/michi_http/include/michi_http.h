#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief HTTP API layer (phase 4: P0-1/P0-2/P0-3/P0-4/P0-5 (shared with
 *        audio_output: error propagation) fixed by construction).
 *
 * Serves the migrated read-only endpoints on port 80:
 *  - GET /api/v1/receiver/info
 *  - GET /api/v1/receiver/firmware
 *
 * Pairing (phase 10) and session/volume (phase 12) endpoints are written
 * with the SAME handler contract below; no handler may deviate from it.
 *
 * ------------------------------------------------------------------
 * Handler contract (P0-1/P0-2 fixed by construction)
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
 * @brief Start the HTTP server and register the read-only endpoints.
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
 * @brief Read a request body completely (P0-3 fixed).
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
 *         400 Bad Request).
 */
esp_err_t michi_http_read_body(httpd_req_t *req, char *buf, size_t buf_len,
                               size_t *out_len);

/**
 * @brief Checked JSON string getter (P0-4 fixed).
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
 * @brief Checked JSON integer getter (P0-4 fixed).
 *
 * Requires an exact JSON number type AND an integer value that fits in
 * int: strings are NOT coerced, fractional or out-of-range values fail
 * (never truncated).
 *
 * @return true and *out set on success.
 */
bool michi_http_json_get_int(const cJSON *obj, const char *key, int *out);

/**
 * @brief Checked JSON bool getter (P0-4 fixed).
 *
 * Requires an exact JSON boolean type.
 *
 * @return true and *out set on success.
 */
bool michi_http_json_get_bool(const cJSON *obj, const char *key, bool *out);

/**
 * @brief Send a {error:{code,message,details:{}}} JSON response (P0-5 -
 *        shared with audio_output: error propagation - fixed: returns
 *        the SEND result, ESP_OK once the response was sent - never
 *        ESP_FAIL after responding).
 *
 * @param status  HTTP status; supported: 200, 400, 401, 404, 409, 500
 *                (anything else maps to 500).
 * @return ESP_OK when the response was sent; the httpd send error
 *         otherwise.
 */
esp_err_t michi_http_send_error(httpd_req_t *req, int status,
                                const char *code, const char *message);

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

#ifdef __cplusplus
}
#endif
