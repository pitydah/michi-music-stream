"""MS-09 E2E harness: canonical simulator runtime + crypto/schema helpers.

Runs the canonical receiver simulator (simulator/receiver_sim.py, MS-02)
in-process behind a REAL TCP HTTP server (werkzeug). In-process access is
required by the simulator contract itself: the physical pairing window is
opened through the internal hook (state.open_pairing_window, the same hook
the CLI flag uses) and the PIN is read from the local "display" - it is
never exposed over HTTP. The RTP path uses real UDP sockets against the
port the simulator binds at session creation.
"""

import base64
import json
import os
import sys
import threading
import urllib.error
import urllib.request
from pathlib import Path

import blake3
from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey
from jsonschema import Draft7Validator
from referencing import Registry, Resource

REPO_ROOT = Path(__file__).resolve().parents[2]
SIM_DIR = REPO_ROOT / "simulator"
BUNDLE_DIR = REPO_ROOT / "contracts" / "michi-link"

if str(SIM_DIR) not in sys.path:
    sys.path.insert(0, str(SIM_DIR))

from receiver_sim import SimulatorState, STANDARD_CONFIG, create_app  # noqa: E402
from werkzeug.serving import make_server  # noqa: E402


def b64url_nopad(raw_bytes):
    return base64.urlsafe_b64encode(raw_bytes).decode("ascii").rstrip("=")


def b64url_decode(value):
    if not isinstance(value, str) or not value or "=" in value:
        raise ValueError("strict base64url without padding required")
    return base64.urlsafe_b64decode(value + "=" * (-len(value) % 4))


def derive_michi_id(public_key_bytes):
    return b64url_nopad(blake3.blake3(public_key_bytes).digest())


def ed25519_verify(public_key_b64, signature_b64, message_bytes):
    public_key = Ed25519PublicKey.from_public_bytes(b64url_decode(public_key_b64))
    try:
        public_key.verify(b64url_decode(signature_b64), message_bytes)
        return True
    except InvalidSignature:
        return False


def discovery_canonical_bytes(announce):
    """Canonical signed-announce bytes (lexicographic key order, signature
    excluded) - byte-identical to the firmware builder and the Rust
    DiscoveryEngine::canonical_bytes."""
    features = announce["features"]
    return (
        '{"api_version":"%s","device_id":"%s",'
        '"features":{"heartbeat":%s,"session":%s,"volume":%s},'
        '"host":"%s","michi_id":"%s","name":"%s","nonce":"%s",'
        '"port":%u,"public_key":"%s","roles":["%s"],"service":"%s",'
        '"timestamp_ms":%d}'
    ) % (
        announce["api_version"],
        announce["device_id"],
        "true" if features["heartbeat"] else "false",
        "true" if features["session"] else "false",
        "true" if features["volume"] else "false",
        announce["host"],
        announce["michi_id"],
        announce["name"],
        announce["nonce"],
        announce["port"],
        announce["public_key"],
        announce["roles"][0],
        announce["service"],
        announce["timestamp_ms"],
    )


class BundleSchemas:
    """Vendored michi-link schemas with a shared $ref registry."""

    def __init__(self):
        self.docs = {}
        for name in sorted(os.listdir(BUNDLE_DIR / "schemas")):
            if name.endswith(".schema.json"):
                with open(BUNDLE_DIR / "schemas" / name, encoding="utf-8") as handle:
                    self.docs[name] = json.load(handle)
        self.registry = Registry().with_resources(
            (doc["$id"], Resource.from_contents(doc)) for doc in self.docs.values()
        )

    def validate(self, schema_name, body):
        validator = Draft7Validator(self.docs[schema_name], registry=self.registry)
        return list(validator.iter_errors(body))


class HttpClient:
    """Tiny real-TCP HTTP client for the canonical receiver routes."""

    def __init__(self, base_url):
        self.base_url = base_url

    def request(self, method, path, body=None, headers=None):
        data = json.dumps(body).encode("utf-8") if body is not None else None
        req = urllib.request.Request(self.base_url + path, data=data, method=method)
        if body is not None:
            req.add_header("Content-Type", "application/json")
        for key, value in (headers or {}).items():
            req.add_header(key, value)
        try:
            with urllib.request.urlopen(req, timeout=15) as resp:
                raw = resp.read().strip()
                return resp.status, (json.loads(raw) if raw else None)
        except urllib.error.HTTPError as exc:
            raw = exc.read().strip()
            return exc.code, (json.loads(raw) if raw else None)


class FakeMonoClock:
    """Injectable monotonic clock (the simulator watchdog/lease clock)."""

    def __init__(self, start=0.0):
        self.now = float(start)

    def __call__(self):
        return self.now

    def advance(self, seconds):
        self.now += seconds


class SimRuntime:
    """Canonical simulator + real TCP HTTP server on an ephemeral port."""

    def __init__(self):
        self.clock = FakeMonoClock()
        self.state = SimulatorState(STANDARD_CONFIG, mono_clock=self.clock)
        self.app = create_app(self.state)
        self.server = make_server("127.0.0.1", 0, self.app, threaded=True)
        self.port = self.server.server_port
        self.client = HttpClient(f"http://127.0.0.1:{self.port}")
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def stop(self):
        self.server.shutdown()
        self.thread.join(timeout=5)
