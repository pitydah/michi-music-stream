#!/usr/bin/env python3
"""Read-only verification of a Michi Stream pretest source archive.

Verify SHA-256 of every contained file, exact archive membership, safe paths,
and, when supplied, the signed-off *hash* in the separate release metadata.
This is integrity verification, not an assertion that firmware tests passed.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import PurePosixPath, Path
import sys
import zipfile

MANIFEST = "SOURCE_SNAPSHOT_MANIFEST.json"


class VerificationError(ValueError):
    """A source archive is unsafe, incomplete, or inconsistent."""


def _hex64(value: object) -> bool:
    return (isinstance(value, str) and len(value) == 64
            and all(c in "0123456789abcdef" for c in value))


def verify_archive(archive: Path, release: Path | None = None) -> dict:
    if not archive.is_file():
        raise VerificationError(f"archive missing: {archive}")
    digest = hashlib.sha256()
    with archive.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    archive_sha = digest.hexdigest()
    if release is not None:
        try:
            ref = json.loads(release.read_text(encoding="utf-8"))
        except (OSError, UnicodeError, ValueError) as error:
            raise VerificationError("unreadable H22 release metadata") from error
        if ref.get("archive") != archive.name or ref.get("archive_sha256") != archive_sha:
            raise VerificationError("release archive name or SHA-256 mismatch")
        if ref.get("baseline") != "H22-DRIVE-SOURCE-PRETEST":
            raise VerificationError("unexpected release baseline")
    with zipfile.ZipFile(archive) as zf:
        names = zf.namelist()
        if len(names) != len(set(names)) or names.count(MANIFEST) != 1:
            raise VerificationError("duplicate entries or missing source manifest")
        for item in zf.infolist():
            path = PurePosixPath(item.filename)
            if (item.filename.startswith("/") or "\\" in item.filename
                    or any(p in ("", ".", "..") for p in item.filename.split("/"))
                    or path.is_absolute() or item.is_dir()):
                raise VerificationError(f"unsafe archive path: {item.filename}")
            # Unix special file types and symlinks cannot be extracted as source.
            mode = (item.external_attr >> 16) & 0o170000
            if mode not in (0, 0o100000):
                raise VerificationError(f"not a regular source file: {item.filename}")
        try:
            obj = json.loads(zf.read(MANIFEST))
        except (UnicodeError, ValueError, KeyError) as error:
            raise VerificationError("invalid embedded manifest") from error
        if (obj.get("project") != "michi-music-stream"
                or obj.get("baseline") != "H22-DRIVE-SOURCE-PRETEST"
                or obj.get("kind") != "source-only-not-tested-not-certified"):
            raise VerificationError("unexpected project, baseline or certification metadata")
        files = obj.get("files")
        if not isinstance(files, list) or not files:
            raise VerificationError("no file inventory")
        expected = {MANIFEST}
        for record in files:
            if not isinstance(record, dict):
                raise VerificationError("invalid file inventory item")
            name, size, sha = record.get("path"), record.get("size"), record.get("sha256")
            if not isinstance(name, str) or name in expected or not isinstance(size, int) or size < 0 or not _hex64(sha):
                raise VerificationError("malformed or duplicated inventory entry")
            expected.add(name)
            if name not in names:
                raise VerificationError(f"missing archive source: {name}")
            data = zf.read(name)
            if len(data) != size or hashlib.sha256(data).hexdigest() != sha:
                raise VerificationError(f"file integrity failure: {name}")
        if set(names) != expected:
            raise VerificationError("archive contains unlisted files")
        if release is not None and ref.get("source_file_count") != len(files):
            raise VerificationError("release file count mismatch")
    return {"baseline": obj["baseline"], "file_count": len(files), "archive_sha256": archive_sha,
            "verified": True, "certified_firmware": False}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--release", type=Path)
    args = parser.parse_args()
    try:
        result = verify_archive(args.archive, args.release)
    except (VerificationError, OSError, zipfile.BadZipFile) as error:
        print(f"SOURCE INTEGRITY REJECTED: {error}", file=sys.stderr)
        return 2
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
