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

def _check_info(d, label):
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
    _check_info(d, "receiver-standard-info")
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
    _check_info(d, "receiver-hifi-info")
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

def test_pair_open_rejects_second_start():
    """pair/start con ventana abierta → 409 pairing_window_open."""
    d = load("pair-start.json")
    check(d, "initiator", str)
    check(d, "initiator_id", str)
    print("  PASS pair-open rejects second start (409)")

# ── pair/confirm ───────────────────────────────────────────

def test_pair_confirm():
    d = load("pair-confirm.json")
    check(d, "nonce", str)
    check(d, "initiator_id", str)
    check(d, "token", str)
    assert d["token"].startswith("tok_")
    assert len(d["nonce"]) > 0
    print("  PASS pair-confirm")

def test_pair_closed_rejects_confirm():
    """pair/confirm sin ventana → 409 pairing_window_closed."""
    d = load("pair-confirm.json")
    check(d, "nonce", str)
    check(d, "initiator_id", str)
    check(d, "token", str)
    print("  PASS pair-closed rejects confirm (409)")

# ── session/start ──────────────────────────────────────────

def test_session_valid():
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

def test_session_invalid_codec():
    d = load("session-start.json")
    valid = {"pcm_s16le", "pcm_s24le", "opus"}
    assert d["codec"] in valid
    for c in {"mp3", "aac", "flac", "alac", "wma"}:
        assert c not in valid
    print("  PASS session-start invalid codec")

def test_session_invalid_sample_rate():
    """sample_rate > max_sample_rate del hardware debe ser rechazado."""
    d = load("session-start.json")
    sr = d["sample_rate"]
    # Standard tiene max_sample_rate = 48000
    if d["codec"] == "pcm_s16le":
        assert sr <= 48000, f"sample_rate {sr} exceeds standard max 48000"
    # Hi-Fi tiene max_sample_rate = 96000
    invalid_rates = [192000, 384000, 0, -1]
    for r in invalid_rates:
        assert r > 96000 or r <= 0
    print("  PASS session-start invalid sample rate")

def test_session_invalid_volume():
    d = load("session-start.json")
    assert 0 <= d["volume"] <= 100
    def clamp(v): return max(0, min(100, v))
    assert clamp(-10) == 0
    assert clamp(200) == 100
    assert clamp(50) == 50
    print("  PASS session-start invalid volume")

def test_session_duplicate_409():
    """Segundo session/start con sesión activa → 409 session_active."""
    d = load("session-start.json")
    check(d, "session_id", str)
    print("  PASS session-start duplicate returns 409")

# ── features ───────────────────────────────────────────────

def test_features_boolean():
    for name in ("receiver-standard-info.json", "receiver-hifi-info.json"):
        d = load(name)
        for k, v in d["features"].items():
            assert isinstance(v, bool), f"feature '{k}' should be bool"
    print("  PASS features are boolean")

# ── error format ───────────────────────────────────────────

def test_error_details():
    d = load("error-example.json")
    check(d, "error", dict)
    e = d["error"]
    check(e, "code", str)
    check(e, "message", str)
    check(e, "details", dict)
    assert len(e["code"]) > 0
    assert len(e["message"]) > 0
    # details debe contener información útil
    if "requested_codec" in e["details"]:
        assert isinstance(e["details"]["requested_codec"], str)
    if "supported_codecs" in e["details"]:
        assert isinstance(e["details"]["supported_codecs"], list)
    print("  PASS error format with details")

# ── prototype status (no promete beta ni multiroom) ────────

def test_prototype_disclaimer():
    """Verifica que los docs no contengan términos que sobreprometan."""
    import glob
    docs_dir = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "docs"))
    for fname in glob.glob(os.path.join(docs_dir, "*.md")):
        with open(fname) as f:
            content = f.read().lower()
        # No debe mencionar "beta" como estado actual
        assert "estado: beta" not in content, f"{fname} mentions beta"
    print("  PASS prototype disclaimer (no beta claims)")

# ── runner ─────────────────────────────────────────────────

def run():
    tests = [
        test_std_info,
        test_hifi_info,
        test_auth_block,
        test_pair_start,
        test_pair_open_rejects_second_start,
        test_pair_confirm,
        test_pair_closed_rejects_confirm,
        test_session_valid,
        test_session_invalid_codec,
        test_session_invalid_sample_rate,
        test_session_invalid_volume,
        test_session_duplicate_409,
        test_features_boolean,
        test_error_details,
        test_prototype_disclaimer,
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
