/* Host-side tests for the persistent Michi identity (MS-04).
 *
 * Compiles the REAL firmware sources - michi_identity.c, identity_nvs.c
 * and the vendored crypto (Monocypher 4.0.3 + BLAKE3 1.8.6) - against
 * test shims (fake NVS in RAM, deterministic esp_fill_random). No
 * reimplementation of the component.
 *
 * Golden vectors: values copied verbatim from the vendored contract
 * bundle contracts/michi-link/vectors/ (identity/, discovery/, pairing/).
 * The crate (ed25519-dalek) generated those signatures; Monocypher
 * (RFC 8032) must verify them, proving wire interop.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "michi_identity.h"
#include "nvs.h" /* fake NVS shim: test hooks only */
#include "monocypher-ed25519.h" /* seed expansion for blob inspection */

static int failures = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            printf("  FAIL %s\n", msg);                                     \
            failures++;                                                     \
        }                                                                   \
    } while (0)

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* ── golden vector material (contracts/michi-link/vectors/) ── */

/* identity/server-info-standard.json + identity/server-info-hifi.json */
static const char VEC_RECEIVER_PK_B64[] =
    "RpHnJr9oP1DXBkPuIMuk0hJ2hAJ5SiWO2hAQVCMGREE";
static const char VEC_RECEIVER_MICHI_ID[] =
    "f2UwxQaeA6vA8LO7Cr1nGRr5MStned_Gbmc_ua48qUc";

/* pairing/pair-start-valid.json (+ confirm/nonce-altered vectors) */
static const char VEC_PAIR_PK_B64[] =
    "j8oIHv906goIsvcANXl_SZX8-OPcZftDkTPwTYaQQ7E";
static const char VEC_PAIR_MICHI_ID[] =
    "JXcHys3oHoK2xsmQqlWEKi-KH_s4TrxJGw3YbiKP9-U";
static const char VEC_PAIR_SIG_B64[] =
    "5Hg1TwCbzj-x6MaU7mvToRCALEyQXtRKmeJWLmShuXuJWSes16wGptbq573FfY3_H5VWRxPU9LI8hanyNfbXCA";
static const char VEC_PAIR_NONCE_B64[] = "CxIZICcuNTxDSlFYX2ZtdA";
static const char VEC_PAIR_NONCE_ALTERED_B64[] = "DBMaISgvNj1ES1JZYGdudQ";

/* discovery/announce-valid.json: Ed25519 signature over the canonical
 * payload. The canonical payload is DiscoveryEngine::canonical_bytes()
 * of the announce fields (lexicographic keys, signature excluded) -
 * verified byte-for-byte against the Rust crate. */
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
static const char VEC_ANNOUNCE_SIG_B64[] =
    "9mhYMykseCzFm6Znbd7HT1-9fx8IqJybF6qRV8xb0Da2X7mYoBVO29f5rixgDcxOlakewOXCfgbXqNZGf3iEBQ";
/* discovery/announce-signature-altered.json: first byte XOR 0x01 */
static const char VEC_ANNOUNCE_SIG_ALTERED_B64[] =
    "92hYMykseCzFm6Znbd7HT1-9fx8IqJybF6qRV8xb0Da2X7mYoBVO29f5rixgDcxOlakewOXCfgbXqNZGf3iEBQ";

static bool b64_decode(const char *in, uint8_t *out, size_t out_cap,
                       size_t *out_len)
{
    return michi_identity_base64url_decode(in, out, out_cap, out_len) ==
           ESP_OK;
}

/* ── base64url (no padding) ──────────────────────────────── */

static void test_base64url_roundtrip(void)
{
    printf("base64url: roundtrip\n");
    uint8_t data[32];
    for (size_t i = 0; i < sizeof(data); i++) {
        data[i] = (uint8_t)(i * 7 + 3);
    }
    char enc[64];
    CHECK(michi_identity_base64url_encode(data, sizeof(data), enc,
                                          sizeof(enc)) == ESP_OK,
          "encode 32 bytes succeeds");
    CHECK(strlen(enc) == 43, "32 bytes encode to 43 chars");
    CHECK(strchr(enc, '+') == NULL && strchr(enc, '/') == NULL &&
              strchr(enc, '=') == NULL,
          "no standard-alphabet or padding chars");
    uint8_t dec[32];
    size_t dec_len = 0;
    CHECK(b64_decode(enc, dec, sizeof(dec), &dec_len) &&
              dec_len == sizeof(data) && memcmp(dec, data, sizeof(data)) == 0,
          "roundtrip decodes to the same bytes");
}

