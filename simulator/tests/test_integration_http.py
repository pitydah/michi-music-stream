#!/usr/bin/env python3
"""HTTP integration tests for Michi Music Stream Simulator.

Tests the Flask routing layer: headers, auth, JSON parsing, status codes.
"""

import json, os, sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from receiver_sim import SimulatorState, STANDARD_CONFIG, create_app

import pytest
from flask import Flask

@pytest.fixture
def app_std():
    state = SimulatorState(STANDARD_CONFIG)
    app = create_app(state)
    app.config["TESTING"] = True
    return app, state

@pytest.fixture
def app_paired():
    state = SimulatorState(STANDARD_CONFIG)
    state.pair_start("micro_001")
    state.pair_confirm(state.current_nonce, "micro_001", "tok_test")
    app = create_app(state)
    app.config["TESTING"] = True
    return app, state


class TestInfo:
    def test_get_info_200(self, app_std):
        app, _ = app_std
        with app.test_client() as c:
            r = c.get("/api/v1/receiver/info")
            assert r.status_code == 200
            d = r.get_json()
            assert d["service"] == "michi-stream-standard"
            assert d["type"] == "michi_stream_standard"

    def test_get_firmware_200(self, app_std):
        app, _ = app_std
        with app.test_client() as c:
            r = c.get("/api/v1/receiver/firmware")
            assert r.status_code == 200
            d = r.get_json()
            assert d["current_version"] == "0.1.0-sim"


class TestPairing:
    def test_pair_start_200(self, app_std):
        app, _ = app_std
        with app.test_client() as c:
            r = c.post("/api/v1/receiver/pair/start",
                       json={"initiator": "test", "initiator_id": "t1"})
            assert r.status_code == 200
            d = r.get_json()
            assert d["status"] == "pairing_window_open"
            assert len(d["nonce"]) > 0

    def test_pair_start_missing_id(self, app_std):
        app, _ = app_std
        with app.test_client() as c:
            r = c.post("/api/v1/receiver/pair/start", json={})
            assert r.status_code == 400
            assert r.get_json()["error"]["code"] == "bad_request"

    def test_pair_start_409_on_duplicate(self, app_std):
        app, _ = app_std
        with app.test_client() as c:
            c.post("/api/v1/receiver/pair/start", json={"initiator": "t", "initiator_id": "t1"})
            r = c.post("/api/v1/receiver/pair/start", json={"initiator": "t", "initiator_id": "t2"})
            assert r.status_code == 409
            assert r.get_json()["error"]["code"] == "pairing_window_open"

    def test_pair_confirm_200(self, app_std):
        app, state = app_std
        with app.test_client() as c:
            c.post("/api/v1/receiver/pair/start", json={"initiator": "t", "initiator_id": "t1"})
            nonce = state.current_nonce
            r = c.post("/api/v1/receiver/pair/confirm",
                       json={"nonce": nonce, "initiator_id": "t1", "token": "tok_test"})
            assert r.status_code == 200
            assert r.get_json()["status"] == "paired"

    def test_pair_confirm_409_closed(self, app_std):
        app, _ = app_std
        with app.test_client() as c:
            r = c.post("/api/v1/receiver/pair/confirm",
                       json={"nonce": "x", "initiator_id": "t1", "token": "tok_test"})
            assert r.status_code == 409
            assert r.get_json()["error"]["code"] == "pairing_window_closed"

    def test_pair_confirm_400_missing_fields(self, app_std):
        app, _ = app_std
        with app.test_client() as c:
            r = c.post("/api/v1/receiver/pair/confirm", json={})
            assert r.status_code == 400


class TestAuth:
    def test_volume_401_no_token(self, app_std):
        app, _ = app_std
        with app.test_client() as c:
            r = c.post("/api/v1/receiver/volume", json={"volume": 50})
            assert r.status_code == 401

    def test_volume_401_wrong_token(self, app_paired):
        app, _ = app_paired
        with app.test_client() as c:
            r = c.post("/api/v1/receiver/volume",
                       json={"volume": 50},
                       headers={"Authorization": "Bearer wrong"})
            assert r.status_code == 401
            assert r.get_json()["error"]["code"] == "invalid_token"

    def test_session_start_401_no_token(self, app_std):
        app, _ = app_std
        with app.test_client() as c:
            r = c.post("/api/v1/receiver/session/start", json={
                "session_id": "s1", "codec": "pcm_s16le", "sample_rate": 48000,
                "bit_depth": 16, "channels": 2, "stream_port": 55300, "buffer_ms": 250,
            })
            assert r.status_code == 401


