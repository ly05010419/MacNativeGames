# Mac foreground playtime

The compact overlay displays total playtime in both full and FPS-only modes. The
counter runs after shader warm-up while the game window is active, including menus.
Switching apps, minimizing, locking the session or sleeping pauses it. Hiding the
HUD does not stop it. Long timer gaps are excluded rather than counted as play.

`playtime.json` lives inside `TOTK_PROFILE_DIR` (or `user-data`). It starts at zero;
there is no Screen Time access or automatic historical import. A user can seed the
counter while the game is closed using `{"schema":1,"seconds":7200}`. Extra metadata
is preserved. Existing invalid files are left untouched and the HUD reports that
playtime is unavailable. Writes are atomic every 15 seconds and on focus loss/exit.
An abrupt process termination can lose the time since the last checkpoint.

Run `make -C games/totk probe-playtime` for foreground/pause/sleep, checkpoint/reload,
metadata preservation, invalid-file and separate-profile checks. No game data needed.

Validation before the rebase: TOTK 1.0.0, M5 Pro, macOS 27.0.1, title menu. Full/compact/hidden HUD,
counting, save on exit and restored total on relaunch were checked. Other Mac game
versions were not played. This AppKit frontend is excluded from the iOS project.

Rebased onto main at bb1940f: integrates with TOTKHudBox and the settings panel,
retains the upstream FPS graph/build label/state-reporting hooks, and uses a
separate main-run-loop timer so hiding the HUD or selecting TOTK_HUD=corners does
not stop counting. The settings-panel fixture replaces the removed shortcut probe.
The counter storage and file format are unchanged. This rebase was reviewed as a
source diff only: no builds, tests or game launches were run, and no installed app,
profile, save or personal playtime record was read or modified.
