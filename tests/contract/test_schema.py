#!/usr/bin/env python3
"""JSON Schema (draft-07) validation for the contract examples (F15).

Validates examples/receiver-*.json against tests/contract/schemas/info.json
and the diagnostics shape against tests/contract/schemas/diagnostics.json
(the diagnostics endpoint is firmware-only: no example file exists, so the
fixture below mirrors the EXACT shape emitted by diagnostics_get_handler
in firmware/components/michi_http/http_server.c - fields, types and
optionality, including the F15 last_error.target field).

Requires: jsonschema (pip install -r requirements.txt).
"""

import json
import os
import sys

try:
    import jsonschema
except ImportError:
    print("ERROR: jsonschema is required. Run: pip install -r requirements.txt")
    sys.exit(1)

BASE = os.path.dirname(os.path.abspath(__file__))
EX = os.path.normpath(os.path.join(BASE, "..", "..", "examples"))
SCHEMAS = os.path.join(BASE, "schemas")


def load(path):
    with open(path) as f:
        return json.load(f)


# Diagnostics fixture: mirrors the firmware's diagnostics_get_handler
# output (http_server.c). last_error carries the F15 `target` field.
DIAGNOSTICS_FIXTURE = {
    "uptime_seconds": 1234,
    "heap_free": 200000,
    "heap_min_free": 150000,
    "psram_free": 7000000,
    "psram_size": 8388608,
    "reset_reason": "power_on",
    "wifi": {
        "connected": True,
        "ssid": "home",
        "rssi_dbm": -45,
        "reconnects": 2,
    },
    "audio": {
        "session_active": True,
        "ssrc": 55300,
        "received": 1000,
        "lost": 3,
        "late": 1,
        "duplicate": 0,
        "reordered": 2,
        "underruns": 1,
        "overruns": 0,
        "drops_malformed": 0,
        "drops_pt_s24le": 0,
        "drops_pt_other": 0,
        "drops_ssrc_filtered": 0,
        "drops_payload_geometry": 0,
        "jitter_us": 1200,
        "buffer_ms": 250,
        "packets_in_buffer": 42,
        "last_seq": 1042,
        "last_timestamp": 9984000,
    },
    "dac": {
        "detected": True,
        "initialized": True,
        "model": "pcm5102a",
        "tier": "hifi",
        "sample_rate": 48000,
        "bit_depth": 24,
    },
    "session": {
        "active": True,
        "session_id": "s1",
        "codec": "pcm_s16le",
        "sample_rate": 48000,
        "bit_depth": 16,
        "channels": 2,
        "stream_port": 55300,
        "buffer_ms": 250,
        "volume": 70,
        "paused": False,
        "ssrc": 55300,
        "source_addr": "192.168.1.10",
    },
    "i2s_errors": 0,
    "last_error": {"event": "error", "data": 257, "target": "recoverable"},
    "firmware": {"version": "1.2.3", "build_date": "2026-08-06", "board": "standard"},
    "ota": {"state": "idle", "percent": 0},
}


def run():
    ok = 0
    total = 0

    def validate(label, schema_name, doc):
        nonlocal ok, total
        total += 1
        schema = load(os.path.join(SCHEMAS, schema_name))
        try:
            jsonschema.validate(doc, schema)
            ok += 1
            print(f"  PASS {label} vs {schema_name}")
        except jsonschema.ValidationError as e:
            print(f"  FAIL {label} vs {schema_name}: {e.message}")
            print(f"       at {list(e.absolute_path)}")

    info = load(os.path.join(EX, "receiver-standard-info.json"))
    validate("receiver-standard-info.json", "info.json", info)

    hifi = load(os.path.join(EX, "receiver-hifi-info.json"))
    validate("receiver-hifi-info.json", "info.json", hifi)

    # Error examples must be REJECTED by the info schema (shape guard).
    for name in ("error-example.json", "session-start.json"):
        total += 1
        doc = load(os.path.join(EX, name))
        try:
            jsonschema.validate(doc, load(os.path.join(SCHEMAS, "info.json")))
            print(f"  FAIL {name}: expected rejection, schema accepted it")
        except jsonschema.ValidationError:
            ok += 1
            print(f"  PASS {name} rejected by info.json")

    # Diagnostics: fixture with an error captured (event=error + target).
    validate("diagnostics fixture (error captured)", "diagnostics.json",
             DIAGNOSTICS_FIXTURE)

    # Diagnostics: no error captured this boot - firmware emits
    # event=null, data=0, target="none".
    no_err = dict(DIAGNOSTICS_FIXTURE)
    no_err["last_error"] = {"event": None, "data": 0, "target": "none"}
    validate("diagnostics fixture (no error)", "diagnostics.json", no_err)

    # Diagnostics: inactive session block {"active": false} only.
    idle = dict(DIAGNOSTICS_FIXTURE)
    idle["session"] = {"active": False}
    idle["audio"] = {k: v for k, v in idle["audio"].items() if k != "ssrc"}
    validate("diagnostics fixture (idle session)", "diagnostics.json", idle)

    print(f"\n{ok}/{total} schema validations passed")
    return ok == total


sys.exit(0 if run() else 1)
