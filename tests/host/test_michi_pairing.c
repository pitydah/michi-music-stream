/* Host-side tests for the receiver-button pairing component (MS-06).
 *
 * Compiles the REAL firmware sources - michi_pairing.c, validators.c,
 * michi_identity.c (+ vendored Monocypher/BLAKE3) and identity_nvs.c -
 * against test shims (fake NVS in RAM, fake monotonic clock, fake state
 * bus, deterministic esp_fill_random, host SHA-256 test double). No
 * reimplementation of the component.
 *
 * Golden vectors: copied verbatim from the vendored contract bundle
 * contracts/michi-link/vectors/pairing/ (the same material the identity
 * tests use). The crate (ed25519-dalek) generated those signatures;
 * Monocypher (RFC 8032) must verify them, proving wire interop.
 *
 * Covers the MS-06 mandatory cases: window closed/open/expired; invalid
 * nonce/signature/michi_id; correct/wrong PIN; sixth attempt (429);
 * expired session; double confirmation; reboot (digest persists, session
 * does not); token never in NVS; revocation and factory reset.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "michi_pairing.h"
#include "validators.h"
#include "nvs.h" /* fake NVS shim: test hooks only */
#include "esp_timer.h" /* fake clock: test hooks */
#include "michi_state.h" /* fake state bus: test hooks */
#include "esp_random.h" /* deterministic stream: reseed hook */
#include "mbedtls/sha256.h" /* host SHA-256 test double */

static int failures = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            printf("  FAIL %s\n", msg);                                     \
            failures++;                                                     \
        }                                                                   \
    } while (0)

/* ── golden vector material (contracts/michi-link/vectors/pairing/) ── */

static const char VEC_PAIR_PK_B64[] =
    "j8oIHv906goIsvcANXl_SZX8-OPcZftDkTPwTYaQQ7E";
static const char VEC_PAIR_MICHI_ID[] =
    "JXcHys3oHoK2xsmQqlWEKi-KH_s4TrxJGw3YbiKP9-U";
static const char VEC_PAIR_SIG_B64[] =
    "5Hg1TwCbzj-x6MaU7mvToRCALEyQXtRKmeJWLmShuXuJWSes16wGptbq573FfY3_H5VWRxPU9LI8hanyNfbXCA";
static const char VEC_PAIR_NONCE_B64[] = "CxIZICcuNTxDSlFYX2ZtdA";
static const char VEC_PAIR_NONCE_ALTERED_B64[] = "DBMaISgvNj1ES1JZYGdudQ";
static const char VEC_RECEIVER_MICHI_ID[] =
    "f2UwxQaeA6vA8LO7Cr1nGRr5MStned_Gbmc_ua48qUc";
static const char VEC_RECEIVER_PK_B64[] =
    "RpHnJr9oP1DXBkPuIMuk0hJ2hAJ5SiWO2hAQVCMGREE";

/* ── PIN display spy ──────────────────────────────────────── */

static char spy_pin[MICHI_PAIRING_PIN_BUF_LEN];
static int spy_pin_calls;
static int spy_clear_calls;

static void pin_spy(const char *pin, void *ctx)
{
    (void)ctx;
    if (pin != NULL) {
        strlcpy(spy_pin, pin, sizeof(spy_pin));
        spy_pin_calls++;
    } else {
        spy_clear_calls++;
    }
}

static void spy_reset(void)
{
    spy_pin[0] = '\0';
    spy_pin_calls = 0;
    spy_clear_calls = 0;
}

/* ── harness helpers ──────────────────────────────────────── */

static void pairing_test_reset(uint64_t rng_seed)
{
    test_nvs_reset();
    test_esp_timer_reset();
    test_state_reset();
    test_esp_random_seed(rng_seed);
    spy_reset();
    michi_pairing_set_pin_display_cb(pin_spy, NULL);
}

/* Post-shutdown reset for the reboot test: reseeds the RNG and the spy
 * WITHOUT touching the NVS store (the persisted registry must survive)
 * or the timer (shutdown deleted it; init creates a fresh one). */
static void pairing_test_reset_rng_only(void)
{
    test_esp_random_seed(0x00C0FFEE);
    spy_reset();
}

