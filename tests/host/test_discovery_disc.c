/* Runtime discovery hardening suite (DISC-01..DISC-15).
 *
 * Exercises the REAL announce path end to end - product profile ->
 * runtime discovery state -> canonical serialization -> Ed25519
 * signature -> JSON datagram -> Michi Link verification - compiling the
 * SAME firmware sources the device runs:
 *  - michi_discovery.c (runtime: gate, socket, timer, state machine)
 *  - announce.c (canonical builder + signing) + discovery_nvs.c
 *  - michi_time.c (SNTP wall clock gating the signed announce)
 *  - michi_identity component (real Ed25519 via monocypher + BLAKE3)
 *  - capabilities.c + canonical_json.c (the /server/info builder,
 *    DISC-05 parity)
 * against the host shims. No ideal objects are built by hand: every
 * datagram under test was produced by the runtime's announce_now_locked
 * path and captured by the lwip socket shim.
 *
 * Overlap with the other suites is intentional (each case is re-proven
 * over the real datagram, not reimplemented):
 *  - test_michi_time.c covers the clock gate lifecycle (start/stop
 *    semantics, defer logging, stale sync give) - DISC-01/02/03 assert
 *    the same runtime gate from the datagram side;
 *  - test_michi_discovery.c covers the canonical bytes + golden vector
 *    signature - DISC-06/07/08 run the same verify path over a
 *    RUNTIME-produced datagram;
 *  - test_capability_parity.c covers announce vs /server/info at the
 *    builder level - DISC-05 compares the RUNTIME datagram against
 *    /server/info.
 *
 * DISC-09/10 implement the consumer-side replay check the contract
 * delegates to the receiver: the discovery-announce schema only
 * REQUIRES a fresh nonce (>= 22 base64url chars, 16 bytes) per
 * datagram and exposes REPLAY_DETECTED as an error code - it does not
 * prescribe the cache algorithm. This suite models the receiver with a
 * mini verifier: seen-nonce tracking keyed on the nonce string with a
 * monotonic first-seen timestamp (CLOCK_MONOTONIC). No contract change.
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "cJSON.h"

#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "mdns.h"
#include "michi_discovery.h"
#include "michi_http.h"
#include "michi_identity.h"
#include "michi_product_profile_fake.h"
#include "michi_time.h"
#include "nvs.h"
#include "time_shim.h"

static int failures = 0;

/* Each assertion carries its DISC number so every case is traceable. */
#define DISC(num, cond, msg)                                                \
    do {                                                                    \
        if (!(cond)) {                                                      \
            printf("  FAIL DISC-%02d: %s\n", num, msg);                     \
            failures++;                                                     \
        }                                                                   \
    } while (0)

#define INJECTED_UNIX 1767225600ULL

/* ------------------------------------------------------------------ */
/* datagram capture helpers                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    char raw[MICHI_DISCOVERY_MAX_DATAGRAM_BYTES + 1];
    size_t raw_len;
    char device_id[MICHI_DISCOVERY_UUID_LEN];
    char name[64];
    char service[64];
    char api_version[32];
    char host[32];
    int port;
    bool f_session;
    bool f_heartbeat;
    bool f_volume;
    char michi_id[MICHI_IDENTITY_MICHI_ID_LEN];
    char public_key[MICHI_IDENTITY_PUBLIC_KEY_B64_LEN];
    char nonce[64];
    char signature[MICHI_IDENTITY_SIGNATURE_B64_LEN];
    int64_t timestamp_ms;
    bool ok;
} disc_datagram_t;

static disc_datagram_t s_first;  /* first synced announce (DISC-01..15) */
static disc_datagram_t s_second; /* first periodic tick (DISC-10/12)   */
static disc_datagram_t s_third;  /* reconnect announce (DISC-11/12)    */

static bool field_str(const cJSON *root, const char *key, char *out,
                      size_t out_len)
{
    const cJSON *v = cJSON_GetObjectItem(root, key);
    if (v == NULL || !cJSON_IsString(v)) {
        out[0] = '\0';
        return false;
    }
    snprintf(out, out_len, "%s", v->valuestring);
    return true;
}

static bool field_bool(const cJSON *features, const char *key, bool *out)
{
    const cJSON *v = cJSON_GetObjectItem(features, key);
    if (v == NULL || !cJSON_IsBool(v)) {
        return false;
    }
    *out = cJSON_IsTrue(v);
    return true;
}

