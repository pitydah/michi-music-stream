/* Canonical RTP session guard (MS-07): the single datagram validation
 * module shared by the firmware engine and the host-side tests. See
 * rtp_guard.h for the full contract. Pure C: no ESP-IDF, no lwip. */

#include "rtp_guard.h"

bool michi_rtp_guard_parse(const uint8_t *buf, size_t len,
                           michi_rtp_guard_packet_t *out)
{
    if (buf == NULL || out == NULL || len < 12) {
        return false;
    }
    /* First byte: bits 7-6 version (must be 2), bit 5 padding (must be
     * 0), bit 4 extension (must be 0), bits 3-0 CSRC count (must be 0).
     * The only accepted pattern is the exact byte 0x80 (v2, no padding,
     * no extension, no CSRC). */
    if (buf[0] != 0x80u) {
        return false;
    }
    out->pt = buf[1] & 0x7Fu;
    out->seq = ((uint16_t)buf[2] << 8) | buf[3];
    out->timestamp = ((uint32_t)buf[4] << 24) | ((uint32_t)buf[5] << 16) |
                     ((uint32_t)buf[6] << 8) | buf[7];
    out->ssrc = ((uint32_t)buf[8] << 24) | ((uint32_t)buf[9] << 16) |
                ((uint32_t)buf[10] << 8) | buf[11];
    out->payload = buf + 12;
    out->payload_len = (uint16_t)(len - 12);
    return true;
}

michi_rtp_guard_verdict_t michi_rtp_guard_classify(
    const michi_rtp_guard_packet_t *pkt,
    const michi_rtp_guard_session_t *session, uint32_t sender_be32)
{
    if (pkt == NULL || session == NULL) {
        return MICHI_RTP_GUARD_MALFORMED;
    }
    if (pkt->pt != session->pt) {
        return MICHI_RTP_GUARD_PT_MISMATCH;
    }
    if (pkt->ssrc != session->ssrc) {
        return MICHI_RTP_GUARD_SSRC_MISMATCH;
    }
    if (sender_be32 != session->source_be32) {
        return MICHI_RTP_GUARD_SOURCE_MISMATCH;
    }
    if (pkt->payload_len != MICHI_RTP_GUARD_PAYLOAD_BYTES) {
        return MICHI_RTP_GUARD_SIZE_MISMATCH;
    }
    return MICHI_RTP_GUARD_OK;
}

uint32_t michi_rtp_guard_lost_delta(uint16_t last_seq, uint16_t seq)
{
    const int16_t diff = (int16_t)(seq - last_seq);
    return (diff > 1) ? (uint32_t)(diff - 1) : 0;
}