static void test_base64url_strict(void)
{
    printf("base64url: strict decode\n");
    uint8_t out[64];
    size_t out_len = 0;
    /* Standard alphabet and padding must be rejected on the wire. */
    CHECK(michi_identity_base64url_decode("AB+/=", out, sizeof(out),
                                          &out_len) != ESP_OK,
          "rejects standard base64 (AB+/=)");
    CHECK(michi_identity_base64url_decode("a", out, sizeof(out), &out_len) !=
              ESP_OK,
          "rejects 1-char dangling encoding");
    /* Positive: 2 and 3-char no-pad encodings decode to 1 and 2 bytes. */
    CHECK(michi_identity_base64url_decode("ab", out, sizeof(out), &out_len) ==
              ESP_OK && out_len == 1 && out[0] == 0x69,
          "2 chars decode to 1 byte");
    CHECK(michi_identity_base64url_decode("abc", out, sizeof(out), &out_len) ==
              ESP_OK && out_len == 2 && out[0] == 0x69 && out[1] == 0xb7,
          "3 chars decode to 2 bytes");
    /* Known vector: the 16-byte nonce of the pairing vectors. */
    uint8_t nonce[16] = {11, 18, 25, 32, 39, 46, 53, 60,
                         67, 74, 81, 88, 95, 102, 109, 116};
    char enc[32];
    CHECK(michi_identity_base64url_encode(nonce, sizeof(nonce), enc,
                                          sizeof(enc)) == ESP_OK,
          "encode nonce");
    CHECK(strcmp(enc, VEC_PAIR_NONCE_B64) == 0,
          "nonce encodes to the crate golden value");
}

/* ── michi_id derivation (golden vectors) ────────────────── */

static void test_derive_golden(void)
{
    printf("derive: golden vectors\n");
    uint8_t pk[32];
    size_t pk_len = 0;
    char id[MICHI_IDENTITY_MICHI_ID_LEN];

    CHECK(b64_decode(VEC_RECEIVER_PK_B64, pk, sizeof(pk), &pk_len) &&
              pk_len == 32,
          "receiver pk decodes to 32 bytes");
    CHECK(michi_identity_derive_michi_id(pk, id, sizeof(id)) == ESP_OK &&
              strcmp(id, VEC_RECEIVER_MICHI_ID) == 0,
          "receiver pk derives the golden michi_id");

    CHECK(b64_decode(VEC_PAIR_PK_B64, pk, sizeof(pk), &pk_len) &&
              pk_len == 32,
          "pairing pk decodes to 32 bytes");
    CHECK(michi_identity_derive_michi_id(pk, id, sizeof(id)) == ESP_OK &&
              strcmp(id, VEC_PAIR_MICHI_ID) == 0,
          "pairing pk derives the golden michi_id");

    CHECK(strlen(VEC_RECEIVER_MICHI_ID) == 43 && strlen(VEC_PAIR_MICHI_ID) == 43,
          "golden michi_ids are 43 chars");
}

/* ── Ed25519 interop: crate signatures, Monocypher verifies ─ */

static void test_verify_pairing_vectors(void)
{
    printf("interop: pairing challenge vectors\n");
    uint8_t pk[32], sig[64], nonce[32], nonce2[32];
    size_t pk_len = 0, sig_len = 0, n1_len = 0, n2_len = 0;

    CHECK(b64_decode(VEC_PAIR_PK_B64, pk, sizeof(pk), &pk_len) &&
              b64_decode(VEC_PAIR_SIG_B64, sig, sizeof(sig), &sig_len) &&
              b64_decode(VEC_PAIR_NONCE_B64, nonce, sizeof(nonce), &n1_len) &&
              b64_decode(VEC_PAIR_NONCE_ALTERED_B64, nonce2, sizeof(nonce2),
                         &n2_len),
          "pairing vector material decodes");
    CHECK(pk_len == 32 && sig_len == 64 && n1_len == 16 && n2_len == 16,
          "pairing vector lengths are canonical");

    /* pair-start-valid: signature over the RAW nonce bytes. */
    CHECK(michi_identity_verify(nonce, n1_len, sig, pk),
          "valid challenge signature verifies");
    /* pair-start-nonce-altered: same signature, different nonce. */
    CHECK(!michi_identity_verify(nonce2, n2_len, sig, pk),
          "signature over an altered nonce is rejected");
    /* pair-start-wrong-michi-id: the derived id must NOT match. */
    char id[MICHI_IDENTITY_MICHI_ID_LEN];
    CHECK(michi_identity_derive_michi_id(pk, id, sizeof(id)) == ESP_OK &&
              strcmp(id, VEC_PAIR_MICHI_ID) == 0 &&
              strcmp(id, VEC_RECEIVER_MICHI_ID) != 0,
          "derived id matches the pair client, not the receiver");
}

