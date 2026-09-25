# Michi Stream Audit Remediation State — Round 2

- **Branch:** `fix/ui-device-gaps`
- **HEAD inicial R2:** `b18ec53bc52a83cc63a0539c1d0f1f51b44bd19f`
- **Base:** `main` (`d18d4698e5ef83649d2f1dcda35a477d000b0b88`)
- **PR:** #33
- **Objetivo actual:** Fases R2-A a R2-P del plan KILLCRITIC Round 2

## Baseline Checks (R2)
- `git status --short`: clean
- `make -C tests/host clean && make -C tests/host test`: PASS (100%)
- `python3 tests/e2e/run_e2e.py`: PASS (13/13 cases, MOCK_PASS=true)
- `cppcheck`: PASS (0 warnings, --error-exitcode=1)
- `contracts / schema / simulator`: PASS (100%)

## Registro de Fases (Round 2)
- [x] Pre-flight y Baseline R2 completados
- [x] R2-A: /server/info DIAGNOSTIC fail-closed (wire service contract based)
- [x] R2-B: independent expected-audio / SKU truth (decoupled from runtime autodetect)
- [ ] R2-C: pairing/discovery real coalescing (atomic pending / notification, no event loss)
- [ ] R2-D: pairing/discovery cooperative shutdown (request stop -> wake -> exit -> join -> cleanup)
- [ ] R2-E: buffer_ms recovery + capacity truth (target_buffer_ms in recovery, engine clamp/validation)
- [ ] R2-F: pause/stop quiesce semantics (michi_audio_output_quiesce, mute, bounded drain)
- [ ] R2-G: display DMA timeout recovery (quarantine state, no reuse / no UAF)
- [ ] R2-H: cppcheck production equivalence (compile-time macro config, no divergent branching in prod)
- [ ] R2-I: host sdkconfig parity (sync with Kconfig defaults + parity test)
- [ ] R2-J: HTTP recv deadline (recv_wait_timeout aligned with bounded body timeout)
- [ ] R2-K: session peer-IP policy (strict policy on heartbeat IP change)
- [ ] R2-L: PCM5122 capability semantics (silicon max vs driver vs advertised)
- [ ] R2-M: JB fallback + telemetry cleanup (closest behind playhead selection fix)
- [ ] R2-N: comments/docs truth (eliminate obsolete comments)
- [ ] R2-O: full verification (test matrix, falsification suites)
- [ ] R2-P: final KILLCRITIC audit & report
