#include "michi_jb.h"
#include <string.h>

esp_err_t michi_jb_init(michi_jb_t *jb, michi_jb_entry_t *entries, uint8_t *pool,
                        uint32_t capacity, size_t rx_buf_bytes)
{
    if (jb == NULL || capacity == 0 ||
        (entries == NULL && capacity > MICHI_JB_TEST_MAX_PACKETS) ||
        (pool != NULL && rx_buf_bytes == 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    jb->entries = (entries != NULL) ? entries : jb->entries_static;
    jb->pool = pool;
    jb->capacity = capacity;
    jb->rx_buf_bytes = rx_buf_bytes;
    michi_jb_flush(jb);
    return ESP_OK;
}

void michi_jb_flush(michi_jb_t *jb)
{
    if (jb == NULL) {
        return;
    }
    if (jb->entries == NULL) {
        jb->entries = jb->entries_static;
        jb->capacity = MICHI_JB_TEST_MAX_PACKETS;
        jb->pool = NULL;
        jb->rx_buf_bytes = 0;
    }
    for (uint32_t i = 0; i < jb->capacity; i++) {
        jb->entries[i].used = false;
        jb->entries[i].seq = 0;
        jb->entries[i].timestamp = 0;
        jb->entries[i].len = 0;
    }
    jb->count = 0;
}

michi_jb_entry_t *michi_jb_find(const michi_jb_t *jb, uint16_t seq)
{
    if (jb == NULL || jb->entries == NULL) {
        return NULL;
    }
    for (uint32_t i = 0; i < jb->capacity; i++) {
        if (jb->entries[i].used && jb->entries[i].seq == seq) {
            return &jb->entries[i];
        }
    }
    return NULL;
}

michi_jb_entry_t *michi_jb_oldest(const michi_jb_t *jb, uint16_t playhead)
{
    if (jb == NULL || jb->entries == NULL) {
        return NULL;
    }
    michi_jb_entry_t *best = NULL;
    int16_t best_diff = INT16_MAX;
    for (uint32_t i = 0; i < jb->capacity; i++) {
        michi_jb_entry_t *e = &jb->entries[i];
        if (!e->used) {
            continue;
        }
        int16_t diff = (int16_t)(e->seq - playhead);
        if (diff >= 0 && diff < best_diff) {
            best = e;
            best_diff = diff;
        }
    }
    /* If no packet is >= playhead (all are strictly behind), pick the one closest to playhead */
    if (best == NULL) {
        for (uint32_t i = 0; i < jb->capacity; i++) {
            michi_jb_entry_t *e = &jb->entries[i];
            if (!e->used) {
                continue;
            }
            if (best == NULL) {
                best = e;
                continue;
            }
            int16_t diff_e = (int16_t)(e->seq - playhead);
            int16_t diff_best = (int16_t)(best->seq - playhead);
            /* Since both diffs are negative, the entry closest to playhead has
             * the maximum (least negative) signed difference (e.g. -1 > -3). */
            if (diff_e > diff_best) {
                best = e;
            }
        }
    }
    return best;
}

michi_jb_insert_result_t michi_jb_insert_packet(michi_jb_t *jb, uint16_t playhead,
                                               uint16_t seq, uint32_t timestamp,
                                               const void *payload, uint16_t len,
                                               uint16_t *out_evicted_seq)
{
    if (jb == NULL) {
        return MICHI_JB_INSERT_OVERRUN_DROPPED;
    }
    if (jb->entries == NULL) {
        jb->entries = jb->entries_static;
        jb->capacity = MICHI_JB_TEST_MAX_PACKETS;
        jb->pool = NULL;
        jb->rx_buf_bytes = 0;
    }

    /* Reject invalid/oversized payload BEFORE evicting a valid packet.
     * Silent clipping paired with slot->len=len could otherwise read beyond
     * the slot at playback. A test-only buffer (pool=NULL) allows metadata
     * inserts with NULL payload; production pooled buffers do not. */
    if (jb->pool != NULL &&
        (jb->rx_buf_bytes == 0 || len > jb->rx_buf_bytes ||
         (len != 0 && payload == NULL))) {
        return MICHI_JB_INSERT_OVERRUN_DROPPED;
    }

    /* H6: never insert a packet behind the current RTP playout cursor,
     * regardless of whether capacity is available. Signed 16-bit sequence
     * distance works across FFFF->0000 wrap for a bounded jitter window. */
    const int16_t incoming_distance = (int16_t)(seq - playhead);
    if (incoming_distance < 0) {
        return MICHI_JB_INSERT_OVERRUN_DROPPED;
    }

    /* Duplicate detection precedes eviction and never mutates the queue. */
    if (michi_jb_find(jb, seq) != NULL) {
        return MICHI_JB_INSERT_DUPLICATE;
    }

    michi_jb_insert_result_t res = MICHI_JB_INSERT_OK;

    if (jb->count >= jb->capacity) {
        /* H6 playout-first full-buffer policy: first reclaim obsolete PCM,
         * never destroy the playhead packet to make room for distant audio.
         * If no stale slot exists, a closer packet may replace ONLY the
         * farthest future slot; otherwise the arrival is rejected. */
        michi_jb_entry_t *evict = NULL;
        int16_t farthest_future = -1;
        for (uint32_t i = 0; i < jb->capacity; ++i) {
            michi_jb_entry_t *entry = &jb->entries[i];
            if (!entry->used) continue;
            const int16_t distance = (int16_t)(entry->seq - playhead);
            if (distance < 0) {
                evict = entry;  /* Prefer a stale slot over all future PCM. */
                break;
            }
            if (distance > farthest_future) {
                farthest_future = distance;
                evict = entry;
            }
        }
        if (evict == NULL) {
            return MICHI_JB_INSERT_OVERRUN_DROPPED;
        }
        const int16_t candidate_distance =
            (int16_t)(evict->seq - playhead);
        if (candidate_distance >= 0 &&
            incoming_distance >= candidate_distance) {
            /* Arrival is at least as far from playout as existing PCM. */
            return MICHI_JB_INSERT_OVERRUN_DROPPED;
        }
        if (out_evicted_seq != NULL) {
            *out_evicted_seq = evict->seq;
        }
        evict->used = false;
        jb->count--;
        res = MICHI_JB_INSERT_OVERRUN_EVICTED;
    }

    for (uint32_t i = 0; i < jb->capacity; i++) {
        michi_jb_entry_t *slot = &jb->entries[i];
        if (!slot->used) {
            if (payload != NULL && jb->pool != NULL && len != 0) {
                memcpy(jb->pool + (size_t)i * jb->rx_buf_bytes, payload, len);
            }
            slot->seq = seq;
            slot->timestamp = timestamp;
            slot->len = len;
            slot->used = true;
            jb->count++;
            return res;
        }
    }

    return MICHI_JB_INSERT_OVERRUN_DROPPED;
}

void michi_jb_release(michi_jb_t *jb, michi_jb_entry_t *e)
{
    if (jb == NULL || e == NULL || !e->used) {
        return;
    }
    e->used = false;
    if (jb->count > 0) {
        jb->count--;
    }
}

/* Reject packets whose signed 16-bit RTP distance is behind the playout
 * cursor. Works across wrap (0xFFFF -> 0); caller counts these as late.
 * This prevents accidental huge silence insertion from a stale buffer head. */
uint32_t michi_jb_discard_stale(michi_jb_t *jb, uint16_t playhead)
{
    if (jb == NULL || jb->entries == NULL) {
        return 0;
    }
    uint32_t removed = 0;
    for (uint32_t i = 0; i < jb->capacity; ++i) {
        michi_jb_entry_t *entry = &jb->entries[i];
        if (entry->used && (int16_t)(entry->seq - playhead) < 0) {
            michi_jb_release(jb, entry);
            ++removed;
        }
    }
    return removed;
}
