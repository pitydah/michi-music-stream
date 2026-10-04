#!/usr/bin/env python3
"""HTTP integration tests for the canonical receiver v1-lite simulator.

Tests the Flask routing layer: headers, auth, JSON parsing, status codes.
"""

import base64
import os
import secrets
import sys

import blake3
from cryptography.hazmat.primitives import serialization as ser
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey, Ed25519PublicKey

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from receiver_sim import (
    SimulatorState,
    STANDARD_CONFIG,
    CONTROLLER_IDENTITY,
    create_app,
    make_membership_cert,
)

import pytest


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


@pytest.fixture
def app_std():
    state = SimulatorState(STANDARD_CONFIG)
    app = create_app(state)
    app.config["TESTING"] = True
    return app, state


def authenticate_client(client, state):
    client_sk = Ed25519PrivateKey.generate()
    pk_bytes = client_sk.public_key().public_bytes(ser.Encoding.Raw, ser.PublicFormat.Raw)
    client_pk = base64.urlsafe_b64encode(pk_bytes).decode("ascii").rstrip("=")
    client_id = base64.urlsafe_b64encode(blake3.blake3(pk_bytes).digest()).decode("ascii").rstrip("=")

    r = client.post("/api/v1/auth/challenge", json={
        "client_michi_id": client_id,
        "client_public_key": client_pk,
        "home_id": state.home_id,
    })
    assert r.status_code == 200, r.get_data(as_text=True)
    ch = r.get_json()
    cid = ch["challenge_id"]
    nonce = ch["challenge_nonce"]

    mem = make_membership_cert(state.home_root_private_key, state.home_id, client_id, client_pk)
    auth_payload = (
        b"michi-link-device-auth-v1"
        + state.home_id.encode("ascii")
        + state.michi_id.encode("ascii")
        + client_id.encode("ascii")
        + cid.encode("ascii")
        + nonce.encode("ascii")
    )
    sig = base64.urlsafe_b64encode(client_sk.sign(auth_payload)).decode("ascii").rstrip("=")

    r = client.post("/api/v1/auth/session", json={
        "challenge_id": cid,
        "client_michi_id": client_id,
        "membership": mem,
        "client_signature": sig,
    })
    assert r.status_code == 200, r.get_data(as_text=True)
    return r.get_json()["session_token"]


pair_via_http = authenticate_client


def start_session(client, state, token):
    r = client.post(
        "/api/v1/receiver-lite/session",
        json=SESSION_BODY,
        headers={"Authorization": f"Bearer {token}"},
    )
    assert r.status_code == 201
    return r.get_json()


class TestServerInfo:
    def test_get_server_info_200(self, app_std):
        app, _ = app_std
        with app.test_client() as c:
            r = c.get("/api/v1/server/info")
            assert r.status_code == 200
            d = r.get_json()
            assert d["service"] == "michi-stream-standard"
            assert d["api_version"] == "v1-lite"
            assert d["roles"] == ["audio_receiver"]
            assert d["auth"]["strategy"] == "HOME_MEMBERSHIP"
            assert "michi_home_id" in d


class TestLegacyRoutes:
    LEGACY_PATHS = [
        ("GET", "/api/v1/receiver/info"),
        ("POST", "/api/v1/receiver/pair/start"),
        ("POST", "/api/v1/receiver/session/start"),
        ("POST", "/api/v1/receiver/session/stop"),
        ("POST", "/api/v1/receiver/heartbeat"),
        ("POST", "/api/v1/receiver/volume"),
        ("GET", "/api/v1/receiver-lite/info"),
        ("POST", "/api/v1/receiver-lite/volume"),
        ("GET", "/api/v1/receiver-lite/config"),
        ("POST", "/api/v1/pair/start"),
        ("GET", "/api/v1/pair/status"),
        ("POST", "/api/v1/pair/confirm"),
        ("POST", "/api/v1/pair/recover/start"),
        ("POST", "/api/v1/pair/recover"),
    ]

    def test_legacy_routes_404(self, app_std):
        app, _ = app_std
        with app.test_client() as c:
            for method, path in self.LEGACY_PATHS:
                r = c.open(path, method=method, json={})
                assert r.status_code == 404, path
                assert r.get_json()["error"]["code"] == "NOT_FOUND", path


