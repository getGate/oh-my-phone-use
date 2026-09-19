#!/usr/bin/env python3
"""
Host-side controller for the PhoneUse firmware (ESP32-PICO-KIT-1 over USB serial).

Examples:
    python3 tools/control.py move 100 50
    python3 tools/control.py click
    python3 tools/control.py type "hello world"
    python3 tools/control.py key ENTER
    python3 tools/control.py scroll -200
    python3 tools/control.py status
    python3 tools/control.py wifi                      # Wi-Fi link state, IP address, hostname
    python3 tools/control.py wifi "My Network" "pass"  # store credentials and connect
    python3 tools/control.py wifi clear
    python3 tools/control.py secret                    # is the input-device secret configured?
    python3 tools/control.py secret --file ipad_computer_use/control_server/.state/input_device_secret
    python3 tools/control.py secret clear
    python3 tools/control.py -p /dev/cu.usbserial-XXXXX ping
    python3 tools/control.py repl          # interactive session, one command per line

The serial port is taken from -p/--port, else $PHONEUSE_PORT, else auto-detected (CP210x bridge).
Exit status: 0 = OK/PONG/STATUS, 1 = ERR from the firmware, 2 = usage, port or timeout error.
"""
import argparse
import os
import sys
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:  # pragma: no cover
    sys.exit("pyserial is required:  python3 -m pip install pyserial")

DEFAULT_BAUD = 115200
DEFAULT_TIMEOUT = 5.0
RESPONSE_WORDS = ("OK", "ERR", "PONG", "STATUS", "WIFI", "SECRET")
CP210X_VID = 0x10C4
COMMANDS = ("move", "click", "down", "up", "scroll", "type", "key", "keydown", "keyup",
            "ping", "status", "unpair", "wifi", "secret", "raw", "repl")


def find_port():
    env = os.environ.get("PHONEUSE_PORT")
    if env:
        return env
    candidates = set()
    for info in list_ports.comports():
        device = info.device
        if sys.platform == "darwin" and device.startswith("/dev/tty."):
            device = device.replace("/dev/tty.", "/dev/cu.", 1)
        looks_like_esp = (
            info.vid == CP210X_VID
            or "CP210" in (info.description or "")
            or "usbserial" in device
            or "SLAB_USBtoUART" in device
        )
        if looks_like_esp:
            candidates.add(device)
    if len(candidates) == 1:
        return candidates.pop()
    if not candidates:
        raise LookupError("no ESP32 serial port found; pass -p /dev/cu.usbserial-XXXX (see: ls /dev/cu.*)")
    raise LookupError("several candidate ports, pass -p to pick one:\n  " + "\n  ".join(sorted(candidates)))


class Device:
    def __init__(self, port, baud, timeout, verbose=False):
        self.timeout = timeout
        self.verbose = verbose
        # Leave DTR/RTS at pyserial's defaults (both asserted). On the ESP32-PICO-KIT-1's
        # CP2102N auto-reset circuit that is the state that does NOT reset the board;
        # clearing the lines after open (dtr=False/rts=False) does reset it.
        self.ser = serial.Serial(port, baud, timeout=0.1)
        self.ser.reset_input_buffer()

    def close(self):
        self.ser.close()

    def command(self, line, timeout=None):
        """Send one line, return the first protocol response line (or None on timeout)."""
        deadline = time.monotonic() + (timeout or self.timeout)
        try:
            payload = (line + "\n").encode("ascii")
        except UnicodeEncodeError:
            raise ValueError("protocol lines must be ASCII")
        self.ser.reset_input_buffer()
        self.ser.write(payload)
        self.ser.flush()
        while time.monotonic() < deadline:
            raw = self.ser.readline()
            if not raw:
                continue
            text = raw.decode("utf-8", "replace").strip()
            if not text:
                continue
            if text.split(" ", 1)[0] in RESPONSE_WORDS:
                return text
            if self.verbose:
                print("  # " + text, file=sys.stderr)
        return None


def escape_text(text):
    for ch in text:
        if ch in "\n\t\r":
            continue
        if ord(ch) < 0x20 or ord(ch) > 0x7E:
            raise ValueError("text must be printable ASCII (got %r)" % ch)
    return (text.replace("\\", "\\\\").replace("\n", "\\n")
                .replace("\t", "\\t").replace("\r", "\\r"))


