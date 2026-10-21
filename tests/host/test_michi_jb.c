/* Host-side tests for the jitter-buffer insertion logic (P0-08, PR M).
 *
 * Contracts under test:
 *   JB-1: Normal insert fills slots, count increments.
 *   JB-2: Duplicate seq is ignored (DUPLICATE, count unchanged).
 *   JB-3: Full buffer preserves the imminent packet; farther arrivals drop.
 *   JB-4: Reordered earlier PCM evicts the farthest future packet only.
 *   JB-5: Old, stale and duplicate packets cannot displace live PCM.
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
#include "michi_audio_recv_policy.h"

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

/* ---- JB-3/4/5: full buffer prefers imminent PCM ---- */
static void test_overrun_preserves_urgent_pcm(void)
{
    printf("jb: full queue cannot sacrifice imminent PCM for distant arrival\n");
    michi_jb_t jb = {0};
    michi_jb_flush(&jb);
    for (uint16_t i = 0; i < MICHI_JB_TEST_MAX_PACKETS; ++i) {
        CHECK(michi_jb_insert(&jb, 1000u, (uint16_t)(1000u + i),
                              i * 480u, 1920u) == MICHI_JB_INSERT_OK,
              "fill packet accepted");
    }
    uint16_t evicted = 0xA5A5u;
    const uint8_t audio[1920] = {0};
    CHECK(michi_jb_insert_packet(&jb, 1000u, 1032u, 32u * 480u,
                                 audio, 1920u, &evicted) ==
              MICHI_JB_INSERT_OVERRUN_DROPPED,
          "farthest future packet must drop");
    CHECK(evicted == 0xA5A5u, "dropped arrival must not set evicted seq");
    CHECK(jb.count == MICHI_JB_TEST_MAX_PACKETS, "count remains full");
    CHECK(michi_jb_find(&jb, 1000u) != NULL, "imminent packet protected");
    CHECK(michi_jb_find(&jb, 1031u) != NULL, "existing future packet protected");
    CHECK(michi_jb_find(&jb, 1032u) == NULL, "rejected packet not queued");
}

static void test_overrun_reordered_near_packet(void)
{
    printf("jb: reordered near packet replaces farthest future PCM\n");
    michi_jb_t jb = {0};
    michi_jb_entry_t entries[4];
    uint8_t pool[4 * 1920];
    uint8_t pcm[1920];
    memset(pool, 0xAB, sizeof(pool));
    memset(pcm, 0x33, sizeof(pcm));
    CHECK(michi_jb_init(&jb, entries, pool, 4, 1920) == ESP_OK,
          "pooled queue initialized");
    const uint16_t sequences[] = {1000u, 1002u, 1003u, 1004u};
    for (uint32_t i = 0; i < 4u; ++i) {
        CHECK(michi_jb_insert_packet(&jb, 1000u, sequences[i], i * 480u,
                                     pcm, 1920, NULL) == MICHI_JB_INSERT_OK,
              "fill pooled queue");
    }
    uint16_t evicted = 0;
    memset(pcm, 0x5C, sizeof(pcm));
    CHECK(michi_jb_insert_packet(&jb, 1000u, 1001u, 480u, pcm, 1920u,
                                 &evicted) == MICHI_JB_INSERT_OVERRUN_EVICTED,
          "near reordered PCM replaces distant slot");
    CHECK(evicted == 1004u, "farthest future sequence evicted");
    CHECK(jb.count == 4u, "bounded count after replacement");
    CHECK(michi_jb_find(&jb, 1000u) != NULL, "current cursor retained");
    CHECK(michi_jb_find(&jb, 1004u) == NULL, "farthest discarded");
    michi_jb_entry_t *near = michi_jb_find(&jb, 1001u);
    CHECK(near != NULL, "closer PCM queued");
    if (near != NULL) {
        uint8_t *data = michi_jb_entry_payload(&jb, near);
        CHECK(data != NULL && memcmp(data, pcm, sizeof(pcm)) == 0,
              "new PCM copied to reclaimed slot without corruption");
    }
    CHECK(michi_jb_insert_packet(&jb, 1000u, 999u, 0, pcm, 1920u,
                                 NULL) == MICHI_JB_INSERT_OVERRUN_DROPPED,
          "late packet must not replace valid PCM");
    CHECK(michi_jb_insert_packet(&jb, 1000u, 1001u, 0, pcm, 1920u,
                                 NULL) == MICHI_JB_INSERT_DUPLICATE,
          "duplicate rejected before displacement");
}

