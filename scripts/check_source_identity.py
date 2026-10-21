#!/usr/bin/env python3
"""Fail-fast guard against cross-file source overwrites during Drive sync.

This checks source ownership and uniquely identifying implementations.
It is a structural gate, not a replacement for compiling or testing.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import sys

ROLE_MARKERS = {
    "firmware/components/michi_audio/michi_jb.c": (
        ("michi_jb_init(", "michi_jb_insert_packet(", "michi_jb_discard_stale("),
        ("httpd_uri_t", "michi_http_start(", "/api/v1/receiver-lite/session"),
    ),
    "firmware/components/michi_http/http_server.c": (
        ("httpd_uri_t", "/api/v1/receiver-lite/session"),
        ("michi_jb_insert_packet(",),
    ),
    "firmware/components/michi_audio/michi_audio.c": (
        ("static void session_task(", "michi_audio_session_start(",
         "michi_audio_session_stop("),
        ("httpd_uri_t",),
    ),
    "firmware/components/michi_audio_output/michi_audio_output.c": (
        ("i2s_channel_write(", "michi_audio_output_quiesce("),
        ("httpd_uri_t",),
    ),
    "firmware/components/michi_session/michi_session.c": (
        ("lease_watchdog_cb(", "session_teardown_locked(",
         "michi_session_heartbeat("),
        ("httpd_uri_t",),
    ),
}


def check(root: Path) -> list[str]:
    errors: list[str] = []
    for name, (required, forbidden) in ROLE_MARKERS.items():
        p = root / name
        if not p.is_file():
            errors.append(f"MISSING SOURCE: {name}")
            continue
        contents = p.read_text(encoding="utf-8", errors="replace")
        for marker in required:
            if marker not in contents:
                errors.append(f"WRONG CONTENT: {name}: expected {marker!r}")
        for marker in forbidden:
            if marker in contents:
                errors.append(f"CROSS-FILE CONTAMINATION: {name}: {marker!r}")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parent.parent)
    args = parser.parse_args()
    problems = check(args.root)
    if problems:
        for problem in problems:
            print("FAIL:", problem, file=sys.stderr)
        return 1
    print(f"PASS: {len(ROLE_MARKERS)} source-role identity gates")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
