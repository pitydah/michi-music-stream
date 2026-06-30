#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../firmware"
echo "Building firmware for $1 target..."
idf.py set-target "$1"
idf.py build
echo "Build complete."
