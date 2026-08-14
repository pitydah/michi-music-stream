/* Host-side tests for the signed discovery (MS-05).
 *
 * Compiles the REAL firmware sources - announce.c, discovery_nvs.c and
 * the michi_identity component (sign/verify + base64url + fake NVS) -
 * against the test shims. No reimplementation.
 *
 * Golden vectors: values copied verbatim from the vendored contract
 * bundle contracts/michi-link/vectors/discovery/. The canonical payload
 * must be byte-identical to the Rust crate DiscoveryEngine::canonical_bytes
 * (lexicographic keys, signature excluded) and the crate's signature over
 * that payload must verify - proving wire interop of the announce format.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

#include "discovery_nvs.h"
#include "michi_discovery.h"
#include "michi_identity.h"
#include "nvs.h" /* fake NVS shim: test hooks only */

static int failures = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            printf("  FAIL %s\n", msg);                                     \
            failures++;                                                     \
        }                                                                   \
    } while (0)

/* ── golden vector material (contracts/michi-link/vectors/discovery/) ── */

static const char VEC_DEVICE_ID[] = "550e8400-e29b-41d4-a716-446655440000";
static const char VEC_NAME[] = "Michi Stream Cocina";
static const char VEC_SERVICE[] = "michi-stream-standard";
static const char VEC_API_VERSION[] = "v1-lite";
static const char VEC_HOST[] = "192.168.1.102";
static const char VEC_MICHI_ID[] = "f2UwxQaeA6vA8LO7Cr1nGRr5MStned_Gbmc_ua48qUc";
static const char VEC_PUBLIC_KEY[] =
    "RpHnJr9oP1DXBkPuIMuk0hJ2hAJ5SiWO2hAQVCMGREE";
static const char VEC_NONCE[] = "ChEYHyYtNDtCSVBXXmVscw";
static const char VEC_SIGNATURE[] =
    "9mhYMykseCzFm6Znbd7HT1-9fx8IqJybF6qRV8xb0Da2X7mYoBVO29f5rixgDcxOlakewOXCfgbXqNZGf3iEBQ";

/* The canonical payload of announce-valid.json, byte for byte. */
static const char VEC_ANNOUNCE_CANONICAL[] =
    "{\"api_version\":\"v1-lite\","
    "\"device_id\":\"550e8400-e29b-41d4-a716-446655440000\","
    "\"features\":{\"heartbeat\":true,\"session\":true,\"volume\":true},"
    "\"host\":\"192.168.1.102\","
    "\"michi_id\":\"f2UwxQaeA6vA8LO7Cr1nGRr5MStned_Gbmc_ua48qUc\","
    "\"name\":\"Michi Stream Cocina\","
    "\"nonce\":\"ChEYHyYtNDtCSVBXXmVscw\","
    "\"port\":8600,"
    "\"public_key\":\"RpHnJr9oP1DXBkPuIMuk0hJ2hAJ5SiWO2hAQVCMGREE\","
    "\"roles\":[\"audio_receiver\"],"
    "\"service\":\"michi-stream-standard\","
    "\"timestamp_ms\":1767225600000}";

static michi_discovery_announce_t vec_announce(void)
{
    const michi_discovery_announce_t a = {
        .device_id = VEC_DEVICE_ID,
        .name = VEC_NAME,
        .service = VEC_SERVICE,
        .api_version = VEC_API_VERSION,
        .host = VEC_HOST,
        .port = 8600,
        .feature_session = true,
        .feature_heartbeat = true,
        .feature_volume = true,
        .michi_id = VEC_MICHI_ID,
        .public_key = VEC_PUBLIC_KEY,
        .timestamp_ms = 1767225600000LL,
        .nonce = VEC_NONCE,
    };
    return a;
}

static bool b64_decode(const char *in, uint8_t *out, size_t out_cap,
                       size_t *out_len)
{
    return michi_identity_base64url_decode(in, out, out_cap, out_len) ==
           ESP_OK;
}

