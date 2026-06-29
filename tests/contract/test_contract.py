#!/usr/bin/env python3
import json, os, sys

EX = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "examples"))

def load(f): return json.load(open(os.path.join(EX, f)))

def check(obj, field, typ, opt=False):
    if field not in obj:
        if opt: return
        raise AssertionError(f"Missing {field}")
    if not isinstance(obj[field], typ):
        raise AssertionError(f"{field} should be {typ}, got {type(obj[field])}")

def test_std_info():
    d = load("receiver-standard-info.json")
    assert d["type"] == "michi_stream_standard"
    assert d["output"]["connector"] == "jack_3_5"
    assert d["output"]["max_sample_rate"] == 48000
    assert d["output"]["max_bit_depth"] == 16
    assert "pcm_s16le" in d["supported_codecs"]
    assert "pcm_s24le" not in d["supported_codecs"]
    assert d["features"]["ota_update"] == False
    assert d["features"]["pairing_button"] == True
    print("  PASS standard-info")

def test_hifi_info():
    d = load("receiver-hifi-info.json")
    assert d["type"] == "michi_stream_hifi"
    assert d["output"]["connector"] == "rca_stereo"
    assert d["output"]["dac"] == "hifi_i2s"
    assert d["output"]["max_sample_rate"] == 96000
    assert d["output"]["max_bit_depth"] == 24
    assert "pcm_s24le" in d["supported_codecs"]
    assert d["features"]["ota_update"] == True
    print("  PASS hifi-info")

def test_pair_start():
    d = load("pair-start.json")
    check(d, "initiator", str); check(d, "initiator_id", str)
    print("  PASS pair-start")

def test_pair_confirm():
    d = load("pair-confirm.json")
    check(d, "nonce", str); check(d, "initiator_id", str); check(d, "token", str)
    assert d["token"].startswith("tok_")
    print("  PASS pair-confirm")

def test_session():
    d = load("session-start.json")
    assert d["codec"] in ("pcm_s16le","pcm_s24le","opus")
    assert d["transport"] == "udp"
    assert 1024 <= d["stream_port"] <= 65535
    assert 50 <= d["buffer_ms"] <= 2000
    assert 0 <= d["volume"] <= 100
    print("  PASS session-start")

def run():
    tests = [test_std_info, test_hifi_info, test_pair_start, test_pair_confirm, test_session]
    ok = 0
    for t in tests:
        try: t(); ok += 1
        except Exception as e: print(f"  FAIL {t.__name__}: {e}")
    print(f"\n{ok}/{len(tests)} passed")
    return ok == len(tests)

sys.exit(0 if run() else 1)
