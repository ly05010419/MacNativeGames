# Source copies

This game is isolated as the root CONTRIBUTING.md requires: its build and scripts never read, import, run or copy tools
from another game's folder at build time. Everything it builds and runs with is a copy inside `games/eow`, except the
files every game uses, which it reaches through links into the repository's `shared/` folder (below).

## Imported source

Base: `games/by3` (Bayonetta 3) of this repository at commit **5281186af56bdcce6ae9b3bb9daa9791bc02994e**
(2026-10-24), MIT, copied whole on 2026-10-24. `games/by3` is itself a fork of `games/totk`; its own
`prebuilt/macos-arm64/README.md` says where the shader translator came from.

| Destination in this game | Copied from (`games/by3` at that commit) | Changes for Echoes of Wisdom |
|---|---|---|
| `runtime/` | `runtime/` | Title, version, names; new host answers (oe notification event and exit-request section, hid style-set event and Joy-Con assignment, vi layer crop / destroy / finalize, C stdio on RomFS for main in the new `stdio_host.c`, decoded system fonts, movie decoder mode 2 into the game's texture, audio volume ramps / depop / end of stream); see `CHANGELOG.md` |
| `third_party/d2r/` | `third_party/d2r/` | None |
| `third_party/d2r_gfx/` | `third_party/d2r_gfx/` | Lower-left window origin, sRGB present encode, 2D-engine conversion / scaling / mirroring and depth copies, `SetCopyImageStride`, cube-view level clamp, ZCull storage size and title warm-up switches, dirty-page tracking of destroyed and nested pools, slotless size-query texture binding, precompiled shader libraries for every runtime compile; see `CHANGELOG.md` |
| `prebuilt/macos-arm64/` | `prebuilt/macos-arm64/` | None (`totk_xlate`, checked against its SHA-256) |
| `scripts/` | `scripts/` | `merge_romfs.py` reads NCA compression and a CTR-Ex table that stops at the indirect table; new `build_shader_libs.py`, `disasm.py`, `xref.py`, `callers.py`, `lldb/`; Bayonetta-only `make_res_patch.py` removed |
| `Makefile`, `requirements.txt` | the same files | Names and paths; the app / install targets removed (no launcher yet) |
| `app/launcher.c` | `games/acnh/scripts/acnh_app_launcher.c` at `8d532cd3` (2026-11-17), this repository, MIT | Paths, environment and data folder of this game; the README's play settings; `TOTK_LOCALE` from the Mac's preferred languages |

`assets/expected-v1.1.0.json` and `CHANGELOG.md` were written for this game.

## Shared files and the settings panel (2026-11-17)

Since 2026-11-17 these are no longer copies but relative symlinks into the repository's `shared/` folder
(shared/README.md), the same files the Zelda games, Super Mario Odyssey and Animal Crossing use:
`third_party/d2r_gfx/gfx/nvn/fps_overlay.{c,h}` (the old Bayonetta 3 copy is gone),
`third_party/d2r_gfx/gfx/nvn/include/totk_ui_lang.h` (which now follows the Mac's first preferred language, not the app
bundle's locale), `third_party/d2r_gfx/gfx/nvn/fps_board.{c,h}`, `nvn_capture_region.h`, and `runtime/hud_box.h`,
`settings_panel.h`, `screenshot.h`, `board_upload.h`, `board_tab.h`, `nick_row.h`, `shot_upload.h`, `shot_tab.h`,
`dsu_client.h`, `dsu_protocol.h`, `dsu_settings_rows.h`. The shortcut box of keys 1-4 that `runtime/frontend.m` had is
replaced by the shared corner box and the settings panel; this game's rows, restore code and key binding actions are in
`runtime/settings_game.h`. `runtime/account.c` stays this game's own (one account: its saves stay where they are).

| Destination in this game | Copied from | Changes for Echoes of Wisdom |
|---|---|---|
| `runtime/controller_map.h`, `gamepad_layout.h`, `gamepad.m`, `gamepad_feedback.m`, `hid_feedback.c` | `games/acnh/runtime/` in the working tree on top of `b704821` (2026-11-17; the same files as `games/by3`'s, which this game had, plus the settings panel's controller code: the layout writer, the controller type, the panel's L3 + R3, the screenshot hold, the capture button, DSU, the rumble switch) | Comments only |
| `runtime/keyboard_map.h`, `bindings_diagram.h` | `games/acnh/runtime/` (same) | The default keyboard layout is the fixed one this game had (`input.c` `button()`, which scripted input keeps); the drawings name the Switch buttons instead of Animal Crossing's uses |
| `runtime/playtime.h` | `games/totk/runtime/playtime.h` at `b704821` | None (the corner box's session and total playtime, `profile/playtime.json` beside the save folder) |
| `third_party/d2r_gfx/gfx/nvn/totk_contrib_upload.h` | `games/acnh/third_party/d2r_gfx/gfx/nvn/totk_contrib_upload.h` (same; the identity part of `games/odyssey`'s) | `GAME_INBOX_ID` `eow`; no shader upload |
| The key bindings, mouse buttons, key 1 and the screenshot key in `runtime/input.c`; the panel's wiring in `runtime/frontend.m`; the game-frame capture (`mtl_grab_next_game_frame`), the board's frame record and view (`nvn_mtl.m` `mtl_board_view`, `hle_nvn.c` `nvn_board_pacing`), the long-frame lines (`hitch_overlay_note`, `hitch_overlay_publish`) and the full-information-only shader and load lines | `games/acnh` (same) | The captured game frame is turned over for this game's lower-left origin (as `dm_gfx_shot_at` does); the board counts the game's presents (what this FPS box counts); keys 2-4 removed (none did anything here) |


## Small runtime and graphics files linked from `shared/` (2026-11-18)

These were older copies of games/totk's (or, for `ipc.{c,h}`, the same as Odyssey's newer one) and are now symlinks
into the repository's `shared/` folder (shared/README.md): `third_party/d2r_gfx/gfx/nvn/totk_mfx_up.{m,h}`,
`totk_sysload.{c,h}`, and `runtime/playtime.h`, `gamepad_layout.h`, `feedback_settings.h` (new here), `hid_feedback.c`,
`motion_fusion.c`, `swkbd_host.c`, `ipc.{c,h}`, `time_host.c`, `bindings_diagram.h`. The rumble switch is the shared
`runtime_feedback_*` now (`runtime_rumble_locked`, `_choice` and `_choose` are gone). The key bindings drawings' words
for this game (the Switch buttons, not their uses; the mouse moves nothing; the right stick's directions) are in
`runtime/bindings_game.h`. Still this game's own: `runtime/controller_map.h` (no mouse lines),
`keyboard_map.h`, `shared_font.c`, `game_slot.c` and `third_party/d2r`.


## The app launcher and the build plumbing (2026-11-18)

`app/launcher.c` keeps this game's own launcher code (the README's play settings, the runtime started by its real path, the game data check); the plumbing every port's launcher shares is
`app/app_launcher.h`, a symlink to the repository's `shared/app/app_launcher.h` (shared/README.md). `build.py` and
`scripts/package_app.py` keep this game's steps, checks and contents and import the shared Python from `tools/port_app/`
(`adapter.py`: MNG Launcher's arguments, the work folder's venv, the express update's entry; `bundle.py`: clones,
Info.plist, the signature, staging beside the app, Launch Services). These are reached in the repository, not copied
from another game.

## Updating

Copy newer code from another game explicitly into this folder, record the source commit here, and rebuild and play the
game again (see `README.md`). Never make the Makefile or a script read another game's folder.


## The input stack linked from `shared/` (2026-11-18)

`runtime/gamepad.m`, `gamepad_feedback.m` and the new `gamepad_core.h`, `gamepad_settings.h`, `gamepad_hid_pad.h`,
`gamepad_mouse.h`, `hid_imu.h`, `switch_left_fallback.h` and `input_file.h` are symlinks into the repository's `shared/`
folder (shared/README.md). The copies were games/acnh's (above), older versions of games/totk's. This game's own:
`runtime/gamepad_game.h` (no mouse mode: the mouse belongs to the keyboard layout), `runtime/feedback_game.h` (nothing
beyond the shared rumble and motion), `runtime/controller_map.h` (no mouse lines), and in `runtime/input.c` the
`button()` table scripted input uses, the HID answers and the `gpucapture` and `panelshot` commands.
