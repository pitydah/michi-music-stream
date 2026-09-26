/* Host-side unit tests for RFC 3550 interarrival jitter and clock offset tracking.
 *
 * Compiles the REAL header firmware/components/michi_audio/michi_rtp_jitter.h.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>

#include "michi_rtp_jitter.h"

static int failures = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            printf("  FAIL %s\n", msg);                                     \
            failures++;                                                     \
        }                                                                   \
    } while (0)

static void test_initial_and_perfect_transit(void)
{
    printf("rtp jitter: initial seed and perfect transit\n");
    michi_rtp_jitter_t j;
    michi_rtp_jitter_reset(&j);
    CHECK(!j.seeded, "not seeded initially");

    /* First packet arrives at t=1,000,000 us, RTP ts=0 */
    uint32_t jit = michi_rtp_jitter_feed(&j, 1000000LL, 0, 48000);
    CHECK(j.seeded, "seeded after first packet");
    CHECK(jit == 0, "jitter is 0 on first packet");
    CHECK(j.jitter_us == 0, "jitter_us field is 0");

    /* Packet 2 arrives exactly 10ms (10,000 us) later with 480 samples advance */
    /* 480 samples at 48000 Hz = 10,000 us */
    jit = michi_rtp_jitter_feed(&j, 1010000LL, 480, 48000);
    CHECK(jit == 0, "jitter is 0 on perfect transit");

    /* Packet 3 arrives another 10ms later with another 480 samples */
    jit = michi_rtp_jitter_feed(&j, 1020000LL, 960, 48000);
    CHECK(jit == 0, "jitter remains 0");
}

static void test_transit_variation_ewma(void)
{
    printf("rtp jitter: transit variation and EWMA filter\n");
    michi_rtp_jitter_t j;
    michi_rtp_jitter_reset(&j);

    /* Seed at t=0, ts=0 */
    michi_rtp_jitter_feed(&j, 0LL, 0, 48000);

    /* Packet 2: expected arrival at 10,000 us (480 samples), but arrives at 15,000 us (5,000 us late) */
    /* D = (15000 - 0) - (480 * 1000000 / 48000) = 15000 - 10000 = 5000 us */
    /* EWMA J = (0 * 15 + 5000) / 16 = 312 */
    uint32_t jit = michi_rtp_jitter_feed(&j, 15000LL, 480, 48000);
    CHECK(jit == 312, "EWMA first sample: 5000 / 16 = 312");

    /* Packet 3: arrives early! expected 10,000 us transit from packet 2, but arrives 7,000 us later */
    /* d_arr = 22000 - 15000 = 7000 us */
    /* d_ts = 480 * 1000000 / 48000 = 10000 us */
    /* D = 7000 - 10000 = -3000 us -> |D| = 3000 us */
    /* EWMA J = (312 * 15 + 3000) / 16 = (4680 + 3000) / 16 = 7680 / 16 = 480 */
    jit = michi_rtp_jitter_feed(&j, 22000LL, 960, 48000);
    CHECK(jit == 480, "EWMA early arrival: |D|=3000 correctly folded into EWMA");
}

static void test_clamp_and_out_of_order(void)
{
    printf("rtp jitter: clamping and out-of-order arrival\n");
    michi_rtp_jitter_t j;
    michi_rtp_jitter_reset(&j);

    michi_rtp_jitter_feed(&j, 0LL, 0, 48000);

    /* Huge network spike: 2 seconds delay for a 10ms packet */
    /* D = 2000000 - 10000 = 1990000 us -> clamped to MICHI_AUDIO_JITTER_SAMPLE_CLAMP_US (1000000 us) */
    /* EWMA J = (0 + 1000000) / 16 = 62500 */
    uint32_t jit = michi_rtp_jitter_feed(&j, 2000000LL, 480, 48000);
    CHECK(jit == 62500, "huge delay clamped to 1000000 us ceiling");

    /* Out-of-order packet: timestamp is smaller than previous packet */
    /* Suppose packet with ts=240 arrives 5ms later (t=2,005,000 us) */
    /* d_arr = 5000 us */
    /* d_ts = (int32_t)(240 - 480) = -240 samples = -5000 us */
    /* D = 5000 - (-5000) = 10000 us */
    /* EWMA J = (62500 * 15 + 10000) / 16 = (937500 + 10000) / 16 = 947500 / 16 = 59218 */
    jit = michi_rtp_jitter_feed(&j, 2005000LL, 240, 48000);
    CHECK(jit == 59218, "out of order packet signed difference handled correctly");
}

static void test_clock_offset(void)
{
    printf("rtp jitter: cumulative clock offset tracking\n");
    int64_t base_arr = 10000000LL;
    uint32_t sample_rate = 48000;

    /* 1. Exact match */
    /* 48000 samples = 1,000,000 us */
    int32_t offset = michi_rtp_clock_offset_us(11000000LL, base_arr, 48000, sample_rate);
    CHECK(offset == 0, "exact match has 0 offset");

    /* 2. Receiver ahead (+150 us drift) */
    offset = michi_rtp_clock_offset_us(11000150LL, base_arr, 48000, sample_rate);
    CHECK(offset == 150, "positive drift +150 us");

    /* 3. Receiver behind (-200 us drift) */
    offset = michi_rtp_clock_offset_us(10999800LL, base_arr, 48000, sample_rate);
    CHECK(offset == -200, "negative drift -200 us");

    /* 4. Clamping bounds */
    int64_t huge_ahead = base_arr + 1000000LL + 3000000000LL; /* > INT32_MAX */
    offset = michi_rtp_clock_offset_us(huge_ahead, base_arr, 48000, sample_rate);
    CHECK(offset == INT32_MAX, "clamped to INT32_MAX");

    int64_t huge_behind = base_arr + 1000000LL - 3000000000LL; /* < INT32_MIN */
    offset = michi_rtp_clock_offset_us(huge_behind, base_arr, 48000, sample_rate);
    CHECK(offset == INT32_MIN, "clamped to INT32_MIN");
}

int main(void)
{
    test_initial_and_perfect_transit();
    test_transit_variation_ewma();
    test_clamp_and_out_of_order();
    test_clock_offset();

    if (failures != 0) {
        printf("rtp_jitter: %d FAILURE(S)\n", failures);
        return 1;
    }
    printf("rtp_jitter: all tests passed\n");
    return 0;
}
