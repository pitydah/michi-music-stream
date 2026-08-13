#!/usr/bin/env python3
"""Contract conformance tests for the simulator (MS-02).

Every JSON body produced by the REAL simulator is validated against the
VENDORIZED Michi Link schemas (contracts/michi-link/schemas/). The
simulator is exercised through its Flask test client; no static JSON
payloads are asserted as handler output.

Run: python3 tests/contract/test_contract.py
     python3 -m pytest tests/contract/test_contract.py
"""

import json
import os
import re
import sys

BASE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(BASE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "simulator"))

import jsonschema
from jsonschema import Draft7Validator
from referencing import Registry, Resource

from receiver_sim import (
    SimulatorState,
    STANDARD_CONFIG,
    CONTROLLER_IDENTITY,
    create_app,
    VECTOR_SERVER_ID,
    VECTOR_MICHI_ID,
    VECTOR_PUBLIC_KEY,
)

SCHEMAS_DIR = os.path.join(ROOT, "contracts", "michi-link", "schemas")
VECTORS_DIR = os.path.join(ROOT, "contracts", "michi-link", "vectors")

SESSION_BODY = {
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

TOKEN_RE = re.compile(r"^[A-Za-z0-9_-]{43}$")


class FakeClock:
    def __init__(self, start=1000.0):
        self.t = start

    def __call__(self):
        return self.t

    def advance(self, seconds):
        self.t += seconds


def load_schemas():
    schemas = {}
    for name in sorted(os.listdir(SCHEMAS_DIR)):
        if name.endswith(".schema.json"):
            with open(os.path.join(SCHEMAS_DIR, name), encoding="utf-8") as handle:
                schemas[name] = json.load(handle)
    return schemas


SCHEMAS = load_schemas()
REGISTRY = Registry().with_resources(
    (doc["$id"], Resource.from_contents(doc)) for doc in SCHEMAS.values()
)


def validate_against(body, schema_name, label):
    validator = Draft7Validator(SCHEMAS[schema_name], registry=REGISTRY)
    validator.validate(body)
    print(f"  PASS {label} vs {schema_name}")


def make_app(clock=None):
    state = SimulatorState(STANDARD_CONFIG, mono_clock=clock or FakeClock())
    app = create_app(state)
    app.config["TESTING"] = True
    return app, state


def pair_via_http(client, state):
    state.open_pairing_window()
    r = client.post("/api/v1/pair/start", json=CONTROLLER_IDENTITY)
    assert r.status_code == 201
    sid = r.get_json()["session_id"]
    pin = state.pairing_sessions[sid]["pin"]
    r = client.post("/api/v1/pair/confirm", json={
        "session_id": sid,
        "pin": pin,
        "michi_id": CONTROLLER_IDENTITY["michi_id"],
        "public_key": CONTROLLER_IDENTITY["public_key"],
    })
    assert r.status_code == 200
    return r.get_json()["token"]


def start_session(client, state, token):
    r = client.post(
        "/api/v1/receiver-lite/session",
        json=SESSION_BODY,
        headers={"Authorization": f"Bearer {token}"},
    )
    assert r.status_code == 201
    return r.get_json()


def session_headers(token, created):
    return {
        "Authorization": f"Bearer {token}",
        "X-Michi-Session": created["session_token"],
    }


# ── mandatory case: info ─────────────────────────────────────

def test_case_info_validates_schema():
    app, _ = make_app()
    with app.test_client() as c:
        r = c.get("/api/v1/server/info")
        assert r.status_code == 200
        body = r.get_json()
        validate_against(body, "server-info.schema.json", "server/info response")
        assert body["service"] == "michi-stream-standard"
        assert body["api_version"] == "v1-lite"
        assert body["server_id"] == VECTOR_SERVER_ID
        with open(os.path.join(VECTORS_DIR, "identity", "server-info-standard.json"), encoding="utf-8") as handle:
            vector = json.load(handle)
        assert body["michi_id"] == vector["michi_id"]
        assert body["public_key"] == vector["public_key"]
        assert body["server_id"] == vector["server_id"]
    print("PASS case: info (real response validates, identity == bundle vector)")


# ── mandatory case: legacy 404 ───────────────────────────────

def test_case_legacy_routes_404():
    legacy = [
        "/api/v1/receiver/info",
        "/api/v1/receiver/pair/start",
        "/api/v1/receiver/session/start",
        "/api/v1/receiver/session/stop",
        "/api/v1/receiver/heartbeat",
        "/api/v1/receiver/volume",
        "/api/v1/receiver-lite/info",
        "/api/v1/receiver-lite/volume",
        "/api/v1/receiver-lite/config",
    ]
    app, _ = make_app()
    with app.test_client() as c:
        for path in legacy:
            r = c.get(path)
            assert r.status_code == 404, path
            body = r.get_json()
            validate_against(body, "error.schema.json", f"legacy 404 {path}")
            assert body["error"]["code"] == "NOT_FOUND"
    print("PASS case: legacy routes 404 with canonical error")


# ── mandatory case: pairing window closed 403 ────────────────

def test_case_pairing_window_closed_403():
    app, _ = make_app()
    with app.test_client() as c:
        r = c.post("/api/v1/pair/start", json=CONTROLLER_IDENTITY)
        assert r.status_code == 403
        body = r.get_json()
        validate_against(body, "error.schema.json", "pair/start 403")
        assert body["error"]["code"] == "FORBIDDEN"
    print("PASS case: pairing window closed -> 403 FORBIDDEN")


# ── mandatory case: pairing completo ─────────────────────────

def test_case_pair_start_bundle_vectors():
    """Golden tests: the pairing vectors of the vendored bundle drive the
    real endpoint. Valid vector -> 201; nonce-altered and wrong-michi-id
    vectors -> 400 INVALID_REQUEST with no session created (section 2.3).
    """
    app, state = make_app()
    with app.test_client() as c:
        state.open_pairing_window()
        for name in ("pair-start-nonce-altered.json", "pair-start-wrong-michi-id.json"):
            with open(os.path.join(VECTORS_DIR, "pairing", name), encoding="utf-8") as handle:
                vector = json.load(handle)
            r = c.post("/api/v1/pair/start", json=vector)
            assert r.status_code == 400, name
            body = r.get_json()
            validate_against(body, "error.schema.json", f"pair/start {name} 400")
            assert body["error"]["code"] == "INVALID_REQUEST", name
            assert body["error"]["details"]["field"] in ("challenge_signature", "michi_id"), name
            assert state.pairing_sessions == {}, name

        with open(os.path.join(VECTORS_DIR, "pairing", "pair-start-valid.json"), encoding="utf-8") as handle:
            vector = json.load(handle)
        r = c.post("/api/v1/pair/start", json=vector)
        assert r.status_code == 201
        started = r.get_json()
        validate_against(started, "pair-start-response.schema.json", "pair/start valid vector 201")
        assert started["server_michi_id"] == VECTOR_MICHI_ID
        assert started["server_public_key"] == VECTOR_PUBLIC_KEY
        assert "pin" not in started
        assert len(state.pairing_sessions) == 1
    print("PASS case: pairing bundle vectors (valid 201, tampered 400, no session)")


def test_case_complete_pairing():
    app, state = make_app()
    with app.test_client() as c:
        state.open_pairing_window()
        r = c.post("/api/v1/pair/start", json=CONTROLLER_IDENTITY)
        assert r.status_code == 201
        started = r.get_json()
        validate_against(started, "pair-start-response.schema.json", "pair/start 201")
        assert started["server_michi_id"] == VECTOR_MICHI_ID
        assert started["server_public_key"] == VECTOR_PUBLIC_KEY
        assert started["attempts_remaining"] == 5
        assert "pin" not in started

        sid = started["session_id"]
        r = c.get(f"/api/v1/pair/status?session_id={sid}")
        assert r.status_code == 200
        status = r.get_json()
        validate_against(status, "pair-status.schema.json", "pair/status 200")
        assert status["status"] == "pending"

        pin = state.pairing_sessions[sid]["pin"]
        r = c.post("/api/v1/pair/confirm", json={
            "session_id": sid,
            "pin": pin,
            "michi_id": CONTROLLER_IDENTITY["michi_id"],
            "public_key": CONTROLLER_IDENTITY["public_key"],
        })
        assert r.status_code == 200
        confirmed = r.get_json()
        validate_against(confirmed, "pair-confirm-response.schema.json", "pair/confirm 200")
        assert confirmed["expires_in"] == 0
        assert confirmed["server_id"] == VECTOR_SERVER_ID
        assert TOKEN_RE.fullmatch(confirmed["token"])

        r = c.post("/api/v1/pair/confirm", json={
            "session_id": sid,
            "pin": pin,
            "michi_id": CONTROLLER_IDENTITY["michi_id"],
            "public_key": CONTROLLER_IDENTITY["public_key"],
        })
        assert r.status_code == 409
        consumed = r.get_json()
        validate_against(consumed, "error.schema.json", "pair/confirm double 409")
        assert consumed["error"]["code"] == "PAIRING_ALREADY_CONSUMED"

        r = c.get(f"/api/v1/pair/status?session_id={sid}")
        assert r.status_code == 200
        assert r.get_json()["status"] == "confirmed"
    print("PASS case: complete pairing flow (all bodies vs bundle schemas)")


# ── mandatory case: start sin Bearer 401 ─────────────────────

def test_case_session_start_without_bearer_401():
    app, _ = make_app()
    with app.test_client() as c:
        r = c.post("/api/v1/receiver-lite/session", json=SESSION_BODY)
        assert r.status_code == 401
        body = r.get_json()
        validate_against(body, "error.schema.json", "session create 401")
        assert body["error"]["code"] == "UNAUTHORIZED"
    print("PASS case: session start without Bearer -> 401 UNAUTHORIZED")


# ── mandatory case: start válido 201 ─────────────────────────

def test_case_session_start_valid_201():
    app, state = make_app()
    with app.test_client() as c:
        token = pair_via_http(c, state)
        r = c.post(
            "/api/v1/receiver-lite/session",
            json=SESSION_BODY,
            headers={"Authorization": f"Bearer {token}"},
        )
        assert r.status_code == 201
        created = r.get_json()
        validate_against(created, "receiver-session.schema.json", "session create 201")
        assert TOKEN_RE.fullmatch(created["session_token"])
        assert created["lease_seconds"] == 30
        assert 49152 <= created["effective"]["stream_port"] <= 65535
        assert "source_ip" not in created["effective"]
        assert state.session["source_ip"] == "127.0.0.1"

        body_with_ip = dict(SESSION_BODY)
        body_with_ip["source_ip"] = "10.0.0.99"
        r = c.post(
            "/api/v1/receiver-lite/session",
            json=body_with_ip,
            headers={"Authorization": f"Bearer {token}"},
        )
        assert r.status_code == 400
    print("PASS case: session start valid -> 201 (port assigned, source IP inferred)")


# ── mandatory case: segundo start 409 ────────────────────────

def test_case_second_start_409():
    app, state = make_app()
    with app.test_client() as c:
        token = pair_via_http(c, state)
        start_session(c, state, token)
        r = c.post(
            "/api/v1/receiver-lite/session",
            json=SESSION_BODY,
            headers={"Authorization": f"Bearer {token}"},
        )
        assert r.status_code == 409
        body = r.get_json()
        validate_against(body, "error.schema.json", "session duplicate 409")
        assert body["error"]["code"] == "CONFLICT"
    print("PASS case: second start -> 409 CONFLICT")


# ── mandatory case: PATCH sin session token 401 ──────────────

def test_case_patch_without_session_token_401():
    app, state = make_app()
    with app.test_client() as c:
        token = pair_via_http(c, state)
        start_session(c, state, token)
        r = c.patch(
            "/api/v1/receiver-lite/session",
            json={"volume": 55},
            headers={"Authorization": f"Bearer {token}"},
        )
        assert r.status_code == 401
        body = r.get_json()
        validate_against(body, "error.schema.json", "patch without session token 401")
        assert body["error"]["code"] == "UNAUTHORIZED"
    print("PASS case: PATCH without session token -> 401 UNAUTHORIZED")


# ── mandatory case: heartbeat replay 409 ─────────────────────

def test_case_heartbeat_replay_409():
    app, state = make_app()
    with app.test_client() as c:
        token = pair_via_http(c, state)
        created = start_session(c, state, token)
        headers = session_headers(token, created)
        hb = {"session_id": created["session_id"], "sequence": 7, "sent_at_ms": 1786564800000}
        r = c.post("/api/v1/receiver-lite/heartbeat", json=hb, headers=headers)
        assert r.status_code == 200
        validated = r.get_json()
        validate_against(validated, "receiver-heartbeat-response.schema.json", "heartbeat 200")
        assert validated["status"] == "alive"
        assert validated["lease_seconds"] == 30

        r = c.post("/api/v1/receiver-lite/heartbeat", json=hb, headers=headers)
        assert r.status_code == 409
        replay = r.get_json()
        validate_against(replay, "error.schema.json", "heartbeat replay 409")
        assert replay["error"]["code"] == "CONFLICT"

        older = dict(hb)
        older["sequence"] = 6
        r = c.post("/api/v1/receiver-lite/heartbeat", json=older, headers=headers)
        assert r.status_code == 409

        newer = dict(hb)
        newer["sequence"] = 8
        r = c.post("/api/v1/receiver-lite/heartbeat", json=newer, headers=headers)
        assert r.status_code == 200
    print("PASS case: heartbeat replay/older -> 409, no renew; newer -> 200")


# ── mandatory case: reloj +31 s cierra ───────────────────────

def test_case_clock_plus_31s_closes():
    clock = FakeClock()
    app, state = make_app(clock)
    with app.test_client() as c:
        token = pair_via_http(c, state)
        start_session(c, state, token)
        clock.advance(31)
        r = c.get("/api/v1/receiver-lite/session", headers={"Authorization": f"Bearer {token}"})
        assert r.status_code == 404
        body = r.get_json()
        validate_against(body, "error.schema.json", "session after lease expiry 404")
        assert body["error"]["code"] == "NOT_FOUND"
        assert state.session_id is None
        assert state.session_token is None
        assert state.stream_socket is None
        assert state.lease_expirations == 1
    print("PASS case: clock +31s closes session like DELETE (lease expired)")


# ── mandatory case: DELETE libera ────────────────────────────

def test_case_delete_frees():
    app, state = make_app()
    with app.test_client() as c:
        token = pair_via_http(c, state)
        created = start_session(c, state, token)
        headers = session_headers(token, created)
        r = c.delete("/api/v1/receiver-lite/session", headers=headers)
        assert r.status_code == 204
        assert r.data == b""

        r = c.get("/api/v1/receiver-lite/session", headers={"Authorization": f"Bearer {token}"})
        assert r.status_code == 404

        new_created = start_session(c, state, token)
        assert 49152 <= new_created["effective"]["stream_port"] <= 65535
        assert state.session_id == new_created["session_id"]
    print("PASS case: DELETE releases session (204, GET 404, new session allowed)")


# ── invalid request bodies vs bundle schemas ─────────────────

def test_case_invalid_requests_400():
    app, state = make_app()
    with app.test_client() as c:
        token = pair_via_http(c, state)
        headers = {"Authorization": f"Bearer {token}"}
        for mutated in (
            {"buffer_ms": 49},
            {"ssrc": 0},
            {"codec": "opus"},
            {"sample_rate": 96000},
            {"volume": 101},
        ):
            body = dict(SESSION_BODY)
            body.update(mutated)
            r = c.post("/api/v1/receiver-lite/session", json=body, headers=headers)
            assert r.status_code == 400, mutated
            err = r.get_json()
            validate_against(err, "error.schema.json", f"session create invalid {mutated}")
            assert err["error"]["code"] == "INVALID_REQUEST"
            assert "details" in err["error"]

        camel = dict(SESSION_BODY)
        camel["sampleRate"] = 48000
        r = c.post("/api/v1/receiver-lite/session", json=camel, headers=headers)
        assert r.status_code == 400
    print("PASS case: invalid session bodies -> 400 INVALID_REQUEST with details.field")


# ── runner ───────────────────────────────────────────────────

def run():
    tests = [
        test_case_info_validates_schema,
        test_case_legacy_routes_404,
        test_case_pairing_window_closed_403,
        test_case_pair_start_bundle_vectors,
        test_case_complete_pairing,
        test_case_session_start_without_bearer_401,
        test_case_session_start_valid_201,
        test_case_second_start_409,
        test_case_patch_without_session_token_401,
        test_case_heartbeat_replay_409,
        test_case_clock_plus_31s_closes,
        test_case_delete_frees,
        test_case_invalid_requests_400,
    ]
    ok = 0
    for t in tests:
        try:
            t()
            ok += 1
        except Exception as e:
            print(f"  FAIL {t.__name__}: {e}")
    print(f"\n{ok}/{len(tests)} contract conformance cases passed")
    return ok == len(tests)


if __name__ == "__main__":
    sys.exit(0 if run() else 1)
