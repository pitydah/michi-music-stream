#!/usr/bin/env python3
"""Unit tests for the canonical receiver v1-lite simulator (MS-02)."""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from receiver_sim import (
    SimulatorState,
    STANDARD_CONFIG,
    HIFI_CONFIG,
    CONTROLLER_IDENTITY,
    PAIRING_WINDOW_SECONDS,
    STREAM_PORT_MIN,
    STREAM_PORT_MAX,
    VECTOR_SERVER_ID,
    VECTOR_MICHI_ID,
    VECTOR_PUBLIC_KEY,
    derive_michi_id,
    decode_base64url_strict,
)

import json

PROJECT_DIR = os.path.normpath(
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
)
VECTORS_DIR = os.path.join(PROJECT_DIR, "contracts", "michi-link", "vectors")


def load_vector(*path_parts):
    with open(os.path.join(VECTORS_DIR, *path_parts), encoding="utf-8") as handle:
        return json.load(handle)


class FakeClock:
    def __init__(self, start=1000.0):
        self.t = start

    def __call__(self):
        return self.t

    def advance(self, seconds):
        self.t += seconds


def std_state(clock=None):
    return SimulatorState(STANDARD_CONFIG, mono_clock=clock or FakeClock())


def paired_state(clock=None):
    state = std_state(clock)
    state.open_pairing_window()
    _, started = state.pairing_start(CONTROLLER_IDENTITY)
    session_id = started["session_id"]
    pin = state.pairing_sessions[session_id]["pin"]
    _, confirmed = state.pairing_confirm(
        session_id, pin, CONTROLLER_IDENTITY["michi_id"], CONTROLLER_IDENTITY["public_key"]
    )
    return state, confirmed["token"]