/* Copies the last captured datagram and parses every field the
 * verification path needs (canonical rebuild + signature check). */
static bool disc_fetch(disc_datagram_t *d)
{
    memset(d, 0, sizeof(*d));
    if (!test_socket_last_datagram(d->raw, sizeof(d->raw), &d->raw_len)) {
        return false;
    }
    cJSON *root = cJSON_Parse(d->raw);
    if (root == NULL) {
        return false;
    }
    const cJSON *feat = cJSON_GetObjectItem(root, "features");
    const cJSON *port = cJSON_GetObjectItem(root, "port");
    const cJSON *ts = cJSON_GetObjectItem(root, "timestamp_ms");
    d->ok =
        field_str(root, "device_id", d->device_id, sizeof(d->device_id)) &&
        field_str(root, "name", d->name, sizeof(d->name)) &&
        field_str(root, "service", d->service, sizeof(d->service)) &&
        field_str(root, "api_version", d->api_version,
                  sizeof(d->api_version)) &&
        field_str(root, "host", d->host, sizeof(d->host)) &&
        port != NULL && cJSON_IsNumber(port) &&
        field_bool(feat, "session", &d->f_session) &&
        field_bool(feat, "heartbeat", &d->f_heartbeat) &&
        field_bool(feat, "volume", &d->f_volume) &&
        field_str(root, "michi_id", d->michi_id, sizeof(d->michi_id)) &&
        field_str(root, "public_key", d->public_key,
                  sizeof(d->public_key)) &&
        field_str(root, "nonce", d->nonce, sizeof(d->nonce)) &&
        field_str(root, "signature", d->signature, sizeof(d->signature)) &&
        ts != NULL && cJSON_IsNumber(ts);
    if (d->ok) {
        d->port = (int)port->valueint;
        d->timestamp_ms = (int64_t)ts->valuedouble;
    }
    cJSON_Delete(root);
    return d->ok;
}

/* Rebuilds the canonical payload from the PARSED datagram fields with
 * the real builder - the exact bytes the runtime signed (proven by the
 * verify in DISC-07). */
static bool disc_rebuild_canonical(const disc_datagram_t *d, char *out,
                                   size_t out_len)
{
    const michi_discovery_announce_t a = {
        .device_id = d->device_id,
        .name = d->name,
        .service = d->service,
        .api_version = d->api_version,
        .host = d->host,
        .port = (uint16_t)d->port,
        .feature_session = d->f_session,
        .feature_heartbeat = d->f_heartbeat,
        .feature_volume = d->f_volume,
        .michi_id = d->michi_id,
        .public_key = d->public_key,
        .timestamp_ms = d->timestamp_ms,
        .nonce = d->nonce,
    };
    return michi_discovery_canonical_json(&a, out, out_len) == ESP_OK;
}

/* Decodes the datagram key material and verifies the signature over the
 * rebuilt canonical payload (michi_identity -> monocypher Ed25519). */
static bool disc_verify(const disc_datagram_t *d)
{
    uint8_t pk[MICHI_IDENTITY_KEY_BYTES];
    uint8_t sig[MICHI_IDENTITY_SIGNATURE_BYTES];
    size_t pk_len = 0, sig_len = 0;
    char canonical[MICHI_DISCOVERY_MAX_DATAGRAM_BYTES];
    if (michi_identity_base64url_decode(d->public_key, pk, sizeof(pk),
                                        &pk_len) != ESP_OK ||
        michi_identity_base64url_decode(d->signature, sig, sizeof(sig),
                                        &sig_len) != ESP_OK ||
        pk_len != MICHI_IDENTITY_KEY_BYTES ||
        sig_len != MICHI_IDENTITY_SIGNATURE_BYTES ||
        !disc_rebuild_canonical(d, canonical, sizeof(canonical))) {
        return false;
    }
    return michi_identity_verify((const uint8_t *)canonical,
                                 strlen(canonical), sig, pk);
}

/* The datagram the runtime sends must be addressed to the canonical
 * multicast group (contract 2.2) - captured by the socket shim. */
static bool disc_dest_is_multicast(void)
{
    uint32_t ip = 0;
    uint16_t port = 0;
    if (!test_socket_last_dest(&ip, &port)) {
        return false;
    }
    return ip == inet_addr(MICHI_DISCOVERY_MULTICAST_GROUP) &&
           port == htons(MICHI_DISCOVERY_MULTICAST_PORT);
}

