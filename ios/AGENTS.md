# Working on the iOS app

SwiftUI app for iPhone and iPad (deployment target iOS 17), a ReplayKit broadcast extension and a
Live Activity widget, all in `OhMyPhoneUse.xcodeproj` (scheme `OhMyPhoneUse`). [`README.md`](README.md)
describes the setup flow, session flow, Live Activity, Control Center/Shortcuts and tests;
[`ICONS.md`](ICONS.md) the icon. The app contains no agent and no server: it talks to the
control server over its documented WebSocket/HTTP protocol and to the input tool over
`docs/input_protocol.md`.

## Targets and files

| Target | Directory | Notes |
| --- | --- | --- |
| App | `OhMyPhoneUse/` | Settings, setup steps, session/status client (`RelayConnection.swift`), input-tool client (`DongleClient.swift`), calibration UI, Live Activity updates, Start Session intent |
| Broadcast extension | `Broadcast/` | `SampleHandler.swift` opens `WS /device`, `FrameCapture.swift` keeps the latest fresh frame; forwards input in the background |
| Session widget | `SessionWidget/` | Live Activity presentation and the Control Center control |

`SharedConfiguration.swift`, `DongleClient.swift`, `RelayConnection.swift`, `SetupStatus.swift`,
`FrameCapture.swift`, `start_session_intent.swift` and `calibration_click_policy.swift` are
compiled by the macOS test harnesses with plain `swiftc` (`-target arm64-apple-macosx14.0`), so
they may import only frameworks that exist on macOS as well (today: Foundation, Observation,
Security, CoreMedia, CoreImage, ImageIO, UniformTypeIdentifiers, AppIntents). No UIKit,
SwiftUI, ReplayKit or ActivityKit in those files; put platform UI in the views, the
`SampleHandler` and the widget instead. When you add a file, add it to the right target(s) in
`project.pbxproj` and, if it carries logic, to the matching script in `scripts/`.

## Identity and signing

- `ios/.signing.env` (ignored; template `.signing.env.example`) supplies `DEVELOPMENT_TEAM`
  and `KEY_RELAY_BUNDLE_ID`; the extension IDs derive from it (`.broadcast`, `.session`).
  Never commit a team ID, bundle ID, profile or certificate, and do not hard-code them in the
  project file beyond the `org.example.ipadcontrol` placeholder.
- The build-setting name `KEY_RELAY_BUNDLE_ID`, the keychain service and the URL scheme keep
  their upstream (ipad_computer_use) names on purpose so existing installations keep their
  data. The user-visible name is "Oh My Phone Use". Do not rename the identifiers.
- The default input-tool address when the field is empty is the upstream USB XIAO
  (`http://172.31.254.1`); the ESP32 board is `http://phoneuse.local` or an IP. Both must keep
  working; the readiness check only requires `running`, `state` and `hidReady` in `/status`.

## Build and run

```bash
bash ios/scripts/build.sh -allowProvisioningUpdates   # xcodebuild, Debug, generic/platform=iOS, output in ios/build/
xcrun devicectl list devices
bash ios/scripts/install.sh DEVICE_ID
```

This needs full Xcode, a signing team, and a paired device in Developer Mode; a sandboxed
agent usually cannot do it. Report that rather than claiming an install. UI behaviour
(calibration, broadcast prompt, Live Activity, Control Center) is only verifiable on a device.

## Tests

```bash
bash ios/scripts/test_relay.sh            # builds the relay harness, then runs the control server's node tests with RELAY_NATIVE_TEST
bash ios/scripts/test_frames.sh           # fresh-frame selection
bash ios/scripts/test_setup.sh            # setup-step status against a simulated input tool and server
bash ios/scripts/test_session_launch.sh   # Start Session intent hand-off
bash ios/scripts/test_calibration_click.sh
```

They need macOS Swift tooling and Node.js, not a device. Harness sources are in `test/`; a
behavioural change in a shared file must come with a harness update, and the relay harness
must still satisfy `host/control_server/test/native.test.js` (session authorization,
forwarding, cancellation, confirmed input release).

## Safety behaviour to keep

End Session revokes new commands first and then waits for the hardware release; a Live Activity
dismissal or a widget tap never stops input or starts capture on its own; the broadcast
extension only ever dials out (no listening socket on the device); screenshots are the latest
fresh frame scaled to at most 1280 px. Keep the user-consent steps (broadcast prompt, setup
steps, explicit End Session and Save) in front of every state change.
