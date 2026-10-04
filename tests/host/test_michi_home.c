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
    CHECK(strcmp(cid1, cid2) != 0, "new challenge ID generated on client slot refresh");

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

    /* Device membership handling */
    uint8_t root_sk[64], root_pk[32];
    char root_pk_b64[44], root_michi_id[44];
    make_keypair(0x77, root_sk, root_pk, root_pk_b64, root_michi_id);

    michi_home_erase();
    michi_home_set_credentials(root_michi_id, root_pk);
    CHECK(!michi_home_has_device_membership(), "no device membership initially");

    uint8_t dev_sk[64], dev_pk[32];
    char dev_pk_b64[44], dev_michi_id[44];
    make_keypair(0x88, dev_sk, dev_pk, dev_pk_b64, dev_michi_id);

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

    /* One-shot factory_cfg import */
    michi_home_erase();
    CHECK(!michi_home_is_provisioned(), "erased home");

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
    michi_factory_cfg_set_test_partition_data(NULL);
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

    if (failures == 0) {
        printf("PASS test_michi_home (all assertions passed)\n");
        return 0;
    }
    printf("FAIL test_michi_home (%d failures)\n", failures);
    return 1;
}
