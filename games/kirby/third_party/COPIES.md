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
