#!/bin/bash
# Build the PhoneUse ESP32-PICO-KIT firmware (BLE HID + Wi-Fi input-tool API) with ESP-IDF.
set -euo pipefail
# shellcheck disable=SC1091
source "$(dirname "$0")/esp32_env.sh"
idf_env
idf.py -C "$FIRMWARE_DIR" build "$@"
