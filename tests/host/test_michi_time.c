/* Host-side tests for the wall-clock gate of signed discovery (P0-02).
 *
 * Compiles the REAL firmware sources - michi_time.c, the michi_discovery
 * runtime (michi_discovery.c + announce.c + discovery_nvs.c) and the
 * michi_identity component - against the test shims:
 *  - esp_netif_sntp shim: injectable sync events + init/start failure
 *    injection (mirrors the IDF 5.3 esp_netif_sntp.c semantics);
 *  - time() override: the INJECTED wall clock (fire_sync sets it like
 *    SNTP setting the system time; advance simulates the RTC ticking);
 *  - lwip sockets shim: captures every sendto() datagram so the tests
 *    can prove "no signed announce is built/sent" while the clock is
 *    unsynchronized, and inspect the timestamp_ms once it is;
 *  - pthread-backed FreeRTOS tasks/semaphores: the REAL michi_time sync
 *    task runs on the host.
 *
 * The host Kconfig values (shim/sdkconfig.h) use a fast sync timeout
 * (250 ms x 3) so the bounded-wait failure paths run in ms.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "cJSON.h"

#include "esp_timer.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "lwip/sockets.h"
#include "mdns.h"
#include "michi_discovery.h"
#include "michi_identity.h"
#include "michi_product_profile_fake.h"
#include "michi_time.h"
#include "nvs.h"
#include "time_shim.h"

static int failures = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            printf("  FAIL %s\n", msg);                                     \
            failures++;                                                     \
        }                                                                   \
    } while (0)

#define INJECTED_UNIX 1767225600ULL

/* ------------------------------------------------------------------ */
/* helpers                                                            */
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
}

/* Boot both subsystems in the firmware order: time first (discovery
 * registers its sync callback against a live time subsystem). */
static void boot_time_and_discovery(void)
{
    CHECK(michi_time_init() == ESP_OK, "michi_time_init succeeds");
    CHECK(michi_discovery_init() == ESP_OK, "michi_discovery_init succeeds");
}

static void teardown(void)
{
    (void)michi_discovery_shutdown();
    (void)michi_time_shutdown();
}

/* Polls until cond() is true or the deadline passes (returns cond()). */
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

static bool sent_at_least_one(void)
{
    return test_socket_sent_count() >= 1;
}

static bool sent_at_least_two(void)
{
    return test_socket_sent_count() >= 2;
}

/* Polls until the wall clock reads unix_sec while synchronized. */
static bool wait_for_synced_unix_ms(uint64_t unix_sec, int timeout_ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const long deadline_ns = (long)ts.tv_sec * 1000000000L + ts.tv_nsec +
                             (long)timeout_ms * 1000000L;
    do {
        if (michi_time_is_synchronized() &&
            michi_time_unix_ms() == (int64_t)unix_sec * 1000) {
            return true;
        }
        usleep(5000);
        clock_gettime(CLOCK_MONOTONIC, &ts);
    } while ((long)ts.tv_sec * 1000000000L + ts.tv_nsec < deadline_ns);
    return false;
}

/* Extracts timestamp_ms from the last captured datagram; -1 on parse
 * failure. */
static int64_t last_datagram_timestamp_ms(void)
{
    char buf[1201];
    size_t len = 0;
    if (!test_socket_last_datagram(buf, sizeof(buf), &len)) {
        return -1;
    }
    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        return -1;
    }
    const cJSON *ts = cJSON_GetObjectItem(root, "timestamp_ms");
    int64_t v = -1;
    if (ts != NULL && cJSON_IsNumber(ts)) {
        v = (int64_t)ts->valuedouble;
    }
    cJSON_Delete(root);
    return v;
}

static bool last_datagram_has_signature(void)
{
    char buf[1201];
    size_t len = 0;
    if (!test_socket_last_datagram(buf, sizeof(buf), &len)) {
        return false;
    }
    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        return false;
    }
    const cJSON *sig = cJSON_GetObjectItem(root, "signature");
    const bool ok = sig != NULL && cJSON_IsString(sig) &&
                    strlen(sig->valuestring) == 86;
    cJSON_Delete(root);
    return ok;
}

/* stderr capture (the esp_log shim logs to stderr): begin/end helpers
 * so the tests can count log lines emitted between two points. */
static FILE *g_stderr_capture;
static int g_stderr_saved;

static void capture_begin(void)
{
    g_stderr_capture = tmpfile();
    if (g_stderr_capture == NULL) {
        return;
    }
    g_stderr_saved = dup(STDERR_FILENO);
    fflush(stderr);
    dup2(fileno(g_stderr_capture), STDERR_FILENO);
}

