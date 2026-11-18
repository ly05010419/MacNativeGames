# Copies from other games in this repository

Every game other than Tears of the Kingdom / Breath of the Wild carries its own copy of what it builds with, so a change
to another game never changes this one. These were copied on 2026-10-23 from the commit the port was tested on
(daf270e, 2026-10-22); update them here, by hand, when Odyssey needs a newer version.

| Folder | Copied from | Used by |
|---|---|---|
| `d2r/` (MIT, see its LICENSE) | `games/by3/third_party/d2r` | `scripts/build_odyssey.py` (patch_nso.py, the ahead-of-time ARM64 rewrite) |
| `totk-scripts/` | `games/totk/scripts`: extract_exefs, extract_romfs, extract_shared_fonts, prepare_shader_seed, shader_list | `build_odyssey.py`, `extract_code.py`, `inspect_input.py`, `prepare_odyssey_shader_seed.py`, `locate_runtime_shaders.py`, `probe_shader_translation.py` |
| `by3-scripts/` | `games/by3/scripts`: merge_romfs (with its extract_exefs / extract_romfs), prepare_modules | `merge_romfs.py`, `inspect_version.py`, `survey_modules.py` |
| `../prebuilt/macos-arm64/` | `games/totk/prebuilt/macos-arm64` (totk_xlate and its SHA-256; see its README) | `Makefile` (`shader-service/totk_xlate`) |

`prepare_odyssey_shader_seed.py` points the copied seed tool at `games/odyssey/assets/shader_list.tsv` (none yet), not at
the Tears of the Kingdom list.

## Added on 2026-10-26 (keys 3 to 7, update 1.4.1)

Compared against `a8807a1` (2026-10-26). These parts were first played in a Super Mario Odyssey port outside this
repository that `games/ssbu` was copied from as well, so the closest copy in the repository is `games/ssbu`'s.

| File | Same as | Changed here |
|---|---|---|
| `d2r_gfx/gfx/nvn/totk_taa.m`, `totk_taa.h` | `games/ssbu/third_party/d2r_gfx/gfx/nvn` (a copy of `games/totk`'s) | the camera cut is `odyssey_mfx_cut` (centimetres), the camera comes from `odyssey_mfx.h` |
| `../runtime/aspect_math.h`, `../runtime/mouse_look.h` | `games/ssbu/runtime` | nothing |
| `../runtime/gamepad_layout.h`, `../runtime/motion_fusion.c` (keys 6 and 7) | `games/ssbu/runtime` | nothing |
| `../runtime/gamepad.m` (keys 6 and 7) | `games/ssbu/runtime/gamepad.m` | Odyssey's stick reach (`controller_reach_parse`) kept, on the HID path too |
| `../scripts/make_render_res_overlay.py` | `games/ssbu/scripts` | Odyssey's work folder layout (`extracted/local`) |
| `../tests/gamepad_layout/`, `gamepad_type/`, `mouse/`, `mouse_look/` | `games/ssbu/tests` | nothing |
| `by3-scripts/merge_romfs.py` | (above) | the layout of 1.4.1's AES-CTR-Ex table |

`../runtime/odyssey_aspect.c` is Odyssey's own (key 4); it reads the camera projections the way `games/totk`'s
`game_patches_botw.c` describes them, without using that file.

## Added on 2026-11-01 (settings)

The frontend, panel, binding diagrams, keyboard mapping, playtime and controller input/feedback follow games/totk
at 4227378, including d3389b0 and the prior graph, persistence and controller revisions. FPS/sysload follow 0fd9358
(95422a7, d8b16b1 and 95e04c0). Odyssey's stick calibration, keyboard defaults and input ABI are retained. Zelda-specific
bow/Ultrahand key transforms and ability-wheel mouse pointing are omitted. Output-resolution scaling uses the standalone
local BOTW helper, copied here; the build/runtime never reads another game's folder.

Since 2026-11-17 some of these are no longer copies: `d2r_gfx/gfx/nvn/fps_overlay.{c,h}`, `d2r_gfx/gfx/nvn/include/totk_ui_lang.h`,
`../runtime/hud_box.h` and `../runtime/settings_panel.h` are symlinks into the repository's `shared/` folder (shared/README.md),
the same files the Zelda games use. Odyssey's own settings rows are in `../runtime/settings_game.h`.
Also linked from `shared/` since then, for the settings panel's other tabs: `../runtime/screenshot.h`,
`board_upload.h`, `board_tab.h`, `nick_row.h`, `shot_upload.h`, `shot_tab.h`, `users.h`, `account.c` (it replaces
Odyssey's single-user copy; Odyssey's own account entries stay behind `GAME_ODYSSEY` there), `dsu_client.h`,
`dsu_protocol.h`, `dsu_settings_rows.h`, and `d2r_gfx/gfx/nvn/fps_board.{c,h}` and `nvn_capture_region.h`. Adapted
from games/totk in the same change, in Odyssey's own files: the screenshot action and hold (`keyboard_map.h`,
`controller_map.h`, `gamepad.m`, `input.c`, `bindings_diagram.h`), DSU in `gamepad.m` and `gamepad_feedback.m`,
the game-frame capture (`mtl_grab_next_game_frame`), the board's frame record and view (`nvn_mtl.m`
`mtl_board_view`, `hle_nvn.c` `nvn_board_pacing`) and `totk_leaderboard_id_file` in `totk_contrib_upload.h`.


