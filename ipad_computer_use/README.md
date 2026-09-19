# iPad Computer Use

Control an iPad through a tiny input tool. A native iPad app shares its screen
and forwards keyboard and mouse commands to the tool, which plays them back as
real HID input. Two tools are supported:

- **Seeed Studio XIAO RP2040** (upstream design): USB HID + USB networking; the
  app reaches it at `http://172.31.254.1` over the USB cable.
- **Espressif ESP32-PICO-KIT** (this fork): Bluetooth LE HID + Wi-Fi; the app
  reaches it at `http://phoneuse.local` (or its IP address) on the local network.
  No USB connection to the iPad/iPhone is needed, so it also works with Lightning
  iPhones. See [ESP32-PICO-KIT input tool](#esp32-pico-kit-input-tool).

USB-C iPhones are also supported with the XIAO; see [iPhone setup](#iphone-setup)
for the additional AssistiveTouch configuration.

![iPad Computer Use demo](docs/demo.gif)

## Three components

```text
Your client / agent
       | commands and screenshot requests
       v
control_server/  <------ WebSocket ------>  ipad_app/
                                              |
                                  HTTP over USB (XIAO) or Wi-Fi (ESP32)
                                              v
                                         input_device/
                                              |
                              USB HID (XIAO) or Bluetooth LE HID (ESP32)
                                              v
                                            iPadOS
```

| Component | Owns | Does not own |
| --- | --- | --- |
| [Input device](input_device/README.md) | RP2040 firmware, ESP32 firmware integration, HTTP input protocol, HID reports, input codec | Screenshots, sessions, agents |
| [Control server](control_server/README.md) | Session permits, action routing, screenshot requests, calibration profiles | Apple APIs or physical USB access |
| [iPad app](ipad_app/README.md) | User-started sessions, ReplayKit capture, Live Activity, forwarding input to the tool | Agent decisions or firmware builds |

The control server runs independently of the iPad and can be hosted on another
computer. The iPad initiates the connection; it does not need an incoming server.

## Security model

This project intentionally does **not** include an app-layer pairing password or
OAuth flow. Its security model relies on the control network being reachable
only by you. Run the control server locally, on a private network such as
Tailscale, or behind a reverse proxy/tunnel that already authenticates access.
Do not expose the raw control server, calibration server, or MCP adapter on the
public internet.

**Current scope:** one connected iPad, ordered high-level computer-use actions,
fresh screenshots, native pointer calibration, and a no-auth MCP
adapter that defaults to loopback with optional private Tailnet access. This is not yet a multi-client pub-sub
service. Low-level mouse deltas remain available for diagnostics, but MCP
callers should use the high-level schema.

## Quickstart

You need a **Seeed Studio XIAO RP2040**, a USB data cable, and an iPad or USB-C iPhone.
For an **ESP32-PICO-KIT** instead of the XIAO, do steps 1, 3 and 4 below, then follow
[ESP32-PICO-KIT input tool](#esp32-pico-kit-input-tool) for the firmware, pairing and
the input-tool address.
Physical testing has used an M-series iPad and an iPhone 17 Pro. Development needs Node.js 20+
and npm; firmware builds need Arduino CLI. Building/installing the iPad app
requires macOS, full Xcode with command-line tools, and Apple signing credentials.
You do not need to open the Xcode UI.

### 1. Install dependencies

From the project root:

```sh
npm ci
```

### 2. Prepare the input tool

Flash the XIAO with the input-tool firmware:

```sh
bash input_device/scripts/setup.sh
bash input_device/scripts/build.sh
bash input_device/scripts/arduino.sh board list
bash input_device/scripts/flash.sh /dev/cu.YOUR_XIAO_PORT
```

The build script creates or reads `control_server/.state/input_device_secret`
and embeds it into the firmware. Start the control server from the same checkout
or set `INPUT_DEVICE_SECRET` explicitly in both places.

Use the serial port listed for your XIAO. If automatic flashing fails, connect
the board while holding BOOT, release it, and copy
`input_device/build/firmware/input_tool.ino.uf2` to the `RPI-RP2` drive.
Only use this firmware on a XIAO RP2040. [Firmware details](input_device/README.md)

### 3. Start the control server

```sh
npm run start:background --workspace control_server
npm run calibration:background --workspace control_server
npm run mcp:background --workspace control_server
```

The device connection uses port **8765**; calibration uses **8766**. For this
quickstart, use a trusted Wi-Fi network and allow those ports through the host
firewall only when that network is private to you.

### 4. Install the iPad app

```sh
cp ipad_app/.signing.env.example ipad_app/.signing.env
```

Set your Apple team ID and a unique bundle ID in that local file, then:

```sh
bash ipad_app/scripts/build.sh -allowProvisioningUpdates
xcrun devicectl list devices
bash ipad_app/scripts/install.sh YOUR_IPAD_DEVICE_ID
```

The iPad must be paired, unlocked, and in Developer Mode. Installation can use
the paired wireless connection. [App setup and signing](ipad_app/README.md)
For iPhone, use its device ID in the same install command, then complete
[iPhone setup](#iphone-setup) before pointer calibration.

### 5. Start a session

1. Plug the input tool into the iPad and keep Wi-Fi connected.
2. Open **iPad Computer Use**. Follow setup: connect the input tool, enter `ws://YOUR_SERVER_IP:8765/device`, then complete pointer calibration. Later, edit the server directly on the main page.
3. Tap **Start Session**, then confirm **Start Broadcast** in Apple's prompt.
4. Run **Calibrate Pointer** if requested. Keep the calibration screen open until it finishes.

There is no App/Broadcast mode selection. A session enables both screenshots and
input. End Session revokes new commands and requests release of existing input.

### 6. Try it

On the control_server host:

```sh
node control_server/client.js status
node control_server/client.js screen
node control_server/client.js send 'hello'
```

The last command types into the **currently focused iPad field**; focus a test
note first. Screenshots are saved under `control_server/.state/` by default.
More actions and API examples are in the [control_server guide](control_server/README.md).

For ChatGPT/Codex through OpenAI Secure MCP Tunnel, point the tunnel at:

```text
http://127.0.0.1:8780/mcp
```

For an agent that is itself on your Tailnet, you can instead bind the same MCP
service to the host's Tailscale IP and use `http://TAILSCALE_IP:8780/mcp`.
Set `MCP_HOST=TAILSCALE_IP` and `MCP_ALLOW_TAILNET=1` for that mode. The MCP
adapter has no MCP-layer auth. Run the control server on a trusted host or
behind a private transport such as Tailscale or OpenAI Secure MCP Tunnel. There
is intentionally no app-layer pairing password.

## ESP32-PICO-KIT input tool

The ESP32-PICO-KIT (ESP32-PICO-V3/D4, classic ESP32) has no native USB, so the
tool cannot enumerate as a USB device. Instead the ESP32 is a **Bluetooth LE
mouse + keyboard** for the iPad/iPhone and exposes the same HTTP input protocol
as the XIAO **over Wi-Fi**. The iPad app talks to it exactly like it talks to
the XIAO, only the address changes.

```text
control_server  <-- WebSocket -->  ipad_app  -- HTTP over Wi-Fi -->  ESP32-PICO-KIT
                                                                          |
                                                                     Bluetooth LE HID
                                                                          v
                                                                    iPadOS / iOS
```

The firmware lives in the [PhoneUse](../README.md) repository that vendors this
fork (`../` from here); the `input_device/scripts/esp32_*.sh` wrappers find it
through `PHONEUSE_FIRMWARE_DIR` (default: the parent checkout). It needs ESP-IDF
v5.1+ (built and tested with v6.1); see the firmware README for the toolchain.

1. **Build and flash** (the board shows up as `/dev/cu.usbserial-XXXX` on macOS):

   ```sh
   bash input_device/scripts/esp32_build.sh
   bash input_device/scripts/esp32_flash.sh /dev/cu.usbserial-XXXX
   ```

2. **Start the control server** (step 3 of the quickstart) so the input-device
   secret exists in `control_server/.state/input_device_secret`, then **provision**
   the secret and the Wi-Fi credentials over the board's serial port. The board
   must join the same Wi-Fi network as the iPad/iPhone:

   ```sh
   bash input_device/scripts/esp32_provision.sh /dev/cu.usbserial-XXXX "Your Wi-Fi" "password"
   ```

   Both values are stored in the ESP32's flash (NVS) and survive reflashing. The
   script prints the board's IP address and mDNS name (`phoneuse.local`).

3. **Pair over Bluetooth**: on the iPad/iPhone open **Settings > Bluetooth** and
   tap **PhoneUse HID** under *Other Devices*. The bond is stored on the ESP32, so
   it reconnects automatically after a reset.

4. **AssistiveTouch** (iPhone only): follow [iPhone setup](#iphone-setup), but
   select **PhoneUse HID** under *Devices* and press the ESP32's **BOOT** button
   when iOS asks for a button press (it sends a left click while the tool is
   idle). Assign **Button 1** to **Single-Tap**.

   An iPad does not need it: iPadOS drives a Bluetooth mouse with its own
   pointer, so leave AssistiveTouch **off** there. With it on, clicks reach apps
   as finger taps, and on the iPad tested the move that followed a click was
   not reported as a pointer position. Calibration works around that (it nudges
   the pointer after each click, see
   [calibration](control_server/calibration/README.md)), but it is slower and
   one more thing between the mouse button and the app.

5. **Install the app** (step 4 of the quickstart), open it, and enter the input
   tool address in the first setup step (or later under **Edit Input Tool** on
   the main page): `http://phoneuse.local`, or `http://<IP address>` if `.local`
   names do not resolve on your network. The **Input Tool** row shows
   *PhoneUse ESP32 · Wi-Fi + BLE* once the board is reachable **and** connected
   to the device over Bluetooth; until then it shows what is missing (pair, set
   the secret).

6. Continue with the control server URL and pointer calibration as usual.

Differences from the XIAO to be aware of:

- `hidReady` means "a phone is connected over Bluetooth LE", and `POST /input`
  answers 409 while no phone is connected. Everything else in the
  [protocol](input_device/PROTOCOL.md) is unchanged.
- The HTTP server never blocks while a key is held: `POST /stop` takes effect
  within a few milliseconds and a batch is stopped by the 70-second deadline or
  by a lost Bluetooth link, releasing every key and button.
- Wi-Fi and Bluetooth LE share one radio. Expect slightly more latency per
  report than over USB; the record spacing (15 ms) matches the XIAO firmware.
- Plaintext HTTP on your Wi-Fi network instead of a private USB link: keep the
  network trusted, see [SECURITY.md](SECURITY.md).
- Serial diagnostics use the firmware's own console (`python3 tools/control.py
  status`, `wifi`, `secret` in the firmware repository), not `read_diagnostics.py`.

## iPhone setup

Use the same firmware, control server, and app as for iPad. Before running the
app's pointer calibration:

1. Connect the flashed XIAO to the iPhone using a USB data cable. Keep Wi-Fi connected for access to the control server.
2. Open **Settings > Accessibility > Touch > AssistiveTouch** and turn **AssistiveTouch on**. Leave it enabled while using mouse input.
3. Under **Devices**, select **XIAO RP2040**, then **Customize Additional Buttons...**.
4. When asked to press a button on the pointer device, briefly press the XIAO's **BOOT** button while it is already connected and idle. The current firmware emits a left mouse click. Do not press RESET or hold BOOT while connecting; that enters the flashing workflow instead.
5. Assign **Button 1** to **Single-Tap**, as shown below. If Button 1 is already listed, edit its action directly.

<img src="docs/iphone_assistive_touch.png" alt="iPhone AssistiveTouch settings for XIAO RP2040, with Button 1 assigned to Single-Tap" width="360">

Apple's [AssistiveTouch guide](https://support.apple.com/en-gb/111794) provides
additional accessibility setup details.

Return to **iPad Computer Use**, enter your control server URL, and complete
pointer calibration. Confirm Apple's **Start Broadcast** prompt when asked.
Keep the calibration screen open and **do not touch the screen or press BOOT
during calibration**: AssistiveTouch delivers mouse clicks as touch events, so
manual taps can interfere with the measurements. Calibration automatically
moves the pointer, checks six target clicks, and saves the resulting profile
on the control server.

If the pointer moves but clicks fail, check both that AssistiveTouch is enabled
and that **Button 1 = Single-Tap**, then retry with the latest app build. If
Safari can open `http://172.31.254.1/status` but the app cannot reach the input
tool, check the app's **Local Network** permission under **Settings > Privacy &
Security > Local Network**.

## Development

```sh
npm test
bash input_device/scripts/test.sh
bash input_device/scripts/esp32_check.sh http://phoneuse.local   # ESP32 tool, phone paired
bash ipad_app/scripts/test_relay.sh
bash ipad_app/scripts/test_frames.sh
bash ipad_app/scripts/test_setup.sh
npm run release:check
```

Each component contains its own sources, scripts, tests, and documentation.
The server depends on the input device's exported JavaScript codec package;
the iPad app communicates over documented network protocols, not source imports.

## Before publishing

See [SECURITY.md](SECURITY.md) for network and session limitations, and
[THIRD_PARTY.md](THIRD_PARTY.md) for dependency notices. Original code uses the
[MIT License](LICENSE). Packages remain private to prevent accidental npm publication.
The Live Activity implementation builds, but its background update and dismissal
behavior still needs physical-device verification.

Secrets, signing configuration, toolchains, builds, screenshots, and retired
experiments are excluded by `.gitignore`. Retired local work is preserved in
`.local/archive/`, not in the release tree. Do not publish the entire working
directory as an unfiltered archive.

Run `npm run release:pack` to create the allowlisted source archive at
`.local/releases/ipad_computer_use_source.tar.gz`. It includes the license and
third-party notices, and excludes local credentials, screenshots, and builds.
