#!/usr/bin/env bash
# run_tests.sh — Michi Music Stream local test runner (P1-08).
#
# Modes:
#   ./scripts/run_tests.sh           -> FULL (the default; == --full)
#   ./scripts/run_tests.sh --full    -> every suite below
#   ./scripts/run_tests.sh --quick   -> fast subset: the Python-only suites
#                                       plus the two static scans (skips the
#                                       E2E harness, the E2E report + drift
#                                       guard and the host C tests)
#
# FULL suites, in order:
#   1.  contract bundle sync      scripts/sync_michi_link_contract.py --check
#   2.  contract behavior tests   tests/contract/test_contract.py
#   3.  JSON schema tests         tests/contract/test_schema.py
#   4.  simulator unit tests      simulator/tests/test_simulator.py
#   5.  simulator scenario tests  simulator/tests/test_scenarios.py
#   6.  simulator HTTP tests      simulator/tests/test_integration_http.py
#   7.  E2E canonical harness     pytest tests/e2e (the WHOLE folder)
#   8.  E2E deterministic report  tests/e2e/run_e2e.py (+ drift guard)
#   9.  host C tests              make -C tests/host test
#   10. official examples         tests/contract/test_examples.py
#   11. legacy reference scan     rg of the mission pattern vs allowlist
#   12. DEVICE_E2E_PASS gate scan rg DEVICE_E2E_PASS (only "never claimed")
#
# Behavior:
#   - Fail-fast: the first failing suite aborts the run, printing its
#     EXACT name and preserving its real exit code.
#   - Suite output is NEVER filtered (no `tail`, no grep): what the suite
#     printed is what you see.
#   - The final summary lists ONLY the suites actually executed in this
#     run - it never claims PASS for suites that were skipped.
#   - Host dependency policy: the cJSON-based host tests need libcjson.
#     The runner detects it via pkg-config (PKG_CONFIG_PATH is honored,
#     so a local prefix works) and FAILS with the install command instead
#     of silently skipping.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

MODE="full"
case "${1:-}" in
    ""|--full) MODE="full" ;;
    --quick)   MODE="quick" ;;
    *)
        echo "usage: $0 [--quick|--full]  (default: --full)" >&2
        exit 2
        ;;
esac

echo "=== Michi Music Stream — Test Suite (mode: $MODE) ==="

RESULTS=()

# Run one suite with its full output on screen. Fail-fast: on a non-zero
# exit the run aborts with the suite name and the REAL exit code.
run_suite() {
    local name="$1"
    shift
    echo ""
    echo "--- $name ---"
    set +e
    "$@"
    local rc=$?
    set -e
    if [ "$rc" -ne 0 ]; then
        echo ""
        echo "FAIL: suite '$name' exited with code $rc"
        exit "$rc"
    fi
    RESULTS+=("$name")
}

# ── Suite 9 preflight: libcjson must be resolvable (no silent skip) ──
require_host_cjson() {
    if pkg-config --exists libcjson 2>/dev/null; then
        return 0
    fi
    if pkg-config --exists cJSON 2>/dev/null; then
        return 0
    fi
    echo "FAIL: host C tests require libcjson (the cJSON-based suites), but"
    echo "      pkg-config cannot find it. Install it instead of skipping:"
    echo "        Ubuntu/Debian: sudo apt-get install -y libcjson-dev"
    echo "        Arch:          sudo pacman -S cjson"
    echo "        Local prefix:  export PKG_CONFIG_PATH=\"<prefix>/lib/pkgconfig\""
    echo "      (the check above already honors PKG_CONFIG_PATH)"
    return 1
}

# ── Suite 11: legacy route/field scan (mission rg, allowlisted) ──
# Matches may ONLY live in: (1) legitimate negative tests that prove the
# old protocol is rejected, (2) explicit retirement comments in code, or
# (3) documentation explicitly marked historical/retired. Anything else
# fails the gate. contracts/ is vendored and excluded from the scan.
LEGACY_PATTERN='/api/v1/receiver/|receiver-lite/info|receiver-lite/volume|receiver-lite/config|pcm_s24le|opus|music_stream_receiver|unsupported_codec|client_token|initiator_id'
LEGACY_ALLOWLIST='
tests/host/test_pairing_http.c
tests/host/test_session_http.c
tests/host/test_michi_session.c
tests/contract/test_contract.py
simulator/tests/test_integration_http.py
tests/e2e/test_e2e_micro_stream.py
firmware/components/michi_http/json_helpers.c
firmware/components/michi_http/include/michi_http.h
firmware/components/michi_http/http_server.c
firmware/components/michi_pairing/include/michi_pairing.h
firmware/components/michi_product_profile/michi_product_profile.c
firmware/components/michi_product_profile/include/michi_product_profile.h
firmware/README.md
README.md
docs/ROADMAP.md
'