/* ------------------------------------------------------------------ */
/* consumer-side replay verifier (DISC-09/10)                         */
/* ------------------------------------------------------------------ */

/* Mini verifier modeling the Michi Link receiver: every datagram nonce
 * is recorded on first sight (monotonic timestamp); a repeated nonce is
 * rejected. The contract only REQUIRES the nonce (schema) and exposes
 * REPLAY_DETECTED - the cache policy is the receiver's. */
#define DISC_REPLAY_CAPACITY 64

typedef struct {
    char nonce[64];
    long first_seen_ms;
} disc_replay_entry_t;

static disc_replay_entry_t s_replay[DISC_REPLAY_CAPACITY];
static size_t s_replay_count;

static long disc_mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* true = fresh (recorded), false = replay (already seen). */
static bool disc_replay_observe(const char *nonce)
{
    for (size_t i = 0; i < s_replay_count; i++) {
        if (strcmp(s_replay[i].nonce, nonce) == 0) {
            return false;
        }
    }
    if (s_replay_count >= DISC_REPLAY_CAPACITY) {
        return false; /* capacity far beyond this suite's usage */
    }
    snprintf(s_replay[s_replay_count].nonce,
             sizeof(s_replay[s_replay_count].nonce), "%s", nonce);
    s_replay[s_replay_count].first_seen_ms = disc_mono_ms();
    s_replay_count++;
    return true;
}

/* Receives a datagram and checks its nonce (the receiver path). */
static bool disc_replay_observe_raw(const char *raw)
{
    cJSON *root = cJSON_Parse(raw);
    if (root == NULL) {
        return false;
    }
    const cJSON *n = cJSON_GetObjectItem(root, "nonce");
    bool fresh = false;
    if (n != NULL && cJSON_IsString(n) && n->valuestring[0] != '\0') {
        fresh = disc_replay_observe(n->valuestring);
    }
    cJSON_Delete(root);
    return fresh;
}

/* ------------------------------------------------------------------ */
/* boot / teardown                                                     */
/* ------------------------------------------------------------------ */

static void reset_all(void)
{
    test_nvs_reset();
    michi_identity_test_reset();
    test_sntp_reset();
    test_socket_reset();
    test_mdns_reset();
    test_esp_timer_reset();
    test_time_reset();
    test_profile_reset();
    memset(&s_first, 0, sizeof(s_first));
    memset(&s_second, 0, sizeof(s_second));
    memset(&s_third, 0, sizeof(s_third));
    s_replay_count = 0;
}

static void boot_time_and_discovery(void)
{
    DISC(0, michi_time_init() == ESP_OK, "michi_time_init succeeds");
    DISC(0, michi_discovery_init() == ESP_OK,
         "michi_discovery_init succeeds");
}

static void teardown(void)
{
    (void)michi_discovery_shutdown();
    (void)michi_time_shutdown();
}

static bool sent_at_least_one(void)
{
    return test_socket_sent_count() >= 1;
}

static bool wait_for(bool (*cond)(void), int timeout_ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const long deadline_ns = (long)ts.tv_sec * 1000000000L + ts.tv_nsec +
                             (long)timeout_ms * 1000000L;
    do {
        if (cond()) {
            return true;
        }
        usleep(5000);
        clock_gettime(CLOCK_MONOTONIC, &ts);
    } while ((long)ts.tv_sec * 1000000000L + ts.tv_nsec < deadline_ns);
    return cond();
}

/* ------------------------------------------------------------------ */
/* DISC-01: clock unsynchronized -> no signed announce                */
/* ------------------------------------------------------------------ */

static void disc01_unsynced_clock_no_announce(void)
{
    printf("DISC-01: unsynchronized clock -> no signed announce\n");
    reset_all();
    boot_time_and_discovery();

    DISC(1, michi_time_is_synchronized() == false,
         "clock starts unsynchronized");
    DISC(1, michi_identity_get_state() == MICHI_IDENTITY_READY,
         "identity is READY (the ONLY gate is the clock)");
    DISC(1, michi_discovery_start("192.168.1.102") == ESP_OK,
         "discovery starts while the clock is unsynchronized");
    DISC(1, test_socket_sent_count() == 0,
         "no announce datagram emitted while unsynchronized");

    /* The gate holds across periodic ticks too (defer, never spam). */
    test_esp_timer_advance(40000000);
    test_esp_timer_advance(40000000);
    DISC(1, test_socket_sent_count() == 0,
         "periodic ticks still emit nothing while unsynchronized");

    teardown();
}

