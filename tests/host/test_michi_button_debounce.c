/* Host-side tests for the PURE debouncer (PAIR-BTN-01).
 *
 * Compiles ONLY michi_button_debounce.c - no GPIO, no FreeRTOS, no timer.
 * Proves the single-authority contract:
 *   - a clean tap yields exactly one PRESS + one RELEASE (no loss, no double),
 *   - press-side bounce collapses to a single PRESS,
 *   - release-side bounce (the P0 failure mode) collapses to a single
 *     RELEASE and therefore a single action,
 *   - a sub-debounce glitch yields nothing,
 *   - boundary durations track the physical press faithfully (offsets cancel),
 *   - 1000 randomized stress runs never produce a duplicate/lost action,
 *   - seed level is respected and feed() is NULL-safe.
 *
 * Samples are fed at a 5 ms poll cadence to match MICHI_BUTTON_POLL_MS.
 */

#include <stdio.h>
#include <string.h>

#include "michi_button_debounce.h"

static int failures = 0;
#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            printf("  FAIL %s\n", msg);                                     \
            failures++;                                                     \
        }                                                                   \
    } while (0)

#define POLL_US 5000LL
#define DB_DEBOUNCE_MS 30u

static michi_button_debounce_evt_t run_level(michi_button_debounce_t *d,
                                             int level, int samples,
                                             int64_t *start_us)
{
    michi_button_debounce_evt_t last = MICHI_BTN_DEBOUNCE_NONE;
    for (int i = 0; i < samples; i++) {
        last = michi_button_debounce_feed(d, level, *start_us);
        *start_us += POLL_US;
    }
    return last;
}

/* Feed a press of exactly `ms` milliseconds then release; return the
 * measured stable-to-stable duration in ms (0 if a transition was lost). */
static uint32_t measured_duration_ms(uint32_t debounce_ms, uint32_t ms)
{
    michi_button_debounce_t d;
    michi_button_debounce_init(&d, debounce_ms);
    int64_t t = 0;
    int64_t press_t = 0, release_t = 0;
    for (int i = 0; i < 6; i++) { michi_button_debounce_feed(&d, 1, t); t += POLL_US; }
    int low_samples = (int)(ms / 5);
    if (low_samples < 1) low_samples = 1;
    for (int i = 0; i < low_samples; i++) {
        michi_button_debounce_evt_t e = michi_button_debounce_feed(&d, 0, t);
        t += POLL_US;
        if (e == MICHI_BTN_DEBOUNCE_PRESS) press_t = t;
    }
    for (int i = 0; i < 8; i++) {
        michi_button_debounce_evt_t e = michi_button_debounce_feed(&d, 1, t);
        t += POLL_US;
        if (e == MICHI_BTN_DEBOUNCE_RELEASE) release_t = t;
    }
    if (press_t == 0 || release_t == 0) return 0;
    return (uint32_t)((release_t - press_t) / 1000);
}

/* ---------------------------------------------------------------------- */
/* 31. CLEAN TAP -> exactly one PRESS + one RELEASE.                     */
/* ---------------------------------------------------------------------- */
static void test_clean_tap(void)
{
    printf("debounce: clean tap -> one PRESS + one RELEASE\n");
    michi_button_debounce_t d;
    michi_button_debounce_init(&d, DB_DEBOUNCE_MS);
    int64_t t = 1000;
    int presses = 0, releases = 0;
    michi_button_debounce_evt_t last;

    run_level(&d, 1, 6, &t);
    for (int i = 0; i < 60; i++) {
        last = michi_button_debounce_feed(&d, 0, t); t += POLL_US;
        if (last == MICHI_BTN_DEBOUNCE_PRESS) presses++;
    }
    for (int i = 0; i < 60; i++) {
        last = michi_button_debounce_feed(&d, 1, t); t += POLL_US;
        if (last == MICHI_BTN_DEBOUNCE_RELEASE) releases++;
    }
    CHECK(presses == 1, "clean tap: exactly one PRESS");
    CHECK(releases == 1, "clean tap: exactly one RELEASE");
    CHECK(d.stable_presses == 1, "clean tap: counter stable_presses == 1");
    CHECK(d.stable_releases == 1, "clean tap: counter stable_releases == 1");
}

