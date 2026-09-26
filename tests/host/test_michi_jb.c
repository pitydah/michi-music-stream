/* Host-side tests for the jitter-buffer insertion logic (P0-08, PR M).
 *
 * Contracts under test:
 *   JB-1: Normal insert fills slots, count increments.
 *   JB-2: Duplicate seq is ignored (DUPLICATE, count unchanged).
 *   JB-3: Overrun evicts the OLDEST packet (closest to playhead, not behind).
 *   JB-4: After eviction the evicted seq is no longer in the buffer.
 *   JB-5: The newly inserted packet IS in the buffer after eviction.
 *   JB-6: 16-bit sequence wrap: oldest-relative-to-playhead uses signed diff.
 *   JB-7: michi_jb_flush() empties the buffer completely.
 *   JB-8: michi_jb_oldest() on an empty buffer returns NULL.
 *   JB-9: Insert after flush succeeds (buffer reusable).
 *   JB-10: 32 packets fill the buffer exactly (no phantom full at 31).
 *   JB-11: michi_jb_init with custom pool & entries, payload copy, and release.
 *   JB-12: michi_jb_oldest() selects closest packet behind playhead when all packets are behind.
 */

#include <inttypes.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "michi_jb.h"

static int failures = 0;

#define CHECK(cond, msg)                                                \
    do {                                                                \
        if (!(cond)) {                                                  \
            printf("  FAIL %s\n", msg);                                 \
            failures++;                                                  \
        }                                                               \
    } while (0)

/* ---- JB-1: normal insert ---- */
static void test_normal_insert(void)
{
    printf("jb: normal insert fills slots\n");
    michi_jb_t jb = {0};
    michi_jb_flush(&jb);

    for (uint16_t i = 0; i < 10; i++) {
        michi_jb_insert_result_t r = michi_jb_insert(&jb, 0, i, i * 480u, 1920u);
        CHECK(r == MICHI_JB_INSERT_OK, "insert OK");
    }
    CHECK(jb.count == 10, "count == 10 after 10 inserts");
    CHECK(michi_jb_find(&jb, 5) != NULL, "seq 5 findable");
}

/* ---- JB-2: duplicate ---- */
static void test_duplicate(void)
{
    printf("jb: duplicate seq ignored\n");
    michi_jb_t jb = {0};
    michi_jb_flush(&jb);

    michi_jb_insert(&jb, 0, 10, 0, 1920);
    uint32_t c = jb.count;
    michi_jb_insert_result_t r = michi_jb_insert(&jb, 0, 10, 100, 1920);
    CHECK(r == MICHI_JB_INSERT_DUPLICATE, "DUPLICATE returned");
    CHECK(jb.count == c, "count unchanged after duplicate");
}

/* ---- JB-3/4/5: overrun evicts oldest ---- */
static void test_overrun_evicts_oldest(void)
{
    printf("jb: overrun evicts oldest (relative to playhead)\n");
    michi_jb_t jb = {0};
    michi_jb_flush(&jb);

    /* Fill buffer: seq 1000..1031 (32 packets), playhead = 1000. */
    for (uint16_t i = 0; i < MICHI_JB_TEST_MAX_PACKETS; i++) {
        michi_jb_insert(&jb, 1000u, (uint16_t)(1000u + i), i * 480u, 1920u);
    }
    CHECK(jb.count == MICHI_JB_TEST_MAX_PACKETS, "buffer full");

    /* Insert seq 1032: should evict seq 1000 (oldest = closest to playhead). */
    michi_jb_insert_result_t r = michi_jb_insert(&jb, 1000u, 1032u, 32*480u, 1920u);
    CHECK(r == MICHI_JB_INSERT_OVERRUN_EVICTED, "OVERRUN_EVICTED returned");
    CHECK(jb.count == MICHI_JB_TEST_MAX_PACKETS, "count unchanged after evict+insert");
    CHECK(michi_jb_find(&jb, 1000u) == NULL, "evicted seq 1000 gone");   /* JB-4 */
    CHECK(michi_jb_find(&jb, 1032u) != NULL, "new seq 1032 present");    /* JB-5 */
}

/* ---- JB-6: 16-bit wrap ---- */
static void test_wrap_oldest(void)
{
    printf("jb: 16-bit seq wrap: oldest uses signed diff\n");
    michi_jb_t jb = {0};
    michi_jb_flush(&jb);

    /* Playhead at 0xFFF8. Packets: 0xFFF8, 0xFFF9, 0xFFFA, 0x0000 (wrapped). */
    michi_jb_insert(&jb, 0xFFF8u, 0xFFF8u, 0, 1920);
    michi_jb_insert(&jb, 0xFFF8u, 0xFFF9u, 480, 1920);
    michi_jb_insert(&jb, 0xFFF8u, 0xFFFAu, 960, 1920);
    michi_jb_insert(&jb, 0xFFF8u, 0x0000u, 1440, 1920); /* wraps */

    /* Oldest relative to playhead 0xFFF8 must be 0xFFF8 (diff=0, i.e. smallest). */
    michi_jb_entry_t *o = michi_jb_oldest(&jb, 0xFFF8u);
    CHECK(o != NULL, "oldest found across wrap");
    CHECK(o->seq == 0xFFF8u, "oldest is 0xFFF8 (not the wrapped 0x0000)");
}