/* ------------------------------------------------------------------ */
/* DISC-02: synchronized clock -> announce emitted                    */
/* ------------------------------------------------------------------ */

static void disc02_synced_clock_announce_emitted(void)
{
    printf("DISC-02: synchronized clock -> announce emitted\n");
    reset_all();
    boot_time_and_discovery();

    DISC(2, michi_discovery_start("192.168.1.102") == ESP_OK,
         "discovery starts (gated)");
    DISC(2, test_socket_sent_count() == 0, "gated before the sync");
    DISC(2, michi_time_start() == ESP_OK, "time start (GOT_IP) succeeds");

    test_esp_timer_set_time(1000000);
    test_sntp_fire_sync(INJECTED_UNIX);

    DISC(2, wait_for(sent_at_least_one, 2000),
         "announce emitted once the clock is synchronized");
    DISC(2, michi_time_is_synchronized(), "runtime clock state: synced");
    DISC(2, disc_fetch(&s_first), "emitted datagram parses");
    DISC(2, s_first.signature[0] != '\0',
         "the emitted datagram carries a signature");

    teardown();
}

/* ------------------------------------------------------------------ */
/* DISC-03: timestamp within +-90 s                                   */
/* ------------------------------------------------------------------ */

static void disc03_timestamp_within_90s(void)
{
    printf("DISC-03: timestamp within +-90 s\n");
    reset_all();
    boot_time_and_discovery();
    DISC(3, michi_discovery_start("192.168.1.102") == ESP_OK, "start");
    DISC(3, michi_time_start() == ESP_OK, "time start");
    test_esp_timer_set_time(1000000);
    test_sntp_fire_sync(INJECTED_UNIX);
    DISC(3, wait_for(sent_at_least_one, 2000), "announce emitted");
    DISC(3, disc_fetch(&s_first), "datagram parses");

    const int64_t injected_ms = (int64_t)INJECTED_UNIX * 1000;
    DISC(3, s_first.timestamp_ms > 0,
         "timestamp is a positive wall-clock value");
    DISC(3, injected_ms - s_first.timestamp_ms <= 90000 &&
                s_first.timestamp_ms - injected_ms <= 90000,
         "timestamp within +-90 s of the synchronized clock");
    DISC(3, michi_time_unix_ms() == injected_ms,
         "the runtime timestamp source is the synchronized clock");

    teardown();
}

/* ------------------------------------------------------------------ */
/* DISC-04: feature flags session/heartbeat/volume true               */
/* ------------------------------------------------------------------ */

static void disc04_feature_flags_true(void)
{
    printf("DISC-04: feature flags session/heartbeat/volume true\n");
    reset_all();
    boot_time_and_discovery();
    DISC(4, michi_discovery_start("192.168.1.102") == ESP_OK, "start");
    DISC(4, michi_time_start() == ESP_OK, "time start");
    test_esp_timer_set_time(1000000);
    test_sntp_fire_sync(INJECTED_UNIX);
    DISC(4, wait_for(sent_at_least_one, 2000), "announce emitted");
    DISC(4, disc_fetch(&s_first), "datagram parses");

    DISC(4, s_first.f_session, "features.session is true");
    DISC(4, s_first.f_heartbeat, "features.heartbeat is true");
    DISC(4, s_first.f_volume, "features.volume is true");

    teardown();
}

/* ------------------------------------------------------------------ */
/* DISC-05: feature flags match /server/info                          */
/* ------------------------------------------------------------------ */

static void disc05_flags_match_server_info(void)
{
    printf("DISC-05: feature flags match /server/info\n");
    reset_all();
    boot_time_and_discovery();
    DISC(5, michi_discovery_start("192.168.1.102") == ESP_OK, "start");
    DISC(5, michi_time_start() == ESP_OK, "time start");
    test_esp_timer_set_time(1000000);
    test_sntp_fire_sync(INJECTED_UNIX);
    DISC(5, wait_for(sent_at_least_one, 2000), "announce emitted");
    DISC(5, disc_fetch(&s_first), "datagram parses");

    /* /server/info built by the REAL build_info_json from the SAME
     * runtime profile the announce consumed (michi_product_profile_get)
     * - the capability flags come from the canonical getter inside. */
    cJSON *info_root = cJSON_CreateObject();
    DISC(5, info_root != NULL &&
                build_info_json(info_root, michi_product_profile_get()) ==
                    ESP_OK,
         "server info build succeeds (runtime profile)");
    bool ok = false;
    bool i_session = false, i_heartbeat = false, i_volume = false;
    if (info_root != NULL) {
        const cJSON *feat = cJSON_GetObjectItem(info_root, "features");
        ok = field_bool(feat, "session", &i_session) &&
             field_bool(feat, "heartbeat", &i_heartbeat) &&
             field_bool(feat, "volume", &i_volume);
    }
    DISC(5, ok, "server info features parse");
    if (ok) {
        DISC(5, s_first.f_session == i_session,
             "discovery.features.session == server_info.features.session");
        DISC(5, s_first.f_heartbeat == i_heartbeat,
             "discovery.features.heartbeat == server_info.features.heartbeat");
        DISC(5, s_first.f_volume == i_volume,
             "discovery.features.volume == server_info.features.volume");
    }
    if (info_root != NULL) {
        cJSON_Delete(info_root);
    }

    teardown();
}

