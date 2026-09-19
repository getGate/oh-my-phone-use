#!/bin/bash
# Run the protocol check against an ESP32 input tool over Wi-Fi.
# Usage: esp32_check.sh [http://phoneuse.local]   (requires the paired phone to be connected)
set -euo pipefail
cd "$(dirname "$0")/.."
INPUT_DEVICE_URL="${1:-${INPUT_DEVICE_URL:-http://phoneuse.local}}" python3 scripts/check_device.py
