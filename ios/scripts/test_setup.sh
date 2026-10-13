#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build
xcrun swiftc -parse-as-library -target arm64-apple-macosx14.0 \
  OhMyPhoneUse/DongleClient.swift OhMyPhoneUse/SharedConfiguration.swift \
  OhMyPhoneUse/SetupStatus.swift test/SetupStatusHarness.swift -o build/setup-status-harness
build/setup-status-harness
