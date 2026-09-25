#include "michi_jb.h"
#include <string.h>

esp_err_t michi_jb_init(michi_jb_t *jb, michi_jb_entry_t *entries, uint8_t *pool,
                        uint32_t capacity, size_t rx_buf_bytes)
{
    if (jb == NULL || capacity == 0) {
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
            if (diff_e < diff_best) {
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

    /* Duplicate? */
    if (michi_jb_find(jb, seq) != NULL) {
        return MICHI_JB_INSERT_DUPLICATE;
    }

    michi_jb_insert_result_t res = MICHI_JB_INSERT_OK;

    if (jb->count >= jb->capacity) {
        michi_jb_entry_t *oldest = michi_jb_oldest(jb, playhead);
        if (oldest != NULL) {
            if (out_evicted_seq != NULL) {
                *out_evicted_seq = oldest->seq;
            }
            oldest->used = false;
            jb->count--;
            res = MICHI_JB_INSERT_OVERRUN_EVICTED;
        } else {
            return MICHI_JB_INSERT_OVERRUN_DROPPED;
        }
    }

    for (uint32_t i = 0; i < jb->capacity; i++) {
        michi_jb_entry_t *slot = &jb->entries[i];
        if (!slot->used) {
            if (payload != NULL && jb->pool != NULL) {
                size_t copy_bytes = len;
                if (jb->rx_buf_bytes > 0 && copy_bytes > jb->rx_buf_bytes) {
                    copy_bytes = jb->rx_buf_bytes;
                }
                memcpy(jb->pool + (size_t)i * jb->rx_buf_bytes, payload, copy_bytes);
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
