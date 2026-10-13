# Working on the firmware

ESP-IDF firmware for the ESP32-PICO-KIT-1 (classic ESP32, no PSRAM). [`README.md`](README.md)
is the specification: requirements, build variants, the serial protocol, the HTTP input-tool
API, architecture, every Kconfig option and the hardware acceptance test. Read its
*Architecture* section before changing a component.

## Build

- ESP-IDF **v6.1**, Python 3.10+ first on `PATH`, `. ~/esp/esp-idf/export.sh` once per shell.
  Target is `esp32` only; `set-target` to anything else is unsupported.
- Always from the repository root with `-C firmware`, and flash at `-b 230400` (faster rates
  corrupt through USB hubs on this board). Serial ports are `/dev/cu.usbserial-*`, never `tty.`.
- Three build configurations, each in its own build directory so they do not overwrite each
  other's `sdkconfig`:

  | Build | Directory | Defaults | Contents |
  | --- | --- | --- | --- |
  | Default (screen control) | `build/` | `sdkconfig.defaults` | BLE HID + Wi-Fi input tool, HFP off |
  | HFP test | `build_hfp/` | `sdkconfig.defaults;sdkconfig.hfp_test` | Hands-free unit only |
  | HFP + HID | `build_hfp/dual/` | `…;sdkconfig.hfp_test;sdkconfig.hfp_hid_test` | HFP + BLE HID, Wi-Fi off, heap diagnostics |

  `sdkconfig` (generated) is ignored; a change to `sdkconfig.defaults` needs a fresh
  `sdkconfig` to take effect. New options go into `components/*/Kconfig` with the `PHONEUSE_`
  prefix, get a default in `sdkconfig.defaults` and a row in the README's *Configuration* table.
- Without ESP-IDF installed you cannot build or flash. Say so; do not report a build as done.

## Architecture rules

- `main/main.c` only wires modules together.
- These files are **pure C** (no ESP-IDF headers) and are compiled into the host test:
  `command/command_parser.c`, `hid/hid_keymap.c`, `hid/hid_motion.c`,
  `input_tool/input_records.c`, `hfp/audio_codec.c`, `hfp/call_audio.c`. Platform access goes
  through the hook structs they receive (`command_platform_t`, …). Keep protocol parsing,
  validation and codecs in this layer so they stay testable; put new ESP-IDF calls in the
  transport/driver files next to them.
- `hid/hid_ble.c` is the only file that knows `esp_hid`; the contract with `hid_device.c` is
  `hid/private/hid_transport.h`. `hid_null.c` is the transport of builds with
  `PHONEUSE_HID_ENABLE=n`; keep both transports in step.
- HID reports are sent only from the serial console task and the input-player task, never
  from Bluetooth, Wi-Fi or HTTP callbacks. `hid_device.c` serialises with a mutex and commands
  complete synchronously: `OK` / `state: done` mean the last report is queued in the stack.
- Memory is the binding constraint: ~87 KB heap free after boot with Wi-Fi, IRAM is full enough
  that the build is `-Os` with Wi-Fi/FreeRTOS hot paths in flash, and the 3 MB app partition
  (`partitions.csv`, no OTA) is deliberate. Any new task, buffer or stack feature must be
  checked against the free heap (the `dual` build's `PHONEUSE_HEAP_DIAGNOSTICS` prints it
  every five seconds). The README's *Status* and *Planned HFP integration* sections record why
  Wi-Fi + HFP + BLE HID do not fit yet; do not claim otherwise.
- Serial responses are exactly one line per command; log lines share the UART and must never
  begin with a response word (`OK`, `ERR`, `PONG`, `STATUS`, `WIFI`, `SECRET`, `CALL`, `AUDIO`,
  `EVT`, `A `). Unsolicited `EVT`/`A` lines exist only in HFP builds.

## Changing a protocol

- **Serial command or response**: update the parser, the README's *Serial protocol* table, the
  tests in `test/host/test_main.c`, and the Python clients in `host/tools/` (`control.py`,
  `call_audio.py`, `phone_service.py`, `provision.sh`) that parse the response.
- **HTTP input tool**: the routes, records, limits and status codes are the upstream XIAO
  protocol in `docs/input_protocol.md`. The phone app and the control server depend on it, and
  the app must keep working with the upstream USB tool, so extend the `/status` JSON (as
  `transport` and `hint` were) rather than changing existing fields or codes.

## Tests

```bash
make -C firmware/test/host test     # cc, -Wall -Wextra -Werror; tens of millions of checks, a few seconds
```

Add cases to `test/host/test_main.c` (stubs for ESP-IDF headers live in `test/host/stubs/`,
the HID mock in `mock_hid.c`). Everything that can be covered there must be; the hardware
acceptance test in the README is for the board, pairing, AssistiveTouch, Wi-Fi and the secret,
and needs a phone and a network.

## Serial port etiquette

`idf.py monitor` holds the port exclusively and resets the board through DTR/RTS; close it
before `host/tools/control.py` or the phone service. The host tools deliberately leave DTR/RTS
alone so the board is not reset: keep it that way.
