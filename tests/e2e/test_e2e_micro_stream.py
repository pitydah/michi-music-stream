#!/usr/bin/env python3
"""MS-09 cross-repo E2E: Michi Micro Server (controller) against the
canonical Michi Music Stream receiver simulator, certified against the
vendored michi-link alpha.1 bundle.

Flow (contract sections 2.2-2.6, plan section 6 MS-09): signed discovery
vector; server info; physical pairing window via the internal hook; full
pairing (start/status/confirm) with real Ed25519 verification; canonical
session creation; 100 canonical RTP packets over real UDP to the socket
the simulator binds; RTP rejection classes evidenced through the shared
host-tested rtp_guard.c; heartbeat + replay; volume/pause/resume; DELETE;
new session and lease expiry. Every HTTP response is validated against
the vendored schemas.

Runs against the in-process canonical simulator behind a real TCP server
(see harness.py); the RTP path uses real UDP sockets. The simulator does
not ingest RTP by design (MS-02): transport is evidenced up to the bound
socket and rejection accounting comes from the same rtp_guard.c source
the firmware engine compiles (tests/host/test_rtp_guard.c).
"""

import hashlib
import json
import os
import re
import secrets
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import uuid
from pathlib import Path

import pytest
from cryptography.hazmat.primitives import serialization as ser
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

import harness
from harness import (
    b64url_decode,
    b64url_nopad,
    derive_michi_id,
    discovery_canonical_bytes,
    ed25519_verify,
)
from rtp_client import (
    RTP_PACKET_BYTES,
    RTP_PAYLOAD_BYTES,
    RTP_PAYLOAD_TYPE,
    build_packet,
    parse_packet,
    pcm10ms_payload,
)

REPO_ROOT = harness.REPO_ROOT
BUNDLE_DIR = harness.BUNDLE_DIR

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

RTP_PACKETS = 100
RTP_BATCH = 20


def load_vector(name):
    path = BUNDLE_DIR / "vectors" / name
    return json.loads(path.read_text(encoding="utf-8"))


def test_01_discovery_signed_announce_vector(bundle):
    """Signed discovery announce: schema, identity derivation and Ed25519."""
    valid = load_vector("discovery/announce-valid.json")
    assert bundle.validate("discovery-announce.schema.json", valid) == []
    assert valid["service"] == "michi-stream-standard"
    assert valid["michi_id"] == derive_michi_id(b64url_decode(valid["public_key"]))
    assert ed25519_verify(
        valid["public_key"],
        valid["signature"],
        discovery_canonical_bytes(valid).encode("utf-8"),
    )

    altered = load_vector("discovery/announce-signature-altered.json")
    assert bundle.validate("discovery-announce.schema.json", altered) == []
    assert not ed25519_verify(
        altered["public_key"],
        altered["signature"],
        discovery_canonical_bytes(altered).encode("utf-8"),
    )


def test_02_server_info_canonical(sim, bundle):
    """GET /server/info: canonical profile, identity, no legacy routes."""
    status, info = sim.client.request("GET", "/api/v1/server/info")
    assert status == 200
    assert bundle.validate("server-info.schema.json", info) == []
    assert info["service"] == "michi-stream-standard"
    assert info["api_version"] == "v1-lite"
    assert info["roles"] == ["audio_receiver"]
    assert info["identity_scheme"] == "ed25519-blake3-v1"
    assert info["auth"] == {
        "required": True,
        "strategy": "RECEIVER_BUTTON",
        "token_refresh": False,
    }
    assert info["features"]["session"] is True
    assert info["features"]["heartbeat"] is True
    assert info["features"]["volume"] is True
    assert info["audio"] == {
        "transports": ["rtp_udp"],
        "codecs": ["pcm_s16le"],
        "sample_rates": [48000],
        "bit_depths": [16],
        "channels": [2],
        "packet_ms": [10],
        "payload_types": [97],
        "buffer_ms_min": 50,
        "buffer_ms_max": 500,
    }
    assert uuid.UUID(info["server_id"]).version == 4
    assert info["server_id"] != info["michi_id"]
    assert info["michi_id"] == derive_michi_id(b64url_decode(info["public_key"]))

    status, err = sim.client.request("GET", "/api/v1/receiver/info")
    assert status == 404
    assert bundle.validate("error.schema.json", err) == []
    assert err["error"]["code"] == "NOT_FOUND"


