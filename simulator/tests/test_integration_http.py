#!/usr/bin/env python3
"""HTTP integration tests for the canonical receiver v1-lite simulator.

Tests the Flask routing layer: headers, auth, JSON parsing, status codes.
"""

import base64
import os
import secrets
import sys

import blake3
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from receiver_sim import SimulatorState, STANDARD_CONFIG, CONTROLLER_IDENTITY, create_app

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
    ]

    def test_legacy_routes_404(self, app_std):
        app, _ = app_std
        with app.test_client() as c:
            for method, path in self.LEGACY_PATHS:
                r = c.open(path, method=method, json={})
                assert r.status_code == 404, path
                assert r.get_json()["error"]["code"] == "NOT_FOUND", path


class TestPairing:
    def test_pair_start_403_window_closed(self, app_std):
        app, _ = app_std
        with app.test_client() as c:
            r = c.post("/api/v1/pair/start", json=CONTROLLER_IDENTITY)
            assert r.status_code == 403
            assert r.get_json()["error"]["code"] == "FORBIDDEN"

    def test_pair_start_201_window_open(self, app_std):
        app, state = app_std
        state.open_pairing_window()
        with app.test_client() as c:
            r = c.post("/api/v1/pair/start", json=CONTROLLER_IDENTITY)
            assert r.status_code == 201
            d = r.get_json()
            assert d["attempts_remaining"] == 5
            assert "pin" not in d

    def test_pair_status_and_confirm_flow(self, app_std):
        app, state = app_std
        state.open_pairing_window()
        with app.test_client() as c:
            r = c.post("/api/v1/pair/start", json=CONTROLLER_IDENTITY)
            sid = r.get_json()["session_id"]
            pin = state.pairing_sessions[sid]["pin"]
            r = c.get(f"/api/v1/pair/status?session_id={sid}")
            assert r.status_code == 200
            assert r.get_json()["status"] == "pending"
            r = c.post("/api/v1/pair/confirm", json={
                "session_id": sid,
                "pin": pin,
                "michi_id": CONTROLLER_IDENTITY["michi_id"],
                "public_key": CONTROLLER_IDENTITY["public_key"],
            })
            assert r.status_code == 200
            assert r.get_json()["expires_in"] == 0
            r = c.post("/api/v1/pair/confirm", json={
                "session_id": sid,
                "pin": pin,
                "michi_id": CONTROLLER_IDENTITY["michi_id"],
                "public_key": CONTROLLER_IDENTITY["public_key"],
            })
            assert r.status_code == 409
            assert r.get_json()["error"]["code"] == "CONFLICT"

    def test_pair_start_invalid_body_400(self, app_std):
        app, state = app_std
        state.open_pairing_window()
        with app.test_client() as c:
            body = dict(CONTROLLER_IDENTITY)
            body["deviceId"] = "camel"
            r = c.post("/api/v1/pair/start", json=body)
            assert r.status_code == 400
            assert r.get_json()["error"]["code"] == "INVALID_REQUEST"

    def test_pair_recover_http_flow(self, app_std):
        app, state = app_std
        priv = Ed25519PrivateKey.generate()
        pub = priv.public_key()
        pub_bytes = pub.public_bytes_raw()
        pub_b64 = base64.urlsafe_b64encode(pub_bytes).decode("ascii").rstrip("=")
        michi_id = base64.urlsafe_b64encode(blake3.blake3(pub_bytes).digest()).decode("ascii").rstrip("=")
        start_nonce = base64.urlsafe_b64encode(secrets.token_bytes(32)).decode("ascii").rstrip("=")
        start_sig = base64.urlsafe_b64encode(priv.sign(base64.urlsafe_b64decode(start_nonce + "=="))).decode("ascii").rstrip("=")
        ctrl_id = {
            "device_name": "Test Controller",
            "device_type": "server",
            "roles": ["music_server"],
            "auth_strategy": "RECEIVER_BUTTON",
            "michi_id": michi_id,
            "public_key": pub_b64,
            "challenge_nonce": start_nonce,
            "challenge_signature": start_sig,
        }

        with app.test_client() as c:
            # 1. Recover start before pairing -> 404
            r = c.post("/api/v1/pair/recover/start", json={
                "michi_id": ctrl_id["michi_id"],
                "public_key": ctrl_id["public_key"],
            })
            assert r.status_code == 404
            assert r.get_json()["error"]["code"] == "NOT_FOUND"

            # Recover without active challenge -> 400
            r = c.post("/api/v1/pair/recover", json=ctrl_id)
            assert r.status_code == 400
            assert r.get_json()["error"]["code"] == "INVALID_REQUEST"

            # 2. Pair normally
            state.open_pairing_window()
            r = c.post("/api/v1/pair/start", json=ctrl_id)
            assert r.status_code == 201
            sid = r.get_json()["session_id"]
            pin = state.pairing_sessions[sid]["pin"]
            r = c.post("/api/v1/pair/confirm", json={
                "session_id": sid,
                "pin": pin,
                "michi_id": ctrl_id["michi_id"],
                "public_key": ctrl_id["public_key"],
            })
            assert r.status_code == 200
            token1 = r.get_json()["token"]

            # 3. Recover start returns challenge_nonce
            r = c.post("/api/v1/pair/recover/start", json={
                "michi_id": ctrl_id["michi_id"],
                "public_key": ctrl_id["public_key"],
            })
            assert r.status_code == 200
            rec_nonce = r.get_json()["challenge_nonce"]
            rec_sig = base64.urlsafe_b64encode(priv.sign(base64.urlsafe_b64decode(rec_nonce + "=="))).decode("ascii").rstrip("=")

            # 4. Recover rotates token
            recover_payload = {
                "michi_id": ctrl_id["michi_id"],
                "public_key": ctrl_id["public_key"],
                "challenge_nonce": rec_nonce,
                "challenge_signature": rec_sig,
            }
            r = c.post("/api/v1/pair/recover", json=recover_payload)
            assert r.status_code == 200
            token2 = r.get_json()["token"]
            assert token2 != token1

            # 5. Replay attack: single-use consumption rejects replaying exact same recovery payload
            r = c.post("/api/v1/pair/recover", json=recover_payload)
            assert r.status_code == 400
            assert r.get_json()["error"]["code"] == "INVALID_REQUEST"

            # 6. Old token fails session create, new token succeeds
            r = c.post("/api/v1/receiver-lite/session", json=SESSION_BODY, headers={"Authorization": f"Bearer {token1}"})
            assert r.status_code == 401
            r = c.post("/api/v1/receiver-lite/session", json=SESSION_BODY, headers={"Authorization": f"Bearer {token2}"})
            assert r.status_code == 201


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
