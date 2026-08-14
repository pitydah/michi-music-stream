/* JSON access helpers with exact-type semantics (F15: extracted from
 * http_server.c so the host-side tests compile the SAME source - no
 * reimplementation). Uses cJSON; on the host the tests link the system
 * libcjson (CI: apt install libcjson-dev). */

#include "michi_http.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "validators.h"

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

/* --- pairing body gates (MS-06) ---------------------------------------- */

/* The legacy fields initiator_id and client_token are REJECTED with 400:
 * they are not part of the canonical receiver-button flow (spec 2.3 +
 * MS-06). Returns the offending field name (NULL when absent). */
static const char *pairing_rejected_field(const cJSON *obj)
{
    if (cJSON_GetObjectItem(obj, "initiator_id") != NULL) {
        return "initiator_id";
    }
    if (cJSON_GetObjectItem(obj, "client_token") != NULL) {
        return "client_token";
    }
    return NULL;
}

/* Strict base64url-nopad string of an EXACT length (the canonical wire
 * alphabet: A-Za-z0-9-_). */
static bool b64url_exact(const char *s, size_t exact_len)
{
    if (s == NULL || strlen(s) != exact_len) {
        return false;
    }
    for (size_t i = 0; i < exact_len; i++) {
        const char c = s[i];
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) {
            return false;
        }
    }
    return true;
}

/* The pair/start schema also requires these well-formed fields (they are
 * validated and then discarded: the receiver flow does not persist the
 * controller's display metadata). */
static bool pair_start_metadata_valid(const cJSON *obj)
{
    const cJSON *name = cJSON_GetObjectItem(obj, "device_name");
    if (name == NULL || !cJSON_IsString(name) || name->valuestring == NULL ||
        name->valuestring[0] == '\0' ||
        strlen(name->valuestring) > 128) {
        return false;
    }
    const cJSON *type = cJSON_GetObjectItem(obj, "device_type");
    if (type == NULL || !cJSON_IsString(type) || type->valuestring == NULL) {
        return false;
    }
    static const char *const k_types[] = {
        "mobile", "desktop", "receiver", "server",
    };
    bool type_ok = false;
    for (size_t i = 0; i < sizeof(k_types) / sizeof(k_types[0]); i++) {
        if (strcmp(type->valuestring, k_types[i]) == 0) {
            type_ok = true;
            break;
        }
    }
    if (!type_ok) {
        return false;
    }
    const cJSON *roles = cJSON_GetObjectItem(obj, "roles");
    if (roles == NULL || !cJSON_IsArray(roles) ||
        cJSON_GetArraySize(roles) == 0) {
        return false;
    }
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, roles)
    {
        if (!cJSON_IsString(item) || item->valuestring == NULL ||
            item->valuestring[0] == '\0') {
            return false;
        }
    }
    const cJSON *strategy = cJSON_GetObjectItem(obj, "auth_strategy");
    if (strategy == NULL || !cJSON_IsString(strategy) ||
        strategy->valuestring == NULL) {
        return false;
    }
    static const char *const k_strategies[] = {
        "PLAYER_PASSWORD", "SERVER_CODE", "ED25519_CHALLENGE",
        "RECEIVER_BUTTON", "LEGACY",
    };
    for (size_t i = 0; i < sizeof(k_strategies) / sizeof(k_strategies[0]);
         i++) {
        if (strcmp(strategy->valuestring, k_strategies[i]) == 0) {
            return true;
        }
    }
    return false;
}

