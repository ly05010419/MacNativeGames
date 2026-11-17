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
   Calibration then saves once, reports completion and returns the button to
   Calibrate. A fully covered side alone does not commit a partial result.
4. Cancel, closing/hiding the page or disconnecting discards the entire sampling
   session without writing it. After cancellation/completion the dots are centred
   and LT/RT display zero again, as they do before calibration starts.
5. While calibrating, the circles preview the calibrated radial response used by
   the game. Centre, travel and coverage are measured from unmodified raw input.
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
itself to check L/R calibration, cached HID updates and restart-equivalent reload.

原始输入在对角线同时达到 X=1、Y=1 时模长为 √2，按坐标直接绘制会越出圆形。
现在游戏输出和界面预览使用同一径向死区与单位圆限幅，保留方向，不对原始采样做
裁剪；校准中心/行程仍用原始值。该改变影响实际手感，需真手柄确认。
