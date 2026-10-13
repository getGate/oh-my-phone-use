# Third-party code and licensing

## iPad Computer Use

The screen-control part of Oh My Phone Use is derived from
[jamiepinheiro/ipad_computer_use](https://github.com/jamiepinheiro/ipad_computer_use)
(commit `ac07704`, tagged `upstream-ac07704` in this repository). The idea of
driving an iPad/iPhone through a hardware HID input tool plus a ReplayKit
screen-sharing app comes from that project, and these parts contain its code,
modified for the Oh My Phone Use board:

- `ipad_app/`: iPad/iPhone app, broadcast extension and session widget
- `host/control_server/`: control server, pointer calibration, MCP adapter and the
  input-record codec (`host/control_server/protocol/`)
- `docs/input_protocol.md`: the input tool HTTP protocol, implemented by
  `firmware/components/input_tool/`

That code is used under the following license:

```text
MIT License

Copyright (c) 2026 iPad Computer Use contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Dependencies (not included in this source tree)

- [ESP-IDF](https://github.com/espressif/esp-idf) and the `espressif/mdns`
  component (Apache-2.0), fetched by the ESP-IDF component manager at build time.
- `ws` 8.21.3 (MIT), installed by npm from `host/control_server/package-lock.json`.
- `pyserial` (BSD-3-Clause), installed from `host/tools/requirements.txt`.
- whisper.cpp, ffmpeg and the macOS `say` command, used by the call tools when
  installed separately.
- Apple SDKs and tooling, installed separately.
