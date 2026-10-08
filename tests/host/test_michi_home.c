/* Host-side tests for Michi Home Membership & Authentication (Trust Architecture V2).
 *
 * Compiles the REAL firmware sources:
 *   - michi_home.c
 *   - michi_auth.c
 *   - factory_cfg.c
 *   - validators.c
 *   - michi_identity.c (+ Monocypher & BLAKE3)
 * against test shims (fake NVS in RAM, fake esp_timer, FreeRTOS shims).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "michi_home.h"
#include "michi_auth.h"
#include "validators.h"
#include "michi_identity.h"
#include "monocypher-ed25519.h"
#include "nvs.h"
#include "esp_timer.h"

static int failures = 0;

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            printf("  FAIL %s (line %d)\n", msg, __LINE__); \
            failures++; \
        } \
    } while (0)

/* Helper: generate keypair from seed */
static void make_keypair(uint8_t seed_byte, uint8_t sk[64], uint8_t pk[32], char pk_b64[44], char michi_id_b64[44])
{
    uint8_t seed[32];
    memset(seed, seed_byte, sizeof(seed));
    crypto_ed25519_key_pair(sk, pk, seed);
    michi_identity_base64url_encode(pk, 32, pk_b64, 44);
    michi_identity_derive_michi_id(pk, michi_id_b64, 44);
}

/* ── 1. Validators ────────────────────────────────────────── */
static void test_validators(void)
{
    printf("validators: uuid and token matching\n");

    CHECK(michi_uuid_valid("550e8400-e29b-41d4-a716-446655440000"), "valid uuid v4 lowercase");
    CHECK(michi_uuid_valid("a1b2c3d4-e5f6-7890-abcd-ef1234567890"), "valid uuid v4 hex");
    CHECK(!michi_uuid_valid("550E8400-E29B-41D4-A716-446655440000"), "rejects uppercase hex");
    CHECK(!michi_uuid_valid("550e8400-e29b-41d4-a716"), "rejects short string");
    CHECK(!michi_uuid_valid("550e8400-e29b-41d4-a716-446655440000-extra"), "rejects long string");
    CHECK(!michi_uuid_valid("550e8400_e29b_41d4_a716_446655440000"), "rejects bad delimiter");
    CHECK(!michi_uuid_valid(NULL), "rejects NULL");

    uint8_t a[32] = {1, 2, 3};
    uint8_t b[32] = {1, 2, 3};
    uint8_t c[32] = {1, 2, 4};
    CHECK(michi_token_matches(a, b, sizeof(a)), "token matches identical bytes");
    CHECK(!michi_token_matches(a, c, sizeof(a)), "token mismatch detected");
}

/* ── 2. Provisioning & NVS Store ──────────────────────────── */
static void test_home_provisioning(void)
{
    printf("home: provisioning and nvs persistence\n");

    test_nvs_reset();
    CHECK(michi_identity_init() == ESP_OK, "identity initialized");
    CHECK(michi_home_init() == ESP_OK, "home initialized");
    CHECK(!michi_home_is_provisioned(), "initially unprovisioned");

    char home_id_in[MICHI_HOME_ID_LEN] = "FU1FL-wFLfsfew3qpbR7XjDkmStWZY4g84MyW-zXPOs";
    uint8_t root_pk_in[MICHI_HOME_KEY_BYTES];
    memset(root_pk_in, 0x42, sizeof(root_pk_in));

    CHECK(michi_home_set_credentials(home_id_in, root_pk_in) == ESP_OK, "provision home");
    CHECK(michi_home_is_provisioned(), "now provisioned");

    char home_id_out[MICHI_HOME_ID_LEN] = {0};
    uint8_t root_pk_out[MICHI_HOME_KEY_BYTES] = {0};
    CHECK(michi_home_get_id(home_id_out, sizeof(home_id_out)) == ESP_OK, "get home id");
    CHECK(strcmp(home_id_out, home_id_in) == 0, "home id matches");
    CHECK(michi_home_get_root_public_key(root_pk_out) == ESP_OK, "get root pk");
    CHECK(memcmp(root_pk_out, root_pk_in, sizeof(root_pk_in)) == 0, "root pk matches");

    /* Test erase */
    CHECK(michi_home_erase() == ESP_OK, "erase home");
    CHECK(!michi_home_is_provisioned(), "unprovisioned after erase");
}

