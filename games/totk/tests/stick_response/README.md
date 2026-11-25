# Stick calibration response / 摇杆校准响应

Run from the repository root:

```sh
xcrun clang -std=c11 -Wall -Wextra -Werror -fsanitize=undefined \
  -Igames/totk/runtime games/totk/tests/stick_response/probe.c \
  -o /tmp/stick-response-probe && /tmp/stick-response-probe
```

The probe checks centre, deadzone boundary, diagonal unit-circle limits, direction
preservation, and invalid inputs. Sweep coverage: 444,411 XY/deadzone combinations.

Manual macOS checks (not replaced by the probe):
1. Open Input settings with a controller: both dots stay centred and triggers zero.
2. There is one **Calibrate / 校准** button. Click it to start both sticks; it
   becomes **Cancel / 取消**. Release to capture centre, then slowly sweep both
   sticks at full physical travel. Bright sectors are covered; grey sectors need
   more samples (24 sectors, at least two samples each, radius >= 0.55).
3. Both sticks must cover the complete outer ring and sufficient travel in all
   directions. Keep measuring until both are released to centre (within 0.15).
   Calibration then applies once for the connection, reports completion and returns the button to
   Calibrate. A fully covered side alone does not commit a partial result.
4. Cancel, closing/hiding the page or disconnecting discards the entire sampling
   session without applying it. After cancellation/completion the dots are centred
   and LT/RT display zero again, as they do before calibration starts.
5. While calibrating, the circles preview the current response used by the game
   (per-axis until calibrated, radial for the calibrated source). Full diagonals
   are limited to the well only for display: the whole dot and its glow stay
   inside the circle. Centre, travel and coverage are measured from unmodified raw input.
   Calibration cannot infer a device's physical stop: sweep to its actual edge.
6. Left/right deadzones are independent (0–50%, default 12%). Restart with the
   same controls directory and verify `controller-deadzone-left.txt` and
   `controller-deadzone-right.txt`. A missing side falls back to the legacy
   `controller-deadzone.txt`, then to 12%. No trails are retained.
7. Open settings using keyboard 1 without first pressing a controller button.
   An already registered native/HID controller enables Calibrate immediately;
   disconnect disables it and cancels sampling. Verify LT/RT while calibrating,
   including release and reconnection. Background input remains opt-in.
8. The existing upstream DSU source keeps its original response; this calibration
   tool currently targets native GameController and fallback HID controllers.

Automated native state-machine coverage:
`python3 -m unittest discover -s tests -p 'test_stick*.py' -q`
The UI test compiles the actual calibration class from settings_panel.h and drives
its methods without showing a window. Gamepad-background's probe uses gamepad.m
itself to check L/R calibration, cached HID updates and connection-local calibration, capture release and HID reset.

### Controller identity and calibration lifetime

Uncalibrated sticks retain the upstream per-axis response, including full diagonals.
Calibrated sources use a circular response; calibration is scoped to the sampled
physical connection and each stick's native/HID coordinates. Reconnects and app
restarts require a new calibration. The runtime does not load, create, or overwrite
the old unbound profile file. Left and right deadzone preferences still persist.
Capture release uses the source response shape and the greater of the configured
deadzone and 12%; DSU keeps its stock per-axis response for play and release.