static void make_peer(michi_pairing_peer_t *p, const char *michi_id,
                      const char *public_key, const char *nonce,
                      const char *signature)
{
    memset(p, 0, sizeof(*p));
    strlcpy(p->michi_id, michi_id, sizeof(p->michi_id));
    strlcpy(p->public_key, public_key, sizeof(p->public_key));
    strlcpy(p->challenge_nonce, nonce, sizeof(p->challenge_nonce));
    strlcpy(p->challenge_signature, signature,
            sizeof(p->challenge_signature));
}

static const michi_pairing_peer_t *valid_peer(void)
{
    static michi_pairing_peer_t p;
    static bool built = false;
    if (!built) {
        make_peer(&p, VEC_PAIR_MICHI_ID, VEC_PAIR_PK_B64, VEC_PAIR_NONCE_B64,
                  VEC_PAIR_SIG_B64);
        built = true;
    }
    return &p;
}

/* A signature with the first byte altered (tampered). */
static const char *tampered_signature(void)
{
    static char sig[MICHI_IDENTITY_SIGNATURE_B64_LEN];
    static bool built = false;
    if (!built) {
        strlcpy(sig, VEC_PAIR_SIG_B64, sizeof(sig));
        sig[0] = (sig[0] == 'A') ? 'B' : 'A';
        built = true;
    }
    return sig;
}

/* Full happy path: open -> start -> confirm. Fills session_id, token and
 * device_id. Assumes a freshly initialized subsystem. */
static void pair_once(char *out_session_id, size_t sid_len, char *out_token,
                      size_t token_len, char *out_device_id,
                      size_t device_id_len)
{
    CHECK(michi_pairing_open_window() == ESP_OK, "open_window succeeds");
    char expires_at[MICHI_PAIRING_EXPIRES_AT_LEN];
    uint32_t attempts = 0;
    CHECK(michi_pairing_start(valid_peer(), NULL, out_session_id, sid_len,
                              expires_at, sizeof(expires_at),
                              &attempts) == MICHI_PAIRING_START_OK,
          "start succeeds inside the window");
    CHECK(michi_pairing_confirm(out_session_id, spy_pin, VEC_PAIR_MICHI_ID,
                                VEC_PAIR_PK_B64, out_token, token_len,
                                out_device_id, device_id_len) ==
              MICHI_PAIRING_CONFIRM_OK,
          "confirm succeeds with the displayed PIN");
}

/* ── window ──────────────────────────────────────────────── */

static void test_window_closed_rejects_start(void)
{
    printf("pairing: window closed -> start 403\n");
    pairing_test_reset(0x1001);
    CHECK(michi_pairing_init() == ESP_OK, "init succeeds");
    CHECK(!michi_pairing_is_window_open(), "window starts closed");
    char sid[MICHI_PAIRING_SESSION_ID_LEN];
    char exp[MICHI_PAIRING_EXPIRES_AT_LEN];
    uint32_t attempts = 0;
    CHECK(michi_pairing_start(valid_peer(), NULL, sid, sizeof(sid), exp,
                              sizeof(exp), &attempts) ==
              MICHI_PAIRING_START_WINDOW_CLOSED,
          "start rejected while the window is closed");
    /* No session was created. */
    char status[12];
    CHECK(michi_pairing_status(sid, status, sizeof(status), exp, sizeof(exp),
                               &attempts) == MICHI_PAIRING_STATUS_NOT_FOUND,
          "no session exists after a rejected start");
    CHECK(spy_pin_calls == 0, "no PIN was ever shown");
    CHECK(michi_pairing_shutdown() == ESP_OK, "shutdown succeeds");
}

static void test_window_open_and_start(void)
{
    printf("pairing: window open -> start 201\n");
    pairing_test_reset(0x1002);
    CHECK(michi_pairing_init() == ESP_OK, "init succeeds");
    CHECK(michi_pairing_open_window() == ESP_OK, "open_window succeeds");
    CHECK(michi_pairing_is_window_open(), "window is open");
    char sid[MICHI_PAIRING_SESSION_ID_LEN];
    char exp[MICHI_PAIRING_EXPIRES_AT_LEN];
    uint32_t attempts = 0;
    CHECK(michi_pairing_start(valid_peer(), "192.168.1.7", sid, sizeof(sid),
                              exp, sizeof(exp), &attempts) ==
              MICHI_PAIRING_START_OK,
          "start succeeds");
    CHECK(michi_pairing_uuid_valid(sid), "session_id is a UUID");
    CHECK(attempts == MICHI_PAIRING_PIN_ATTEMPTS,
          "attempts_remaining starts at 5");
    CHECK(strlen(exp) == 20 && exp[4] == '-' && exp[10] == 'T' &&
              exp[19] == 'Z',
          "expires_at is RFC 3339");
    /* The PIN went to the LOCAL display, never to the caller. */
    CHECK(spy_pin_calls == 1, "PIN delivered to the display exactly once");
    CHECK(michi_pairing_pin_valid(spy_pin), "displayed PIN is 6 digits");
    CHECK(michi_pairing_shutdown() == ESP_OK, "shutdown succeeds");
}

