#!/usr/bin/env python3
"""JSON Schema validation of the vendored Michi Link contract (MS-01).

Reads schemas exclusively from the VENDORIZED bundle
(contracts/michi-link/schemas/) - the local tests/contract/schemas/ tree
was deleted. The vendored bundle is the single authority; this module
verifies its internal consistency:

- every vendored schema is a valid JSON Schema (draft-07);
- positive examples validate against their canonical schema;
- error examples validate against error.schema.json;
- semantic negatives (replayed/stale heartbeats) remain structurally
  valid, because negativity is a server-side state concern;
- identity, discovery and pairing vectors validate against their
  canonical schemas;
- canonical schemas reject extra properties and out-of-range values.

The canonical schemas cross-reference each other by $id under
https://michi.link/schemas/, so a local Registry resolves every $ref
from the vendored tree - no network access, deterministic everywhere.

Requires: jsonschema (pip install -r requirements.txt).
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
EXAMPLES = os.path.join(ROOT, "contracts", "michi-link", "examples")
VECTORS = os.path.join(ROOT, "contracts", "michi-link", "vectors")


def load(path):
    with open(path) as f:
        return json.load(f)


def load_schemas():
    schemas = {}
    for name in sorted(os.listdir(SCHEMAS)):
        if not name.endswith(".schema.json"):
            continue
        doc = load(os.path.join(SCHEMAS, name))
        schemas[name] = doc
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


def load_example(name):
    return load(os.path.join(EXAMPLES, name))


def load_vector(path):
    return load(os.path.join(VECTORS, path))


# ── validations ──────────────────────────────────────────────

def test_vendored_schemas_are_valid_json_schemas():
    schemas = load_schemas()
    for name, doc in schemas.items():
        jsonschema.Draft7Validator.check_schema(doc)
    print(f"  PASS {len(schemas)} vendored schemas are valid draft-07")


def test_positive_examples():
    schemas = load_schemas()
    registry = build_registry(schemas)
    cases = [
        ("receiver-heartbeat.json", "receiver-heartbeat.schema.json"),
        ("receiver-heartbeat-response.json", "receiver-heartbeat-response.schema.json"),
        ("receiver-session-create.json", "receiver-session-create.schema.json"),
        ("receiver-session-created.json", "receiver-session.schema.json"),
        ("receiver-session-patch.json", "receiver-session-patch.schema.json"),
        ("receiver-session-status.json", "receiver-session.schema.json"),
    ]
    for example, schema_name in cases:
        doc = load_example(os.path.join("positive", example))
        validator_for(schemas[schema_name], registry).validate(doc)
        print(f"  PASS examples/positive/{example} vs {schema_name}")


def test_error_examples():
    schemas = load_schemas()
    registry = build_registry(schemas)
    error_dir = os.path.join(EXAMPLES, "negative")
    names = sorted(n for n in os.listdir(error_dir) if n.startswith("error-"))
    for name in names:
        doc = load(os.path.join(error_dir, name))
        validator_for(schemas["error.schema.json"], registry).validate(doc)
        print(f"  PASS examples/negative/{name} vs error.schema.json")


def test_semantic_negative_heartbeats_are_structurally_valid():
    schemas = load_schemas()
    registry = build_registry(schemas)
    for name in ("receiver-heartbeat-repeated.json", "receiver-heartbeat-stale.json"):
        doc = load_example(os.path.join("negative", name))
        validator_for(schemas["receiver-heartbeat.schema.json"], registry).validate(doc)
        print(f"  PASS examples/negative/{name} vs receiver-heartbeat.schema.json")


def test_identity_vectors():
    schemas = load_schemas()
    registry = build_registry(schemas)
    for name in ("server-info-standard.json", "server-info-hifi.json"):
        doc = load_vector(os.path.join("identity", name))
        validator_for(schemas["server-info.schema.json"], registry).validate(doc)
        print(f"  PASS vectors/identity/{name} vs server-info.schema.json")


def test_discovery_vectors():
    schemas = load_schemas()
    registry = build_registry(schemas)
    for name in ("announce-valid.json", "announce-signature-altered.json"):
        doc = load_vector(os.path.join("discovery", name))
        validator_for(schemas["discovery-announce.schema.json"], registry).validate(doc)
        print(f"  PASS vectors/discovery/{name} vs discovery-announce.schema.json")


def test_pairing_vectors():
    schemas = load_schemas()
    registry = build_registry(schemas)
    for name in ("pair-start-valid.json", "pair-start-nonce-altered.json",
                 "pair-start-wrong-michi-id.json"):
        doc = load_vector(os.path.join("pairing", name))
        validator_for(schemas["pair-start.schema.json"], registry).validate(doc)
        print(f"  PASS vectors/pairing/{name} vs pair-start.schema.json")
    for name in ("pair-confirm-valid.json", "pair-confirm-replay.json",
                 "pair-confirm-wrong-pin.json"):
        doc = load_vector(os.path.join("pairing", name))
        validator_for(schemas["pair-confirm.schema.json"], registry).validate(doc)
        print(f"  PASS vectors/pairing/{name} vs pair-confirm.schema.json")
    doc = load_vector(os.path.join("pairing", "pair-confirm-response.json"))
    validator_for(schemas["pair-confirm-response.schema.json"], registry).validate(doc)
    print("  PASS vectors/pairing/pair-confirm-response.json vs pair-confirm-response.schema.json")


def _expect_rejection(label, doc, schema, registry):
    try:
        validator_for(schema, registry).validate(doc)
    except jsonschema.ValidationError:
        print(f"  PASS {label} rejected")
        return
    raise AssertionError(f"expected rejection for {label}")


def test_extra_properties_rejected():
    schemas = load_schemas()
    registry = build_registry(schemas)
    doc = dict(load_vector(os.path.join("identity", "server-info-standard.json")))
    doc["playlists"] = True
    _expect_rejection("server-info + extra property", doc,
                      schemas["server-info.schema.json"], registry)
    doc = dict(load_example(os.path.join("positive", "receiver-heartbeat.json")))
    doc["extra"] = 1
    _expect_rejection("heartbeat + extra property", doc,
                      schemas["receiver-heartbeat.schema.json"], registry)


def test_out_of_range_values_rejected():
    schemas = load_schemas()
    registry = build_registry(schemas)
    doc = dict(load_example(os.path.join("positive", "receiver-session-create.json")))
    doc["buffer_ms"] = 49
    _expect_rejection("session create buffer_ms=49", doc,
                      schemas["receiver-session-create.schema.json"], registry)
    doc = dict(load_example(os.path.join("positive", "receiver-session-create.json")))
    doc["ssrc"] = 0
    _expect_rejection("session create ssrc=0", doc,
                      schemas["receiver-session-create.schema.json"], registry)


# ── runner ──────────────────────────────────────────────────

def run():
    tests = [
        test_vendored_schemas_are_valid_json_schemas,
        test_positive_examples,
        test_error_examples,
        test_semantic_negative_heartbeats_are_structurally_valid,
        test_identity_vectors,
        test_discovery_vectors,
        test_pairing_vectors,
        test_extra_properties_rejected,
        test_out_of_range_values_rejected,
    ]
    ok = 0
    for t in tests:
        try:
            t()
            ok += 1
        except Exception as e:
            print(f"  FAIL {t.__name__}: {e}")
    print(f"\n{ok}/{len(tests)} vendored contract schema suites passed")
    return ok == len(tests)


if __name__ == "__main__":
    sys.exit(0 if run() else 1)
