# Michi Stream Audit Remediation State

- **Branch:** `fix/ui-device-gaps`
- **HEAD inicial:** `e6c22c77dd0ab138cc429c9a87fb25e868378898`
- **Base:** `main` (`d18d4698e5ef83649d2f1dcda35a477d000b0b88`)
- **PR:** #33
- **Objetivo actual:** Fases A a N del plan KILLCRITIC remediation

## Baseline Checks
- `git status --short`: clean
- `make -C tests/host clean && make -C tests/host test`: PASS (14 suites, 100%)
- `cppcheck`: FAIL (`components/michi_dac/dac_manager.c:201` invalidPrintfArgType_s)
- `python3 tests/e2e/run_e2e.py`: 9 passed, fails on commit drift guard (expected until re-anchor at final step)

## Registro de Fases
- [x] Pre-flight y Baseline completados
- [x] PHASE B: DAC effective profile resolution (`michi_dac_default_profile`, NVS, compile-time SKU, single authoritative resolution)
- [x] PHASE C: buffer_ms Signal Truth (iniciado en e6c22c7; verificar gates BUF-01..BUF-03)
- [x] PHASE D: /server/info fail-closed
- [x] PHASE E: RTP clock wrap + reorder correctness
- [x] PHASE F: OTA audio health + OTA/session exclusion
- [x] PHASE G: esp_timer pairing/discovery non-blocking
- [x] PHASE H: display DMA buffer lifetime (HARDWARE_DISPLAY_PASS: PENDING)
- [x] PHASE I: test-production unification (michi_jb, OTA, profile; JB-01 verified)
- [x] PHASE J: jitter/loss metric correctness
- [x] PHASE K: hardware docs consistency
- [ ] PHASE L: CI full closure (cppcheck clean)
- [ ] PHASE M: E2E re-certification
- [ ] PHASE N: final audit
