#!/usr/bin/env bash
# run_receiver_sim_hifi.sh
# Lanza un receiver Hi-Fi con ventana de pairing abierta.
# Micro Server debe apuntar a http://localhost:53319

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SIM_DIR="$(cd "$SCRIPT_DIR/../simulator" && pwd)"

cd "$SIM_DIR"
python3 receiver_sim.py --type hifi --pairing-open --port 53319
