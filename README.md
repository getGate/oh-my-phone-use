# Oh My Phone Use

Let an AI agent use a real iPhone or iPad: see the screen, tap, type and scroll, and make phone
calls that speak and listen on your behalf.

A small ESP32 board is the agent's hands. It acts as a Bluetooth mouse + keyboard for the phone
and as its Bluetooth hands-free unit (car kit) for calls. An app on the phone shares the screen.
No jailbreak and no private APIs: the phone only sees an ordinary Bluetooth accessory.

## Two ways to use it

1. **With your agent** (works today): Claude or any other MCP client connects to one MCP server
   on your Mac and gets screen-control tools and phone-call tools, plus a
   [skill](skills/oh-my-phone-use/SKILL.md) that teaches it how to use them safely.
2. **Standalone phone app** (planned): the app on the phone drives the phone and conducts calls
   by itself, without a Mac. See [Roadmap](#roadmap).

```
                         ┌─────────────── Mac ───────────────┐
Agent (Claude, …) ──MCP──▶ MCP server ─┬─▶ control server ◀──┼── WebSocket ── phone app (screen sharing)
                         │             └─▶ phone service ────┼── USB serial ──┐            │
                         └───────────────────────────────────┘                │       HTTP over Wi-Fi
                                                                              ▼            ▼
                                                     iPhone / iPad ◀── Bluetooth ── ESP32 board
                                                     (mouse + keyboard, hands-free calls)
```

## Status

| Capability | State |
| --- | --- |
| Screen control through MCP (`get_screen`, `issue_actions`) | Works with the default firmware build; pointer calibration in the app |
| Phone calls through MCP (`phone_*`) | Works with the HFP firmware build: dial or attach, spoken turns (macOS `say`), local transcription (whisper.cpp), hangup, saved results |
| Both at the same time on one board | Not yet: the ESP32-PICO-V3 lacks the RAM for Wi-Fi + HFP + BLE HID together |
| Standalone app | Planned |

## Quick start (with your agent)

You need an ESP32-PICO-KIT-1 (classic ESP32), a Mac with ESP-IDF, Node.js 20+, Xcode and an
Apple signing team, and an iPhone or iPad.

1. **Firmware**: build and flash it, pair the phone over Bluetooth, and on an iPhone enable
   AssistiveTouch ([firmware/README.md](firmware/README.md#build-and-flash)):

   ```bash
   idf.py -C firmware set-target esp32
   idf.py -C firmware build
   idf.py -C firmware -p /dev/cu.usbserial-XXXXX -b 230400 flash
   ```

2. **Control server and MCP server** (the first start creates the input-device secret):

   ```bash
   npm ci --prefix host/control_server
   npm run start:background --prefix host/control_server
   npm run calibration:background --prefix host/control_server
   npm run mcp:background --prefix host/control_server        # http://127.0.0.1:8780/mcp
   ```

3. **Provision the board** with that secret and the Wi-Fi network the phone uses; the script
   prints the board's address (`phoneuse.local` or an IP):

   ```bash
   python3 -m venv .venv && .venv/bin/pip install -r host/tools/requirements.txt
   bash host/tools/provision.sh /dev/cu.usbserial-XXXXX "Wi-Fi SSID" "password"
   ```

4. **Install the app** ([ipad_app/README.md](ipad_app/README.md)): copy
   `ipad_app/.signing.env.example` to `ipad_app/.signing.env`, set your team ID and a unique
   bundle ID, then

   ```bash
   bash ipad_app/scripts/build.sh -allowProvisioningUpdates
   xcrun devicectl list devices
   bash ipad_app/scripts/install.sh YOUR_DEVICE_ID
   ```

   In the app, enter `http://phoneuse.local` as the input tool, `ws://YOUR_MAC_IP:8765/device`
   as the control server, run the pointer calibration, then tap **Start Session** and confirm
   **Start Broadcast**.

5. **Connect your agent** to the MCP server and give it the skill. For Claude Code:

   ```bash
   claude mcp add --transport http oh-my-phone-use http://127.0.0.1:8780/mcp
   cp -R skills/oh-my-phone-use ~/.claude/skills/
   ```

   Ask it to do something on the phone. Quick check without an agent:
   `node host/control_server/client.js screen` saves a screenshot under
   `host/control_server/.state/`.

**Phone calls** need the HFP firmware build and the phone service on the Mac; see
[host/README.md](host/README.md#phone-service).

The MCP server and the control server have no authentication: keep them on loopback or a
private network ([SECURITY.md](SECURITY.md)). A call reaches a real person: the skill makes the
agent dial only numbers you gave it, say that it is an AI, and confirm what it heard.

## Repository layout

| Path | What |
| --- | --- |
| [`firmware/`](firmware/README.md) | ESP-IDF firmware: BLE HID mouse + keyboard, Wi-Fi input tool, HFP hands-free unit, host tests |
| [`host/`](host/README.md) | Mac side: unified MCP server and control server (`host/control_server/`, Node), phone service and serial tools (`host/tools/`, Python) |
| [`ipad_app/`](ipad_app/README.md) | iPhone/iPad app: screen sharing (ReplayKit), sessions, pointer calibration, forwards input to the board |
| [`skills/oh-my-phone-use/`](skills/oh-my-phone-use/SKILL.md) | Skill for agents that use the MCP server |
| [`docs/`](docs/input_protocol.md) | Protocols between the parts |

## Development

```bash
make -C firmware/test/host test                           # firmware logic, no hardware
npm test --prefix host/control_server                     # control server, MCP server, calibration
.venv/bin/python -m unittest discover -s host/test/phone  # phone service, simulated board
bash ipad_app/scripts/test_relay.sh                       # app relay against the control server (Xcode)
bash ipad_app/scripts/test_frames.sh
bash ipad_app/scripts/test_setup.sh
```

## Roadmap

- **Calls and screen control together**: an ESP32 module with PSRAM (for example ESP32-WROVER;
  the ESP32-S3/C3 lack Classic Bluetooth and cannot do HFP) to run Wi-Fi, HFP and BLE HID at
  once.
- **Call control and audio over Wi-Fi**: a network protocol on the board for calls, replacing the
  USB serial link, so that the Mac and the phone app use the same board API.
- **Standalone app**: the agent loop, screen control and calls in the phone app, sharing test
  vectors with the control server's action compiler and calibration.

## Credits and license

The screen-control design, the phone app and the control server come from
[jamiepinheiro/ipad_computer_use](https://github.com/jamiepinheiro/ipad_computer_use) (MIT),
where a USB Seeed XIAO RP2040 is the input tool; this project replaces it with the ESP32 and
keeps the parts it needs. [THIRD_PARTY.md](THIRD_PARTY.md) lists the derived files and keeps
the upstream copyright notice. The tag `upstream-ac07704` marks the commit that imported the
upstream snapshot.

Oh My Phone Use is released under the [MIT License](LICENSE).
