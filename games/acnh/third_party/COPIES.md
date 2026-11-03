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
