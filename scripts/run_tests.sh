#!/usr/bin/env bash
# run_tests.sh
# Ejecuta todos los tests del proyecto: simulator y contrato.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

echo "=== Michi Music Stream — Test Suite ==="
echo ""

# ── Simulator tests ──────────────────────────────────────────
echo "--- simulator tests ---"
cd "$PROJECT_DIR/simulator"
python3 tests/test_simulator.py
SIM_EXIT=$?
echo ""

# ── Simulator HTTP integration tests ─────────────────────────
echo "--- simulator HTTP integration tests ---"
cd "$PROJECT_DIR"
python3 -m pytest simulator/tests/test_integration_http.py -v 2>&1 | tail -3
HTTP_EXIT=$?
echo ""

# ── Contract tests ───────────────────────────────────────────
echo "--- contract tests ---"
cd "$PROJECT_DIR"
python3 tests/contract/test_contract.py
CONT_EXIT=$?
echo ""

# ── E2E tests ────────────────────────────────────────────────
echo "--- E2E tests ---"
cd "$PROJECT_DIR"
python3 tests/e2e/test_e2e_micro_stream.py
E2E_EXIT=$?
echo ""

# ── Results ──────────────────────────────────────────────────
echo "=== Results ==="
echo "  simulator unit:  $([ $SIM_EXIT -eq 0 ] && echo 'PASS' || echo 'FAIL')"
echo "  simulator HTTP:  $([ $HTTP_EXIT -eq 0 ] && echo 'PASS' || echo 'FAIL')"
echo "  contract:        $([ $CONT_EXIT -eq 0 ] && echo 'PASS' || echo 'FAIL')"
echo "  E2E:             $([ $E2E_EXIT -eq 0 ] && echo 'PASS' || echo 'FAIL')"

if [ $SIM_EXIT -ne 0 ] || [ $HTTP_EXIT -ne 0 ] || [ $CONT_EXIT -ne 0 ] || [ $E2E_EXIT -ne 0 ]; then
    exit 1
fi
exit 0
