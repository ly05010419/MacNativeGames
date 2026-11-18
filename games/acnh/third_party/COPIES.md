# Copies from other games in this repository

Animal Crossing: New Horizons carries its own copy of what it builds and runs with, so a change
to another game never changes this one.

| Component | Copied from | Description |
|---|---|---|
| `third_party/d2r/` (MIT) | `games/totk/third_party/d2r` | Ahead-of-time ARM64 instruction rewriter (`patch_nso.py`, `structck.py`, `equiv.py`) |
| `third_party/d2r_gfx/` (MIT/BSD) | `games/totk/third_party/d2r_gfx` | NVN-on-Metal graphics layer, texture decoding (`bcdec`), frame pace, pipeline cache |
| `tools/extract_shared_fonts.py` | `games/totk/scripts/extract_shared_fonts.py` | Bounded system font extractor from firmware data NCAs |
| `tools/extract_exefs.py`, `extract_romfs.py` | `games/by3/scripts/` | Container and filesystem extraction utilities |
| `runtime/keyboard_map.h`, `settings_panel.h`, `bindings_diagram.h`, `gamepad_layout.h`; the settings and controller-map parts of `controller_map.h`, `gamepad.m`, `hid_feedback.c`, `frontend.m` | `games/totk/runtime/` at `032a05d` (the panel of PR #1210) | Keyboard and mouse layout, settings panel and key-binding drawings, adapted to this game's actions; no shader contribution or star buttons |
| `third_party/d2r_gfx/gfx/nvn/fps_overlay.c`, `totk_mfx.m` (HUD mode, `mfx_requested`) | `games/totk/third_party/d2r_gfx` at `032a05d` | What the panel's Display page sets |
| `tools/prepare_shader_seed.py` | `games/totk/scripts/prepare_shader_seed.py` at `032a05d` | Container parsing and the seed manifest, adapted to this game's data and request format |
| `tools/build_hactool.py` | Root `build_app.py` at `1dcf40d` | Pinned hactool build, private ticket file, bounded base NCA and checked BKTR padding support; standalone copy |

Since 2026-11-17 some of these are no longer copies: `third_party/d2r_gfx/gfx/nvn/fps_overlay.{c,h}`,
`third_party/d2r_gfx/gfx/nvn/include/totk_ui_lang.h` (which now follows the Mac's first preferred language, not the app
bundle's locale) and `runtime/settings_panel.h` are symlinks into the repository's `shared/` folder (shared/README.md), the
same files the Zelda games and Super Mario Odyssey use; the corner box and FPS graph that `runtime/frontend.m` had are the
shared `runtime/hud_box.h`. This game's own settings rows and restore code are in `runtime/settings_game.h`. Also linked from
`shared/` since then, for the panel's other pages: `runtime/screenshot.h`, `board_upload.h`, `board_tab.h`, `nick_row.h`,
`shot_upload.h`, `shot_tab.h`, `dsu_client.h`, `dsu_protocol.h`, `dsu_settings_rows.h`, and
`third_party/d2r_gfx/gfx/nvn/fps_board.{c,h}` and `nvn_capture_region.h`. `runtime/account.c` stays this game's own
(one user: Animal Crossing keeps one island per console).

| Component | Copied from | Description |
|---|---|---|
| `runtime/playtime.h` | `games/totk/runtime/playtime.h` at `b704821` | The corner box's session and total playtime (`profile/playtime.json`) |
| `third_party/d2r_gfx/gfx/nvn/totk_contrib_upload.h` | the identity part of `games/odyssey/third_party/d2r_gfx/gfx/nvn/totk_contrib_upload.h` at `b704821` | `GAME_INBOX_ID` (`acnh`), the build, device and system names and the board id's file for the 性能排行榜 and 截图分享 pages; no shader upload |
| The screenshot action and hold (`runtime/keyboard_map.h`, `controller_map.h`, `gamepad.m`, `input.c`, `bindings_diagram.h`), DSU in `gamepad.m` and `gamepad_feedback.m`, `runtime_feedback_*` in `hid_feedback.c`, the game-frame capture (`mtl_grab_next_game_frame`), the board's frame record and view (`nvn_mtl.m` `mtl_board_view`, `hle_nvn.c` `nvn_board_pacing`) and the long-frame lines (`hitch_overlay_note`, `hitch_overlay_publish`) | `games/odyssey` at `b704821` (from games/totk) | Adapted to this game: no mouse lines in `controls.txt`, the capture button through GameController's Share button and, while motion is read from a Switch controller's HID reports, its capture button; the board counts new game frames (what the FPS box counts); no frame lock or interpolation on the board (the 30 / 60 / 120 mode is the game's own clock for the run) |

Since 2026-11-18 these are symlinks into `shared/` as well (shared/README.md): `third_party/d2r_gfx/gfx/nvn/totk_mfx_up.{m,h}`,
`totk_sysload.{c,h}`, and `runtime/playtime.h`, `gamepad_layout.h`, `feedback_settings.h` (new here), `hid_feedback.c`,
`motion_fusion.c`, `swkbd_host.c`, `ipc.{c,h}`, `bindings_diagram.h`. Their copies above were older versions of the
same code. The rumble switch is the shared `runtime_feedback_*` now (`runtime_rumble_locked`, `_choice` and `_choose`
are gone). This game's key bindings words, kinds and legend (the tools on the d-pad, the NookPhone; the mouse
moves nothing) are in `runtime/bindings_game.h`. Still this game's own: `runtime/time_host.c` (the `nn::time` answers its
main imports, such as the adjustable user clock, the steady clock's time point, `IsValidDate` and the calendar
comparisons: in the shared file they would also replace SDK code other games run), `controller_map.h` (no mouse lines:
the mouse belongs to the keyboard layout), `keyboard_map.h`, `shared_font.c`, `game_slot.c` and `third_party/d2r`.

## Optional local ACNH 3.0.1 speed-correction inputs

`tools/prepare_fps.py` and `runtime/frame_rate.c` are original host-side adapters.
They can consume the owner's local tmouh/acnh-60fps data at commit
`e5e03e64739334c9fbd36f19658b98e805f8ba8d`:
<https://github.com/tmouh/acnh-60fps> (CC BY-NC 4.0).
No source code, game bytes, patch tables, parameter files or mod tools from that
project are vendored here. Its pinned patch/parameter inputs remain ignored local
assets, and its tools are not run by this build. A locally built app with the
optional inputs carries their original LICENSE and a source/adaptation NOTICE
under `game/fps60/` and `Resources/Licenses/`. Default builds use the original
30 FPS timing and do not require these assets.


## The app launcher and the build plumbing (2026-11-18)

`scripts/acnh_app_launcher.c` keeps this game's own launcher code (the data folder per version (`ACNH_DATA_FOLDER`), the GPU and presentation settings, the frame-rate and add-on files); the plumbing every port's launcher shares is
`scripts/app_launcher.h`, a symlink to the repository's `shared/app/app_launcher.h` (shared/README.md). `build.py` and
`scripts/package_native_app.py` keep this game's steps, checks and contents and import the shared Python from `tools/port_app/`
(`adapter.py`: MNG Launcher's arguments, the work folder's venv, the express update's entry; `bundle.py`: clones,
Info.plist, the signature, staging beside the app, Launch Services). These are reached in the repository, not copied
from another game.