/* ------------------------------------------------------------------ */
/* DISC-06: michi_id derives from public_key                          */
/* ------------------------------------------------------------------ */

static void disc06_michi_id_derives_from_public_key(void)
{
    printf("DISC-06: michi_id derives from public_key\n");
    reset_all();
    boot_time_and_discovery();
    DISC(6, michi_discovery_start("192.168.1.102") == ESP_OK, "start");
    DISC(6, michi_time_start() == ESP_OK, "time start");
    test_esp_timer_set_time(1000000);
    test_sntp_fire_sync(INJECTED_UNIX);
    DISC(6, wait_for(sent_at_least_one, 2000), "announce emitted");
    DISC(6, disc_fetch(&s_first), "datagram parses");

    uint8_t pk[MICHI_IDENTITY_KEY_BYTES];
    size_t pk_len = 0;
    char derived[MICHI_IDENTITY_MICHI_ID_LEN];
    DISC(6, michi_identity_base64url_decode(s_first.public_key, pk,
                                            sizeof(pk), &pk_len) == ESP_OK &&
                pk_len == MICHI_IDENTITY_KEY_BYTES,
         "datagram public_key decodes to 32 raw bytes");
    DISC(6, michi_identity_derive_michi_id(pk, derived, sizeof(derived)) ==
                ESP_OK,
         "michi_id derivation succeeds (BLAKE3)");
    DISC(6, strcmp(s_first.michi_id, derived) == 0,
         "datagram michi_id == base64url(blake3(public_key))");

    teardown();
}

/* ------------------------------------------------------------------ */
/* DISC-07: signature verifies                                        */
/* ------------------------------------------------------------------ */

static void disc07_signature_verifies(void)
{
    printf("DISC-07: signature verifies\n");
    reset_all();
    boot_time_and_discovery();
    DISC(7, michi_discovery_start("192.168.1.102") == ESP_OK, "start");
    DISC(7, michi_time_start() == ESP_OK, "time start");
    test_esp_timer_set_time(1000000);
    test_sntp_fire_sync(INJECTED_UNIX);
    DISC(7, wait_for(sent_at_least_one, 2000), "announce emitted");
    DISC(7, disc_fetch(&s_first), "datagram parses");

    DISC(7, disc_verify(&s_first),
         "Ed25519 signature verifies over the rebuilt canonical payload "
         "(monocypher)");

    teardown();
}

/* ------------------------------------------------------------------ */
/* DISC-08: altered signature rejected                                */
/* ------------------------------------------------------------------ */

