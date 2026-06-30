#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../firmware"
PORT="${1:-/dev/ttyUSB0}"
echo "Flashing firmware to $PORT ..."
idf.py -p "$PORT" flash
echo "Flash complete."
