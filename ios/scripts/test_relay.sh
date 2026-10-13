#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build
xcrun swiftc -parse-as-library -target arm64-apple-macosx14.0 \
  OhMyPhoneUse/DongleClient.swift OhMyPhoneUse/RelayConnection.swift \
  test/RelayHarness.swift -o build/relay-harness
RELAY_NATIVE_TEST="$PWD/build/relay-harness" node --test ../host/control_server/test/*.test.js
