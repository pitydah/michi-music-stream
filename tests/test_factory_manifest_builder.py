"""Regression cases for the candidate-only ESP-IDF factory manifest builder.

These tests are shipped for the user's local OpenCode run; not executed here.
"""
import importlib.util
import json
from pathlib import Path

import pytest

MODULE = Path(__file__).resolve().parents[1] / "scripts" / "build_factory_manifest.py"
SPEC = importlib.util.spec_from_file_location("michi_factory_manifest", MODULE)
assert SPEC is not None and SPEC.loader is not None
builder = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(builder)


def sample(tmp_path):
    build = tmp_path / "build"
    build.mkdir()
    parts = tmp_path / "partitions.csv"
    parts.write_text("nvs,data,nvs,0x800,0x800\n"
                     "otadata,data,ota,0x1000,0x1000\n"
                     "ota_0,app,ota_0,0x2000,0x2000\n"
                     "ota_1,app,ota_1,0x4000,0x2000\n"
                     "factory_cfg,data,0x99,0xE000,0x2000\n")
    names = {
        "bootloader/bootloader.bin": ("0x0", b"B" * 256),
        "partition_table/partition-table.bin": ("0x700", b"P" * 128),
        "ota_data_initial.bin": ("0x1000", b"O" * 128),
        "music_stream.bin": ("0x2000", b"A" * 1024),
    }
    mapping = {}
    for rel, (offset, data) in names.items():
        dest = build / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(data)
        mapping[offset] = rel
    (build / "flasher_args.json").write_text(json.dumps({"flash_files": mapping}))
    return build, parts


def make(build, parts, out):
    return builder.produce(build=build, partitions=parts, out=out, flash_size=0x10000,
                           board="verified-test-board", source_commit="a" * 40,
                           firmware_version="0.0.test", minimum_ecosystem_version="0.0.test")


def test_candidate_is_fail_closed_and_update_app_only(tmp_path):
    build, parts = sample(tmp_path)
    manifest = make(build, parts, tmp_path / "out")
    assert manifest["channel"] == "dev"
    assert manifest["update_segments"] == ["app"]
    assert manifest["factory_partition"]["offset"] == 0xE000
    assert {s["name"] for s in manifest["segments"]} == {
        "bootloader", "partition-table", "ota_data", "app"}
    with pytest.raises(builder.ManifestBuildError):
        make(build, parts, tmp_path / "out")


def test_refuse_offsets_when_ota_file_missing(tmp_path):
    build, parts = sample(tmp_path)
    path = build / "flasher_args.json"
    raw = json.loads(path.read_text())
    raw["flash_files"].pop("0x1000")
    path.write_text(json.dumps(raw))
    with pytest.raises(builder.ManifestBuildError, match="OTA-init"):
        make(build, parts, tmp_path / "out")


def test_refuse_partition_overflow(tmp_path):
    build, parts = sample(tmp_path)
    with pytest.raises(builder.ManifestBuildError):
        builder.produce(build=build, partitions=parts, out=tmp_path / "out",
                        flash_size=0x8000, board="test", source_commit="a" * 40,
                        firmware_version="1", minimum_ecosystem_version="1")


def test_refuse_unsafe_build_file(tmp_path):
    build, parts = sample(tmp_path)
    path = build / "flasher_args.json"
    raw = json.loads(path.read_text())
    raw["flash_files"]["0x0"] = "../../escaped.bin"
    path.write_text(json.dumps(raw))
    with pytest.raises(builder.ManifestBuildError):
        make(build, parts, tmp_path / "out")