/* ── canonical contract constants ─────────────────────────── */

static void test_constants(void)
{
    printf("constants: canonical discovery values\n");
    CHECK(strcmp(MICHI_DISCOVERY_MDNS_SERVICE, "_michi-link") == 0,
          "mDNS service type is _michi-link");
    CHECK(strcmp(MICHI_DISCOVERY_MDNS_PROTO, "_tcp") == 0,
          "mDNS proto is _tcp");
    CHECK(strcmp(MICHI_DISCOVERY_MULTICAST_GROUP, "224.0.0.167") == 0,
          "UDP group is 224.0.0.167");
    CHECK(MICHI_DISCOVERY_MULTICAST_PORT == 53318, "UDP port is 53318");
    CHECK(MICHI_DISCOVERY_UDP_TTL == 1, "IP TTL is 1");
    CHECK(MICHI_DISCOVERY_MAX_DATAGRAM_BYTES == 1200,
          "datagram limit is 1200 bytes");
    CHECK(MICHI_DISCOVERY_ANNOUNCE_INTERVAL_MS == 30000 &&
              MICHI_DISCOVERY_ANNOUNCE_JITTER_MS == 3000,
          "announce cadence is 30 s +-3 s");
    CHECK(strcmp(MICHI_DISCOVERY_ROLE, "audio_receiver") == 0,
          "role is audio_receiver (plain string)");
    CHECK(strcmp(MICHI_DISCOVERY_API_VERSION, "v1-lite") == 0,
          "api_version is v1-lite");
}

/* ── canonical bytes (golden vector) ──────────────────────── */

static void test_canonical_golden(void)
{
    printf("canonical: byte-identical to the crate golden vector\n");
    const michi_discovery_announce_t a = vec_announce();
    char out[512];
    CHECK(michi_discovery_canonical_json(&a, out, sizeof(out)) == ESP_OK,
          "canonical build succeeds");
    CHECK(strcmp(out, VEC_ANNOUNCE_CANONICAL) == 0,
          "canonical bytes match the crate byte for byte");
    CHECK(strstr(out, "signature") == NULL,
          "signature is NOT part of the canonical payload");

    /* Deterministic: same inputs, same bytes. */
    char out2[512];
    CHECK(michi_discovery_canonical_json(&a, out2, sizeof(out2)) == ESP_OK &&
              strcmp(out, out2) == 0,
          "canonical bytes are deterministic");

    /* A changed field changes the payload (the signature covers it). */
    michi_discovery_announce_t b = vec_announce();
    b.timestamp_ms = 1767225600001LL;
    CHECK(michi_discovery_canonical_json(&b, out2, sizeof(out2)) == ESP_OK &&
              strcmp(out, out2) != 0,
          "timestamp change alters the canonical payload");
}

static void test_canonical_rejects_invalid(void)
{
    printf("canonical: invalid fields rejected\n");
    michi_discovery_announce_t a = vec_announce();
    char out[512];

    a.device_id = NULL;
    CHECK(michi_discovery_canonical_json(&a, out, sizeof(out)) ==
              ESP_ERR_INVALID_ARG, "NULL device_id rejected");
    a = vec_announce();
    a.name = "";
    CHECK(michi_discovery_canonical_json(&a, out, sizeof(out)) ==
              ESP_ERR_INVALID_ARG, "empty name rejected");
    a = vec_announce();
    a.port = 0;
    CHECK(michi_discovery_canonical_json(&a, out, sizeof(out)) ==
              ESP_ERR_INVALID_ARG, "port 0 rejected");
    a = vec_announce();
    CHECK(michi_discovery_canonical_json(&a, out, 10) == ESP_ERR_INVALID_SIZE,
          "too-small buffer rejected");
}

/* ── signed datagram (golden signature verifies) ──────────── */

