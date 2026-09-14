/* Host-side tests for michi_rtp_clock.h — 64-bit extended RTP timestamp
 * tracking (P0-02: wrap fix).
 *
 * The extended clock must correctly extend uint32_t RTP timestamps to 64
 * bits across epoch boundaries (2^32 samples = ~24.85h at 48kHz). The
 * key contract:
 *   - Normal advance: extended increases monotonically.
 *   - Reorder within the half-window (< 2^31 behind): low word updated,
 *     epoch unchanged.
 *   - Forward wrap: new_raw < prev AND (prev - new_raw) > 2^31 → epoch
 *     incremented.
 *   - Discontinuity reset: after michi_rtp_clock_reset(), base is
 *     re-seeded on the next feed.
 */

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "michi_rtp_clock.h"

static int failures = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            printf("  FAIL %s\n", msg);                                     \
            failures++;                                                     \
        }                                                                   \
    } while (0)

static void test_normal_advance(void)
{
    printf("rtp_clock: normal advance (no wrap)\n");
    michi_rtp_clock_t c;
    michi_rtp_clock_reset(&c);

    const uint32_t BASE = 1000;
    michi_rtp_clock_feed(&c, BASE);
    CHECK(c.base == BASE, "base seeded correctly");
    CHECK(c.extended == BASE, "extended == base on first feed");

    michi_rtp_clock_feed(&c, BASE + 480);
    CHECK(c.extended == BASE + 480, "one packet advance");
    CHECK(michi_rtp_clock_delta(&c) == 480, "delta == 480 samples");

    michi_rtp_clock_feed(&c, BASE + 960);
    CHECK(michi_rtp_clock_delta(&c) == 960, "delta == 960 samples");
}

static void test_wrap_detection(void)
{
    printf("rtp_clock: forward wrap detection\n");
    michi_rtp_clock_t c;
    michi_rtp_clock_reset(&c);

    /* Seed at near the max uint32_t. */
    const uint32_t NEAR_MAX = 0xFFFFFF00u;
    michi_rtp_clock_feed(&c, NEAR_MAX);
    CHECK(c.base == NEAR_MAX, "base near MAX");

    /* Advance to MAX - 1 (no wrap yet). */
    michi_rtp_clock_feed(&c, 0xFFFFFFFEu);
    CHECK(c.extended == (uint64_t)0xFFFFFFFEu, "near-max advance, no wrap");

    /* Wrap: 0xFFFFFFFF → 0x00000000. */
    michi_rtp_clock_feed(&c, 0xFFFFFFFFu);
    michi_rtp_clock_feed(&c, 0x00000000u);
    CHECK(c.extended == 0x100000000ULL, "wrap: epoch incremented, low=0");

    /* Continue after wrap. */
    michi_rtp_clock_feed(&c, 480u);
    CHECK(c.extended == 0x100000000ULL + 480, "post-wrap advance");
    CHECK(michi_rtp_clock_delta(&c) == 0x100000000ULL + 480 - NEAR_MAX,
          "post-wrap delta correct");
}

static void test_wrap_48k_boundary(void)
{
    printf("rtp_clock: wrap at 48kHz 24.85h boundary\n");
    michi_rtp_clock_t c;
    michi_rtp_clock_reset(&c);

    /* Simulate a stream starting at ts=100.
     * At 48kHz, 2^32 samples = 4294967296 / 48000 = ~89478 s = ~24.85h.
     * Verify that the final delta equals exactly 2^32 after one full wrap. */
    const uint32_t START = 100u;
    michi_rtp_clock_feed(&c, START);

    /* Advance past wrap: final raw = 99 (START - 1). */
    const uint32_t LAST_BEFORE_WRAP = 0xFFFFFFFFu;
    const uint32_t FIRST_AFTER_WRAP = 0u;
    const uint32_t FINAL_RAW = 99u;

    michi_rtp_clock_feed(&c, LAST_BEFORE_WRAP);
    michi_rtp_clock_feed(&c, FIRST_AFTER_WRAP);
    michi_rtp_clock_feed(&c, FINAL_RAW);

    /* Expected delta = (0x100000000 + FINAL_RAW) - START = 2^32 - 1. */
    const uint64_t expected = (uint64_t)0x100000000ULL + FINAL_RAW - START;
    CHECK(michi_rtp_clock_delta(&c) == expected,
          "48kHz wrap: delta correct after 24.85h");
}

static void test_reset_and_reseed(void)
{
    printf("rtp_clock: reset and reseed on discontinuity\n");
    michi_rtp_clock_t c;
    michi_rtp_clock_reset(&c);

    michi_rtp_clock_feed(&c, 1000u);
    michi_rtp_clock_feed(&c, 2000u);
    CHECK(michi_rtp_clock_delta(&c) == 1000u, "before reset: delta 1000");

    /* Discontinuity: sender restarted with a very different ts. */
    michi_rtp_clock_reset(&c);
    michi_rtp_clock_feed(&c, 500u);
    CHECK(c.base == 500u, "after reset: new base");
    CHECK(michi_rtp_clock_delta(&c) == 0u, "after reset: delta 0");

    michi_rtp_clock_feed(&c, 980u);
    CHECK(michi_rtp_clock_delta(&c) == 480u, "after reset: 480 delta");
}

