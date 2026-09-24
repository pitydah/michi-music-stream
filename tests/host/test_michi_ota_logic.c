/* Host-side pure-logic tests for michi_ota component.
 *
 * These tests intentionally do NOT compile michi_ota.c itself
 * (it pulls in esp_https_ota, esp_ota_ops, FreeRTOS, NVS, cJSON, mbedTLS,
 * etc. - none of which are available on the host). Instead, the firmware
 * logic that can be extracted as self-contained pure functions is
 * reimplemented verbatim (or close enough) here and tested directly.
 *
 * Three areas covered:
 *
 *   1. OTA-start gate  – the ota_spawn_task() guard is modelled as
 *      ota_gate_check(task_running, is_pending_verify). All four input
 *      combinations are exercised, with priority ordering verified.
 *
 *   2. NVS pending_version comparison – simulate loading a version string
 *      from NVS, parsing it with the real semver_parse(), comparing it
 *      to a "running" version with the real semver_cmp().  Covers cases
 *      not present in test_semver.c: the NVS value is NEWER, EQUAL, and
 *      OLDER than the running firmware, and a truncated NVS value edge-case.
 *
 *   3. Boot-time latch decision – the decision function that answers
 *      "should a new OTA of target_version be blocked because
 *      pending_version in NVS already covers it or is newer?" is modelled
 *      as latch_should_block(pending_version, running_version,
 *      target_version) and tested exhaustively.
 *
 * The REAL semver.c / semver.h from firmware/components/michi_ota are
 * linked (same as test_semver.c), so the parsing and comparison results
 * are production-identical.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "semver.h"

/* -------------------------------------------------------------------------
 * Minimal test framework (mirrors test_semver.c style)
 * ---------------------------------------------------------------------- */

static int failures = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            printf("  FAIL %s\n", msg);                                     \
            failures++;                                                     \
        }                                                                   \
    } while (0)

/* =========================================================================
 * 1. OTA-start gate (extracted from ota_spawn_task)
 *
 * The real firmware gate in ota_spawn_task() checks (in order):
 *   a) if an OTA task is already running -> ESP_ERR_INVALID_STATE ("busy")
 *   b) if the running partition is PENDING_VERIFY -> ESP_ERR_NOT_ALLOWED
 *   c) otherwise create the task (OK)
 *
 * We model this as a pure function requiring zero firmware headers.
 * ======================================================================= */

typedef enum {
    OTA_START_OK,
    OTA_START_BUSY,
    OTA_START_PENDING_VERIFY
} ota_start_gate_t;

/* Extracted verbatim from the task specification and matches the priority
 * ordering inside ota_spawn_task() (busy is checked first, PENDING_VERIFY
 * second). */
static ota_start_gate_t ota_gate_check(bool task_running, bool is_pending_verify)
{
    if (task_running)      return OTA_START_BUSY;
    if (is_pending_verify) return OTA_START_PENDING_VERIFY;
    return OTA_START_OK;
}

static void test_ota_gate(void)
{
    printf("ota_gate: all (task_running x is_pending_verify) combinations\n");

    /* (false, false) -> OK: no task running, partition is valid */
    CHECK(ota_gate_check(false, false) == OTA_START_OK,
          "(false,false) == OTA_START_OK");

    /* (false, true) -> PENDING_VERIFY: no task but image not yet validated */
    CHECK(ota_gate_check(false, true) == OTA_START_PENDING_VERIFY,
          "(false,true) == OTA_START_PENDING_VERIFY");

    /* (true, false) -> BUSY: task already running, partition healthy */
    CHECK(ota_gate_check(true, false) == OTA_START_BUSY,
          "(true,false) == OTA_START_BUSY");

    /* (true, true) -> BUSY: task wins over PENDING_VERIFY (priority order) */
    CHECK(ota_gate_check(true, true) == OTA_START_BUSY,
          "(true,true) == OTA_START_BUSY (busy takes priority over pending_verify)");

    /* Symmetry: a cleared task that also clears PENDING must give OK */
    ota_start_gate_t g = ota_gate_check(false, false);
    CHECK(g != OTA_START_BUSY && g != OTA_START_PENDING_VERIFY,
          "(false,false) is neither BUSY nor PENDING_VERIFY");
}

