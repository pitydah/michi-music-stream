#!/usr/bin/env bash
# test_receiver_simulator.sh
# Ejecuta todos los tests del simulator y valida contract.
# Útil como paso de CI para certificar que el simulador es funcional.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

echo "=== Receiver Simulator — Certification Suite ==="
echo ""

# ── 1. Simulator unit tests ─────────────────────────────────
echo "--- simulator unit tests ---"
cd "$PROJECT_DIR/simulator"
python3 tests/test_simulator.py
SIM_EXIT=$?
echo ""

# ── 2. Contract tests ───────────────────────────────────────
echo "--- contract tests ---"
cd "$PROJECT_DIR"
python3 tests/contract/test_contract.py
CONT_EXIT=$?
echo ""

# ── 3. Quick launch smoke test ──────────────────────────────
echo "--- smoke test (launch + info + shutdown) ---"
SMOKE_EXIT=0
PORT=53320

python3 "$PROJECT_DIR/simulator/receiver_sim.py" --type standard --pairing-open --port $PORT &
SIM_PID=$!
sleep 1

if kill -0 "$SIM_PID" 2>/dev/null; then
    INFO=$(curl -s http://127.0.0.1:$PORT/api/v1/receiver/info 2>/dev/null)
    SERVICE=$(echo "$INFO" | python3 -c "import sys,json; print(json.load(sys.stdin).get('service',''))" 2>/dev/null)
    if [ "$SERVICE" = "michi-stream-standard" ]; then
        echo "  PASS smoke: /info returns michi-stream-standard"
    else
        echo "  FAIL smoke: /info returned service='$SERVICE'"
        SMOKE_EXIT=1
    fi
    kill "$SIM_PID" 2>/dev/null
    wait "$SIM_PID" 2>/dev/null || true
else
    echo "  FAIL smoke: simulator failed to start"
    SMOKE_EXIT=1
fi
echo ""

# ── Results ──────────────────────────────────────────────────
echo "=== Results ==="
echo "  simulator unit:  $([ $SIM_EXIT -eq 0 ] && echo 'PASS' || echo 'FAIL')"
echo "  contract:        $([ $CONT_EXIT -eq 0 ] && echo 'PASS' || echo 'FAIL')"
echo "  smoke:           $([ $SMOKE_EXIT -eq 0 ] && echo 'PASS' || echo 'FAIL')"

if [ $SIM_EXIT -ne 0 ] || [ $CONT_EXIT -ne 0 ] || [ $SMOKE_EXIT -ne 0 ]; then
    exit 1
fi
exit 0