static void test_window_expiry(void)
{
    printf("pairing: window expiry (120 s monotonic)\n");
    pairing_test_reset(0x1003);
    CHECK(michi_pairing_init() == ESP_OK, "init succeeds");
    CHECK(michi_pairing_open_window() == ESP_OK, "open_window succeeds");
    char sid[MICHI_PAIRING_SESSION_ID_LEN];
    char exp[MICHI_PAIRING_EXPIRES_AT_LEN];
    uint32_t attempts = 0;
    CHECK(michi_pairing_start(valid_peer(), NULL, sid, sizeof(sid), exp,
                              sizeof(exp), &attempts) ==
              MICHI_PAIRING_START_OK,
          "start succeeds");
    CHECK(michi_pairing_is_window_open(), "window open before expiry");

    /* 120 s on the monotonic clock: the one-shot timer fires and closes
     * the window with the FSM event. */
    test_esp_timer_advance(120 * 1000000LL);
    CHECK(!michi_pairing_is_window_open(), "window closed after 120 s");
    CHECK(test_state_saw_event(MICHI_EVENT_PAIRING_WINDOW_CLOSED),
          "PAIRING_WINDOW_CLOSED posted on expiry");
    CHECK(spy_clear_calls >= 1, "PIN display cleared on expiry");

    /* The expired session is still answerable with status "expired". */
    char status[12];
    CHECK(michi_pairing_status(sid, status, sizeof(status), exp, sizeof(exp),
                               &attempts) == MICHI_PAIRING_STATUS_OK,
          "expired session still exists");
    CHECK(strcmp(status, "expired") == 0, "status reports expired");

    /* Confirm of an expired session and a new start are rejected. */
    char token[MICHI_PAIRING_TOKEN_B64_LEN];
    char device_id[MICHI_PAIRING_DEVICE_ID_LEN];
    CHECK(michi_pairing_confirm(sid, spy_pin, VEC_PAIR_MICHI_ID,
                                VEC_PAIR_PK_B64, token, sizeof(token),
                                device_id, sizeof(device_id)) ==
              MICHI_PAIRING_CONFIRM_NOT_FOUND,
          "confirm of an expired session -> 404");
    CHECK(michi_pairing_start(valid_peer(), NULL, sid, sizeof(sid), exp,
                              sizeof(exp), &attempts) ==
              MICHI_PAIRING_START_WINDOW_CLOSED,
          "start after expiry -> 403");
    CHECK(michi_pairing_shutdown() == ESP_OK, "shutdown succeeds");
}

static void test_window_reopen_replaces(void)
{
    printf("pairing: re-open replaces the window and pending sessions\n");
    pairing_test_reset(0x1004);
    CHECK(michi_pairing_init() == ESP_OK, "init succeeds");
    CHECK(michi_pairing_open_window() == ESP_OK, "first open");
    char sid1[MICHI_PAIRING_SESSION_ID_LEN];
    char exp[MICHI_PAIRING_EXPIRES_AT_LEN];
    uint32_t attempts = 0;
    CHECK(michi_pairing_start(valid_peer(), "10.0.0.1", sid1, sizeof(sid1),
                              exp, sizeof(exp), &attempts) ==
              MICHI_PAIRING_START_OK,
          "first session created");

    /* The physical press again: the previous window AND its pending
     * sessions are gone. */
    CHECK(michi_pairing_open_window() == ESP_OK, "re-open succeeds");
    char status[12];
    CHECK(michi_pairing_status(sid1, status, sizeof(status), exp, sizeof(exp),
                               &attempts) == MICHI_PAIRING_STATUS_NOT_FOUND,
          "pending session dropped by the re-open");
    char sid2[MICHI_PAIRING_SESSION_ID_LEN];
    CHECK(michi_pairing_start(valid_peer(), "10.0.0.1", sid2, sizeof(sid2),
                              exp, sizeof(exp), &attempts) ==
              MICHI_PAIRING_START_OK,
          "the fresh window accepts a new start");
    CHECK(strcmp(sid1, sid2) != 0, "new session has a new id");
    CHECK(michi_pairing_shutdown() == ESP_OK, "shutdown succeeds");
}

