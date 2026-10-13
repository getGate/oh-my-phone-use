# Native pointer calibration

Owned by the control server. The iPad provides a UIKit calibration surface and
pointer/click telemetry; the worker measures movement and validates six targets.
The input tool remains unaware of coordinates or calibration.

Start the calibration service with `npm run calibration --prefix control_server`.
Start an iPad session, then choose Calibrate Pointer (or Settings > Recalibrate
Pointer). Keep the surface visible and do not manually move the pointer.

Twelve movement magnitudes are measured on both axes. A native profile is saved
only after all six targets pass with maximum error <= 3 UIKit points. Failed
runs preserve the previous profile. Disconnect, Stop, or viewport change cancels
the worker and requests input cancellation; uncertain actions are not replayed.

## Telemetry the worker does not take for granted

A pointer report normally shows up as one position sample. Three cases where it
does not are handled instead of ending the run:

- **Hidden pointer.** A direct touch hides the iPadOS pointer: the finger that
  opened the page, and every click when AssistiveTouch delivers the mouse button
  as a tap. The report that brings the pointer back is not reported as a
  position. The worker nudges the pointer out and back (`wake`) before the first
  move and after every click until the page reports a position again.
- **Late samples.** Through a Wi-Fi + BLE input tool a sample can trail the
  tool's completion. A position is read once samples have stopped for
  `CALIBRATION_SETTLE_MS` (120 ms), not at the first new sample.
- **Unobserved moves.** A move without a sample within
  `CALIBRATION_FRESH_WAIT_MS` (2500 ms) is recorded, the pointer is found again
  and the measurement is repeated, at most 8 times per run. Moves are harmless
  to repeat because the position is read back every time. **Clicks are never
  repeated.**

If positions are reported more coarsely than the slowest reports travel, those
measure as no travel; the curve is then extended linearly through the origin and
kept non-decreasing. (The iPad tested reports sub-pixel positions, where one
count measures as 0.20 points, so this did not apply.) The worker checks the
profile with the same rules the control server uses to load it, so "Calibration
complete" always means pointer actions are accepted.

The six targets are reached with position feedback, which real use does not
have. After they pass, four open-loop moves (one batch of reports each, exactly
as `/computer-use/actions` sends them) are measured and summarised as
`openLoop` in the profile. It is informative, not a gate: a large `maxError`
there means the curve does not predict batched reports on this transport. Set
`CALIBRATION_OPEN_LOOP=0` to skip it.

One run may take `CALIBRATION_BUDGET_MS` (300000); the lease and the worker share
it. Expect roughly 40 s over USB and 80-90 s through a Wi-Fi + BLE input tool
(about 90 actions at ~0.3 s and ~0.6 s each, plus settling).

## State

Profiles are stored as `.state/pointer-<deviceID>-<width>x<height>.json`. Every
run, passed or failed, also writes `.state/calibration-<timestamp>.json` with
each move, the stage reached, wake-ups, the open-loop check and the error. They
are runtime data, never source-controlled examples. Both services use
CONTROL_SERVER_STATE_DIR.

The current gate matches device identity and a validated profile. It does not
automatically detect changes to pointer speed or live orientation. Recalibrate
after changing either. Calibration is not an absolute-position tracker across
apps; UIKit points are not necessarily screenshot pixels.

`simulated_ipad.js` is a test double (control server, input tool and calibration
page) used by `calibrate.test.js`; it never touches real HID.

The former Safari calibration page, anonymous telemetry route, open-loop test
driver, and browser screenshots are retired. Only the native app flow is shipped.
