#!/bin/bash
# Sourced by the esp32_*.sh scripts: locates the PhoneUse ESP32 firmware and ESP-IDF.
#
#   PHONEUSE_FIRMWARE_DIR  firmware checkout (default: the repository that vendors this
#                          project, i.e. three levels above input_device/scripts/)
#   IDF_PATH               ESP-IDF v5.1+ checkout (default: ~/esp/esp-idf)
#   IDF_PYTHON_DIR         directory holding a Python >= 3.10 to put first on PATH before
#                          export.sh (macOS ships 3.9, which recent ESP-IDF versions reject)
#   PHONEUSE_PYTHON        python3 with pyserial for tools/control.py (default: python3)
set -euo pipefail
_scripts_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FIRMWARE_DIR="${PHONEUSE_FIRMWARE_DIR:-$(cd "$_scripts_dir/../../.." && pwd)}"
if [[ ! -f "$FIRMWARE_DIR/CMakeLists.txt" || ! -d "$FIRMWARE_DIR/components/input_tool" ]]; then
  echo "PhoneUse ESP32 firmware not found at $FIRMWARE_DIR (set PHONEUSE_FIRMWARE_DIR)" >&2
  exit 2
fi
REPO_DIR="$(cd "$_scripts_dir/../.." && pwd)"
if [[ -z "${PHONEUSE_PYTHON:-}" ]]; then
  # Prefer the firmware's own virtualenv (it has pyserial), else whatever python3 is on PATH.
  if [[ -x "$FIRMWARE_DIR/.venv/bin/python3" ]]; then
    PHONEUSE_PYTHON="$FIRMWARE_DIR/.venv/bin/python3"
  else
    PHONEUSE_PYTHON="python3"
  fi
fi

idf_env() {
  if command -v idf.py >/dev/null 2>&1; then return; fi
  local idf="${IDF_PATH:-$HOME/esp/esp-idf}"
  if [[ ! -f "$idf/export.sh" ]]; then
    echo "ESP-IDF not found at $idf (set IDF_PATH)" >&2
    exit 2
  fi
  if [[ -n "${IDF_PYTHON_DIR:-}" ]]; then export PATH="$IDF_PYTHON_DIR:$PATH"; fi
  # shellcheck disable=SC1091
  . "$idf/export.sh" >/dev/null
}
