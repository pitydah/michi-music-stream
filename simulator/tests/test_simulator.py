#!/usr/bin/env python3
"""Tests for Michi Music Stream Simulator."""

import json
import os
import sys
import time

# Add parent to path so we can import receiver_sim
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from receiver_sim import SimulatorState, STANDARD_CONFIG, HIFI_CONFIG, create_app


# ── Fixtures ────────────────────────────────────────────────

def std_state():
    return SimulatorState(STANDARD_CONFIG)

def hifi_state():
    return SimulatorState(HIFI_CONFIG)

def paired_state():
    s = std_state()
    s.pair_start("micro_001")
    s.pair_confirm(s.current_nonce, "micro_001", "tok_test")
    return s, "tok_test"

def session_state():
    s, token = paired_state()
    s.session_start("sess_test", "pcm_s16le", 48000, 16, 2, 55300, 250, 70)
    return s, token


# ── receiver/info ───────────────────────────────────────────

def test_info_standard():
    s = std_state()
    info = s.info()
    assert info["service"] == "michi-stream-standard"
    assert info["type"] == "michi_stream_standard"
    assert info["output"]["max_sample_rate"] == 48000
    assert info["output"]["max_bit_depth"] == 16
    assert "pcm_s16le" in info["supported_codecs"]
    assert "pcm_s16le" in info["supported_codecs"]
    assert "pcm_s24le" not in info["supported_codecs"]
    assert info["features"]["ota_update"] == False
    assert info["auth"]["strategy"] == "RECEIVER_BUTTON"
    print("PASS info standard")

def test_info_hifi():
    s = hifi_state()
    info = s.info()
    assert info["service"] == "michi-stream-hifi"
    assert info["type"] == "michi_stream_hifi"
    assert info["output"]["max_sample_rate"] == 96000
    assert info["output"]["max_bit_depth"] == 24
    assert "pcm_s24le" in info["supported_codecs"]
    assert info["output"]["dac"] == "hifi_i2s"
    assert info["features"]["ota_update"] == True
    print("PASS info hifi")


# ── Pairing ─────────────────────────────────────────────────

def test_pair_open():
    s = std_state()
    code, resp = s.pair_start("micro_001")
    assert code == 200
    assert resp["status"] == "pairing_window_open"
    assert resp["pairing_window_seconds"] == 120
    assert len(resp["nonce"]) > 0
    assert s.window_open is True
    print("PASS pair open")

def test_pair_rejects_second_open():
    s = std_state()
    s.pair_start("micro_001")
    code, resp = s.pair_start("micro_002")
    assert code == 409
    assert resp["error"]["code"] == "pairing_window_open"
    print("PASS pair rejects second open")

def test_pair_confirm():
    s = std_state()
    s.pair_start("micro_001")
    code, resp = s.pair_confirm(s.current_nonce, "micro_001", "tok_test")
    assert code == 200
    assert resp["status"] == "paired"
    assert resp["token"] == "tok_test"
    assert "micro_001" in s.controllers
    assert s.window_open is False
    print("PASS pair confirm")

def test_pair_rejects_closed():
    s = std_state()
    code, resp = s.pair_confirm("bad_nonce", "micro_001", "tok_test")
    assert code == 409
    assert resp["error"]["code"] == "pairing_window_closed"
    print("PASS pair rejects closed")

def test_pair_rejects_bad_nonce():
    s = std_state()
    s.pair_start("micro_001")
    code, resp = s.pair_confirm("wrong_nonce", "micro_001", "tok_test")
    assert code == 409
    print("PASS pair rejects bad nonce")

def test_validate_token():
    s = std_state()
    s.pair_start("m1")
    s.pair_confirm(s.current_nonce, "m1", "tok_good")
    assert s.validate_token("tok_good") is True
    assert s.validate_token("tok_bad") is False
    print("PASS validate token")


# ── Session ─────────────────────────────────────────────────

