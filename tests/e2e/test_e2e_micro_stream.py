#!/usr/bin/env python3
"""
E2E test: Micro Server ↔ Michi Music Stream Simulator.

Orquesta el receiver simulator y ejecuta el flujo completo:
pairing → session/start → heartbeat → volume → session/stop.

Requisitos:
  pip install flask requests

Uso:
  python3 test_e2e_micro_stream.py
  python3 test_e2e_micro_stream.py --stream-port 53319 --micro-url http://localhost:8096
"""

import argparse
import json
import os
import subprocess
import sys
import time
import urllib.request
import urllib.error

STREAM_PORT = 53319
PASS = 0
FAIL = 0

def test(name, got, expected, desc=""):
    global PASS, FAIL
    if got == expected:
        PASS += 1
        print(f"  PASS {name}")
    else:
        FAIL += 1
        print(f"  FAIL {name}: expected {expected}, got {got}. {desc}")

def req(method, url, body=None, headers=None, timeout=5):
    data = json.dumps(body).encode() if body else None
    r = urllib.request.Request(url, data=data, method=method)
    r.add_header("Content-Type", "application/json")
    if headers:
        for k, v in headers.items():
            r.add_header(k, v)
    try:
        resp = urllib.request.urlopen(r, timeout=timeout)
        return resp.status, json.loads(resp.read())
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read())
    except Exception as e:
        return 0, {"error": {"code": "network_error", "message": str(e)}}


