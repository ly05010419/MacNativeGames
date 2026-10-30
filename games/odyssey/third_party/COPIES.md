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