/* =========================================================================
 * 2. NVS pending_version comparison
 *
 * The firmware stores the pending version as a string (up to 15 chars +
 * NUL, key "pending_version" in NVS namespace "ota_local").  At boot-time
 * check time it is read back, parsed with semver_parse(), and compared
 * against the running firmware version with semver_cmp().
 *
 * We model the NVS read as simply providing a C string (simulated NVS
 * contents).  The real firmware then parses + compares; we do the same.
 *
 * Cases NOT present in test_semver.c:
 *   a) NVS pending > running  -> a newer OTA was staged
 *   b) NVS pending == running -> the running image IS the staged version
 *   c) NVS pending < running  -> stale/leftover latch from an older update
 *   d) NVS pending string truncated to 15 chars still parses as valid semver
 *   e) NVS pending string is corrupt / empty -> semver_parse fails gracefully
 * ======================================================================= */

/* Sentinel: semver_parse() failed for at least one of the two inputs. */
#define PARSE_FAILED (-999)

/* Simulates: read NVS string `nvs_ver`, parse it, compare to `running_ver`.
 * Returns semver_cmp result (<0, 0, >0), or PARSE_FAILED on parse failure. */
static int nvs_version_cmp_to_running(const char *nvs_ver,
                                      const char *running_ver)
{
    semver_t pend, run;
    if (!semver_parse(nvs_ver, &pend) || !semver_parse(running_ver, &run)) {
        return PARSE_FAILED;
    }
    return semver_cmp(&pend, &run);
}

static void test_nvs_pending_version_cmp(void)
{
    printf("nvs_pending_version: comparison against running firmware\n");
    int r;

    /* a) Pending is NEWER than running: cmp > 0 */
    r = nvs_version_cmp_to_running("1.3.0", "1.2.5");
    CHECK(r > 0, "1.3.0 pending > 1.2.5 running");

    /* a2) Pending is a pre-release of the next minor, still newer than old final */
    r = nvs_version_cmp_to_running("1.3.0-rc1", "1.2.5");
    CHECK(r > 0, "1.3.0-rc1 pending > 1.2.5 running");

    /* b) Pending EQUALS running (boot self-test: we are running the staged version) */
    r = nvs_version_cmp_to_running("1.2.5", "1.2.5");
    CHECK(r == 0, "1.2.5 pending == 1.2.5 running");

    /* b2) Both pre-release and equal */
    r = nvs_version_cmp_to_running("1.2.5-rc2", "1.2.5-rc2");
    CHECK(r == 0, "1.2.5-rc2 pending == 1.2.5-rc2 running");

    /* c) Pending is OLDER than running: stale latch */
    r = nvs_version_cmp_to_running("1.2.4", "1.2.5");
    CHECK(r < 0, "1.2.4 pending < 1.2.5 running (stale latch)");

    /* c2) Pre-release pending older than final running */
    r = nvs_version_cmp_to_running("1.2.5-beta", "1.2.5");
    CHECK(r < 0, "1.2.5-beta pending < 1.2.5 running (pre < final)");

    /* d) String of 14 chars (fits within NVS_VERSION_LEN=16) still parses.
     * "10.20.30-rc100" = 14 chars. */
    r = nvs_version_cmp_to_running("10.20.30-rc100", "10.20.29");
    CHECK(r > 0, "10.20.30-rc100 (14 chars, within NVS limit) > 10.20.29");

    /* d2) Exactly 15 chars: "10.20.30-rc1000" would be 15 chars; still valid.
     * rc1000 > rc100. */
    r = nvs_version_cmp_to_running("10.20.30-rc1000", "10.20.30-rc100");
    CHECK(r > 0, "10.20.30-rc1000 (15 chars) > 10.20.30-rc100");

    /* e) Corrupt NVS value -> parse fails -> PARSE_FAILED sentinel */
    r = nvs_version_cmp_to_running("", "1.2.5");
    CHECK(r == PARSE_FAILED, "empty pending string -> parse failure");

    r = nvs_version_cmp_to_running("not-a-semver", "1.2.5");
    CHECK(r == PARSE_FAILED, "garbage pending string -> parse failure");

    r = nvs_version_cmp_to_running("1.2.3+build", "1.2.3");
    CHECK(r == PARSE_FAILED, "build metadata not supported -> parse failure");

    /* e2) Running firmware version malformed */
    r = nvs_version_cmp_to_running("1.0.0", "bad");
    CHECK(r == PARSE_FAILED, "bad running version -> parse failure");

    /* e3) Both malformed */
    r = nvs_version_cmp_to_running("bad", "also-bad");
    CHECK(r == PARSE_FAILED, "both malformed -> parse failure");
}