static void test_overrun_reclaim_stale_and_wrap(void)
{
    printf("jb: stale slot reclaimed; selection works through seq wrap\n");
    michi_jb_t jb = {0};
    michi_jb_entry_t entries[4];
    CHECK(michi_jb_init(&jb, entries, NULL, 4, 0) == ESP_OK,
          "metadata test slots initialized");
    const uint16_t initial[] = {0xFFFDu, 0xFFFEu, 0x0000u, 0x0001u};
    for (unsigned i = 0; i < 4; ++i) {
        CHECK(michi_jb_insert(&jb, 0xFFFDu, initial[i], i * 480u, 1920u) ==
              MICHI_JB_INSERT_OK, "wrap queue prefill");
    }
    uint16_t evicted = 0;
    CHECK(michi_jb_insert_packet(&jb, 0xFFFEu, 0xFFFFu, 960u,
                                 NULL, 1920u, &evicted) ==
              MICHI_JB_INSERT_OVERRUN_EVICTED,
          "new near packet replaces stale 0xFFFD");
    CHECK(evicted == 0xFFFDu, "stale slot evicted before future slots");
    CHECK(michi_jb_find(&jb, 0xFFFFu) != NULL, "near wrapped PCM retained");
    CHECK(michi_jb_find(&jb, 0xFFFEu) != NULL, "cursor packet retained");
    CHECK(michi_jb_oldest(&jb, 0xFFFEu)->seq == 0xFFFEu,
          "wrap-aware earliest playable packet selected");
    CHECK(michi_jb_insert(&jb, 0xFFFEu, 0x0002u, 0, 1920u) ==
          MICHI_JB_INSERT_OVERRUN_DROPPED,
          "additional farther wrapped packet refused");
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
    printf("jb: exactly %u packets fill buffer; farther arrivals are dropped\n",
           MICHI_JB_TEST_MAX_PACKETS);
    michi_jb_t jb = {0};
    michi_jb_flush(&jb);
    for (uint16_t i = 0; i < MICHI_JB_TEST_MAX_PACKETS; i++) {
        michi_jb_insert_result_t r = michi_jb_insert(&jb, 0, i, i * 480u, 1920);
        CHECK(r == MICHI_JB_INSERT_OK, "insert OK up to capacity");
    }
    CHECK(jb.count == MICHI_JB_TEST_MAX_PACKETS, "count at full capacity");
    /* One farther packet must be dropped; the playback head survives. */
    michi_jb_insert_result_t r = michi_jb_insert(
        &jb, 0, MICHI_JB_TEST_MAX_PACKETS, 0, 1920);
    CHECK(r == MICHI_JB_INSERT_OVERRUN_DROPPED, "capacity+1 drops distant arrival");
    CHECK(jb.count == MICHI_JB_TEST_MAX_PACKETS, "capacity remains bounded");
    CHECK(michi_jb_find(&jb, 0u) != NULL, "cursor remains available");
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

/* JB-13: stale seq cannot become a 65535-packet silence gap. */
static void test_stale_discard(void)
{
    printf("jb: stale playout packet is dropped, not replayed\n");
    michi_jb_t jb = {0};
    michi_jb_flush(&jb);
    michi_jb_insert(&jb, 90u, 99u, 0, 1920);
    michi_jb_insert(&jb, 90u, 100u, 480, 1920);
    michi_jb_insert(&jb, 90u, 101u, 960, 1920);
    CHECK(michi_jb_discard_stale(&jb, 100u) == 1, "one stale packet discarded");
    CHECK(jb.count == 2, "future packets stay queued");
    CHECK(michi_jb_find(&jb, 99u) == NULL, "stale seq removed");
    CHECK(michi_jb_oldest(&jb, 100u)->seq == 100u, "playhead seq remains playable");
    CHECK(michi_jb_discard_stale(&jb, 102u) == 2, "all obsolete packets removed");
    CHECK(michi_jb_oldest(&jb, 102u) == NULL, "nothing to replay");

    michi_jb_insert(&jb, 0xFFFFu, 0xFFFFu, 0, 1920);
    michi_jb_insert(&jb, 0xFFFFu, 0u, 480, 1920);
    CHECK(michi_jb_discard_stale(&jb, 0u) == 1, "wrap: FFFF is behind 0000");
    CHECK(michi_jb_find(&jb, 0u) != NULL, "wrap: 0000 remains playable");
}

/* JB-14: oversize writes and missing PCM must not corrupt/eject live slots. */
static void test_pooled_payload_bounds(void)
{
    printf("jb: payload pool rejects oversize, missing PCM and zero-stride\n");
    michi_jb_t jb = {0};
    uint8_t pool[2 * 1920];
    michi_jb_entry_t entries[2];
    uint8_t pcm[1921];
    memset(pool, 0x5A, sizeof(pool));
    memset(pcm, 0xAB, sizeof(pcm));
    CHECK(michi_jb_init(&jb, entries, pool, 2, 1920) == ESP_OK, "init 1920-byte slots");
    CHECK(michi_jb_insert_packet(&jb, 100, 100, 1, pcm, 1920, NULL) == MICHI_JB_INSERT_OK,
          "first PCM fits exactly");
    CHECK(michi_jb_insert_packet(&jb, 100, 101, 2, pcm, 1921, NULL) == MICHI_JB_INSERT_OVERRUN_DROPPED,
          "oversized packet rejected");
    CHECK(michi_jb_insert_packet(&jb, 100, 101, 2, NULL, 1920, NULL) == MICHI_JB_INSERT_OVERRUN_DROPPED,
          "missing production payload rejected");
    CHECK(jb.count == 1 && michi_jb_find(&jb, 100) != NULL, "good audio remains queued");
    CHECK(pool[1920] == 0x5A, "neighboring slot untouched");
    CHECK(michi_jb_init(&jb, entries, pool, 2, 0) == ESP_ERR_INVALID_ARG,
          "zero-stride pool is invalid");
    CHECK(michi_jb_init(&jb, NULL, NULL, MICHI_JB_TEST_MAX_PACKETS + 1, 0) == ESP_ERR_INVALID_ARG,
          "static entries cannot exceed fixed capacity");
}

/* H5-01: transient timeout and EINTR cannot terminate RTP. Unexpected
 * socket errors close a session after a bounded number of failures. */
static void test_socket_receive_error_policy(void)
{
    uint8_t streak = 0;
    CHECK(!michi_audio_recv_error_is_fatal(&streak, EAGAIN), "timeout is transient");
    CHECK(!michi_audio_recv_error_is_fatal(&streak, EINTR), "EINTR is transient");
    CHECK(streak == 0, "no fatal failures counted for transient conditions");
    CHECK(!michi_audio_recv_error_is_fatal(&streak, EBADF), "first unexpected recv failure");
    CHECK(!michi_audio_recv_error_is_fatal(&streak, ENOTSOCK), "second unexpected recv failure");
    CHECK(streak == 2, "fatal streak counts consecutive errors");
    CHECK(!michi_audio_recv_error_is_fatal(&streak, EWOULDBLOCK), "timeout resets streak");
    CHECK(streak == 0, "timeout resets unexpected recv failures");
    CHECK(!michi_audio_recv_error_is_fatal(&streak, EBADF), "unexpected error count 1");
    CHECK(!michi_audio_recv_error_is_fatal(&streak, EBADF), "unexpected error count 2");
    CHECK(michi_audio_recv_error_is_fatal(&streak, EBADF), "unexpected error count 3 closes");
    CHECK(streak == MICHI_AUDIO_RECV_FATAL_THRESHOLD, "failure count saturates");
    michi_audio_recv_error_reset(&streak);
    CHECK(streak == 0, "successful recv clears streak");
}

int main(void)
{
    test_normal_insert();
    test_duplicate();
    test_overrun_preserves_urgent_pcm();
    test_overrun_reordered_near_packet();
    test_overrun_reclaim_stale_and_wrap();
    test_wrap_oldest();
    test_flush();
    test_oldest_empty();
    test_reuse_after_flush();
    test_exact_capacity();
    test_payload_pool();
    test_oldest_all_behind_playhead();
    test_stale_discard();
    test_pooled_payload_bounds();
    test_socket_receive_error_policy();

    if (failures == 0) {
        printf("PASS test_michi_jb\n");
        return 0;
    }
    printf("FAIL test_michi_jb (%d failures)\n", failures);
    return 1;
}
