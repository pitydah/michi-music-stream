# Michi Music Stream — H17: reproducible source distribution

**Base of changes:** H16R-PRETEST. This change adds a safe deterministic source-only snapshot generator and tests. It does not modify firmware or existing RTP/Home contracts. **No tests/builds executed.**

## New files

- `scripts/package_source_snapshot.py`: builds source-only ZIP deterministically, rechecks each archived SHA-256, includes machine-readable `SOURCE_SNAPSHOT_MANIFEST.json`, refuses symlinks, suspicious private keys, output within source, duplicate names and overwrites. Excludes `.git`, build outputs, caches, `managed_components`, flash binaries, legacy patch ZIPs and machine-specific `sdkconfig`.
- `tests/test_source_snapshot.py`: pytest scenarios for reproducible byte-identical archives, exclusion of generated artifacts, symlink denial, sensitive-file denial and overwrite denial.

## Intended local handoff

```bash
python3 -m pytest -q tests/test_source_snapshot.py
python3 scripts/package_source_snapshot.py --root . --out ../michi-music-stream-H16R-source.zip --baseline H16R-PRETEST
```

The generated archive is **not** an ESP-IDF build, is **not** flashed, does **not** include device credentials, and does **not** certify functionality. It is a reproducible source handoff.

## Outstanding Drive source gates

1. Compile the **real** RTP engine in a host test harness, including deterministic blocked `recvfrom`, pause/stop race, aborted allocation, late task exit and cleanup of I2S resources.
2. Document and enforce fail-closed watchdog policy for combined queue saturation / retry timer failure; provide deterministic injection in host suite.
3. Reconcile API states `CONNECTED`/`BUFFERING`/`PLAYING` with actual PCM acceptance, respecting frozen Michi Link protocol.
4. Review security/OTA/Home/Wi-Fi error-path matrices and add missing regressions (no feature claims without source).
5. Update canonical documentation and CI wiring and run a full static source tree integrity audit before declaring **100% Drive deliverable**.

**Status H17:** only the source snapshot tooling scope completed at source level. Never claim 100% just because the archive can be generated. Runtime validation is separate.
