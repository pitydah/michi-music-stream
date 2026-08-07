#!/usr/bin/env python3
"""Behavior scenarios for the Michi Music Stream SIMULATOR (F15).

These are SIMULATOR scenarios, not firmware tests: the simulator models
the firmware's behavior (wifi reconnect backoff, RTP jitter buffer
handling with metrics and silence, pairing window expiry, OTA lifecycle)
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
    WIFI_BACKOFF_BASE_S,
    WIFI_BACKOFF_MAX_S,
    RtpJitterBufferModel,
    OtaModel,
)


def std_state():
    return SimulatorState(STANDARD_CONFIG)


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


# ── pairing window expiry ───────────────────────────────────

def test_pairing_window_expired_closes():
    s = std_state()
    s.pair_start("micro_001")
    assert s.window_open is True

    # Expire the window (the sim has no background timer; expiry is
    # evaluated on first contact). 121 > 120: the window is 120s
    # (receiver_sim.py), so 121 forces expiry.
    s.window_expires = s.window_expires - 121
    code, body = s.pair_confirm(s.current_nonce, "micro_001", "tok_x")
    assert code == 409
    assert body["error"]["code"] == "pairing_window_closed"
    assert s.window_open is False
    assert s.current_nonce == ""
    print("PASS pairing window expired -> closed")


def test_pairing_window_valid_before_expiry():
    s = std_state()
    s.pair_start("micro_001")
    code, body = s.pair_confirm(s.current_nonce, "micro_001", "tok_x")
    assert code == 200 and body["status"] == "paired"
    print("PASS pairing window confirm before expiry")


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
        test_pairing_window_expired_closes,
        test_pairing_window_valid_before_expiry,
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


sys.exit(0 if run() else 1)
