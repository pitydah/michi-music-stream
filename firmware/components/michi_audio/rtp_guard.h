#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Canonical RTP session guard (MS-07, contract section 2.5).
 *
 * The single validation module for every datagram the engine accepts.
 * The canonical session is EXACT: RTP v2 with no CSRC, no extension and
 * no padding; payload type 97; the SSRC negotiated at session start (no
 * first-packet-wins); the exact IPv4 source of the HTTP request; and a
 * 1920-byte payload (10 ms at 48 kHz / 16-bit / stereo = 480 frames =
 * 960 samples, PCM S16LE interleaved L/R).
 *
 * Pure C (no ESP-IDF, no lwip): compiled by the firmware engine
 * (michi_audio) AND by the host-side tests (tests/host) so packet
 * validation is proven by the SAME source the device runs.
 */

/*!< Canonical RTP payload type (contract section 2.5; not a Kconfig
 * knob - a packet with any other PT is rejected and counted). */
#define MICHI_RTP_GUARD_PT_S16LE 97
/*!< Canonical RTP version. */
#define MICHI_RTP_GUARD_VERSION 2
/*!< Canonical payload size: 10 ms @ 48 kHz / 16-bit / stereo = 1920 B. */
#define MICHI_RTP_GUARD_PAYLOAD_BYTES 1920

/**
 * @brief Per-class rejection verdict for a parsed datagram.
 *
 * The engine maps every verdict to its metric class (packets_rejected =
 * the sum of all classes) and keeps the session alive - a bad packet
 * never closes the session.
 */
typedef enum {
    MICHI_RTP_GUARD_OK = 0,          /*!< Accept: play/queue it */
    MICHI_RTP_GUARD_MALFORMED,       /*!< Not RTP v2, CSRC, extension, padding or truncated */
    MICHI_RTP_GUARD_PT_MISMATCH,     /*!< Payload type != 97 */
    MICHI_RTP_GUARD_SSRC_MISMATCH,   /*!< SSRC != the negotiated value */
    MICHI_RTP_GUARD_SOURCE_MISMATCH, /*!< IPv4 source != the HTTP request peer */
    MICHI_RTP_GUARD_SIZE_MISMATCH,   /*!< Payload != 1920 bytes */
} michi_rtp_guard_verdict_t;

/**
 * @brief Parsed canonical RTP header (RFC 3550 fixed 12 bytes).
 */
typedef struct {
    uint8_t        pt;        /*!< Payload type (low 7 bits) */
    uint16_t       seq;       /*!< Sequence number (16-bit, wraps) */
    uint32_t       timestamp; /*!< RTP timestamp (32-bit) */
    uint32_t       ssrc;      /*!< Synchronization source */
    const uint8_t *payload;   /*!< First payload byte (buf + 12) */
    uint16_t       payload_len; /*!< Payload bytes */
} michi_rtp_guard_packet_t;

/**
 * @brief Strict RTP header parse for the canonical session.
 *
 * Accepts EXACTLY: version 2, no padding bit, no extension bit, no
 * CSRCs, at least 12 bytes. Anything else (including padding/extension
 * the legacy parser skipped or trimmed) is malformed: the canonical
 * contract accepts none of them.
 *
 * @param buf Datagram bytes.
 * @param len Datagram length.
 * @param out Parsed header (payload points INTO buf).
 * @return true when the header is canonical.
 */
bool michi_rtp_guard_parse(const uint8_t *buf, size_t len,
                           michi_rtp_guard_packet_t *out);

/**
 * @brief Negotiated session constants the guard validates against.
 *
 * Built once at session start from the canonical negotiation (HTTP
 * request): PT 97, the requested SSRC (never 0 - no first-packet-wins)
 * and the HTTP request peer IPv4 in NETWORK byte order.
 */
typedef struct {
    uint8_t  pt;          /*!< Canonical payload type (97) */
    uint32_t ssrc;        /*!< Negotiated SSRC (1..4294967295) */
    uint32_t source_be32; /*!< HTTP peer IPv4, network byte order */
} michi_rtp_guard_session_t;

/**
 * @brief Classify a parsed datagram against the negotiated session.
 *
 * Check order: PT, SSRC, source address, payload size - the first
 * violation is the verdict. `sender_be32` is the datagram sender IPv4
 * in NETWORK byte order (the exact value stored in struct
 * in_addr::s_addr by recvfrom); it is NOT part of the RTP header, so
 * the caller provides it and the guard only compares.
 *
 * @param pkt Parsed header (michi_rtp_guard_parse must have succeeded).
 * @param session Negotiated session constants.
 * @param sender_be32 Actual sender address (network byte order).
 * @return The verdict.
 */
michi_rtp_guard_verdict_t michi_rtp_guard_classify(
    const michi_rtp_guard_packet_t *pkt,
    const michi_rtp_guard_session_t *session, uint32_t sender_be32);

/**
 * @brief Packets lost when the sequence advances from last_seq to seq.
 *
 * 16-bit wrap-safe (int16 diff math, RFC 3550 semantics): 0 for the
 * next expected packet, duplicates and reordered arrivals; (gap - 1)
 * for an in-order jump. The session NEVER closes on loss/reordering -
 * the caller only accumulates the packets_lost counter.
 *
 * @param last_seq Highest in-order sequence received so far.
 * @param seq Sequence of the packet being accepted.
 * @return Lost packets to add (0 for reorder/duplicate/next).
 */
uint32_t michi_rtp_guard_lost_delta(uint16_t last_seq, uint16_t seq);

#ifdef __cplusplus
}
#endif