def test_03_pairing_receiver_button_flow(sim, bundle):
    """Full pairing: window hook, signed challenge, PIN, receiver-issued token."""
    client = sim.client
    state = sim.state

    private_key = Ed25519PrivateKey.generate()
    public_bytes = private_key.public_key().public_bytes(
        ser.Encoding.Raw, ser.PublicFormat.Raw
    )
    michi_id = derive_michi_id(public_bytes)
    public_key = b64url_nopad(public_bytes)
    nonce = secrets.token_bytes(16)
    challenge = {
        "device_name": "Michi Micro Server",
        "device_type": "server",
        "roles": ["music_server"],
        "auth_strategy": "RECEIVER_BUTTON",
        "michi_id": michi_id,
        "public_key": public_key,
        "challenge_nonce": b64url_nopad(nonce),
        "challenge_signature": b64url_nopad(private_key.sign(nonce)),
    }
    assert bundle.validate("pair-start.schema.json", challenge) == []

    state.open_pairing_window()

    status, started = client.request("POST", "/api/v1/pair/start", challenge)
    assert status == 201
    assert bundle.validate("pair-start-response.schema.json", started) == []
    assert len(b64url_decode(started["server_public_key"])) == 32
    assert started["server_michi_id"] == derive_michi_id(
        b64url_decode(started["server_public_key"])
    )
    pairing_session_id = started["session_id"]

    status, status_body = client.request(
        "GET", f"/api/v1/pair/status?session_id={pairing_session_id}"
    )
    assert status == 200
    assert bundle.validate("pair-status.schema.json", status_body) == []
    assert status_body["status"] == "pending"

    pin = state.pairing_sessions[pairing_session_id]["pin"]
    assert re.fullmatch(r"[0-9]{6}", pin) is not None

    confirm = {
        "session_id": pairing_session_id,
        "pin": pin,
        "michi_id": michi_id,
        "public_key": public_key,
    }
    assert bundle.validate("pair-confirm.schema.json", confirm) == []

    status, confirmed = client.request("POST", "/api/v1/pair/confirm", confirm)
    assert status == 200
    assert bundle.validate("pair-confirm-response.schema.json", confirmed) == []
    assert confirmed["expires_in"] == 0
    assert confirmed["server_id"] == state.server_id
    token = confirmed["token"]
    assert len(b64url_decode(token)) == 32

    controller = list(state.controllers.values())[0]
    assert controller["token_sha256"] == hashlib.sha256(
        token.encode("utf-8")
    ).hexdigest()
    assert "token" not in controller and "pin" not in controller
    assert controller["michi_id"] == michi_id
    assert controller["public_key"] == public_key
    assert controller["permissions"] == [
        "receiver.status",
        "receiver.session",
        "receiver.volume",
        "receiver.now_playing",
    ]

    status, status_body = client.request(
        "GET", f"/api/v1/pair/status?session_id={pairing_session_id}"
    )
    assert status == 200
    assert status_body["status"] == "confirmed"

    status, err = client.request("POST", "/api/v1/pair/confirm", confirm)
    assert status == 409
    assert bundle.validate("error.schema.json", err) == []

    sim.pairing_token = token