/* ── 3. Canonical Membership Serialization & Verification ─── */
static void test_membership_verification(void)
{
    printf("home: canonical membership serialization and verification\n");

    /* Generate Home Root Authority */
    uint8_t root_sk[64], root_pk[32];
    char root_pk_b64[44], root_michi_id[44];
    make_keypair(0x11, root_sk, root_pk, root_pk_b64, root_michi_id);
    const char *home_id = root_michi_id;

    /* Generate Client device */
    uint8_t client_sk[64], client_pk[32];
    char client_pk_b64[44], client_michi_id[44];
    make_keypair(0x22, client_sk, client_pk, client_pk_b64, client_michi_id);

    /* Construct membership */
    michi_membership_t mem;
    memset(&mem, 0, sizeof(mem));
    mem.version = 1;
    snprintf(mem.home_id, sizeof(mem.home_id), "%s", home_id);
    snprintf(mem.device_michi_id, sizeof(mem.device_michi_id), "%s", client_michi_id);
    snprintf(mem.device_public_key, sizeof(mem.device_public_key), "%s", client_pk_b64);
    snprintf(mem.device_type, sizeof(mem.device_type), "server");
    snprintf(mem.roles[0], sizeof(mem.roles[0]), "music_server");
    mem.role_count = 1;
    snprintf(mem.issued_at, sizeof(mem.issued_at), "2026-10-04T12:00:00Z");
    mem.serial = 1;

    /* Build canonical bytes and sign with root authority */
    uint8_t canon[512];
    size_t canon_len = michi_home_canonical_membership_bytes(
        mem.home_id, mem.device_michi_id, mem.device_public_key,
        mem.device_type, mem.roles, mem.role_count,
        mem.issued_at, mem.serial,
        canon, sizeof(canon));
    CHECK(canon_len > 0, "canonical membership bytes built");

    uint8_t sig_raw[64];
    crypto_ed25519_sign(sig_raw, root_sk, canon, canon_len);
    michi_identity_base64url_encode(sig_raw, sizeof(sig_raw), mem.signature, sizeof(mem.signature));

    /* Verify membership */
    CHECK(michi_home_verify_membership(&mem, root_pk), "valid membership verifies");

    /* Tamper signature */
    mem.signature[10] ^= 0x01;
    CHECK(!michi_home_verify_membership(&mem, root_pk), "tampered signature fails");
    mem.signature[10] ^= 0x01;

    /* Tamper device_michi_id (identity coherence check) */
    mem.device_michi_id[0] ^= 0x01;
    CHECK(!michi_home_verify_membership(&mem, root_pk), "mismatched device michi_id fails coherence check");
    mem.device_michi_id[0] ^= 0x01;

    /* Version != 1 */
    mem.version = 2;
    CHECK(!michi_home_verify_membership(&mem, root_pk), "version != 1 fails");
    mem.version = 1;
}

/* ── 4. Mutual Device & Server Authentication ─────────────── */
static void test_device_and_server_auth(void)
{
    printf("home: mutual challenge-response authentication\n");

    const char *home_id = "test-home-id-43chars-base64url-nopad-123456";
    const char *server_michi_id = "test-server-michi-id-43chars-base64url-123";
    const char *client_michi_id = "test-client-michi-id-43chars-base64url-123";
    const char *challenge_id = "550e8400-e29b-41d4-a716-446655440001";
    const char *challenge_nonce = "MDEyMzQ1Njc4OWFiY2RlZg";

    /* Generate client keypair */
    uint8_t client_sk[64], client_pk[32];
    char client_pk_b64[44], client_id[44];
    make_keypair(0x33, client_sk, client_pk, client_pk_b64, client_id);

    /* Client signs challenge */
    uint8_t auth_payload[512];
    size_t written = snprintf((char *)auth_payload, sizeof(auth_payload),
                              "%s%s%s%s%s%s",
                              MICHI_HOME_DOMAIN_DEVICE_AUTH,
                              home_id, server_michi_id, client_michi_id,
                              challenge_id, challenge_nonce);

    uint8_t client_sig[64];
    crypto_ed25519_sign(client_sig, client_sk, auth_payload, written);

    /* Verify device auth signature */
    CHECK(michi_home_verify_device_auth(home_id, server_michi_id, client_michi_id,
                                        challenge_id, challenge_nonce,
                                        client_sig, client_pk),
          "device auth signature verifies");

    /* Wrong nonce fails */
    CHECK(!michi_home_verify_device_auth(home_id, server_michi_id, client_michi_id,
                                         challenge_id, "wrong-nonce",
                                         client_sig, client_pk),
          "wrong nonce rejected");

    /* Server signs mutual confirmation signature */
    const char *session_token = "ephemeral-ram-session-token-32bytes-b64";
    uint8_t server_sig[64];
    CHECK(michi_home_sign_server_auth(home_id, server_michi_id, client_michi_id,
                                      challenge_id, session_token, server_sig) == ESP_OK,
          "server mutual auth signing succeeds");

    /* Verify server signature using receiver identity */
    uint8_t server_pk[32];
    CHECK(michi_identity_public_key(server_pk) == ESP_OK, "got server public key");

    uint8_t s_payload[512];
    size_t s_len = snprintf((char *)s_payload, sizeof(s_payload),
                            "%s%s%s%s%s%s",
                            MICHI_HOME_DOMAIN_SERVER_AUTH,
                            home_id, server_michi_id, client_michi_id,
                            challenge_id, session_token);
    CHECK(crypto_ed25519_check(server_sig, server_pk, s_payload, s_len) == 0,
          "server confirmation signature verifies under server public key");
}

