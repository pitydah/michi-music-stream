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
#include "michi_discovery.h"
#include "michi_identity.h"

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

/* The canonical receiver v1-lite info profile (section 2.1).
 * Emits the complete contract surface required by server-info.schema.json:
 * service, name, version, api_version, roles, auth, features, server_id,
 * identity_scheme, michi_id, public_key, audio. */
esp_err_t build_info_json_with_identity(cJSON *root, const michi_product_profile_t *p,
                                        const char *server_id,
                                        const char *michi_id,
                                        const char *public_key)
{
    if (root == NULL || p == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (p->tier == MICHI_PRODUCT_STANDARD || p->tier == MICHI_PRODUCT_HIFI) {
        if (server_id == NULL || server_id[0] == '\0' ||
            michi_id == NULL || michi_id[0] == '\0' ||
            public_key == NULL || public_key[0] == '\0') {
            return ESP_ERR_INVALID_STATE;
        }
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

    /* Feature flags: read from michi_product_profile_capabilities_for(p)
     * (Signal Truth: session/heartbeat/volume false when audio_available is false). */
    const michi_product_capabilities_t caps =
        michi_product_profile_capabilities_for(p);
    cJSON *feat = cJSON_AddObjectToObject(root, "features");
    if (feat == NULL ||
        cJSON_AddBoolToObject(feat, "session", caps.session) == NULL ||
        cJSON_AddBoolToObject(feat, "heartbeat", caps.heartbeat) == NULL ||
        cJSON_AddBoolToObject(feat, "volume", caps.volume) == NULL ||
        cJSON_AddBoolToObject(feat, "now_playing", caps.now_playing) == NULL ||
        cJSON_AddBoolToObject(feat, "diagnostics", caps.diagnostics) == NULL ||
        cJSON_AddBoolToObject(feat, "ota", caps.ota) == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* Identity group: mandatory for michi-stream-* per server-info.schema.json */
    if (server_id != NULL && server_id[0] != '\0') {
        if (cJSON_AddStringToObject(root, "server_id", server_id) == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (cJSON_AddStringToObject(root, "identity_scheme", MICHI_IDENTITY_SCHEME) == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (michi_id != NULL && michi_id[0] != '\0') {
        if (cJSON_AddStringToObject(root, "michi_id", michi_id) == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (public_key != NULL && public_key[0] != '\0') {
        if (cJSON_AddStringToObject(root, "public_key", public_key) == NULL) {
            return ESP_ERR_NO_MEM;
        }
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

esp_err_t build_info_json(cJSON *root, const michi_product_profile_t *p)
{
    char server_id[MICHI_DISCOVERY_UUID_LEN] = {0};
    char michi_id[MICHI_IDENTITY_MICHI_ID_LEN] = {0};
    uint8_t pk_raw[MICHI_IDENTITY_KEY_BYTES];
    char pk_b64[MICHI_IDENTITY_PUBLIC_KEY_B64_LEN] = {0};

    const char *s_id = NULL;
    const char *m_id = NULL;
    const char *pk = NULL;

    if (michi_discovery_get_server_id(server_id, sizeof(server_id)) == ESP_OK) {
        s_id = server_id;
    }
    if (michi_identity_michi_id(michi_id, sizeof(michi_id)) == ESP_OK) {
        m_id = michi_id;
    }
    if (michi_identity_public_key(pk_raw) == ESP_OK &&
        michi_identity_base64url_encode(pk_raw, sizeof(pk_raw), pk_b64, sizeof(pk_b64)) == ESP_OK) {
        pk = pk_b64;
    }

    return build_info_json_with_identity(root, p, s_id, m_id, pk);
}
