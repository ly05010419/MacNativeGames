# OOT native logic time step (Mac)

Update 131072 has two nominal-frame-rate ratio producers: input at
`main+0x3b4eaf8` and logic at `main+0x6e6cc4`. Both divide a native numerator
by the framework's configured FPS at `+0x2c8`. The logic numerator retains
the game's original 30/60 selector. At a configured 60 FPS and an actual
29 FPS, this advances about half a second of simulation per wall second.

The framework already owns the actual elapsed system ticks at `+0x100`,
written by `main+0x346c7ec`. The SDK tick frequency is 31,250,000 Hz. The
Mac patch substitutes `numerator * elapsed_ticks / 31250000` at both ratio
producers. The native VFR update at `main+0x3b4e300` still applies the game's
pause, time-scale and domain settings. SDK clocks, GPU timestamps, frame
pacing, resolution and rendering work are unchanged. No rolling estimate
of FPS or global clock multiplier is used. A zero initial duration retains
the original division. Very long frame durations are not artificially
clamped; loading, pause transitions and long hitches need separate coverage.

The patch checks the complete instruction contexts and text layout before
writing. It rejects occupied veneer/data space. It is excluded from iOS
runtime and `TOTK_EXPORT_IOS`. Other NS2 titles retain their original path.

`OOT_REALTIME=0` retains the unmodified configured-FPS behavior. Diagnostic
`framestep <ms>` pins a fixed duration; `framestep 0` restores the framework
duration. Pinning 16.666667 ms reproduces the old behavior while configured
FPS is 60. This pin is not a performance optimization.

Run the actual veneer instructions in a native arm64 executable:

```sh
python3 games/oot/tests/oot_timestep_native.py --work-dir /external/path/oot-step-test
```

This covers 16.7/33.3/50/100 ms, the 30/60 selector, zero-duration startup,
live pinning, accumulated time, integer/vector registers and NZCV. It needs
no game data. iOS syntax compilation verifies exclusion, not iOS gameplay.

On 2026-11-07, one Mac M4/16 GB run used the same desktop App and active
village save. Two approximately 10-second windows both rendered 287 frames:
old pin 28.80 FPS, real time 28.71 FPS. Accumulated native logic input was
4.80 seconds versus 10.05 seconds. Independent reads of the native VFR
object confirmed `+8` changed from 0.016667 seconds to about 0.034–0.037
seconds. The game loaded, turned the camera, captured a frame and closed
normally (exit 0); there were no new PSOs/libraries in the measured windows.
This verifies short-session timing, not full physics/cutscene/menu coverage
or a 60 FPS performance result. User confirmation of movement remains open.

External evidence: `MacNativeGames-artifacts/codex/oot-fps-regression-20261107/`
(`realtime-final1`, `realtime-final-build.log`, `realtime-ios-syntax.log`,
`native-veneer-test`, `realtime-app-build-record.json`). An earlier run's
windows completed, but its final invalid screenshot command caused an
intentional diagnostic failure; that run is not a successful session.