/* =========================================================================
 * 3. Boot-time latch decision
 *
 * The logical question the latch must answer:
 *
 *   Given:
 *     pending_version  - the NVS-stored version that was being staged
 *     running_version  - the currently executing firmware version
 *     target_version   - the version the new OTA request wants to install
 *
 *   Should we BLOCK the new OTA because the latch already covers it?
 *
 * The firmware's intent (from the review F1 comments and code):
 *
 *   - The latch is set BEFORE a local update applies, cleared on success
 *     (mark_success) or after the rollback disable.
 *   - If pending_version > running: we were mid-update when we restarted.
 *     The update should proceed (or selftest handles it). The latch must NOT
 *     block a new OTA for the same target in this case.
 *   - If pending_version == running: the staged image is already running.
 *     Re-applying the same version must be blocked (idempotency guard).
 *   - If pending_version < running: stale latch; new OTA to higher version
 *     is allowed.
 *
 * Decision rule:
 *
 *   ALWAYS block when target <= running  (anti-downgrade).
 *   ADDITIONALLY block when:
 *     pending is parseable
 *     AND semver_cmp(pending, running) == 0   (already on the staged version)
 *     AND semver_cmp(target, pending) <= 0    (target is not newer than staged)
 *
 * ======================================================================= */

/* Returns true when the OTA start should be blocked. */
static bool latch_should_block(const char *pending_version,
                               const char *running_version,
                               const char *target_version)
{
    semver_t running, target;
    if (!semver_parse(running_version, &running) ||
        !semver_parse(target_version, &target)) {
        /* Unparseable version strings: be conservative and block. */
        return true;
    }

    /* Basic anti-downgrade: target must be strictly newer than running. */
    if (semver_cmp(&target, &running) <= 0) {
        return true;
    }

    /* Latch idempotency guard: if the latch records a version we are already
     * running (pending == running), and the target is not strictly newer than
     * that, block (we already have it). */
    semver_t pending;
    if (semver_parse(pending_version, &pending)) {
        if (semver_cmp(&pending, &running) == 0 &&
            semver_cmp(&target, &pending) <= 0) {
            return true;
        }
    }
    /* Unparseable pending: no latch active, basic rules are sufficient. */

    return false;
}

