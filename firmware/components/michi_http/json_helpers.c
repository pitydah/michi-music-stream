/* JSON access helpers with exact-type semantics (F15: extracted from
 * http_server.c so the host-side tests compile the SAME source - no
 * reimplementation). Uses cJSON; on the host the tests link the system
 * libcjson (CI: apt install libcjson-dev). */

#include "michi_http.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_timer.h"

#include "validators.h"
#include "michi_home.h"

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

bool michi_http_json_get_auth_challenge(
    const cJSON *obj,
    char *client_michi_id, size_t client_id_len,
    char *client_pk, size_t pk_len,
    char *home_id, size_t home_id_len,
    char *err_field, size_t err_field_len)
{
    if (obj == NULL || client_michi_id == NULL || client_pk == NULL ||
        home_id == NULL || err_field == NULL || err_field_len == 0) {
        return false;
    }
    const cJSON *c_id = cJSON_GetObjectItem(obj, "client_michi_id");
    const cJSON *c_pk = cJSON_GetObjectItem(obj, "client_public_key");
    const cJSON *h_id = cJSON_GetObjectItem(obj, "home_id");

    if (c_id == NULL || !cJSON_IsString(c_id) || c_id->valuestring == NULL ||
        !b64url_exact(c_id->valuestring, 43)) {
        snprintf(err_field, err_field_len, "client_michi_id");
        return false;
    }
    if (c_pk == NULL || !cJSON_IsString(c_pk) || c_pk->valuestring == NULL ||
        !b64url_exact(c_pk->valuestring, 43)) {
        snprintf(err_field, err_field_len, "client_public_key");
        return false;
    }
    if (h_id == NULL || !cJSON_IsString(h_id) || h_id->valuestring == NULL ||
        !b64url_exact(h_id->valuestring, 43)) {
        snprintf(err_field, err_field_len, "home_id");
        return false;
    }
    if (client_id_len < 44 || pk_len < 44 || home_id_len < 44) {
        snprintf(err_field, err_field_len, "body");
        return false;
    }
    memcpy(client_michi_id, c_id->valuestring, 44);
    memcpy(client_pk, c_pk->valuestring, 44);
    memcpy(home_id, h_id->valuestring, 44);
    return true;
}

