/* Host-side tests for the canonical RTP guard (MS-07).
 *
 * Compiles the REAL firmware source: components/michi_audio/rtp_guard.c
 * (linked from the Makefile) - the SAME validation the firmware engine
 * runs on every datagram. No reimplementation.
 *
 * Covers (contract section 2.5):
 *  - strict header parse: RTP v2 with no CSRC/extension/padding only;
 *  - classification: PT/SSRC/source-IP/size rejects per class;
 *  - 16-bit sequence wrap math for the packets_lost accounting.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "rtp_guard.h"

static int failures = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            printf("  FAIL %s\n", msg);                                     \
            failures++;                                                     \
        }                                                                   \
    } while (0)

/* Canonical datagram: v2, no CSRC/ext/pad, PT 97, 1920-byte payload. */
#define SSRC 0x1234ABCDu
static uint8_t buf[12 + MICHI_RTP_GUARD_PAYLOAD_BYTES + 16];

static void build_canonical(uint8_t *b, size_t payload_len)
{
    memset(b, 0, 12 + payload_len + 16);
    b[0] = 0x80; /* v2, no pad, no ext, no CSRC */
    b[1] = MICHI_RTP_GUARD_PT_S16LE;
    b[2] = 0x00;
    b[3] = 0x01; /* seq 1 */
    b[4] = 0x00;
    b[5] = 0x00;
    b[6] = 0x00;
    b[7] = 0x00;
    b[8] = (uint8_t)(SSRC >> 24);
    b[9] = (uint8_t)(SSRC >> 16);
    b[10] = (uint8_t)(SSRC >> 8);
    b[11] = (uint8_t)SSRC;
    for (size_t i = 0; i < payload_len; i++) {
        b[12 + i] = (uint8_t)(i & 0xFFu);
    }
}

static void test_parse_canonical(void)
{
    printf("rtp guard: canonical header parses\n");
    build_canonical(buf, MICHI_RTP_GUARD_PAYLOAD_BYTES);
    michi_rtp_guard_packet_t pkt;
    CHECK(michi_rtp_guard_parse(buf, 12 + MICHI_RTP_GUARD_PAYLOAD_BYTES, &pkt),
          "canonical datagram parses");
    CHECK(pkt.pt == MICHI_RTP_GUARD_PT_S16LE, "PT parsed");
    CHECK(pkt.seq == 1, "seq parsed");
    CHECK(pkt.ssrc == SSRC, "ssrc parsed");
    CHECK(pkt.payload_len == MICHI_RTP_GUARD_PAYLOAD_BYTES, "payload size");
    CHECK(pkt.payload == buf + 12, "payload points past the header");
}

static void test_parse_rejects(void)
{
    printf("rtp guard: header rejects v1/CSRC/extension/padding/short\n");
    michi_rtp_guard_packet_t pkt;

    build_canonical(buf, MICHI_RTP_GUARD_PAYLOAD_BYTES);
    buf[0] = 0x00; /* v0 */
    CHECK(!michi_rtp_guard_parse(buf, 12 + MICHI_RTP_GUARD_PAYLOAD_BYTES, &pkt),
          "v0 rejected");

    build_canonical(buf, MICHI_RTP_GUARD_PAYLOAD_BYTES);
    buf[0] = 0x81; /* v2 + 1 CSRC (24 extra bytes after the header) */
    CHECK(!michi_rtp_guard_parse(buf, 12 + 4 + MICHI_RTP_GUARD_PAYLOAD_BYTES, &pkt),
          "CSRC rejected");

    build_canonical(buf, MICHI_RTP_GUARD_PAYLOAD_BYTES);
    buf[0] = 0x90; /* v2 + extension bit */
    CHECK(!michi_rtp_guard_parse(buf, 12 + 4 + MICHI_RTP_GUARD_PAYLOAD_BYTES, &pkt),
          "extension rejected");

    build_canonical(buf, MICHI_RTP_GUARD_PAYLOAD_BYTES);
    buf[0] = 0xA0; /* v2 + padding bit */
    CHECK(!michi_rtp_guard_parse(buf, 12 + MICHI_RTP_GUARD_PAYLOAD_BYTES, &pkt),
          "padding rejected");

    CHECK(!michi_rtp_guard_parse(buf, 11, &pkt), "short datagram rejected");
    CHECK(!michi_rtp_guard_parse(NULL, 12, &pkt), "NULL buffer rejected");
}

