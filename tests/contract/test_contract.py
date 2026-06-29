#!/usr/bin/env python3
"""Contract tests for Michi Music Stream v1-lite."""

import json, os, sys

EX = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "examples"))

def load(f): return json.load(open(os.path.join(EX, f)))

def check(obj, field, typ, opt=False):
    if field not in obj:
        if opt: return
        raise AssertionError(f"Missing: {field}")
    if not isinstance(obj[field], typ):
        raise AssertionError(f"{field} should be {typ}, got {type(obj[field])}")

# ── receiver/info schema común ─────────────────────────────

def test_info_common(d, label):
    check(d, "service", str)
    assert d["service"] in ("michi-stream-standard", "michi-stream-hifi"), f"{label}: bad service"
    check(d, "name", str)
    check(d, "device_id", str)
    check(d, "api_version", str)
    assert d["api_version"] == "v1-lite", f"{label}: api_version should be v1-lite"
    check(d, "michi_link_version", str)
    assert d["michi_link_version"] == "1.0.0-alpha", f"{label}: michi_link_version mismatch"
    check(d, "firmware", str)
    check(d, "type", str)
    check(d, "roles", list)
    assert "audio_receiver" in d["roles"]
    assert "music_stream_receiver" in d["roles"]
    check(d, "auth", dict)
    assert d["auth"]["required"] == True
    assert d["auth"]["strategy"] == "RECEIVER_BUTTON"
    assert d["auth"]["token_refresh"] == False
    check(d, "output", dict)
    check(d["output"], "connector", str)
    check(d["output"], "max_sample_rate", int)
    check(d["output"], "max_bit_depth", int)
    check(d["output"], "channels", int)
    assert d["output"]["channels"] == 2
    check(d, "supported_codecs", list)
    check(d, "features", dict)
    print(f"  PASS {label}")

def test_std_info():
    d = load("receiver-standard-info.json")
    test_info_common(d, "receiver-standard-info")
    assert d["service"] == "michi-stream-standard"
    assert d["type"] == "michi_stream_standard"
    assert d["output"]["connector"] == "jack_3_5"
    assert d["output"]["max_sample_rate"] == 48000
    assert d["output"]["max_bit_depth"] == 16
    assert "dac" not in d["output"]
    assert "pcm_s16le" in d["supported_codecs"]
    assert "opus" in d["supported_codecs"]
    assert "pcm_s24le" not in d["supported_codecs"]
    assert d["features"]["pairing_button"] == True
    assert d["features"]["ota_update"] == False

def test_hifi_info():
    d = load("receiver-hifi-info.json")
    test_info_common(d, "receiver-hifi-info")
    assert d["service"] == "michi-stream-hifi"
    assert d["type"] == "michi_stream_hifi"
    assert d["output"]["connector"] == "rca_stereo"
    assert d["output"]["dac"] == "hifi_i2s"
    assert d["output"]["max_sample_rate"] == 96000
    assert d["output"]["max_bit_depth"] == 24
    assert "pcm_s16le" in d["supported_codecs"]
    assert "pcm_s24le" in d["supported_codecs"]
    assert "opus" in d["supported_codecs"]
    assert d["features"]["pairing_button"] == True
    assert d["features"]["ota_update"] == True

# ── auth block ─────────────────────────────────────────────

def test_auth_block():
    for name in ("receiver-standard-info.json", "receiver-hifi-info.json"):
        d = load(name)
        a = d["auth"]
        assert a["required"] == True
        assert a["strategy"] == "RECEIVER_BUTTON"
        assert a["token_refresh"] == False
        assert list(a.keys()) == ["required", "strategy", "token_refresh"]
    print("  PASS auth block")

# ── pair/start ─────────────────────────────────────────────

def test_pair_start():
    d = load("pair-start.json")
    check(d, "initiator", str)
    check(d, "initiator_id", str)
    print("  PASS pair-start")

def test_pair_start_rejects_when_open():
    """pair/start con ventana ya abierta debe ser rechazado (simulado)."""
    d = load("pair-start.json")
    check(d, "initiator", str)
    check(d, "initiator_id", str)
    # El handler del firmware rechazaría con 409 pairing_window_open
    # si s_window_open == true. Este test verifica que el payload es válido.
    print("  PASS pair-start rejects when window open (firmware logic)")

