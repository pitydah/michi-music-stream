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


import base64
import blake3
from cryptography.hazmat.primitives import serialization as ser
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey, Ed25519PublicKey


def make_app(clock=None):
    state = SimulatorState(STANDARD_CONFIG, mono_clock=clock or FakeClock())
    app = create_app(state)
    app.config["TESTING"] = True
    return app, state


def make_membership(root_sk, home_id, dev_michi_id, dev_pubkey, dev_type="server", roles=None, issued_at="2026-10-04T12:00:00Z", serial=1):
    if roles is None:
        roles = ["music_server"]
    sorted_roles = sorted(roles)
    canon = (
        b"michi-link-membership-v1"
        + home_id.encode("ascii")
        + dev_michi_id.encode("ascii")
        + dev_pubkey.encode("ascii")
        + dev_type.encode("ascii")
    )
    for r in sorted_roles:
        canon += b":" + r.encode("utf-8")
    canon += b":" + issued_at.encode("ascii") + b":" + str(serial).encode("ascii")
    sig = root_sk.sign(canon)
    sig_b64 = base64.urlsafe_b64encode(sig).decode("ascii").rstrip("=")
    return {
        "version": 1,
        "home_id": home_id,
        "device_michi_id": dev_michi_id,
        "device_public_key": dev_pubkey,
        "device_type": dev_type,
        "roles": roles,
        "issued_at": issued_at,
        "serial": serial,
        "signature": sig_b64,
    }


def authenticate_via_http(client, state):
    client_sk = Ed25519PrivateKey.generate()
    client_pk_bytes = client_sk.public_key().public_bytes(ser.Encoding.Raw, ser.PublicFormat.Raw)
    client_pk_b64 = base64.urlsafe_b64encode(client_pk_bytes).decode("ascii").rstrip("=")
    client_michi_id = base64.urlsafe_b64encode(blake3.blake3(client_pk_bytes).digest()).decode("ascii").rstrip("=")

    ch_req = {
        "client_michi_id": client_michi_id,
        "client_public_key": client_pk_b64,
        "home_id": state.home_id,
    }
    r = client.post("/api/v1/auth/challenge", json=ch_req)
    assert r.status_code == 200
    ch_resp = r.get_json()
    challenge_id = ch_resp["challenge_id"]
    challenge_nonce = ch_resp["challenge_nonce"]

    membership = make_membership(state.home_root_private_key, state.home_id, client_michi_id, client_pk_b64)

    auth_payload = (
        b"michi-link-device-auth-v1"
        + state.home_id.encode("ascii")
        + state.michi_id.encode("ascii")
        + client_michi_id.encode("ascii")
        + challenge_id.encode("ascii")
        + challenge_nonce.encode("ascii")
    )
    client_sig = base64.urlsafe_b64encode(client_sk.sign(auth_payload)).decode("ascii").rstrip("=")

    sess_req = {
        "challenge_id": challenge_id,
        "client_michi_id": client_michi_id,
        "membership": membership,
        "client_signature": client_sig,
    }
    r = client.post("/api/v1/auth/session", json=sess_req)
    assert r.status_code == 200
    return r.get_json()["session_token"]


pair_via_http = authenticate_via_http


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
        assert body["auth"]["strategy"] == "HOME_MEMBERSHIP"
        assert body["michi_id"] == VECTOR_MICHI_ID
        assert body["public_key"] == VECTOR_PUBLIC_KEY
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
        "/api/v1/pair/start",
        "/api/v1/pair/status",
        "/api/v1/pair/confirm",
        "/api/v1/pair/recover/start",
        "/api/v1/pair/recover",
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


# ── mandatory case: auth challenge ───────────────────────────

def test_case_auth_challenge():
    app, state = make_app()
    with app.test_client() as c:
        client_sk = Ed25519PrivateKey.generate()
        pk_bytes = client_sk.public_key().public_bytes(ser.Encoding.Raw, ser.PublicFormat.Raw)
        client_pk = base64.urlsafe_b64encode(pk_bytes).decode("ascii").rstrip("=")
        client_id = base64.urlsafe_b64encode(blake3.blake3(pk_bytes).digest()).decode("ascii").rstrip("=")

        # Valid challenge
        r = c.post("/api/v1/auth/challenge", json={
            "client_michi_id": client_id,
            "client_public_key": client_pk,
            "home_id": state.home_id,
        })
        assert r.status_code == 200
        body = r.get_json()
        validate_against(body, "device-auth-challenge-response.schema.json", "auth/challenge 200")
        assert body["server_michi_id"] == state.michi_id
        assert body["server_public_key"] == state.public_key
        assert body["expires_in"] == 60

        # Mismatched home_id -> 403
        r_wrong_home = c.post("/api/v1/auth/challenge", json={
            "client_michi_id": client_id,
            "client_public_key": client_pk,
            "home_id": "wrong-home-id-43chars-base64url-nopad-12345",
        })
        assert r_wrong_home.status_code == 403
        err_home = r_wrong_home.get_json()
        validate_against(err_home, "error.schema.json", "auth/challenge 403")
        assert err_home["error"]["code"] == "FORBIDDEN"

        # Mismatched client_michi_id -> 400
        r_wrong_id = c.post("/api/v1/auth/challenge", json={
            "client_michi_id": "f2UwxQaeA6vA8LO7Cr1nGRr5MStned_Gbmc_ua48qUc",
            "client_public_key": client_pk,
            "home_id": state.home_id,
        })
        assert r_wrong_id.status_code == 400
        err_id = r_wrong_id.get_json()
        validate_against(err_id, "error.schema.json", "auth/challenge 400")
        assert err_id["error"]["code"] == "INVALID_REQUEST"
    print("PASS case: device auth challenge (positive 200, wrong home 403, bad id 400)")