/* ── pair/start validation ────────────────────────────────── */

static void test_invalid_challenge(void)
{
    printf("pairing: invalid nonce/signature/michi_id -> 400\n");
    pairing_test_reset(0x1005);
    CHECK(michi_pairing_init() == ESP_OK, "init succeeds");
    CHECK(michi_pairing_open_window() == ESP_OK, "open_window succeeds");

    michi_pairing_peer_t bad;
    char sid[MICHI_PAIRING_SESSION_ID_LEN];
    char exp[MICHI_PAIRING_EXPIRES_AT_LEN];
    uint32_t attempts = 0;

    /* Nonce altered (signature does not cover it). */
    make_peer(&bad, VEC_PAIR_MICHI_ID, VEC_PAIR_PK_B64,
              VEC_PAIR_NONCE_ALTERED_B64, VEC_PAIR_SIG_B64);
    CHECK(michi_pairing_start(&bad, NULL, sid, sizeof(sid), exp, sizeof(exp),
                              &attempts) == MICHI_PAIRING_START_INVALID,
          "altered nonce rejected");

    /* Signature tampered. */
    make_peer(&bad, VEC_PAIR_MICHI_ID, VEC_PAIR_PK_B64, VEC_PAIR_NONCE_B64,
              tampered_signature());
    CHECK(michi_pairing_start(&bad, NULL, sid, sizeof(sid), exp, sizeof(exp),
                              &attempts) == MICHI_PAIRING_START_INVALID,
          "tampered signature rejected");

    /* michi_id does not correspond to public_key (wrong id). */
    make_peer(&bad, VEC_RECEIVER_MICHI_ID, VEC_PAIR_PK_B64,
              VEC_PAIR_NONCE_B64, VEC_PAIR_SIG_B64);
    CHECK(michi_pairing_start(&bad, NULL, sid, sizeof(sid), exp, sizeof(exp),
                              &attempts) == MICHI_PAIRING_START_INVALID,
          "mismatched michi_id rejected");

    /* Signature with a DIFFERENT (valid) key. */
    make_peer(&bad, VEC_PAIR_MICHI_ID, VEC_RECEIVER_PK_B64,
              VEC_PAIR_NONCE_B64, VEC_PAIR_SIG_B64);
    CHECK(michi_pairing_start(&bad, NULL, sid, sizeof(sid), exp, sizeof(exp),
                              &attempts) == MICHI_PAIRING_START_INVALID,
          "signature under a different key rejected");

    /* NO session was created by any of the failures. */
    CHECK(michi_pairing_status(sid, (char[12]){0}, 12, exp, sizeof(exp),
                               &attempts) == MICHI_PAIRING_STATUS_NOT_FOUND,
          "no session created by rejected starts");
    CHECK(spy_pin_calls == 0, "no PIN shown for rejected starts");
    CHECK(michi_pairing_shutdown() == ESP_OK, "shutdown succeeds");
}