def launch_sim(sim_dir, sim_port, pairing_open=True):
    cmd = [sys.executable, "receiver_sim.py", "--type", "standard",
           "--port", str(sim_port)]
    if pairing_open:
        cmd.append("--pairing-open")
    proc = subprocess.Popen(cmd, cwd=sim_dir,
                            stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    time.sleep(2)
    if proc.poll() is not None:
        print("FAIL: simulator failed to start")
        sys.exit(1)
    return proc

def kill_sim(proc):
    proc.terminate()
    try:
        proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        proc.kill()


def main():
    parser = argparse.ArgumentParser(description="E2E Micro ↔ Stream test")
    parser.add_argument("--stream-port", type=int, default=STREAM_PORT)
    args = parser.parse_args()

    sim_port = args.stream_port
    sim_url = f"http://127.0.0.1:{sim_port}"
    sim_dir = os.path.normpath(os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "..", "simulator"))

    global PASS, FAIL
    PASS = 0
    FAIL = 0

    print(f"Stream simulator dir: {sim_dir}")
    print(f"Stream simulator URL: {sim_url}")
    print()

    proc = launch_sim(sim_dir, sim_port, pairing_open=True)

    try:
        # ── 1. GET /api/v1/receiver/info ──────────────────────
        print("--- 1. GET /api/v1/receiver/info ---")
        code, data = req("GET", f"{sim_url}/api/v1/receiver/info")
        test("info returns 200", code, 200)
        if code == 200:
            test("service is michi-stream-standard",
                 data.get("service"), "michi-stream-standard")
            test("auth required is true", data.get("auth", {}).get("required"), True)
            test("auth strategy", data.get("auth", {}).get("strategy"), "RECEIVER_BUTTON")
        print()

        # ── 2. Pairing ya abierto (--pairing-open) ────────────
        print("--- 2. POST /api/v1/receiver/pair/start (already open) ---")
        code, data = req("POST", f"{sim_url}/api/v1/receiver/pair/start",
                         {"initiator": "t", "initiator_id": "m1"})
        test("pair/start detects already open -> 409", code, 409)
        test("error code is pairing_window_open",
             data.get("error", {}).get("code"), "pairing_window_open")
        print()
    finally:
        kill_sim(proc)

    # ── Reiniciar sin --pairing-open para flujo controlado ────
    proc2 = launch_sim(sim_dir, sim_port, pairing_open=False)

    try:
        # ── 3. Pair/start exitoso ─────────────────────────────
        print("--- 3. POST /api/v1/receiver/pair/start ---")
        code, data = req("POST", f"{sim_url}/api/v1/receiver/pair/start",
                         {"initiator": "michi_micro_server", "initiator_id": "micro_e2e"})
        test("pair/start returns 200", code, 200)
        nonce = data.get("nonce", "")
        test("nonce is not empty", bool(nonce), True)
        print()

        # ── 4. Pair/confirm ───────────────────────────────────
        print("--- 4. POST /api/v1/receiver/pair/confirm ---")
        code, data = req("POST", f"{sim_url}/api/v1/receiver/pair/confirm",
                         {"nonce": nonce, "initiator_id": "micro_e2e",
                          "token": "tok_e2e_test"})
        test("pair/confirm returns 200", code, 200)
        test("status is paired", data.get("status"), "paired")
        token = data.get("token", "")
        test("token is present", bool(token), True)
        print()

        # ── 5. Session/start ──────────────────────────────────
        print("--- 5. POST /api/v1/receiver/session/start ---")
        session_body = {
            "session_id": "sess_e2e_001",
            "codec": "pcm_s16le",
            "sample_rate": 48000,
            "bit_depth": 16,
            "channels": 2,
            "transport": "udp",
            "stream_port": 55300,
            "buffer_ms": 250,
            "volume": 70,
        }
        auth = {"Authorization": f"Bearer {token}"}
        code, data = req("POST", f"{sim_url}/api/v1/receiver/session/start",
                         session_body, auth)
        test("session/start returns 200", code, 200)
        test("status is session_started", data.get("status"), "session_started")
        print()

        # ── 6. Duplicate session ──────────────────────────────
        print("--- 6. POST /api/v1/receiver/session/start (duplicate) ---")
        code, data = req("POST", f"{sim_url}/api/v1/receiver/session/start",
                         session_body, auth)
        test("duplicate session returns 409", code, 409)
        test("error code is session_active",
             data.get("error", {}).get("code"), "session_active")
        print()

        # ── 7. Heartbeat ──────────────────────────────────────
        print("--- 7. POST /api/v1/receiver/heartbeat ---")
        code, data = req("POST", f"{sim_url}/api/v1/receiver/heartbeat",
                         {"session_id": "sess_e2e_001"}, auth)
        test("heartbeat returns 200", code, 200)
        test("status is alive", data.get("status"), "alive")
        test("uptime >= 0", data.get("uptime_seconds", -1) >= 0, True)
        print()

        # ── 8. Volume ─────────────────────────────────────────
        print("--- 8. POST /api/v1/receiver/volume ---")
        code, data = req("POST", f"{sim_url}/api/v1/receiver/volume",
                         {"volume": 42}, auth)
        test("volume returns 200", code, 200)
        test("volume is 42", data.get("volume"), 42)
        print()

        # ── 9. Volume clamp ───────────────────────────────────
        print("--- 9. POST /api/v1/receiver/volume (out of range) ---")
        code, data = req("POST", f"{sim_url}/api/v1/receiver/volume",
                         {"volume": 999}, auth)
        test("volume clamp returns 200", code, 200)
        test("volume clamped to 100", data.get("volume"), 100)
        print()

        # ── 10. Session/stop ──────────────────────────────────
        print("--- 10. POST /api/v1/receiver/session/stop ---")
        code, data = req("POST", f"{sim_url}/api/v1/receiver/session/stop",
                         {"session_id": "sess_e2e_001"}, auth)
        test("session/stop returns 200", code, 200)
        test("status is session_stopped", data.get("status"), "session_stopped")
        print()

        # ── 11. Pairing fuera de ventana ──────────────────────
        print("--- 11. POST /api/v1/receiver/pair/confirm (window closed) ---")
        code, data = req("POST", f"{sim_url}/api/v1/receiver/pair/confirm",
                         {"nonce": "x", "initiator_id": "micro_e2e",
                          "token": "tok_test"})
        test("confirm without window returns 409", code, 409)
        test("error code is pairing_window_closed",
             data.get("error", {}).get("code"), "pairing_window_closed")
        print()

        # ── 12. Auth invalido ─────────────────────────────────
        print("--- 12. POST /api/v1/receiver/volume (invalid token) ---")
        code, data = req("POST", f"{sim_url}/api/v1/receiver/volume",
                         {"volume": 50},
                         {"Authorization": "Bearer invalid_token"})
        test("invalid token returns 401", code, 401)
        test("error code is invalid_token",
             data.get("error", {}).get("code"), "invalid_token")
        print()

    finally:
        kill_sim(proc2)

    print(f"=== Results: {PASS}/{PASS+FAIL} E2E tests passed ===")
    return 0 if FAIL == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