bool michi_http_json_get_auth_session(
    const cJSON *obj,
    char *challenge_id, size_t challenge_id_len,
    char *client_michi_id, size_t client_id_len,
    michi_membership_t *out_membership,
    char *client_signature, size_t sig_len,
    char *err_field, size_t err_field_len)
{
    if (obj == NULL || challenge_id == NULL || client_michi_id == NULL ||
        out_membership == NULL || client_signature == NULL ||
        err_field == NULL || err_field_len == 0) {
        return false;
    }
    const cJSON *cid = cJSON_GetObjectItem(obj, "challenge_id");
    const cJSON *c_id = cJSON_GetObjectItem(obj, "client_michi_id");
    const cJSON *mem = cJSON_GetObjectItem(obj, "membership");
    const cJSON *sig = cJSON_GetObjectItem(obj, "client_signature");

    if (cid == NULL || !cJSON_IsString(cid) || cid->valuestring == NULL ||
        !michi_uuid_valid(cid->valuestring) || strlen(cid->valuestring) >= challenge_id_len) {
        snprintf(err_field, err_field_len, "challenge_id");
        return false;
    }
    if (c_id == NULL || !cJSON_IsString(c_id) || c_id->valuestring == NULL ||
        !b64url_exact(c_id->valuestring, 43) || client_id_len < 44) {
        snprintf(err_field, err_field_len, "client_michi_id");
        return false;
    }
    if (sig == NULL || !cJSON_IsString(sig) || sig->valuestring == NULL ||
        !b64url_exact(sig->valuestring, 86) || sig_len < 87) {
        snprintf(err_field, err_field_len, "client_signature");
        return false;
    }

    if (mem == NULL || !cJSON_IsObject(mem)) {
        snprintf(err_field, err_field_len, "membership");
        return false;
    }

    /* Parse membership fields */
    const cJSON *ver = cJSON_GetObjectItem(mem, "version");
    if (ver == NULL || !cJSON_IsNumber(ver) || ver->valueint != 1) {
        snprintf(err_field, err_field_len, "membership.version");
        return false;
    }
    out_membership->version = 1;

    const cJSON *m_hid = cJSON_GetObjectItem(mem, "home_id");
    if (m_hid == NULL || !cJSON_IsString(m_hid) || !b64url_exact(m_hid->valuestring, 43)) {
        snprintf(err_field, err_field_len, "membership.home_id");
        return false;
    }
    memcpy(out_membership->home_id, m_hid->valuestring, 44);

    const cJSON *m_did = cJSON_GetObjectItem(mem, "device_michi_id");
    if (m_did == NULL || !cJSON_IsString(m_did) || !b64url_exact(m_did->valuestring, 43)) {
        snprintf(err_field, err_field_len, "membership.device_michi_id");
        return false;
    }
    memcpy(out_membership->device_michi_id, m_did->valuestring, 44);

    const cJSON *m_dpk = cJSON_GetObjectItem(mem, "device_public_key");
    if (m_dpk == NULL || !cJSON_IsString(m_dpk) || !b64url_exact(m_dpk->valuestring, 43)) {
        snprintf(err_field, err_field_len, "membership.device_public_key");
        return false;
    }
    memcpy(out_membership->device_public_key, m_dpk->valuestring, 44);

    const cJSON *m_dtype = cJSON_GetObjectItem(mem, "device_type");
    if (m_dtype == NULL || !cJSON_IsString(m_dtype) || m_dtype->valuestring == NULL) {
        snprintf(err_field, err_field_len, "membership.device_type");
        return false;
    }
    snprintf(out_membership->device_type, sizeof(out_membership->device_type), "%s", m_dtype->valuestring);

    const cJSON *m_roles = cJSON_GetObjectItem(mem, "roles");
    if (m_roles == NULL || !cJSON_IsArray(m_roles) || cJSON_GetArraySize(m_roles) == 0) {
        snprintf(err_field, err_field_len, "membership.roles");
        return false;
    }
    out_membership->role_count = 0;
    const cJSON *role_item = NULL;
    cJSON_ArrayForEach(role_item, m_roles) {
        if (!cJSON_IsString(role_item) || role_item->valuestring == NULL ||
            out_membership->role_count >= MICHI_MAX_MEMBERSHIP_ROLES) {
            snprintf(err_field, err_field_len, "membership.roles");
            return false;
        }
        snprintf(out_membership->roles[out_membership->role_count++],
                 MICHI_MAX_ROLE_NAME_LEN, "%s", role_item->valuestring);
    }

    const cJSON *m_issued = cJSON_GetObjectItem(mem, "issued_at");
    if (m_issued == NULL || !cJSON_IsString(m_issued) || m_issued->valuestring == NULL) {
        snprintf(err_field, err_field_len, "membership.issued_at");
        return false;
    }
    snprintf(out_membership->issued_at, sizeof(out_membership->issued_at), "%s", m_issued->valuestring);

    const cJSON *m_ser = cJSON_GetObjectItem(mem, "serial");
    if (m_ser == NULL || !cJSON_IsNumber(m_ser) || m_ser->valuedouble < 1.0) {
        snprintf(err_field, err_field_len, "membership.serial");
        return false;
    }
    out_membership->serial = (uint64_t)m_ser->valuedouble;

    const cJSON *m_sig = cJSON_GetObjectItem(mem, "signature");
    if (m_sig == NULL || !cJSON_IsString(m_sig) || !b64url_exact(m_sig->valuestring, 86)) {
        snprintf(err_field, err_field_len, "membership.signature");
        return false;
    }
    memcpy(out_membership->signature, m_sig->valuestring, 87);

    snprintf(challenge_id, challenge_id_len, "%s", cid->valuestring);
    memcpy(client_michi_id, c_id->valuestring, 44);
    memcpy(client_signature, sig->valuestring, 87);
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
        michi_audio_validate_buffer_ms((uint16_t)buffer_ms) != ESP_OK) {
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
        !michi_uuid_valid(sid->valuestring) ||
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

void michi_http_configure_defaults(httpd_config_t *cfg)
{
    if (cfg == NULL) {
        return;
    }
    cfg->server_port = MICHI_HTTP_PORT;
    cfg->lru_purge_enable = true;
    cfg->max_uri_handlers = 16;
    cfg->stack_size = 8192;
    cfg->recv_wait_timeout = MICHI_HTTP_RECV_WAIT_TIMEOUT_S;
    cfg->send_wait_timeout = MICHI_HTTP_SEND_WAIT_TIMEOUT_S;
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
        if (esp_timer_get_time() >= deadline_us) {
            return ESP_ERR_TIMEOUT;
        }
    }
    if (esp_timer_get_time() >= deadline_us) {
        return ESP_ERR_TIMEOUT;
    }
    buf[received] = '\0';
    *out_len = received;
    return ESP_OK;
}
