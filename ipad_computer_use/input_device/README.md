# Input device

Two boards implement the same HTTP input protocol ([PROTOCOL.md](PROTOCOL.md)):

| Board | Command transport | HID transport | Firmware |
| --- | --- | --- | --- |
| Seeed Studio XIAO RP2040 | HTTP over USB networking (NCM), `http://172.31.254.1` | USB keyboard + mouse | `firmware/input_tool/` (Arduino) |
| Espressif ESP32-PICO-KIT | HTTP over Wi-Fi, `http://phoneuse.local` or IP | Bluetooth LE keyboard + mouse | [PhoneUse firmware](../../README.md) (ESP-IDF), driven by `scripts/esp32_*.sh` |

**XIAO RP2040**: it enumerates as USB networking (NCM), keyboard, mouse, and a
serial diagnostics interface. The host sends HTTP commands over USB; the board
emits HID reports back to that same host.

**ESP32-PICO-KIT**: the classic ESP32 has no USB device controller, so the board
pairs with the iPad/iPhone as a Bluetooth LE HID device and joins the Wi-Fi
network for the HTTP protocol. Records are validated with the same rules and
played back with the same timing (15 ms per record). See
[ESP32-PICO-KIT](#esp32-pico-kit) below.

This component contains no agent, screen capture, control server, or Apple APIs.
It can be used independently with curl. On the XIAO the physical LEDs report
idle, queued, executing, completed, and error states. The external LED-matrix
experiment is not included.

## Layout

- `firmware/input_tool/`: XIAO firmware, bounded input parser, DHCP, onboard LEDs, diagnostics.
- `libraries/tusb-ncm/`: required TinyUSB NCM implementation, with upstream notices.
- `protocol/`: JavaScript action encoder exported as `ipad_input_device/actions`.
- `scripts/`: setup, build, flash, validation, and serial diagnostics; `esp32_*.sh` wrap the ESP32 firmware.
- `test/`: protocol and host-compiled firmware tests.

## Build and flash

Install Arduino CLI, then run from the repository root:

```sh
bash input_device/scripts/setup.sh
bash input_device/scripts/build.sh
bash input_device/scripts/arduino.sh board list
bash input_device/scripts/flash.sh /dev/cu.YOUR_XIAO_PORT
```

The setup script installs the pinned Arduino-Pico **6.1.0** core. Toolchain files
stay in this component's ignored `.tools/` directory; output goes to `build/`.
On another OS, pass the serial port reported by Arduino CLI. For BOOT mode, copy
the generated UF2 to the board's `RPI-RP2` volume instead.

## ESP32-PICO-KIT

The ESP32 firmware is an ESP-IDF project (BLE HID + Wi-Fi station + HTTP server)
maintained in the repository that vendors this fork. The wrappers below locate it
through `PHONEUSE_FIRMWARE_DIR` (default: three directories up, i.e. the parent
checkout) and ESP-IDF through `IDF_PATH` (default `~/esp/esp-idf`); on macOS put
a Python 3.10+ first on `PATH` or set `IDF_PYTHON_DIR`.

```sh
bash input_device/scripts/esp32_build.sh
bash input_device/scripts/esp32_flash.sh /dev/cu.usbserial-XXXX
bash input_device/scripts/esp32_provision.sh /dev/cu.usbserial-XXXX "Wi-Fi SSID" "password"
bash input_device/scripts/esp32_check.sh http://phoneuse.local
```

`esp32_provision.sh` stores the control server's input-device secret
(`control_server/.state/input_device_secret`, or `INPUT_DEVICE_SECRET`) and the
Wi-Fi credentials in the board's NVS over its USB serial port, then prints the
IP address and hostname. Nothing is embedded at build time, so the same binary
works with any control server; re-run the script after the secret changes.

Pair the board (**PhoneUse HID**) in the device's Bluetooth settings. The
firmware reports `hidReady` only while a phone is connected over Bluetooth, and
its `/status` adds `transport`, `bleConnected`, `bonds`, `secretConfigured`,
`wifiConnected`, `ip`, `hostname` and a `hint` describing the next setup step.
The BOOT button sends a left click while idle, for AssistiveTouch's
*Customize Additional Buttons* prompt.

## Direct HTTP example

The XIAO is **172.31.254.1**; the host normally receives **172.31.254.2** by DHCP.
These commands run on the host physically connected to the board, not on a
remote control server. For an ESP32 replace the address with
`http://phoneuse.local` (any host on the same Wi-Fi network can send them).
They type `abc` into the focused field after one second:

```sh
curl -fsS http://172.31.254.1/input \
  -H "X-Input-Device-Secret: $INPUT_DEVICE_SECRET" -H 'Content-Type: text/plain' \
  --data-binary "$(node input_device/scripts/encode_actions.js '[
    {"type":"wait","ms":1000},
    {"type":"keys","sequence":"abc"}
  ]')"
```

For a relative movement of +10, +10 HID counts:

```sh
curl -fsS http://172.31.254.1/input \
  -H "X-Input-Device-Secret: $INPUT_DEVICE_SECRET" -H 'Content-Type: text/plain' \
  --data-binary "$(node input_device/scripts/encode_actions.js '[{"type":"move","dx":10,"dy":10}]')"
```

This is a mouse report, **not** a touch/trackpad gesture or a move to pixel (10,10).
The firmware accepts encoded reports rather than an English command language.
See [PROTOCOL.md](PROTOCOL.md) for the complete format and limits.

## Encode structured actions

After root `npm ci`:

```sh
node input_device/scripts/encode_actions.js '[
  {"type":"keys","sequence":"abc"},
  {"type":"move","dx":10,"dy":10},
  {"type":"click","button":"left"}
]'
```

The wrapper only prints hex; it does not make network requests. Firmware owns
validation and playback.

## Test and troubleshoot

```sh
npm test --workspace input_device
bash input_device/scripts/test.sh
python3 input_device/scripts/check_device.py
INPUT_DEVICE_URL=http://phoneuse.local python3 input_device/scripts/check_device.py   # ESP32
```

The last commands require the board (for the ESP32: on Wi-Fi and connected to a
paired phone). They test validation/cancellation without intentionally pressing
keys; avoid running them during another session. The ESP32 firmware's own
host tests (`make -C test/host test` in the firmware repository) cover its port
of the record validation and key mapping.
On iPad, an empty Ethernet address means DHCP did not complete. Replug first.
For diagnosis, use manual address 172.31.254.2 and mask 255.255.255.248, with no
router or DNS. VPN routing can prevent access to this USB-only subnet.
