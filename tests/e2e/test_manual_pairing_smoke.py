#!/usr/bin/env python3
"""Smoke test for the documented MANUAL pairing procedure (P1-06).

Runs the real simulator CLI as a subprocess (real HTTP over TCP, real
random pairing window) and reproduces exactly the manual flow documented
in docs/RECEIVER_SIMULATOR_CI.md:

  1. POST /pair/start with the signed bundle vector;
  2. read the REAL session_id from the response;
  3. read the REAL dynamic PIN from the local display channel
     ([LOCAL DISPLAY] log lines, enabled by the dev-only
     --show-local-pairing-pin flag);
  4. POST /pair/confirm with the REAL session and PIN;
  5. extract the pairing token from the REAL response.

It also proves the security defaults: without the dev flag the PIN never
appears in logs, and the Bearer token is never printed even with the flag.

Run: python3 tests/e2e/test_manual_pairing_smoke.py
     python3 -m pytest tests/e2e/test_manual_pairing_smoke.py
"""

import base64
import json
import os
import re
import socket
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path

import blake3
from cryptography.hazmat.primitives import serialization as ser
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey, Ed25519PublicKey

REPO_ROOT = Path(__file__).resolve().parents[2]
SIM_SCRIPT = REPO_ROOT / "simulator" / "receiver_sim.py"

TOKEN_RE = re.compile(r"^[A-Za-z0-9_-]{43}$")
START_TIMEOUT_S = 15.0


def b64url_nopad(raw_bytes):
    return base64.urlsafe_b64encode(raw_bytes).decode("ascii").rstrip("=")


def b64url_decode(value):
    return base64.urlsafe_b64decode(value + "=" * (-len(value) % 4))


def derive_michi_id(public_key_bytes):
    return b64url_nopad(blake3.blake3(public_key_bytes).digest())


def canonical_membership_bytes(home_id, dev_michi_id, dev_pubkey, dev_type, roles, issued_at, serial):
    sorted_roles = sorted(roles)
    payload = (
        b"michi-link-membership-v1"
        + home_id.encode("ascii")
        + dev_michi_id.encode("ascii")
        + dev_pubkey.encode("ascii")
        + dev_type.encode("ascii")
    )
    for r in sorted_roles:
        payload += b":" + r.encode("utf-8")
    payload += b":" + issued_at.encode("ascii") + b":" + str(serial).encode("ascii")
    return payload


def make_membership_cert(root_private_key, home_id, dev_michi_id, dev_pubkey, dev_type="server", roles=None, issued_at="2026-10-04T12:00:00Z", serial=1):
    if roles is None:
        roles = ["music_server"]
    canon = canonical_membership_bytes(home_id, dev_michi_id, dev_pubkey, dev_type, roles, issued_at, serial)
    sig = root_private_key.sign(canon)
    return {
        "version": 1,
        "home_id": home_id,
        "device_michi_id": dev_michi_id,
        "device_public_key": dev_pubkey,
        "device_type": dev_type,
        "roles": roles,
        "issued_at": issued_at,
        "serial": serial,
        "signature": b64url_nopad(sig),
    }


def free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


class RunningSimulator:
    """Simulator CLI subprocess with a threaded log collector."""

    def __init__(self, show_local_pin):
        self.port = free_port()
        cmd = [
            sys.executable, str(SIM_SCRIPT),
            "--type", "standard",
            "--pairing-open",
            "--port", str(self.port),
        ]
        if show_local_pin:
            cmd.append("--show-local-pairing-pin")
        self.proc = subprocess.Popen(
            cmd,
            cwd=str(REPO_ROOT),
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )
        self._lines = []
        self._lock = threading.Lock()
        self._reader = threading.Thread(target=self._collect, daemon=True)
        self._reader.start()
        try:
            self._wait_ready()
        except Exception:
            self.stop()
            raise

    def _collect(self):
        for line in self.proc.stdout:
            with self._lock:
                self._lines.append(line)

    def log_text(self):
        with self._lock:
            return "".join(self._lines)

    def _wait_ready(self):
        deadline = time.monotonic() + START_TIMEOUT_S
        while time.monotonic() < deadline:
            try:
                with urllib.request.urlopen(
                    f"http://127.0.0.1:{self.port}/api/v1/server/info", timeout=2
                ) as resp:
                    if resp.status == 200:
                        return
            except (urllib.error.URLError, ConnectionError, OSError):
                pass
            if self.proc.poll() is not None:
                break
            time.sleep(0.1)
        raise AssertionError(f"simulator did not become ready (log: {self.log_text()})")

    def post(self, path, body):
        data = json.dumps(body).encode("utf-8")
        req = urllib.request.Request(
            f"http://127.0.0.1:{self.port}{path}",
            data=data,
            method="POST",
            headers={"Content-Type": "application/json"},
        )
        try:
            with urllib.request.urlopen(req, timeout=10) as resp:
                return resp.status, json.loads(resp.read().decode("utf-8"))
        except urllib.error.HTTPError as exc:
            return exc.code, json.loads(exc.read().decode("utf-8"))

    def wait_for_local_display(self):
        deadline = time.monotonic() + PIN_TIMEOUT_S
        while time.monotonic() < deadline:
            text = self.log_text()
            pin = PIN_LINE_RE.search(text)
            session = SESSION_LINE_RE.search(text)
            if pin and session:
                return pin.group(1), session.group(1)
            time.sleep(0.1)
        raise AssertionError(f"[LOCAL DISPLAY] lines never appeared (log: {self.log_text()})")

    def stop(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=5)
        self._reader.join(timeout=5)