/* ---------------------------------------------------------------------- */
/* 32. PRESS BOUNCE collapses to a single PRESS.                          */
/* ---------------------------------------------------------------------- */
static void test_press_bounce(void)
{
    printf("debounce: press-side bounce collapses to one PRESS\n");
    michi_button_debounce_t d;
    michi_button_debounce_init(&d, DB_DEBOUNCE_MS);
    int64_t t = 1000;
    int presses = 0;

    run_level(&d, 1, 6, &t);
    michi_button_debounce_feed(&d, 0, t); t += POLL_US;
    michi_button_debounce_feed(&d, 1, t); t += POLL_US;  /* revert */
    michi_button_debounce_feed(&d, 0, t); t += POLL_US;
    michi_button_debounce_feed(&d, 1, t); t += POLL_US;  /* revert */
    for (int i = 0; i < 8; i++) {
        michi_button_debounce_evt_t e = michi_button_debounce_feed(&d, 0, t);
        t += POLL_US;
        if (e == MICHI_BTN_DEBOUNCE_PRESS) presses++;
    }
    CHECK(presses == 1, "press bounce: exactly one PRESS");
}

/* ---------------------------------------------------------------------- */
/* 33. RELEASE BOUNCE (the P0 failure mode) -> one RELEASE, no abort.     */
/* ---------------------------------------------------------------------- */
static void test_release_bounce(void)
{
    printf("debounce: release-side bounce -> one RELEASE (no abort)\n");
    michi_button_debounce_t d;
    michi_button_debounce_init(&d, DB_DEBOUNCE_MS);
    int64_t t = 1000;
    int releases = 0;

    run_level(&d, 1, 6, &t);
    run_level(&d, 0, 8, &t);  /* confirm PRESS */
    for (int i = 0; i < 4; i++) {
        michi_button_debounce_feed(&d, 1, t); t += POLL_US;  /* H candidate */
        michi_button_debounce_feed(&d, 0, t); t += POLL_US;  /* L revert */
    }
    for (int i = 0; i < 8; i++) {
        michi_button_debounce_evt_t e = michi_button_debounce_feed(&d, 1, t);
        t += POLL_US;
        if (e == MICHI_BTN_DEBOUNCE_RELEASE) releases++;
    }
    CHECK(releases == 1, "release bounce: exactly one RELEASE (old code lost it)");
    CHECK(d.stable_releases == 1, "release bounce: counter == 1");
}

/* ---------------------------------------------------------------------- */
/* 34. GLITCH: a 5 ms LOW dip during idle => nothing.                     */
/* ---------------------------------------------------------------------- */
static void test_glitch(void)
{
    printf("debounce: 5 ms glitch rejected (debounce 30 ms)\n");
    michi_button_debounce_t d;
    michi_button_debounce_init(&d, DB_DEBOUNCE_MS);
    int64_t t = 1000;
    int spurious = 0;

    run_level(&d, 1, 6, &t);
    michi_button_debounce_feed(&d, 0, t); t += POLL_US;
    for (int i = 0; i < 8; i++) {
        michi_button_debounce_evt_t e = michi_button_debounce_feed(&d, 1, t);
        t += POLL_US;
        if (e != MICHI_BTN_DEBOUNCE_NONE) spurious++;
    }
    CHECK(spurious == 0, "glitch: no spurious PRESS/RELEASE");
}

/* ---------------------------------------------------------------------- */
/* 36. BOUNDARY: measured duration tracks physical press (offsets cancel).*/
/* ---------------------------------------------------------------------- */
static void test_boundaries(void)
{
    printf("debounce: measured duration tracks physical press (offsets cancel)\n");
    /* The debouncer confirms each edge after a full DEBOUNCE window, so both
     * the PRESS and the RELEASE fire `debounce_ms` after their true edge.
     * Those offsets are equal and cancel, so the measured (confirmed)
     * duration must equal the PHYSICAL press duration exactly. At the test's
     * 5 ms sample granularity the physical durations are low_samples*5. */
    uint32_t low49 = (49 / 5) * 5;   /* 9 samples -> 45 ms (granularity) */
    uint32_t low50 = (50 / 5) * 5;   /* 10 samples -> 50 ms */
    uint32_t d49 = measured_duration_ms(DB_DEBOUNCE_MS, 49);
    uint32_t d50 = measured_duration_ms(DB_DEBOUNCE_MS, 50);
    CHECK(d49 == low49, "49 ms physical -> measured == physical (offsets cancel)");
    CHECK(d50 == low50, "50 ms physical -> measured == physical (offsets cancel)");
    CHECK(d49 < d50, "45 ms < 50 ms (MIN_PRESS line distinguishes the bands)");
}