static void test_verify_discovery_vectors(void)
{
    printf("interop: signed discovery announce vectors\n");
    uint8_t pk[32], sig[64], sig_bad[64];
    size_t pk_len = 0, sig_len = 0, bad_len = 0;
    const size_t msg_len = strlen(VEC_ANNOUNCE_CANONICAL);

    CHECK(b64_decode(VEC_RECEIVER_PK_B64, pk, sizeof(pk), &pk_len) &&
              b64_decode(VEC_ANNOUNCE_SIG_B64, sig, sizeof(sig), &sig_len) &&
              b64_decode(VEC_ANNOUNCE_SIG_ALTERED_B64, sig_bad, sizeof(sig_bad),
                         &bad_len),
          "discovery vector material decodes");
    CHECK(sig_len == 64 && bad_len == 64,
          "discovery signatures are 64 bytes");

    /* announce-valid: crate signature over the canonical payload. */
    CHECK(michi_identity_verify((const uint8_t *)VEC_ANNOUNCE_CANONICAL,
                                msg_len, sig, pk),
          "announce-valid signature verifies over the canonical payload");
    /* announce-signature-altered. */
    CHECK(!michi_identity_verify((const uint8_t *)VEC_ANNOUNCE_CANONICAL,
                                 msg_len, sig_bad, pk),
          "altered announce signature is rejected");
    /* Tampering with the payload itself also breaks verification. */
    uint8_t tampered[512];
    memcpy(tampered, VEC_ANNOUNCE_CANONICAL, msg_len + 1);
    tampered[msg_len - 1] ^= 0x01; /* flip a digit of timestamp_ms */
    CHECK(!michi_identity_verify(tampered, msg_len, sig, pk),
          "tampered canonical payload is rejected");
}

/* ── lifecycle: NVS persistence + state machine ──────────── */

static void test_lifecycle_first_boot(void)
{
    printf("lifecycle: first boot generates and persists once\n");
    test_nvs_reset();
    michi_identity_test_reset();

    CHECK(michi_identity_get_state() == MICHI_IDENTITY_UNINITIALIZED,
          "starts UNINITIALIZED");
    CHECK(michi_identity_init() == ESP_OK, "init succeeds on empty store");
    CHECK(michi_identity_get_state() == MICHI_IDENTITY_READY,
          "READY after generation");
    CHECK(test_nvs_write_count("michi_identity") == 1,
          "store written exactly once on first boot");

    char id[MICHI_IDENTITY_MICHI_ID_LEN];
    CHECK(michi_identity_michi_id(id, sizeof(id)) == ESP_OK &&
              strlen(id) == 43,
          "generated michi_id is 43 chars");
    CHECK(strchr(id, '+') == NULL && strchr(id, '/') == NULL &&
              strchr(id, '=') == NULL,
          "generated michi_id is base64url-nopad");

    /* Re-init is a no-op: no second persist, same identity. */
    char id2[MICHI_IDENTITY_MICHI_ID_LEN];
    CHECK(michi_identity_init() == ESP_OK, "re-init returns ESP_OK");
    CHECK(michi_identity_michi_id(id2, sizeof(id2)) == ESP_OK &&
              strcmp(id, id2) == 0,
          "re-init keeps the same identity");
    CHECK(test_nvs_write_count("michi_identity") == 1,
          "re-init does not persist again");
}