bool michi_http_json_get_pair_start(const cJSON *obj,
                                    char *michi_id, size_t michi_id_len,
                                    char *public_key, size_t public_key_len,
                                    char *challenge_nonce, size_t nonce_len,
                                    char *challenge_signature,
                                    size_t signature_len,
                                    char *err_field, size_t err_field_len)
{
    if (obj == NULL || michi_id == NULL || public_key == NULL ||
        challenge_nonce == NULL || challenge_signature == NULL ||
        err_field == NULL || err_field_len == 0) {
        return false;
    }
    /* Parse -> copy -> delete: only copies leave this helper. */
    const char *rejected = pairing_rejected_field(obj);
    if (rejected != NULL) {
        snprintf(err_field, err_field_len, "%s", rejected);
        return false;
    }
    const cJSON *mi = cJSON_GetObjectItem(obj, "michi_id");
    const cJSON *pk = cJSON_GetObjectItem(obj, "public_key");
    const cJSON *nonce = cJSON_GetObjectItem(obj, "challenge_nonce");
    const cJSON *sig = cJSON_GetObjectItem(obj, "challenge_signature");
    if (!pair_start_metadata_valid(obj)) {
        snprintf(err_field, err_field_len, "%s", "body");
        return false;
    }
    if (mi == NULL || !cJSON_IsString(mi) || mi->valuestring == NULL ||
        !b64url_exact(mi->valuestring, 43)) {
        snprintf(err_field, err_field_len, "%s", "michi_id");
        return false;
    }
    if (pk == NULL || !cJSON_IsString(pk) || pk->valuestring == NULL ||
        !b64url_exact(pk->valuestring, 43)) {
        snprintf(err_field, err_field_len, "%s", "public_key");
        return false;
    }
    if (nonce == NULL || !cJSON_IsString(nonce) ||
        nonce->valuestring == NULL || strlen(nonce->valuestring) < 22 ||
        strlen(nonce->valuestring) >= nonce_len ||
        !b64url_exact(nonce->valuestring, strlen(nonce->valuestring))) {
        snprintf(err_field, err_field_len, "%s", "challenge_nonce");
        return false;
    }
    if (sig == NULL || !cJSON_IsString(sig) || sig->valuestring == NULL ||
        !b64url_exact(sig->valuestring, 86)) {
        snprintf(err_field, err_field_len, "%s", "challenge_signature");
        return false;
    }
    if (michi_id_len < 44 || public_key_len < 44 ||
        signature_len < 87) {
        snprintf(err_field, err_field_len, "%s", "body");
        return false;
    }
    memcpy(michi_id, mi->valuestring, 44);
    memcpy(public_key, pk->valuestring, 44);
    memcpy(challenge_nonce, nonce->valuestring, strlen(nonce->valuestring) + 1);
    memcpy(challenge_signature, sig->valuestring, 87);
    return true;
}

bool michi_http_json_get_pair_confirm(const cJSON *obj,
                                      char *session_id, size_t session_id_len,
                                      char *pin, size_t pin_len,
                                      char *michi_id, size_t michi_id_len,
                                      char *public_key, size_t public_key_len,
                                      char *err_field, size_t err_field_len)
{
    if (obj == NULL || session_id == NULL || pin == NULL || michi_id == NULL ||
        public_key == NULL || err_field == NULL || err_field_len == 0) {
        return false;
    }
    const char *rejected = pairing_rejected_field(obj);
    if (rejected != NULL) {
        snprintf(err_field, err_field_len, "%s", rejected);
        return false;
    }
    const cJSON *sid = cJSON_GetObjectItem(obj, "session_id");
    const cJSON *pin_item = cJSON_GetObjectItem(obj, "pin");
    const cJSON *mi = cJSON_GetObjectItem(obj, "michi_id");
    const cJSON *pk = cJSON_GetObjectItem(obj, "public_key");
    if (sid == NULL || !cJSON_IsString(sid) || sid->valuestring == NULL ||
        !michi_pairing_uuid_valid(sid->valuestring) ||
        strlen(sid->valuestring) >= session_id_len) {
        snprintf(err_field, err_field_len, "%s", "session_id");
        return false;
    }
    if (pin_item == NULL || !cJSON_IsString(pin_item) ||
        pin_item->valuestring == NULL ||
        !michi_pairing_pin_valid(pin_item->valuestring) ||
        strlen(pin_item->valuestring) >= pin_len) {
        snprintf(err_field, err_field_len, "%s", "pin");
        return false;
    }
    if (mi == NULL || !cJSON_IsString(mi) || mi->valuestring == NULL ||
        !b64url_exact(mi->valuestring, 43)) {
        snprintf(err_field, err_field_len, "%s", "michi_id");
        return false;
    }
    if (pk == NULL || !cJSON_IsString(pk) || pk->valuestring == NULL ||
        !b64url_exact(pk->valuestring, 43)) {
        snprintf(err_field, err_field_len, "%s", "public_key");
        return false;
    }
    if (michi_id_len < 44 || public_key_len < 44) {
        snprintf(err_field, err_field_len, "%s", "body");
        return false;
    }
    memcpy(session_id, sid->valuestring, strlen(sid->valuestring) + 1);
    memcpy(pin, pin_item->valuestring, strlen(pin_item->valuestring) + 1);
    memcpy(michi_id, mi->valuestring, 44);
    memcpy(public_key, pk->valuestring, 44);
    return true;
}
