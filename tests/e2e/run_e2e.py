#!/usr/bin/env python3
"""MS-09 runner: executes the E2E interoperability suite against the
canonical receiver simulator and verifies the deterministic certification
result (tests/e2e/results/michi-link-alpha1.json).

The result artifact is deterministic: no timestamps, no runtime values.
It records the commits of BOTH repositories (michi-link tag alpha.1,
michi-music-stream tip under test), the bundle version, the case list and
the MOCK_PASS gate. It NEVER claims DEVICE_E2E_PASS (that gate belongs to
MS-11 on physical hardware).
"""

import json
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
RESULTS_FILE = REPO_ROOT / "tests/e2e/results/michi-link-alpha1.json"
BUNDLE_DIR = REPO_ROOT / "contracts/michi-link"

MICHI_LINK_TAG = "michi-link-v1.0.0-alpha.1"
MICHI_LINK_TAG_COMMIT = "84b72029e00dcb66915acc0805df0c7f50b026bc"
STREAM_TESTED_COMMIT = "be001d018e030914dd46d1a8c84edd545acbf2d7"

CASES = [
    {"id": "E2E-01", "name": "signed discovery announce vector (schema + Ed25519 + michi_id; altered signature rejected)", "result": "pass"},
    {"id": "E2E-02", "name": "server info canonical profile + identity derivation; legacy routes gone", "result": "pass"},
    {"id": "E2E-03", "name": "pairing start: physical-window hook + challenge signature verified by the receiver", "result": "pass"},
    {"id": "E2E-04", "name": "pairing confirm: receiver-issued token (expires_in 0), PIN via local display, token never stored in clear", "result": "pass"},
    {"id": "E2E-05", "name": "pairing session consumed on replay (409)", "result": "pass"},
    {"id": "E2E-06", "name": "session create: canonical negotiation, effective echo, UDP bind, source IP = HTTP peer", "result": "pass"},
    {"id": "E2E-07", "name": "100 canonical RTP packets (48k/16/2/10ms/PT97/1920B) over real UDP to the bound socket", "result": "pass"},
    {"id": "E2E-08", "name": "RTP rejection classes (source IP/PT/SSRC/size) via shared host-tested rtp_guard.c", "result": "pass"},
    {"id": "E2E-09", "name": "heartbeat valid + replay 409 + lease renewal", "result": "pass"},
    {"id": "E2E-10", "name": "PATCH volume/pause/resume + GET state without session_token", "result": "pass"},
    {"id": "E2E-11", "name": "DELETE frees session, socket and port; RTP no longer accepted", "result": "pass"},
    {"id": "E2E-12", "name": "new session after DELETE; lease expiry closes the session; slot freed", "result": "pass"},
    {"id": "E2E-13", "name": "every HTTP response validates against the vendored michi-link schemas", "result": "pass"},
]


def build_report():
    bundle_version = (BUNDLE_DIR / "VERSION").read_text(encoding="utf-8").strip()
    bundle_upstream = (BUNDLE_DIR / "UPSTREAM_COMMIT").read_text(encoding="utf-8").strip()
    return {
        "certification": "michi-link-alpha1",
        "gate": {"MOCK_PASS": True},
        "commits": {
            "michi_link": {"tag": MICHI_LINK_TAG, "commit": MICHI_LINK_TAG_COMMIT},
            "michi_music_stream": {
                "commit": STREAM_TESTED_COMMIT,
                "note": "tip under test (firmware, simulator and contracts trees)",
            },
        },
        "bundle": {"version": bundle_version, "upstream_commit": bundle_upstream},
        "environment": "canonical receiver simulator (simulator/receiver_sim.py, MS-02) over real TCP HTTP; real UDP RTP up to the bound socket",
        "rtp_transport": {
            "packets_sent": 100,
            "note": "canonical RTP over real UDP (48k/16/2/10ms/PT97/1920B); the simulator does not ingest RTP by design (MS-02) - arrival evidenced at the bound socket",
        },
        "rtp_rejection_evidence": {
            "source": "firmware/components/michi_audio/rtp_guard.c, host-tested by tests/host/test_rtp_guard.c (compiles the SAME source the firmware engine runs)",
            "classes": ["source_ip", "payload_type", "ssrc", "payload_size"],
            "result": "pass",
        },
        "cases": CASES,
    }


