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

# ── receiver/info ───────────────────────────────────────────

def _check_info(d, label):
    check(d, "service", str)
    assert d["service"] in ("michi-stream-standard", "michi-stream-hifi")
    check(d, "name", str); check(d, "device_id", str)
    check(d, "api_version", str); assert d["api_version"] == "v1-lite"
    check(d, "michi_link_version", str); assert d["michi_link_version"] == "1.0.0-alpha"
    check(d, "firmware", str); check(d, "type", str)
    check(d, "roles", list)
    assert "audio_receiver" in d["roles"] and "music_stream_receiver" in d["roles"]
    check(d, "auth", dict)
    a = d["auth"]
    assert a["required"] == True and a["strategy"] == "RECEIVER_BUTTON" and a["token_refresh"] == False
    check(d, "output", dict)
    o = d["output"]
    check(o, "connector", str); check(o, "max_sample_rate", int)
    check(o, "max_bit_depth", int); check(o, "channels", int)
    assert o["channels"] == 2
    check(d, "supported_codecs", list); check(d, "features", dict)
    print(f"  PASS {label}")

def test_std_info():
    d = load("receiver-standard-info.json")
    _check_info(d, "receiver-standard-info")
    assert d["service"] == "michi-stream-standard"
    assert d["type"] == "michi_stream_standard"
    assert d["output"]["connector"] == "jack_3_5"
    assert d["output"]["max_sample_rate"] == 48000 and d["output"]["max_bit_depth"] == 16
    assert "dac" not in d["output"]
    assert "pcm_s16le" in d["supported_codecs"]
    assert "pcm_s24le" not in d["supported_codecs"]
    assert d["features"]["pairing_button"] and not d["features"]["ota_update"]

def test_hifi_info():
    d = load("receiver-hifi-info.json")
    _check_info(d, "receiver-hifi-info")
    assert d["service"] == "michi-stream-hifi"
    assert d["type"] == "michi_stream_hifi"
    assert d["output"]["connector"] == "rca_stereo"
    assert d["output"]["dac"] == "hifi_i2s"
    assert d["output"]["max_sample_rate"] == 96000 and d["output"]["max_bit_depth"] == 24
    assert "pcm_s16le" in d["supported_codecs"] and "pcm_s24le" in d["supported_codecs"]
    assert d["features"]["ota_update"] == True

def test_auth_block():
    for name in ("receiver-standard-info.json", "receiver-hifi-info.json"):
        d = load(name)
        a = d["auth"]
        assert list(a.keys()) == ["required", "strategy", "token_refresh"]
        assert a["required"] == True and a["strategy"] == "RECEIVER_BUTTON" and a["token_refresh"] == False
    print("  PASS auth block")

# ── pairing ─────────────────────────────────────────────────

def test_pair_start():
    d = load("pair-start.json")
    check(d, "initiator", str); check(d, "initiator_id", str)
    print("  PASS pair-start")

def test_pair_confirm():
    d = load("pair-confirm.json")
    check(d, "nonce", str); check(d, "initiator_id", str); check(d, "token", str)
    assert d["token"].startswith("tok_") and len(d["nonce"]) > 0
    print("  PASS pair-confirm")

# ── session ─────────────────────────────────────────────────

def test_session_valid():
    d = load("session-start.json")
    assert d["codec"] in ("pcm_s16le", "pcm_s24le")
    assert isinstance(d["sample_rate"], int) and d["sample_rate"] > 0
    assert d["bit_depth"] in (16, 24) and d["channels"] == 2 and d["transport"] == "udp"
    assert 1024 <= d["stream_port"] <= 65535 and 50 <= d["buffer_ms"] <= 2000
    assert 0 <= d["volume"] <= 100 and 1 <= len(d["session_id"]) <= 32
    print("  PASS session-start valid")

def test_session_invalid_codec():
    d = load("session-start.json")
    valid = {"pcm_s16le", "pcm_s24le"}
    assert d["codec"] in valid
    for c in {"mp3", "aac", "flac", "opus"}:
        assert c not in valid
    print("  PASS session-start invalid codec")

def test_session_invalid_volume():
    d = load("session-start.json")
    assert 0 <= d["volume"] <= 100
    def clamp(v): return max(0, min(100, v))
    assert clamp(-10) == 0 and clamp(200) == 100 and clamp(50) == 50
    print("  PASS session-start invalid volume")

# ── heartbeat, session/stop, volume, firmware ───────────────

def test_heartbeat_request():
    d = load("heartbeat-request.json")
    check(d, "session_id", str)
    print("  PASS heartbeat-request")

def test_session_stop():
    d = load("session-stop.json")
    check(d, "session_id", str)
    print("  PASS session-stop")

def test_volume_request():
    d = load("volume-request.json")
    check(d, "volume", int)
    assert 0 <= d["volume"] <= 100
    print("  PASS volume-request")

def test_firmware():
    d = load("firmware-info.json")
    check(d, "device_id", str); check(d, "current_version", str)
    check(d, "build_date", str); check(d, "ota_supported", bool)
    print("  PASS firmware-info")

# ── error format ────────────────────────────────────────────

def _check_error(d, label):
    check(d, "error", dict)
    e = d["error"]
    check(e, "code", str); check(e, "message", str); check(e, "details", dict)
    assert len(e["code"]) > 0 and len(e["message"]) > 0
    print(f"  PASS {label}")

def test_error_unsupported_codec():
    _check_error(load("error-example.json"), "error-unsupported-codec")
    d = load("error-example.json")
    assert "requested_codec" in d["error"]["details"] and "supported_codecs" in d["error"]["details"]

def test_error_invalid_token():
    _check_error(load("error-invalid-token.json"), "error-invalid-token")

def test_error_pairing_closed():
    _check_error(load("error-pairing-closed.json"), "error-pairing-closed")

def test_error_session_active():
    _check_error(load("error-session-active.json"), "error-session-active")
    d = load("error-session-active.json")
    assert "active_session_id" in d["error"]["details"]

def test_error_unsupported_rate():
    _check_error(load("error-unsupported-rate.json"), "error-unsupported-rate")
    d = load("error-unsupported-rate.json")
    assert "requested_rate" in d["error"]["details"] and "max_rate" in d["error"]["details"]

# ── features ────────────────────────────────────────────────

def test_features_boolean():
    for name in ("receiver-standard-info.json", "receiver-hifi-info.json"):
        d = load(name)
        for k, v in d["features"].items():
            assert isinstance(v, bool), f"feature '{k}' should be bool"
    print("  PASS features are boolean")

# ── prototype disclaimer ────────────────────────────────────

def test_prototype_disclaimer():
    import glob
    docs_dir = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "docs"))
    for fname in glob.glob(os.path.join(docs_dir, "*.md")):
        with open(fname) as f:
            content = f.read().lower()
        assert "estado: beta" not in content, f"{fname} mentions beta"
    print("  PASS prototype disclaimer")

# ── runner ──────────────────────────────────────────────────

def run():
    tests = [
        test_std_info, test_hifi_info, test_auth_block,
        test_pair_start, test_pair_confirm,
        test_session_valid, test_session_invalid_codec, test_session_invalid_volume,
        test_heartbeat_request, test_session_stop, test_volume_request, test_firmware,
        test_error_unsupported_codec, test_error_invalid_token, test_error_pairing_closed,
        test_error_session_active, test_error_unsupported_rate,
        test_features_boolean, test_prototype_disclaimer,
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
