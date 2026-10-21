#!/usr/bin/env python3
"""Produce a *developer candidate* factory flash manifest from ESP-IDF outputs.

No guessed offsets, firmware signing, secret injection, OTA publication or board
write occurs here. The unsigned result MUST NOT be promoted as an official
release. Ecosystem's compatibility/signature/provenance gates still apply.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
from pathlib import Path


class ManifestBuildError(ValueError):
    pass


def _number(text: str) -> int:
    return int(text, 0)


def _partitions(path: Path) -> dict[str, tuple[int, int]]:
    """Require explicit offsets/sizes: no ESP-IDF implicit placement guesses."""
    result = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        fields = [x.strip() for x in line.split("#", 1)[0].split(",")]
        if not fields or not fields[0]:
            continue
        if len(fields) < 5 or not fields[3] or not fields[4]:
            raise ManifestBuildError(f"partition {fields[0]} lacks explicit offset/size")
        name = fields[0]
        if name in result:
            raise ManifestBuildError(f"duplicate partition {name}")
        offset, size = _number(fields[3]), _number(fields[4])
        if offset < 0 or size <= 0:
            raise ManifestBuildError(f"invalid bounds for {name}")
        result[name] = offset, size
    spans = sorted((off, off + size, name) for name, (off, size) in result.items())
    for left, right in zip(spans, spans[1:]):
        if left[1] > right[0]:
            raise ManifestBuildError(f"partition overlap: {left[2]} / {right[2]}")
    return result


def _flash_files(path: Path, build_root: Path) -> dict[int, Path]:
    raw = json.loads(path.read_text(encoding="utf-8"))
    items = raw.get("flash_files")
    if not isinstance(items, dict) or not items:
        raise ManifestBuildError("ESP-IDF flasher_args.json missing flash_files")
    result = {}
    for raw_offset, raw_filename in items.items():
        if not isinstance(raw_filename, str) or not isinstance(raw_offset, str):
            raise ManifestBuildError("invalid flash_files entry")
        offset = _number(raw_offset)
        src = (build_root / raw_filename).resolve()
        if not src.is_relative_to(build_root.resolve()) or not src.is_file():
            raise ManifestBuildError(f"missing or unsafe build file at {raw_offset}")
        if offset in result or offset < 0:
            raise ManifestBuildError("duplicate or invalid flash offset")
        result[offset] = src
    return result


def produce(*, build: Path, partitions: Path, out: Path, flash_size: int,
            board: str, source_commit: str, firmware_version: str,
            minimum_ecosystem_version: str) -> dict:
    if not re.fullmatch(r"[0-9a-f]{40}", source_commit):
        raise ManifestBuildError("source_commit must be the exact 40-character Git SHA")
    if not firmware_version or not board or not minimum_ecosystem_version:
        raise ManifestBuildError("version, board and Ecosystem minimum must be explicit")
    if out.resolve().is_relative_to(build.resolve()):
        raise ManifestBuildError("output directory must be separate from ESP-IDF build")
    parts = _partitions(partitions)
    for p in ("factory_cfg", "ota_0", "ota_1", "otadata"):
        if p not in parts:
            raise ManifestBuildError(f"required partition missing: {p}")
    if parts["factory_cfg"][1] < 4096:
        raise ManifestBuildError("factory_cfg is too small")
    if max(off + size for off, size in parts.values()) > flash_size:
        raise ManifestBuildError("partition map exceeds declared flash size")
    files = _flash_files(build / "flasher_args.json", build)
    app_offset = parts["ota_0"][0]
    ota_offset = parts["otadata"][0]
    if app_offset not in files or ota_offset not in files:
        raise ManifestBuildError("app/OTA-init not present at their partition offsets")
    boot = [(off, f) for off, f in files.items() if f.name == "bootloader.bin"]
    partition_bin = [(off, f) for off, f in files.items()
                     if f.name == "partition-table.bin"]
    if len(boot) != 1 or len(partition_bin) != 1:
        raise ManifestBuildError("bootloader or partition table missing/ambiguous")
    selected = [
        ("bootloader", *boot[0]),
        ("partition-table", *partition_bin[0]),
        ("ota_data", ota_offset, files[ota_offset]),
        ("app", app_offset, files[app_offset]),
    ]
    if len({offset for _, offset, _ in selected}) != 4:
        raise ManifestBuildError("factory segments have overlapping offsets")
    spans = sorted((off, off + src.stat().st_size, name)
                   for name, off, src in selected)
    for left, right in zip(spans, spans[1:]):
        if left[1] > right[0]:
            raise ManifestBuildError(f"flash file overlap: {left[2]} / {right[2]}")
    if any(end > flash_size for _, end, _ in spans):
        raise ManifestBuildError("image exceeds flash boundary")
    if files[app_offset].stat().st_size > parts["ota_0"][1]:
        raise ManifestBuildError("app is larger than ota_0")
    if files[ota_offset].stat().st_size > parts["otadata"][1]:
        raise ManifestBuildError("OTA data image exceeds partition")
    factory_lo, factory_size = parts["factory_cfg"]
    for lo, hi, name in spans:
        if lo < factory_lo + factory_size and factory_lo < hi:
            raise ManifestBuildError(f"{name} overlaps factory_cfg")
    if out.exists():
        raise ManifestBuildError("output already exists (never silently overwrite artifacts)")
    out.mkdir(parents=True)
    entries = []
    for name, offset, src in selected:
        target_name = f"{name}.bin"
        target = out / target_name
        shutil.copyfile(src, target)
        data = target.read_bytes()
        entries.append({"name": name, "offset": offset, "file": target_name,
                        "size": len(data), "sha256": hashlib.sha256(data).hexdigest()})
    manifest = {
        "version": firmware_version, "board": board,
        "product": "michi-music-stream", "chip_target": "esp32s3",
        "channel": "dev", "release_tag": "", "source_commit": source_commit,
        "flash_size": flash_size,
        "minimum_ecosystem_version": minimum_ecosystem_version,
        "protocol_compatibility": ["v1-lite"],
        "partition_table_digest": next(item["sha256"] for item in entries if item["name"] == "partition-table"),
        "factory_partition": {"name": "factory_cfg", "offset": factory_lo,
                              "capacity": factory_size, "format": "michi-f1", "schema_version": 1},
        "update_segments": ["app"],
        "segments": entries,
    }
    (out / "flash-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return manifest


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--build", required=True, type=Path, help="ESP-IDF build directory")
    ap.add_argument("--partitions", required=True, type=Path, help="partitions.csv")
    ap.add_argument("--out", required=True, type=Path, help="new, empty-named destination")
    ap.add_argument("--flash-size", required=True, type=_number, help="actual board flash size in bytes")
    ap.add_argument("--board", required=True)
    ap.add_argument("--source-commit", required=True)
    ap.add_argument("--firmware-version", required=True)
    ap.add_argument("--minimum-ecosystem-version", required=True)
    a = ap.parse_args()
    try:
        produce(build=a.build, partitions=a.partitions, out=a.out,
                flash_size=a.flash_size, board=a.board,
                source_commit=a.source_commit, firmware_version=a.firmware_version,
                minimum_ecosystem_version=a.minimum_ecosystem_version)
    except (ManifestBuildError, ValueError, OSError) as e:
        ap.exit(2, f"Refused to build candidate manifest: {e}\n")
    print(f"Developer candidate manifest generated at {a.out}. NOT a signed release.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