class TestAuth:
    def test_auth_challenge_success(self, app_std):
        app, state = app_std
        client_sk = Ed25519PrivateKey.generate()
        pk_bytes = client_sk.public_key().public_bytes(ser.Encoding.Raw, ser.PublicFormat.Raw)
        client_pk = base64.urlsafe_b64encode(pk_bytes).decode("ascii").rstrip("=")
        client_id = base64.urlsafe_b64encode(blake3.blake3(pk_bytes).digest()).decode("ascii").rstrip("=")

        with app.test_client() as c:
            r = c.post("/api/v1/auth/challenge", json={
                "client_michi_id": client_id,
                "client_public_key": client_pk,
                "home_id": state.home_id,
            })
            assert r.status_code == 200
            d = r.get_json()
            assert "challenge_id" in d
            assert "challenge_nonce" in d
            assert d["server_michi_id"] == state.michi_id
            assert d["expires_in"] == 60

    def test_auth_challenge_wrong_home_id_403(self, app_std):
        app, state = app_std
        client_sk = Ed25519PrivateKey.generate()
        pk_bytes = client_sk.public_key().public_bytes(ser.Encoding.Raw, ser.PublicFormat.Raw)
        client_pk = base64.urlsafe_b64encode(pk_bytes).decode("ascii").rstrip("=")
        client_id = base64.urlsafe_b64encode(blake3.blake3(pk_bytes).digest()).decode("ascii").rstrip("=")

        with app.test_client() as c:
            r = c.post("/api/v1/auth/challenge", json={
                "client_michi_id": client_id,
                "client_public_key": client_pk,
                "home_id": "X" * 43,
            })
            assert r.status_code == 403
            assert r.get_json()["error"]["code"] == "FORBIDDEN"

    def test_auth_session_success_and_mutual_sig(self, app_std):
        app, state = app_std
        client_sk = Ed25519PrivateKey.generate()
        pk_bytes = client_sk.public_key().public_bytes(ser.Encoding.Raw, ser.PublicFormat.Raw)
        client_pk = base64.urlsafe_b64encode(pk_bytes).decode("ascii").rstrip("=")
        client_id = base64.urlsafe_b64encode(blake3.blake3(pk_bytes).digest()).decode("ascii").rstrip("=")

        with app.test_client() as c:
            r = c.post("/api/v1/auth/challenge", json={
                "client_michi_id": client_id,
                "client_public_key": client_pk,
                "home_id": state.home_id,
            })
            assert r.status_code == 200
            ch = r.get_json()
            cid = ch["challenge_id"]
            nonce = ch["challenge_nonce"]

            mem = make_membership_cert(state.home_root_private_key, state.home_id, client_id, client_pk)
            auth_payload = (
                b"michi-link-device-auth-v1"
                + state.home_id.encode("ascii")
                + state.michi_id.encode("ascii")
                + client_id.encode("ascii")
                + cid.encode("ascii")
                + nonce.encode("ascii")
            )
            sig = base64.urlsafe_b64encode(client_sk.sign(auth_payload)).decode("ascii").rstrip("=")

            r = c.post("/api/v1/auth/session", json={
                "challenge_id": cid,
                "client_michi_id": client_id,
                "membership": mem,
                "client_signature": sig,
            })
            assert r.status_code == 200
            sess = r.get_json()
            assert sess["token_type"] == "Bearer"
            assert "session_token" in sess
            assert sess["expires_in"] == 3600
            assert sess["server_michi_id"] == state.michi_id

            # Verify mutual server confirmation signature
            server_pub_raw = state.server_private_key.public_key().public_bytes(ser.Encoding.Raw, ser.PublicFormat.Raw)
            server_pub = Ed25519PublicKey.from_public_bytes(server_pub_raw)
            server_expected = (
                b"michi-link-server-auth-v1"
                + state.home_id.encode("ascii")
                + state.michi_id.encode("ascii")
                + client_id.encode("ascii")
                + cid.encode("ascii")
                + sess["session_token"].encode("ascii")
            )
            server_sig = base64.urlsafe_b64decode(sess["server_signature"] + "==")
            server_pub.verify(server_sig, server_expected)

            # Replay of challenge -> 404
            r2 = c.post("/api/v1/auth/session", json={
                "challenge_id": cid,
                "client_michi_id": client_id,
                "membership": mem,
                "client_signature": sig,
            })
            assert r2.status_code == 404
            assert r2.get_json()["error"]["code"] == "NOT_FOUND"

    def test_auth_session_tampered_membership_401(self, app_std):
        app, state = app_std
        client_sk = Ed25519PrivateKey.generate()
        pk_bytes = client_sk.public_key().public_bytes(ser.Encoding.Raw, ser.PublicFormat.Raw)
        client_pk = base64.urlsafe_b64encode(pk_bytes).decode("ascii").rstrip("=")
        client_id = base64.urlsafe_b64encode(blake3.blake3(pk_bytes).digest()).decode("ascii").rstrip("=")

        with app.test_client() as c:
            r = c.post("/api/v1/auth/challenge", json={
                "client_michi_id": client_id,
                "client_public_key": client_pk,
                "home_id": state.home_id,
            })
            ch = r.get_json()
            cid = ch["challenge_id"]
            nonce = ch["challenge_nonce"]

            mem = make_membership_cert(state.home_root_private_key, state.home_id, client_id, client_pk)
            # Tamper membership roles (valid enum roles, but alters signed payload)
            mem["roles"] = ["music_server", "playback_host"]
            auth_payload = (
                b"michi-link-device-auth-v1"
                + state.home_id.encode("ascii")
                + state.michi_id.encode("ascii")
                + client_id.encode("ascii")
                + cid.encode("ascii")
                + nonce.encode("ascii")
            )
            sig = base64.urlsafe_b64encode(client_sk.sign(auth_payload)).decode("ascii").rstrip("=")

            r = c.post("/api/v1/auth/session", json={
                "challenge_id": cid,
                "client_michi_id": client_id,
                "membership": mem,
                "client_signature": sig,
            })
            assert r.status_code == 401
            assert r.get_json()["error"]["code"] == "UNAUTHORIZED"