static void test_boot_latch_decision(void)
{
    printf("boot_latch: latch_should_block logic\n");

    /* --- Anti-downgrade cases (no pending latch needed to reject) --- */

    CHECK(latch_should_block("", "1.2.5", "1.2.5"),
          "target==running blocked (anti-downgrade)");

    CHECK(latch_should_block("", "1.2.5", "1.2.4"),
          "target < running blocked (downgrade)");

    /* Pre-release target < final running */
    CHECK(latch_should_block("", "1.2.5", "1.2.5-rc1"),
          "target(rc1) < running(final) blocked");

    CHECK(latch_should_block("", "1.2.5", "1.2.4-rc9"),
          "target(1.2.4-rc9) < running(1.2.5) blocked");

    /* --- Legitimate upgrade, no pending latch --- */

    CHECK(!latch_should_block("", "1.2.5", "1.2.6"),
          "patch upgrade allowed (no pending)");

    CHECK(!latch_should_block("", "1.2.5", "1.3.0"),
          "minor upgrade allowed (no pending)");

    CHECK(!latch_should_block("", "1.2.5", "2.0.0"),
          "major upgrade allowed (no pending)");

    CHECK(!latch_should_block("", "1.2.5-rc1", "1.2.5"),
          "rc1->final upgrade allowed (no pending)");

    CHECK(!latch_should_block("", "1.2.5-rc1", "1.2.5-rc2"),
          "rc1->rc2 upgrade allowed (no pending)");

    /* --- Latch set, pending > running (mid-update restart) --- */

    /* OTA to an even newer version is allowed */
    CHECK(!latch_should_block("1.3.0", "1.2.5", "1.3.1"),
          "pending(1.3.0)>running, target(1.3.1)>running -> allowed");

    /* OTA to the staged version itself: pending>running, so NOT same-version
     * guard (pending != running), so allowed. */
    CHECK(!latch_should_block("1.3.0", "1.2.5", "1.3.0"),
          "pending(1.3.0)>running, target==pending -> allowed (mid-update)");

    /* Downgrade relative to running still blocked even with pending latch */
    CHECK(latch_should_block("1.3.0", "1.2.5", "1.2.4"),
          "pending>running but target<running -> still blocked");

    /* target == running -> blocked by anti-downgrade even with higher pending */
    CHECK(latch_should_block("1.3.0", "1.2.5", "1.2.5"),
          "pending>running but target==running -> blocked");

    /* --- Latch set, pending == running (already booted the staged version) --- */

    CHECK(latch_should_block("1.2.5", "1.2.5", "1.2.5"),
          "pending==running, target==running -> blocked (idempotency)");

    CHECK(latch_should_block("1.2.5", "1.2.5", "1.2.4"),
          "pending==running, target<running -> blocked");

    /* target > running -> allowed regardless of pending==running */
    CHECK(!latch_should_block("1.2.5", "1.2.5", "1.2.6"),
          "pending==running, target>running -> allowed");

    CHECK(!latch_should_block("1.2.5", "1.2.5", "1.3.0"),
          "pending==running, target(1.3.0)>running -> allowed");

    /* --- Latch set, pending < running (stale latch from an older update) --- */

    CHECK(!latch_should_block("1.2.3", "1.2.5", "1.2.6"),
          "pending(1.2.3)<running, target(1.2.6)>running -> allowed");

    CHECK(latch_should_block("1.2.3", "1.2.5", "1.2.5"),
          "pending<running, target==running -> blocked (anti-downgrade)");

    CHECK(latch_should_block("1.2.3", "1.2.5", "1.2.4"),
          "pending<running, target<running -> blocked");

    /* --- Pre-release latch cases --- */

    /* pending==running==rc1, target=rc2 -> allowed */
    CHECK(!latch_should_block("1.2.5-rc1", "1.2.5-rc1", "1.2.5-rc2"),
          "pending==running(rc1), target(rc2)>running -> allowed");

    /* pending==running==rc1, re-apply rc1 -> blocked */
    CHECK(latch_should_block("1.2.5-rc1", "1.2.5-rc1", "1.2.5-rc1"),
          "pending==running(rc1), target==rc1 -> blocked (idempotency)");

    /* pending==running==rc1, target=final -> allowed (rc1->final upgrade) */
    CHECK(!latch_should_block("1.2.5-rc1", "1.2.5-rc1", "1.2.5"),
          "pending==running(rc1), target=final -> allowed");

    /* pending=rc1, running=final; target=rc2 < running(final) -> blocked */
    CHECK(latch_should_block("1.2.5-rc1", "1.2.5", "1.2.5-rc2"),
          "pending=rc1, running=final, target=rc2 < running -> blocked");

    /* pending=rc1, running=final; target=1.2.6 -> allowed */
    CHECK(!latch_should_block("1.2.5-rc1", "1.2.5", "1.2.6"),
          "pending=rc1, running=final, target(1.2.6) > running -> allowed");

    /* --- Unparseable pending string (latch treated as absent) --- */

    CHECK(!latch_should_block("not-a-version", "1.2.5", "1.2.6"),
          "corrupt pending ignored, target>running -> allowed");

    CHECK(latch_should_block("not-a-version", "1.2.5", "1.2.4"),
          "corrupt pending ignored, target<running -> blocked");

    CHECK(latch_should_block("not-a-version", "1.2.5", "1.2.5"),
          "corrupt pending ignored, target==running -> blocked");

    /* build metadata in pending (rejected by semver_parse) */
    CHECK(!latch_should_block("1.2.5+meta", "1.2.5", "1.2.6"),
          "pending with +meta ignored, target>running -> allowed");
}