def session_payload():
    return {
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


# ── server info ──────────────────────────────────────────────

def test_info_standard_canonical():
    s = std_state()
    info = s.info()
    assert info["service"] == "michi-stream-standard"
    assert info["api_version"] == "v1-lite"
    assert info["roles"] == ["audio_receiver"]
    assert info["identity_scheme"] == "ed25519-blake3-v1"
    assert info["features"]["session"] is True
    assert info["features"]["heartbeat"] is True
    assert info["features"]["volume"] is True
    assert info["features"]["now_playing"] is False
    assert info["features"]["diagnostics"] is False
    assert info["features"]["ota"] is False
    assert info["audio"]["codecs"] == ["pcm_s16le"]
    assert info["audio"]["sample_rates"] == [48000]
    assert info["audio"]["payload_types"] == [97]
    print("PASS info standard canonical")


def test_info_hifi_service():
    s = SimulatorState(HIFI_CONFIG, mono_clock=FakeClock())
    assert s.info()["service"] == "michi-stream-hifi"
    assert s.info()["roles"] == ["audio_receiver"]
    assert s.info()["audio"]["codecs"] == ["pcm_s16le"]
    print("PASS info hifi service")


def test_identity_is_deterministic_fixture():
    a = std_state()
    b = std_state()
    for field in ("server_id", "michi_id", "public_key"):
        assert a.info()[field] == b.info()[field]
    assert a.info()["server_id"] == VECTOR_SERVER_ID
    assert a.info()["michi_id"] == VECTOR_MICHI_ID
    assert a.info()["public_key"] == VECTOR_PUBLIC_KEY
    print("PASS identity deterministic fixture")


# ── pairing window ───────────────────────────────────────────

def test_pairing_start_closed_window_403():
    s = std_state()
    code, body = s.pairing_start(CONTROLLER_IDENTITY)
    assert code == 403
    assert body["error"]["code"] == "FORBIDDEN"
    assert s.pairing_sessions == {}
    print("PASS pairing start closed window 403")


def test_michi_id_derivation_matches_bundle_identity_vectors():
    for vector in load_vector("identity", "server-info-standard.json"), load_vector("identity", "server-info-hifi.json"):
        derived = derive_michi_id(decode_base64url_strict(vector["public_key"]))
        assert derived == vector["michi_id"]
    assert derive_michi_id(decode_base64url_strict(VECTOR_PUBLIC_KEY)) == VECTOR_MICHI_ID
    discovery = load_vector("discovery", "announce-valid.json")
    assert derive_michi_id(decode_base64url_strict(discovery["public_key"])) == discovery["michi_id"]
    print("PASS michi_id derivation matches bundle identity/discovery vectors")


def test_pairing_start_bundle_vector_valid_201():
    vector = load_vector("pairing", "pair-start-valid.json")
    s = std_state()
    s.open_pairing_window()
    code, body = s.pairing_start(vector)
    assert code == 201
    assert body["server_michi_id"] == VECTOR_MICHI_ID
    assert body["server_public_key"] == VECTOR_PUBLIC_KEY
    assert len(s.pairing_sessions) == 1
    print("PASS pairing start bundle vector valid -> 201")


def test_pairing_start_nonce_altered_400_no_session():
    vector = load_vector("pairing", "pair-start-nonce-altered.json")
    s = std_state()
    s.open_pairing_window()
    code, body = s.pairing_start(vector)
    assert code == 400
    assert body["error"]["code"] == "INVALID_REQUEST"
    assert body["error"]["details"]["field"] == "challenge_signature"
    assert s.pairing_sessions == {}
    print("PASS pairing start nonce altered -> 400 INVALID_REQUEST, no session")


def test_pairing_start_wrong_michi_id_400_no_session():
    vector = load_vector("pairing", "pair-start-wrong-michi-id.json")
    s = std_state()
    s.open_pairing_window()
    code, body = s.pairing_start(vector)
    assert code == 400
    assert body["error"]["code"] == "INVALID_REQUEST"
    assert body["error"]["details"]["field"] == "michi_id"
    assert s.pairing_sessions == {}
    print("PASS pairing start wrong michi_id -> 400 INVALID_REQUEST, no session")


def test_pairing_start_forged_signature_400():
    vector = dict(load_vector("pairing", "pair-start-valid.json"))
    vector["challenge_signature"] = "A" * 86
    s = std_state()
    s.open_pairing_window()
    code, body = s.pairing_start(vector)
    assert code == 400
    assert body["error"]["code"] == "INVALID_REQUEST"
    assert s.pairing_sessions == {}
    print("PASS pairing start forged signature -> 400 INVALID_REQUEST")


def test_pairing_start_open_201_pin_local_only():
    s = std_state()
    s.open_pairing_window()
    code, body = s.pairing_start(CONTROLLER_IDENTITY)
    assert code == 201
    assert body["attempts_remaining"] == 5
    assert body["server_michi_id"] == VECTOR_MICHI_ID
    assert body["server_public_key"] == VECTOR_PUBLIC_KEY
    assert "pin" not in body
    session = s.pairing_sessions[body["session_id"]]
    assert re.fullmatch(r"[0-9]{6}", session["pin"])
    assert session["status"] == "pending"
    print("PASS pairing start open 201, pin local only")


def test_pairing_reopen_drops_pending_sessions():
    s = std_state()
    s.open_pairing_window()
    _, body = s.pairing_start(CONTROLLER_IDENTITY)
    old_sid = body["session_id"]
    s.open_pairing_window()
    assert old_sid not in s.pairing_sessions
    print("PASS pairing reopen drops pending sessions")


def test_pairing_status_pending():
    s = std_state()
    s.open_pairing_window()
    _, body = s.pairing_start(CONTROLLER_IDENTITY)
    code, status = s.pairing_status(body["session_id"])
    assert code == 200
    assert status["status"] == "pending"
    assert status["attempts_remaining"] == 5
    print("PASS pairing status pending")


def test_pairing_status_unknown_404():
    s = std_state()
    code, body = s.pairing_status("550e8400-e29b-41d4-a716-446655449999")
    assert code == 404
    assert body["error"]["code"] == "NOT_FOUND"
    print("PASS pairing status unknown 404")


# ── pairing confirm ──────────────────────────────────────────

def test_pairing_confirm_wrong_pin_401_decrements():
    s = std_state()
    s.open_pairing_window()
    _, started = s.pairing_start(CONTROLLER_IDENTITY)
    sid = started["session_id"]
    wrong = "000000" if s.pairing_sessions[sid]["pin"] != "000000" else "999999"
    code, body = s.pairing_confirm(sid, wrong, CONTROLLER_IDENTITY["michi_id"], CONTROLLER_IDENTITY["public_key"])
    assert code == 401
    assert body["error"]["code"] == "PAIRING_PIN_MISMATCH"
    assert s.pairing_sessions[sid]["attempts_remaining"] == 4
    assert s.pairing_sessions[sid]["status"] == "pending"
    print("PASS pairing confirm wrong pin 401")


def test_pairing_confirm_locked_429_after_five():
    s = std_state()
    s.open_pairing_window()
    _, started = s.pairing_start(CONTROLLER_IDENTITY)
    sid = started["session_id"]
    real = s.pairing_sessions[sid]["pin"]
    wrong = "000000" if real != "000000" else "999999"
    for _ in range(5):
        code, _ = s.pairing_confirm(sid, wrong, CONTROLLER_IDENTITY["michi_id"], CONTROLLER_IDENTITY["public_key"])
    assert code == 429
    assert s.pairing_sessions[sid]["status"] == "locked"
    code2, body2 = s.pairing_confirm(sid, real, CONTROLLER_IDENTITY["michi_id"], CONTROLLER_IDENTITY["public_key"])
    assert code2 == 429
    assert body2["error"]["code"] == "RATE_LIMITED"
    print("PASS pairing confirm locked 429 after five")


def test_pairing_confirm_success_token_digest_only():
    s = std_state()
    s.open_pairing_window()
    _, started = s.pairing_start(CONTROLLER_IDENTITY)
    sid = started["session_id"]
    pin = s.pairing_sessions[sid]["pin"]
    code, body = s.pairing_confirm(sid, pin, CONTROLLER_IDENTITY["michi_id"], CONTROLLER_IDENTITY["public_key"])
    assert code == 200
    assert body["expires_in"] == 0
    assert body["server_id"] == VECTOR_SERVER_ID
    assert re.fullmatch(r"[A-Za-z0-9_-]{43}", body["token"])
    controller = s.controllers[body["device_id"]]
    assert controller["michi_id"] == CONTROLLER_IDENTITY["michi_id"]
    assert body["token"] not in str(controller)
    assert controller["token_sha256"] != body["token"]
    assert "receiver.ota" not in controller["permissions"]
    assert s.pairing_sessions[sid]["status"] == "confirmed"
    print("PASS pairing confirm success, digest only")


def test_pairing_double_confirm_409():
    s = std_state()
    s.open_pairing_window()
    _, started = s.pairing_start(CONTROLLER_IDENTITY)
    sid = started["session_id"]
    pin = s.pairing_sessions[sid]["pin"]
    s.pairing_confirm(sid, pin, CONTROLLER_IDENTITY["michi_id"], CONTROLLER_IDENTITY["public_key"])
    code, body = s.pairing_confirm(sid, pin, CONTROLLER_IDENTITY["michi_id"], CONTROLLER_IDENTITY["public_key"])
    assert code == 409
    assert body["error"]["code"] == "PAIRING_ALREADY_CONSUMED"
    print("PASS pairing double confirm 409")


def test_pairing_identity_mismatch_400():
    s = std_state()
    s.open_pairing_window()
    _, started = s.pairing_start(CONTROLLER_IDENTITY)
    sid = started["session_id"]
    pin = s.pairing_sessions[sid]["pin"]
    code, body = s.pairing_confirm(sid, pin, VECTOR_MICHI_ID, CONTROLLER_IDENTITY["public_key"])
    assert code == 400
    assert body["error"]["code"] == "INVALID_REQUEST"
    assert body["error"]["details"]["field"] == "michi_id"
    print("PASS pairing identity mismatch 400")


def test_validate_pairing_token_sha256_digest():
    s, token = paired_state()
    assert s.validate_pairing_token(token) is True
    assert s.validate_pairing_token("wrong") is False
    assert s.validate_pairing_token(token + "x") is False
    print("PASS validate pairing token (digest compare)")


# ── session ──────────────────────────────────────────────────

def test_session_create_201_port_in_range_socket_bound():
    s, _ = paired_state()
    code, body = s.session_create(session_payload(), source_ip="10.0.0.7")
    assert code == 201
    assert body["lease_seconds"] == 30
    assert re.fullmatch(r"[A-Za-z0-9_-]{43}", body["session_token"])
    port = body["effective"]["stream_port"]
    assert STREAM_PORT_MIN <= port <= STREAM_PORT_MAX
    assert body["effective"]["ssrc"] == 305419896
    assert s.stream_socket is not None
    assert s.session["source_ip"] == "10.0.0.7"
    print("PASS session create 201, port in range, socket bound")


def test_session_create_duplicate_409():
    s, _ = paired_state()
    s.session_create(session_payload(), source_ip="127.0.0.1")
    code, body = s.session_create(session_payload(), source_ip="127.0.0.1")
    assert code == 409
    assert body["error"]["code"] == "CONFLICT"
    print("PASS session create duplicate 409")


def test_session_state_body():
    s, _ = paired_state()
    s.session_create(session_payload(), source_ip="127.0.0.1")
    code, body = s.session_state()
    assert code == 200
    assert body["state"] == "playing"
    assert body["paused"] is False
    assert body["volume"] == 70
    assert 0 <= body["lease_remaining_ms"] <= 30000
    assert "session_token" not in body
    print("PASS session state body")


def test_session_patch_volume_and_paused():
    s, _ = paired_state()
    s.session_create(session_payload(), source_ip="127.0.0.1")
    code, body = s.session_patch({"volume": 55, "paused": True})
    assert code == 200
    assert body["volume"] == 55
    assert body["paused"] is True
    assert body["state"] == "paused"
    code2, body2 = s.session_patch({"paused": False})
    assert code2 == 200
    assert body2["state"] == "playing"
    print("PASS session patch volume/paused")


def test_session_delete_releases_resources():
    s, _ = paired_state()
    s.session_create(session_payload(), source_ip="127.0.0.1")
    sock = s.stream_socket
    code, body = s.session_delete()
    assert code == 204
    assert body is None
    assert s.session_id is None
    assert s.session_token is None
    assert s.stream_socket is None
    assert sock.fileno() == -1
    code2, _ = s.session_state()
    assert code2 == 404
    print("PASS session delete releases resources")


# ── heartbeat and lease ──────────────────────────────────────

def test_heartbeat_valid_renews_lease():
    clock = FakeClock()
    s, _ = paired_state(clock)
    s.session_create(session_payload(), source_ip="127.0.0.1")
    clock.advance(10)
    code, body = s.heartbeat(s.session_id, 7)
    assert code == 200
    assert body["status"] == "alive"
    assert body["lease_seconds"] == 30
    assert body["receiver_uptime_ms"] >= 0
    assert s.lease_until_mono == clock() + 30
    print("PASS heartbeat valid renews lease")


def test_heartbeat_replay_409_no_renew():
    clock = FakeClock()
    s, _ = paired_state(clock)
    s.session_create(session_payload(), source_ip="127.0.0.1")
    s.heartbeat(s.session_id, 7)
    lease = s.lease_until_mono
    clock.advance(10)
    code, body = s.heartbeat(s.session_id, 7)
    assert code == 409
    assert body["error"]["code"] == "CONFLICT"
    assert s.lease_until_mono == lease
    print("PASS heartbeat replay 409, no renew")


def test_heartbeat_older_409():
    clock = FakeClock()
    s, _ = paired_state(clock)
    s.session_create(session_payload(), source_ip="127.0.0.1")
    s.heartbeat(s.session_id, 7)
    code, _ = s.heartbeat(s.session_id, 6)
    assert code == 409
    print("PASS heartbeat older 409")


def test_lease_expiry_closes_session_like_delete():
    clock = FakeClock()
    s, _ = paired_state(clock)
    s.session_create(session_payload(), source_ip="127.0.0.1")
    sock = s.stream_socket
    clock.advance(31)
    code, body = s.session_state()
    assert code == 404
    assert body["error"]["code"] == "NOT_FOUND"
    assert s.session_id is None
    assert s.session_token is None
    assert s.stream_socket is None
    assert sock.fileno() == -1
    assert s.lease_expirations == 1
    print("PASS lease expiry closes session like delete")


def test_session_token_valid():
    s, _ = paired_state()
    _, body = s.session_create(session_payload(), source_ip="127.0.0.1")
    assert s.session_token_valid(body["session_token"]) is True
    assert s.session_token_valid("wrong") is False
    print("PASS session token valid")


# ── runner ───────────────────────────────────────────────────

def run():
    tests = [
        test_info_standard_canonical,
        test_info_hifi_service,
        test_identity_is_deterministic_fixture,
        test_pairing_start_closed_window_403,
        test_michi_id_derivation_matches_bundle_identity_vectors,
        test_pairing_start_bundle_vector_valid_201,
        test_pairing_start_nonce_altered_400_no_session,
        test_pairing_start_wrong_michi_id_400_no_session,
        test_pairing_start_forged_signature_400,
        test_pairing_start_open_201_pin_local_only,
        test_pairing_reopen_drops_pending_sessions,
        test_pairing_status_pending,
        test_pairing_status_unknown_404,
        test_pairing_confirm_wrong_pin_401_decrements,
        test_pairing_confirm_locked_429_after_five,
        test_pairing_confirm_success_token_digest_only,
        test_pairing_double_confirm_409,
        test_pairing_identity_mismatch_400,
        test_validate_pairing_token_sha256_digest,
        test_session_create_201_port_in_range_socket_bound,
        test_session_create_duplicate_409,
        test_session_state_body,
        test_session_patch_volume_and_paused,
        test_session_delete_releases_resources,
        test_heartbeat_valid_renews_lease,
        test_heartbeat_replay_409_no_renew,
        test_heartbeat_older_409,
        test_lease_expiry_closes_session_like_delete,
        test_session_token_valid,
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