static void disc08_altered_signature_rejected(void)
{
    printf("DISC-08: altered signature rejected\n");
    reset_all();
    boot_time_and_discovery();
    DISC(8, michi_discovery_start("192.168.1.102") == ESP_OK, "start");
    DISC(8, michi_time_start() == ESP_OK, "time start");
    test_esp_timer_set_time(1000000);
    test_sntp_fire_sync(INJECTED_UNIX);
    DISC(8, wait_for(sent_at_least_one, 2000), "announce emitted");
    DISC(8, disc_fetch(&s_first), "datagram parses");
    DISC(8, disc_verify(&s_first), "baseline: the runtime signature verifies");

    /* Tamper the signature raw bytes: the SAME payload must reject. */
    uint8_t pk[MICHI_IDENTITY_KEY_BYTES];
    uint8_t sig[MICHI_IDENTITY_SIGNATURE_BYTES];
    size_t pk_len = 0, sig_len = 0;
    char canonical[MICHI_DISCOVERY_MAX_DATAGRAM_BYTES];
    DISC(8, michi_identity_base64url_decode(s_first.public_key, pk,
                                            sizeof(pk), &pk_len) == ESP_OK &&
                michi_identity_base64url_decode(s_first.signature, sig,
                                                sizeof(sig), &sig_len) ==
                    ESP_OK &&
                pk_len == MICHI_IDENTITY_KEY_BYTES &&
                sig_len == MICHI_IDENTITY_SIGNATURE_BYTES &&
                disc_rebuild_canonical(&s_first, canonical,
                                       sizeof(canonical)),
         "key material decodes for the tamper check");
    if (pk_len == MICHI_IDENTITY_KEY_BYTES &&
        sig_len == MICHI_IDENTITY_SIGNATURE_BYTES) {
        sig[0] ^= 0x01;
        DISC(8, !michi_identity_verify((const uint8_t *)canonical,
                                       strlen(canonical), sig, pk),
             "flipped signature byte rejected");
        /* Supporting integrity assert: a tampered canonical payload
         * also rejects under the ORIGINAL signature. */
        sig[0] ^= 0x01;
        char tampered[MICHI_DISCOVERY_MAX_DATAGRAM_BYTES];
        snprintf(tampered, sizeof(tampered), "%s", canonical);
        tampered[10] ^= 0x01;
        DISC(8, !michi_identity_verify((const uint8_t *)tampered,
                                       strlen(tampered), sig, pk),
             "tampered payload rejected under the original signature");
    }

    teardown();
}

/* ------------------------------------------------------------------ */
/* DISC-09: repeated nonce rejected (consumer-side replay check)      */
/* ------------------------------------------------------------------ */

static void disc09_repeated_nonce_rejected(void)
{
    printf("DISC-09: repeated nonce rejected (consumer replay check)\n");
    reset_all();
    boot_time_and_discovery();
    DISC(9, michi_discovery_start("192.168.1.102") == ESP_OK, "start");
    DISC(9, michi_time_start() == ESP_OK, "time start");
    test_esp_timer_set_time(1000000);
    test_sntp_fire_sync(INJECTED_UNIX);
    DISC(9, wait_for(sent_at_least_one, 2000), "announce emitted");
    DISC(9, disc_fetch(&s_first), "datagram parses");

    /* Receiver sees the datagram for the first time: accepted. */
    DISC(9, disc_replay_observe_raw(s_first.raw),
         "first sight of the datagram nonce accepted");
    /* The SAME datagram is replayed: rejected (nonce already seen). */
    DISC(9, !disc_replay_observe_raw(s_first.raw),
         "replayed datagram (same nonce) rejected");

    teardown();
}

/* ------------------------------------------------------------------ */
/* DISC-10: fresh nonce accepted                                      */
/* ------------------------------------------------------------------ */

static void disc10_fresh_nonce_accepted(void)
{
    printf("DISC-10: fresh nonce accepted\n");
    reset_all();
    boot_time_and_discovery();
    DISC(10, michi_discovery_start("192.168.1.102") == ESP_OK, "start");
    DISC(10, michi_time_start() == ESP_OK, "time start");
    test_esp_timer_set_time(1000000);
    test_sntp_fire_sync(INJECTED_UNIX);
    DISC(10, wait_for(sent_at_least_one, 2000), "first announce emitted");
    DISC(10, disc_fetch(&s_first), "first datagram parses");
    DISC(10, disc_replay_observe_raw(s_first.raw),
         "first nonce recorded by the receiver");

    /* One periodic tick (30 s +-3 s) -> a NEW announce, a NEW nonce. */
    test_esp_timer_advance(40000000);
    DISC(10, test_socket_sent_count() >= 2,
         "periodic tick emits the next announce");
    DISC(10, disc_fetch(&s_second), "second datagram parses");
    DISC(10, strcmp(s_second.nonce, s_first.nonce) != 0,
         "each announce carries a distinct nonce");

    /* Schema compliance of the fresh nonce: >= 22 base64url chars. */
    uint8_t nonce_raw[32];
    size_t nonce_len = 0;
    DISC(10, strlen(s_second.nonce) >= 22,
         "fresh nonce is >= 22 base64url chars (schema)");
    DISC(10, michi_identity_base64url_decode(s_second.nonce, nonce_raw,
                                             sizeof(nonce_raw), &nonce_len) ==
                 ESP_OK &&
                 nonce_len == 16,
         "fresh nonce decodes to 16 raw bytes");

    /* The receiver accepts the fresh nonce. */
    DISC(10, disc_replay_observe_raw(s_second.raw),
         "fresh nonce accepted by the replay verifier");
    DISC(10, !disc_replay_observe_raw(s_second.raw),
         "the fresh nonce is only accepted once");

    teardown();
}