static void test_start_rate_limits(void)
{
    printf("pairing: start rate limits (per IP / global)\n");
    pairing_test_reset(0x1006);
    CHECK(michi_pairing_init() == ESP_OK, "init succeeds");
    CHECK(michi_pairing_open_window() == ESP_OK, "open_window succeeds");
    char sid[MICHI_PAIRING_SESSION_ID_LEN];
    char exp[MICHI_PAIRING_EXPIRES_AT_LEN];
    uint32_t attempts = 0;

    /* Per-IP limit: 3 starts from the same IP pass, the 4th fails. */
    int ok = 0;
    for (int i = 0; i < 3; i++) {
        if (michi_pairing_start(valid_peer(), "10.9.8.7", sid, sizeof(sid),
                                exp, sizeof(exp), &attempts) ==
            MICHI_PAIRING_START_OK) {
            ok++;
        }
    }
    CHECK(ok == 3, "three starts from one IP allowed");
    CHECK(michi_pairing_start(valid_peer(), "10.9.8.7", sid, sizeof(sid),
                              exp, sizeof(exp), &attempts) ==
              MICHI_PAIRING_START_RATE_LIMITED,
          "fourth start from the same IP rate-limited");

    /* Fresh window (the re-open resets the counters). */
    CHECK(michi_pairing_open_window() == ESP_OK, "re-open resets limits");
    ok = 0;
    char ip[MICHI_PAIRING_IP_MAX];
    for (int i = 0; i < 6; i++) {
        snprintf(ip, sizeof(ip), "10.0.0.%d", i + 1);
        if (michi_pairing_start(valid_peer(), ip, sid, sizeof(sid), exp,
                                sizeof(exp), &attempts) ==
            MICHI_PAIRING_START_OK) {
            ok++;
        }
    }
    /* 4 sessions fit the per-window session table; the 5th start hits the
     * global limit (5 per window). */
    CHECK(ok == 4, "four starts across distinct IPs allowed");
    CHECK(michi_pairing_shutdown() == ESP_OK, "shutdown succeeds");
}

/* ── pair/confirm ─────────────────────────────────────────── */

static void test_status_and_wrong_pin(void)
{
    printf("pairing: status pending + wrong PIN attempts\n");
    pairing_test_reset(0x1007);
    CHECK(michi_pairing_init() == ESP_OK, "init succeeds");
    CHECK(michi_pairing_open_window() == ESP_OK, "open_window succeeds");
    char sid[MICHI_PAIRING_SESSION_ID_LEN];
    char exp[MICHI_PAIRING_EXPIRES_AT_LEN];
    uint32_t attempts = 0;
    CHECK(michi_pairing_start(valid_peer(), NULL, sid, sizeof(sid), exp,
                              sizeof(exp), &attempts) ==
              MICHI_PAIRING_START_OK,
          "start succeeds");

    char status[12];
    CHECK(michi_pairing_status(sid, status, sizeof(status), exp, sizeof(exp),
                               &attempts) == MICHI_PAIRING_STATUS_OK,
          "status of the new session exists");
    CHECK(strcmp(status, "pending") == 0, "status pending");
    CHECK(attempts == 5, "5 attempts left");
    CHECK(michi_pairing_status("550e8400-e29b-41d4-a716-446655440099",
                               status, sizeof(status), exp, sizeof(exp),
                               &attempts) == MICHI_PAIRING_STATUS_NOT_FOUND,
          "unknown session -> 404");

    /* A wrong PIN (guaranteed different from the displayed one). */
    char wrong[MICHI_PAIRING_PIN_BUF_LEN];
    snprintf(wrong, sizeof(wrong), "%06u",
             (unsigned)((strtoul(spy_pin, NULL, 10) + 1) % 1000000));
    char token[MICHI_PAIRING_TOKEN_B64_LEN];
    char device_id[MICHI_PAIRING_DEVICE_ID_LEN];
    CHECK(michi_pairing_confirm(sid, wrong, VEC_PAIR_MICHI_ID,
                                VEC_PAIR_PK_B64, token, sizeof(token),
                                device_id, sizeof(device_id)) ==
              MICHI_PAIRING_CONFIRM_PIN_MISMATCH,
          "wrong PIN rejected");
    CHECK(michi_pairing_status(sid, status, sizeof(status), exp, sizeof(exp),
                               &attempts) == MICHI_PAIRING_STATUS_OK &&
              strcmp(status, "pending") == 0 && attempts == 4,
          "one attempt consumed, still pending");

    /* Malformed PIN: rejected without consuming an attempt. */
    CHECK(michi_pairing_confirm(sid, "12x456", VEC_PAIR_MICHI_ID,
                                VEC_PAIR_PK_B64, token, sizeof(token),
                                device_id, sizeof(device_id)) ==
              MICHI_PAIRING_CONFIRM_INVALID,
          "malformed PIN rejected");
    CHECK(michi_pairing_status(sid, status, sizeof(status), exp, sizeof(exp),
                               &attempts) == MICHI_PAIRING_STATUS_OK &&
              attempts == 4,
          "malformed PIN consumed no attempt");

    /* Identity mismatch: rejected WITHOUT consuming an attempt. */
    CHECK(michi_pairing_confirm(sid, spy_pin, VEC_RECEIVER_MICHI_ID,
                                VEC_RECEIVER_PK_B64, token, sizeof(token),
                                device_id, sizeof(device_id)) ==
              MICHI_PAIRING_CONFIRM_INVALID,
          "identity mismatch rejected");
    CHECK(michi_pairing_status(sid, status, sizeof(status), exp, sizeof(exp),
                               &attempts) == MICHI_PAIRING_STATUS_OK &&
              attempts == 4,
          "identity mismatch consumed no attempt");
    CHECK(michi_pairing_shutdown() == ESP_OK, "shutdown succeeds");
}

