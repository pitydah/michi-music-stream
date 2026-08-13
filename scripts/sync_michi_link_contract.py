#!/usr/bin/env python3
"""Verify the vendored Michi Link contract bundle (MS-01).

The vendored bundle under `contracts/michi-link/` is the immutable
receiver v1-lite conformance contract published by Michi Link. This
script enforces two guarantees:

1. `--check`: the vendored tree is byte-exact against its own
   `manifest.json`. Every SHA-256 is recomputed and any modified,
   missing or extra file fails the check. `manifest.json` itself must
   byte-match the manifest regenerated from the tree (sorted file list,
   no timestamps), so tampering with the manifest is also detected.

2. `--verify-source`: the vendored tree is byte-exact against the
   pinned source: the Michi Link repository at a fixed tag/commit,
   extracted with `git archive` (which is deterministic regardless of
   worktree state). The source is pinned to the immutable tag
   `michi-link-v1.0.0-alpha.1` by default and can be overridden.

Usage:
    python3 scripts/sync_michi_link_contract.py --check
    python3 scripts/sync_michi_link_contract.py --verify-source
    python3 scripts/sync_michi_link_contract.py --verify-source \
        --source-repo /path/to/michi-link --source-ref michi-link-v1.0.0-alpha.1

Exit code is 0 when every check passes and 1 otherwise.
"""

import argparse
import hashlib
import json
import shutil
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
BUNDLE_DIR = REPO_ROOT / "contracts" / "michi-link"
MANIFEST_NAME = "manifest.json"
SOURCE_BUNDLE_PATH = "contracts/receiver-v1-lite"
DEFAULT_SOURCE_REF = "michi-link-v1.0.0-alpha.1"
DEFAULT_SOURCE_URL = "https://github.com/pitydah/michi-link.git"


def sha256(path: Path) -> str:
    """Return the lowercase hex SHA-256 digest of a file."""
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 16), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8").rstrip("\n")


def fail(message: str) -> int:
    print(f"FAIL: {message}")
    return 1


def bundle_files() -> list[Path]:
    """Every regular file under the bundle dir except manifest.json."""
    return sorted(
        p for p in BUNDLE_DIR.rglob("*") if p.is_file() and p.name != MANIFEST_NAME
    )


def check() -> int:
    """Recompute manifest hashes and fail on any drift."""
    problems = 0

    version_path = BUNDLE_DIR / "VERSION"
    commit_path = BUNDLE_DIR / "UPSTREAM_COMMIT"
    manifest_path = BUNDLE_DIR / MANIFEST_NAME
    for path in (version_path, commit_path, manifest_path):
        if not path.is_file():
            problems += fail(f"missing bundle metadata file: {path.name}")

    if problems:
        return 1

    version = read_text(version_path)
    upstream_commit = read_text(commit_path)
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        return fail(f"manifest.json is not valid JSON: {exc}")

    actual = {}
    for path in bundle_files():
        rel = path.relative_to(BUNDLE_DIR).as_posix()
        actual[rel] = sha256(path)

    listed = {entry["path"]: entry["sha256"] for entry in manifest.get("files", [])}

    for rel in sorted(set(listed) - set(actual)):
        problems += fail(f"manifest lists '{rel}' but the file is missing")
    for rel in sorted(set(actual) - set(listed)):
        problems += fail(f"'{rel}' is present but not listed in the manifest")
    for rel in sorted(set(actual) & set(listed)):
        if actual[rel] != listed[rel]:
            problems += fail(
                f"hash mismatch for '{rel}': "
                f"manifest={listed[rel]} actual={actual[rel]}"
            )

    regenerated = {
        "version": version,
        "upstream_commit": upstream_commit,
        "files": [
            {"path": rel, "sha256": actual[rel]} for rel in sorted(actual)
        ],
    }
    regenerated_bytes = (json.dumps(regenerated, indent=2) + "\n").encode("utf-8")
    if manifest_path.read_bytes() != regenerated_bytes:
        problems += fail(
            "manifest.json does not match the manifest regenerated from the "
            "vendored tree (version, upstream_commit or file list drifted)"
        )

    if problems:
        return 1
    print(
        f"OK: vendored bundle {version} ({len(actual)} files) matches "
        f"manifest.json (upstream {upstream_commit[:12]})"
    )
    return 0


def resolve_source(repo: str, ref: str) -> int:
    """Resolve a tag/commit in a michi-link checkout to a full commit SHA."""
    output = subprocess.run(
        ["git", "-C", repo, "rev-parse", f"{ref}^{{commit}}"],
        check=True,
        capture_output=True,
        text=True,
    )
    return output.stdout.strip()


