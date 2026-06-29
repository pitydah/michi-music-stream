#!/usr/bin/env python3
"""
Michi Music Stream Simulator — v1-lite prototype.

Simula un receptor Michi Music Stream (Standard o Hi-Fi) completo con:
  - GET  /api/v1/receiver/info
  - POST /api/v1/receiver/pair/start
  - POST /api/v1/receiver/pair/confirm
  - POST /api/v1/receiver/heartbeat
  - POST /api/v1/receiver/session/start
  - POST /api/v1/receiver/session/stop
  - POST /api/v1/receiver/volume

Uso:
  python3 receiver_sim.py --config config.json
  python3 receiver_sim.py --type standard
  python3 receiver_sim.py --type hifi --port 8081

Requiere: flask
"""

import argparse
import json
import logging
import os
import secrets
import sys
import time
from datetime import datetime, timezone

try:
    from flask import Flask, request, jsonify
except ImportError:
    print("ERROR: flask is required. Install with: pip install flask")
    sys.exit(1)

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
    datefmt="%Y-%m-%dT%H:%M:%S",
)
log = logging.getLogger("michi-sim")

# ── Default configurations ──────────────────────────────────

STANDARD_CONFIG = {
    "service": "michi-stream-standard",
    "name": "Michi Stream Sim Standard",
    "device_id": "rcv_sim_standard_001",
    "api_version": "v1-lite",
    "michi_link_version": "1.0.0-alpha",
    "firmware": "0.1.0-sim",
    "type": "michi_stream_standard",
    "roles": ["audio_receiver", "music_stream_receiver"],
    "auth": {"required": True, "strategy": "RECEIVER_BUTTON", "token_refresh": False},
    "output": {
        "connector": "jack_3_5",
        "max_sample_rate": 48000,
        "max_bit_depth": 16,
        "channels": 2,
    },
    "supported_codecs": ["pcm_s16le", "opus"],
    "features": {"pairing_button": True, "volume": True, "heartbeat": True, "ota_update": False},
}

HIFI_CONFIG = {
    "service": "michi-stream-hifi",
    "name": "Michi Stream Sim Hi-Fi",
    "device_id": "rcv_sim_hifi_001",
    "api_version": "v1-lite",
    "michi_link_version": "1.0.0-alpha",
    "firmware": "0.1.0-sim",
    "type": "michi_stream_hifi",
    "roles": ["audio_receiver", "music_stream_receiver"],
    "auth": {"required": True, "strategy": "RECEIVER_BUTTON", "token_refresh": False},
    "output": {
        "connector": "rca_stereo",
        "dac": "hifi_i2s",
        "max_sample_rate": 96000,
        "max_bit_depth": 24,
        "channels": 2,
    },
    "supported_codecs": ["pcm_s16le", "pcm_s24le", "opus"],
    "features": {"pairing_button": True, "volume": True, "heartbeat": True, "ota_update": True},
}


# ── SimulatorState ──────────────────────────────────────────