scan_against_allowlist() {
    local pattern="$1"
    local allowlist="$2"
    local what="$3"
    local matches=""
    local offenders=""
    local count=""
    matches="$(cd "$PROJECT_DIR" && rg -n --glob '!contracts/**' \
        --glob '!tests/e2e/results/**' --glob '!scripts/run_tests.sh' \
        --glob '!*.pyc' --glob '!.git/**' \
        "$pattern" . 2>/dev/null || true)"
    local line file
    while IFS= read -r line; do
        [ -z "$line" ] && continue
        file="${line%%:*}"
        file="${file#./}"
        if ! printf '%s\n' "$allowlist" | grep -qxF "$file"; then
            offenders="${offenders}${line}"$'\n'
        fi
    done <<< "$matches"
    if [ -n "$offenders" ]; then
        echo "FAIL: $what outside the approved allowlist:" >&2
        printf '%s' "$offenders" >&2
        return 1
    fi
    count="$(printf '%s\n' "$matches" | sed '/^$/d' | wc -l | tr -d ' ')"
    echo "PASS $what: $count matches, every one inside the approved allowlist"
    echo "     (negative tests / explicit retirement comments / marked historical docs)"
}

# ── Suite 12: DEVICE_E2E_PASS gate scan ──
GATE_ALLOWLIST='
docs/ROADMAP.md
tests/e2e/run_e2e.py
'

# Run a scan suite in THIS shell (the scanner is a shell function, not a
# subprocess) with the same fail-fast contract as run_suite.
run_scan() {
    local name="$1" pattern="$2" allowlist="$3" what="$4"
    echo ""
    echo "--- $name ---"
    set +e
    scan_against_allowlist "$pattern" "$allowlist" "$what"
    local rc=$?
    set -e
    if [ "$rc" -ne 0 ]; then
        echo ""
        echo "FAIL: suite '$name' exited with code $rc"
        exit "$rc"
    fi
    RESULTS+=("$name")
}

# ══ the suites ═════════════════════════════════════════════════════

run_suite "contract bundle sync" \
    python3 scripts/sync_michi_link_contract.py --check

run_suite "contract behavior tests" \
    python3 tests/contract/test_contract.py

run_suite "JSON schema tests" \
    python3 tests/contract/test_schema.py

run_suite "simulator unit tests" \
    python3 simulator/tests/test_simulator.py

run_suite "simulator scenario tests" \
    python3 simulator/tests/test_scenarios.py

run_suite "simulator HTTP integration tests" \
    python3 -m pytest simulator/tests/test_integration_http.py -q

run_suite "official examples validation" \
    python3 tests/contract/test_examples.py

run_scan "legacy reference scan" \
    "$LEGACY_PATTERN" "$LEGACY_ALLOWLIST" "legacy references"

run_scan "DEVICE_E2E_PASS gate scan" \
    "DEVICE_E2E_PASS" "$GATE_ALLOWLIST" "DEVICE_E2E_PASS"

if [ "$MODE" = "full" ]; then
    run_suite "E2E canonical harness (pytest tests/e2e)" \
        python3 -m pytest tests/e2e -q

    run_suite "E2E deterministic report + drift guard" \
        python3 tests/e2e/run_e2e.py

    if ! require_host_cjson; then
        exit 1
    fi
    run_suite "host C tests (make -C tests/host test)" \
        make -C tests/host test
fi

# ── summary (only suites actually executed) ──────────────────────
echo ""
echo "=== Results (mode: $MODE) ==="
for name in "${RESULTS[@]}"; do
    echo "  PASS  $name"
done
if [ "$MODE" = "quick" ]; then
    echo "  NOTE  quick mode: E2E harness, E2E report + drift guard and host C"
    echo "        tests were NOT executed (run with --full for the complete gate)"
fi
echo ""
echo "ALL GREEN: ${#RESULTS[@]} suites executed, 0 failures"
exit 0