def extract_bundle(repo: str, commit: str, dest: Path) -> None:
    """Deterministically extract the bundle path at a commit via git archive."""
    archive = subprocess.run(
        ["git", "-C", repo, "archive", "--format=tar", commit, SOURCE_BUNDLE_PATH],
        check=True,
        capture_output=True,
    )
    with tempfile.NamedTemporaryFile(suffix=".tar") as tmp:
        tmp.write(archive.stdout)
        tmp.flush()
        with tarfile.open(tmp.name, mode="r:") as tar:
            members = [
                m for m in tar.getmembers()
                if m.name.startswith(SOURCE_BUNDLE_PATH + "/")
            ]
            for member in members:
                relative = member.name[len(SOURCE_BUNDLE_PATH) + 1:]
                if not relative:
                    continue
                target = dest / relative
                if member.isdir():
                    target.mkdir(parents=True, exist_ok=True)
                elif member.isfile():
                    target.parent.mkdir(parents=True, exist_ok=True)
                    payload = tar.extractfile(member)
                    if payload is None:
                        raise RuntimeError(f"cannot extract {member.name}")
                    target.write_bytes(payload.read())


def clone_source(url: str, ref: str) -> tuple[str, str]:
    """Clone the remote source repo into a temp dir and resolve the ref."""
    tmpdir = tempfile.mkdtemp(prefix="michi-link-source-")
    subprocess.run(
        ["git", "clone", "--quiet", "--no-checkout", url, tmpdir],
        check=True,
    )
    subprocess.run(
        ["git", "-C", tmpdir, "fetch", "--quiet", "origin", ref],
        check=True,
    )
    commit = subprocess.run(
        ["git", "-C", tmpdir, "rev-parse", "FETCH_HEAD^{commit}"],
        check=True,
        capture_output=True,
        text=True,
    ).stdout.strip()
    return tmpdir, commit


def verify_source(source_repo: str | None, source_ref: str) -> int:
    """Byte-compare the vendored tree against the pinned source bundle."""
    repo: str | None = source_repo
    temporary: str | None = None
    if repo is None:
        sibling = REPO_ROOT.parent / "michi-link"
        if (sibling / ".git").exists():
            repo = str(sibling)
        else:
            temporary, commit = clone_source(DEFAULT_SOURCE_URL, source_ref)
            repo = temporary
            source_ref = commit

    try:
        commit = resolve_source(repo, source_ref)
        with tempfile.TemporaryDirectory(prefix="michi-link-bundle-") as td:
            extracted = Path(td) / "bundle"
            extract_bundle(repo, commit, extracted)
            problems = 0
            source_files = {
                p.relative_to(extracted).as_posix(): sha256(p)
                for p in extracted.rglob("*")
                if p.is_file()
            }
            vendored_files = {
                p.relative_to(BUNDLE_DIR).as_posix(): sha256(p)
                for p in BUNDLE_DIR.rglob("*")
                if p.is_file()
            }
            for rel in sorted(set(source_files) - set(vendored_files)):
                problems += fail(f"'{rel}' missing from the vendored tree")
            for rel in sorted(set(vendored_files) - set(source_files)):
                problems += fail(f"'{rel}' is not present in the source bundle")
            for rel in sorted(set(source_files) & set(vendored_files)):
                if source_files[rel] != vendored_files[rel]:
                    problems += fail(
                        f"'{rel}' differs from the source bundle "
                        f"({commit[:12]}, {source_ref})"
                    )
            if problems:
                return 1
            print(
                f"OK: vendored tree is byte-identical to the source bundle "
                f"at {source_ref} ({commit[:12]})"
            )
            return 0
    finally:
        if temporary is not None:
            shutil.rmtree(temporary, ignore_errors=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument(
        "--check",
        action="store_true",
        help="recompute manifest hashes and fail on any drift",
    )
    group.add_argument(
        "--verify-source",
        action="store_true",
        help="byte-compare the vendored tree against the pinned source",
    )
    parser.add_argument(
        "--source-repo",
        default=None,
        help="local michi-link checkout or git URL "
             "(default: ../michi-link if present, else the public GitHub repo)",
    )
    parser.add_argument(
        "--source-ref",
        default=DEFAULT_SOURCE_REF,
        help=f"tag or commit to pin the source (default: {DEFAULT_SOURCE_REF})",
    )
    args = parser.parse_args()

    if args.check:
        return check()
    return verify_source(args.source_repo, args.source_ref)


if __name__ == "__main__":
    sys.exit(main())
