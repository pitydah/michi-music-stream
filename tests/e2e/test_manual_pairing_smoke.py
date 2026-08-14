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

REPO_ROOT = Path(__file__).resolve().parents[2]
SIM_SCRIPT = REPO_ROOT / "simulator" / "receiver_sim.py"
PAIR_START_VECTOR = REPO_ROOT / "contracts" / "michi-link" / "vectors" / "pairing" / "pair-start-valid.json"

PIN_LINE_RE = re.compile(r"\[LOCAL DISPLAY\] Pairing PIN: ([0-9]{6})")
SESSION_LINE_RE = re.compile(
    r"\[LOCAL DISPLAY\] Pairing session: "
    r"([0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12})"
)
TOKEN_RE = re.compile(r"^[A-Za-z0-9_-]{43}$")

START_TIMEOUT_S = 15.0
PIN_TIMEOUT_S = 10.0


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
        self._wait_ready()

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


def load_pair_start_vector():
    with open(PAIR_START_VECTOR, encoding="utf-8") as handle:
        return json.load(handle)


def test_manual_pairing_flow_with_dynamic_pin():
    """The documented manual flow works end to end with REAL runtime data."""
    vector = load_pair_start_vector()
    sim = RunningSimulator(show_local_pin=True)
    try:
        status, started = sim.post("/api/v1/pair/start", vector)
        assert status == 201, started
        response_session = started["session_id"]
        assert "pin" not in started

        pin, display_session = sim.wait_for_local_display()
        assert display_session == response_session, "display session must match pair/start response"

        status, confirmed = sim.post("/api/v1/pair/confirm", {
            "session_id": response_session,
            "pin": pin,
            "michi_id": vector["michi_id"],
            "public_key": vector["public_key"],
        })
        assert status == 200, confirmed
        token = confirmed["token"]
        assert TOKEN_RE.fullmatch(token), "token must be 43-char base64url"

        log_text = sim.log_text()
        assert token not in log_text, "the pairing token must never be printed to logs"

        status, status_body = sim.post("/api/v1/pair/confirm", {
            "session_id": response_session,
            "pin": pin,
            "michi_id": vector["michi_id"],
            "public_key": vector["public_key"],
        })
        assert status == 409, status_body
        assert status_body["error"]["code"] == "PAIRING_ALREADY_CONSUMED"
    finally:
        sim.stop()
    print("PASS manual pairing smoke: dynamic PIN from local display -> real token")


def test_pin_hidden_without_dev_flag():
    """Without --show-local-pairing-pin the PIN never appears in logs."""
    vector = load_pair_start_vector()
    sim = RunningSimulator(show_local_pin=False)
    try:
        status, started = sim.post("/api/v1/pair/start", vector)
        assert status == 201, started
        time.sleep(1.0)
        log_text = sim.log_text()
        assert "[LOCAL DISPLAY]" not in log_text
        assert "Pairing session created" in log_text
        assert not PIN_LINE_RE.search(log_text)
        assert not SESSION_LINE_RE.search(log_text)
    finally:
        sim.stop()
    print("PASS manual pairing smoke: PIN hidden without the dev-only flag")


def run():
    tests = [
        test_manual_pairing_flow_with_dynamic_pin,
        test_pin_hidden_without_dev_flag,
    ]
    ok = 0
    for t in tests:
        try:
            t()
            ok += 1
        except Exception as e:
            print(f"  FAIL {t.__name__}: {e}")
    print(f"\n{ok}/{len(tests)} manual pairing smoke cases passed")
    return ok == len(tests)


if __name__ == "__main__":
    sys.exit(0 if run() else 1)