/* ── 5. In-RAM Challenge & Session State Manager ─────────── */
static void test_michi_auth_manager(void)
{
    printf("auth: in-RAM challenge and session state manager\n");

    michi_auth_reset();

    /* Provision home */
    uint8_t root_sk[64], root_pk[32];
    char root_pk_b64[44], root_michi_id[44];
    make_keypair(0x55, root_sk, root_pk, root_pk_b64, root_michi_id);
    michi_home_set_credentials(root_michi_id, root_pk);

    /* Generate client */
    uint8_t client_sk[64], client_pk[32];
    char client_pk_b64[44], client_michi_id[44];
    make_keypair(0x66, client_sk, client_pk, client_pk_b64, client_michi_id);

    /* 1. Create challenge */
    char challenge_id[MICHI_AUTH_CHALLENGE_ID_LEN] = {0};
    char nonce[MICHI_AUTH_NONCE_B64_LEN] = {0};
    uint32_t expires_in = 0;

    /* Rejects mismatched home_id */
    CHECK(michi_auth_create_challenge(client_michi_id, client_pk_b64, "wrong-home-id",
                                      challenge_id, sizeof(challenge_id),
                                      nonce, sizeof(nonce), &expires_in) == ESP_ERR_NOT_ALLOWED,
          "challenge rejected for mismatched home");

    CHECK(michi_auth_create_challenge(client_michi_id, client_pk_b64, root_michi_id,
                                      challenge_id, sizeof(challenge_id),
                                      nonce, sizeof(nonce), &expires_in) == ESP_OK,
          "challenge created successfully");
    CHECK(michi_uuid_valid(challenge_id), "challenge id is valid UUID");
    CHECK(strlen(nonce) == 22, "nonce is 22 chars base64url");
    CHECK(expires_in == 60, "challenge ttl is 60s");

    /* 2. Client prepares membership certificate */
    michi_membership_t mem;
    memset(&mem, 0, sizeof(mem));
    mem.version = 1;
    snprintf(mem.home_id, sizeof(mem.home_id), "%s", root_michi_id);
    snprintf(mem.device_michi_id, sizeof(mem.device_michi_id), "%s", client_michi_id);
    snprintf(mem.device_public_key, sizeof(mem.device_public_key), "%s", client_pk_b64);
    snprintf(mem.device_type, sizeof(mem.device_type), "server");
    snprintf(mem.roles[0], sizeof(mem.roles[0]), "music_server");
    mem.role_count = 1;
    snprintf(mem.issued_at, sizeof(mem.issued_at), "2026-10-04T12:00:00Z");
    mem.serial = 1;

    uint8_t canon[512];
    size_t c_len = michi_home_canonical_membership_bytes(
        mem.home_id, mem.device_michi_id, mem.device_public_key,
        mem.device_type, mem.roles, mem.role_count,
        mem.issued_at, mem.serial, canon, sizeof(canon));
    uint8_t mem_sig[64];
    crypto_ed25519_sign(mem_sig, root_sk, canon, c_len);
    michi_identity_base64url_encode(mem_sig, sizeof(mem_sig), mem.signature, sizeof(mem.signature));

    /* 3. Client signs challenge */
    char server_michi_id[MICHI_HOME_ID_LEN];
    michi_identity_michi_id(server_michi_id, sizeof(server_michi_id));

    uint8_t auth_payload[512];
    size_t auth_len = snprintf((char *)auth_payload, sizeof(auth_payload),
                              "%s%s%s%s%s%s",
                              MICHI_HOME_DOMAIN_DEVICE_AUTH,
                              root_michi_id, server_michi_id, client_michi_id,
                              challenge_id, nonce);
    uint8_t client_sig_raw[64];
    crypto_ed25519_sign(client_sig_raw, client_sk, auth_payload, auth_len);
    char client_sig_b64[MICHI_HOME_SIG_B64_LEN];
    michi_identity_base64url_encode(client_sig_raw, sizeof(client_sig_raw), client_sig_b64, sizeof(client_sig_b64));

    /* 4. Complete session handshake */
    char token[MICHI_AUTH_TOKEN_B64_LEN] = {0};
    char server_sig_b64[MICHI_HOME_SIG_B64_LEN] = {0};
    uint32_t session_expires = 0;

    esp_err_t res = michi_auth_verify_and_create_session(
        challenge_id, client_michi_id, &mem, client_sig_b64,
        token, sizeof(token), server_sig_b64, sizeof(server_sig_b64),
        &session_expires);
    CHECK(res == ESP_OK, "session created successfully");
    CHECK(strlen(token) == 43, "token is 43 chars base64url");
    CHECK(session_expires == 3600, "session ttl is 3600s");
    CHECK(strlen(server_sig_b64) == 86, "server signature is 86 chars base64url");

    /* 5. Anti-replay: challenge consumed on first use */
    res = michi_auth_verify_and_create_session(
        challenge_id, client_michi_id, &mem, client_sig_b64,
        token, sizeof(token), server_sig_b64, sizeof(server_sig_b64),
        &session_expires);
    CHECK(res == ESP_ERR_NOT_FOUND, "replaying consumed challenge fails with NOT_FOUND");

    /* 6. Validate session token */
    char out_id[MICHI_IDENTITY_MICHI_ID_LEN] = {0};
    CHECK(michi_auth_validate_token(token, out_id, sizeof(out_id)), "session token validates");
    CHECK(strcmp(out_id, client_michi_id) == 0, "token maps to client michi_id");

    CHECK(!michi_auth_validate_token("invalid-random-token", out_id, sizeof(out_id)), "bogus token rejected");

    /* 7. Role-based permissions */
    CHECK(michi_auth_validate_token_perm(token, MICHI_PERM_PLAYBACK, out_id, sizeof(out_id)) == ESP_OK,
          "music_server token has playback permission");
    CHECK(michi_auth_validate_token_perm(token, MICHI_PERM_VOLUME, out_id, sizeof(out_id)) == ESP_OK,
          "music_server token has volume permission");
    CHECK(michi_auth_validate_token_perm(token, MICHI_PERM_OTA, out_id, sizeof(out_id)) == ESP_ERR_INVALID_STATE,
          "music_server token lacks OTA permission (403)");
    CHECK(michi_auth_validate_token_perm("invalid-random-token", MICHI_PERM_PLAYBACK, out_id, sizeof(out_id)) == ESP_ERR_NOT_FOUND,
          "bogus token returns NOT_FOUND (401)");
}