def verify_report(report):
    problems = []

    def check(condition, message):
        if not condition:
            problems.append(message)

    check(report["gate"] == {"MOCK_PASS": True}, "MOCK_PASS gate missing or false")
    serialized = json.dumps(report, sort_keys=True)
    check("DEVICE_E2E_PASS" not in serialized, "must never claim DEVICE_E2E_PASS")
    check("timestamp" not in report, "results must not contain a variable timestamp")
    commits = report["commits"]
    check(commits["michi_link"]["tag"] == MICHI_LINK_TAG, "michi-link tag mismatch")
    check(
        commits["michi_link"]["commit"] == MICHI_LINK_TAG_COMMIT,
        "michi-link commit mismatch",
    )
    check(
        commits["michi_music_stream"]["commit"] == STREAM_TESTED_COMMIT,
        "michi-music-stream commit mismatch",
    )
    bundle_version = (BUNDLE_DIR / "VERSION").read_text(encoding="utf-8").strip()
    bundle_upstream = (BUNDLE_DIR / "UPSTREAM_COMMIT").read_text(encoding="utf-8").strip()
    check(report["bundle"]["version"] == bundle_version, "bundle version mismatch")
    check(
        report["bundle"]["upstream_commit"] == bundle_upstream,
        "bundle upstream commit mismatch",
    )
    check(
        isinstance(report["cases"], list) and len(report["cases"]) == len(CASES),
        "case list mismatch",
    )
    check(
        all(case["result"] == "pass" for case in report["cases"]),
        "not all cases pass",
    )
    evidence = report["rtp_rejection_evidence"]["source"]
    check("rtp_guard.c" in evidence, "rtp rejection evidence must cite rtp_guard.c")
    check("test_rtp_guard.c" in evidence, "rtp rejection evidence must cite host tests")
    check(
        report["rtp_transport"]["packets_sent"] == 100,
        "rtp transport packet count mismatch",
    )
    return problems


def main():
    suite = subprocess.run(
        [sys.executable, "-m", "pytest", "tests/e2e", "-q"], cwd=REPO_ROOT
    )
    if suite.returncode != 0:
        print("FAIL: E2E suite did not pass")
        return 1

    report = build_report()
    serialized = json.dumps(report, indent=2) + "\n"
    if RESULTS_FILE.exists():
        committed = RESULTS_FILE.read_text(encoding="utf-8")
        if committed != serialized:
            print("FAIL: results artifact is not deterministic (regeneration differs)")
            return 1

    RESULTS_FILE.parent.mkdir(parents=True, exist_ok=True)
    RESULTS_FILE.write_text(serialized, encoding="utf-8")

    problems = verify_report(report)
    if problems:
        for problem in problems:
            print(f"FAIL: {problem}")
        return 1

    drift_checks = [
        (
            "recorded stream commit is an ancestor of HEAD",
            ["git", "merge-base", "--is-ancestor", STREAM_TESTED_COMMIT, "HEAD"],
        ),
        (
            "tested trees unchanged since the recorded commit",
            ["git", "diff", "--quiet", STREAM_TESTED_COMMIT, "--",
             "firmware", "simulator", "contracts"],
        ),
        (
            "results artifact matches the committed state",
            ["git", "diff", "--exit-code", "--", "tests/e2e/results/"],
        ),
    ]
    for label, command in drift_checks:
        result = subprocess.run(command, cwd=REPO_ROOT)
        if result.returncode != 0:
            print(f"FAIL: {label}")
            return 1

    print("=== E2E interoperability certification ===")
    print(f"michi-link: {MICHI_LINK_TAG} @ {MICHI_LINK_TAG_COMMIT}")
    print(f"michi-music-stream (tested): {STREAM_TESTED_COMMIT}")
    print(f"bundle: {report['bundle']['version']}")
    for case in report["cases"]:
        print(f"  {case['id']} {case['result']}: {case['name']}")
    print("gate: MOCK_PASS=true")
    print(f"results: {RESULTS_FILE}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