static void test_classify(void)
{
    printf("rtp guard: classification per class\n");
    michi_rtp_guard_packet_t pkt;
    const michi_rtp_guard_session_t session = {
        .pt = MICHI_RTP_GUARD_PT_S16LE,
        .ssrc = SSRC,
        .source_be32 = 0x0A000001u, /* 10.0.0.1 in network byte order */
    };
    const uint32_t peer = 0x0A000001u;
    const uint32_t other = 0x0A000002u;

    build_canonical(buf, MICHI_RTP_GUARD_PAYLOAD_BYTES);
    CHECK(michi_rtp_guard_parse(buf, 12 + MICHI_RTP_GUARD_PAYLOAD_BYTES, &pkt),
          "precondition parse");
    CHECK(michi_rtp_guard_classify(&pkt, &session, peer) ==
              MICHI_RTP_GUARD_OK,
          "canonical packet accepted");

    /* PT: a PT 10 packet must be rejected (the legacy default is gone). */
    build_canonical(buf, MICHI_RTP_GUARD_PAYLOAD_BYTES);
    buf[1] = 10;
    CHECK(michi_rtp_guard_parse(buf, 12 + MICHI_RTP_GUARD_PAYLOAD_BYTES, &pkt) &&
              michi_rtp_guard_classify(&pkt, &session, peer) ==
                  MICHI_RTP_GUARD_PT_MISMATCH,
          "PT 10 rejected");

    /* SSRC: the negotiated SSRC only - no first-packet-wins. */
    build_canonical(buf, MICHI_RTP_GUARD_PAYLOAD_BYTES);
    buf[8] ^= 0xFFu; /* corrupt SSRC */
    CHECK(michi_rtp_guard_parse(buf, 12 + MICHI_RTP_GUARD_PAYLOAD_BYTES, &pkt) &&
              michi_rtp_guard_classify(&pkt, &session, peer) ==
                  MICHI_RTP_GUARD_SSRC_MISMATCH,
          "SSRC mismatch rejected");

    /* Source IP: exact HTTP peer only. */
    build_canonical(buf, MICHI_RTP_GUARD_PAYLOAD_BYTES);
    CHECK(michi_rtp_guard_parse(buf, 12 + MICHI_RTP_GUARD_PAYLOAD_BYTES, &pkt) &&
              michi_rtp_guard_classify(&pkt, &session, other) ==
                  MICHI_RTP_GUARD_SOURCE_MISMATCH,
          "source IP mismatch rejected");

    /* Size: 1919 / 1921 / 0 bytes are all wrong. */
    build_canonical(buf, MICHI_RTP_GUARD_PAYLOAD_BYTES - 1);
    CHECK(michi_rtp_guard_parse(buf, 12 + MICHI_RTP_GUARD_PAYLOAD_BYTES - 1, &pkt) &&
              michi_rtp_guard_classify(&pkt, &session, peer) ==
                  MICHI_RTP_GUARD_SIZE_MISMATCH,
          "1919-byte payload rejected");
    build_canonical(buf, MICHI_RTP_GUARD_PAYLOAD_BYTES + 1);
    CHECK(michi_rtp_guard_parse(buf, 12 + MICHI_RTP_GUARD_PAYLOAD_BYTES + 1, &pkt) &&
              michi_rtp_guard_classify(&pkt, &session, peer) ==
                  MICHI_RTP_GUARD_SIZE_MISMATCH,
          "1921-byte payload rejected");
    CHECK(michi_rtp_guard_parse(buf, 12, &pkt) &&
              michi_rtp_guard_classify(&pkt, &session, peer) ==
                  MICHI_RTP_GUARD_SIZE_MISMATCH,
          "empty payload rejected");
}

static void test_lost_delta_wrap(void)
{
    printf("rtp guard: sequence wrap loss accounting\n");
    CHECK(michi_rtp_guard_lost_delta(100, 100) == 0, "same seq: no loss");
    CHECK(michi_rtp_guard_lost_delta(100, 101) == 0, "next seq: no loss");
    CHECK(michi_rtp_guard_lost_delta(100, 103) == 2, "gap of 3: 2 lost");
    CHECK(michi_rtp_guard_lost_delta(100, 99) == 0, "reorder: no loss");
    CHECK(michi_rtp_guard_lost_delta(100, 5) == 0, "far reorder: no loss");
    /* Wrap: 65535 -> 0 is the NEXT packet (16-bit wrap), no loss. */
    CHECK(michi_rtp_guard_lost_delta(65535, 0) == 0, "wrap 65535->0: no loss");
    CHECK(michi_rtp_guard_lost_delta(65534, 0) == 1, "wrap 65534->0: 1 lost");
    CHECK(michi_rtp_guard_lost_delta(65534, 1) == 2, "wrap 65534->1: 2 lost");
    CHECK(michi_rtp_guard_lost_delta(65533, 0) == 2, "wrap 65533->0: 2 lost");
    CHECK(michi_rtp_guard_lost_delta(0, 65535) == 0, "backward wrap: reorder");
}

int main(void)
{
    test_parse_canonical();
    test_parse_rejects();
    test_classify();
    test_lost_delta_wrap();
    if (failures != 0) {
        printf("rtp_guard: %d FAILURE(S)\n", failures);
        return 1;
    }
    printf("rtp_guard: all tests passed\n");
    return 0;
}