def test_session_start_valid():
    s, _ = paired_state()
    code, resp = s.session_start("sess_001", "pcm_s16le", 48000, 16, 2, 55300, 250, 70)
    assert code == 200
    assert resp["status"] == "session_started"
    assert s.session_active is True
    print("PASS session start valid")

def test_session_rejects_duplicate():
    s, _ = session_state()
    code, resp = s.session_start("sess_002", "pcm_s16le", 48000, 16, 2, 55300, 250, 70)
    assert code == 409
    assert resp["error"]["code"] == "session_active"
    print("PASS session rejects duplicate")

def test_session_invalid_codec():
    s, _ = paired_state()
    code, resp = s.session_start("sess_003", "mp3", 48000, 16, 2, 55300, 250, 70)
    assert code == 400
    assert resp["error"]["code"] == "unsupported_codec"
    print("PASS session invalid codec")

def test_session_invalid_rate():
    s, _ = paired_state()
    code, resp = s.session_start("sess_004", "pcm_s16le", 96000, 16, 2, 55300, 250, 70)
    assert code == 400
    assert resp["error"]["code"] == "unsupported_rate"
    print("PASS session invalid rate")

def test_session_invalid_depth():
    s, _ = paired_state()
    code, resp = s.session_start("sess_005", "pcm_s16le", 48000, 32, 2, 55300, 250, 70)
    assert code == 400
    print("PASS session invalid depth")

def test_session_invalid_channels():
    s, _ = paired_state()
    code, resp = s.session_start("sess_006", "pcm_s16le", 48000, 16, 1, 55300, 250, 70)
    assert code == 400
    print("PASS session invalid channels")

def test_session_stop():
    s, _ = session_state()
    code, resp = s.session_stop()
    assert code == 200
    assert resp["status"] == "session_stopped"
    assert s.session_active is False
    print("PASS session stop")


# ── Heartbeat ───────────────────────────────────────────────

def test_heartbeat():
    s, _ = session_state()
    code, resp = s.heartbeat()
    assert code == 200
    assert resp["status"] == "alive"
    assert resp["uptime_seconds"] >= 0
    print("PASS heartbeat")

def test_heartbeat_no_session():
    s = std_state()
    code, resp = s.heartbeat()
    assert code == 409
    print("PASS heartbeat no session")


# ── Volume ──────────────────────────────────────────────────

def test_volume():
    s = std_state()
    code, resp = s.set_volume(50)
    assert code == 200
    assert resp["volume"] == 50
    assert s.volume == 50
    print("PASS volume")

def test_volume_clamp():
    s = std_state()
    s.set_volume(-10)
    assert s.volume == 0
    s.set_volume(150)
    assert s.volume == 100
    print("PASS volume clamp")


# ── Auth ────────────────────────────────────────────────────

def test_auth_rejects_invalid():
    s, _ = paired_state()
    assert s.validate_token("wrong_token") is False
    print("PASS auth rejects invalid")


# ── Runner ──────────────────────────────────────────────────

def run():
    tests = [
        test_info_standard,
        test_info_hifi,
        test_pair_open,
        test_pair_rejects_second_open,
        test_pair_confirm,
        test_pair_rejects_closed,
        test_pair_rejects_bad_nonce,
        test_validate_token,
        test_session_start_valid,
        test_session_rejects_duplicate,
        test_session_invalid_codec,
        test_session_invalid_rate,
        test_session_invalid_depth,
        test_session_invalid_channels,
        test_session_stop,
        test_heartbeat,
        test_heartbeat_no_session,
        test_volume,
        test_volume_clamp,
        test_auth_rejects_invalid,
    ]
    ok = 0
    for t in tests:
        try:
            t()
            ok += 1
        except Exception as e:
            print(f"FAIL {t.__name__}: {e}")
    print(f"\n{ok}/{len(tests)} simulator tests passed")
    return ok == len(tests)

if __name__ == "__main__":
    sys.exit(0 if run() else 1)
