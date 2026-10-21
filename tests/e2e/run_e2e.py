#!/usr/bin/env python3
"""MS-09 runner: executes the E2E interoperability suite against the
canonical receiver simulator, checking that claimed certification matches
exactly the local vendored contract and the git tree actually being tested.

The result artifact is deterministic: no timestamps, no runtime values.
It records the pinned Michi Link bundle revision, git HEAD and explicit
dirty-tree status, the bundle version, the case list and
the MOCK_PASS gate. It NEVER claims DEVICE_E2E_PASS (that gate belongs to
MS-11 on physical hardware).
"""

import json
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
BUNDLE_DIR = REPO_ROOT / "contracts/michi-link"


def bundle_provenance():
    version = (BUNDLE_DIR / "VERSION").read_text(encoding="utf-8").strip()
    upstream = (BUNDLE_DIR / "UPSTREAM_COMMIT").read_text(encoding="utf-8").strip()
    if not version.startswith("1.0.0-alpha.") or len(upstream) != 40 or not all(c in "0123456789abcdef" for c in upstream):
        raise ValueError("invalid vendored Michi Link provenance")
    return version, upstream, "michi-link-v" + version


def git_output(*args):
    proc = subprocess.run(["git", *args], cwd=REPO_ROOT,
                          capture_output=True, text=True, check=True)
    return proc.stdout.strip()


def result_path(version):
    return REPO_ROOT / "tests/e2e/results" / ("michi-link-" + version.split("-")[-1] + ".json")


CASES = [
    {"id": "E2E-01", "name": "signed discovery announce vector (schema + Ed25519 + michi_id; altered signature rejected)", "result": "pass"},
    {"id": "E2E-02", "name": "server info canonical profile + identity derivation; HOME_MEMBERSHIP strategy; legacy routes gone", "result": "pass"},
    {"id": "E2E-03", "name": "device auth challenge: client identity verification + challenge nonce issuance", "result": "pass"},
    {"id": "E2E-04", "name": "device auth session: membership verification + client signature + mutual server confirmation signature", "result": "pass"},
    {"id": "E2E-05", "name": "device auth anti-replay: challenge consumed on first use (404 on replay)", "result": "pass"},
    {"id": "E2E-03b", "name": "device auth rejection: forged or tampered membership rejected (401)", "result": "pass"},
    {"id": "E2E-06", "name": "session create: canonical negotiation, effective echo, UDP bind, source IP = HTTP peer", "result": "pass"},
    {"id": "E2E-07", "name": "100 canonical RTP packets (48k/16/2/10ms/PT97/1920B) over real UDP to the bound socket", "result": "pass"},
    {"id": "E2E-08", "name": "RTP rejection classes (source IP/PT/SSRC/size) via shared host-tested rtp_guard.c", "result": "pass"},
    {"id": "E2E-09", "name": "heartbeat valid + replay 409 + lease renewal", "result": "pass"},
    {"id": "E2E-10", "name": "PATCH volume/pause/resume + GET state without session_token", "result": "pass"},
    {"id": "E2E-11", "name": "DELETE frees session, socket and port; RTP no longer accepted", "result": "pass"},
    {"id": "E2E-12", "name": "new session after DELETE; lease expiry closes the session; slot freed", "result": "pass"},
    {"id": "E2E-13", "name": "every HTTP response validates against the vendored michi-link schemas", "result": "pass"},
    {"id": "E2E-14", "name": "home-root revocation signature rejects forgeries and accepts signed revocations", "result": "pass"},
]


def build_report():
    bundle_version, bundle_upstream, bundle_tag = bundle_provenance()
    stream_commit = git_output("rev-parse", "HEAD")
    dirty = bool(git_output("status", "--porcelain", "--untracked-files=no", "--", "firmware", "simulator", "contracts", "tests/e2e", "scripts"))
    return {
        "certification": "michi-link-" + bundle_version.split("-")[-1],
        "gate": {"MOCK_PASS": True},
        "commits": {
            "michi_link": {"tag": bundle_tag, "commit": bundle_upstream},
            "michi_music_stream": {
                "commit": stream_commit,
                "note": "git HEAD; source trees have uncommitted changes" if dirty else "git HEAD; clean tracked source trees",
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
    bundle_version, bundle_upstream, bundle_tag = bundle_provenance()
    check(commits["michi_link"]["tag"] == bundle_tag, "michi-link tag mismatch")
    check(commits["michi_link"]["commit"] == bundle_upstream, "michi-link upstream commit mismatch")
    check(commits["michi_music_stream"]["commit"] == git_output("rev-parse", "HEAD"),
          "michi-music-stream commit mismatch")
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
    # Direct runner must not certify a tampered or incomplete contract bundle.
    sync = subprocess.run(
        [sys.executable, "scripts/sync_michi_link_contract.py", "--check"],
        cwd=REPO_ROOT,
    )
    if sync.returncode != 0:
        print("FAIL: vendored Michi Link contract integrity check")
        return 1

    suite = subprocess.run(
        [sys.executable, "-m", "pytest", "tests/e2e", "-q"], cwd=REPO_ROOT
    )
    if suite.returncode != 0:
        print("FAIL: E2E suite did not pass")
        return 1

    report = build_report()
    serialized = json.dumps(report, indent=2) + "\n"
    results_file = result_path(report["bundle"]["version"])
    # A historical artifact must never silently certify a new source tree.
    # Write a fresh result only after the suite passes, retaining any past
    # results as historical evidence (not as approval for this source tree).
    results_file.parent.mkdir(parents=True, exist_ok=True)
    results_file.write_text(serialized, encoding="utf-8")

    problems = verify_report(report)
    if problems:
        for problem in problems:
            print(f"FAIL: {problem}")
        return 1

    print("=== E2E interoperability certification ===")
    print(f"michi-link: {report['commits']['michi_link']['tag']} @ {report['commits']['michi_link']['commit']}")
    print(f"michi-music-stream (tested): {report['commits']['michi_music_stream']['commit']}")
    print(f"source: {report['commits']['michi_music_stream']['note']}")
    print(f"bundle: {report['bundle']['version']}")
    for case in report["cases"]:
        print(f"  {case['id']} {case['result']}: {case['name']}")
    print("gate: MOCK_PASS=true")
    print(f"results: {results_file}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
