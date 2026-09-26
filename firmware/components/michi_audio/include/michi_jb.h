#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MICHI_JB_TEST_MAX_PACKETS 32u

typedef struct {
    uint16_t seq;
    uint32_t timestamp;
    uint16_t len;
    bool     used;
} michi_jb_entry_t;

typedef struct {
    michi_jb_entry_t entries_static[MICHI_JB_TEST_MAX_PACKETS];
    michi_jb_entry_t *entries;
    uint8_t          *pool;
    uint32_t          count;
    uint32_t          capacity;
    size_t            rx_buf_bytes;
} michi_jb_t;

typedef enum {
    MICHI_JB_INSERT_OK,
    MICHI_JB_INSERT_DUPLICATE,
    MICHI_JB_INSERT_OVERRUN_DROPPED,  /* buffer full, packet dropped (no slot) */
    MICHI_JB_INSERT_OVERRUN_EVICTED,  /* buffer full, oldest evicted */
} michi_jb_insert_result_t;

esp_err_t michi_jb_init(michi_jb_t *jb, michi_jb_entry_t *entries, uint8_t *pool,
                        uint32_t capacity, size_t rx_buf_bytes);

void michi_jb_flush(michi_jb_t *jb);

michi_jb_entry_t *michi_jb_find(const michi_jb_t *jb, uint16_t seq);

michi_jb_entry_t *michi_jb_oldest(const michi_jb_t *jb, uint16_t playhead);

michi_jb_insert_result_t michi_jb_insert_packet(michi_jb_t *jb, uint16_t playhead,
                                               uint16_t seq, uint32_t timestamp,
                                               const void *payload, uint16_t len,
                                               uint16_t *out_evicted_seq);

static inline michi_jb_insert_result_t michi_jb_insert(
        michi_jb_t *jb, uint16_t playhead,
        uint16_t seq, uint32_t timestamp, uint16_t len)
{
    return michi_jb_insert_packet(jb, playhead, seq, timestamp, NULL, len, NULL);
}

static inline michi_jb_insert_result_t michi_jb_insert_simple(
        michi_jb_t *jb, uint16_t playhead,
        uint16_t seq, uint32_t timestamp, uint16_t len)
{
    return michi_jb_insert_packet(jb, playhead, seq, timestamp, NULL, len, NULL);
}

void michi_jb_release(michi_jb_t *jb, michi_jb_entry_t *e);

static inline uint32_t michi_jb_entry_index(const michi_jb_t *jb, const michi_jb_entry_t *e)
{
    return (uint32_t)(e - jb->entries);
}

static inline michi_jb_entry_t *michi_jb_entry_by_index(const michi_jb_t *jb, uint32_t idx)
{
    return &jb->entries[idx];
}

static inline uint8_t *michi_jb_entry_payload(const michi_jb_t *jb, const michi_jb_entry_t *e)
{
    if (jb->pool == NULL) {
        return NULL;
    }
    return jb->pool + (size_t)michi_jb_entry_index(jb, e) * jb->rx_buf_bytes;
}

#ifdef __cplusplus
}
#endif