static void test_pin_lockout_sixth_attempt(void)
{
    printf("pairing: five wrong PINs, sixth attempt -> 429\n");
    pairing_test_reset(0x1008);
    CHECK(michi_pairing_init() == ESP_OK, "init succeeds");
    CHECK(michi_pairing_open_window() == ESP_OK, "open_window succeeds");
    char sid[MICHI_PAIRING_SESSION_ID_LEN];
    char exp[MICHI_PAIRING_EXPIRES_AT_LEN];
    uint32_t attempts = 0;
    CHECK(michi_pairing_start(valid_peer(), NULL, sid, sizeof(sid), exp,
                              sizeof(exp), &attempts) ==
              MICHI_PAIRING_START_OK,
          "start succeeds");

    char wrong[MICHI_PAIRING_PIN_BUF_LEN];
    snprintf(wrong, sizeof(wrong), "%06u",
             (unsigned)((strtoul(spy_pin, NULL, 10) + 1) % 1000000));
    char token[MICHI_PAIRING_TOKEN_B64_LEN];
    char device_id[MICHI_PAIRING_DEVICE_ID_LEN];

    /* Five failed attempts are allowed (401 each); the fifth locks. */
    for (int i = 0; i < 5; i++) {
        const michi_pairing_confirm_result_t r = michi_pairing_confirm(
            sid, wrong, VEC_PAIR_MICHI_ID, VEC_PAIR_PK_B64, token,
            sizeof(token), device_id, sizeof(device_id));
        CHECK(r == MICHI_PAIRING_CONFIRM_PIN_MISMATCH,
              "wrong PIN answered 401 (attempt within the five)");
    }
    char status[12];
    CHECK(michi_pairing_status(sid, status, sizeof(status), exp, sizeof(exp),
                               &attempts) == MICHI_PAIRING_STATUS_OK &&
              strcmp(status, "locked") == 0,
          "session locked after five failures");

    /* The SIXTH attempt (even with the CORRECT PIN) -> 429, consumed. */
    CHECK(michi_pairing_confirm(sid, spy_pin, VEC_PAIR_MICHI_ID,
                                VEC_PAIR_PK_B64, token, sizeof(token),
                                device_id, sizeof(device_id)) ==
              MICHI_PAIRING_CONFIRM_LOCKED,
          "sixth attempt rate-limited even with the right PIN");
    CHECK(michi_pairing_status(sid, status, sizeof(status), exp, sizeof(exp),
                               &attempts) == MICHI_PAIRING_STATUS_OK &&
              strcmp(status, "locked") == 0,
          "session stays locked");
    CHECK(michi_pairing_shutdown() == ESP_OK, "shutdown succeeds");
}