static void test_lifecycle_reload_stable(void)
{
    printf("lifecycle: identity stable across simulated reboots\n");
    test_nvs_reset();
    michi_identity_test_reset();

    CHECK(michi_identity_init() == ESP_OK, "boot 1 generates");
    char id_boot1[MICHI_IDENTITY_MICHI_ID_LEN];
    uint8_t pk_boot1[32];
    CHECK(michi_identity_michi_id(id_boot1, sizeof(id_boot1)) == ESP_OK &&
              michi_identity_public_key(pk_boot1) == ESP_OK,
          "boot 1 identity readable");

    michi_identity_test_reset();
    CHECK(michi_identity_get_state() == MICHI_IDENTITY_UNINITIALIZED,
          "simulated reboot resets RAM state");
    CHECK(michi_identity_init() == ESP_OK, "boot 2 reloads");
    char id_boot2[MICHI_IDENTITY_MICHI_ID_LEN];
    uint8_t pk_boot2[32];
    CHECK(michi_identity_michi_id(id_boot2, sizeof(id_boot2)) == ESP_OK &&
              michi_identity_public_key(pk_boot2) == ESP_OK,
          "boot 2 identity readable");
    CHECK(strcmp(id_boot1, id_boot2) == 0 &&
              memcmp(pk_boot1, pk_boot2, 32) == 0,
          "identity identical across reboots");
    CHECK(test_nvs_write_count("michi_identity") == 1,
          "no regeneration: exactly one persist");

    /* The persisted blob itself reproduces the same identity. */
    uint8_t blob[40];
    size_t blob_len = 0;
    CHECK(test_nvs_get_blob("michi_identity", "seed", blob, sizeof(blob),
                            &blob_len) && blob_len == 40,
          "persisted blob is 40 bytes");
    uint32_t version = 0;
    memcpy(&version, blob, 4);
    CHECK(version == 1, "blob version is 1");
    /* Expand the persisted seed to its public key and derive the id
     * (key_pair wipes its seed argument: use a copy). */
    uint8_t seed_copy[32];
    memcpy(seed_copy, &blob[4], 32);
    uint8_t pk_from_blob[32];
    uint8_t secret_from_blob[64];
    crypto_ed25519_key_pair(secret_from_blob, pk_from_blob, seed_copy);
    char id_from_blob[MICHI_IDENTITY_MICHI_ID_LEN];
    CHECK(memcmp(pk_from_blob, pk_boot1, 32) == 0,
          "persisted seed reproduces the same public key");
    CHECK(michi_identity_derive_michi_id(pk_from_blob, id_from_blob,
                                         sizeof(id_from_blob)) == ESP_OK &&
              strcmp(id_from_blob, id_boot1) == 0,
          "persisted seed reproduces the same michi_id");
}

static void test_lifecycle_corrupt_sticky(void)
{
    printf("lifecycle: corruption requires factory reset (never regenerated)\n");
    test_nvs_reset();
    michi_identity_test_reset();
    CHECK(michi_identity_init() == ESP_OK, "boot generates identity");

    /* Corrupt the stored blob: wrong version. */
    uint8_t blob[40];
    size_t blob_len = 0;
    CHECK(test_nvs_get_blob("michi_identity", "seed", blob, sizeof(blob),
                            &blob_len),
          "stored blob readable for corruption");
    blob[0] = 0xFF;
    {
        /* Re-store the corrupted blob through the fake NVS. */
        nvs_handle_t h;
        CHECK(nvs_open("michi_identity", NVS_READWRITE, &h) == ESP_OK &&
                  nvs_set_blob(h, "seed", blob, blob_len) == ESP_OK,
              "corrupted blob re-stored");
    }

    michi_identity_test_reset();
    CHECK(michi_identity_init() != ESP_OK, "init fails on corrupt blob");
    CHECK(michi_identity_get_state() == MICHI_IDENTITY_CORRUPT,
          "state is CORRUPT");
    CHECK(michi_identity_init() == ESP_ERR_INVALID_STATE,
          "CORRUPT is sticky: init keeps failing");
    CHECK(michi_identity_get_state() == MICHI_IDENTITY_CORRUPT,
          "state remains CORRUPT (no silent regeneration)");
    CHECK(test_nvs_write_count("michi_identity") == 1,
          "corruption never wrote a new seed");
}

static void test_lifecycle_corrupt_read_error(void)
{
    printf("lifecycle: NVS read failure maps to CORRUPT\n");
    test_nvs_reset();
    michi_identity_test_reset();
    CHECK(michi_identity_init() == ESP_OK, "boot generates identity");

    test_nvs_force_read_error("michi_identity", true);
    michi_identity_test_reset();
    CHECK(michi_identity_init() != ESP_OK, "init fails on read error");
    CHECK(michi_identity_get_state() == MICHI_IDENTITY_CORRUPT,
          "read error maps to CORRUPT");
    test_nvs_force_read_error("michi_identity", false);
}

static void test_lifecycle_factory_reset(void)
{
    printf("lifecycle: factory reset is the explicit recovery\n");
    test_nvs_reset();
    michi_identity_test_reset();
    CHECK(michi_identity_init() == ESP_OK, "boot generates identity");
    char id_old[MICHI_IDENTITY_MICHI_ID_LEN];
    CHECK(michi_identity_michi_id(id_old, sizeof(id_old)) == ESP_OK,
          "identity captured");

    CHECK(michi_identity_factory_reset() == ESP_OK, "factory reset erases");
    CHECK(michi_identity_get_state() == MICHI_IDENTITY_UNINITIALIZED,
          "state returns to UNINITIALIZED");
    CHECK(michi_identity_michi_id(id_old, sizeof(id_old)) ==
              ESP_ERR_INVALID_STATE,
          "keys wiped: getter fails while UNINITIALIZED");

    CHECK(michi_identity_init() == ESP_OK, "explicit re-init mints a new one");
    char id_new[MICHI_IDENTITY_MICHI_ID_LEN];
    CHECK(michi_identity_michi_id(id_new, sizeof(id_new)) == ESP_OK,
          "new identity readable");
    CHECK(michi_identity_get_state() == MICHI_IDENTITY_READY,
          "READY after explicit regeneration");

    /* Deterministic stream makes distinct seeds (and ids) overwhelmingly
     * likely; assert they differ to prove regeneration happened. */
    CHECK(strcmp(id_old, id_new) != 0, "fresh identity differs from the old");
}