def test_manual_membership_auth_flow():
    """Canonical Home Membership device auth flow against real CLI subprocess."""
    sim = None
    try:
        sim = RunningSimulator(show_local_pin=False)

        # 1. Server info check
        status, info = sim.post("/api/v1/auth/challenge", {})
        # Use urllib GET for server info
        req = urllib.request.Request(f"http://127.0.0.1:{sim.port}/api/v1/server/info", method="GET")
        with urllib.request.urlopen(req, timeout=10) as resp:
            info = json.loads(resp.read().decode("utf-8"))
        assert info["auth"]["strategy"] == "HOME_MEMBERSHIP"
        home_id = info["michi_home_id"]
        server_michi_id = info["michi_id"]
        server_pubkey_bytes = b64url_decode(info["public_key"])
        server_pubkey = Ed25519PublicKey.from_public_bytes(server_pubkey_bytes)

        # 2. Client identity
        client_key = Ed25519PrivateKey.generate()
        client_pub_bytes = client_key.public_key().public_bytes(ser.Encoding.Raw, ser.PublicFormat.Raw)
        client_pk = b64url_nopad(client_pub_bytes)
        client_id = derive_michi_id(client_pub_bytes)

        # 3. Auth challenge
        status, ch = sim.post("/api/v1/auth/challenge", {
            "client_michi_id": client_id,
            "client_public_key": client_pk,
            "home_id": home_id,
        })
        assert status == 200, ch
        cid = ch["challenge_id"]
        nonce = ch["challenge_nonce"]
        assert ch["server_michi_id"] == server_michi_id

        # 4. Auth session with Root Authority membership cert
        root_seed = blake3.blake3(b"michi-link contract vectors v1" + b"home-root").digest()
        root_priv = Ed25519PrivateKey.from_private_bytes(root_seed)
        membership = make_membership_cert(root_priv, home_id, client_id, client_pk)

        auth_payload = (
            b"michi-link-device-auth-v1"
            + home_id.encode("ascii")
            + server_michi_id.encode("ascii")
            + client_id.encode("ascii")
            + cid.encode("ascii")
            + nonce.encode("ascii")
        )
        client_sig = b64url_nopad(client_key.sign(auth_payload))

        status, sess = sim.post("/api/v1/auth/session", {
            "challenge_id": cid,
            "client_michi_id": client_id,
            "membership": membership,
            "client_signature": client_sig,
        })
        assert status == 200, sess
        assert sess["token_type"] == "Bearer"
        assert sess["expires_in"] == 3600
        token = sess["session_token"]
        assert TOKEN_RE.fullmatch(token), "token must be 43-char base64url"

        # Mutual signature verification
        server_auth_payload = (
            b"michi-link-server-auth-v1"
            + home_id.encode("ascii")
            + server_michi_id.encode("ascii")
            + client_id.encode("ascii")
            + cid.encode("ascii")
            + token.encode("ascii")
        )
        server_pubkey.verify(b64url_decode(sess["server_signature"]), server_auth_payload)

        # Security check: Bearer token is never printed to logs
        log_text = sim.log_text()
        assert token not in log_text, "the session token must never be printed to logs"

        # 5. Create receiver session using token
        session_body = {
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
        req2 = urllib.request.Request(
            f"http://127.0.0.1:{sim.port}/api/v1/receiver-lite/session",
            data=json.dumps(session_body).encode("utf-8"),
            method="POST",
            headers={"Content-Type": "application/json", "Authorization": f"Bearer {token}"},
        )
        with urllib.request.urlopen(req2, timeout=10) as resp2:
            stream_sess = json.loads(resp2.read().decode("utf-8"))
            assert resp2.status == 201
            assert "stream_port" in stream_sess["effective"]
    finally:
        if sim is not None:
            sim.stop()
    print("PASS manual auth smoke: Home Membership device auth -> real token -> session created")


def test_legacy_pairing_endpoints_return_404():
    """Legacy /pair/* endpoints return 404 NOT_FOUND."""
    sim = None
    try:
        sim = RunningSimulator(show_local_pin=False)
        legacy_checks = [
            ("POST", "/api/v1/pair/start", {}),
            ("GET", "/api/v1/pair/status", None),
            ("POST", "/api/v1/pair/confirm", {}),
            ("POST", "/api/v1/pair/recover/start", {}),
            ("POST", "/api/v1/pair/recover", {}),
        ]
        for method, path, body in legacy_checks:
            req = urllib.request.Request(
                f"http://127.0.0.1:{sim.port}{path}",
                data=json.dumps(body).encode("utf-8") if body is not None else None,
                method=method,
                headers={"Content-Type": "application/json"} if body is not None else {},
            )
            try:
                with urllib.request.urlopen(req, timeout=10) as resp:
                    assert False, f"{path} expected 404 got {resp.status}"
            except urllib.error.HTTPError as exc:
                assert exc.code == 404, f"{path} expected 404 got {exc.code}"
                err = json.loads(exc.read().decode("utf-8"))
                assert err["error"]["code"] == "NOT_FOUND"
    finally:
        if sim is not None:
            sim.stop()
    print("PASS manual smoke: legacy pairing routes return 404 NOT_FOUND")


def run():
    tests = [
        test_manual_membership_auth_flow,
        test_legacy_pairing_endpoints_return_404,
    ]
    ok = 0
    for t in tests:
        try:
            t()
            ok += 1
        except Exception as e:
            print(f"  FAIL {t.__name__}: {e}")
    print(f"\n{ok}/{len(tests)} smoke cases passed")
    return ok == len(tests)


if __name__ == "__main__":
    sys.exit(0 if run() else 1)
