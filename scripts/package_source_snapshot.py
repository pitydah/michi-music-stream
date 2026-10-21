#!/usr/bin/env python3
"""Create a deterministic, source-only Michi Music Stream archive.

This is a developer snapshot, not signed firmware, built binaries, or evidence
that tests/hardware passed. No network, flashing or test execution occurs.
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
from pathlib import Path
import re
import stat
import sys
import zipfile

EXCLUDE_DIRS = {
    ".git", ".pytest_cache", ".mypy_cache", ".ruff_cache", "__pycache__",
    "build", "managed_components", ".venv", "venv", "node_modules",
    ".tox", "dist", ".cache", ".fonts", ".atl", ".opencode",
    "H21_FINAL_STAGE", "H22_SYNC_STAGE", "H19_TRANSFER_STAGE", "H15_TRANSFER_STAGE",
    "cmake-build-debug", "cmake-build-release",
}
EXCLUDE_NAMES = {"sdkconfig", "sdkconfig.old", ".DS_Store", "Thumbs.db",
                 "START_HERE_H20.md", "H22_SOURCE_TREE_MANIFEST.json", "H22_COMPLETE_SOURCE_RELEASE.json"}
EXCLUDE_SUFFIX = {".pyc", ".pyo", ".o", ".a", ".so", ".dll", ".exe", ".elf", ".map", ".gcda", ".gcno", ".ttf", ".otf", ".woff", ".woff2", ".patch"}
DENY_SECRET = re.compile(r"(^|[._-])(id_rsa|id_ed25519|private|secret|credential|password|token)([._-]|$)", re.I)
# Build flash artifacts, private signing material and identity never ship in a
# source archive. User-controlled media and firmwares may legitimately be .bin;
# exclude compiled bins except known, necessary source assets (none required).
EXCLUDE_BINARY_SUFFIX = {".bin", ".zip", ".tar", ".gz", ".7z", ".rar"}
# Public schema/negative-vector fixtures are not private credentials. Require
# exact path AND harmless JSON structure instead of blindly excluding them.
PUBLIC_TOKEN_DOCS = {
    "contracts/michi-link/schemas/token-refresh.schema.json",
    "examples/error-invalid-token.json",
}

EPOCH = (1980, 1, 1, 0, 0, 0)  # Portable ZIP timestamp, reproducible

# H18: an integrity hash is insufficient when the archive is missing whole
# components. Fail closed if the downloaded Drive tree is incomplete. This is
# an intentionally explicit contract, not a fuzzy "at least N files" guess.
# Every production component must be present. A manifest of a partial
# downloaded folder is not an acceptable release snapshot.
REQUIRED_COMPONENTS = (
    "michi_audio", "michi_audio_output", "michi_board", "michi_dac",
    "michi_discovery", "michi_display", "michi_home", "michi_http",
    "michi_identity", "michi_led", "michi_log", "michi_ota",
    "michi_product_profile", "michi_sd", "michi_session", "michi_state",
    "michi_time", "michi_volume", "michi_wifi",
)
REQUIRED_DIRS = (
    "firmware", "firmware/main", "firmware/components",
    "scripts", "tests", "tests/host", "tests/contract", "tests/e2e",
    "contracts", "contracts/michi-link", "simulator", "docs",
    *(f"firmware/components/{name}" for name in REQUIRED_COMPONENTS),
)
REQUIRED_FILES = (
    "README.md", "firmware/CMakeLists.txt", "firmware/main/app_main.c",
    "firmware/components/michi_audio/michi_audio.c",
    "firmware/components/michi_audio/michi_jb.c",
    "firmware/components/michi_audio_output/michi_audio_output.c",
    "firmware/components/michi_session/michi_session.c",
    "firmware/components/michi_http/http_server.c",
    "tests/host/Makefile", "tests/host/test_michi_audio_engine.c",
    "tests/host/shim_rtp/lwip/sockets.h",
    "tests/host/shim_rtp/lwip/ip4_addr.h",
    "tests/host/shim_rtp/esp_heap_caps.h",
    "tests/host/shim_rtp/rtp_output_fake.c",
    "tests/host/shim_rtp/rtp_output_fake.h",
    "tests/host/shim_rtp/rtp_faults.c",
    "tests/host/shim_rtp/rtp_time.c",
    "scripts/run_tests.sh", "scripts/check_source_identity.py",
    "scripts/package_source_snapshot.py",
    "firmware/components/michi_audio/include/michi_audio_diag.h",
    "firmware/components/michi_wifi/Kconfig",
    "firmware/components/michi_wifi/include/michi_wifi_secret.h",
    "tests/host/test_michi_audio_diagnostics.c",
    "tests/host/test_michi_wifi_secret.c",
    "firmware/sdkconfig.defaults", "firmware/sdkconfig.production.defaults",
    "scripts/check_release_profile.py", "scripts/reconcile_drive_sources.py",
    "tests/test_release_profile.py", "tests/test_reconcile_drive_sources.py",
    "tests/test_source_snapshot.py", ".github/workflows/ci.yml",
    "SOURCE_BASELINE_H21.md",
    "contracts/michi-link/VERSION", "contracts/michi-link/manifest.json",
    "scripts/install_drive_sources.py", "START_HERE.md", "SOURCE_BASELINE_H22.md",
    "docs/H22_OPENCODE_LOCAL_VALIDATION.md",
)

def validate_required_sources(root: Path) -> None:
    missing_dirs = [name for name in REQUIRED_DIRS if not (root / name).is_dir()]
    missing_files = [name for name in REQUIRED_FILES
                     if not (root / name).is_file() or (root / name).stat().st_size == 0]
    if missing_dirs or missing_files:
        missing = [f"directory: {name}" for name in missing_dirs]
        missing += [f"file (missing or empty): {name}" for name in missing_files]
        raise SnapshotError("incomplete Drive source export; missing " + ", ".join(missing))


class SnapshotError(ValueError):
    pass


def inspect_tree(root: Path, output: Path) -> list[tuple[str, Path, bytes]]:
    root = root.resolve(strict=True)
    if not root.is_dir():
        raise SnapshotError("not a Michi Music Stream source tree")
    validate_required_sources(root)
    output = output.resolve()
    if output == root or output.is_relative_to(root):
        raise SnapshotError("output must be outside the source tree")
    selected: list[tuple[str, Path, bytes]] = []
    seen_lower: set[str] = set()
    for path in sorted(root.rglob("*"), key=lambda p: p.relative_to(root).as_posix()):
        rel = path.relative_to(root)
        parts = rel.parts
        if any(part in EXCLUDE_DIRS for part in parts[:-1]):
            continue
        if path.is_symlink():
            # Deliberately reject rather than dereference a link into a secret
            # or machine-specific system path; user must resolve explicitly.
            raise SnapshotError(f"symlink in source tree: {rel.as_posix()}")
        if not path.is_file():
            continue
        name = path.name
        if name in EXCLUDE_NAMES or name.endswith("~") or name.startswith(".env"):
            continue
        if name.lower() in {"secrets.json", "credentials.json", "factory_cfg.bin"}:
            raise SnapshotError(f"sensitive artifact inside source tree: {rel}")
        if DENY_SECRET.search(name) and name.lower().endswith((".pem", ".key", ".p12", ".pfx", ".json", ".env")):
            if rel.as_posix() not in PUBLIC_TOKEN_DOCS:
                raise SnapshotError(f"potential secret in source tree: {rel}")
            try:
                fixture = json.loads(path.read_text(encoding="utf-8"))
            except (UnicodeError, ValueError) as exc:
                raise SnapshotError(f"invalid public token fixture: {rel}") from exc
            if rel.as_posix().startswith("contracts/"):
                safe = (isinstance(fixture, dict)
                        and isinstance(fixture.get("$schema"), str)
                        and fixture.get("type") == "object"
                        and isinstance(fixture.get("properties"), dict))
            else:
                safe = (isinstance(fixture, dict)
                        and set(fixture) == {"error"}
                        and isinstance(fixture["error"], dict)
                        and fixture["error"].get("code") == "UNAUTHORIZED"
                        and "token" not in fixture["error"])
            if not safe:
                raise SnapshotError(f"unexpected public token fixture: {rel}")
        if path.suffix.lower() in EXCLUDE_SUFFIX | EXCLUDE_BINARY_SUFFIX:
            continue
        if rel.parts[0] == "docs" and name.startswith("MICHI_MUSIC_STREAM_H"):
            continue  # Historical overlay bundles are not source
        if stat.S_ISREG(path.lstat().st_mode) is False:
            raise SnapshotError(f"not a regular source file: {rel}")
        key = rel.as_posix().casefold()
        if key in seen_lower:
            raise SnapshotError(f"case-insensitive archive collision: {rel}")
        seen_lower.add(key)
        content = path.read_bytes()
        selected.append((rel.as_posix(), path, content))
    if not selected:
        raise SnapshotError("no source files selected")
    return selected


def write_snapshot(root: Path, output: Path, *, baseline: str) -> dict:
    if not re.fullmatch(r"[A-Za-z0-9._-]{1,60}", baseline):
        raise SnapshotError("baseline must be a nonempty safe identifier")
    entries = inspect_tree(root, output)
    content_manifest = {
        "format_version": 1,
        "project": "michi-music-stream",
        "baseline": baseline,
        "kind": "source-only-not-tested-not-certified",
        "files": [
            {"path": name, "size": len(data), "sha256": hashlib.sha256(data).hexdigest()}
            for name, _path, data in entries
        ],
    }
    raw_manifest = (json.dumps(content_manifest, sort_keys=True, ensure_ascii=False,
                               indent=2) + "\n").encode("utf-8")
    output.parent.mkdir(parents=True, exist_ok=True)
    if output.exists():
        raise SnapshotError("output exists: refusing overwrite")
    # Atomic publication: partial archives are never marked as ready.
    temp = output.with_name(output.name + ".incomplete")
    if temp.exists():
        raise SnapshotError("partial output exists; inspect before deleting")
    try:
        with zipfile.ZipFile(temp, "w", compression=zipfile.ZIP_DEFLATED,
                             compresslevel=9, strict_timestamps=True) as zf:
            for name, _path, data in entries:
                info = zipfile.ZipInfo(name, EPOCH)
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = (0o100644 << 16)
                zf.writestr(info, data, compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)
            info = zipfile.ZipInfo("SOURCE_SNAPSHOT_MANIFEST.json", EPOCH)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = (0o100644 << 16)
            zf.writestr(info, raw_manifest)
        with zipfile.ZipFile(temp) as zf:
            if zf.testzip() is not None:
                raise SnapshotError("ZIP integrity verification failed")
            for item in content_manifest["files"]:
                if hashlib.sha256(zf.read(item["path"])).hexdigest() != item["sha256"]:
                    raise SnapshotError(f"digest mismatch: {item['path']}")
        temp.replace(output)
    except BaseException:
        temp.unlink(missing_ok=True)
        raise
    return {"file_count": len(entries), "archive_sha256": hashlib.sha256(output.read_bytes()).hexdigest(),
            "baseline": baseline, "output": str(output)}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--baseline", default="H16R-PRETEST")
    args = ap.parse_args()
    try:
        result = write_snapshot(args.root, args.out, baseline=args.baseline)
    except (SnapshotError, OSError, zipfile.BadZipFile) as exc:
        print(f"SOURCE SNAPSHOT REFUSED: {exc}", file=sys.stderr)
        return 2
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
