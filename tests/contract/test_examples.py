#!/usr/bin/env python3
"""Canonical example validation for the repository examples/ tree (P1-05).

Every official example under examples/ must validate against the CURRENT
vendored schemas (contracts/michi-link/schemas/). The schema for each file
is resolved through an explicit mapping - robust against files whose
content is not self-describing. The mapping doubles as a completeness
gate: any .json file present in examples/ without a mapping fails the
suite, so a new or legacy example can never slip through unvalidated.

Run: python3 tests/contract/test_examples.py
     python3 -m pytest tests/contract/test_examples.py
"""

import json
import os
import sys

try:
    import jsonschema
    from referencing import Registry, Resource
except ImportError:
    print("ERROR: jsonschema is required. Run: pip install -r requirements.txt")
    sys.exit(1)

BASE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(BASE, "..", ".."))
SCHEMAS = os.path.join(ROOT, "contracts", "michi-link", "schemas")
EXAMPLES = os.path.join(ROOT, "examples")

EXAMPLE_SCHEMA_MAP = {
    "receiver-standard-info.json": "server-info.schema.json",
    "receiver-hifi-info.json": "server-info.schema.json",
    "error-example.json": "error.schema.json",
    "error-invalid-token.json": "error.schema.json",
    "error-pairing-closed.json": "error.schema.json",
    "error-session-active.json": "error.schema.json",
    "error-unsupported-rate.json": "error.schema.json",
    "heartbeat-request.json": "receiver-heartbeat.schema.json",
    "pair-start.json": "pair-start.schema.json",
    "pair-confirm.json": "pair-confirm.schema.json",
    "session-start.json": "receiver-session-create.schema.json",
    "session-patch.json": "receiver-session-patch.schema.json",
}

CERTIFIED_AUDIO = {
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


def load(path):
    with open(path, encoding="utf-8") as handle:
        return json.load(handle)


def load_schemas():
    schemas = {}
    for name in sorted(os.listdir(SCHEMAS)):
        if not name.endswith(".schema.json"):
            continue
        schemas[name] = load(os.path.join(SCHEMAS, name))
    return schemas


def build_registry(schemas):
    resources = []
    for doc in schemas.values():
        if "$id" not in doc:
            raise AssertionError("vendored schema without $id")
        resources.append((doc["$id"], Resource.from_contents(doc)))
    return Registry().with_resources(resources)


def validator_for(schema, registry):
    return jsonschema.Draft7Validator(schema, registry=registry)


def on_disk_examples():
    return sorted(name for name in os.listdir(EXAMPLES) if name.endswith(".json"))


# ── validations ──────────────────────────────────────────────

def test_every_official_example_has_a_schema_mapping():
    on_disk = on_disk_examples()
    mapped = sorted(EXAMPLE_SCHEMA_MAP)
    if on_disk != mapped:
        unmapped = sorted(set(on_disk) - set(mapped))
        missing = sorted(set(mapped) - set(on_disk))
        raise AssertionError(
            f"examples/ out of sync with EXAMPLE_SCHEMA_MAP: "
            f"unmapped={unmapped} missing={missing}"
        )
    print(f"  PASS {len(on_disk)} official examples mapped to canonical schemas")


def test_official_examples_validate():
    schemas = load_schemas()
    registry = build_registry(schemas)
    for name, schema_name in EXAMPLE_SCHEMA_MAP.items():
        doc = load(os.path.join(EXAMPLES, name))
        validator_for(schemas[schema_name], registry).validate(doc)
        print(f"  PASS examples/{name} vs {schema_name}")


def test_receiver_examples_announce_certified_audio_profile():
    for name in ("receiver-standard-info.json", "receiver-hifi-info.json"):
        doc = load(os.path.join(EXAMPLES, name))
        assert doc["audio"] == CERTIFIED_AUDIO, name
        print(f"  PASS examples/{name} announces the certified audio profile")


def test_receiver_examples_only_differ_in_service():
    standard = load(os.path.join(EXAMPLES, "receiver-standard-info.json"))
    hifi = load(os.path.join(EXAMPLES, "receiver-hifi-info.json"))
    standard_sans_service = {k: v for k, v in standard.items() if k != "service"}
    hifi_sans_service = {k: v for k, v in hifi.items() if k != "service"}
    assert standard_sans_service == hifi_sans_service
    assert standard["service"] == "michi-stream-standard"
    assert hifi["service"] == "michi-stream-hifi"
    print("  PASS standard and hifi examples differ only in service")


def test_error_example_is_canonical_invalid_request():
    doc = load(os.path.join(EXAMPLES, "error-example.json"))
    error = doc["error"]
    assert error["code"] == "INVALID_REQUEST"
    assert error["message"] == "Unsupported codec"
    assert error["request_id"] == "550e8400-e29b-41d4-a716-446655440000"
    assert error["details"] == {"field": "codec"}
    print("  PASS examples/error-example.json matches the canonical INVALID_REQUEST")


# ── runner ──────────────────────────────────────────────────

def run():
    tests = [
        test_every_official_example_has_a_schema_mapping,
        test_official_examples_validate,
        test_receiver_examples_announce_certified_audio_profile,
        test_receiver_examples_only_differ_in_service,
        test_error_example_is_canonical_invalid_request,
    ]
    ok = 0
    for t in tests:
        try:
            t()
            ok += 1
        except Exception as e:
            print(f"  FAIL {t.__name__}: {e}")
    print(f"\n{ok}/{len(tests)} official example suites passed")
    return ok == len(tests)


if __name__ == "__main__":
    sys.exit(0 if run() else 1)