class TestSessionAuth:
    def test_session_create_401_no_bearer(self, app_std):
        app, _ = app_std
        with app.test_client() as c:
            r = c.post("/api/v1/receiver-lite/session", json=SESSION_BODY)
            assert r.status_code == 401
            assert r.get_json()["error"]["code"] == "UNAUTHORIZED"

    def test_session_create_401_wrong_bearer(self, app_std):
        app, _ = app_std
        with app.test_client() as c:
            r = c.post(
                "/api/v1/receiver-lite/session",
                json=SESSION_BODY,
                headers={"Authorization": "Bearer wrong"},
            )
            assert r.status_code == 401


class TestSession:
    def test_session_create_201_and_duplicate_409(self, app_std):
        app, state = app_std
        with app.test_client() as c:
            token = pair_via_http(c, state)
            headers = {"Authorization": f"Bearer {token}"}
            r = c.post("/api/v1/receiver-lite/session", json=SESSION_BODY, headers=headers)
            assert r.status_code == 201
            d = r.get_json()
            assert d["lease_seconds"] == 30
            assert 49152 <= d["effective"]["stream_port"] <= 65535
            r = c.post("/api/v1/receiver-lite/session", json=SESSION_BODY, headers=headers)
            assert r.status_code == 409
            assert r.get_json()["error"]["code"] == "CONFLICT"

    def test_session_create_400_invalid_field(self, app_std):
        app, state = app_std
        with app.test_client() as c:
            token = pair_via_http(c, state)
            body = dict(SESSION_BODY)
            body["buffer_ms"] = 49
            r = c.post(
                "/api/v1/receiver-lite/session",
                json=body,
                headers={"Authorization": f"Bearer {token}"},
            )
            assert r.status_code == 400
            d = r.get_json()
            assert d["error"]["code"] == "INVALID_REQUEST"
            assert d["error"]["details"]["field"] == "buffer_ms"

    def test_session_create_400_source_ip_rejected(self, app_std):
        app, state = app_std
        with app.test_client() as c:
            token = pair_via_http(c, state)
            body = dict(SESSION_BODY)
            body["source_ip"] = "10.0.0.99"
            r = c.post(
                "/api/v1/receiver-lite/session",
                json=body,
                headers={"Authorization": f"Bearer {token}"},
            )
            assert r.status_code == 400

    def test_session_get_200_and_404(self, app_std):
        app, state = app_std
        with app.test_client() as c:
            token = pair_via_http(c, state)
            headers = {"Authorization": f"Bearer {token}"}
            r = c.get("/api/v1/receiver-lite/session", headers=headers)
            assert r.status_code == 404
            start_session(c, state, token)
            r = c.get("/api/v1/receiver-lite/session", headers=headers)
            assert r.status_code == 200
            assert "session_token" not in r.get_json()

    def test_patch_without_session_token_401(self, app_std):
        app, state = app_std
        with app.test_client() as c:
            token = pair_via_http(c, state)
            start_session(c, state, token)
            r = c.patch(
                "/api/v1/receiver-lite/session",
                json={"volume": 55},
                headers={"Authorization": f"Bearer {token}"},
            )
            assert r.status_code == 401
            assert r.get_json()["error"]["code"] == "UNAUTHORIZED"

    def test_patch_with_session_token_200(self, app_std):
        app, state = app_std
        with app.test_client() as c:
            token = pair_via_http(c, state)
            created = start_session(c, state, token)
            headers = {
                "Authorization": f"Bearer {token}",
                "X-Michi-Session": created["session_token"],
            }
            r = c.patch("/api/v1/receiver-lite/session", json={"volume": 55, "paused": True}, headers=headers)
            assert r.status_code == 200
            d = r.get_json()
            assert d["volume"] == 55
            assert d["paused"] is True
            assert d["state"] == "paused"

    def test_delete_204_and_get_404(self, app_std):
        app, state = app_std
        with app.test_client() as c:
            token = pair_via_http(c, state)
            created = start_session(c, state, token)
            headers = {
                "Authorization": f"Bearer {token}",
                "X-Michi-Session": created["session_token"],
            }
            r = c.delete("/api/v1/receiver-lite/session", headers=headers)
            assert r.status_code == 204
            assert r.data == b""
            r = c.get("/api/v1/receiver-lite/session", headers={"Authorization": f"Bearer {token}"})
            assert r.status_code == 404

    def test_delete_wrong_session_token_401(self, app_std):
        app, state = app_std
        with app.test_client() as c:
            token = pair_via_http(c, state)
            start_session(c, state, token)
            headers = {
                "Authorization": f"Bearer {token}",
                "X-Michi-Session": "wrong",
            }
            r = c.delete("/api/v1/receiver-lite/session", headers=headers)
            assert r.status_code == 401


