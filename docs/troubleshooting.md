# Troubleshooting

Symptoms, their usual cause, and where the fix is described. Work top-down: the board must be
reachable over USB before Bluetooth, Bluetooth before Wi-Fi, and the phone app session before
any MCP tool. `curl -s http://phoneuse.local/status` and `python3 host/tools/control.py status`
answer most questions about the board; `node host/control_server/client.js status` and the MCP
`status` tool answer the rest.

## Board and serial port

| Symptom | Cause and fix |
| --- | --- |
| `no ESP32 serial port found` from `control.py` / `call_audio.py` | The CP210x bridge is not visible. Check `ls /dev/cu.*`; a USB hub or cable without data lines is the usual reason. Pass `-p /dev/cu.usbserial-XXXXX` or set `PHONEUSE_PORT` if the port has an unexpected name. |
| `several candidate ports, pass -p to pick one` | More than one CP210x device is attached; choose with `-p`. |
| Port opens but every command times out (exit status 2) | Another process holds the port: `idf.py monitor`, a second `control.py`, `call_audio.py` or the phone service. Only one of them may run at a time ([firmware/README.md](../firmware/README.md#build-and-flash), [host/README.md](../host/README.md#phone-service)). Also use the `cu.` device, never `tty.`. |
| Garbled text, `ERR invalid_command` for valid commands, flashing fails | Baud rate too high through a hub: flash and monitor at `-b 230400`; the serial protocol itself runs at 115200. |
| Board resets when a host tool connects (`uptime` restarts, phone reconnects) | Something toggled DTR/RTS. The shipped tools leave them at pyserial defaults; `idf.py monitor`/`flash` reset the board by design. |
| `ERR unsupported` for `WIFI` / `SECRET`, or `CALL` / `DIAL` | The flashed build does not include that feature: Wi-Fi + input tool are off in the HFP builds, HFP is off in the default build. Only one group fits in RAM at a time; see [Build variants](../firmware/AGENTS.md#build) and [Planned HFP integration](../firmware/README.md#planned-hfp-integration). |
| `idf.py build` fails on IRAM overflow or heap warnings after a change | The ESP32-PICO-V3 has no PSRAM and ~87 KB heap free after boot; the build must stay `-Os`. Read the *Architecture* section of [firmware/README.md](../firmware/README.md#architecture) before adding tasks or buffers. |
| `sdkconfig.defaults` change has no effect | `sdkconfig` is generated once; delete it (or the build directory) and rebuild. |

## Bluetooth pairing and input

| Symptom | Cause and fix |
| --- | --- |
| "Oh My Phone Use HID" does not appear under Other Devices | Board not powered or still booting; or the bond exists on the board but was forgotten on the phone. Run `control.py unpair`, then **Forget This Device** on the phone and pair again ([Pair the ESP32](../firmware/README.md#pair-the-esp32-with-an-iphone)). |
| Paired, but `status` shows `connected=0` and `/status` hints `Waiting for the paired phone to reconnect` | The phone dropped the link (screen locked for long, Bluetooth toggled). Open Settings > Bluetooth on the phone; it reconnects within seconds. One phone at a time is supported. |
| Pointer moves but clicks do nothing on an iPhone | AssistiveTouch is off. Enable it and map Button 1 to Single-Tap ([Enable AssistiveTouch](../firmware/README.md#enable-assistivetouch)). Leave it off on an iPad. |
| `type` outputs wrong characters or `ERR unsupported_char` | Only ASCII on a U.S. hardware keyboard layout is supported; set the phone's hardware keyboard to U.S. Nothing is typed when a character is rejected. |
| On-screen keyboard disappeared | iOS hides it while a Bluetooth keyboard is connected. AssistiveTouch > **Show Onscreen Keyboard** restores it. |
| Pointer travels a different distance than `dx`/`dy` | Expected: iOS applies pointer acceleration. The app's calibration measures it; the serial commands do not compensate. |
| Scroll barely moves or goes the wrong way | `SCROLL` and `scroll.dy` are wheel notches, signed; iOS decides the distance and Settings can invert it. Try larger values (80) over the intended pane and check the opposite sign before repeating ([MCP action shapes](../host/control_server/README.md#mcp-action-shapes)). |

## Wi-Fi and the HTTP input tool

| Symptom | Cause and fix |
| --- | --- |
| `wifi` shows `configured=1 connected=0` | Wrong credentials, 5 GHz-only network, or non-ASCII SSID/password. The board is 2.4 GHz station only; re-run `control.py wifi "SSID" "password"`. |
| `phoneuse.local` does not resolve | mDNS blocked by the router or guest network isolation; use the IP from `control.py wifi`. On the phone, the app's `NSBonjourServices` entry is required for `.local` names ([ios/README.md](../ios/README.md#session-flow)). |
| `/status` returns `"hidReady":false` | No phone connected over Bluetooth; the `hint` field says whether to pair or to wait for a reconnect. |
| `POST /input` → `403 Input device secret required.` | The secret on the board differs from `host/control_server/.state/input_device_secret` (or `INPUT_DEVICE_SECRET`). Re-run `bash host/tools/provision.sh PORT SSID PASSWORD`, or `control.py secret --file ...`. The `/status` hint reports when no secret is set at all. |
| `POST /input` → `409 A sequence is already running.` / `Bluetooth HID host is not connected.` | Wait for the batch or `POST /stop`; or pair the phone first. |
| `POST /input` → `400` / `413` | Malformed records or more than 512 records / 10 s of waits: the limits of [input_protocol.md](input_protocol.md). Build bodies with `host/control_server/scripts/encode_actions.js`. |
| `check_device.py` fails | Run it exactly as in the firmware README's acceptance test, with `INPUT_DEVICE_URL` set and the phone connected; it needs the secret from the control server state directory. |

## Phone app and control server

| Symptom | Cause and fix |
| --- | --- |
| App's input tool step shows "not ready" although the board is up | The check needs `running`, `state` and `hidReady` in `/status` and HID readiness; see the `hint` line in the app. Empty address means the upstream USB XIAO at `http://172.31.254.1`, not the ESP32. |
| Control server URL check fails | Use `ws://MAC_IP:8765/device` (keep `/device`) and make sure the Mac and phone share the network; `npm run start:background` must be running. Check `host/control_server/.state/*.log`. |
| `Start Session` succeeds but input never arrives | Session permits are single-use and expire after 120 s; a restarted server invalidates every session. End the session in the app and start again ([PROTOCOL.md](../host/control_server/PROTOCOL.md)). |
| Calibration fails repeatedly or the pointer "disappears" during it | Known iPadOS behaviour: a tap hides the pointer and the next report is not a position. The worker works around it; keep the surface visible and do not touch the screen or move the pointer yourself ([calibration/README.md](../host/control_server/calibration/README.md)). Failed runs keep the previous profile. |
| `428 calibration_required` from `client.js` or the MCP tools | Pointer actions need a saved calibration profile for this device. Run **Calibrate Pointer** in the app. |
| `409 calibration_in_progress` | Stop the calibration before sending other input. |
| `503 iPad is not connected` | The broadcast extension is not connected; start a session in the app and confirm **Start Broadcast**. |
| `504 Completion timed out. Do not retry without checking the iPad.` | The board or the app did not acknowledge within the command timeout. Look at the phone before retrying; the server never retries on its own. |
| Screenshots are stale or black | ReplayKit returns the latest fresh frame scaled to 1280 px; protected content is not capturable, and a stopped broadcast ends the session. |
| Live Activity says "Open app for current status" | Expected without APNs: background updates from the extension are unreliable; push needs a paid team ([LIVE_ACTIVITY.md](../host/control_server/LIVE_ACTIVITY.md)). |
| Build or install fails with signing errors | `ios/.signing.env` is missing or has the placeholder team/bundle ID; the device must be paired, trusted, in Developer Mode and unlocked ([ios/README.md](../ios/README.md#build-and-install)). |

## MCP server and agents

| Symptom | Cause and fix |
| --- | --- |
| Agent sees `status` / `get_screen` / `issue_actions` but no `phone_*` tools (only a placeholder `phone_status`) | The phone service was not running when the agent connected. Start it, then reconnect the agent so the tool list refreshes ([host/README.md](../host/README.md#connect-an-agent)). |
| `issue_actions` rejected with a schema error | `press.keys` must be an object (`{"key":"space","modifiers":["cmd"]}`), the first absolute `click`/`move_to` needs top-level `pointer` and `coordinateSpace`. Valid types are listed in [MCP action shapes](../host/control_server/README.md#mcp-action-shapes). |
| Agent cannot reach `http://127.0.0.1:8780/mcp` from another machine | Loopback-only by design. Use a private transport, or `MCP_HOST=<tailnet IP> MCP_ALLOW_TAILNET=1` with tailnet ACLs as the boundary ([SECURITY.md](../SECURITY.md)). |
| Agent loops on the same failing action | The skill tells it to stop after two attempts; make sure the agent was given [`skills/oh-my-phone-use/SKILL.md`](../skills/oh-my-phone-use/SKILL.md). |

## Phone calls

| Symptom | Cause and fix |
| --- | --- |
| `phone_status` reports `speech.ready: false` with `Speech dependencies missing` | Install `brew install whisper-cpp ffmpeg` and put a ggml model at `~/.cache/phoneuse/models/ggml-large-v3-turbo-q5_0.bin` (or pass `--model`). `say` is part of macOS. |
| `call.slc` is not `"1"` | The phone has no hands-free connection to the board: pair **Oh My Phone Use Audio** under Settings > Bluetooth (a second pairing, separate from the HID one) and set **Call Audio Routing** to Bluetooth Headset ([HFP call audio test](../firmware/README.md#hfp-call-audio-test)). After a reset the board reconnects within 15 s. |
| Phone service will not start: serial port busy | It is the single owner of the port; stop `control.py`, `call_audio.py`, serial monitors and other service instances. |
| `request_id already used for a different call.` / `A session is already running.` / `Phone already has a call; refusing to dial over it.` | Deliberate protections: use a new `request_id` per intended call (and the same one for retries), end the running session with `phone_end`, or end the existing call on the phone first. |
| `phone_start` returns `start_failed` after ~45 s | The call did not connect in time (no SIM, no signal, wrong number, routing not set to the board). For a SIM-less test connect a FaceTime call by hand and use `attach: true`. |
| Transcripts are wrong, especially one-word answers | Expected with short clips; every turn is flagged `uncertain`. The agent must repeat important facts back and ask for confirmation. Raise `end_silence` for speakers who pause mid-sentence. |
| Other party hears nothing / agent hears nothing | Check the `AUDIO` counters with `call_audio.py`: `tx_underrun` means the host stream ran dry, `rx_drop`/`tx_drop` an overflowed ring, `rx_bad` a bad radio link. Only CVSD (8 kHz) is supported. |
| Call did not end after `phone_end` | The service verifies the phone is idle; a rejected or unconfirmed hangup is reported as an error and the user must end the call on the phone. |
| Live listening page is silent | Click **开始旁听** once (browser audio policy) and open the `listen_url` on the same Mac; the page is loopback-only and its token changes on every service restart. |
| A session shows `state: interrupted`, `reason: service_restarted` | The service was restarted during the call; sessions are never redialed automatically. Check the phone and start a new request if needed. A single turn with `status: interrupted_or_failed` carries its own `error` (speech tool failure, serial error, cancelled call). |

## Where the state lives

- Control server logs, PIDs, frames, calibration profiles and the input-device secret:
  `host/control_server/.state/` (`CONTROL_SERVER_STATE_DIR`).
- Phone service log, connection token, call sessions and transcripts: `.state/phoneuse/` at
  the repository root.
- Board: bonds, Wi-Fi credentials and the secret are in NVS and survive reflashing;
  `control.py unpair`, `wifi clear` and `secret clear` remove them.

None of these directories belong in a commit or a bug report; redact secrets, numbers and
transcripts before sharing logs.
