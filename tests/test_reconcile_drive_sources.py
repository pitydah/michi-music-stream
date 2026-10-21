"""H20 regression definitions for local pytest; no execution in Drive."""
from __future__ import annotations
import hashlib
import io
import json
from pathlib import Path
import sys
import zipfile

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"scripts"))
import reconcile_drive_sources as tool


def fixture(tmp_path: Path):
    root=tmp_path/"source"
    (root/"firmware"/"components").mkdir(parents=True)
    (root/"tests"/"host").mkdir(parents=True)
    (root/"docs").mkdir()
    # Real expected SHA256s are tested in the production repository; this
    # fixture uses the smallest ZIP with one entry to test manifest handling.
    source={"README.md":b"release\n"}
    manifest={"version":tool.EXPECTED_BASELINE,"is_full_repository":False,
              "files":[{"path":n,"bytes":len(v),"sha256":tool.digest(v)} for n,v in source.items()]}
    patch=root/"docs"/tool.PATCH_NAME
    with zipfile.ZipFile(patch,"w") as z:
        for name, data in source.items(): z.writestr(name,data)
        z.writestr(tool.MANIFEST_NAME,json.dumps(manifest))
    (root/"README.md").write_bytes(b"release\n")
    return root,patch


def test_complete_bundle_is_read_only(tmp_path):
    root,patch=fixture(tmp_path)
    _, _, same, repairable, unknown=tool.audit(root,patch)
    assert same == ["README.md"] and not repairable and not unknown


def test_manifest_hash_mismatch_rejected(tmp_path):
    root,patch=fixture(tmp_path)
    with zipfile.ZipFile(patch,"a") as z:
        z.writestr("README.md",b"tampered")
    try: tool.load_bundle(patch)
    except tool.ReconciliationError: pass
    else: raise AssertionError("duplicate entries must be rejected")


def test_known_stale_has_to_match_before_modification(tmp_path):
    root,patch=fixture(tmp_path)
    (root/"README.md").write_bytes(b"unrecognized local changes")
    _, _, _,repairable,unknown=tool.audit(root,patch)
    assert not repairable and unknown


def test_path_traversal_is_rejected():
    for path in ("../README.md","/tmp/malicious","a/../b",".git/config"):
        try: tool.safe_path(path)
        except tool.ReconciliationError: pass
        else: raise AssertionError(f"Unsafe path accepted: {path}")


def test_apply_without_backup_refused(tmp_path):
    root,patch=fixture(tmp_path)
    assert tool.main(["--root",str(root),"--apply","--patch",str(patch)]) != 0


def test_only_known_stale_is_repaired_with_external_backup(tmp_path, monkeypatch):
    root, patch = fixture(tmp_path)
    old = b'pre-H19 original\n'
    (root / 'README.md').write_bytes(old)
    monkeypatch.setitem(tool.KNOWN_STALE,'README.md',tool.digest(old))
    _, bundle, same, repairable, unknown = tool.audit(root, patch)
    assert repairable == ['README.md'] and not unknown
    backup = tmp_path / 'backup'
    tool.apply_known_repairs(root,bundle,repairable,backup)
    assert (root / 'README.md').read_bytes() == b'release\n'
    assert (backup / 'README.md').read_bytes() == old
    _, _, same, repairable, unknown = tool.audit(root,patch)
    assert same == ['README.md'] and not repairable and not unknown


def test_cannot_write_backups_inside_source_tree(tmp_path, monkeypatch):
    root, patch = fixture(tmp_path)
    old=b'legacy\n'
    (root/'README.md').write_bytes(old)
    monkeypatch.setitem(tool.KNOWN_STALE,'README.md',tool.digest(old))
    _, bundle, _, repairable, _ = tool.audit(root,patch)
    try:
        tool.apply_known_repairs(root,bundle,repairable,root/'docs'/'backups')
    except tool.ReconciliationError as error:
        assert 'outside' not in str(error) or 'separate' in str(error) or 'backup' in str(error)
    else:
        raise AssertionError('backups may not pollute the source release')