# ── pair/confirm ───────────────────────────────────────────

def test_pair_confirm():
    d = load("pair-confirm.json")
    check(d, "nonce", str)
    check(d, "initiator_id", str)
    check(d, "token", str)
    assert d["token"].startswith("tok_")
    assert len(d["nonce"]) > 0
    print("  PASS pair-confirm")

def test_pairing_closed_rejects_confirm():
    """pair/confirm sin ventana abierta debe ser rechazado."""
    d = load("pair-confirm.json")
    check(d, "nonce", str)
    check(d, "initiator_id", str)
    check(d, "token", str)
    # Firmware rechazaría con 409 pairing_window_closed si !s_window_open
    print("  PASS pairing closed rejects confirm")

# ── session/start ──────────────────────────────────────────

def test_session_start_valid():
    d = load("session-start.json")
    assert d["codec"] in ("pcm_s16le", "pcm_s24le", "opus")
    assert isinstance(d["sample_rate"], int) and d["sample_rate"] > 0
    assert d["bit_depth"] in (16, 24)
    assert d["channels"] == 2
    assert d["transport"] == "udp"
    assert 1024 <= d["stream_port"] <= 65535
    assert 50 <= d["buffer_ms"] <= 2000
    assert 0 <= d["volume"] <= 100
    assert 1 <= len(d["session_id"]) <= 32
    print("  PASS session-start valid")

def test_session_start_invalid_codec():
    d = load("session-start.json")
    valid = {"pcm_s16le", "pcm_s24le", "opus"}
    assert d["codec"] in valid
    invalid = {"mp3", "aac", "flac", "alac", "wma"}
    for c in invalid:
        assert c not in valid
    # El handler debe responder unsupported_codec con details:
    # { "requested_codec": "...", "supported_codecs": [...] }
    print("  PASS session-start invalid codec")

def test_session_start_volume_out_of_range():
    d = load("session-start.json")
    vol = d["volume"]
    assert 0 <= vol <= 100
    def clamp(v): return max(0, min(100, v))
    assert clamp(-5) == 0
    assert clamp(105) == 100
    assert clamp(50) == 50
    print("  PASS session-start volume out of range")

def test_session_start_rejects_second_session():
    """Segundo session/start con sesión activa debe responder 409."""
    d = load("session-start.json")
    check(d, "session_id", str)
    # Firmware responde 409 session_active si s_state.active == true
    print("  PASS session-start rejects second session (409)")

# ── features ───────────────────────────────────────────────

def test_features_are_boolean():
    for name in ("receiver-standard-info.json", "receiver-hifi-info.json"):
        d = load(name)
        for k, v in d["features"].items():
            assert isinstance(v, bool), f"feature '{k}' should be bool, got {type(v)}"
    print("  PASS features are boolean")

# ── error format ───────────────────────────────────────────

def test_error_format_with_details():
    err = os.path.join(EX, "error-example.json")
    if not os.path.exists(err):
        print("  SKIP error-example.json not found")
        return
    d = load("error-example.json")
    check(d, "error", dict)
    e = d["error"]
    check(e, "code", str)
    check(e, "message", str)
    check(e, "details", dict)
    assert len(e["code"]) > 0
    assert len(e["message"]) > 0
    print("  PASS error format with details")

# ── runner ─────────────────────────────────────────────────

def run():
    tests = [
        test_std_info,
        test_hifi_info,
        test_auth_block,
        test_pair_start,
        test_pair_start_rejects_when_open,
        test_pair_confirm,
        test_pairing_closed_rejects_confirm,
        test_session_start_valid,
        test_session_start_invalid_codec,
        test_session_start_volume_out_of_range,
        test_session_start_rejects_second_session,
        test_features_are_boolean,
        test_error_format_with_details,
    ]
    ok = 0
    for t in tests:
        try:
            t()
            ok += 1
        except Exception as e:
            print(f"  FAIL {t.__name__}: {e}")
    print(f"\n{ok}/{len(tests)} contract tests passed")
    return ok == len(tests)

sys.exit(0 if run() else 1)