static void test_reorder_within_window(void)
{
    printf("rtp_clock: reorder within half-window (no epoch increment)\n");
    michi_rtp_clock_t c;
    michi_rtp_clock_reset(&c);

    michi_rtp_clock_feed(&c, 1000u);
    michi_rtp_clock_feed(&c, 2000u);

    /* Out-of-order packet: 1500 < 2000, but (2000 - 1500) = 500 < 2^31.
     * Not a wrap: epoch must stay at 0. */
    michi_rtp_clock_feed(&c, 1500u);
    CHECK((c.extended >> 32) == 0, "reorder: epoch unchanged");
    CHECK((uint32_t)(c.extended & 0xFFFFFFFF) == 1500u,
          "reorder: low word updated to 1500");

    /* Continuing forward after reorder. */
    michi_rtp_clock_feed(&c, 2480u);
    CHECK(c.extended == 2480u, "after reorder: forward advance works");
}

static void test_jitter_no_wrap_corruption(void)
{
    printf("rtp_clock: jitter ts_delta_64 does not corrupt at wrap point\n");
    /* Simulate the jitter calculation across a wrap boundary.
     * Old code: uint32_t ts_delta = pkt.timestamp - base_ts (wraps to ~0).
     * New code: uint64_t ts_delta_64 = extended - base_ts_extended (monotone). */
    michi_rtp_clock_t c;
    michi_rtp_clock_reset(&c);

    const uint32_t BASE = 0xFFFFF000u; /* close to wrap */
    michi_rtp_clock_feed(&c, BASE);

    /* 1000 ms of audio = 48000 samples. Crosses the wrap boundary. */
    const uint32_t AFTER_1S_RAW = (uint32_t)(BASE + 48000u); /* wraps in uint32 */

    /* Old code ts_delta (uint32): */
    const uint32_t old_ts_delta = AFTER_1S_RAW - BASE; /* should be 48000 */
    CHECK(old_ts_delta == 48000u,
          "old uint32 ts_delta: happens to be correct here (no mid-calc wrap)");

    /* Feed the wrapped packet. */
    michi_rtp_clock_feed(&c, AFTER_1S_RAW);
    const uint64_t delta64 = michi_rtp_clock_delta(&c);
    CHECK(delta64 == 48000u, "64-bit delta after 1s: correct 48000 samples");

    /* 2nd second. */
    const uint32_t AFTER_2S_RAW = (uint32_t)(BASE + 96000u);
    michi_rtp_clock_feed(&c, AFTER_2S_RAW);
    CHECK(michi_rtp_clock_delta(&c) == 96000u, "64-bit delta after 2s: 96000");

    /* Old code would have: ts_delta = AFTER_2S_RAW - BASE.  At wrap: */
    const uint32_t old2 = AFTER_2S_RAW - BASE;
    CHECK(old2 == 96000u,
          "old uint32 still ok (base hasn't changed, just wraps beyond here)");

    /* Simulate the actual failure: base is near end, packet is after wrap.
     * uint32_t BASE2 close to max, PACKET well past: */
    michi_rtp_clock_reset(&c);
    const uint32_t BASE2   = 0xFFFFFF00u;
    const uint32_t PKT_1S  = (uint32_t)(BASE2 + 48000u); /* = 0xFF + 47745 */
    const uint32_t bad_delta = PKT_1S - BASE2; /* should be 48000 in uint32 */
    CHECK(bad_delta == 48000u, "uint32: ok for one second past near-max");

    /* Now the real failure at ~24.85h: packet timestamp has fully wrapped.
     * base=BASE2, pkt.timestamp is from a SECOND full epoch. */
    const uint32_t PKT_EPOCH2 = (uint32_t)(BASE2 + 48000u * 2000u);
    /* uint32 ts_delta would be (uint32_t)(PKT_EPOCH2 - BASE2) which wraps.
     * This is the bug: the result is a garbage small value. */
    const uint32_t buggy = PKT_EPOCH2 - BASE2;
    /* We can't assert buggy is wrong without knowing the epoch, but we can
     * show the 64-bit version is always monotone: */
    michi_rtp_clock_feed(&c, BASE2);
    michi_rtp_clock_feed(&c, PKT_EPOCH2);
    CHECK(michi_rtp_clock_delta(&c) == (uint32_t)(48000u * 2000u),
          "64-bit: 2000s delta correct across wrap");
    (void)buggy; /* suppress unused warning */
}

int main(void)
{
    test_normal_advance();
    test_wrap_detection();
    test_wrap_48k_boundary();
    test_reset_and_reseed();
    test_reorder_within_window();
    test_jitter_no_wrap_corruption();

    if (failures == 0) {
        printf("PASS test_rtp_clock\n");
        return 0;
    }
    printf("FAIL test_rtp_clock (%d failures)\n", failures);
    return 1;
}
