# Input tool HTTP protocol

Base URL: `http://172.31.254.1` for the XIAO RP2040 (transport: the attached USB
NCM interface), or the address saved in the iPad app for the ESP32-PICO-KIT
(`http://phoneuse.local` or `http://<ip>`, transport: Wi-Fi). Both firmwares
implement the routes, records, limits and status codes below.
`GET /status` returns non-sensitive readiness state. Mutating requests require
the `X-Input-Device-Secret` header. By default, the XIAO firmware build script
embeds the secret stored in `control_server/.state/input_device_secret`, or the
value of `INPUT_DEVICE_SECRET` when that environment variable is set; the ESP32
stores the same secret in NVS (`input_device/scripts/esp32_provision.sh`).

| Route | Behavior |
| --- | --- |
| `GET /` | Device/API description |
| `GET /status` | hidReady, running, state, waitMs, reports |
| `POST /input` | Validate and queue ordered keyboard/mouse/wait reports |
| `POST /stop` | Request key/button release and stop playback |

`/input` body is hexadecimal five-byte records. Use wait records for timing.
There is no separate delay field. Each record is:

| Kind | Five bytes |
| --- | --- |
| Key | `01 modifiers key 00 00` |
| Mouse | `02 buttons dx dy wheel` |
| Wait | `03 millisecondsLow millisecondsHigh 00 00` |

Mouse deltas/wheel are signed 8-bit values. Buttons: left=1, right=2, middle=4.
Key modifiers: Ctrl=1, Shift=2, Alt=4, Command=8. Key values use the Arduino
keyboard encoding; the exported codec handles names and U.S.-layout ASCII text.
Keys press then release. A drag is mouse records with a held button followed by
a zero-button release. It is not a native touch digitizer gesture.

At most 512 records; total waits at most 10 seconds; no unreleased buttons at
the end. Only one batch runs at a time. The JavaScript codec additionally limits
structured batches to 128 actions. Large deltas split into multiple reports.

`POST /input` returns 202 when the batch is queued. `POST /stop` returns 200
when stop has been requested. Successful mutating responses have empty bodies.
Malformed input returns 400, wrong or missing secrets return 403, and a running
batch returns 409. A queued batch has not necessarily completed: poll `/status`
until `running` is false and `state` is `done`. A reboot can still make an
in-flight result uncertain. Do not automatically retry. Firmware has a 70-second
deadline and stops on USB unmount/suspend. HTTP handling may wait while a
key/button is held; `/stop` is not a guaranteed instantaneous emergency stop.
Unplug the tool when input release cannot be confirmed.

The protocol carries relative HID counts, not screenshot coordinates. Neither
the firmware nor its codec knows the current iPad pointer position.

## ESP32-PICO-KIT specifics

- `hidReady` is true only while a phone is connected over Bluetooth LE. While it
  is false, `POST /input` returns 409 with an explanatory `error`.
- `GET /status` additionally reports `transport` (`"Wi-Fi + BLE"`),
  `bleConnected`, `bleAdvertising`, `bonds`, `buttons`, `secretConfigured`,
  `wifiConnected`, `ssid`, `ip`, `rssi`, `hostname`, `batches`, `uptime`, and,
  while something needs attention, a human-readable `hint`. `reports` counts
  HID reports accepted by the Bluetooth stack.
- `waitMs` is the remaining time of the wait record being executed.
- The HTTP handler runs in its own task, so requests are answered while a key
  or button is held and `POST /stop` is honoured within a few milliseconds
  (during waits, at 20 ms granularity). A stop always releases every key and
  button, also when nothing is running, and sets `state` to `stopped`.
- A batch also stops (with all input released) when the Bluetooth link drops or
  the 70-second deadline passes.
- Bodies larger than the 5120-character limit return 413 instead of 400.
