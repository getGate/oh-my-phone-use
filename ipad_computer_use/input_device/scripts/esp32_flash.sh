#!/bin/bash
# Flash the PhoneUse ESP32-PICO-KIT firmware. Usage: esp32_flash.sh SERIAL_PORT [idf.py args]
# ESPTOOL_BAUD defaults to 230400 (faster rates were unreliable through USB hubs).
set -euo pipefail
# shellcheck disable=SC1091
source "$(dirname "$0")/esp32_env.sh"
if [[ $# -lt 1 ]]; then
  echo "Usage: bash input_device/scripts/esp32_flash.sh SERIAL_PORT   (macOS: /dev/cu.usbserial-XXXX)" >&2
  exit 2
fi
port="$1"; shift
idf_env
idf.py -C "$FIRMWARE_DIR" -p "$port" -b "${ESPTOOL_BAUD:-230400}" flash "$@"
