# Mac frame lock

The settings panel extends main's frame lock with 72, 90 and 120 FPS. One list
supplies the menu, saved-value validation and NVN setter. The existing atomic
lock and pacing remain: game/movie 30 FPS holds take priority; 72 FPS at 144 Hz
uses two display ticks; non-divisible rates use the existing clock.

The existing `graphics-frame.txt` is the only frame-mode preference:
`lock-<fps>`, `interp-<mode>` or `off`. Selecting a lock disables interpolation;
selecting interpolation clears the lock, and the shared file stores that choice.
An explicit `TOTK_MFX`, including `0`, prevents restoring a saved lock and greys
out the frame-lock row. No separate FPS preferences or migration are added.
No frame-lock keyboard shortcut is assigned while HDR #582 is being discussed.

## Focused validation (2026-10-28)

Run from the repository root:

```sh
PYTHONDONTWRITEBYTECODE=1 python3 tests/test_frame_lock.py -v
```

The settings check compiles the production frontend/panel for the base game and
1.4.3. Its `frame` action returns before creating an NSApplication or window.
It exercises the actual row callbacks and persistence functions with temporary
file-I/O fixtures: all seven choices, interpolation exclusion, restoring a
choice, explicit environment overrides, malformed/deleted files, no settings
directory, and key 9 remaining inert. NVN and controller state are fixtures.
The pacing check instead calls production NVN helpers: 140 combinations of
seven choices, five interpolation modes and 60/100/120/144 Hz, including game
hold/release, 72 FPS display ticks and invalid setter input. Both checks pass.

No installed app, player profile, GPU workload, full runtime build or gameplay
was used for this revision. Temporary test executables and settings are removed
automatically. The full panel/window suite was not rerun. Earlier gameplay
measurements apply to the older implementation and are recorded in the
[previous review reply](https://github.com/ly05010419/MacNativeGames/pull/640#issuecomment-6407280254);
they are not a gameplay check of this revision. Maintainer verification on 1.4.3
is still pending.
