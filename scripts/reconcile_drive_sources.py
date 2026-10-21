#!/usr/bin/env python3
"""H20 source-only Drive reconciliation, using the *official* H19 manifest.

Never compiles, flashes, installs dependencies or runs tests.  The default is
read-only.  --apply repairs only three exact, known stale files after verifying
that the complete H19 overlay is internally intact.  It refuses unknown edits,
missing source directories and unexpected historical revisions.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import sys
import tempfile
import zipfile

PATCH_NAME = "MICHI_MUSIC_STREAM_H19_SOURCE_PATCH.zip"
MANIFEST_NAME = "docs/H19_SOURCE_MANIFEST.json"
# SHA256 of previous known-good Drive original, BEFORE partial H19.
KNOWN_STALE = {
    "README.md": "a31e31e4b1db66c2cbe6f8d2856f516cf0d3ec329245b67a99bec2fb8e8b57ed",
    "docs/CURRENT_PRETEST_BASELINE.md": "3e46225efaa0238f67e24c4e584bfbf2328f6301e1c6425bfe7225270f7843a4",
    "scripts/package_source_snapshot.py": "17cede45c4e1012ef38500a5ae8ef27d9e3faa06391d9c5240eb2e085d9db96c",
}
EXPECTED_BASELINE = "H19-SOURCE-PRETEST"

class ReconciliationError(RuntimeError):
    pass


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def safe_path(value: str) -> str:
    p = PurePosixPath(value)
    if not value or p.is_absolute() or any(x in {"..", ".", ""} for x in value.split("/")) or "\\" in value:
        raise ReconciliationError(f"unsafe path: {value!r}")
    if p.parts[0] == ".git":
        raise ReconciliationError("refusing .git mutation")
    return p.as_posix()


def load_bundle(path: Path):
    if not path.is_file():
        raise ReconciliationError(f"H19 overlay missing: {path}")
    with zipfile.ZipFile(path, "r") as z:
        names = z.namelist()
        if len(names) != len(set(names)):
            raise ReconciliationError("duplicate ZIP entries")
        if MANIFEST_NAME not in names:
            raise ReconciliationError("official H19 manifest absent")
        for info in z.infolist():
            safe_path(info.filename)
            if info.is_dir() or (info.external_attr >> 16) & 0o170000 == 0o120000:
                raise ReconciliationError("directories or symlinks not accepted in overlay")
        doc = json.loads(z.read(MANIFEST_NAME))
        if doc.get("version") != EXPECTED_BASELINE or doc.get("is_full_repository") is not False:
            raise ReconciliationError("wrong baseline or wrong bundle type")
        entries = doc.get("files", [])
        if not isinstance(entries, list) or not entries:
            raise ReconciliationError("empty or invalid manifest")
        expected = {safe_path(e["path"]): e for e in entries}
        if len(expected) != len(entries) or set(names) != set(expected) | {MANIFEST_NAME}:
            raise ReconciliationError("manifest and ZIP entries disagree")
        data = {}
        for name, e in expected.items():
            contents = z.read(name)
            if len(contents) != e["bytes"] or digest(contents) != e["sha256"]:
                raise ReconciliationError(f"mismatched H19 archive content: {name}")
            data[name] = contents
        return doc, data


def audit(root: Path, patch: Path):
    root = root.resolve(strict=True)
    if not root.is_dir() or not (root / "firmware" / "components").is_dir() or not (root / "tests" / "host").is_dir():
        raise ReconciliationError("not a complete Michi Music Stream source folder")
    doc, contents = load_bundle(patch)
    same, repairable, unknown = [], [], []
    for name, desired in contents.items():
        target = root / name
        if target.is_symlink():
            unknown.append((name, "symlink in destination"))
        elif target.is_file() and digest(target.read_bytes()) == digest(desired):
            same.append(name)
        elif name in KNOWN_STALE and target.is_file() and digest(target.read_bytes()) == KNOWN_STALE[name]:
            repairable.append(name)
        else:
            unknown.append((name, "missing or unexpected version"))
    return root, contents, sorted(same), sorted(repairable), sorted(unknown)


def apply_known_repairs(root: Path, contents: dict[str, bytes], repairable: list[str], backup_dir: Path) -> None:
    backup_dir = backup_dir.resolve()
    if backup_dir == root or root in backup_dir.parents or backup_dir in root.parents:
        raise ReconciliationError("backup directory must be separate from source and its parents")
    if backup_dir.exists():
        raise ReconciliationError("backup directory already exists; choose a new empty path")
    backup_dir.mkdir(parents=True)
    # Re-check each expected digest immediately before mutation.  No unknown
    # changes may be overwritten.  Retain exact old bytes outside source tree.
    old = {}
    for name in repairable:
        path = root / name
        content = path.read_bytes()
        if digest(content) != KNOWN_STALE[name]:
            raise ReconciliationError(f"concurrent edit detected: {name}")
        old[name] = content
    for name, content in old.items():
        destination = backup_dir / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(content)
    installed = []
    try:
        for name in repairable:
            dest = root / name
            fd, temp_name = tempfile.mkstemp(prefix=".h20-", dir=dest.parent)
            try:
                with os.fdopen(fd, "wb") as file:
                    file.write(contents[name]); file.flush(); os.fsync(file.fileno())
                if digest(dest.read_bytes()) != KNOWN_STALE[name]:
                    raise ReconciliationError(f"changed while preparing update: {name}")
                os.replace(temp_name, dest)
                installed.append(name)
            finally:
                if os.path.exists(temp_name):
                    os.unlink(temp_name)
    except Exception:
        for name in installed:
            (root / name).write_bytes(old[name])
        raise


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--patch", type=Path, help="official H19 ZIP (defaults to ROOT/docs)")
    parser.add_argument("--apply", action="store_true", help="repair only known-safe stale files")
    parser.add_argument("--backup-dir", type=Path, help="required with --apply; must not exist")
    args = parser.parse_args(argv)
    root = args.root.resolve()
    patch = args.patch or root / "docs" / PATCH_NAME
    if args.apply and args.backup_dir is None:
        print("H20 RECONCILIATION REFUSED: --apply requires --backup-dir", file=sys.stderr)
        return 2
    try:
        root, contents, same, repairable, unknown = audit(root, patch)
        report = {"bundle":EXPECTED_BASELINE,"tested":False,"matching":len(same),
                  "repairable":repairable,"unknown":unknown,"complete":not repairable and not unknown}
        print(json.dumps(report,indent=2,sort_keys=True))
        if unknown:
            return 3
        if args.apply and repairable:
            apply_known_repairs(root, contents, repairable, args.backup_dir)
            _, _, _, remaining, conflicts = audit(root, patch)
            return 0 if not remaining and not conflicts else 4
        return 0 if not repairable else 2
    except (ReconciliationError, OSError, ValueError, zipfile.BadZipFile, KeyError) as exc:
        print(f"H20 RECONCILIATION REFUSED: {exc}",file=sys.stderr)
        return 3

if __name__ == "__main__":
    raise SystemExit(main())