/* ── sign / verify with the component identity ───────────── */

static void test_sign_verify_roundtrip(void)
{
    printf("sign: Ed25519 roundtrip with the generated identity\n");
    test_nvs_reset();
    michi_identity_test_reset();
    CHECK(michi_identity_init() == ESP_OK, "identity ready for signing");

    uint8_t pk[32];
    CHECK(michi_identity_public_key(pk) == ESP_OK, "public key available");
    char id[MICHI_IDENTITY_MICHI_ID_LEN];
    CHECK(michi_identity_michi_id(id, sizeof(id)) == ESP_OK,
          "michi_id available");
    char id_derived[MICHI_IDENTITY_MICHI_ID_LEN];
    CHECK(michi_identity_derive_michi_id(pk, id_derived, sizeof(id_derived)) ==
              ESP_OK && strcmp(id, id_derived) == 0,
          "component michi_id derives from its own public key");

    const uint8_t msg[] = "michi-sign-test-payload";
    uint8_t sig[64];
    CHECK(michi_identity_sign(msg, sizeof(msg) - 1, sig) == ESP_OK,
          "sign succeeds");
    CHECK(michi_identity_verify(msg, sizeof(msg) - 1, sig, pk),
          "own signature verifies");

    uint8_t bad_sig[64];
    memcpy(bad_sig, sig, sizeof(bad_sig));
    bad_sig[0] ^= 0x01;
    CHECK(!michi_identity_verify(msg, sizeof(msg) - 1, bad_sig, pk),
          "altered signature is rejected");
    uint8_t bad_pk[32];
    memcpy(bad_pk, pk, sizeof(bad_pk));
    bad_pk[0] ^= 0x01;
    CHECK(!michi_identity_verify(msg, sizeof(msg) - 1, sig, bad_pk),
          "wrong public key is rejected");
    CHECK(!michi_identity_verify(msg, sizeof(msg) - 2, sig, pk),
          "truncated message is rejected");
}

static void test_sign_requires_ready(void)
{
    printf("sign: state gating\n");
    test_nvs_reset();
    michi_identity_test_reset();

    uint8_t sig[64];
    CHECK(michi_identity_sign((const uint8_t *)"x", 1, sig) ==
              ESP_ERR_INVALID_STATE,
          "sign before init fails");
    uint8_t pk[32];
    CHECK(michi_identity_public_key(pk) == ESP_ERR_INVALID_STATE,
          "public key before init fails");

    /* verify() is stateless: usable in any state (pairing needs it). */
    uint8_t vec_pk[32], vec_sig[64], nonce[32];
    size_t pk_len = 0, sig_len = 0, nonce_len = 0;
    if (b64_decode(VEC_PAIR_PK_B64, vec_pk, sizeof(vec_pk), &pk_len) &&
        b64_decode(VEC_PAIR_SIG_B64, vec_sig, sizeof(vec_sig), &sig_len) &&
        b64_decode(VEC_PAIR_NONCE_B64, nonce, sizeof(nonce), &nonce_len)) {
        CHECK(michi_identity_verify(nonce, nonce_len, vec_sig, vec_pk),
              "verify works before init (stateless)");
    } else {
        CHECK(0, "vector decode for stateless verify");
    }
}

int main(void)
{
    test_base64url_roundtrip();
    test_base64url_strict();
    test_derive_golden();
    test_verify_pairing_vectors();
    test_verify_discovery_vectors();
    test_lifecycle_first_boot();
    test_lifecycle_reload_stable();
    test_lifecycle_corrupt_sticky();
    test_lifecycle_corrupt_read_error();
    test_lifecycle_factory_reset();
    test_sign_verify_roundtrip();
    test_sign_requires_ready();

    if (failures == 0) {
        printf("test_michi_identity: all tests passed\n");
        return 0;
    }
    printf("test_michi_identity: %d check(s) FAILED\n", failures);
    return 1;
}