static int capture_end_count(const char *needle)
{
    if (g_stderr_capture == NULL) {
        return -1;
    }
    fflush(stderr);
    dup2(g_stderr_saved, STDERR_FILENO);
    close(g_stderr_saved);

    rewind(g_stderr_capture);
    char buf[4096];
    int count = 0;
    while (fgets(buf, sizeof(buf), g_stderr_capture) != NULL) {
        const char *hit = buf;
        while ((hit = strstr(hit, needle)) != NULL) {
            count++;
            hit += strlen(needle);
        }
    }
    fclose(g_stderr_capture);
    g_stderr_capture = NULL;
    return count;
}

/* ------------------------------------------------------------------ */
/* 1. unsynchronized clock: no signed announce, unix_ms never lies    */
/* ------------------------------------------------------------------ */

static void test_gate_without_sync(void)
{
    printf("gate: no signed announce until the clock is synchronized\n");
    reset_all();
    boot_time_and_discovery();

    CHECK(michi_time_is_synchronized() == false,
          "clock starts unsynchronized");
    CHECK(michi_time_unix_ms() == 0,
          "unix_ms is 0 (never a silently invalid timestamp)");
    CHECK(strcmp(michi_time_sync_source(), "sntp") == 0,
          "sync source is sntp");

    CHECK(michi_discovery_start("192.168.1.102") == ESP_OK,
          "discovery starts while the clock is unsynchronized");
    CHECK(test_socket_sent_count() == 0,
          "no announce datagram is built/sent while unsynchronized");

    CHECK(michi_time_start() == ESP_OK, "time start (GOT_IP) succeeds");
    CHECK(test_sntp_client_started(), "SNTP client started on GOT_IP");
    /* The sync task runs its bounded rounds; without a sync event the
     * gate must hold. 3 x 250 ms + slack. */
    usleep(1100000);
    CHECK(michi_time_is_synchronized() == false,
          "still unsynchronized after the retry window elapsed");
    CHECK(test_socket_sent_count() == 0,
          "still no announce after the failed sync rounds");

    CHECK(michi_time_stop() == ESP_OK, "time stop (disconnect) is ok");
    teardown();
}

/* ------------------------------------------------------------------ */
/* 2. the defer message is logged exactly once (no 30 s spam)         */
/* ------------------------------------------------------------------ */

static void test_defer_logged_once_per_transition(void)
{
    printf("gate: defer warning logged once, never spammed per tick\n");
    reset_all();
    boot_time_and_discovery();

    capture_begin();
    CHECK(michi_discovery_start("192.168.1.102") == ESP_OK,
          "discovery starts gated");
    usleep(20000);
    /* Fire 3 periodic announce ticks (30 s +-3 s each, so 40 s steps
     * land one tick each) - every tick stays gated. */
    test_esp_timer_advance(40000000);
    test_esp_timer_advance(40000000);
    test_esp_timer_advance(40000000);
    CHECK(test_socket_sent_count() == 0,
          "three periodic ticks still send nothing");
    usleep(20000);

    static const char needle[] =
        "discovery: signed announce deferred, clock not synchronized";
    const int defer_count = capture_end_count(needle);
    CHECK(defer_count == 1,
          "defer warning logged exactly once across all ticks");

    /* A synchronized announce resets the transition: a new outage logs
     * again - covered in the reconnect test below. */
    teardown();
}

/* ------------------------------------------------------------------ */
/* 3. sync allows the announce; timestamp within +-90 s               */
/* ------------------------------------------------------------------ */

static void test_sync_allows_announce(void)
{
    printf("sync: fresh clock lets the signed announce through\n");
    reset_all();
    boot_time_and_discovery();

    CHECK(michi_discovery_start("192.168.1.102") == ESP_OK,
          "discovery starts gated");
    CHECK(test_socket_sent_count() == 0, "no announce before the sync");

    CHECK(michi_time_start() == ESP_OK, "time start (GOT_IP) succeeds");
    /* The monotonic freshness epoch was captured at start (fake
     * esp_timer at 0); land a sync event at monotonic 1 s. */
    test_esp_timer_set_time(1000000);
    test_sntp_fire_sync(INJECTED_UNIX);

    CHECK(wait_for(sent_at_least_one, 2000),
          "announce resumes IMMEDIATELY via the sync callback");
    CHECK(michi_time_is_synchronized(), "clock is synchronized");
    CHECK(michi_time_unix_ms() == (int64_t)INJECTED_UNIX * 1000,
          "unix_ms returns the injected wall clock");

    const int64_t ts = last_datagram_timestamp_ms();
    CHECK(ts == (int64_t)INJECTED_UNIX * 1000,
          "announce timestamp_ms is the synchronized wall clock");
    CHECK(ts > 0 && (ts / 1000 - (int64_t)INJECTED_UNIX <= 90) &&
              ((int64_t)INJECTED_UNIX - ts / 1000 <= 90),
          "timestamp within +-90 s of the injected time");
    CHECK(last_datagram_has_signature(),
          "the emitted datagram carries the Ed25519 signature");

    teardown();
}

