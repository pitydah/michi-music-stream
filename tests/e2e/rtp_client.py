"""Canonical RTP packet builder/parser for the MS-09 E2E client.

RTP v2, no CSRC, no extension, no padding; payload type 97; 1920-byte
payload (10 ms at 48 kHz / 16-bit / stereo = 480 frames = 960 samples,
PCM S16LE interleaved L/R).
"""

import struct

RTP_HEADER_BYTES = 12
RTP_PAYLOAD_TYPE = 97
RTP_PAYLOAD_BYTES = 1920
RTP_PACKET_BYTES = RTP_HEADER_BYTES + RTP_PAYLOAD_BYTES


def build_packet(seq, timestamp, ssrc, payload):
    header = struct.pack(
        "!BBHII",
        0x80,
        RTP_PAYLOAD_TYPE,
        seq & 0xFFFF,
        timestamp & 0xFFFFFFFF,
        ssrc & 0xFFFFFFFF,
    )
    return header + payload


def pcm10ms_payload(seed=0):
    return bytes(((i * 131 + seed) & 0xFF) for i in range(RTP_PAYLOAD_BYTES))


def parse_packet(datagram):
    if len(datagram) < RTP_HEADER_BYTES:
        raise ValueError("datagram shorter than the RTP header")
    version_byte, pt, seq, timestamp, ssrc = struct.unpack(
        "!BBHII", datagram[:RTP_HEADER_BYTES]
    )
    return {
        "version_byte": version_byte,
        "pt": pt,
        "seq": seq,
        "timestamp": timestamp,
        "ssrc": ssrc,
        "payload_len": len(datagram) - RTP_HEADER_BYTES,
    }
