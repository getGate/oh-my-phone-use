#!/bin/bash
# Store the control server's input-device secret and the Wi-Fi credentials on the ESP32 over
# its USB serial port. Usage: esp32_provision.sh SERIAL_PORT WIFI_SSID [WIFI_PASSWORD]
#
# The secret comes from $INPUT_DEVICE_SECRET or control_server/.state/input_device_secret
# (created on demand, exactly like the XIAO build script). Close `idf.py monitor` first: it
# holds the port. Run with only SERIAL_PORT to update the secret and show the Wi-Fi state.
set -euo pipefail
# shellcheck disable=SC1091
source "$(dirname "$0")/esp32_env.sh"
if [[ $# -lt 1 || $# -gt 3 ]]; then
  echo "Usage: bash input_device/scripts/esp32_provision.sh SERIAL_PORT [WIFI_SSID [WIFI_PASSWORD]]" >&2
  exit 2
fi
port="$1"
control=("$PHONEUSE_PYTHON" "$FIRMWARE_DIR/tools/control.py" -p "$port")
secret="${INPUT_DEVICE_SECRET:-}"
if [[ -z "$secret" ]]; then
  secret="$(cd "$REPO_DIR" && node -e 'process.stdout.write(require("./control_server/config").readInputDeviceSecret())')"
fi
echo "Storing the input-device secret (${#secret} characters)..."
"${control[@]}" secret "$secret"
if [[ $# -ge 2 ]]; then
  echo "Storing Wi-Fi credentials for \"$2\" and connecting..."
  if [[ $# -eq 3 ]]; then "${control[@]}" wifi "$2" "$3"; else "${control[@]}" wifi "$2"; fi
  for _ in $(seq 1 15); do
    sleep 1
    state="$("${control[@]}" wifi)"
    if [[ "$state" == *"connected=1"* ]]; then break; fi
  done
fi
"${control[@]}" wifi
"${control[@]}" secret
echo "Use the ip= or hostname= value above as the input tool address in the iPad app (http://...)."