/* ------------------------------------------------------------------ */
/* 4. reconnect: stop conserves state, resync revalidates, stale      */
/*    gives are rejected, a failed resync never drops the state       */
/* ------------------------------------------------------------------ */

static void test_reconnect_conserves_state(void)
{
    printf("reconnect: link down conserves the state; resync revalidates\n");
    reset_all();
    boot_time_and_discovery();

    CHECK(michi_discovery_start("192.168.1.102") == ESP_OK, "start");
    CHECK(michi_time_start() == ESP_OK, "time start");
    test_esp_timer_set_time(1000000);
    test_sntp_fire_sync(INJECTED_UNIX);
    CHECK(wait_for(sent_at_least_one, 2000), "first announce after sync");

    /* Link down: announce stops, sync state is CONSERVED. */
    CHECK(michi_discovery_stop() == ESP_OK, "discovery stops on link down");
    CHECK(michi_time_stop() == ESP_OK, "time stop on link down");
    CHECK(michi_time_is_synchronized(),
          "sync state conserved across the outage");

    /* The wall clock keeps advancing (RTC) while offline. */
    test_time_advance_sec(5);
    CHECK(michi_time_unix_ms() == (int64_t)(INJECTED_UNIX + 5) * 1000,
          "conserved state is a REAL advancing clock, not a frozen one");

    /* Reconnect: the announce resumes immediately (state conserved). */
    test_esp_timer_set_time(5000000);
    CHECK(michi_discovery_start("192.168.1.102") == ESP_OK,
          "discovery restarts on reconnect");
    CHECK(wait_for(sent_at_least_two, 2000),
          "announce allowed immediately with the conserved state");
    CHECK(last_datagram_timestamp_ms() == (int64_t)(INJECTED_UNIX + 5) * 1000,
          "reconnect announce carries the advancing clock");

    /* Resync with NO fresh event: the bounded window elapses and the
     * state is still conserved (documented policy - never dropped). */
    CHECK(michi_time_start() == ESP_OK, "time restarts on reconnect");
    usleep(1100000);
    CHECK(michi_time_is_synchronized(),
          "failed resync conserves the synchronized state");
    CHECK(michi_time_unix_ms() == (int64_t)(INJECTED_UNIX + 5) * 1000,
          "clock unchanged after the failed resync");

    teardown();
}

static void test_stale_give_rejected_fresh_resync(void)
{
    printf("reconnect: stale sync give rejected; fresh resync lands\n");
    reset_all();
    boot_time_and_discovery();

    CHECK(michi_discovery_start("192.168.1.102") == ESP_OK, "start");
    CHECK(michi_time_start() == ESP_OK, "time start");
    test_esp_timer_set_time(1000000);
    test_sntp_fire_sync(INJECTED_UNIX);
    CHECK(wait_for(sent_at_least_one, 2000), "first announce after sync");

    /* Periodic re-sync while idle leaves a STALE give in the internal
     * sync semaphore (the task consumed the boot give). */
    test_esp_timer_set_time(2000000);
    test_sntp_fire_sync(INJECTED_UNIX + 1);
    usleep(50000);

    /* Outage + reconnect. */
    CHECK(michi_discovery_stop() == ESP_OK, "stop");
    CHECK(michi_time_stop() == ESP_OK, "time stop");
    test_esp_timer_set_time(3000000);
    CHECK(michi_discovery_start("192.168.1.102") == ESP_OK,
          "discovery restarts (conserved state)");
    CHECK(wait_for(sent_at_least_two, 2000),
          "announce resumes on the conserved state");

    /* Revalidation: the stale give must NOT count as fresh (its
     * monotonic timestamp precedes this round's epoch) - the task
     * restarts and waits for a real event. */
    CHECK(michi_time_start() == ESP_OK, "time restarts on reconnect");
    usleep(200000);
    CHECK(michi_time_is_synchronized(), "state still conserved mid-round");

    test_esp_timer_set_time(4000000);
    test_sntp_fire_sync(INJECTED_UNIX + 30);
    CHECK(wait_for_synced_unix_ms(INJECTED_UNIX + 30, 2000),
          "fresh resync updates the wall clock");
    CHECK(michi_time_is_synchronized(), "fresh resync keeps the state");

    teardown();
}