static void test_datagram_golden_signature(void)
{
    printf("datagram: crate signature verifies over our canonical bytes\n");
    uint8_t pk[32], sig[64];
    size_t pk_len = 0, sig_len = 0;
    CHECK(b64_decode(VEC_PUBLIC_KEY, pk, sizeof(pk), &pk_len) &&
              b64_decode(VEC_SIGNATURE, sig, sizeof(sig), &sig_len),
          "golden key material decodes");
    CHECK(pk_len == 32 && sig_len == 64, "golden key lengths are canonical");

    CHECK(michi_identity_verify((const uint8_t *)VEC_ANNOUNCE_CANONICAL,
                                strlen(VEC_ANNOUNCE_CANONICAL), sig, pk),
          "announce-valid signature verifies over the canonical payload");
}

static void check_wire_json(const char *wire, const michi_discovery_announce_t *a)
{
    cJSON *root = cJSON_Parse(wire);
    CHECK(root != NULL, "wire datagram parses as JSON");
    if (root == NULL) {
        return;
    }
    const cJSON *v;
    CHECK((v = cJSON_GetObjectItem(root, "device_id")) != NULL &&
              cJSON_IsString(v) && strcmp(v->valuestring, a->device_id) == 0,
          "wire device_id matches");
    CHECK((v = cJSON_GetObjectItem(root, "name")) != NULL &&
              cJSON_IsString(v) && strcmp(v->valuestring, a->name) == 0,
          "wire name matches");
    CHECK((v = cJSON_GetObjectItem(root, "service")) != NULL &&
              cJSON_IsString(v) && strcmp(v->valuestring, a->service) == 0,
          "wire service matches");
    CHECK((v = cJSON_GetObjectItem(root, "api_version")) != NULL &&
              cJSON_IsString(v) &&
              strcmp(v->valuestring, a->api_version) == 0,
          "wire api_version matches");
    CHECK((v = cJSON_GetObjectItem(root, "host")) != NULL &&
              cJSON_IsString(v) && strcmp(v->valuestring, a->host) == 0,
          "wire host matches");
    CHECK((v = cJSON_GetObjectItem(root, "port")) != NULL &&
              cJSON_IsNumber(v) && v->valueint == (int)a->port,
          "wire port matches");
    CHECK((v = cJSON_GetObjectItem(root, "michi_id")) != NULL &&
              cJSON_IsString(v) && strcmp(v->valuestring, a->michi_id) == 0,
          "wire michi_id matches");
    CHECK((v = cJSON_GetObjectItem(root, "public_key")) != NULL &&
              cJSON_IsString(v) &&
              strcmp(v->valuestring, a->public_key) == 0,
          "wire public_key matches");
    CHECK((v = cJSON_GetObjectItem(root, "nonce")) != NULL &&
              cJSON_IsString(v) && strcmp(v->valuestring, a->nonce) == 0,
          "wire nonce matches");
    CHECK((v = cJSON_GetObjectItem(root, "timestamp_ms")) != NULL &&
              cJSON_IsNumber(v) &&
              (int64_t)v->valuedouble == a->timestamp_ms,
          "wire timestamp_ms matches");
    const cJSON *roles = cJSON_GetObjectItem(root, "roles");
    CHECK(roles != NULL && cJSON_IsArray(roles) &&
              cJSON_GetArraySize(roles) == 1 &&
              strcmp(cJSON_GetArrayItem(roles, 0)->valuestring,
                     MICHI_DISCOVERY_ROLE) == 0,
          "wire roles is exactly [audio_receiver]");
    const cJSON *feat = cJSON_GetObjectItem(root, "features");
    CHECK(feat != NULL &&
              cJSON_IsBool(cJSON_GetObjectItem(feat, "session")) &&
              cJSON_IsBool(cJSON_GetObjectItem(feat, "heartbeat")) &&
              cJSON_IsBool(cJSON_GetObjectItem(feat, "volume")),
          "wire features carries the boolean flags");
    CHECK((v = cJSON_GetObjectItem(root, "signature")) != NULL &&
              cJSON_IsString(v) && strlen(v->valuestring) == 86,
          "wire signature is 86 base64url chars");
    cJSON_Delete(root);
}