# ── mandatory case: auth session ─────────────────────────────

def test_case_auth_session():
    app, state = make_app()
    with app.test_client() as c:
        client_sk = Ed25519PrivateKey.generate()
        pk_bytes = client_sk.public_key().public_bytes(ser.Encoding.Raw, ser.PublicFormat.Raw)
        client_pk = base64.urlsafe_b64encode(pk_bytes).decode("ascii").rstrip("=")
        client_id = base64.urlsafe_b64encode(blake3.blake3(pk_bytes).digest()).decode("ascii").rstrip("=")

        r = c.post("/api/v1/auth/challenge", json={
            "client_michi_id": client_id,
            "client_public_key": client_pk,
            "home_id": state.home_id,
        })
        assert r.status_code == 200
        ch = r.get_json()
        cid = ch["challenge_id"]
        nonce = ch["challenge_nonce"]

        mem = make_membership(state.home_root_private_key, state.home_id, client_id, client_pk)
        auth_bytes = (
            b"michi-link-device-auth-v1"
            + state.home_id.encode("ascii")
            + state.michi_id.encode("ascii")
            + client_id.encode("ascii")
            + cid.encode("ascii")
            + nonce.encode("ascii")
        )
        sig = base64.urlsafe_b64encode(client_sk.sign(auth_bytes)).decode("ascii").rstrip("=")

        # Valid session
        r_sess = c.post("/api/v1/auth/session", json={
            "challenge_id": cid,
            "client_michi_id": client_id,
            "membership": mem,
            "client_signature": sig,
        })
        assert r_sess.status_code == 200
        s_body = r_sess.get_json()
        validate_against(s_body, "device-auth-session-response.schema.json", "auth/session 200")
        assert s_body["token_type"] == "Bearer"
        assert s_body["expires_in"] == 3600
        assert s_body["server_michi_id"] == state.michi_id

        # Verify server confirmation signature
        server_auth_bytes = (
            b"michi-link-server-auth-v1"
            + state.home_id.encode("ascii")
            + state.michi_id.encode("ascii")
            + client_id.encode("ascii")
            + cid.encode("ascii")
            + s_body["session_token"].encode("ascii")
        )
        server_pk = Ed25519PublicKey.from_public_bytes(base64.urlsafe_b64decode(state.public_key + "="))
        server_pk.verify(base64.urlsafe_b64decode(s_body["server_signature"] + "=" * (-len(s_body["server_signature"]) % 4)), server_auth_bytes)

        # Anti-replay single use: re-submitting challenge -> 404
        r_replay = c.post("/api/v1/auth/session", json={
            "challenge_id": cid,
            "client_michi_id": client_id,
            "membership": mem,
            "client_signature": sig,
        })
        assert r_replay.status_code == 404
        replay_body = r_replay.get_json()
        validate_against(replay_body, "error.schema.json", "auth/session replay 404")
        assert replay_body["error"]["code"] == "NOT_FOUND"

        # Tampered membership certificate -> 401
        r_ch2 = c.post("/api/v1/auth/challenge", json={
            "client_michi_id": client_id,
            "client_public_key": client_pk,
            "home_id": state.home_id,
        })
        ch2 = r_ch2.get_json()
        cid2 = ch2["challenge_id"]
        nonce2 = ch2["challenge_nonce"]
        auth_bytes2 = (
            b"michi-link-device-auth-v1"
            + state.home_id.encode("ascii")
            + state.michi_id.encode("ascii")
            + client_id.encode("ascii")
            + cid2.encode("ascii")
            + nonce2.encode("ascii")
        )
        sig2 = base64.urlsafe_b64encode(client_sk.sign(auth_bytes2)).decode("ascii").rstrip("=")
        bad_mem = dict(mem)
        bad_mem["signature"] = ("A" if bad_mem["signature"][0] != "A" else "B") + bad_mem["signature"][1:]

        r_bad_mem = c.post("/api/v1/auth/session", json={
            "challenge_id": cid2,
            "client_michi_id": client_id,
            "membership": bad_mem,
            "client_signature": sig2,
        })
        assert r_bad_mem.status_code == 401
        err_mem = r_bad_mem.get_json()
        validate_against(err_mem, "error.schema.json", "auth/session bad mem 401")
        assert err_mem["error"]["code"] == "UNAUTHORIZED"
    print("PASS case: device auth session (200 + mutual signature, anti-replay 404, bad mem 401)")


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
        test_case_auth_challenge,
        test_case_auth_session,
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