def quote(value):
    """Quote a WIFI/SECRET argument so it may contain spaces (protocol escapes: \\" and \\\\)."""
    for ch in value:
        if ord(ch) < 0x20 or ord(ch) > 0x7E:
            raise ValueError("value must be printable ASCII (got %r)" % ch)
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def build_line(command, args):
    """Turn CLI words into one protocol line; returns (line, timeout_override)."""
    def need(count):
        if len(args) != count:
            raise ValueError("%s takes %d argument(s)" % (command, count))

    if command == "move":
        need(2)
        return "MOVE %d %d" % (int(args[0]), int(args[1])), None
    if command == "scroll":
        need(1)
        return "SCROLL %d" % int(args[0]), None
    if command in ("click", "down", "up", "ping", "status", "unpair"):
        need(0)
        return command.upper(), None
    if command in ("key", "keydown", "keyup"):
        need(1)
        return "%s %s" % (command.upper(), args[0]), None
    if command == "type":
        if not args:
            raise ValueError("type needs text")
        text = " ".join(args)
        return "TYPE " + escape_text(text), max(DEFAULT_TIMEOUT, 3.0 + 0.05 * len(text))
    if command == "wifi":
        if not args:
            return "WIFI", None
        if len(args) == 1 and args[0].lower() == "clear":
            return "WIFI CLEAR", None
        if len(args) > 2:
            raise ValueError("wifi takes SSID [PASSWORD]")
        return "WIFI " + " ".join(quote(a) for a in args), 10.0
    if command == "secret":
        if not args:
            return "SECRET", None
        if len(args) == 1 and args[0].lower() == "clear":
            return "SECRET CLEAR", None
        if len(args) == 2 and args[0] == "--file":
            with open(args[1], encoding="utf-8") as f:
                value = f.read().strip()
        elif len(args) == 1:
            value = args[0]
        else:
            raise ValueError("secret takes VALUE, --file PATH, or clear")
        return "SECRET " + quote(value), None
    if command == "raw":
        if not args:
            raise ValueError("raw needs a protocol line")
        return " ".join(args), None
    raise ValueError("unknown command %r" % command)


def repl(dev):
    print("PhoneUse HID console. Type protocol lines (e.g. MOVE 20 0, CLICK, TYPE hi, STATUS). Ctrl-D exits.")
    while True:
        try:
            line = input("> ").strip()
        except EOFError:
            print()
            return 0
        if not line:
            continue
        try:
            response = dev.command(line, timeout=max(dev.timeout, 3.0 + 0.05 * len(line)))
        except ValueError as exc:
            print("error: %s" % exc)
            continue
        print(response if response is not None else "ERR timeout (no response from firmware)")


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Control an iPhone through the PhoneUse ESP32 firmware, and provision its Wi-Fi link.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="commands: " + " ".join(COMMANDS) + "\n" + __doc__.split("Examples:")[1],
    )
    parser.add_argument("-p", "--port", help="serial port (default: $PHONEUSE_PORT or auto-detect)")
    parser.add_argument("-b", "--baud", type=int, default=DEFAULT_BAUD)
    parser.add_argument("-t", "--timeout", type=float, default=DEFAULT_TIMEOUT,
                        help="seconds to wait for a response (default %(default)s)")
    parser.add_argument("-v", "--verbose", action="store_true", help="show firmware log lines on stderr")
    parser.add_argument("command", choices=COMMANDS)
    parser.add_argument("args", nargs=argparse.REMAINDER)
    opts = parser.parse_args(argv)

    try:
        port = opts.port or find_port()
        dev = Device(port, opts.baud, opts.timeout, opts.verbose)
    except LookupError as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 2
    except (serial.SerialException, OSError) as exc:
        print("error: cannot open serial port: %s" % exc, file=sys.stderr)
        return 2

    try:
        if opts.command == "repl":
            return repl(dev)
        try:
            line, timeout = build_line(opts.command, opts.args)
        except ValueError as exc:
            parser.error(str(exc))
        try:
            response = dev.command(line, timeout)
        except ValueError as exc:
            parser.error(str(exc))
        if response is None:
            print("ERR timeout (no response from firmware on %s)" % port)
            return 2
        print(response)
        return 0 if response.split(" ", 1)[0] in ("OK", "PONG", "STATUS", "WIFI", "SECRET") else 1
    finally:
        dev.close()


if __name__ == "__main__":
    sys.exit(main())
