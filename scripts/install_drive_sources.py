#!/usr/bin/env python3
"""Verify and import H22 source files from a downloaded Google Drive folder.

Default is read-only. --apply backs up overwritten files before writing.
Never copies or deletes .git, device secrets, caches, compiled binaries, or
historic patch bundles. Does not compile, run tests, flash, or change NVS.
"""
from __future__ import annotations

import argparse
from datetime import datetime
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import stat
import subprocess
import sys
import uuid

MANIFEST = Path("docs/H22_SOURCE_TREE_MANIFEST.json")
BASELINE = "H22-DRIVE-SOURCE-PRETEST"


class ImportErrorH22(ValueError):
    """Unsafe source tree or incomplete H22 transfer."""


def digest_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def _exec_intent(checkout: Path, rel: Path) -> bool | None:
    """Executable intent recorded by git for rel in the destination checkout.

    The source inventory records path/size/sha256 only - no mode - so a Drive
    transfer cannot tell an entrypoint from a library by itself. The
    destination checkout is the authority: `git ls-files -s` still knows the
    mode the file is supposed to have. Returns None when git does not track
    the path (new file) or the destination is not a usable checkout.
    """
    try:
        proc = subprocess.run(
            ["git", "-C", str(checkout), "ls-files", "-s", "--", rel.as_posix()],
            capture_output=True, text=True, check=False,
        )
    except OSError:
        return None
    fields = proc.stdout.split()
    if proc.returncode != 0 or not fields:
        return None
    return fields[0] == "100755"


def _needs_exec(checkout: Path, rel: Path) -> bool:
    """True when rel must be executable in the destination tree.

    Shell entrypoints under scripts/ always are (a Drive download loses the
    bit and the plain suffix rule is what H22.1 relied on). Everything else
    follows the mode git records, which covers shebang'd Python entrypoints
    such as scripts/sign_manifest.py (100755) without touching scripts that
    git deliberately keeps non-executable (e.g.
    scripts/sync_michi_link_contract.py, 100644, also has a shebang).
    """
    if rel.parts[0] == "scripts" and rel.suffix == ".sh":
        return True
    return _exec_intent(checkout, rel) is True


def _missing_exec_bits(checkout: Path, rels: list[Path]) -> list[Path]:
    """Paths whose content already matches the inventory but lost the exec bit.

    The copy loop only visits files whose digest differs, so a Drive tree that
    matches byte-for-byte (the usual case after replacing the folder) never
    passes through it. Without this pass the mode damage stays invisible to
    the installer forever.
    """
    missing: list[Path] = []
    for rel in rels:
        dest = checkout / rel
        if not dest.is_file() or not _needs_exec(checkout, rel):
            continue
        if not stat.S_IMODE(dest.stat().st_mode) & 0o111:
            missing.append(rel)
    return missing


def _safe_relative(text: object) -> Path:
    if not isinstance(text, str) or not text or "\\" in text or text.startswith("/"):
        raise ImportErrorH22(f"invalid manifest path: {text!r}")
    posix = PurePosixPath(text)
    if any(part in ("", ".", "..") for part in text.split("/")) or str(posix) != text:
        raise ImportErrorH22(f"invalid manifest path: {text!r}")
    if any(part in {".git", "build", "managed_components", "__pycache__", ".pytest_cache"} for part in posix.parts):
        raise ImportErrorH22(f"refusing generated/private path: {text}")
    if text == MANIFEST.as_posix():
        raise ImportErrorH22("manifest cannot include itself")
    return Path(*posix.parts)


def _check_no_symlink(root: Path, rel: Path, *, is_file: bool) -> Path:
    candidate = root
    for part in rel.parts:
        candidate = candidate / part
        if candidate.is_symlink():
            raise ImportErrorH22(f"symlink in transfer path: {candidate}")
    if is_file and (not candidate.is_file() or not stat.S_ISREG(candidate.lstat().st_mode)):
        raise ImportErrorH22(f"required regular source file missing: {candidate}")
    return candidate