class SimulatorState:
    def __init__(self, config: dict):
        self.config = config
        self.device_id = config["device_id"]
        self.max_sr = config["output"]["max_sample_rate"]
        self.max_bd = config["output"]["max_bit_depth"]
        self.supported_codecs = config["supported_codecs"]

        # Pairing
        self.window_open = False
        self.window_expires = 0.0
        self.current_nonce = ""
        self.controllers = {}  # initiator_id -> token

        # Session
        self.session_active = False
        self.session_id = ""
        self.session_codec = ""
        self.session_sr = 0
        self.session_bd = 0
        self.session_ch = 0
        self.session_port = 0
        self.session_buffer = 0
        self.volume = 70

        # Heartbeat
        self.last_heartbeat = 0.0
        self.session_started_at = 0.0

        # Logging
        log.info("Simulator initialized: %s (%s)", config["device_id"], config["type"])
        log.info("  service: %s", config["service"])
        log.info("  max sample_rate: %d, max bit_depth: %d", self.max_sr, self.max_bd)
        log.info("  codecs: %s", self.supported_codecs)

    def info(self) -> dict:
        return self.config

    # ── Pairing ─────────────────────────────────────────────

    def pair_start(self, initiator_id: str) -> tuple:
        if self.window_open:
            remaining = max(0, int(self.window_expires - time.time()))
            return (
                409,
                {
                    "error": {
                        "code": "pairing_window_open",
                        "message": "Ya hay una ventana de pairing activa.",
                        "details": {"remaining_seconds": remaining},
                    }
                },
            )
        self.window_open = True
        self.window_expires = time.time() + 120
        self.current_nonce = secrets.token_hex(8)
        log.info("Pairing window OPEN (nonce=%s, expires in 120s)", self.current_nonce)
        return (
            200,
            {
                "status": "pairing_window_open",
                "device_id": self.device_id,
                "pairing_window_seconds": 120,
                "nonce": self.current_nonce,
            },
        )

    def pair_confirm(self, nonce: str, initiator_id: str, token: str) -> tuple:
        if not self.window_open:
            return (
                409,
                {
                    "error": {
                        "code": "pairing_window_closed",
                        "message": "La ventana de pairing no esta abierta. Presione el boton fisico.",
                        "details": {},
                    }
                },
            )
        if nonce != self.current_nonce:
            return (
                409,
                {
                    "error": {
                        "code": "pairing_window_closed",
                        "message": "Nonce invalido.",
                        "details": {},
                    }
                },
            )
        self.controllers[initiator_id] = token
        self.window_open = False
        self.current_nonce = ""
        log.info("Pairing CONFIRMED: controller=%s token=%s", initiator_id, token)
        return (
            200,
            {
                "status": "paired",
                "device_id": self.device_id,
                "controller_id": initiator_id,
                "token": token,
            },
        )

    def validate_token(self, token: str) -> bool:
        return token in self.controllers.values()

    # ── Session ─────────────────────────────────────────────

    def session_start(
        self, session_id: str, codec: str, sample_rate: int,
        bit_depth: int, channels: int, stream_port: int,
        buffer_ms: int, volume: int
    ) -> tuple:
        if self.session_active:
            return (
                409,
                {
                    "error": {
                        "code": "session_active",
                        "message": "Ya hay una sesion de audio activa.",
                        "details": {"active_session_id": self.session_id},
                    }
                },
            )
        if codec not in self.supported_codecs:
            return (
                400,
                {
                    "error": {
                        "code": "unsupported_codec",
                        "message": f"Codec '{codec}' no soportado. Codecs: {self.supported_codecs}.",
                        "details": {
                            "requested_codec": codec,
                            "supported_codecs": self.supported_codecs,
                        },
                    }
                },
            )
        if sample_rate > self.max_sr:
            return (
                400,
                {
                    "error": {
                        "code": "unsupported_rate",
                        "message": f"Sample rate {sample_rate} excede el maximo {self.max_sr}.",
                        "details": {"requested_rate": sample_rate, "max_rate": self.max_sr},
                    }
                },
            )
        if bit_depth > self.max_bd:
            return (
                400,
                {
                    "error": {
                        "code": "bad_request",
                        "message": f"Bit depth {bit_depth} excede el maximo {self.max_bd}.",
                        "details": {"requested_depth": bit_depth, "max_depth": self.max_bd},
                    }
                },
            )
        if channels != 2:
            return (
                400,
                {
                    "error": {
                        "code": "bad_request",
                        "message": "Solo se soporta estereo (channels=2).",
                        "details": {"requested_channels": channels},
                    }
                },
            )
        vol = max(0, min(100, volume))

        self.session_active = True
        self.session_id = session_id
        self.session_codec = codec
        self.session_sr = sample_rate
        self.session_bd = bit_depth
        self.session_ch = channels
        self.session_port = stream_port
        self.session_buffer = buffer_ms
        self.volume = vol
        self.last_heartbeat = time.time()
        self.session_started_at = time.time()

        log.info(
            "Session STARTED: id=%s codec=%s %dHz %dbit ch=%d port=%d buf=%dms vol=%d",
            session_id, codec, sample_rate, bit_depth, channels,
            stream_port, buffer_ms, vol,
        )
        return (
            200,
            {
                "status": "session_started",
                "session_id": session_id,
                "device_id": self.device_id,
                "stream_port": stream_port,
                "buffer_ms": buffer_ms,
            },
        )

    def session_stop(self) -> tuple:
        if not self.session_active:
            return (409, {"error": {"code": "bad_request", "message": "No hay sesion activa.", "details": {}}})
        sid = self.session_id
        self.session_active = False
        self.session_id = ""
        log.info("Session STOPPED: id=%s", sid)
        return (200, {"status": "session_stopped", "session_id": sid})

    # ── Heartbeat ───────────────────────────────────────────

    def heartbeat(self) -> tuple:
        if not self.session_active:
            return (409, {"error": {"code": "bad_request", "message": "No hay sesion activa.", "details": {}}})
        self.last_heartbeat = time.time()
        uptime = int(time.time() - self.session_started_at)
        log.info("Heartbeat received (uptime=%ds)", uptime)
        return (200, {"status": "alive", "session_id": self.session_id, "uptime_seconds": uptime})

    # ── Volume ──────────────────────────────────────────────

    def set_volume(self, vol: int) -> tuple:
        self.volume = max(0, min(100, vol))
        log.info("Volume changed to %d", self.volume)
        return (200, {"status": "volume_set", "volume": self.volume})

    # ── Firmware ────────────────────────────────────────────

    def firmware(self) -> dict:
        return {
            "device_id": self.device_id,
            "current_version": "0.1.0-sim",
            "build_date": "2026-06-29",
            "ota_supported": self.config["features"]["ota_update"],
        }