static void test_build_announce_golden_inputs(void)
{
    printf("build: signed datagram from the golden inputs\n");
    test_nvs_reset();
    michi_identity_test_reset();
    CHECK(michi_identity_init() == ESP_OK, "identity ready for signing");

    const michi_discovery_announce_t a = vec_announce();
    char wire[MICHI_DISCOVERY_MAX_DATAGRAM_BYTES + 1];
    size_t wire_len = 0;
    CHECK(michi_discovery_build_announce(&a, wire, sizeof(wire),
                                         &wire_len) == ESP_OK,
          "announce build succeeds");
    CHECK(wire_len > 0 && wire_len <= MICHI_DISCOVERY_MAX_DATAGRAM_BYTES,
          "datagram is within the 1200-byte limit");
    check_wire_json(wire, &a);

    /* The embedded signature verifies over the canonical payload with
     * the DEVICE public key (the builder signs with the live identity,
     * not the golden key - the golden signature itself is covered by
     * test_datagram_golden_signature). */
    char canonical[512];
    CHECK(michi_discovery_canonical_json(&a, canonical, sizeof(canonical)) ==
              ESP_OK, "canonical rebuild succeeds");
    uint8_t my_pk[MICHI_IDENTITY_KEY_BYTES];
    CHECK(michi_identity_public_key(my_pk) == ESP_OK,
          "device public key available");
    cJSON *root = cJSON_Parse(wire);
    CHECK(root != NULL, "wire parses for signature extraction");
    if (root != NULL) {
        const cJSON *sig_item = cJSON_GetObjectItem(root, "signature");
        uint8_t sig[64];
        size_t sig_len = 0;
        CHECK(sig_item != NULL &&
                  b64_decode(sig_item->valuestring, sig, sizeof(sig),
                             &sig_len),
              "wire signature decodes");
        CHECK(michi_identity_verify((const uint8_t *)canonical,
                                    strlen(canonical), sig, my_pk),
              "wire signature verifies over the canonical payload");
        cJSON_Delete(root);
    }
}

static void test_build_announce_own_identity(void)
{
    printf("build: signed datagram with the device identity\n");
    test_nvs_reset();
    michi_identity_test_reset();
    CHECK(michi_identity_init() == ESP_OK, "identity ready for signing");

    char michi_id[MICHI_IDENTITY_MICHI_ID_LEN];
    uint8_t pk_raw[MICHI_IDENTITY_KEY_BYTES];
    char pk_b64[MICHI_IDENTITY_PUBLIC_KEY_B64_LEN];
    CHECK(michi_identity_michi_id(michi_id, sizeof(michi_id)) == ESP_OK &&
              michi_identity_public_key(pk_raw) == ESP_OK &&
              michi_identity_base64url_encode(pk_raw, sizeof(pk_raw), pk_b64,
                                              sizeof(pk_b64)) == ESP_OK,
          "identity material available");

    const michi_discovery_announce_t a = {
        .device_id = VEC_DEVICE_ID,
        .name = VEC_NAME,
        .service = VEC_SERVICE,
        .api_version = MICHI_DISCOVERY_API_VERSION,
        .host = VEC_HOST,
        .port = 80,
        .feature_session = false,
        .feature_heartbeat = false,
        .feature_volume = false,
        .michi_id = michi_id,
        .public_key = pk_b64,
        .timestamp_ms = 1234567890123LL,
        .nonce = VEC_NONCE,
    };
    char wire[MICHI_DISCOVERY_MAX_DATAGRAM_BYTES + 1];
    size_t wire_len = 0;
    CHECK(michi_discovery_build_announce(&a, wire, sizeof(wire),
                                         &wire_len) == ESP_OK,
          "announce build succeeds with own identity");
    check_wire_json(wire, &a);

    /* Signature verifies with the device public key; a tampered
     * canonical payload breaks it. */
    char canonical[512];
    CHECK(michi_discovery_canonical_json(&a, canonical, sizeof(canonical)) ==
              ESP_OK, "canonical rebuild succeeds");
    cJSON *root = cJSON_Parse(wire);
    CHECK(root != NULL, "wire parses for signature extraction");
    if (root != NULL) {
        const cJSON *sig_item = cJSON_GetObjectItem(root, "signature");
        uint8_t sig[64];
        size_t sig_len = 0;
        CHECK(sig_item != NULL &&
                  b64_decode(sig_item->valuestring, sig, sizeof(sig),
                             &sig_len),
              "wire signature decodes");
        CHECK(michi_identity_verify((const uint8_t *)canonical,
                                    strlen(canonical), sig, pk_raw),
              "signature verifies with the device public key");
        canonical[strlen(canonical) - 1] ^= 0x01;
        CHECK(!michi_identity_verify((const uint8_t *)canonical,
                                     strlen(canonical), sig, pk_raw),
              "tampered canonical payload breaks the signature");
        cJSON_Delete(root);
    }
}

