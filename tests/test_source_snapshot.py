"""Source snapshot tests. Written for local execution; not run in Drive."""
from __future__ import annotations
import json
import shutil
from pathlib import Path
import sys
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import package_source_snapshot as snapshot


def project(tmp_path: Path) -> Path:
    p = tmp_path / "michi-music-stream"
    for dirname in snapshot.REQUIRED_DIRS:
        (p / dirname).mkdir(parents=True, exist_ok=True)
    for filename in snapshot.REQUIRED_FILES:
        path = p / filename
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("H18 test fixture source\n", encoding="utf-8")
    (p / "scripts" / "build.sh").write_text("#!/bin/sh\n")
    return p


def test_snapshot_excludes_generated_files_and_is_reproducible(tmp_path: Path) -> None:
    root = project(tmp_path)
    (root / ".git").mkdir()
    (root / ".git" / "config").write_text("local")
    (root / "firmware" / "build").mkdir()
    (root / "firmware" / "build" / "firmware.elf").write_bytes(b"\x00")
    (root / "sdkconfig").write_text("machine-specific")
    a, b = tmp_path / "a.zip", tmp_path / "b.zip"
    r1 = snapshot.write_snapshot(root, a, baseline="H16R-PRETEST")
    r2 = snapshot.write_snapshot(root, b, baseline="H16R-PRETEST")
    assert r1["archive_sha256"] == r2["archive_sha256"]
    with zipfile.ZipFile(a) as z:
        names = z.namelist()
        assert "firmware/main/app_main.c" in names
        assert "firmware/build/firmware.elf" not in names
        assert ".git/config" not in names
        assert "sdkconfig" not in names
        manifest = json.loads(z.read("SOURCE_SNAPSHOT_MANIFEST.json"))
        assert manifest["kind"] == "source-only-not-tested-not-certified"
        assert len(manifest["files"]) == len(snapshot.REQUIRED_FILES) + 1


def test_snapshot_refuses_symlink(tmp_path: Path) -> None:
    root = project(tmp_path)
    (root / "scripts" / "link").symlink_to(root / "firmware" / "main" / "app_main.c")
    try:
        snapshot.write_snapshot(root, tmp_path / "out.zip", baseline="H16R")
    except snapshot.SnapshotError as e:
        assert "symlink" in str(e)
    else:
        raise AssertionError("symlink must be rejected")


def test_snapshot_rejects_sensitive_material(tmp_path: Path) -> None:
    root = project(tmp_path)
    (root / "scripts" / "private_key.pem").write_text("fake")
    try:
        snapshot.write_snapshot(root, tmp_path / "out.zip", baseline="H16R")
    except snapshot.SnapshotError as e:
        assert "secret" in str(e)
    else:
        raise AssertionError("sensitive file must be rejected")


def test_snapshot_refuses_overwrite(tmp_path: Path) -> None:
    root = project(tmp_path)
    out = tmp_path / "out.zip"
    snapshot.write_snapshot(root, out, baseline="H16R")
    try:
        snapshot.write_snapshot(root, out, baseline="H16R")
    except snapshot.SnapshotError as e:
        assert "exists" in str(e)
    else:
        raise AssertionError("overwrite must be rejected")


def test_snapshot_rejects_missing_core_component(tmp_path: Path) -> None:
    root = project(tmp_path)
    (root / "firmware/components/michi_audio/michi_audio.c").unlink()
    try:
        snapshot.write_snapshot(root, tmp_path / "out.zip", baseline="H18")
    except snapshot.SnapshotError as error:
        assert "michi_audio.c" in str(error)
    else:
        raise AssertionError("a partial Drive folder must never be packaged")


def test_snapshot_rejects_missing_contract_folder(tmp_path: Path) -> None:
    root = project(tmp_path)
    shutil.rmtree(root / "contracts")
    try:
        snapshot.write_snapshot(root, tmp_path / "out.zip", baseline="H18")
    except snapshot.SnapshotError as error:
        assert "contracts" in str(error)
    else:
        raise AssertionError("contracts cannot be omitted silently")


def test_snapshot_rejects_missing_h19_diagnostic_header(tmp_path: Path) -> None:
    root = project(tmp_path)
    (root / "firmware/components/michi_audio/include/michi_audio_diag.h").unlink()
    try:
        snapshot.write_snapshot(root, tmp_path / "out.zip", baseline="H19")
    except snapshot.SnapshotError as error:
        assert "michi_audio_diag.h" in str(error)
    else:
        raise AssertionError("H19 required diagnostic source cannot be omitted")


def test_snapshot_allows_only_known_public_token_contracts(tmp_path: Path) -> None:
    root = project(tmp_path)
    schema = root / "contracts/michi-link/schemas/token-refresh.schema.json"
    schema.parent.mkdir(parents=True, exist_ok=True)
    schema.write_text(json.dumps({"$schema": "http://json-schema.org/draft-07/schema#", "type": "object", "properties": {"token": {"type": "string"}}}))
    error = root / "examples/error-invalid-token.json"
    error.parent.mkdir(parents=True, exist_ok=True)
    error.write_text(json.dumps({"error": {"code": "UNAUTHORIZED", "message": "invalid token"}}))
    result = snapshot.write_snapshot(root, tmp_path / "safe.zip", baseline="H21")
    assert result["file_count"] == len(snapshot.REQUIRED_FILES) + 3


def test_snapshot_rejects_disguised_public_token_secret(tmp_path: Path) -> None:
    root = project(tmp_path)
    error = root / "examples/error-invalid-token.json"
    error.parent.mkdir(parents=True, exist_ok=True)
    error.write_text(json.dumps({"token": "private-real-token"}))
    try:
        snapshot.write_snapshot(root, tmp_path / "refused.zip", baseline="H21")
    except snapshot.SnapshotError as e:
        assert "unexpected public token fixture" in str(e)
    else:
        raise AssertionError("allowlisted name does not permit secret payload")


def test_snapshot_excludes_font_files(tmp_path: Path) -> None:
    root = project(tmp_path)
    folder = root / "firmware/components/michi_display/tools/.fonts"
    folder.mkdir(parents=True)
    (folder / "private.ttf").write_bytes(b"font")
    out = tmp_path / "sources.zip"
    snapshot.write_snapshot(root, out, baseline="H21")
    with zipfile.ZipFile(out) as archive:
        assert not any(name.endswith(".ttf") for name in archive.namelist())


def test_snapshot_rejects_missing_production_component(tmp_path: Path) -> None:
    root = project(tmp_path)
    name = snapshot.REQUIRED_COMPONENTS[-1]
    component = root / "firmware" / "components" / name
    shutil.rmtree(component)
    try:
        snapshot.write_snapshot(root, tmp_path / "invalid.zip", baseline="H22")
    except snapshot.SnapshotError as exc:
        assert name in str(exc)
    else:
        raise AssertionError("required firmware component missing")
