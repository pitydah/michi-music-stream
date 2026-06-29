#!/usr/bin/env bash
# run_receiver_sim_pairing_closed.sh
# Lanza un receiver Standard con ventana de pairing CERRADA.
# Micro Server debe pedir pair/start primero.
# Útil para probar que el server maneja pairing_window_closed.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SIM_DIR="$(cd "$SCRIPT_DIR/../simulator" && pwd)"

cd "$SIM_DIR"
python3 receiver_sim.py --type standard --pairing-closed --port 53319