static void test_build_announce_requires_ready_identity(void)
{
    printf("build: signing requires a READY identity\n");
    test_nvs_reset();
    michi_identity_test_reset();
    const michi_discovery_announce_t a = vec_announce();
    char wire[MICHI_DISCOVERY_MAX_DATAGRAM_BYTES + 1];
    size_t wire_len = 0;
    CHECK(michi_discovery_build_announce(&a, wire, sizeof(wire),
                                         &wire_len) == ESP_ERR_INVALID_STATE,
          "announce build fails while identity is UNINITIALIZED");
}

static void test_build_announce_rejects_invalid(void)
{
    printf("build: invalid fields rejected\n");
    test_nvs_reset();
    michi_identity_test_reset();
    CHECK(michi_identity_init() == ESP_OK, "identity ready");
    char wire[MICHI_DISCOVERY_MAX_DATAGRAM_BYTES + 1];
    size_t wire_len = 0;

    michi_discovery_announce_t a = vec_announce();
    a.public_key = NULL;
    CHECK(michi_discovery_build_announce(&a, wire, sizeof(wire),
                                         &wire_len) == ESP_ERR_INVALID_ARG,
          "NULL public_key rejected");
    a = vec_announce();
    CHECK(michi_discovery_build_announce(&a, wire, 50, &wire_len) ==
              ESP_ERR_INVALID_SIZE,
          "too-small output buffer rejected");
    a = vec_announce();
    CHECK(michi_discovery_build_announce(&a, NULL, sizeof(wire),
                                         &wire_len) == ESP_ERR_INVALID_ARG,
          "NULL output buffer rejected");
}

/* ── server_id lifecycle (NVS persistence) ────────────────── */

static bool uuid_shape(const char *s)
{
    if (s == NULL || strlen(s) != 36 || s[8] != '-' || s[13] != '-' ||
        s[18] != '-' || s[23] != '-' || s[14] != '4') {
        return false;
    }
    for (int i = 0; i < 36; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            continue;
        }
        if (!((s[i] >= '0' && s[i] <= '9') ||
              (s[i] >= 'a' && s[i] <= 'f') ||
              (s[i] >= 'A' && s[i] <= 'F'))) {
            return false;
        }
    }
    const char v = s[19];
    return v == '8' || v == '9' || v == 'a' || v == 'b' || v == 'A' ||
           v == 'B';
}