/* ------------------------------------------------------------------ */
/* DISC-12: reconnect creates a new nonce                             */
/* ------------------------------------------------------------------ */

static void disc12_reconnect_creates_new_nonce(void)
{
    printf("DISC-12: reconnect creates a new nonce\n");
    reset_all();
    boot_time_and_discovery();
    DISC(12, michi_discovery_start("192.168.1.102") == ESP_OK, "start");
    DISC(12, michi_time_start() == ESP_OK, "time start");
    test_esp_timer_set_time(1000000);
    test_sntp_fire_sync(INJECTED_UNIX);
    DISC(12, wait_for(sent_at_least_one, 2000), "first announce emitted");
    DISC(12, disc_fetch(&s_first), "first datagram parses");

    /* Link down: the runtime stops announcing and closes the socket. */
    DISC(12, michi_discovery_stop() == ESP_OK, "discovery stops on link down");
    DISC(12, michi_time_stop() == ESP_OK, "time stop on link down");
    const int sent_before = test_socket_sent_count();

    /* Reconnect (GOT_IP): the runtime announces IMMEDIATELY - with a
     * fresh nonce, never the previous one. */
    DISC(12, michi_discovery_start("192.168.1.102") == ESP_OK,
         "discovery restarts on reconnect");
    DISC(12, michi_time_start() == ESP_OK, "time restarts on reconnect");
    DISC(12, test_socket_sent_count() > sent_before,
         "reconnect emits a new announce immediately");
    DISC(12, disc_fetch(&s_third), "reconnect datagram parses");
    DISC(12, strcmp(s_third.nonce, s_first.nonce) != 0,
         "reconnect nonce differs from the pre-outage nonce");
    DISC(12, disc_replay_observe_raw(s_third.raw),
         "reconnect nonce is fresh for the receiver");

    teardown();
}

/* ------------------------------------------------------------------ */
/* DISC-11: announced host equals the datagram source (runtime IPv4)  */
/* ------------------------------------------------------------------ */

static void disc11_announced_host_equals_datagram_source(void)
{
    printf("DISC-11: announced host equals the datagram source\n");
    reset_all();
    boot_time_and_discovery();

    /* Boot + sync on the first IPv4. */
    DISC(11, michi_discovery_start("192.168.1.102") == ESP_OK, "start");
    DISC(11, michi_time_start() == ESP_OK, "time start");
    test_esp_timer_set_time(1000000);
    test_sntp_fire_sync(INJECTED_UNIX);
    DISC(11, wait_for(sent_at_least_one, 2000), "announce emitted");
    DISC(11, disc_fetch(&s_first), "datagram parses");
    DISC(11, strcmp(s_first.host, "192.168.1.102") == 0,
         "announced host == the runtime IPv4 (192.168.1.102)");
    DISC(11, disc_dest_is_multicast(),
         "datagram addressed to the canonical group 224.0.0.167:53318");

    /* IP change: the runtime re-opens the socket and announces the NEW
     * address - the datagram source follows the runtime state. */
    DISC(11, michi_discovery_start("192.168.1.55") == ESP_OK,
         "start with a new IPv4 (GOT_IP after renewal)");
    disc_datagram_t d4;
    DISC(11, disc_fetch(&d4), "post-renewal datagram parses");
    DISC(11, strcmp(d4.host, "192.168.1.55") == 0,
         "announced host updated to the new runtime IPv4");
    DISC(11, strcmp(d4.host, s_first.host) != 0,
         "the datagram source changed with the runtime state");
    DISC(11, disc_dest_is_multicast(),
         "post-renewal datagram still targets the canonical group");

    teardown();
}

/* ------------------------------------------------------------------ */
/* DISC-13: service standard correct                                  */
/* ------------------------------------------------------------------ */