class TestHeartbeat:
    def test_heartbeat_200_and_replay_409(self, app_std):
        app, state = app_std
        with app.test_client() as c:
            token = pair_via_http(c, state)
            created = start_session(c, state, token)
            headers = {
                "Authorization": f"Bearer {token}",
                "X-Michi-Session": created["session_token"],
            }
            body = {"session_id": created["session_id"], "sequence": 7, "sent_at_ms": 1786564800000}
            r = c.post("/api/v1/receiver-lite/heartbeat", json=body, headers=headers)
            assert r.status_code == 200
            assert r.get_json()["status"] == "alive"
            assert r.get_json()["lease_seconds"] == 30
            r = c.post("/api/v1/receiver-lite/heartbeat", json=body, headers=headers)
            assert r.status_code == 409
            assert r.get_json()["error"]["code"] == "CONFLICT"

    def test_heartbeat_401_without_session_token(self, app_std):
        app, state = app_std
        with app.test_client() as c:
            token = pair_via_http(c, state)
            created = start_session(c, state, token)
            r = c.post(
                "/api/v1/receiver-lite/heartbeat",
                json={"session_id": created["session_id"], "sequence": 1, "sent_at_ms": 1},
                headers={"Authorization": f"Bearer {token}"},
            )
            assert r.status_code == 401
