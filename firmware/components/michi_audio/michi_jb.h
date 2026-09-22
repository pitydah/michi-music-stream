/* michi_jb.h — Jitter-buffer insertion logic, header-only (P0-08 PR M).
 *
 * The actual jitter_buffer_t in michi_audio.c uses PSRAM for the pool and
 * is deeply embedded in session_t. This module re-exposes the slot-selection
 * and drop-oldest policy as a pure-C function so host tests can exercise it
 * without any firmware deps.
 *
 * The logic mirrors jb_insert() / jb_oldest() in michi_audio.c verbatim.
 * Keep in sync with any changes to those functions.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Tiny testable jitter-buffer (stack/heap, no PSRAM) */
/* ------------------------------------------------------------------ */

#define MICHI_JB_TEST_MAX_PACKETS 32u

typedef struct {
    uint16_t seq;
    uint32_t timestamp;
    uint16_t len;
    bool     used;
} michi_jb_entry_t;

typedef struct {
    michi_jb_entry_t entries[MICHI_JB_TEST_MAX_PACKETS];
    uint32_t         count;
} michi_jb_t;

/* Reset (flush) the buffer. */
static inline void michi_jb_flush(michi_jb_t *jb)
{
    memset(jb->entries, 0, sizeof(jb->entries));
    jb->count = 0;
}

/* Find the slot with the smallest (int16_t)(seq - playhead), i.e. the
 * packet closest to but not behind the playhead. */
static inline michi_jb_entry_t *michi_jb_oldest(michi_jb_t *jb,
                                                  uint16_t playhead)
{
    michi_jb_entry_t *best = NULL;
    for (uint32_t i = 0; i < MICHI_JB_TEST_MAX_PACKETS; i++) {
        michi_jb_entry_t *e = &jb->entries[i];
        if (!e->used) continue;
        if (best == NULL) { best = e; continue; }
        int16_t diff_e    = (int16_t)(e->seq    - playhead);
        int16_t diff_best = (int16_t)(best->seq  - playhead);
        if (diff_e < diff_best) best = e;
    }
    return best;
}

/* Find an exact seq match. */
static inline michi_jb_entry_t *michi_jb_find(michi_jb_t *jb, uint16_t seq)
{
    for (uint32_t i = 0; i < MICHI_JB_TEST_MAX_PACKETS; i++) {
        if (jb->entries[i].used && jb->entries[i].seq == seq)
            return &jb->entries[i];
    }
    return NULL;
}

typedef enum {
    MICHI_JB_INSERT_OK,
    MICHI_JB_INSERT_DUPLICATE,
    MICHI_JB_INSERT_OVERRUN_DROPPED,  /* buffer full, packet dropped (no slot) */
    MICHI_JB_INSERT_OVERRUN_EVICTED,  /* buffer full, oldest evicted */
} michi_jb_insert_result_t;

/** @brief Insert pkt into the jitter buffer.
 *
 * Mirrors jb_insert() in michi_audio.c:
 *   - Duplicate seq → DUPLICATE (ignored).
 *   - Buffer full, found oldest → evict oldest, insert pkt.
 *   - Buffer full, all slots used (shouldn't happen) → OVERRUN_DROPPED.
 *   - Buffer not full → insert in first free slot.
 */
static inline michi_jb_insert_result_t michi_jb_insert(
        michi_jb_t *jb, uint16_t playhead,
        uint16_t seq, uint32_t timestamp, uint16_t len)
{
    /* Duplicate? */
    if (michi_jb_find(jb, seq) != NULL)
        return MICHI_JB_INSERT_DUPLICATE;

    michi_jb_insert_result_t res = MICHI_JB_INSERT_OK;

    if (jb->count >= MICHI_JB_TEST_MAX_PACKETS) {
        /* Drop-oldest overrun policy. */
        michi_jb_entry_t *oldest = michi_jb_oldest(jb, playhead);
        if (oldest != NULL) {
            oldest->used = false;
            jb->count--;
            res = MICHI_JB_INSERT_OVERRUN_EVICTED;
        } else {
            return MICHI_JB_INSERT_OVERRUN_DROPPED;
        }
    }

    /* Insert in first free slot. */
    for (uint32_t i = 0; i < MICHI_JB_TEST_MAX_PACKETS; i++) {
        if (!jb->entries[i].used) {
            jb->entries[i].seq       = seq;
            jb->entries[i].timestamp = timestamp;
            jb->entries[i].len       = len;
            jb->entries[i].used      = true;
            jb->count++;
            return res;
        }
    }
    return MICHI_JB_INSERT_OVERRUN_DROPPED;
}

#ifdef __cplusplus
}
#endif
