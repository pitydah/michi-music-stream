/* JSON access helpers with exact-type semantics (F15: extracted from
 * http_server.c so the host-side tests compile the SAME source - no
 * reimplementation). Uses cJSON; on the host the tests link the system
 * libcjson (CI: apt install libcjson-dev). */

#include "michi_http.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "michi_pairing.h"

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

/* --- session body gates (MS-07) ---------------------------------------- */

/* The canonical session-create field names (receiver-session-create
 * .schema.json): additionalProperties is false - anything else is 400. */
static bool session_create_field_known(const char *name)
{
    static const char *const k_fields[] = {
        "transport", "codec", "sample_rate", "bit_depth", "channels",
        "packet_ms", "buffer_ms", "payload_type", "ssrc", "volume",
    };
    for (size_t i = 0; i < sizeof(k_fields) / sizeof(k_fields[0]); i++) {
        if (strcmp(name, k_fields[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* Any property outside the canonical set is rejected: the receiver picks
 * the stream port and the RTP source IP is the HTTP request peer - they
 * can never arrive in JSON (stream_port/source_ip -> 400). */
static const char *session_create_extra_field(const cJSON *obj)
{
    for (const cJSON *item = obj->child; item != NULL; item = item->next) {
        if (item->string != NULL && !session_create_field_known(item->string)) {
            return item->string;
        }
    }
    return NULL;
}

/* Exact JSON string equal to `expect` (const values are not ranges). */
static bool json_string_is(const cJSON *obj, const char *key,
                           const char *expect)
{
    const cJSON *item = cJSON_GetObjectItem(obj, key);
    return item != NULL && cJSON_IsString(item) && item->valuestring != NULL &&
           strcmp(item->valuestring, expect) == 0;
}

/* Exact JSON integer equal to `expect` (const values are not ranges). */
static bool json_int_is(const cJSON *obj, const char *key, int expect)
{
    const cJSON *item = cJSON_GetObjectItem(obj, key);
    if (item == NULL || !cJSON_IsNumber(item)) {
        return false;
    }
    const double d = item->valuedouble;
    return d == (double)expect && d == (double)(int)d;
}

bool michi_http_json_get_session_create(const cJSON *obj,
                                        michi_http_session_create_body_t *out,
                                        char *err_field,
                                        size_t err_field_len)
{
    if (obj == NULL || out == NULL || err_field == NULL ||
        err_field_len == 0) {
        return false;
    }
    /* Required + exact const/range values, in schema order. No rounding,
     * no correction: an invalid value is rejected and NAMED. */
    if (!json_string_is(obj, "transport", "rtp_udp")) {
        snprintf(err_field, err_field_len, "%s", "transport");
        return false;
    }
    if (!json_string_is(obj, "codec", "pcm_s16le")) {
        snprintf(err_field, err_field_len, "%s", "codec");
        return false;
    }
    if (!json_int_is(obj, "sample_rate", 48000)) {
        snprintf(err_field, err_field_len, "%s", "sample_rate");
        return false;
    }
    if (!json_int_is(obj, "bit_depth", 16)) {
        snprintf(err_field, err_field_len, "%s", "bit_depth");
        return false;
    }
    if (!json_int_is(obj, "channels", 2)) {
        snprintf(err_field, err_field_len, "%s", "channels");
        return false;
    }
    if (!json_int_is(obj, "packet_ms", 10)) {
        snprintf(err_field, err_field_len, "%s", "packet_ms");
        return false;
    }
    int buffer_ms = 0;
    if (!michi_http_json_get_int(obj, "buffer_ms", &buffer_ms) ||
        buffer_ms < 50 || buffer_ms > 500) {
        snprintf(err_field, err_field_len, "%s", "buffer_ms");
        return false;
    }
    if (!json_int_is(obj, "payload_type", 97)) {
        snprintf(err_field, err_field_len, "%s", "payload_type");
        return false;
    }
    /* ssrc: unsigned 32-bit 1..4294967295 - beyond INT_MAX, so the
     * checked double path (exact integer, no fractional, no coercion). */
    const cJSON *ssrc_item = cJSON_GetObjectItem(obj, "ssrc");
    if (ssrc_item == NULL || !cJSON_IsNumber(ssrc_item)) {
        snprintf(err_field, err_field_len, "%s", "ssrc");
        return false;
    }
    const double ssrc_d = ssrc_item->valuedouble;
    if (ssrc_d < 1.0 || ssrc_d > 4294967295.0 ||
        ssrc_d != (double)(uint64_t)ssrc_d) {
        snprintf(err_field, err_field_len, "%s", "ssrc");
        return false;
    }
    int volume = 0;
    if (!michi_http_json_get_int(obj, "volume", &volume) ||
        volume < 0 || volume > 100) {
        snprintf(err_field, err_field_len, "%s", "volume");
        return false;
    }
    /* additionalProperties: false - an unknown property (including
     * stream_port/source_ip) is a 400 with the offending field name. */
    const char *extra = session_create_extra_field(obj);
    if (extra != NULL) {
        snprintf(err_field, err_field_len, "%s", extra);
        return false;
    }
    /* Copy everything (parse -> copy -> delete: only copies leave). */
    const cJSON *t = cJSON_GetObjectItem(obj, "transport");
    const cJSON *c = cJSON_GetObjectItem(obj, "codec");
    if (t == NULL || c == NULL) {
        snprintf(err_field, err_field_len, "%s", "body");
        return false;
    }
    if (strlen(t->valuestring) >= sizeof(out->transport) ||
        strlen(c->valuestring) >= sizeof(out->codec)) {
        snprintf(err_field, err_field_len, "%s", "body");
        return false;
    }
    strlcpy(out->transport, t->valuestring, sizeof(out->transport));
    strlcpy(out->codec, c->valuestring, sizeof(out->codec));
    out->sample_rate = 48000;
    out->bit_depth = 16;
    out->channels = 2;
    out->packet_ms = 10;
    out->buffer_ms = buffer_ms;
    out->payload_type = 97;
    out->ssrc = (uint32_t)ssrc_d;
    out->volume = volume;
    return true;
}

bool michi_http_json_get_session_patch(const cJSON *obj,
                                       michi_http_session_patch_body_t *out,
                                       char *err_field, size_t err_field_len)
{
    if (obj == NULL || out == NULL || err_field == NULL ||
        err_field_len == 0) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    /* additionalProperties: false - only volume/paused exist. */
    for (const cJSON *item = obj->child; item != NULL; item = item->next) {
        if (item->string == NULL) {
            continue;
        }
        if (strcmp(item->string, "volume") != 0 &&
            strcmp(item->string, "paused") != 0) {
            snprintf(err_field, err_field_len, "%s", item->string);
            return false;
        }
    }
    const cJSON *v = cJSON_GetObjectItem(obj, "volume");
    if (v != NULL) {
        if (!cJSON_IsNumber(v) || !michi_http_json_get_int(obj, "volume",
                                                            &out->volume) ||
            out->volume < 0 || out->volume > 100) {
            snprintf(err_field, err_field_len, "%s", "volume");
            return false;
        }
        out->has_volume = true;
    }
    const cJSON *p = cJSON_GetObjectItem(obj, "paused");
    if (p != NULL) {
        if (!cJSON_IsBool(p)) {
            snprintf(err_field, err_field_len, "%s", "paused");
            return false;
        }
        out->has_paused = true;
        out->paused = cJSON_IsTrue(p);
    }
    if (!out->has_volume && !out->has_paused) {
        /* minProperties: 1 - an empty body is a 400. */
        snprintf(err_field, err_field_len, "%s", "body");
        return false;
    }
    return true;
}

/* --- heartbeat body gate (MS-08) --------------------------------------- */

/* The canonical heartbeat field names (receiver-heartbeat.schema.json):
 * additionalProperties is false - anything else is 400. */
static bool heartbeat_field_known(const char *name)
{
    static const char *const k_fields[] = {
        "session_id", "sequence", "sent_at_ms",
    };
    for (size_t i = 0; i < sizeof(k_fields) / sizeof(k_fields[0]); i++) {
        if (strcmp(name, k_fields[i]) == 0) {
            return true;
        }
    }
    return false;
}

static const char *heartbeat_extra_field(const cJSON *obj)
{
    for (const cJSON *item = obj->child; item != NULL; item = item->next) {
        if (item->string != NULL && !heartbeat_field_known(item->string)) {
            return item->string;
        }
    }
    return NULL;
}

bool michi_http_json_get_heartbeat(const cJSON *obj,
                                   michi_http_heartbeat_body_t *out,
                                   char *err_field, size_t err_field_len)
{
    if (obj == NULL || out == NULL || err_field == NULL ||
        err_field_len == 0) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    /* session_id: required, UUID v4 (format uuid). */
    const cJSON *sid = cJSON_GetObjectItem(obj, "session_id");
    if (sid == NULL || !cJSON_IsString(sid) || sid->valuestring == NULL ||
        !michi_pairing_uuid_valid(sid->valuestring) ||
        strlen(sid->valuestring) >= sizeof(out->session_id)) {
        snprintf(err_field, err_field_len, "%s", "session_id");
        return false;
    }
    /* sequence: required, unsigned integer (0..4294967295). */
    const cJSON *seq = cJSON_GetObjectItem(obj, "sequence");
    if (seq == NULL || !cJSON_IsNumber(seq)) {
        snprintf(err_field, err_field_len, "%s", "sequence");
        return false;
    }
    const double seq_d = seq->valuedouble;
    if (seq_d < 0.0 || seq_d > 4294967295.0 ||
        seq_d != (double)(uint64_t)seq_d) {
        snprintf(err_field, err_field_len, "%s", "sequence");
        return false;
    }
    /* sent_at_ms: required, unsigned Unix epoch ms. Informational ONLY:
     * the local lease timeout never reads it (contract 2.6). */
    const cJSON *sent = cJSON_GetObjectItem(obj, "sent_at_ms");
    if (sent == NULL || !cJSON_IsNumber(sent)) {
        snprintf(err_field, err_field_len, "%s", "sent_at_ms");
        return false;
    }
    const double sent_d = sent->valuedouble;
    /* Integrality is checked BEFORE the int64 cast (a value at the very
     * top of the double range would be UB to cast); the ceiling is far
     * beyond any real epoch-ms value. */
    if (sent_d < 0.0 || sent_d > (double)(INT64_MAX / 2) ||
        sent_d != (double)(int64_t)sent_d) {
        snprintf(err_field, err_field_len, "%s", "sent_at_ms");
        return false;
    }
    /* additionalProperties: false. */
    const char *extra = heartbeat_extra_field(obj);
    if (extra != NULL) {
        snprintf(err_field, err_field_len, "%s", extra);
        return false;
    }
    /* Parse -> copy -> delete: only copies leave this helper. */
    memcpy(out->session_id, sid->valuestring, strlen(sid->valuestring) + 1);
    out->sequence = (uint32_t)seq_d;
    out->sent_at_ms = (int64_t)sent_d;
    return true;
}