/* =========================================================================
 * 4. Trial Boot Audio Health Gate (OTA-01..OTA-03)
 *
 * If a SKU expects audio (expected_audio == true) but audio is not available
 * (audio_available == false), the trial boot self-test MUST evaluate to
 * FATAL to prevent cancelling rollback and avoid stranding the device on
 * broken firmware.
 *
 * OTA-01: expected_audio=true, audio_available=true -> PASS (rollback cancelled)
 * OTA-02: expected_audio=true, audio_available=false -> FATAL (triggers rollback)
 * OTA-03: expected_audio=false, audio_available=false -> DEGRADED (diagnostic SKU allowed)
 * ======================================================================= */

typedef enum {
    OTA_TEST_SELFTEST_PASS,
    OTA_TEST_SELFTEST_DEGRADED,
    OTA_TEST_SELFTEST_FATAL
} ota_test_selftest_res_t;

static ota_test_selftest_res_t evaluate_trial_boot_gate(bool critical_checks_ok,
                                                        bool expected_audio,
                                                        bool audio_available)
{
    if (!critical_checks_ok) {
        return OTA_TEST_SELFTEST_FATAL;
    }
    if (!audio_available) {
        if (expected_audio) {
            return OTA_TEST_SELFTEST_FATAL;
        }
        return OTA_TEST_SELFTEST_DEGRADED;
    }
    return OTA_TEST_SELFTEST_PASS;
}

static void test_trial_boot_audio_health_gate(void)
{
    printf("trial_boot_gate: OTA audio health evaluation (OTA-01..OTA-03)\n");

    /* OTA-01: Audio SKU with working audio passes trial boot gate */
    CHECK(evaluate_trial_boot_gate(true, true, true) == OTA_TEST_SELFTEST_PASS,
          "OTA-01: expected_audio=true && audio_available=true -> PASS");

    /* OTA-02: Audio SKU with broken/missing audio evaluates to FATAL (triggers rollback) */
    CHECK(evaluate_trial_boot_gate(true, true, false) == OTA_TEST_SELFTEST_FATAL,
          "OTA-02: expected_audio=true && audio_available=false -> FATAL (triggers rollback)");

    /* OTA-03: Diagnostic SKU (expected_audio=false) with audio_available=false accepts DEGRADED */
    CHECK(evaluate_trial_boot_gate(true, false, false) == OTA_TEST_SELFTEST_DEGRADED,
          "OTA-03: expected_audio=false && audio_available=false -> DEGRADED");

    /* Critical check failure always results in FATAL regardless of audio */
    CHECK(evaluate_trial_boot_gate(false, true, true) == OTA_TEST_SELFTEST_FATAL,
          "critical failure -> FATAL");
}

/* =========================================================================
 * main
 * ======================================================================= */

int main(void)
{
    test_ota_gate();
    test_nvs_pending_version_cmp();
    test_boot_latch_decision();
    test_trial_boot_audio_health_gate();

    if (failures == 0) {
        printf("PASS test_michi_ota_logic\n");
        return 0;
    }
    printf("FAIL test_michi_ota_logic (%d)\n", failures);
    return 1;
}