## Small runtime and graphics files linked from `shared/` (2026-11-18)

These were older copies of games/totk's (or, for `ipc.{c,h}`, the newer one) and are now symlinks into the repository's
`shared/` folder (shared/README.md): `d2r_gfx/gfx/nvn/totk_mfx_up.{m,h}`, `totk_sysload.{c,h}`, and
`../runtime/playtime.h`, `gamepad_layout.h`, `feedback_settings.h`, `hid_feedback.c`, `motion_fusion.c`,
`swkbd_host.c`, `ipc.{c,h}`, `time_host.c`, `bindings_diagram.h`. Odyssey's own parts stay its own: Cappy (the second
player) gets no host rumble or motion in the shared `hid_feedback.c` behind `GAME_ODYSSEY`; this game's CMIF domains
are the shared `ipc.c`'s; the motion handoff fix (`buffered`) is in the shared `motion_fusion.c`; the key bindings
drawings' words and kinds are in `../runtime/bindings_game.h`.

Still this game's own: `../runtime/controller_map.h` (the worn-stick `reach` lines, applied by `gamepad.m`),
`keyboard_map.h`, `shared_font.c` (rows from the generated `shared_fonts_config.h`), `game_slot.c` (its own slot folder)
and `third_party/d2r`.


## The app launcher and the build plumbing (2026-11-18)

`scripts/odyssey_app_launcher.c` keeps this game's own launcher code (the session limit read again while the game runs, display.txt, face-buttons.txt, no-metalfx, the slot folder, the crash log); the plumbing every port's launcher shares is
`scripts/app_launcher.h`, a symlink to the repository's `shared/app/app_launcher.h` (shared/README.md). `build.py` and
`scripts/package_native_app.py` keep this game's steps, checks and contents and import the shared Python from `tools/port_app/`
(`adapter.py`: MNG Launcher's arguments, the work folder's venv, the express update's entry; `bundle.py`: clones,
Info.plist, the signature, staging beside the app, Launch Services). These are reached in the repository, not copied
from another game.


## The input stack linked from `shared/` (2026-11-18)

`../runtime/gamepad_feedback.m`, `hid_imu.h`, `mouse_look.h`, `switch_left_fallback.h` and the new `gamepad_core.h`,
`gamepad_settings.h`, `gamepad_hid_pad.h`, `gamepad_mouse.h` and `input_file.h` are symlinks into the repository's
`shared/` folder (shared/README.md). Except for this game's fixes below, its copies were older versions of games/totk's.
This game's own parts stay its own: `../runtime/gamepad.m` keeps the two seats (Mario and Cappy), the layout per seat and
the routing of rumble and motion to Mario's controller, on top of `gamepad_core.h`; `../runtime/gamepad_game.h` has mouse
mode and the worn-stick `reach` lines (also applied to the generic HID pads, as before); `../runtime/feedback_game.h`
keeps rumble and motion with the first capable controller (`FEEDBACK_KEEP_FIRST`) and the trigger stubs;
`../runtime/input.c` keeps its key table, two players and `gpucapture`; `../runtime/controller_map.h` keeps its `reach`
lines. Odyssey's fixes now in the shared files for every game: a queued motion sample of a controller that no longer
feeds the fusion is dropped, a Switch HID sensor that does not open leaves motion free for the next controller, rumble
and motion may belong to two controllers without sharing one HID device, reports of a closed device are ignored, a
controller that leaves takes its last pulse and its HID stick fallback with it, and its sensors are switched off.


## The platform layer's identical files linked from `shared/` (2026-11-18)

These were byte-identical to games/totk's (and, for `freeze_watchdog.c`, to Animal Crossing's) and are now symlinks into the repository's `shared/` folder (shared/README.md): `../runtime/gate.S`, `thread_enter.S`, `thread_compat.h`, `hle_stack.c`, `memory_probe.c`, `audio_effects.{c,h}`, `audio_reverb.{c,h}`, `romfs_path.h`, `text_keyboard.h`, `freeze_watchdog.c` and `audio_ramp.h` (which differed only in its first comment line, "ported from BOTW e38682d"; that note is in the shared file; this game's `audio_mixer.c` still ramps every voice, as before). Nothing compiled changes. The rest of this game's platform layer (`threads.c`, `fs_host.{c,h}`, `romfs.c`, the audio mixer, renderer and output, `opus_codec.c`, `loader.c`, `services.c`, `system_services.c`, `applet_host.c`, `movie_host.{c,h}`, `autoload.{c,h}`, `world_survey.{c,h}`, `hle_stack.S` and the probes) stays its own: it carries this game's scheduling, saves, timing and answers (shared/README.md).