/* ---------------------------------------------------------------------- */
/* 39. STRESS: 1000 randomized short taps -> one PRESS+RELEASE each.      */
/* ---------------------------------------------------------------------- */
static void test_stress(void)
{
    printf("debounce: 1000 randomized stress taps -> exactly one PRESS+RELEASE each\n");
    uint32_t seed = 0x9E3779B9u;
    int stress_failures = 0;
    /* Tiny deterministic LCG (no stdlib rand needed; reproducible). */
    for (int i = 0; i < 1000; i++) {
        michi_button_debounce_t d;
        michi_button_debounce_init(&d, DB_DEBOUNCE_MS);
        int64_t t = 1000;

        for (int k = 0; k < 6; k++) { michi_button_debounce_feed(&d, 1, t); t += POLL_US; }

        seed = seed * 1664525u + 1013904223u;
        uint32_t dur_ms = 80 + (seed % 721);
        seed = seed * 1664525u + 1013904223u;
        int relbounce = (int)(seed % 5);

        int low_samples = (int)(dur_ms / 5);
        int presses = 0, releases = 0;

        seed = seed * 1664525u + 1013904223u;
        int pressbounce = (int)(seed % 3);
        for (int b = 0; b < pressbounce; b++) {
            michi_button_debounce_feed(&d, 0, t); t += POLL_US;
            michi_button_debounce_feed(&d, 1, t); t += POLL_US;
        }
        for (int k = 0; k < low_samples; k++) {
            michi_button_debounce_evt_t e = michi_button_debounce_feed(&d, 0, t);
            t += POLL_US;
            if (e == MICHI_BTN_DEBOUNCE_PRESS) presses++;
        }
        for (int b = 0; b < relbounce; b++) {
            michi_button_debounce_feed(&d, 1, t); t += POLL_US;
            michi_button_debounce_feed(&d, 0, t); t += POLL_US;
        }
        for (int k = 0; k < 8; k++) {
            michi_button_debounce_evt_t e = michi_button_debounce_feed(&d, 1, t);
            t += POLL_US;
            if (e == MICHI_BTN_DEBOUNCE_RELEASE) releases++;
        }
        if (presses != 1 || releases != 1) {
            stress_failures++;
            printf("  FAIL stress[%d]: dur=%ums pb=%d rb=%d -> presses=%d releases=%d\n",
                   i, dur_ms, pressbounce, relbounce, presses, releases);
        }
    }
    /* Fold the per-tap result into the global failures exactly once. We do
     * NOT also CHECK() here: CHECK increments `failures` on mismatch, so a
     * second +1 would double-count and inflate the exit tally (R3 noted this
     * class of bug). The local counter already carries the per-tap signal. */
    if (stress_failures == 0) {
        printf("  ok stress: all 1000 taps produced one PRESS+RELEASE\n");
    } else {
        failures += stress_failures;
        printf("  FAIL stress: %d taps did not produce one PRESS+RELEASE\n",
               stress_failures);
    }
}

/* ---------------------------------------------------------------------- */
/* Seed level respected; NULL-safe.                                       */
/* ---------------------------------------------------------------------- */
static void test_seed_and_null(void)
{
    printf("debounce: seed stable_level + NULL safety\n");
    michi_button_debounce_t d;
    michi_button_debounce_init(&d, DB_DEBOUNCE_MS);
    d.stable_level = 0;
    d.candidate_level = 0;
    int64_t t = 0;
    int presses = 0;
    for (int i = 0; i < 8; i++) {
        michi_button_debounce_evt_t e = michi_button_debounce_feed(&d, 1, t);
        t += POLL_US;
        if (e == MICHI_BTN_DEBOUNCE_PRESS) presses++;
    }
    CHECK(presses == 0, "seeded-low: no spurious PRESS on first release");
    CHECK(michi_button_debounce_feed(NULL, 1, t) == MICHI_BTN_DEBOUNCE_NONE,
          "feed(NULL,...) == NONE");
    michi_button_debounce_init(NULL, DB_DEBOUNCE_MS);
}

int main(void)
{
    test_clean_tap();
    test_press_bounce();
    test_release_bounce();
    test_glitch();
    test_boundaries();
    test_stress();
    test_seed_and_null();

    if (failures == 0) {
        printf("debounce: ALL TESTS PASSED\n");
        return 0;
    }
    printf("debounce: %d FAILURE(S)\n", failures);
    return 1;
}