# ── Flask app factory ───────────────────────────────────────

def create_app(state: SimulatorState) -> Flask:
    app = Flask(__name__)

    def _auth():
        token = request.headers.get("Authorization", "")
        if not token.startswith("Bearer "):
            return None
        return token[7:]

    def _auth_required(endpoint_name: str):
        token = _auth()
        if token is None or not state.validate_token(token):
            log.warning("Auth FAILED for %s (token=%s)", endpoint_name, token)
            return jsonify({
                "error": {
                    "code": "invalid_token",
                    "message": "Token de autorizacion invalido o ausente.",
                    "details": {},
                }
            }), 401
        return None

    # ── GET /api/v1/receiver/info ───────────────────────────

    @app.route("/api/v1/receiver/info", methods=["GET"])
    def get_info():
        return jsonify(state.info())

    # ── GET /api/v1/receiver/firmware ───────────────────────

    @app.route("/api/v1/receiver/firmware", methods=["GET"])
    def get_firmware():
        return jsonify(state.firmware())

    # ── POST /api/v1/receiver/pair/start ────────────────────

    @app.route("/api/v1/receiver/pair/start", methods=["POST"])
    def post_pair_start():
        data = request.get_json(silent=True) or {}
        init_id = data.get("initiator_id", "")
        if not init_id:
            return jsonify({
                "error": {"code": "bad_request", "message": "Falta initiator_id.", "details": {}}
            }), 400
        status, resp = state.pair_start(init_id)
        return jsonify(resp), status

    # ── POST /api/v1/receiver/pair/confirm ──────────────────

    @app.route("/api/v1/receiver/pair/confirm", methods=["POST"])
    def post_pair_confirm():
        data = request.get_json(silent=True) or {}
        nonce = data.get("nonce", "")
        init_id = data.get("initiator_id", "")
        token = data.get("token", "")
        if not nonce or not init_id or not token:
            return jsonify({
                "error": {
                    "code": "bad_request",
                    "message": "Faltan campos: nonce, initiator_id, token.",
                    "details": {},
                }
            }), 400
        status, resp = state.pair_confirm(nonce, init_id, token)
        return jsonify(resp), status

    # ── POST /api/v1/receiver/heartbeat ─────────────────────

    @app.route("/api/v1/receiver/heartbeat", methods=["POST"])
    def post_heartbeat():
        guard = _auth_required("heartbeat")
        if guard:
            return guard
        status, resp = state.heartbeat()
        return jsonify(resp), status

    # ── POST /api/v1/receiver/session/start ─────────────────

    @app.route("/api/v1/receiver/session/start", methods=["POST"])
    def post_session_start():
        guard = _auth_required("session/start")
        if guard:
            return guard
        data = request.get_json(silent=True) or {}
        required = ["session_id", "codec", "sample_rate", "bit_depth", "channels", "stream_port", "buffer_ms"]
        for f in required:
            if f not in data:
                return jsonify({
                    "error": {"code": "bad_request", "message": f"Falta campo '{f}'.", "details": {}}
                }), 400
        status, resp = state.session_start(
            session_id=data["session_id"],
            codec=data["codec"],
            sample_rate=int(data["sample_rate"]),
            bit_depth=int(data["bit_depth"]),
            channels=int(data["channels"]),
            stream_port=int(data["stream_port"]),
            buffer_ms=int(data["buffer_ms"]),
            volume=int(data.get("volume", 70)),
        )
        return jsonify(resp), status

    # ── POST /api/v1/receiver/session/stop ──────────────────

    @app.route("/api/v1/receiver/session/stop", methods=["POST"])
    def post_session_stop():
        guard = _auth_required("session/stop")
        if guard:
            return guard
        status, resp = state.session_stop()
        return jsonify(resp), status

    # ── POST /api/v1/receiver/volume ────────────────────────

    @app.route("/api/v1/receiver/volume", methods=["POST"])
    def post_volume():
        guard = _auth_required("volume")
        if guard:
            return guard
        data = request.get_json(silent=True) or {}
        if "volume" not in data:
            return jsonify({
                "error": {"code": "bad_request", "message": "Falta campo 'volume'.", "details": {}}
            }), 400
        status, resp = state.set_volume(int(data["volume"]))
        return jsonify(resp), status

    return app