/* ---- JB-7: flush ---- */
static void test_flush(void)
{
    printf("jb: flush empties buffer\n");
    michi_jb_t jb = {0};
    michi_jb_flush(&jb);
    for (uint16_t i = 0; i < 10; i++) michi_jb_insert(&jb, 0, i, 0, 1920);
    michi_jb_flush(&jb);
    CHECK(jb.count == 0, "count 0 after flush");
    CHECK(michi_jb_find(&jb, 5) == NULL, "seq 5 gone after flush");
}

/* ---- JB-8: oldest on empty ---- */
static void test_oldest_empty(void)
{
    printf("jb: oldest on empty buffer returns NULL\n");
    michi_jb_t jb = {0};
    michi_jb_flush(&jb);
    CHECK(michi_jb_oldest(&jb, 100) == NULL, "NULL on empty buffer");
}

/* ---- JB-9: reuse after flush ---- */
static void test_reuse_after_flush(void)
{
    printf("jb: buffer reusable after flush\n");
    michi_jb_t jb = {0};
    michi_jb_flush(&jb);
    michi_jb_insert(&jb, 0, 1, 0, 1920);
    michi_jb_flush(&jb);
    michi_jb_insert_result_t r = michi_jb_insert(&jb, 0, 1, 480, 1920);
    CHECK(r == MICHI_JB_INSERT_OK, "re-insert after flush OK");
    CHECK(jb.count == 1, "count 1 after re-insert");
}

/* ---- JB-10: exact capacity ---- */
static void test_exact_capacity(void)
{
    printf("jb: exactly %u packets fill buffer, %u+1 triggers overrun\n",
           MICHI_JB_TEST_MAX_PACKETS, MICHI_JB_TEST_MAX_PACKETS);
    michi_jb_t jb = {0};
    michi_jb_flush(&jb);
    for (uint16_t i = 0; i < MICHI_JB_TEST_MAX_PACKETS; i++) {
        michi_jb_insert_result_t r = michi_jb_insert(&jb, 0, i, i * 480u, 1920);
        CHECK(r == MICHI_JB_INSERT_OK, "insert OK up to capacity");
    }
    CHECK(jb.count == MICHI_JB_TEST_MAX_PACKETS, "count at full capacity");
    /* One more must trigger overrun. */
    michi_jb_insert_result_t r = michi_jb_insert(
        &jb, 0, MICHI_JB_TEST_MAX_PACKETS, 0, 1920);
    CHECK(r == MICHI_JB_INSERT_OVERRUN_EVICTED, "capacity+1 triggers eviction");
}

/* ---- JB-11: payload pool and release (production path) ---- */
static void test_payload_pool(void)
{
    printf("jb: payload pool write and read with release (production path)\n");
    michi_jb_t jb = {0};
    uint8_t pool[4 * 1920];
    michi_jb_entry_t entries[4];
    michi_jb_init(&jb, entries, pool, 4, 1920);

    uint8_t sample_data[1920];
    memset(sample_data, 0xAB, sizeof(sample_data));
    uint16_t evicted = 0;
    michi_jb_insert_result_t r = michi_jb_insert_packet(&jb, 10, 10, 1000, sample_data, 1920, &evicted);
    CHECK(r == MICHI_JB_INSERT_OK, "insert with payload OK");
    CHECK(jb.count == 1, "count == 1");

    michi_jb_entry_t *found = michi_jb_find(&jb, 10);
    CHECK(found != NULL, "found seq 10");
    uint8_t *payload = michi_jb_entry_payload(&jb, found);
    CHECK(payload != NULL, "payload not NULL");
    CHECK(memcmp(payload, sample_data, 1920) == 0, "payload matches");

    michi_jb_release(&jb, found);
    CHECK(jb.count == 0, "count == 0 after release");
    CHECK(michi_jb_find(&jb, 10) == NULL, "seq 10 gone after release");
}

/* ---- JB-12: all packets behind playhead picks closest to playhead ---- */
static void test_oldest_all_behind_playhead(void)
{
    printf("jb: all packets behind playhead selects closest to playhead (least negative diff)\n");
    michi_jb_t jb = {0};
    michi_jb_flush(&jb);

    /* Insert packets with seq 97, 98, 99. Playhead is at 100.
     * All diffs are negative: 97 is -3, 98 is -2, 99 is -1.
     * The fallback in michi_jb_oldest must select seq 99 (diff -1), NOT 97. */
    michi_jb_insert(&jb, 90u, 97u, 0, 1920);
    michi_jb_insert(&jb, 90u, 98u, 480, 1920);
    michi_jb_insert(&jb, 90u, 99u, 960, 1920);

    michi_jb_entry_t *o = michi_jb_oldest(&jb, 100u);
    CHECK(o != NULL, "entry found when all behind playhead");
    if (o != NULL) {
        CHECK(o->seq == 99u, "closest packet behind playhead (seq 99) selected");
    }
}

int main(void)
{
    test_normal_insert();
    test_duplicate();
    test_overrun_evicts_oldest();
    test_wrap_oldest();
    test_flush();
    test_oldest_empty();
    test_reuse_after_flush();
    test_exact_capacity();
    test_payload_pool();
    test_oldest_all_behind_playhead();

    if (failures == 0) {
        printf("PASS test_michi_jb\n");
        return 0;
    }
    printf("FAIL test_michi_jb (%d failures)\n", failures);
    return 1;
}
