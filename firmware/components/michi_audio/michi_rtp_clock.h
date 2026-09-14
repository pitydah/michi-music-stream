/* michi_rtp_clock.h — 64-bit extended RTP timestamp tracker (P0-02)
 *
 * RTP timestamps are uint32_t and wrap at 2^32 samples (~24.85h at
 * 48kHz). Long sessions without a stream discontinuity will silently
 * produce a huge ts_delta in the jitter calculation, triggering the
 * clamp and distorting the EWMA for hours afterwards.
 *
 * This module maintains a monotonic 64-bit extension of the raw RTP
 * timestamp using forward-wrap detection: a new raw value that is
 * smaller than the previous AND differs by more than 2^31 is an
 * epoch increment.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t last_raw;   /*!< last accepted raw RTP timestamp */
    uint64_t extended;   /*!< 64-bit monotonic form of last_raw */
    uint64_t base;       /*!< extended ts of the first (or resynced) packet */
    bool     seeded;     /*!< true after at least one rtp_clock_feed() call */
} michi_rtp_clock_t;

/** @brief Reset the clock (call on session start or discontinuity). */
static inline void michi_rtp_clock_reset(michi_rtp_clock_t *c)
{
    c->last_raw = 0;
    c->extended = 0;
    c->base     = 0;
    c->seeded   = false;
}

/** @brief Feed a raw RTP timestamp.  Updates extended and, on the first
 *         call after a reset, also seeds the base.
 *
 *  @param c      Clock state.
 *  @param raw    Raw RTP timestamp from the packet header.
 *  @return 64-bit extended form of raw.
 */
static inline uint64_t michi_rtp_clock_feed(michi_rtp_clock_t *c, uint32_t raw)
{
    if (!c->seeded) {
        c->last_raw = raw;
        c->extended = (uint64_t)raw;
        c->base     = (uint64_t)raw;
        c->seeded   = true;
        return c->extended;
    }

    const uint32_t prev = c->last_raw;
    /* Forward wrap: new_raw < prev AND the backwards distance > 2^31. */
    if (raw < prev && (prev - raw) > 0x80000000u) {
        c->extended = ((c->extended & 0xFFFFFFFF00000000ULL) + 0x100000000ULL)
                      | (uint64_t)raw;
    } else {
        c->extended = (c->extended & 0xFFFFFFFF00000000ULL) | (uint64_t)raw;
    }
    c->last_raw = raw;
    return c->extended;
}

/** @brief Timestamp delta from the base in samples (64-bit, no wrap). */
static inline uint64_t michi_rtp_clock_delta(const michi_rtp_clock_t *c)
{
    return c->extended - c->base;
}

#ifdef __cplusplus
}
#endif
