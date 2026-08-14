/*
 * Canonical receiver v1-lite JSON DTO builders (MS-03).
 *
 * Extracted from http_server.c for host-side testing (F15 pattern): the
 * component and tests/host compile the SAME source. Pure cJSON - no
 * ESP-IDF runtime dependencies - so the exact canonical error envelope
 * (section 2.7) and the exact server info profile (section 2.1) of the
 * vendored Michi Link contract are verifiable on the host.
 */

#include "michi_http.h"

#include "cJSON.h"

esp_err_t michi_http_build_error(cJSON **out_root, const char *code,
                                 const char *message, const char *request_id,
                                 const char *field)
{
    if (out_root == NULL || code == NULL || message == NULL ||
        request_id == NULL) {
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
    /* The envelope is all-or-nothing: a partial envelope is never emitted
     * (shared P0-5/F8 rule with http_server.c). */
    if (cJSON_AddStringToObject(e, "code", code) == NULL ||
        cJSON_AddStringToObject(e, "message", message) == NULL ||
        cJSON_AddStringToObject(e, "request_id", request_id) == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    cJSON *d = cJSON_AddObjectToObject(e, "details");
    if (d == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    if (field != NULL &&
        cJSON_AddStringToObject(d, "field", field) == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    *out_root = root;
    return ESP_OK;
}

/* Small fixed-content array helpers for the info profile: the canonical
 * audio block is closed by the contract (sections 2.1 and 4), so every
 * entry is a contract constant, not runtime data. */
static bool add_string_array(cJSON *obj, const char *key,
                             const char *const *items, size_t count)
{
    cJSON *arr = cJSON_AddArrayToObject(obj, key);
    if (arr == NULL) {
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateString(items[i]);
        if (item == NULL || !cJSON_AddItemToArray(arr, item)) {
            if (item != NULL) {
                cJSON_Delete(item);
            }
            return false;
        }
    }
    return true;
}

static bool add_number_array(cJSON *obj, const char *key,
                             const double *items, size_t count)
{
    cJSON *arr = cJSON_AddArrayToObject(obj, key);
    if (arr == NULL) {
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateNumber(items[i]);
        if (item == NULL || !cJSON_AddItemToArray(arr, item)) {
            if (item != NULL) {
                cJSON_Delete(item);
            }
            return false;
        }
    }
    return true;
}

/* The canonical receiver v1-lite info profile (section 2.1). The
 * identity group (server_id, identity_scheme, michi_id, public_key) is
 * NOT emitted: it requires the persistent Ed25519 identity, which lands
 * with michi_identity (MS-04) and is wired into this endpoint by the
 * package that follows it - the profile here is everything this stage
 * can announce truthfully. service is derived from the runtime tier
 * (section 2.1: only michi-stream-standard or michi-stream-hifi); a
 * degraded (DIAGNOSTIC) unit announces the standard service. */
esp_err_t build_info_json(cJSON *root, const michi_product_profile_t *p)
{
    if (root == NULL || p == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *service = (p->tier == MICHI_PRODUCT_HIFI)
                              ? "michi-stream-hifi"
                              : "michi-stream-standard";
    if (cJSON_AddStringToObject(root, "service", service) == NULL ||
        cJSON_AddStringToObject(root, "name", p->product_name) == NULL ||
        cJSON_AddStringToObject(root, "version", p->firmware_version) == NULL ||
        cJSON_AddStringToObject(root, "api_version", "v1-lite") == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (!add_string_array(root, "roles",
                          (const char *const[]){"audio_receiver"}, 1)) {
        return ESP_ERR_NO_MEM;
    }

    cJSON *auth = cJSON_AddObjectToObject(root, "auth");
    if (auth == NULL ||
        cJSON_AddBoolToObject(auth, "required", true) == NULL ||
        cJSON_AddStringToObject(auth, "strategy", "RECEIVER_BUTTON") == NULL ||
        cJSON_AddBoolToObject(auth, "token_refresh", false) == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* Feature flags are truthful by construction: a flag is true only
     * while its handler is registered AND implemented. Session and the
     * heartbeat lease are implemented (MS-07/MS-08) with positive
     * tests, and volume lives inside the session PATCH mutation
     * (implemented with the session), so all three are true. The
     * certified now-playing payload and the OTA flow still answer 501
     * NOT_IMPLEMENTED, so their flags are false; diagnostics is
     * implemented (its response shape is not frozen by the contract). */
    cJSON *feat = cJSON_AddObjectToObject(root, "features");
    if (feat == NULL ||
        cJSON_AddBoolToObject(feat, "session", true) == NULL ||
        cJSON_AddBoolToObject(feat, "heartbeat", true) == NULL ||
        cJSON_AddBoolToObject(feat, "volume", true) == NULL ||
        cJSON_AddBoolToObject(feat, "now_playing", false) == NULL ||
        cJSON_AddBoolToObject(feat, "diagnostics", true) == NULL ||
        cJSON_AddBoolToObject(feat, "ota", false) == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* Reproducible audio: the certified baseline is the SAME for
     * Standard and Hi-Fi (section 4) - PCM S16LE 48 kHz/16-bit/stereo,
     * 10 ms packets, payload type 97, 50..500 ms buffer. */
    cJSON *audio = cJSON_AddObjectToObject(root, "audio");
    if (audio == NULL ||
        !add_string_array(audio, "transports",
                          (const char *const[]){"rtp_udp"}, 1) ||
        !add_string_array(audio, "codecs",
                          (const char *const[]){"pcm_s16le"}, 1) ||
        !add_number_array(audio, "sample_rates",
                          (const double[]){48000}, 1) ||
        !add_number_array(audio, "bit_depths", (const double[]){16}, 1) ||
        !add_number_array(audio, "channels", (const double[]){2}, 1) ||
        !add_number_array(audio, "packet_ms", (const double[]){10}, 1) ||
        !add_number_array(audio, "payload_types", (const double[]){97}, 1) ||
        cJSON_AddNumberToObject(audio, "buffer_ms_min", 50) == NULL ||
        cJSON_AddNumberToObject(audio, "buffer_ms_max", 500) == NULL) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
