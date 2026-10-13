#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build
xcrun swiftc -parse-as-library -target arm64-apple-macosx14.0 \
  OhMyPhoneUse/DongleClient.swift OhMyPhoneUse/RelayConnection.swift \
  Broadcast/FrameCapture.swift test/FrameHarness.swift -o build/frame-harness
build/frame-harness