static void disc13_service_standard_correct(void)
{
    printf("DISC-13: service standard correct\n");
    reset_all();
    boot_time_and_discovery();
    test_profile_set_tier(MICHI_PRODUCT_STANDARD);

    DISC(13, michi_discovery_start("192.168.1.102") == ESP_OK, "start");
    DISC(13, michi_time_start() == ESP_OK, "time start");
    test_esp_timer_set_time(1000000);
    test_sntp_fire_sync(INJECTED_UNIX);
    DISC(13, wait_for(sent_at_least_one, 2000), "announce emitted");
    disc_datagram_t d;
    DISC(13, disc_fetch(&d), "datagram parses");

    DISC(13, strcmp(d.service, "michi-stream-standard") == 0,
         "STANDARD tier announces michi-stream-standard");
    DISC(13, strcmp(michi_product_profile_tier_name(), "standard") == 0,
         "runtime tier name is standard (no drift)");

    teardown();
}

/* ------------------------------------------------------------------ */
/* DISC-14: service hifi correct                                      */
/* ------------------------------------------------------------------ */

static void disc14_service_hifi_correct(void)
{
    printf("DISC-14: service hifi correct\n");
    reset_all();
    boot_time_and_discovery();
    test_profile_set_tier(MICHI_PRODUCT_HIFI);

    DISC(14, michi_discovery_start("192.168.1.102") == ESP_OK, "start");
    DISC(14, michi_time_start() == ESP_OK, "time start");
    test_esp_timer_set_time(1000000);
    test_sntp_fire_sync(INJECTED_UNIX);
    DISC(14, wait_for(sent_at_least_one, 2000), "announce emitted");
    disc_datagram_t d;
    DISC(14, disc_fetch(&d), "datagram parses");

    DISC(14, strcmp(d.service, "michi-stream-hifi") == 0,
         "HIFI tier announces michi-stream-hifi");
    DISC(14, strcmp(michi_product_profile_tier_name(), "hifi") == 0,
         "runtime tier name is hifi (no drift)");

    teardown();
}

/* ------------------------------------------------------------------ */
/* DISC-15: port equals the real HTTP port                            */
/* ------------------------------------------------------------------ */

static void disc15_port_equals_real_http_port(void)
{
    printf("DISC-15: port equals the real HTTP port\n");
    reset_all();
    boot_time_and_discovery();

    DISC(15, michi_discovery_start("192.168.1.102") == ESP_OK, "start");
    DISC(15, michi_time_start() == ESP_OK, "time start");
    test_esp_timer_set_time(1000000);
    test_sntp_fire_sync(INJECTED_UNIX);
    DISC(15, wait_for(sent_at_least_one, 2000), "announce emitted");
    disc_datagram_t d;
    DISC(15, disc_fetch(&d), "datagram parses");

    /* The announce carries the REAL HTTP port (michi_http serves 80:
     * http_server.c MICHI_HTTP_PORT == 80, mirrored by the documented
     * MICHI_DISCOVERY_HTTP_PORT coupling - see michi_discovery.h). */
    DISC(15, d.port == MICHI_DISCOVERY_HTTP_PORT,
         "announced port == MICHI_DISCOVERY_HTTP_PORT");
    DISC(15, d.port == 80, "announced port is the real HTTP port (80)");
    DISC(15, MICHI_DISCOVERY_HTTP_PORT == 80,
         "the mirror constant is the real serving port");

    /* The announce transport is NOT the HTTP port: the datagram went to
     * the multicast port while the payload advertises the HTTP one. */
    uint16_t dst_port = 0;
    DISC(15, test_socket_last_dest(NULL, &dst_port) &&
                 dst_port == htons(MICHI_DISCOVERY_MULTICAST_PORT),
         "transport used the multicast port (53318), not the HTTP port");
    DISC(15, (int)htons(dst_port) != d.port,
         "announced port and transport port are distinct");

    teardown();
}

/* ------------------------------------------------------------------ */

int main(void)
{
    disc01_unsynced_clock_no_announce();
    disc02_synced_clock_announce_emitted();
    disc03_timestamp_within_90s();
    disc04_feature_flags_true();
    disc05_flags_match_server_info();
    disc06_michi_id_derives_from_public_key();
    disc07_signature_verifies();
    disc08_altered_signature_rejected();
    disc09_repeated_nonce_rejected();
    disc10_fresh_nonce_accepted();
    disc12_reconnect_creates_new_nonce();
    disc11_announced_host_equals_datagram_source();
    disc13_service_standard_correct();
    disc14_service_hifi_correct();
    disc15_port_equals_real_http_port();

    if (failures == 0) {
        printf("test_discovery_disc: all DISC-01..DISC-15 passed\n");
        return 0;
    }
    printf("test_discovery_disc: %d check(s) FAILED\n", failures);
    return 1;
}