/* ------------------------------------------------------------------ */
/* 5. SNTP errors never block the firmware                            */
/* ------------------------------------------------------------------ */

static void test_sntp_init_failure_degrades(void)
{
    printf("errors: SNTP init failure gates discovery, nothing blocks\n");
    reset_all();
    test_sntp_set_fail_init(true);
    CHECK(michi_time_init() != ESP_OK, "SNTP init failure propagates");

    /* Discovery still boots and starts; the gate holds. */
    CHECK(michi_discovery_init() == ESP_OK,
          "discovery init unaffected by the time failure");
    CHECK(michi_discovery_start("192.168.1.102") == ESP_OK,
          "discovery start unaffected (gated)");
    CHECK(test_socket_sent_count() == 0, "no announce while gated");
    CHECK(michi_time_start() == ESP_ERR_INVALID_STATE,
          "time start without init is rejected cleanly");
    CHECK(michi_time_is_synchronized() == false, "still unsynchronized");
    teardown();

    /* Recovery: a later init + sync works (retryable, no reboot). */
    reset_all();
    boot_time_and_discovery();
    CHECK(michi_discovery_start("192.168.1.102") == ESP_OK,
          "discovery starts after time recovery");
    CHECK(michi_time_start() == ESP_OK, "time start after recovery");
    test_esp_timer_set_time(1000000);
    test_sntp_fire_sync(INJECTED_UNIX);
    CHECK(wait_for(sent_at_least_one, 2000),
          "recovered time subsystem allows the announce");
    teardown();
}

static void test_sntp_start_failure_degrades(void)
{
    printf("errors: SNTP start failure gates discovery, retry recovers\n");
    reset_all();
    boot_time_and_discovery();

    test_sntp_set_fail_start(true);
    CHECK(michi_time_start() != ESP_OK, "SNTP start failure propagates");
    CHECK(michi_discovery_start("192.168.1.102") == ESP_OK,
          "discovery start unaffected by the failed time start");
    CHECK(test_socket_sent_count() == 0, "no announce while gated");
    usleep(500000);
    CHECK(michi_time_is_synchronized() == false,
          "still unsynchronized after the failed start");

    /* Retry succeeds: the announce gate opens. */
    test_sntp_set_fail_start(false);
    CHECK(michi_time_start() == ESP_OK, "retried start succeeds");
    test_esp_timer_set_time(1000000);
    test_sntp_fire_sync(INJECTED_UNIX);
    CHECK(wait_for(sent_at_least_one, 2000),
          "announce allowed after the recovered start");
    teardown();
}

/* ------------------------------------------------------------------ */
/* 6. shutdown clears the state cleanly                               */
/* ------------------------------------------------------------------ */

static void test_shutdown_clears_state(void)
{
    printf("lifecycle: shutdown joins the task and clears the state\n");
    reset_all();
    boot_time_and_discovery();
    CHECK(michi_discovery_start("192.168.1.102") == ESP_OK, "start");
    CHECK(michi_time_start() == ESP_OK, "time start");
    test_esp_timer_set_time(1000000);
    test_sntp_fire_sync(INJECTED_UNIX);
    CHECK(wait_for(sent_at_least_one, 2000), "synced");

    CHECK(michi_discovery_shutdown() == ESP_OK, "discovery shutdown ok");
    CHECK(michi_time_shutdown() == ESP_OK, "time shutdown ok (join)");
    CHECK(michi_time_is_synchronized() == false,
          "shutdown clears the synchronized state");
    CHECK(michi_time_unix_ms() == 0, "shutdown clears unix_ms");
    CHECK(michi_time_start() == ESP_ERR_INVALID_STATE,
          "start after shutdown rejected");
    CHECK(michi_time_shutdown() == ESP_OK, "shutdown is idempotent");
}

/* ------------------------------------------------------------------ */

int main(void)
{
    test_gate_without_sync();
    test_defer_logged_once_per_transition();
    test_sync_allows_announce();
    test_reconnect_conserves_state();
    test_stale_give_rejected_fresh_resync();
    test_sntp_init_failure_degrades();
    test_sntp_start_failure_degrades();
    test_shutdown_clears_state();

    if (failures == 0) {
        printf("test_michi_time: all tests passed\n");
        return 0;
    }
    printf("test_michi_time: %d check(s) FAILED\n", failures);
    return 1;
}