/* ── 6. DoS Challenge Protection & Slot Reuse ──────────────── */
static void test_challenge_dos_protection(void)
{
    printf("michi_auth: DoS challenge slot allocation and reuse\n");
    uint8_t ca_sk[64], ca_pk[32];
    char ca_pk_b64[MICHI_HOME_PUBKEY_B64_LEN], ca_id[MICHI_IDENTITY_MICHI_ID_LEN];
    make_keypair(0x41, ca_sk, ca_pk, ca_pk_b64, ca_id);

    char home_id[MICHI_HOME_ID_LEN];
    michi_home_get_id(home_id, sizeof(home_id));

    char cid1[MICHI_AUTH_CHALLENGE_ID_LEN];
    char nonce1[MICHI_AUTH_NONCE_B64_LEN];
    uint32_t exp = 0;

    /* Slot reuse for same client */
    CHECK(michi_auth_create_challenge(ca_id, ca_pk_b64, home_id,
                                      cid1, sizeof(cid1), nonce1, sizeof(nonce1), &exp) == ESP_OK,
          "client-A creates challenge 1");
    char cid2[MICHI_AUTH_CHALLENGE_ID_LEN];
    char nonce2[MICHI_AUTH_NONCE_B64_LEN];
    CHECK(michi_auth_create_challenge(ca_id, ca_pk_b64, home_id,
                                      cid2, sizeof(cid2), nonce2, sizeof(nonce2), &exp) == ESP_OK,
          "client-A creates challenge 2 (reuses slot)");
    CHECK(strcmp(cid1, cid2) == 0, "existing challenge ID returned on client slot (idempotency)");

    /* Fill remaining slots up to capacity 16 */
    for (int i = 1; i < 16; i++) {
        uint8_t c_sk[64], c_pk[32];
        char c_pk_b64[MICHI_HOME_PUBKEY_B64_LEN], c_id[MICHI_IDENTITY_MICHI_ID_LEN];
        make_keypair((uint8_t)(0x50 + i), c_sk, c_pk, c_pk_b64, c_id);

        char c[MICHI_AUTH_CHALLENGE_ID_LEN], n[MICHI_AUTH_NONCE_B64_LEN];
        CHECK(michi_auth_create_challenge(c_id, c_pk_b64, home_id,
                                          c, sizeof(c), n, sizeof(n), &exp) == ESP_OK,
              "fill challenge slot");
    }

    /* 17th client rejected without evicting others */
    uint8_t ov_sk[64], ov_pk[32];
    char ov_pk_b64[MICHI_HOME_PUBKEY_B64_LEN], ov_id[MICHI_IDENTITY_MICHI_ID_LEN];
    make_keypair(0x99, ov_sk, ov_pk, ov_pk_b64, ov_id);

    char c_over[MICHI_AUTH_CHALLENGE_ID_LEN], n_over[MICHI_AUTH_NONCE_B64_LEN];
    CHECK(michi_auth_create_challenge(ov_id, ov_pk_b64, home_id,
                                      c_over, sizeof(c_over), n_over, sizeof(n_over), &exp) == ESP_ERR_NO_MEM,
          "17th client rejected with NO_MEM when table is full");

    /* Client-A can still refresh its slot */
    CHECK(michi_auth_create_challenge(ca_id, ca_pk_b64, home_id,
                                      cid1, sizeof(cid1), nonce1, sizeof(nonce1), &exp) == ESP_OK,
          "client-A can refresh its existing slot even when table is full");
}

