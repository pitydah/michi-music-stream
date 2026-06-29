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

# ── Contract tests ───────────────────────────────────────────
echo "--- contract tests ---"
cd "$PROJECT_DIR"
python3 tests/contract/test_contract.py
CONT_EXIT=$?
echo ""

# ── Results ──────────────────────────────────────────────────
echo "=== Results ==="
echo "  simulator: $([ $SIM_EXIT -eq 0 ] && echo 'PASS' || echo 'FAIL')"
echo "  contract:  $([ $CONT_EXIT -eq 0 ] && echo 'PASS' || echo 'FAIL')"

if [ $SIM_EXIT -ne 0 ] || [ $CONT_EXIT -ne 0 ]; then
    exit 1
fi
exit 0
