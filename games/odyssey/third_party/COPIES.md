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


## The app launcher and the build plumbing (2026-11-18)

`scripts/odyssey_app_launcher.c` keeps this game's own launcher code (the session limit read again while the game runs, display.txt, face-buttons.txt, no-metalfx, the slot folder, the crash log); the plumbing every port's launcher shares is
`scripts/app_launcher.h`, a symlink to the repository's `shared/app/app_launcher.h` (shared/README.md). `build.py` and
`scripts/package_native_app.py` keep this game's steps, checks and contents and import the shared Python from `tools/port_app/`
(`adapter.py`: MNG Launcher's arguments, the work folder's venv, the express update's entry; `bundle.py`: clones,
Info.plist, the signature, staging beside the app, Launch Services). These are reached in the repository, not copied
from another game.