/* ── 7. Factory Config Parser (MICHI-F1) & Device Membership ── */
static void test_factory_config(void)
{
    printf("factory_cfg: MICHI-F1 parsing and device membership\n");

    const char *payload_lines =
        "MICHI-F1\n"
        "HOME_ID=FU1FL-wFLfsfew3qpbR7XjDkmStWZY4g84MyW-zXPOs\n"
        "ROOT_PUBLIC_KEY=SSUmCh_mEGLUkwz9IJyZVv9MapeD_PCkKI17twD3c_g\n"
        "WIFI_SSID=MyHomeWiFi\n"
        "WIFI_PASSWORD=SecretPassword123\n";

    michi_factory_cfg_t cfg;
    CHECK(michi_factory_cfg_parse(payload_lines, strlen(payload_lines), &cfg) == ESP_OK,
          "parse line-delimited MICHI-F1 config");
    CHECK(strcmp(cfg.home_id, "FU1FL-wFLfsfew3qpbR7XjDkmStWZY4g84MyW-zXPOs") == 0, "home_id parsed");
    CHECK(strcmp(cfg.wifi_ssid, "MyHomeWiFi") == 0, "wifi_ssid parsed");
    CHECK(strcmp(cfg.wifi_password, "SecretPassword123") == 0, "wifi_password parsed");

    const char *payload_json =
        "{\"home_id\":\"FU1FL-wFLfsfew3qpbR7XjDkmStWZY4g84MyW-zXPOs\","
        "\"root_public_key\":\"SSUmCh_mEGLUkwz9IJyZVv9MapeD_PCkKI17twD3c_g\","
        "\"wifi_ssid\":\"TestWiFi\",\"wifi_password\":\"pass\"}";

    CHECK(michi_factory_cfg_parse(payload_json, strlen(payload_json), &cfg) == ESP_OK,
          "parse json MICHI-F1 config");
    CHECK(strcmp(cfg.wifi_ssid, "TestWiFi") == 0, "json wifi_ssid parsed");

    /* Device membership handling: coherent with local device identity */
    uint8_t root_sk[64], root_pk[32];
    char root_pk_b64[44], root_michi_id[44];
    make_keypair(0x77, root_sk, root_pk, root_pk_b64, root_michi_id);

    michi_home_erase();
    michi_home_set_credentials(root_michi_id, root_pk);
    CHECK(!michi_home_has_device_membership(), "no device membership initially");

    uint8_t dev_pk[32];
    char dev_pk_b64[44], dev_michi_id[44];
    CHECK(michi_identity_public_key(dev_pk) == ESP_OK, "get local dev pk");
    CHECK(michi_identity_base64url_encode(dev_pk, 32, dev_pk_b64, sizeof(dev_pk_b64)) == ESP_OK, "encode local dev pk");
    CHECK(michi_identity_michi_id(dev_michi_id, sizeof(dev_michi_id)) == ESP_OK, "get local dev michi_id");

    michi_membership_t dev_mem;
    memset(&dev_mem, 0, sizeof(dev_mem));
    dev_mem.version = 1;
    snprintf(dev_mem.home_id, sizeof(dev_mem.home_id), "%s", root_michi_id);
    snprintf(dev_mem.device_michi_id, sizeof(dev_mem.device_michi_id), "%s", dev_michi_id);
    snprintf(dev_mem.device_public_key, sizeof(dev_mem.device_public_key), "%s", dev_pk_b64);
    snprintf(dev_mem.device_type, sizeof(dev_mem.device_type), "stream");
    snprintf(dev_mem.roles[0], sizeof(dev_mem.roles[0]), "audio_receiver");
    dev_mem.role_count = 1;
    snprintf(dev_mem.issued_at, sizeof(dev_mem.issued_at), "2026-10-04T12:00:00Z");
    dev_mem.serial = 100;

    uint8_t canon[512];
    size_t c_len = michi_home_canonical_membership_bytes(
        dev_mem.home_id, dev_mem.device_michi_id, dev_mem.device_public_key,
        dev_mem.device_type, dev_mem.roles, dev_mem.role_count,
        dev_mem.issued_at, dev_mem.serial, canon, sizeof(canon));
    uint8_t sig_raw[64];
    crypto_ed25519_sign(sig_raw, root_sk, canon, c_len);
    michi_identity_base64url_encode(sig_raw, sizeof(sig_raw), dev_mem.signature, sizeof(dev_mem.signature));

    CHECK(michi_home_set_device_membership(&dev_mem) == ESP_OK, "set device membership");
    CHECK(michi_home_has_device_membership(), "has device membership now");

    michi_membership_t loaded_mem;
    CHECK(michi_home_get_device_membership(&loaded_mem) == ESP_OK, "get device membership");
    CHECK(strcmp(loaded_mem.device_michi_id, dev_michi_id) == 0, "device michi_id matches");
    CHECK(strcmp(loaded_mem.roles[0], "audio_receiver") == 0, "role matches");

    /* Transactional rejection: incoherent home_id vs root_public_key */
    michi_home_erase();
    char bad_json[1024];
    snprintf(bad_json, sizeof(bad_json),
             "{\"home_id\":\"wrong_home_id_length_43_chars_xxxxxxxxxxxx\",\"root_public_key\":\"%s\",\"wifi_ssid\":\"HomeWiFi\"}",
             root_pk_b64);
    CHECK(michi_home_import_factory_cfg(bad_json, strlen(bad_json)) == ESP_ERR_INVALID_ARG,
          "import rejects incoherent home_id vs root_pk");
    CHECK(!michi_home_is_provisioned(), "home remains unprovisioned after rejected import");

    /* Incoherent device membership identity vs local device identity */
    char wrong_dev_json[1024];
    snprintf(wrong_dev_json, sizeof(wrong_dev_json),
             "{\"home_id\":\"%s\",\"root_public_key\":\"%s\",\"wifi_ssid\":\"HomeWiFi\","
             "\"device_membership\":{\"version\":1,\"home_id\":\"%s\",\"device_michi_id\":\"foreign_device_id_43_chars_xxxxxxxxxxxxxxx\","
             "\"device_public_key\":\"%s\",\"device_type\":\"stream\",\"roles\":[\"audio_receiver\"],"
             "\"issued_at\":\"2026-10-04T12:00:00Z\",\"serial\":100,\"signature\":\"%s\"}}",
             root_michi_id, root_pk_b64, root_michi_id, dev_pk_b64, dev_mem.signature);
    CHECK(michi_home_import_factory_cfg(wrong_dev_json, strlen(wrong_dev_json)) == ESP_ERR_INVALID_ARG,
          "import rejects incoherent device_michi_id vs local identity");
    CHECK(!michi_home_is_provisioned(), "home remains unprovisioned after rejected identity mismatch");

    /* One-shot factory_cfg import with coherent identity & partition wipe */
    char json_with_mem[1024];
    snprintf(json_with_mem, sizeof(json_with_mem),
             "{\"home_id\":\"%s\",\"root_public_key\":\"%s\",\"wifi_ssid\":\"HomeWiFi\","
             "\"device_membership\":{\"version\":1,\"home_id\":\"%s\",\"device_michi_id\":\"%s\","
             "\"device_public_key\":\"%s\",\"device_type\":\"stream\",\"roles\":[\"audio_receiver\"],"
             "\"issued_at\":\"2026-10-04T12:00:00Z\",\"serial\":100,\"signature\":\"%s\"}}",
             root_michi_id, root_pk_b64, root_michi_id, dev_michi_id, dev_pk_b64, dev_mem.signature);

    michi_factory_cfg_set_test_partition_data(json_with_mem);
    CHECK(michi_factory_cfg_check_and_import() == ESP_OK, "one-shot check_and_import succeeds");
    CHECK(michi_home_is_provisioned(), "home is now provisioned");
    CHECK(michi_home_has_device_membership(), "device membership imported from partition");

    /* Real binary blob generated by michi_ecosystem.infrastructure.flashing.provisioning_blob */
    const uint8_t eco_wifi_blob[] = {
        0x4d, 0x49, 0x43, 0x48, 0x49, 0x2d, 0x46, 0x31, /* "MICHI-F1" */
        0x01,                                           /* schema 1 */
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, /* nonce (starts with 0x00!) */
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
        0x00, 0x58,                                     /* len = 88 (starts with 0x00!) */
        /* payload JSON */
        0x7b, 0x22, 0x64, 0x65, 0x76, 0x69, 0x63, 0x65, 0x5f, 0x6e, 0x61, 0x6d, 0x65, 0x22, 0x3a, 0x22,
        0x4c, 0x69, 0x76, 0x69, 0x6e, 0x67, 0x20, 0x53, 0x74, 0x72, 0x65, 0x61, 0x6d, 0x22, 0x2c, 0x22,
        0x70, 0x61, 0x73, 0x73, 0x77, 0x6f, 0x72, 0x64, 0x22, 0x3a, 0x22, 0x73, 0x75, 0x70, 0x33, 0x72,
        0x73, 0x65, 0x63, 0x72, 0x65, 0x74, 0x22, 0x2c, 0x22, 0x72, 0x65, 0x67, 0x69, 0x6f, 0x6e, 0x22,
        0x3a, 0x22, 0x41, 0x52, 0x22, 0x2c, 0x22, 0x73, 0x73, 0x69, 0x64, 0x22, 0x3a, 0x22, 0x4d, 0x69,
        0x63, 0x68, 0x69, 0x4e, 0x65, 0x74, 0x22, 0x7d,
        /* CRC32 big-endian */
        0x14, 0x60, 0x2d, 0xc7
    };

    michi_factory_cfg_t eco_cfg;
    CHECK(michi_factory_cfg_parse((const char *)eco_wifi_blob, sizeof(eco_wifi_blob), &eco_cfg) == ESP_OK,
          "parse real Ecosystem MICHI-F1 binary blob with schema 1 and big-endian CRC32");
    CHECK(strcmp(eco_cfg.wifi_ssid, "MichiNet") == 0, "Ecosystem blob ssid parsed via alias");
    CHECK(strcmp(eco_cfg.wifi_password, "sup3rsecret") == 0, "Ecosystem blob password parsed via alias");

    /* Ecosystem Home-aware binary blob */
    const uint8_t eco_home_blob[] = {
        0x4d, 0x49, 0x43, 0x48, 0x49, 0x2d, 0x46, 0x31,
        0x01,
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
        0x00, 0xa4, /* len = 164 (contains 0x00) */
        0x7b, 0x22, 0x68, 0x6f, 0x6d, 0x65, 0x5f, 0x69, 0x64, 0x22, 0x3a, 0x22, 0x46, 0x55, 0x31, 0x46,
        0x4c, 0x2d, 0x77, 0x46, 0x4c, 0x66, 0x73, 0x66, 0x65, 0x77, 0x33, 0x71, 0x70, 0x62, 0x52, 0x37,
        0x58, 0x6a, 0x44, 0x6b, 0x6d, 0x53, 0x74, 0x57, 0x5a, 0x59, 0x34, 0x67, 0x38, 0x34, 0x4d, 0x79,
        0x57, 0x2d, 0x7a, 0x58, 0x50, 0x4f, 0x73, 0x22, 0x2c, 0x22, 0x70, 0x61, 0x73, 0x73, 0x77, 0x6f,
        0x72, 0x64, 0x22, 0x3a, 0x22, 0x73, 0x75, 0x70, 0x33, 0x72, 0x73, 0x65, 0x63, 0x72, 0x65, 0x74,
        0x22, 0x2c, 0x22, 0x72, 0x6f, 0x6f, 0x74, 0x5f, 0x70, 0x75, 0x62, 0x6c, 0x69, 0x63, 0x5f, 0x6b,
        0x65, 0x79, 0x22, 0x3a, 0x22, 0x53, 0x53, 0x55, 0x6d, 0x43, 0x68, 0x5f, 0x6d, 0x45, 0x47, 0x4c,
        0x55, 0x6b, 0x77, 0x7a, 0x39, 0x49, 0x4a, 0x79, 0x5a, 0x56, 0x76, 0x39, 0x4d, 0x61, 0x70, 0x65,
        0x44, 0x5f, 0x50, 0x43, 0x6b, 0x4b, 0x49, 0x31, 0x37, 0x74, 0x77, 0x44, 0x33, 0x63, 0x5f, 0x67,
        0x22, 0x2c, 0x22, 0x73, 0x73, 0x69, 0x64, 0x22, 0x3a, 0x22, 0x4d, 0x69, 0x63, 0x68, 0x69, 0x4e,
        0x65, 0x74, 0x22, 0x7d,
        0x7c, 0x9b, 0xf9, 0x48
    };

    michi_home_erase();
    michi_factory_cfg_set_test_partition_bytes(eco_home_blob, sizeof(eco_home_blob));
    CHECK(michi_factory_cfg_check_and_import() == ESP_OK,
          "check_and_import succeeds with real Ecosystem Home MICHI-F1 blob without premature zero termination");
    CHECK(michi_home_is_provisioned(), "home is provisioned after Ecosystem MICHI-F1 import");
    char imported_id[MICHI_HOME_ID_LEN] = {0};
    CHECK(michi_home_get_id(imported_id, sizeof(imported_id)) == ESP_OK, "read imported home_id");
    CHECK(strcmp(imported_id, "FU1FL-wFLfsfew3qpbR7XjDkmStWZY4g84MyW-zXPOs") == 0, "home_id matches vector");

    /* Ecosystem corrupted CRC32 check */
    uint8_t bad_eco_blob[sizeof(eco_home_blob)];
    memcpy(bad_eco_blob, eco_home_blob, sizeof(eco_home_blob));
    bad_eco_blob[sizeof(eco_home_blob) - 1] ^= 0xFF;
    CHECK(michi_factory_cfg_parse((const char *)bad_eco_blob, sizeof(bad_eco_blob), &eco_cfg) == ESP_ERR_INVALID_CRC,
          "reject Ecosystem MICHI-F1 blob with corrupted CRC32");

    /* Test hardened rollback on invalid home root derivation */
    michi_home_erase();
    const char *corrupted_home_json = "{\"home_id\":\"FU1FL-wFLfsfew3qpbR7XjDkmStWZY4g84MyW-zXPOs\","
                                      "\"root_public_key\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\","
                                      "\"wifi_ssid\":\"RollbackWiFi\",\"wifi_password\":\"secret\"}";
    michi_factory_cfg_set_test_partition_data(corrupted_home_json);
    CHECK(michi_factory_cfg_check_and_import() != ESP_OK, "check_and_import fails on mismatched home derivation");
    CHECK(!michi_home_is_provisioned(), "home is NOT provisioned after rollback");

    /* Legacy 48-byte header container verification */
    michi_f1_header_t f1_hdr;
    memset(&f1_hdr, 0, sizeof(f1_hdr));
    memcpy(f1_hdr.magic, MICHI_F1_MAGIC, MICHI_F1_MAGIC_LEN);
    f1_hdr.version = MICHI_F1_VERSION;
    f1_hdr.nonce_len = 16;
    memset(f1_hdr.nonce, 0x42, 16);
    const char *f1_json = "{\"home_id\":\"FU1FL-wFLfsfew3qpbR7XjDkmStWZY4g84MyW-zXPOs\","
                          "\"root_public_key\":\"SSUmCh_mEGLUkwz9IJyZVv9MapeD_PCkKI17twD3c_g\","
                          "\"wifi_ssid\":\"BinaryWiFi\",\"wifi_password\":\"secret\"}";
    f1_hdr.payload_len = (uint32_t)strlen(f1_json);

    uint8_t bin_buf[1024];
    memcpy(bin_buf, &f1_hdr, sizeof(f1_hdr));
    memcpy(bin_buf + sizeof(f1_hdr), f1_json, f1_hdr.payload_len);
    size_t data_len = sizeof(f1_hdr) + f1_hdr.payload_len;

    /* Compute CRC32 */
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < data_len; i++) {
        crc ^= bin_buf[i];
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
        }
    }
    uint32_t good_crc = ~crc;
    memcpy(bin_buf + data_len, &good_crc, 4);
    size_t total_bin_len = data_len + 4;

    michi_factory_cfg_t bin_cfg;
    CHECK(michi_factory_cfg_parse((const char *)bin_buf, total_bin_len, &bin_cfg) == ESP_OK,
          "parse legacy 48-byte binary MICHI-F1 container with valid CRC32");
    CHECK(strcmp(bin_cfg.wifi_ssid, "BinaryWiFi") == 0, "binary wifi_ssid parsed");
}

