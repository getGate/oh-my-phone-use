#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
if [[ $# != 1 ]]; then
  echo "Usage: bash ios/scripts/install.sh DEVICE_ID" >&2
  exit 2
fi
xcrun devicectl device install app --device "$1" build/Build/Products/Debug-iphoneos/OhMyPhoneUse.app
