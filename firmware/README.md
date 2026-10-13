# Oh My Phone Use firmware

ESP-IDF firmware for the ESP32-PICO-KIT-1. It makes the board a Bluetooth Low Energy mouse +
keyboard for an iPhone or iPad, driven by serial commands from a Mac (`host/tools/control.py`)
or by the HTTP input-tool protocol over Wi-Fi that the phone app uses for screen control
([`docs/input_protocol.md`](../docs/input_protocol.md)). A separate build turns it into a
Bluetooth hands-free unit for phone calls ([HFP call audio test](#hfp-call-audio-test)).

Run the commands below from the repository root: ESP-IDF commands take `-C firmware`, the Mac
tools live in `host/tools/`.

1. [Requirements](#requirements)
2. [Build and flash](#build-and-flash)
3. [Identify the serial port on macOS](#identify-the-serial-port-on-macos)
4. [Pair the ESP32 with an iPhone](#pair-the-esp32-with-an-iphone)
5. [Enable AssistiveTouch](#enable-assistivetouch)
6. [Control utility](#control-utility)
7. [Serial protocol](#serial-protocol)
8. [HTTP input-tool API](#http-input-tool-api)
9. [Architecture](#architecture)
10. [Configuration](#configuration)
11. [Testing](#testing)
12. [Known limitations](#known-limitations)
13. [HFP call audio test](#hfp-call-audio-test)
14. [Planned HFP integration](#planned-hfp-integration)

## Requirements

| Item | Version / notes |
| --- | --- |
| Board | Espressif ESP32-PICO-KIT-1 (ESP32-PICO-V3, classic ESP32, CP2102N USB-UART). ESP32-PICO-KIT V4 (PICO-D4) should work unchanged. |
| ESP-IDF | **v6.1** (this project was built with v6.1). v5.1+ should work; the `espressif/mdns` component is fetched by the component manager on the first build (needs internet once). |
| Python for ESP-IDF | 3.10 or newer (ESP-IDF v6.1 requirement) |
| Host tools | macOS, Python 3 with `pyserial` (`python3 -m pip install -r host/tools/requirements.txt`) |
| iPhone / iPad | iOS 13 or newer (pointer support); tested target is current iOS |
| For screen control | Node.js 20+, macOS with Xcode and an Apple signing team (see [Screen control](../README.md#quick-start-with-your-agent)) |

Install ESP-IDF v6.1 if you do not have it. macOS ships Python 3.9, which ESP-IDF v6.1 rejects,
so put a newer Python first on `PATH` before running `install.sh` and `export.sh`
(for example `brew install python@3.12`, or `uv python install 3.12`):

```bash
export PATH="$(brew --prefix)/opt/python@3.12/libexec/bin:$PATH"   # or the uv-managed python3
mkdir -p ~/esp && cd ~/esp
git clone -b v6.1 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf && ./install.sh esp32
```

## Build and flash

```bash
. ~/esp/esp-idf/export.sh        # once per shell (Python 3.10+ must be first on PATH)
idf.py -C firmware set-target esp32
idf.py -C firmware build
```

Flash and open the serial monitor (replace the port, see next section):

```bash
idf.py -C firmware -p /dev/cu.usbserial-XXXXX -b 230400 flash monitor
```

Exit the monitor with `Ctrl-]`. Close the monitor before using `host/tools/control.py`: the monitor
holds the port exclusively. Rates above 230400 baud produced serial corruption through a USB
hub with this board, hence `-b 230400`. Targets other than `esp32` are not supported.

The partition table is `partitions.csv` (3 MB app, no OTA): Bluedroid + Wi-Fi + lwIP + HTTP
server are ~1.3 MB, more than the 1.5 MB "single app, large" layout leaves free with -Og. The
NVS partition keeps its default location, so BLE bonds, Wi-Fi credentials and the input-device
secret survive reflashing.

## Identify the serial port on macOS

```bash
ls /dev/cu.*
```

With macOS's built-in CP210x driver the board shows up as `/dev/cu.usbserial-XXXXX`
(with the Silicon Labs VCP driver it is `/dev/cu.SLAB_USBtoUART`). Always use the `cu.` name,
not `tty.`. `host/tools/control.py` auto-detects the port when exactly one CP210x bridge is attached;
otherwise pass `-p PORT` or set `PHONEUSE_PORT=/dev/cu.usbserial-XXXXX`.

## Pair the ESP32 with an iPhone

1. Flash the firmware and power the board. It advertises as **Oh My Phone Use HID**
   (`CONFIG_PHONEUSE_HID_DEVICE_NAME`).
2. On the iPhone open **Settings → Bluetooth**. Under *Other Devices* tap **Oh My Phone Use HID** and
   confirm the pairing prompt. Pairing is "Just Works" (no code) unless
   `CONFIG_PHONEUSE_HID_BLE_MITM` is enabled, in which case iOS shows a six-digit code and the
   firmware confirms it automatically.
3. The device moves to *My Devices* as *Connected*. The bond is stored in NVS on the ESP32, so the
   iPhone reconnects automatically after a board reset.
4. To pair again from scratch: run `python3 host/tools/control.py unpair`, then on the iPhone tap the
   (i) next to the device and **Forget This Device**.

## Enable AssistiveTouch

iPhone only. An iPad drives a Bluetooth mouse with its own pointer, so leave AssistiveTouch off
there. With it on, clicks reach apps as finger taps; on the iPad tested, the move that followed a
click was then not reported as a pointer position, which the pointer calibration
has to work around (see [`host/control_server/calibration/README.md`](../host/control_server/calibration/README.md)).

1. **Settings → Accessibility → Touch → AssistiveTouch → On.**
2. With the ESP32 connected a round pointer appears on screen. Under *Pointer Devices* in the same
   screen you can check **Devices** (the ESP32 is listed there; button 1 is "Single-Tap" by default)
   and set **Tracking Sensitivity**. When iOS asks you to *press a button on the device*
   (Customize Additional Buttons), press the board's **BOOT** button: the firmware sends one
   left click while it is idle.
3. Because the ESP32 also presents a keyboard, iOS hides the on-screen keyboard while it is
   connected. Enable **Show Onscreen Keyboard** in the AssistiveTouch settings if you want to keep it.

## Control utility

```bash
python3 -m pip install -r host/tools/requirements.txt

python3 host/tools/control.py ping                 # PONG
python3 host/tools/control.py status               # STATUS connected=1 advertising=0 bonds=1 ... wifi=1 ip=192.168.1.50 player=idle
python3 host/tools/control.py move 100 50          # relative pointer move
python3 host/tools/control.py click                # tap at the pointer
python3 host/tools/control.py down                 # hold the left button ...
python3 host/tools/control.py move 0 300           # ... drag / swipe ...
python3 host/tools/control.py up                   # ... release
python3 host/tools/control.py scroll -200          # scroll (negative = wheel down)
python3 host/tools/control.py type "hello world"   # type ASCII text
python3 host/tools/control.py key ENTER            # press a key; also CMD+H, SHIFT+TAB, a, 0x28
python3 host/tools/control.py wifi                 # WIFI configured=1 connected=1 ssid=... ip=... rssi=-51 hostname=phoneuse.local
python3 host/tools/control.py wifi "My Network" "password"   # store credentials (NVS) and connect
python3 host/tools/control.py wifi clear
python3 host/tools/control.py secret               # SECRET configured=1 source=nvs
python3 host/tools/control.py secret --file host/control_server/.state/input_device_secret
python3 host/tools/control.py secret clear
python3 host/tools/control.py -p /dev/cu.usbserial-XXXXX status
python3 host/tools/control.py -v type hi           # -v shows firmware log lines on stderr
python3 host/tools/control.py repl                 # interactive: type protocol lines directly
```

Options go before the command. Exit status is 0 for `OK`/`PONG`/`STATUS`/`WIFI`/`SECRET`, 1 for
an `ERR` response, 2 for port or timeout errors. The utility opens the port with pyserial's
default DTR/RTS state, which does not reset this board; do not clear DTR/RTS after opening, that
does.

## Serial protocol

Line oriented, 115200 baud 8N1 on UART0. One command per line (`\n`, `\r\n` accepted); the
firmware answers with exactly one line after the action has completed. Commands are
case-insensitive.

| Command | Effect | Response |
| --- | --- | --- |
| `MOVE <dx> <dy>` | Relative pointer move, each in -32767..32767. Split into as many HID reports as needed. | `OK` |
| `CLICK` | Left button press + release (a tap) | `OK` |
| `DOWN` / `UP` | Hold / release the left button (drag with `MOVE`) | `OK` |
| `SCROLL <dy>` | Vertical scroll, wheel units, positive = up | `OK` |
| `TYPE <text>` | Type ASCII text, US layout. Escapes: `\n` `\t` `\r` `\\` | `OK` |
| `KEY <key>` | Press + release a key: `ENTER`, `BACKSPACE`, `TAB`, `ESC`, `UP`/`DOWN`/`LEFT`/`RIGHT`, `HOME`, `END`, `PAGEUP`, `PAGEDOWN`, `DELETE`, `SPACE`, `F1`..`F12`, a single character (`a`, `A`, `/`), a raw usage (`0x28`), or modifier combos (`CMD+H`, `SHIFT+TAB`, `CTRL+ALT+DELETE`) | `OK` |
| `KEYDOWN <key>` / `KEYUP <key>` | Hold / release a key | `OK` |
| `PING` | Liveness check | `PONG` |
| `STATUS` | Link state | `STATUS connected=1 advertising=0 bonds=1 buttons=0 sent=17 failed=0 uptime=42 wifi=1 ip=192.168.1.50 player=idle` |
| `UNPAIR` | Forget all bonded phones (drops the connection) | `OK` |
| `WIFI` | Wi-Fi link state | `WIFI configured=1 connected=1 ssid=Home ip=192.168.1.50 rssi=-51 hostname=phoneuse.local` |
| `WIFI <ssid> [password]` | Store credentials in NVS and connect. Quote values with spaces: `WIFI "My Net" "pass word"` (`\"` and `\\` escapes). No password = open network; otherwise 8..63 characters. | `OK` |
| `WIFI CLEAR` | Forget the stored credentials and disconnect | `OK` |
| `SECRET` | Input-device secret state | `SECRET configured=1 source=nvs` (`nvs`, `build` or `none`) |
| `SECRET <value>` | Store the secret expected in `X-Input-Device-Secret` (1..128 printable ASCII characters, quote if it contains spaces) | `OK` |
| `SECRET CLEAR` | Remove the stored secret (falls back to the build-time value, if any) | `OK` |
| `CALL` | Phone call state (HFP builds) | `CALL slc=1 audio=cvsd call=1 setup=none held=0 number=10086 peer=aa:bb:cc:dd:ee:ff bonds=1` |
| `DIAL <number>` | Place a call: up to 32 characters of `0-9 * # +` | `OK` |
| `ANSWER` / `HANGUP` | Answer the incoming call / reject or end the call | `OK` |
| `AUDIO` | Call audio state and counters; reading restarts the `peak` level meter | `AUDIO mode=stream link=1 rate=8000 rx=... rx_bad=... tx=... tx_underrun=... peak=1234` |
| `AUDIO OFF\|TONE\|LOOP\|STREAM` | Call audio mode: silence, 400 Hz beep into the call, echo after 0.5 s, exchange audio with the host | `OK` |
| `AUDIO CONNECT\|DISCONNECT` | Ask the phone to open / close the audio link | `OK` |
| `A <base64>` | Call audio for the phone in `STREAM` mode: 8 kHz G.711 mu-law, 20 ms (160 bytes) per line | none |

The call commands answer `ERR unsupported` in a build without `CONFIG_PHONEUSE_HFP_ENABLE`,
and `ERR not_connected` while no phone has a hands-free connection. An HFP build also sends
unsolicited lines: `EVT SLC|AUDIO|CALL|SETUP|HELD|RING|CLIP|PAIR|AT_ERROR ...` for link and
call events, and `A <base64>` with the call audio from the phone in `STREAM` mode.

Errors: `ERR invalid_command`, `ERR invalid_args`, `ERR not_connected`, `ERR send_failed`,
`ERR unsupported_char` (nothing is typed), `ERR unknown_key`, `ERR too_many_keys`,
`ERR line_too_long` (lines are limited to 511 bytes), `ERR unsupported` (WIFI/SECRET in a build
with `CONFIG_PHONEUSE_INPUT_TOOL_ENABLE=n`), `ERR not_ready` / `ERR failed` (storage errors).

Firmware log lines (`I (1234) hid_ble: ...`) share the same UART; they never begin with a response
word and never interleave with a response mid-line, so a host can simply skip them.

## HTTP input-tool API

Port 80 (`CONFIG_PHONEUSE_INPUT_TOOL_HTTP_PORT`), reachable as `http://phoneuse.local` (mDNS)
or by IP once the Wi-Fi link is up. It is the protocol documented in
[`docs/input_protocol.md`](../docs/input_protocol.md):

| Route | Behaviour |
| --- | --- |
| `GET /` | Device description |
| `GET /status` | `{"device":"Oh My Phone Use ESP32","transport":"Wi-Fi + BLE","running":false,"state":"idle","waitMs":0,"hidReady":true,"reports":17,"bleConnected":true,"bleAdvertising":false,"bonds":1,"buttons":0,"secretConfigured":true,"wifiConnected":true,"ssid":"Home","ip":"192.168.1.50","rssi":-51,"hostname":"phoneuse.local","batches":3,"uptime":42}` plus a `hint` while something needs attention |
| `POST /input` | Body: hexadecimal five-byte records (`01 mod key 00 00` key, `02 buttons dx dy wheel` mouse, `03 msLo msHi 00 00` wait). Validated like the XIAO firmware (≤ 512 records, ≤ 10 s of waits, buttons released at the end). 202 queued, 400 invalid, 403 bad/missing `X-Input-Device-Secret`, 409 busy or no phone connected, 413 too large |
| `POST /stop` | Release every key and button, stop the batch (state `stopped`); 200 |

Records are played back by a dedicated task at the XIAO's pace (15 ms per record, keys held
15 ms) through the same logical HID API the serial commands use; Arduino key codes are
translated to HID usages (`176` → Enter, `194..205` → F1..F12, ASCII → US layout with Shift).
A batch is stopped, with everything released, on `POST /stop`, after
`CONFIG_PHONEUSE_INPUT_TOOL_DEADLINE_S` (70 s) or when the Bluetooth link drops.

Quick check from any host on the network (with the phone connected over Bluetooth):

```bash
curl -s http://phoneuse.local/status
curl -s -o /dev/null -w "%{http_code}\n" -X POST http://phoneuse.local/input \
  -H "X-Input-Device-Secret: $(cat host/control_server/.state/input_device_secret)" \
  --data-binary "$(node host/control_server/scripts/encode_actions.js '[{"type":"move","dx":50,"dy":0}]')"
INPUT_DEVICE_URL=http://phoneuse.local python3 host/tools/check_device.py
```

## Architecture

```
main/main.c                       wires the modules together, nothing else
components/
  command/command_parser.c        text protocol -> logical HID API + platform hooks (pure C, host-tested)
  command/serial_console.c        UART0 line reader/writer, routes ESP-IDF logs through the UART driver
  hid/hid_device.c                logical HID API: explicit button/key state, report building,
                                  movement splitting, text typing, raw mouse reports (thread-safe)
  hid/hid_motion.c                splits large deltas into straight-line steps of <= max_step (pure C)
  hid/hid_keymap.c                key names / ASCII -> HID usages, US layout (pure C)
  hid/hid_ble.c                   Bluetooth LE HID transport: report map, GAP security/advertising,
                                  esp_hidd device, connection tracking (the only file that knows esp_hid)
  hid/hid_null.c                  transport of builds without Bluetooth HID (PHONEUSE_HID_ENABLE=n)
  hid/private/hid_transport.h     contract between hid_device.c and the transport
  bt_stack/bt_stack.c             controller + Bluedroid host bring-up shared by HID and HFP
  hfp/hfp_client.c                Classic BT hands-free unit: pairing, call control, call events, audio callback
  hfp/call_audio.c                call audio rings, tone / loop test modes, "A <base64>" host lines (pure C)
  hfp/audio_codec.c               G.711 mu-law and base64 (pure C)
  input_tool/input_records.c      XIAO record protocol: hex decoding, validation, Arduino key codes (pure C)
  input_tool/input_player.c       plays a validated batch back through hid_* from its own task
  input_tool/input_http.c         esp_http_server front end: /, /status, /input, /stop, secret check
  input_tool/input_secret.c       X-Input-Device-Secret storage (NVS, build-time fallback)
  input_tool/boot_button.c        BOOT (GPIO0) -> left click while idle
  net/wifi_link.c                 Wi-Fi station, credentials in NVS, reconnect with backoff, hostname
  net/mdns_advert.c               phoneuse.local (_http._tcp, _phoneuse._tcp) via espressif/mdns
test/host/                        host unit tests for parser, key map, motion splitting, record protocol, call audio
../host/tools/control.py          macOS command-line client (pyserial), incl. wifi/secret provisioning
../host/tools/call_audio.py       call audio test tool: events, record, play, text-to-speech, ask + transcribe
```

Data flow: `serial_console` task reads a line → `command_parser_execute()` → `hid_*()`, and
`esp_http_server` task → `input_http.c` → `input_player_submit()` → player task → `hid_*()`.
Both end in `hid_ble.c` → `esp_hidd_dev_input_set()`. `hid_device.c` serialises everything with a
mutex; sending a report blocks until Bluedroid has queued the GATT notification and a small
configurable gap (8 ms) follows every report. Commands execute synchronously: `OK` (serial) or
`state: done` (HTTP) means the last report has been handed to the stack. HID reports are only
ever sent from the console and player tasks, never from Bluetooth or HTTP callbacks.

HID report map (one map, two report IDs): keyboard (ID 1, 8-byte input + 1-byte LED output) and
mouse (ID 2: 5 buttons, X, Y, wheel, horizontal pan, 8-bit relative). The BLE appearance is
"Mouse" (0x03C2) because some iOS versions ignore the generic HID appearance for pointer
purposes. Pairing: LE Secure Connections, bonding, Just Works by default.

Wi-Fi and Bluetooth share the ESP32 radio under software coexistence
(`CONFIG_ESP_COEX_SW_COEXIST_ENABLE`), with Wi-Fi modem sleep kept on as the coexistence code
expects. The build is `-Os` with the Wi-Fi and FreeRTOS hot paths in flash: with the default
`-Og` build the two stacks overflow the ESP32's 128 KB IRAM. After start-up about 87 KB of heap
are free (measured on the ESP32-PICO-KIT-1 before the Wi-Fi association), which the driver's
reduced buffer counts (`sdkconfig.defaults`) keep sufficient for the small HTTP requests of the
protocol.

ESP-IDF APIs used: `esp_bt_controller_*`, `esp_bluedroid_init_with_cfg/enable`,
`esp_ble_gap_*`, `esp_ble_gatts_register_callback` with `esp_hidd_gatts_event_handler`,
`esp_hidd_dev_init`, `esp_hidd_dev_input_set`, `esp_wifi_*` (station, `esp_wifi_set_ps`,
`esp_wifi_sta_get_rssi`), `esp_netif_*`, `esp_event_*`, `esp_http_server`, `mdns_*`,
`nvs_*`, `uart_driver_install/uart_read_bytes/uart_write_bytes`, `uart_vfs_dev_use_driver`,
`esp_log_set_vprintf`, `esp_timer_*`, `gpio_config/gpio_get_level`, FreeRTOS tasks, mutexes and
task notifications.

## Configuration

`idf.py -C firmware menuconfig` → *PhoneUse* menus (defaults in `sdkconfig.defaults` / `components/*/Kconfig`):

| Option | Default | Meaning |
| --- | --- | --- |
| `PHONEUSE_HID_DEVICE_NAME` | `Oh My Phone Use HID` | Bluetooth name shown on the iPhone |
| `PHONEUSE_HID_REPORT_INTERVAL_MS` | 8 | Gap after every HID report (0 disables) |
| `PHONEUSE_HID_MOUSE_MAX_STEP` | 127 | Largest per-report delta for `MOVE`; smaller = slower but more linear pointer travel under iOS acceleration |
| `PHONEUSE_HID_BLE_MITM` | off | Numeric-comparison pairing instead of Just Works |
| `PHONEUSE_HID_ENABLE` | on | Bluetooth LE HID mouse + keyboard (off: HID commands answer `ERR not_connected`) |
| `PHONEUSE_BT_KEEP_CLASSIC` | off | Keep the Classic BT controller running (selected by `PHONEUSE_HFP_ENABLE`) |
| `PHONEUSE_HFP_ENABLE` | off | Hands-free unit: call commands and call audio (see [HFP call audio test](#hfp-call-audio-test)) |
| `PHONEUSE_HFP_DEVICE_NAME` | `Oh My Phone Use Audio` | Classic Bluetooth name shown on the iPhone |
| `PHONEUSE_INPUT_TOOL_ENABLE` | on | Wi-Fi + HTTP input-tool protocol (off = serial-only BLE HID firmware) |
| `PHONEUSE_INPUT_TOOL_HTTP_PORT` | 80 | HTTP port |
| `PHONEUSE_INPUT_TOOL_SECRET` | empty | Build-time input-device secret; the `SECRET` command (NVS) overrides it |
| `PHONEUSE_INPUT_TOOL_RECORD_GAP_MS` | 15 | Spacing between records (includes the report interval) |
| `PHONEUSE_INPUT_TOOL_KEY_HOLD_MS` | 15 | How long a key record holds its key |
| `PHONEUSE_INPUT_TOOL_DEADLINE_S` | 70 | Batch deadline |
| `PHONEUSE_INPUT_TOOL_BOOT_BUTTON` / `_GPIO` | on / 0 | BOOT button sends a left click |
| `PHONEUSE_WIFI_SSID` / `PHONEUSE_WIFI_PASSWORD` | empty | Build-time Wi-Fi defaults; the `WIFI` command (NVS) overrides them |
| `PHONEUSE_WIFI_HOSTNAME` | `phoneuse` | DHCP hostname and mDNS name (`phoneuse.local`) |
| `PHONEUSE_WIFI_MDNS` | on | Advertise `_http._tcp` / `_phoneuse._tcp` |
| `PHONEUSE_WIFI_POWER_SAVE` | on | Keep Wi-Fi modem sleep (recommended with Bluetooth coexistence) |
| `PHONEUSE_WIFI_MAX_RETRY_DELAY_S` | 30 | Longest reconnection backoff |

Changing `sdkconfig.defaults` only takes effect on a fresh `sdkconfig` (delete it, or use
`idf.py -C firmware menuconfig`).

## Testing

Host tests (no hardware, no ESP-IDF):

```bash
make -C firmware/test/host test
```

They cover every serial command including error paths (with and without the network and call
hooks), the key map, the movement splitter (exact sum, per-report bounds, straight-line steps,
20 000 random cases), the input-record protocol (hex decoding, the reference firmware's
validation cases, Arduino key-code translation) and the call audio (G.711 mu-law and base64
codecs, the rings, the tone / loop / stream modes, host audio lines, overflow and underrun).

Hardware acceptance test (requires the board, an iPhone and a Wi-Fi network):

1. `idf.py -C firmware -p PORT -b 230400 flash monitor` – the log ends with `Oh My Phone Use ready: BLE HID ...`.
2. Pair from the iPhone (see above) – the log shows `host connected` and `pairing complete`.
3. Enable AssistiveTouch, then with the monitor closed:
   - `python3 host/tools/control.py status` → `connected=1`
   - `python3 host/tools/control.py move 200 100` → the pointer moves
   - `python3 host/tools/control.py click` → a tap at the pointer
   - `python3 host/tools/control.py scroll -100` → the page scrolls
   - open Notes, tap a text field, `python3 host/tools/control.py type "hello"` → text appears
   - `python3 host/tools/control.py key ENTER` → new line
4. `python3 host/tools/control.py wifi "SSID" "password"` then `wifi` → `connected=1 ip=...`;
   `curl http://phoneuse.local/status` from the Mac shows `"hidReady":true`.
5. `python3 host/tools/control.py secret --file host/control_server/.state/input_device_secret`,
   then `INPUT_DEVICE_URL=http://phoneuse.local python3 host/tools/check_device.py`
   → `PASS`.
6. Reset the board (EN button): the iPhone reconnects and the Wi-Fi link comes back within a few
   seconds; `status` shows `connected=1 wifi=1` again and `uptime` restarts from zero.
7. Run `status` twice in a row: `uptime` must keep increasing, proving that opening the serial
   port from the control utility does not reset the board.

## Known limitations

- Relative pointer only. iOS applies pointer acceleration to every report, so the distance moved
  on screen is not exactly `dx`/`dy` pixels; the app's pointer calibration measures
  this, the serial commands do not.
- Text is limited to ASCII on a US keyboard layout; set the iPhone hardware keyboard layout to
  U.S. Unsupported characters are rejected before anything is typed.
- Scroll amounts are wheel notches; iOS decides how far each notch scrolls.
- One connected host at a time; all bonds are removed together by `UNPAIR`.
- iOS hides the on-screen keyboard while a Bluetooth keyboard is connected (AssistiveTouch →
  Show Onscreen Keyboard restores it).
- `idf.py monitor` and `idf.py flash` reset the board through DTR/RTS; the iPhone reconnects
  automatically. The control utility avoids toggling those lines.
- Wi-Fi station only (2.4 GHz, WPA2/WPA3-PSK or open): the phone and the board must share a
  network. There is no SoftAP fallback and no captive-portal provisioning; credentials go in
  over the serial port. SSIDs and passwords must be printable ASCII.
- The HTTP API is plaintext and protected only by the shared secret; keep the network private
  (see [SECURITY.md](../SECURITY.md)).
- Serial commands and HTTP batches can run at the same time; they interleave at the HID layer.

## HFP call audio test

A separate build configuration, `sdkconfig.hfp_test`, turns the board into a Bluetooth Classic
hands-free unit (a car kit) for the iPhone and nothing else: Bluetooth LE HID and the Wi-Fi
input tool are off, so the test isolates the question "can the call audio reach the host, in
both directions?". You place or answer the call by hand on the phone.

```
iPhone ──Classic BT HFP (CVSD, 8 kHz)──▶ ESP32 ──USB serial, 230400 baud──▶ host/tools/call_audio.py
```

Build and flash into a second build directory (the normal `build/` stays untouched):

```bash
cd firmware
idf.py -B build_hfp -D SDKCONFIG=build_hfp/sdkconfig \
       -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.hfp_test" build
idf.py -B build_hfp -p /dev/cu.usbserial-XXXXX -b 230400 flash
```

Test steps (`-b 230400` for `host/tools/control.py`; `host/tools/call_audio.py` uses it by default):

1. iPhone: **Settings → Bluetooth**, tap **Oh My Phone Use Audio** under *Other Devices* and confirm.
   `python3 host/tools/call_audio.py status` → `CALL slc=1 ...`. After a reset the board connects
   to the bonded phone by itself within 15 s.
2. iPhone: **Settings → Accessibility → Touch → Call Audio Routing → Bluetooth Headset**, so
   every call uses the board. (Or pick *Oh My Phone Use Audio* with the audio button during a call.)
3. Start `python3 host/tools/call_audio.py monitor` and place a call on the phone. Expect
   `EVT SETUP state=dialing`, `EVT AUDIO state=connected frame=...`, `EVT CALL active=1`.
4. **Board → far end:** `python3 host/tools/call_audio.py tone` – the other party hears a beep,
   0.5 s on and 0.5 s off. `AUDIO` shows `tx` counting and `tx_fail=0`.
5. **Far end → board → far end:** `python3 host/tools/call_audio.py loop` – the other party hears
   their own voice 0.5 s late. `peak` in `AUDIO` follows the loudness of the far end.
6. **Far end → host:** `python3 host/tools/call_audio.py record call.wav -s 20`, then
   `afplay call.wav`. The level meter moves while the other party talks.
7. **Host → far end:** `python3 host/tools/call_audio.py say "Hello, this is the computer."`
   (macOS text-to-speech), or `play file.wav` with any 16-bit PCM WAV file.
8. `python3 host/tools/call_audio.py hangup` ends the call; `dial <number>` and `answer` work too.

`python3 host/tools/call_audio.py selftest` runs steps 4, 5 and 7 in one pass during a call (6 s
beep, 10 s echo, a spoken count to five), so the other party only has to listen.

`python3 host/tools/call_audio.py ask "<question>" --voice Tingting --closing "<goodbye>"` is one
question-and-answer turn, the building block for an agent: it waits until a call is active,
speaks the question (macOS text-to-speech) and a beep, records the answer until the other party
stops talking, speaks the closing text, releases the audio link (the iPhone takes the call audio
back) and prints two transcripts. `TRANSCRIPT_CALL` is everything the other party said from the
start of the call to the end of the answer (the answer is its last part); `TRANSCRIPT_CLIP` is
the answer recording alone, which whisper often gets wrong when the answer is only a few words,
because it lacks the context of the rest of the call.
Speech-to-text runs on the Mac with whisper.cpp (`brew install whisper-cpp`, `ffmpeg`, and a
ggml model in `~/.cache/phoneuse/models/`). The spoken text should say that an AI is speaking
and that the answer is recorded.

`AUDIO` counters to watch: `rx_bad` (packets the baseband marked bad), `rx_drop` / `tx_drop`
(a ring overflowed: the host is too slow or sends faster than real time), `tx_underrun` (the
host stream ran dry), `host_bad` (corrupt serial lines). The audio path is CVSD only; wide
band speech (mSBC) is disabled in `sdkconfig.hfp_test`.

## Planned HFP integration

The [HFP call audio test](#hfp-call-audio-test) covers the hands-free unit on its own. Running
it next to BLE HID and Wi-Fi is the open part:

Phase 2 adds Bluetooth Classic HFP (phone audio):

```
Agent ─Wi-Fi─▶ ESP32 ─┬─ BLE HID  ───▶ AssistiveTouch ─┐
                      └─ BR/EDR HFP ─▶ phone audio ─────┴─ iPhone
```

How it plugs in:

- `bt_stack` already builds the controller in dual mode (`CONFIG_BTDM_CTRL_MODE_BTDM`). Set
  `PHONEUSE_BT_KEEP_CLASSIC=y` so Classic BT memory is not released, enable
  `CONFIG_BT_CLASSIC_ENABLED`, `CONFIG_BT_HFP_CLIENT_ENABLE` (the ESP32 is the hands-free unit,
  the iPhone the audio gateway) and the audio data path (`CONFIG_BT_HFP_AUDIO_DATA_PATH_HCI` or PCM).
- Add `components/hfp/` next to `components/hid/`: register the Classic GAP callback
  (`esp_bt_gap_register_callback`), set the Classic device name and class of device (Audio/Video,
  hands-free), initialise `esp_hf_client_*`, and set the scan mode connectable/discoverable.
  Keeping HID on BLE is deliberate: the Classic class-of-device can then describe an audio device
  without confusing the phone's HID handling.
- Command input stays untouched: both the serial console and the HTTP input tool feed the same
  logical HID API.

Coexistence concerns identified (untested; validate before depending on them):

1. **Single radio, time-shared.** BLE connection events, BR/EDR (e)SCO slots and Wi-Fi share one
   radio. During a call the controller reserves SCO/eSCO slots every 3.75/7.5 ms, so BLE events can
   be delayed: expect higher HID latency and jitter, and watch for BLE supervision timeouts.
2. **RAM.** The Classic controller, Bluedroid Classic host, HFP and SCO buffers need on the order
   of 60-100 KB more RAM. With Wi-Fi already running there are ~87 KB free after boot on the
   ESP32-PICO-V3 (520 KB SRAM, no PSRAM); HFP will need `PHONEUSE_INPUT_TOOL_ENABLE=n`, smaller
   Wi-Fi/lwIP buffers, or an ESP32 module with PSRAM.
3. **Two pairings.** BLE bonding and Classic link keys are separate; Bluedroid does not derive one
   from the other. The iPhone will pair once for BLE HID and once for HFP.
4. **Bluedroid callback ownership.** There is one BLE GAP callback (`hid_ble.c`) and one GATT server
   callback (`esp_hidd_gatts_event_handler`, owned by esp_hid). HFP uses the Classic GAP callback, so
   there is no conflict, but any additional BLE service must chain through esp_hid's handler.
5. **Audio data path.** HCI data path streams SCO audio through the host CPU (more RAM and CPU,
   affects command latency); the PCM path needs external codec pins on the PICO-KIT-1.
6. **Controller mode is fixed at boot.** Releasing Classic BT memory is irreversible until reset;
   the mode switch is a compile-time Kconfig flag, not a runtime command.