static char s_cb_revoked[MICHI_HOME_ID_LEN] = {0};
static void on_test_revoked(const char *dev)
{
    if (dev) {
        snprintf(s_cb_revoked, sizeof(s_cb_revoked), "%s", dev);
    }
}

/* ── 8. Revocation Verification & Enforcement ── */
static void test_revocation(void)
{
    printf("revocation: certificate verification, roster tracking and auth enforcement\n");

    /* Reset home and auth */
    michi_home_erase();
    michi_auth_reset();

    uint8_t root_sk[64], root_pk[32];
    char root_pk_b64[44], root_michi_id[44];
    make_keypair(0x33, root_sk, root_pk, root_pk_b64, root_michi_id);
    CHECK(michi_home_set_credentials(root_michi_id, root_pk) == ESP_OK, "provision home root");

    /* Create client and membership */
    uint8_t client_sk[64], client_pk[32];
    char client_pk_b64[44], client_michi_id[44];
    make_keypair(0x44, client_sk, client_pk, client_pk_b64, client_michi_id);

    michi_membership_t client_mem;
    memset(&client_mem, 0, sizeof(client_mem));
    client_mem.version = 1;
    snprintf(client_mem.home_id, sizeof(client_mem.home_id), "%s", root_michi_id);
    snprintf(client_mem.device_michi_id, sizeof(client_mem.device_michi_id), "%s", client_michi_id);
    snprintf(client_mem.device_public_key, sizeof(client_mem.device_public_key), "%s", client_pk_b64);
    snprintf(client_mem.device_type, sizeof(client_mem.device_type), "server");
    snprintf(client_mem.roles[0], sizeof(client_mem.roles[0]), "music_server");
    client_mem.role_count = 1;
    snprintf(client_mem.issued_at, sizeof(client_mem.issued_at), "2026-10-04T12:00:00Z");
    client_mem.serial = 1;

    uint8_t mem_canon[512];
    size_t mem_canon_len = michi_home_canonical_membership_bytes(
        client_mem.home_id, client_mem.device_michi_id, client_mem.device_public_key,
        client_mem.device_type, client_mem.roles, client_mem.role_count,
        client_mem.issued_at, client_mem.serial, mem_canon, sizeof(mem_canon));
    uint8_t mem_sig_raw[64];
    crypto_ed25519_sign(mem_sig_raw, root_sk, mem_canon, mem_canon_len);
    michi_identity_base64url_encode(mem_sig_raw, sizeof(mem_sig_raw), client_mem.signature, sizeof(client_mem.signature));

    /* Check initial auth before revocation succeeds */
    char cid[MICHI_AUTH_CHALLENGE_ID_LEN], nonce[MICHI_AUTH_NONCE_B64_LEN];
    uint32_t exp = 0;
    CHECK(michi_auth_create_challenge(client_michi_id, client_pk_b64, root_michi_id,
                                      cid, sizeof(cid), nonce, sizeof(nonce), &exp) == ESP_OK,
          "create challenge pre-revocation");

    char server_id[MICHI_IDENTITY_MICHI_ID_LEN];
    CHECK(michi_identity_michi_id(server_id, sizeof(server_id)) == ESP_OK, "get server michi_id");

    uint8_t auth_payload[256];
    size_t auth_payload_len = michi_home_device_auth_payload(
        root_michi_id, server_id, client_michi_id, cid, nonce, auth_payload, sizeof(auth_payload));
    uint8_t client_sig_raw[64];
    crypto_ed25519_sign(client_sig_raw, client_sk, auth_payload, auth_payload_len);
    char client_sig_b64[MICHI_HOME_SIG_B64_LEN];
    michi_identity_base64url_encode(client_sig_raw, sizeof(client_sig_raw), client_sig_b64, sizeof(client_sig_b64));

    char tok[MICHI_AUTH_TOKEN_B64_LEN], srv_sig[MICHI_HOME_SIG_B64_LEN];
    uint32_t sess_exp = 0;
    CHECK(michi_auth_verify_and_create_session(
              cid, client_michi_id, &client_mem, client_sig_b64,
              tok, sizeof(tok), srv_sig, sizeof(srv_sig), &sess_exp) == ESP_OK,
          "pre-revocation auth succeeds");

    /* Issue revocation for client */
    michi_revocation_t rev;
    memset(&rev, 0, sizeof(rev));
    rev.version = 1;
    snprintf(rev.home_id, sizeof(rev.home_id), "%s", root_michi_id);
    snprintf(rev.revoked_device_michi_id, sizeof(rev.revoked_device_michi_id), "%s", client_michi_id);
    snprintf(rev.revoked_at, sizeof(rev.revoked_at), "2026-10-04T15:00:00Z");
    snprintf(rev.reason, sizeof(rev.reason), "Device compromised or retired");

    uint8_t rev_canon[512];
    size_t rev_canon_len = michi_home_canonical_revocation_bytes(
        rev.home_id, rev.revoked_device_michi_id, rev.revoked_at, rev.reason, rev_canon, sizeof(rev_canon));
    CHECK(rev_canon_len > 0, "canonical revocation bytes constructed");

    uint8_t rev_sig_raw[64];
    crypto_ed25519_sign(rev_sig_raw, root_sk, rev_canon, rev_canon_len);
    michi_identity_base64url_encode(rev_sig_raw, sizeof(rev_sig_raw), rev.signature, sizeof(rev.signature));

    /* Verify valid and tampered revocation */
    CHECK(michi_home_verify_revocation(&rev, root_pk), "valid revocation verified");
    rev.signature[10] ^= 0x01;
    CHECK(!michi_home_verify_revocation(&rev, root_pk), "tampered revocation signature rejected");
    rev.signature[10] ^= 0x01; /* restore */

    s_cb_revoked[0] = '\0';
    michi_home_set_revocation_callback(on_test_revoked);

    CHECK(!michi_home_is_device_revoked(client_michi_id), "device not revoked before adding to roster");
    CHECK(michi_home_add_revocation(&rev) == ESP_OK, "add revocation to roster");
    CHECK(michi_home_is_device_revoked(client_michi_id), "device marked as revoked");
    CHECK(strcmp(s_cb_revoked, client_michi_id) == 0, "revocation callback invoked with revoked michi_id");

    michi_revocation_t rev_list[MICHI_MAX_REVOCATIONS];
    size_t rev_cnt = michi_home_get_revocations(rev_list, MICHI_MAX_REVOCATIONS);
    CHECK(rev_cnt == 1, "revocation count is 1");
    CHECK(strcmp(rev_list[0].revoked_device_michi_id, client_michi_id) == 0, "retrieved revocation matches");

    /* Now attempting auth session with the revoked client fails */
    char cid_rev[MICHI_AUTH_CHALLENGE_ID_LEN], nonce_rev[MICHI_AUTH_NONCE_B64_LEN];
    CHECK(michi_auth_create_challenge(client_michi_id, client_pk_b64, root_michi_id,
                                      cid_rev, sizeof(cid_rev), nonce_rev, sizeof(nonce_rev), &exp) == ESP_OK,
          "create challenge for revoked client");

    auth_payload_len = michi_home_device_auth_payload(
        root_michi_id, server_id, client_michi_id, cid_rev, nonce_rev, auth_payload, sizeof(auth_payload));
    crypto_ed25519_sign(client_sig_raw, client_sk, auth_payload, auth_payload_len);
    michi_identity_base64url_encode(client_sig_raw, sizeof(client_sig_raw), client_sig_b64, sizeof(client_sig_b64));

    CHECK(michi_auth_verify_and_create_session(
              cid_rev, client_michi_id, &client_mem, client_sig_b64,
              tok, sizeof(tok), srv_sig, sizeof(srv_sig), &sess_exp) == ESP_ERR_INVALID_RESPONSE,
          "auth session rejected for revoked client (401)");

    /* Verify revocation persistence across simulated restart */
    michi_home_init();
    CHECK(michi_home_is_device_revoked(client_michi_id), "revocation loaded from NVS on restart");

    michi_home_erase();
    CHECK(!michi_home_is_device_revoked(client_michi_id), "revocations erased on home erase");
}

int main(void)
{
    test_validators();
    test_home_provisioning();
    test_membership_verification();
    test_device_and_server_auth();
    test_michi_auth_manager();
    test_challenge_dos_protection();
    test_factory_config();
    test_revocation();

    if (failures == 0) {
        printf("PASS test_michi_home (all assertions passed)\n");
        return 0;
    }
    printf("FAIL test_michi_home (%d failures)\n", failures);
    return 1;
}