static void test_confirm_success_and_token(void)
{
    printf("pairing: confirm success issues the token once\n");
    pairing_test_reset(0x1009);
    CHECK(michi_pairing_init() == ESP_OK, "init succeeds");
    char sid[MICHI_PAIRING_SESSION_ID_LEN];
    char token[MICHI_PAIRING_TOKEN_B64_LEN];
    char device_id[MICHI_PAIRING_DEVICE_ID_LEN];
    pair_once(sid, sizeof(sid), token, sizeof(token), device_id,
              sizeof(device_id));

    /* Token: 32 bytes of CSPRNG, base64url-nopad, 43 chars. */
    CHECK(strlen(token) == 43, "token is 43 chars");
    for (size_t i = 0; i < 43; i++) {
        const char c = token[i];
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) {
            CHECK(0, "token is base64url-nopad");
            break;
        }
    }
    CHECK(michi_pairing_uuid_valid(device_id), "device_id is a UUID");

    /* The token validates and grants the default permissions. */
    char got_id[MICHI_PAIRING_DEVICE_ID_LEN];
    uint32_t perms = 0;
    CHECK(michi_pairing_validate_token(token, got_id, sizeof(got_id),
                                       &perms) == ESP_OK,
          "issued token validates");
    CHECK(strcmp(got_id, device_id) == 0, "validation returns the device id");
    CHECK(perms == MICHI_PERM_DEFAULT, "default permissions granted");
    CHECK(!michi_pairing_has_permission(token, MICHI_PERM_OTA),
          "OTA permission not granted by default");

    /* Second confirmation -> 409 CONFLICT. */
    char token2[MICHI_PAIRING_TOKEN_B64_LEN];
    CHECK(michi_pairing_confirm(sid, spy_pin, VEC_PAIR_MICHI_ID,
                                VEC_PAIR_PK_B64, token2, sizeof(token2),
                                device_id, sizeof(device_id)) ==
              MICHI_PAIRING_CONFIRM_CONFLICT,
          "second confirmation -> 409");
    char status[12];
    char exp[MICHI_PAIRING_EXPIRES_AT_LEN];
    uint32_t attempts = 0;
    CHECK(michi_pairing_status(sid, status, sizeof(status), exp, sizeof(exp),
                               &attempts) == MICHI_PAIRING_STATUS_OK &&
              strcmp(status, "confirmed") == 0,
          "status confirmed");
    CHECK(michi_pairing_shutdown() == ESP_OK, "shutdown succeeds");
}

/* ── persistence (NVS) ────────────────────────────────────── */

static bool blob_contains(const uint8_t *blob, size_t len, const char *needle)
{
    const size_t n = strlen(needle);
    if (n > len) {
        return false;
    }
    for (size_t i = 0; i + n <= len; i++) {
        if (memcmp(blob + i, needle, n) == 0) {
            return true;
        }
    }
    return false;
}

/* Byte-sequence search (the digest is binary: it may contain NULs, so a
 * strlen-based search would stop early). */
static bool blob_contains_bytes(const uint8_t *blob, size_t len,
                                const uint8_t *needle, size_t n)
{
    if (n > len) {
        return false;
    }
    for (size_t i = 0; i + n <= len; i++) {
        if (memcmp(blob + i, needle, n) == 0) {
            return true;
        }
    }
    return false;
}

static void test_reboot_persists_digest_only(void)
{
    printf("pairing: reboot - digest persists, session and window do not\n");
    pairing_test_reset(0x100A);
    CHECK(michi_pairing_init() == ESP_OK, "init succeeds");
    char sid[MICHI_PAIRING_SESSION_ID_LEN];
    char token[MICHI_PAIRING_TOKEN_B64_LEN];
    char device_id[MICHI_PAIRING_DEVICE_ID_LEN];
    pair_once(sid, sizeof(sid), token, sizeof(token), device_id,
              sizeof(device_id));

    /* The raw persisted blob: the token is ABSENT, its SHA-256 digest
     * (computed by the host test double, cross-checked by the KAT below)
     * is PRESENT. */
    uint8_t raw[MICHI_PAIRING_TOKEN_BYTES];
    size_t raw_len = 0;
    CHECK(michi_identity_base64url_decode(token, raw, sizeof(raw),
                                          &raw_len) == ESP_OK,
          "token decodes");
    uint8_t digest[MICHI_PAIRING_DIGEST_BYTES];
    CHECK(mbedtls_sha256(raw, raw_len, digest, 0) == 0, "digest computed");

    uint8_t blob[1600]; /* 8 + 8 * 184: the whole version-2 blob fits */
    size_t blob_len = 0;
    CHECK(test_nvs_get_blob("michi_pairing", "controllers", blob,
                            sizeof(blob), &blob_len),
          "registry blob present in NVS");
    CHECK(!blob_contains(blob, blob_len, token),
          "the plaintext token is NOT in NVS");
    CHECK(blob_contains_bytes(blob, blob_len, digest, sizeof(digest)),
          "the SHA-256 digest IS in NVS");
    CHECK(!blob_contains(blob, blob_len, spy_pin),
          "the PIN is not in NVS");

    /* Reboot: shutdown + init (RAM state gone, NVS reloaded). */
    CHECK(michi_pairing_shutdown() == ESP_OK, "shutdown succeeds");
    pairing_test_reset_rng_only();
    CHECK(michi_pairing_init() == ESP_OK, "re-init succeeds");
    CHECK(!michi_pairing_is_window_open(), "reboot closed the window");
    char status[12];
    char exp[MICHI_PAIRING_EXPIRES_AT_LEN];
    uint32_t attempts = 0;
    CHECK(michi_pairing_status(sid, status, sizeof(status), exp, sizeof(exp),
                               &attempts) == MICHI_PAIRING_STATUS_NOT_FOUND,
          "the pairing session is gone after reboot");
    char got_id[MICHI_PAIRING_DEVICE_ID_LEN];
    uint32_t perms = 0;
    CHECK(michi_pairing_validate_token(token, got_id, sizeof(got_id),
                                       &perms) == ESP_OK,
          "the token still validates after reboot");
    CHECK(strcmp(got_id, device_id) == 0, "same device id after reboot");
    CHECK(michi_pairing_shutdown() == ESP_OK, "shutdown succeeds");
}

