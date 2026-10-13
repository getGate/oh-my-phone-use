"""Check the board's HTTP input-tool validation and cancellation without typing.

Targets http://phoneuse.local; set INPUT_DEVICE_URL (for example http://192.168.1.50) for
another address. The firmware reports hidReady only while the phone is connected over
Bluetooth, so pair it first.
"""
import json
import os
import pathlib
import time
import urllib.error
import urllib.request

ROOT = os.environ.get("INPUT_DEVICE_URL", "http://phoneuse.local").rstrip("/")
SECRET = os.environ.get("INPUT_DEVICE_SECRET")
if not SECRET:
    SECRET = (pathlib.Path(__file__).resolve().parents[1] / "control_server" / ".state" / "input_device_secret").read_text().strip()


def request(path, body=None, secret=None):
    headers = {"Content-Type": "text/plain"}
    if secret:
        headers["X-Input-Device-Secret"] = secret
    req = urllib.request.Request(ROOT + path, data=body, headers=headers)
    try:
        response = urllib.request.urlopen(req, timeout=3)
    except urllib.error.HTTPError as error:
        response = error
    with response:
        data = response.read()
        return response.status, json.loads(data) if data else None


code, status = request("/status")
assert code == 200 and "token" not in status and status["hidReady"] and not status["running"], status
baseline = status["reports"]
assert request("/input", b"0310270000")[0] == 403
assert request("/input", b"NOT-HEX", SECRET)[0] == 400
assert request("/input", b"1\n0100780000", SECRET)[0] == 400
assert request("/input", b"0311270000", SECRET)[0] == 400  # Excessive wait.
try:
    assert request("/input", b"0310270000", SECRET)[0] == 202
    assert request("/input", b"0310270000", SECRET)[0] == 409
    assert request("/status")[1]["reports"] == baseline
finally:
    assert request("/stop", b"", SECRET)[0] == 200
time.sleep(0.1)
status = request("/status")[1]
assert not status["running"] and status["state"] == "stopped", status
assert status["reports"] <= baseline + 2, status
baseline = status["reports"]
assert request("/input", b"0200200000")[0] == 403
assert request("/input", b"0201000000", SECRET)[0] == 400  # Unreleased button.
try:
    assert request("/input", b"0310270000", SECRET)[0] == 202
    assert request("/input", b"0200200000", SECRET)[0] == 409
    assert request("/status")[1]["reports"] == baseline
finally:
    assert request("/stop", b"", SECRET)[0] == 200
time.sleep(0.1)
status = request("/status")[1]
assert not status["running"] and status["reports"] <= baseline + 2, status
print("PASS: secret checks, input validation, busy rejection, and cancellation; no key presses.")
