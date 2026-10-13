#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
if [[ -f .signing.env ]]; then
  source .signing.env
fi
signing=()
if [[ "${LIVE_ACTIVITY_PUSH_ENABLED:-NO}" == "YES" ]]; then
  signing+=("APP_ENTITLEMENTS=OhMyPhoneUse/app.entitlements" "LIVE_ACTIVITY_PUSH_ENABLED=YES")
fi
if [[ -n "${DEVELOPMENT_TEAM:-}" ]]; then signing+=("DEVELOPMENT_TEAM=$DEVELOPMENT_TEAM"); fi
if [[ -n "${KEY_RELAY_BUNDLE_ID:-}" ]]; then signing+=("KEY_RELAY_BUNDLE_ID=$KEY_RELAY_BUNDLE_ID"); fi
xcodebuild -project OhMyPhoneUse.xcodeproj -scheme OhMyPhoneUse \
  -configuration Debug -destination "${DESTINATION:-generic/platform=iOS}" \
  -derivedDataPath build ${signing[@]+"${signing[@]}"} "$@" build