static void test_revocation_and_erase_all(void)
{
    printf("pairing: revocation and factory-reset erase\n");
    pairing_test_reset(0x100B);
    CHECK(michi_pairing_init() == ESP_OK, "init succeeds");
    char sid[MICHI_PAIRING_SESSION_ID_LEN];
    char token[MICHI_PAIRING_TOKEN_B64_LEN];
    char device_id[MICHI_PAIRING_DEVICE_ID_LEN];
    pair_once(sid, sizeof(sid), token, sizeof(token), device_id,
              sizeof(device_id));

    char list[128];
    CHECK(michi_pairing_list(list, sizeof(list)) == ESP_OK &&
              strcmp(list, device_id) == 0,
          "list shows the paired controller");

    CHECK(michi_pairing_revoke(device_id) == ESP_OK, "revoke succeeds");
    uint32_t perms = 0;
    CHECK(michi_pairing_validate_token(token, device_id, sizeof(device_id),
                                       &perms) == ESP_ERR_NOT_FOUND,
          "revoked token no longer validates");
    CHECK(michi_pairing_list(list, sizeof(list)) == ESP_OK &&
              list[0] == '\0',
          "list empty after revoke");
    CHECK(michi_pairing_revoke("550e8400-e29b-41d4-a716-446655440099") ==
              ESP_ERR_NOT_FOUND,
          "revoking an unknown id -> NOT_FOUND");
    CHECK(michi_pairing_revoke("not-a-uuid") == ESP_ERR_INVALID_ARG,
          "malformed id rejected");

    /* Re-pair and wipe the whole registry (factory reset). */
    pair_once(sid, sizeof(sid), token, sizeof(token), device_id,
              sizeof(device_id));
    CHECK(michi_pairing_erase_all() == ESP_OK, "erase_all succeeds");
    CHECK(michi_pairing_validate_token(token, device_id, sizeof(device_id),
                                       &perms) == ESP_ERR_NOT_FOUND,
          "token invalid after factory reset");
    CHECK(michi_pairing_list(list, sizeof(list)) == ESP_OK &&
              list[0] == '\0',
          "registry empty after factory reset");
    CHECK(michi_pairing_shutdown() == ESP_OK, "shutdown succeeds");
}

/* ── shim sanity ──────────────────────────────────────────── */

static void test_sha256_known_answer(void)
{
    printf("pairing: host SHA-256 test double known-answer\n");
    /* SHA-256("abc") - the pairing digest chain relies on this shim. */
    static const uint8_t expect[32] = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
        0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
        0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
        0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad,
    };
    uint8_t out[32];
    CHECK(mbedtls_sha256((const unsigned char *)"abc", 3, out, 0) == 0,
          "sha256 runs");
    CHECK(memcmp(out, expect, sizeof(expect)) == 0, "sha256('abc') correct");
}

int main(void)
{
    test_sha256_known_answer();
    test_window_closed_rejects_start();
    test_window_open_and_start();
    test_window_expiry();
    test_window_reopen_replaces();
    test_invalid_challenge();
    test_start_rate_limits();
    test_status_and_wrong_pin();
    test_pin_lockout_sixth_attempt();
    test_confirm_success_and_token();
    test_reboot_persists_digest_only();
    test_revocation_and_erase_all();

    if (failures == 0) {
        printf("test_michi_pairing: all tests passed\n");
        return 0;
    }
    printf("test_michi_pairing: %d check(s) FAILED\n", failures);
    return 1;
}