static void test_server_id_lifecycle(void)
{
    printf("server_id: generate once, stable across reboots\n");
    test_nvs_reset();

    char sid1[MICHI_DISCOVERY_UUID_LEN];
    CHECK(michi_discovery_nvs_get_or_create_server_id(sid1, sizeof(sid1)) ==
              ESP_OK, "first boot mints a server_id");
    CHECK(uuid_shape(sid1), "server_id is a formatted UUID v4");
    CHECK(test_nvs_write_count(MICHI_DISCOVERY_NVS_NAMESPACE) == 1,
          "server_id persisted exactly once");

    /* Same boot, repeated call: no regeneration, no new write. */
    char sid2[MICHI_DISCOVERY_UUID_LEN];
    CHECK(michi_discovery_nvs_get_or_create_server_id(sid2, sizeof(sid2)) ==
              ESP_OK && strcmp(sid1, sid2) == 0,
          "repeated call returns the same server_id");
    CHECK(test_nvs_write_count(MICHI_DISCOVERY_NVS_NAMESPACE) == 1,
          "repeated call does not persist again");

    /* Simulated reboot (store survives): same UUID reloaded. */
    CHECK(michi_discovery_nvs_get_or_create_server_id(sid2, sizeof(sid2)) ==
              ESP_OK && strcmp(sid1, sid2) == 0,
          "reboot reloads the same server_id");
    CHECK(test_nvs_write_count(MICHI_DISCOVERY_NVS_NAMESPACE) == 1,
          "no regeneration across reboots");

    /* Buffer contract. */
    char small[16];
    CHECK(michi_discovery_nvs_get_or_create_server_id(small, sizeof(small)) ==
              ESP_ERR_INVALID_SIZE,
          "too-small buffer rejected");
}

static void test_server_id_corrupt(void)
{
    printf("server_id: corrupt store is never regenerated\n");
    test_nvs_reset();
    char sid[MICHI_DISCOVERY_UUID_LEN];
    CHECK(michi_discovery_nvs_get_or_create_server_id(sid, sizeof(sid)) ==
              ESP_OK, "boot mints a server_id");

    /* Corrupt the stored string. */
    nvs_handle_t h;
    CHECK(nvs_open(MICHI_DISCOVERY_NVS_NAMESPACE, NVS_READWRITE, &h) ==
              ESP_OK &&
              nvs_set_str(h, MICHI_DISCOVERY_NVS_KEY, "not-a-uuid") ==
                  ESP_OK &&
              nvs_commit(h) == ESP_OK,
          "corrupt value re-stored");
    nvs_close(h);

    CHECK(michi_discovery_nvs_get_or_create_server_id(sid, sizeof(sid)) ==
              ESP_ERR_INVALID_RESPONSE,
          "structurally invalid store is rejected (factory reset needed)");
    /* The corrupt value was NOT silently overwritten. */
    uint8_t raw[64];
    size_t raw_len = 0;
    CHECK(test_nvs_get_blob(MICHI_DISCOVERY_NVS_NAMESPACE,
                            MICHI_DISCOVERY_NVS_KEY, raw, sizeof(raw),
                            &raw_len) &&
              raw_len == strlen("not-a-uuid") + 1 &&
              strcmp((const char *)raw, "not-a-uuid") == 0,
          "corrupt value untouched (never regenerated)");
}

static void test_server_id_read_error(void)
{
    printf("server_id: NVS read failure surfaces (no silent mint)\n");
    test_nvs_reset();
    char sid[MICHI_DISCOVERY_UUID_LEN];
    CHECK(michi_discovery_nvs_get_or_create_server_id(sid, sizeof(sid)) ==
              ESP_OK, "boot mints a server_id");

    test_nvs_force_read_error(MICHI_DISCOVERY_NVS_NAMESPACE, true);
    CHECK(michi_discovery_nvs_get_or_create_server_id(sid, sizeof(sid)) ==
              ESP_FAIL,
          "read error surfaces instead of regenerating");
    test_nvs_force_read_error(MICHI_DISCOVERY_NVS_NAMESPACE, false);
}

int main(void)
{
    test_constants();
    test_canonical_golden();
    test_canonical_rejects_invalid();
    test_datagram_golden_signature();
    test_build_announce_golden_inputs();
    test_build_announce_own_identity();
    test_build_announce_requires_ready_identity();
    test_build_announce_rejects_invalid();
    test_server_id_lifecycle();
    test_server_id_corrupt();
    test_server_id_read_error();

    if (failures == 0) {
        printf("test_michi_discovery: all tests passed\n");
        return 0;
    }
    printf("test_michi_discovery: %d check(s) FAILED\n", failures);
    return 1;
}