def test_04_session_create_and_rtp_transport(sim, bundle):
    """Canonical session + 100 valid RTP packets over real UDP to the bind."""
    client = sim.client
    state = sim.state
    headers = {"Authorization": f"Bearer {sim.pairing_token}"}

    assert bundle.validate("receiver-session-create.schema.json", SESSION_BODY) == []
    status, created = client.request(
        "POST", "/api/v1/receiver-lite/session", SESSION_BODY, headers
    )
    assert status == 201
    assert bundle.validate("receiver-session.schema.json", created) == []
    assert created["lease_seconds"] == 30
    assert created["effective"] == dict(
        SESSION_BODY, stream_port=created["effective"]["stream_port"]
    )
    assert 49152 <= created["effective"]["stream_port"] <= 65535
    assert created["session_token"] != sim.pairing_token
    assert len(b64url_decode(created["session_token"])) == 32

    sim.session_id = created["session_id"]
    sim.session_token = created["session_token"]
    sim.stream_port = created["effective"]["stream_port"]

    assert state.session["source_ip"] == "127.0.0.1"
    assert state.session["ssrc"] == SESSION_BODY["ssrc"]

    sender = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    receiver_socket = state.stream_socket
    receiver_socket.settimeout(2.0)
    received_seq = []
    for batch in range(RTP_PACKETS // RTP_BATCH):
        for index in range(RTP_BATCH):
            seq = batch * RTP_BATCH + index + 1
            packet = build_packet(
                seq, seq * 480, SESSION_BODY["ssrc"], pcm10ms_payload(seq)
            )
            sender.sendto(packet, ("127.0.0.1", sim.stream_port))
        for _ in range(RTP_BATCH):
            datagram, addr = receiver_socket.recvfrom(65536)
            assert addr[0] == "127.0.0.1"
            assert len(datagram) == RTP_PACKET_BYTES
            parsed = parse_packet(datagram)
            assert parsed["version_byte"] == 0x80
            assert parsed["pt"] == RTP_PAYLOAD_TYPE
            assert parsed["ssrc"] == SESSION_BODY["ssrc"]
            assert parsed["payload_len"] == RTP_PAYLOAD_BYTES
            received_seq.append(parsed["seq"])
    sender.close()
    assert sorted(received_seq) == list(range(1, RTP_PACKETS + 1))

    assert state.packets_received == 0


def test_05_rtp_guard_rejection_classes_host_evidence():
    """Rejection accounting per class: the SAME rtp_guard.c the firmware runs."""
    source_guard = REPO_ROOT / "firmware/components/michi_audio/rtp_guard.c"
    source_test = REPO_ROOT / "tests/host/test_rtp_guard.c"
    include_dir = REPO_ROOT / "firmware/components/michi_audio"
    tmp = tempfile.mkdtemp(prefix="michi-rtp-guard-e2e-")
    binary = os.path.join(tmp, "test_rtp_guard")
    try:
        compile_proc = subprocess.run(
            [
                "cc",
                "-std=c11",
                "-O2",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-D_DEFAULT_SOURCE",
                f"-I{include_dir}",
                "-o",
                binary,
                str(source_test),
                str(source_guard),
            ],
            capture_output=True,
            text=True,
            timeout=120,
        )
        assert compile_proc.returncode == 0, compile_proc.stderr
        run_proc = subprocess.run([binary], capture_output=True, text=True, timeout=120)
        assert run_proc.returncode == 0, run_proc.stdout + run_proc.stderr
        output = run_proc.stdout
        assert "FAIL" not in output
        assert "rtp guard: header rejects v1/CSRC/extension/padding/short" in output
        assert "rtp guard: classification per class" in output
        assert "rtp guard: sequence wrap loss accounting" in output
        assert "rtp_guard: all tests passed" in output
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def test_06_heartbeat_volume_pause_resume(sim, bundle):
    """Heartbeat + replay + renewal; PATCH volume/pause/resume; GET state."""
    client = sim.client
    headers = {
        "Authorization": f"Bearer {sim.pairing_token}",
        "X-Michi-Session": sim.session_token,
    }

    heartbeat = {
        "session_id": sim.session_id,
        "sequence": 1,
        "sent_at_ms": int(time.time() * 1000),
    }
    assert bundle.validate("receiver-heartbeat.schema.json", heartbeat) == []
    status, alive = client.request(
        "POST", "/api/v1/receiver-lite/heartbeat", heartbeat, headers
    )
    assert status == 200
    assert bundle.validate("receiver-heartbeat-response.schema.json", alive) == []
    assert alive["status"] == "alive"
    assert alive["lease_seconds"] == 30

    status, err = client.request(
        "POST", "/api/v1/receiver-lite/heartbeat", heartbeat, headers
    )
    assert status == 409
    assert bundle.validate("error.schema.json", err) == []

    heartbeat["sequence"] = 2
    status, alive = client.request(
        "POST", "/api/v1/receiver-lite/heartbeat", heartbeat, headers
    )
    assert status == 200
    assert alive["status"] == "alive"

    patch = {"volume": 55}
    assert bundle.validate("receiver-session-patch.schema.json", patch) == []
    status, state_body = client.request(
        "PATCH", "/api/v1/receiver-lite/session", patch, headers
    )
    assert status == 200
    assert bundle.validate("receiver-session.schema.json", state_body) == []
    assert state_body["volume"] == 55
    assert state_body["paused"] is False
    assert state_body["state"] == "playing"

    status, state_body = client.request(
        "PATCH", "/api/v1/receiver-lite/session", {"paused": True}, headers
    )
    assert status == 200
    assert state_body["state"] == "paused"
    assert state_body["paused"] is True

    status, state_body = client.request(
        "PATCH", "/api/v1/receiver-lite/session", {"paused": False}, headers
    )
    assert status == 200
    assert state_body["state"] == "playing"
    assert state_body["paused"] is False

    status, state_body = client.request(
        "GET",
        "/api/v1/receiver-lite/session",
        headers={"Authorization": f"Bearer {sim.pairing_token}"},
    )
    assert status == 200
    assert bundle.validate("receiver-session.schema.json", state_body) == []
    assert state_body["volume"] == 55
    assert state_body["paused"] is False
    assert state_body["stream_port"] == sim.stream_port
    assert state_body["ssrc"] == SESSION_BODY["ssrc"]
    assert "session_token" not in state_body


def test_07_delete_and_lease_expiry(sim, bundle):
    """DELETE frees everything; new session; lease expiry closes it."""
    client = sim.client
    state = sim.state
    headers = {
        "Authorization": f"Bearer {sim.pairing_token}",
        "X-Michi-Session": sim.session_token,
    }

    status, body = client.request(
        "DELETE", "/api/v1/receiver-lite/session", headers=headers
    )
    assert status == 204
    assert body is None

    assert state.stream_socket is None
    probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    probe.connect(("127.0.0.1", sim.stream_port))
    refused = False
    for _ in range(10):
        try:
            probe.send(b"late-rtp")
        except OSError:
            refused = True
            break
        time.sleep(0.05)
    probe.close()
    assert refused, "closed RTP port still accepts datagrams"

    status, err = client.request(
        "GET",
        "/api/v1/receiver-lite/session",
        headers={"Authorization": f"Bearer {sim.pairing_token}"},
    )
    assert status == 404
    assert bundle.validate("error.schema.json", err) == []

    status, created = client.request(
        "POST",
        "/api/v1/receiver-lite/session",
        SESSION_BODY,
        {"Authorization": f"Bearer {sim.pairing_token}"},
    )
    assert status == 201
    assert bundle.validate("receiver-session.schema.json", created) == []

    sim.clock.advance(31.0)
    status, err = client.request(
        "GET",
        "/api/v1/receiver-lite/session",
        headers={"Authorization": f"Bearer {sim.pairing_token}"},
    )
    assert status == 404
    assert bundle.validate("error.schema.json", err) == []
    assert state.lease_expirations == 1

    status, created = client.request(
        "POST",
        "/api/v1/receiver-lite/session",
        SESSION_BODY,
        {"Authorization": f"Bearer {sim.pairing_token}"},
    )
    assert status == 201

    cleanup_headers = {
        "Authorization": f"Bearer {sim.pairing_token}",
        "X-Michi-Session": created["session_token"],
    }
    status, _ = client.request(
        "DELETE", "/api/v1/receiver-lite/session", headers=cleanup_headers
    )
    assert status == 204


if __name__ == "__main__":
    sys.exit(pytest.main([str(Path(__file__).resolve()), "-q"]))
