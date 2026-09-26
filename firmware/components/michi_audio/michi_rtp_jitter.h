/* michi_rtp_jitter.h — RFC 3550 transit-difference interarrival jitter filter
 * and cumulative clock drift tracker.
 *
 * RFC 3550 Section 6.4.1 / A.8:
 * For consecutive packet arrivals (i-1, i), the transit difference D is:
 *   D(i-1, i) = (arrival_i - arrival_{i-1}) - (timestamp_i - timestamp_{i-1})
 * and the interarrival jitter estimate is updated as:
 *   J = (15 * J + |D|) / 16
 *
 * Clock offset (cumulative crystal / sample-rate drift):
 *   offset = arrival_i - (base_arrival + (extended_ts - base_ts) / sample_rate)
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef MICHI_AUDIO_JITTER_SAMPLE_CLAMP_US
#define MICHI_AUDIO_JITTER_SAMPLE_CLAMP_US 1000000u /* 1 s: sender stalls are not jitter */
#endif

typedef struct {
    bool     seeded;
    int64_t  prev_arrival_us;
    uint32_t prev_rtp_ts;
    uint32_t jitter_us;
} michi_rtp_jitter_t;

static inline void michi_rtp_jitter_reset(michi_rtp_jitter_t *j)
{
    j->seeded = false;
    j->prev_arrival_us = 0;
    j->prev_rtp_ts = 0;
    j->jitter_us = 0;
}

static inline uint32_t michi_rtp_jitter_feed(michi_rtp_jitter_t *j,
                                             int64_t arrival_us,
                                             uint32_t rtp_ts,
                                             uint32_t sample_rate)
{
    if (!j->seeded) {
        j->seeded = true;
        j->prev_arrival_us = arrival_us;
        j->prev_rtp_ts = rtp_ts;
        j->jitter_us = 0;
        return 0;
    }

    if (sample_rate == 0) {
        sample_rate = 48000;
    }

    const int64_t d_arr_us = arrival_us - j->prev_arrival_us;
    const int32_t d_ts_samples = (int32_t)(rtp_ts - j->prev_rtp_ts);
    const int64_t d_ts_us = (int64_t)d_ts_samples * 1000000 / (int64_t)sample_rate;
    const int64_t d_us = d_arr_us - d_ts_us;
    int64_t abs_d = (d_us < 0) ? -d_us : d_us;
    if (abs_d > (int64_t)MICHI_AUDIO_JITTER_SAMPLE_CLAMP_US) {
        abs_d = (int64_t)MICHI_AUDIO_JITTER_SAMPLE_CLAMP_US;
    }

    j->jitter_us = (uint32_t)(((uint64_t)j->jitter_us * 15 + (uint64_t)abs_d) / 16);
    j->prev_arrival_us = arrival_us;
    j->prev_rtp_ts = rtp_ts;
    return j->jitter_us;
}

static inline int32_t michi_rtp_clock_offset_us(int64_t arrival_us,
                                                int64_t base_arrival_us,
                                                uint64_t ts_delta_64,
                                                uint32_t sample_rate)
{
    if (sample_rate == 0) {
        sample_rate = 48000;
    }
    const int64_t expected_us = base_arrival_us +
                                (int64_t)(ts_delta_64 * 1000000u / sample_rate);
    const int64_t offset_us = arrival_us - expected_us;
    if (offset_us > INT32_MAX) {
        return INT32_MAX;
    }
    if (offset_us < INT32_MIN) {
        return INT32_MIN;
    }
    return (int32_t)offset_us;
}

#ifdef __cplusplus
}
#endif