# ── CLI ─────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="Michi Music Stream Simulator v1-lite")
    parser.add_argument("--config", "-c", type=str, help="Path to JSON config file")
    parser.add_argument("--type", "-t", choices=["standard", "hifi"], default="standard",
                        help="Receiver type (ignored if --config is given)")
    parser.add_argument("--port", "-p", type=int, default=8080, help="HTTP port")
    parser.add_argument("--host", default="0.0.0.0", help="Bind address")
    parser.add_argument("--debug", action="store_true", help="Flask debug mode")

    # Initial state flags
    parser.add_argument("--pairing-open", action="store_true",
                        help="Start with pairing window already open (120s)")
    parser.add_argument("--pairing-closed", action="store_true",
                        help="Start with pairing window closed (default)")
    parser.add_argument("--active-session", action="store_true",
                        help="Start with an active audio session (implies pairing)")
    parser.add_argument("--fail-heartbeat", action="store_true",
                        help="Start with session but heartbeat will fail immediately")

    args = parser.parse_args()

    if args.config:
        with open(args.config) as f:
            cfg = json.load(f)
    else:
        cfg = HIFI_CONFIG if args.type == "hifi" else STANDARD_CONFIG

    state = SimulatorState(cfg)

    # Aplicar modos iniciales
    if args.pairing_open:
        state.pair_start("preinit")
        log.info("  initial mode: pairing window OPEN")

    if args.active_session or args.fail_heartbeat:
        # Pair and start a session
        if not args.pairing_open:
            state.pair_start("preinit")
        nonce = state.current_nonce
        state.pair_confirm(nonce, "preinit", "tok_preinit")
        code, _ = state.session_start(
            "sess_preinit", "pcm_s16le", 48000, 16, 2, 55300, 250, 70,
        )
        if code == 200:
            log.info("  initial mode: active session (sess_preinit)")
            log.info("  auth token: tok_preinit")

        if args.fail_heartbeat:
            state.last_heartbeat = 0.0  # heartbeat nunca se envió
            log.info("  initial mode: heartbeat WILL FAIL (last_heartbeat=0)")

    app = create_app(state)

    log.info("=" * 60)
    log.info("Michi Music Stream Simulator v1-lite")
    log.info("  device_id: %s", cfg["device_id"])
    log.info("  type:      %s", cfg["type"])
    log.info("  listening: http://%s:%d", args.host, args.port)
    log.info("  endpoints:")
    log.info("    GET  /api/v1/receiver/info")
    log.info("    GET  /api/v1/receiver/firmware")
    log.info("    POST /api/v1/receiver/pair/start")
    log.info("    POST /api/v1/receiver/pair/confirm")
    log.info("    POST /api/v1/receiver/heartbeat      [auth]")
    log.info("    POST /api/v1/receiver/session/start  [auth]")
    log.info("    POST /api/v1/receiver/session/stop   [auth]")
    log.info("    POST /api/v1/receiver/volume         [auth]")
    log.info("=" * 60)

    app.run(host=args.host, port=args.port, debug=args.debug)


if __name__ == "__main__":
    main()
