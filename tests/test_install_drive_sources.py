"""Regression tests for Drive source installer executable-mode restoration.

H22.1 fixed the lost executable bit with a suffix-only rule (``scripts/*.sh``).
That rule never restores shebang'd Python entrypoints, so after a Drive
transfer ``scripts/sign_manifest.py`` (git mode 100755) stayed 100644 and the
checkout became permanently dirty.

These tests pin the replacement contract: shell entrypoints are always
executable, everything else follows the mode git records for the destination
checkout. That keeps ``scripts/sign_manifest.py`` executable without granting
the bit to ``scripts/sync_michi_link_contract.py`` (100644 in git, also
shebang'd).
"""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import install_drive_sources as drive


def _git(repo: Path, *args: str) -> None:
    subprocess.run(
        ["git", "-C", str(repo), *args],
        check=True,
        capture_output=True,
    )


@pytest.fixture()
def checkout(tmp_path: Path) -> Path:
    repo = tmp_path / "checkout"
    (repo / "scripts").mkdir(parents=True)
    _git(repo, "init", "-q")
    _git(repo, "config", "user.email", "h22-test@example.invalid")
    _git(repo, "config", "user.name", "H22 test")
    return repo


def _track(repo: Path, name: str, mode: int) -> Path:
    path = repo / "scripts" / name
    path.write_text("#!/usr/bin/env python3\n", encoding="utf-8")
    path.chmod(mode)
    _git(repo, "add", "scripts/" + name)
    _git(repo, "commit", "-q", "-m", "seed " + name)
    return path


def test_exec_intent_reads_git_recorded_mode(checkout: Path) -> None:
    _track(checkout, "entry.py", 0o755)
    _track(checkout, "library.py", 0o644)

    assert drive._exec_intent(checkout, Path("scripts/entry.py")) is True
    assert drive._exec_intent(checkout, Path("scripts/library.py")) is False


def test_exec_intent_is_none_when_git_cannot_answer(checkout: Path) -> None:
    (checkout / "scripts" / "untracked.py").write_text("#!/bin/sh\n", encoding="utf-8")
    assert drive._exec_intent(checkout, Path("scripts/untracked.py")) is None

    outside = checkout.parent / "not-a-repo"
    outside.mkdir()
    assert drive._exec_intent(outside, Path("scripts/entry.py")) is None


def test_shell_entrypoints_need_exec_without_git(tmp_path: Path) -> None:
    # A Drive drop is not a git checkout: the .sh rule must stand on its own.
    bare = tmp_path / "bare"
    bare.mkdir()
    assert drive._needs_exec(bare, Path("scripts/build_firmware.sh")) is True


def test_shebang_python_entrypoint_follows_git(checkout: Path) -> None:
    _track(checkout, "sign_manifest.py", 0o755)
    _track(checkout, "sync_michi_link_contract.py", 0o644)

    # The regression: both files have a shebang, only one is an entrypoint.
    assert drive._needs_exec(checkout, Path("scripts/sign_manifest.py")) is True
    assert drive._needs_exec(checkout, Path("scripts/sync_michi_link_contract.py")) is False


def test_new_untracked_python_script_keeps_source_mode(checkout: Path) -> None:
    (checkout / "scripts" / "new_tool.py").write_text("#!/bin/sh\n", encoding="utf-8")
    assert drive._needs_exec(checkout, Path("scripts/new_tool.py")) is False


def test_missing_exec_bits_detects_content_identical_damage(checkout: Path) -> None:
    """A tree that matches by digest still needs the mode-only repair pass.

    This is the real failure mode: the Drive copy replaced the folder, every
    digest matched, so the copy loop never ran and the lost bit survived.
    """
    entry = _track(checkout, "sign_manifest.py", 0o755)
    _track(checkout, "sync_michi_link_contract.py", 0o644)
    shell = checkout / "scripts" / "run_tests.sh"
    shell.write_text("#!/bin/sh\n", encoding="utf-8")
    shell.chmod(0o644)
    _git(checkout, "add", "scripts/run_tests.sh")
    _git(checkout, "commit", "-q", "-m", "seed run_tests.sh")

    # Simulate the mode loss produced by the Drive transfer.
    entry.chmod(0o644)

    missing = drive._missing_exec_bits(
        checkout,
        [Path("scripts/sign_manifest.py"), Path("scripts/sync_michi_link_contract.py"),
         Path("scripts/run_tests.sh")],
    )

    assert Path("scripts/sign_manifest.py") in missing
    assert Path("scripts/run_tests.sh") in missing
    # git keeps this one non-executable on purpose: never grant the bit.
    assert Path("scripts/sync_michi_link_contract.py") not in missing
