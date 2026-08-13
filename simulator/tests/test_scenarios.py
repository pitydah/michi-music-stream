#!/usr/bin/env python3
"""Behavior scenarios for the Michi Music Stream SIMULATOR (F15).

These are SIMULATOR scenarios, not firmware tests: the simulator models
the firmware's behavior (wifi reconnect backoff, RTP jitter buffer
handling with metrics and silence, pairing window expiry, session lease)
so the scenarios run in CI on the host. The REAL firmware is validated
on HARDWARE (see firmware/README.md, Testing & CI).

Run: python3 simulator/tests/test_scenarios.py
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from receiver_sim import (
    SimulatorState,
    STANDARD_CONFIG,
    CONTROLLER_IDENTITY,
    WIFI_BACKOFF_BASE_S,
    WIFI_BACKOFF_MAX_S,
    RtpJitterBufferModel,
    OtaModel,
)


class FakeClock:
    def __init__(self, start=1000.0):
        self.t = start

    def __call__(self):
        return self.t

    def advance(self, seconds):
        self.t += seconds


def std_state(clock=None):
    return SimulatorState(STANDARD_CONFIG, mono_clock=clock or FakeClock())


# ── wifi loss -> reconnect with backoff ─────────────────────

def test_wifi_reconnect_backoff():
    s = std_state()
    assert s.wifi_connected is True

    s.wifi_link_loss()
    assert s.wifi_connected is False
    assert s.wifi_next_backoff() == WIFI_BACKOFF_BASE_S          # 5 s
    assert s.wifi_next_backoff() == WIFI_BACKOFF_BASE_S * 2      # 10 s
    assert s.wifi_next_backoff() == WIFI_BACKOFF_BASE_S * 4      # 20 s

    # Backoff is capped (firmware: arm_reconnect cap).
    for _ in range(10):
        s.wifi_next_backoff()
    assert s.wifi_backoff_s == WIFI_BACKOFF_MAX_S

    # Reconnect resets the chain (firmware: STATE_CHANGED re-arm).
    s.wifi_reconnect_ok()
    assert s.wifi_connected is True
    assert s.wifi_retry == 0
    assert s.wifi_reconnects == 1
    assert s.wifi_next_backoff() == 0.0  # link up: no backoff

    # Loss after reconnect restarts from the base (not accumulated).
    s.wifi_link_loss()
    assert s.wifi_next_backoff() == WIFI_BACKOFF_BASE_S
    print("PASS wifi reconnect backoff")


# ── RTP loss / reorder / duplicates -> metrics + silence ───

def test_rtp_loss_plays_silence():
    jb = RtpJitterBufferModel()
    assert jb.push(1) == "played"
    assert jb.push(2) == "played"
    assert jb.push(3) == "played"
    # seq 5 arrives: seq 4 is lost -> silence fills the gap.
    assert jb.push(5) == "silence"
    assert jb.push(6) == "played"

    m = jb.metrics()
    assert m["lost"] == 1
    assert m["silence_ms"] == 20.0
    assert m["received"] == 5 and m["played"] == 5
    print("PASS rtp loss -> silence + metrics")


def test_rtp_reorder():
    jb = RtpJitterBufferModel()
    assert jb.push(1) == "played"
    assert jb.push(2) == "played"
    # 4 arrives before 3: gap -> silence, then 3 is late (reordered).
    assert jb.push(4) == "silence"
    assert jb.push(3) == "reordered"
    m = jb.metrics()
    assert m["reordered"] == 1
    assert m["lost"] == 1
    assert m["silence_ms"] == 20.0
    print("PASS rtp reorder -> reordered metric")


def test_rtp_duplicates():
    jb = RtpJitterBufferModel()
    assert jb.push(1) == "played"
    assert jb.push(2) == "played"
    assert jb.push(2) == "duplicate"  # repeat of a seen seq
    assert jb.push(2) == "duplicate"
    m = jb.metrics()
    assert m["duplicate"] == 2
    assert m["played"] == 2
    print("PASS rtp duplicate -> duplicate metric")


def test_rtp_metrics_consistency():
    # Classification invariant: every received packet is played,
    # duplicated or reordered (lost is NOT a received packet - it is
    # silence emitted for a gap).
    jb = RtpJitterBufferModel()
    for seq in (1, 2, 3, 5, 4, 4, 7, 9):
        jb.push(seq)
    m = jb.metrics()
    assert m["received"] == m["played"] + m["duplicate"] + m["reordered"]

    # Clean stream: no gaps -> no loss, no silence.
    clean = RtpJitterBufferModel()
    for seq in range(1, 11):
        assert clean.push(seq) == "played"
    cm = clean.metrics()
    assert cm["lost"] == 0 and cm["silence_ms"] == 0.0
    assert cm["received"] == cm["played"] == 10

    # Single gap: 6 and 7 never arrive -> 2 lost slots, 40 ms silence.
    gap = RtpJitterBufferModel()
    for seq in (1, 2, 3, 4, 5, 8, 9, 10):
        gap.push(seq)
    gm = gap.metrics()
    assert gm["lost"] == 2 and gm["silence_ms"] == 40.0
    assert gm["played"] == 8

    # Lost slots must never be counted as received.
    assert m["lost"] == 3  # 5-vs-4 gap, 7-vs-6 gap, 9-vs-8 gap
    assert m["silence_ms"] == m["lost"] * 20.0
    print("PASS rtp metrics consistency")


# ── pairing window expiry (canonical v1-lite) ───────────────

def test_pairing_window_expired_status_expired():
    clock = FakeClock()
    s = std_state(clock)
    s.open_pairing_window()
    _, started = s.pairing_start(CONTROLLER_IDENTITY)
    sid = started["session_id"]
    assert s.window_open is True

    # Expire the window (the sim has no background task; expiry is
    # evaluated on first contact). 121 > 120.
    clock.advance(121)
    code, status = s.pairing_status(sid)
    assert code == 200
    assert status["status"] == "expired"

    # A new pair/start in the closed window is rejected on first contact.
    code3, body3 = s.pairing_start(CONTROLLER_IDENTITY)
    assert code3 == 403
    assert body3["error"]["code"] == "FORBIDDEN"
    assert s.window_open is False

    code2, body2 = s.pairing_confirm(
        sid, "000000", CONTROLLER_IDENTITY["michi_id"], CONTROLLER_IDENTITY["public_key"]
    )
    assert code2 == 404
    assert body2["error"]["code"] == "PAIRING_NOT_FOUND"
    print("PASS pairing window expired -> status expired")


def test_pairing_window_valid_before_expiry():
    clock = FakeClock()
    s = std_state(clock)
    s.open_pairing_window()
    _, started = s.pairing_start(CONTROLLER_IDENTITY)
    sid = started["session_id"]
    pin = s.pairing_sessions[sid]["pin"]
    clock.advance(60)
    code, body = s.pairing_confirm(
        sid, pin, CONTROLLER_IDENTITY["michi_id"], CONTROLLER_IDENTITY["public_key"]
    )
    assert code == 200
    assert body["expires_in"] == 0
    print("PASS pairing confirm before window expiry")


def test_session_lease_expiry_closes():
    clock = FakeClock()
    s = std_state(clock)
    s.open_pairing_window()
    _, started = s.pairing_start(CONTROLLER_IDENTITY)
    sid = started["session_id"]
    pin = s.pairing_sessions[sid]["pin"]
    s.pairing_confirm(sid, pin, CONTROLLER_IDENTITY["michi_id"], CONTROLLER_IDENTITY["public_key"])
    payload = {
        "transport": "rtp_udp",
        "codec": "pcm_s16le",
        "sample_rate": 48000,
        "bit_depth": 16,
        "channels": 2,
        "packet_ms": 10,
        "buffer_ms": 120,
        "payload_type": 97,
        "ssrc": 305419896,
        "volume": 70,
    }
    s.session_create(payload, source_ip="127.0.0.1")
    clock.advance(31)
    code, body = s.session_state()
    assert code == 404
    assert s.session_id is None
    assert s.lease_expirations == 1
    print("PASS session lease expiry closes")


# ── OTA lifecycle ───────────────────────────────────────────

def test_ota_failed_state():
    ota = OtaModel()
    ok, _ = ota.start("https://example.test/michi/manifest.json")
    assert ok is True
    assert ota.state == "fetching"
    assert ota.download(50) is True
    assert ota.state == "downloading" and ota.percent == 50

    ota.fail("signature verification failed")
    assert ota.state == "failed"
    assert ota.error == "signature verification failed"

    # A failed OTA is terminal: no further progress, no restart.
    assert ota.download(90) is False
    ok2, reason = ota.start("https://example.test/again.json")
    assert ok2 is False and reason == "busy"
    print("PASS ota failed -> state failed")


def test_ota_success_lifecycle():
    ota = OtaModel()
    ok, _ = ota.start("https://example.test/manifest.json")
    assert ok is True
    assert ota.state == "fetching"
    for pct in (10, 30, 85):
        assert ota.download(pct) is True
    ota.finish()
    assert ota.state == "done" and ota.percent == 100
    print("PASS ota success lifecycle")


# ── runner ──────────────────────────────────────────────────

def run():
    tests = [
        test_wifi_reconnect_backoff,
        test_rtp_loss_plays_silence,
        test_rtp_reorder,
        test_rtp_duplicates,
        test_rtp_metrics_consistency,
        test_pairing_window_expired_status_expired,
        test_pairing_window_valid_before_expiry,
        test_session_lease_expiry_closes,
        test_ota_failed_state,
        test_ota_success_lifecycle,
    ]
    ok = 0
    for t in tests:
        try:
            t()
            ok += 1
        except Exception as e:
            print(f"  FAIL {t.__name__}: {e}")
    print(f"\n{ok}/{len(tests)} simulator scenarios passed")
    return ok == len(tests)


if __name__ == "__main__":
    sys.exit(0 if run() else 1)