def read_inventory(root: Path) -> list[tuple[Path, int, str]]:
    path = _check_no_symlink(root, MANIFEST, is_file=True)
    try:
        obj = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, ValueError) as e:
        raise ImportErrorH22("unreadable H22 source-tree manifest") from e
    if (obj.get("project") != "michi-music-stream"
            or obj.get("baseline") != BASELINE
            or obj.get("kind") != "source-only-not-tested-not-certified"):
        raise ImportErrorH22("incorrect source-tree project/baseline")
    files = obj.get("files")
    if not isinstance(files, list) or not files:
        raise ImportErrorH22("empty or invalid file inventory")
    seen: set[str] = set()
    inventory = []
    for record in files:
        if not isinstance(record, dict):
            raise ImportErrorH22("invalid inventory record")
        rel = _safe_relative(record.get("path"))
        key = rel.as_posix().casefold()
        nbytes, sha = record.get("size"), record.get("sha256")
        if (key in seen or not isinstance(nbytes, int) or isinstance(nbytes, bool)
                or nbytes < 0 or not isinstance(sha, str) or len(sha) != 64
                or any(c not in "0123456789abcdef" for c in sha)):
            raise ImportErrorH22(f"invalid file entry: {rel}")
        seen.add(key)
        file = _check_no_symlink(root, rel, is_file=True)
        if file.stat().st_size != nbytes or digest_file(file) != sha:
            raise ImportErrorH22(f"source integrity mismatch: {rel}")
        inventory.append((rel, nbytes, sha))
    return inventory


def install(root: Path, checkout: Path, *, apply: bool) -> dict:
    root = root.expanduser().resolve(strict=True)
    checkout = checkout.expanduser().resolve(strict=True)
    if not root.is_dir() or not checkout.is_dir() or root == checkout:
        raise ImportErrorH22("source and checkout must be distinct directories")
    if root in checkout.parents or checkout in root.parents:
        raise ImportErrorH22("source/checkout nesting is unsafe")
    # A .git directory OR a git-worktree .git file is acceptable; never modify it.
    if not (checkout / ".git").exists():
        raise ImportErrorH22("local checkout has no .git; refusing ambiguous destination")
    inventory = read_inventory(root)  # fail BEFORE modifying anything
    changes = []
    synced = []
    for rel, _size, sha in inventory:
        dest = _check_no_symlink(checkout, rel, is_file=False)
        if dest.exists() and not dest.is_file():
            raise ImportErrorH22(f"destination is not a regular file: {dest}")
        if not dest.exists() or digest_file(dest) != sha:
            changes.append((rel, dest.exists()))
        else:
            synced.append(rel)
    mode_fixes = _missing_exec_bits(checkout, synced)
    out = {"baseline": BASELINE, "verified_sources": len(inventory),
           "files_to_update": sum(exists for _, exists in changes),
           "files_to_add": sum(not exists for _, exists in changes),
           "files_to_chmod": len(mode_fixes),
           "would_modify_git": False, "deletions": 0, "applied": False}
    if not apply:
        return out
    if not changes and not mode_fixes:
        return out
    backup = None
    if any(existed for _, existed in changes):
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        backup = checkout.parent / f"{checkout.name}_H22_SOURCE_BACKUP_{timestamp}_{uuid.uuid4().hex[:8]}"
        backup.mkdir(mode=0o700)
        for rel, existed in changes:
            if existed:
                saved = backup / rel
                saved.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(checkout / rel, saved)
    for rel, existed in changes:
        dst = checkout / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        # Drive downloads often lose executable mode. Preserve existing file
        # permissions; make known shell entrypoints executable on Linux.
        old_mode = stat.S_IMODE(dst.stat().st_mode) if existed else None
        temporary = dst.with_name("." + dst.name + ".h22-" + uuid.uuid4().hex)
        try:
            shutil.copy2(root / rel, temporary)
            mode = old_mode if old_mode is not None else stat.S_IMODE(temporary.stat().st_mode)
            # Drive downloads often lose executable mode. Preserve existing
            # permissions, then restore executable intent for entrypoints
            # (shell scripts and anything git records as 100755).
            if _needs_exec(checkout, rel):
                mode |= 0o111
            os.chmod(temporary, mode)
            os.replace(temporary, dst)
        finally:
            temporary.unlink(missing_ok=True)
    for rel in mode_fixes:
        dest = checkout / rel
        os.chmod(dest, stat.S_IMODE(dest.stat().st_mode) | 0o111)
    out["applied"] = True
    if backup is not None:
        out["backup_directory"] = str(backup)
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--from-drive", required=True, type=Path, help="downloaded full Drive project directory")
    ap.add_argument("--to-local", required=True, type=Path, help="existing local Git checkout")
    ap.add_argument("--apply", action="store_true", help="copy verified source files; dry-run otherwise")
    args = ap.parse_args()
    try:
        outcome = install(args.from_drive, args.to_local, apply=args.apply)
    except (ImportErrorH22, OSError) as exc:
        print(f"H22 SOURCE IMPORT REFUSED: {exc}", file=sys.stderr)
        return 2
    print(json.dumps(outcome, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
