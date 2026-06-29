#!/usr/bin/env bash
# run_receiver_sim_standard.sh
# Lanza un receiver Standard con ventana de pairing abierta.
# Micro Server debe apuntar a http://localhost:53319

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SIM_DIR="$(cd "$SCRIPT_DIR/../simulator" && pwd)"

cd "$SIM_DIR"
python3 receiver_sim.py --type standard --pairing-open --port 53319