class TestSession:
    def test_session_start_200(self, app_paired):
        app, _ = app_paired
        with app.test_client() as c:
            r = c.post("/api/v1/receiver/session/start",
                       json={"session_id": "s1", "codec": "pcm_s16le",
                             "sample_rate": 48000, "bit_depth": 16, "channels": 2,
                             "stream_port": 55300, "buffer_ms": 250, "volume": 70},
                       headers={"Authorization": "Bearer tok_test"})
            assert r.status_code == 200
            assert r.get_json()["status"] == "session_started"

    def test_session_start_409_duplicate(self, app_paired):
        app, _ = app_paired
        with app.test_client() as c:
            h = {"Authorization": "Bearer tok_test"}
            c.post("/api/v1/receiver/session/start",
                   json={"session_id": "s1", "codec": "pcm_s16le",
                         "sample_rate": 48000, "bit_depth": 16, "channels": 2,
                         "stream_port": 55300, "buffer_ms": 250, "volume": 70}, headers=h)
            r = c.post("/api/v1/receiver/session/start",
                       json={"session_id": "s2", "codec": "pcm_s16le",
                             "sample_rate": 48000, "bit_depth": 16, "channels": 2,
                             "stream_port": 55300, "buffer_ms": 250, "volume": 70}, headers=h)
            assert r.status_code == 409
            assert r.get_json()["error"]["code"] == "session_active"

    def test_session_start_400_invalid_codec(self, app_paired):
        app, _ = app_paired
        with app.test_client() as c:
            r = c.post("/api/v1/receiver/session/start",
                       json={"session_id": "s1", "codec": "opus",
                             "sample_rate": 48000, "bit_depth": 16, "channels": 2,
                             "stream_port": 55300, "buffer_ms": 250, "volume": 70},
                       headers={"Authorization": "Bearer tok_test"})
            assert r.status_code == 400
            assert r.get_json()["error"]["code"] == "unsupported_codec"

    def test_session_start_400_invalid_port(self, app_paired):
        app, _ = app_paired
        with app.test_client() as c:
            r = c.post("/api/v1/receiver/session/start",
                       json={"session_id": "s1", "codec": "pcm_s16le",
                             "sample_rate": 48000, "bit_depth": 16, "channels": 2,
                             "stream_port": 80, "buffer_ms": 250, "volume": 70},
                       headers={"Authorization": "Bearer tok_test"})
            assert r.status_code == 400

    def test_session_stop_200(self, app_paired):
        app, _ = app_paired
        with app.test_client() as c:
            h = {"Authorization": "Bearer tok_test"}
            c.post("/api/v1/receiver/session/start",
                   json={"session_id": "s1", "codec": "pcm_s16le",
                         "sample_rate": 48000, "bit_depth": 16, "channels": 2,
                         "stream_port": 55300, "buffer_ms": 250, "volume": 70}, headers=h)
            r = c.post("/api/v1/receiver/session/stop",
                       json={"session_id": "s1"}, headers=h)
            assert r.status_code == 200
            assert r.get_json()["status"] == "session_stopped"


class TestHeartbeat:
    def test_heartbeat_200(self, app_paired):
        app, _ = app_paired
        with app.test_client() as c:
            h = {"Authorization": "Bearer tok_test"}
            c.post("/api/v1/receiver/session/start",
                   json={"session_id": "s1", "codec": "pcm_s16le",
                         "sample_rate": 48000, "bit_depth": 16, "channels": 2,
                         "stream_port": 55300, "buffer_ms": 250, "volume": 70}, headers=h)
            r = c.post("/api/v1/receiver/heartbeat", json={"session_id": "s1"}, headers=h)
            assert r.status_code == 200
            assert r.get_json()["status"] == "alive"

    def test_heartbeat_409_no_session(self, app_paired):
        app, _ = app_paired
        with app.test_client() as c:
            r = c.post("/api/v1/receiver/heartbeat", json={"session_id": "s1"},
                       headers={"Authorization": "Bearer tok_test"})
            assert r.status_code == 409


class TestVolume:
    def test_volume_200(self, app_paired):
        app, _ = app_paired
        with app.test_client() as c:
            r = c.post("/api/v1/receiver/volume", json={"volume": 50},
                       headers={"Authorization": "Bearer tok_test"})
            assert r.status_code == 200
            assert r.get_json()["volume"] == 50

    def test_volume_clamp_high(self, app_paired):
        app, _ = app_paired
        with app.test_client() as c:
            r = c.post("/api/v1/receiver/volume", json={"volume": 999},
                       headers={"Authorization": "Bearer tok_test"})
            assert r.status_code == 200
            assert r.get_json()["volume"] == 100

    def test_volume_400_missing_field(self, app_paired):
        app, _ = app_paired
        with app.test_client() as c:
            r = c.post("/api/v1/receiver/volume", json={},
                       headers={"Authorization": "Bearer tok_test"})
            assert r.status_code == 400
