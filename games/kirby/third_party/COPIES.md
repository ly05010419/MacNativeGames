# Copies from other games in this repository

Every game other than Tears of the Kingdom / Breath of the Wild carries its own copy of what it builds with, so a change
to another game never changes this one. Update the copies here, by hand, when Kirby needs a newer version.

| Copy | Copied from | Used by |
|---|---|---|
| `d2r/` (MIT, see its LICENSE) | `games/by3/third_party/d2r` (daf270e, 2026-10-22) | `scripts/build_kirby.py` (patch_nso.py, the ahead-of-time ARM64 rewrite), `scripts/prepare_runtime.py` |
| `../prebuilt/macos-arm64/` | `games/totk/prebuilt/macos-arm64` (daf270e; totk_xlate and its SHA-256, see its README) | `scripts/build_kirby.py`, `scripts/package_native_app.py` |
| `../scripts/extract_exefs.py`, `extract_romfs.py`, `extract_shared_fonts.py`, `extract_app_icon.py` | `games/mvdk/scripts` (6233eb3, 2026-10-25), unchanged | `scripts/build_kirby.py` |
| `../scripts/prepare_runtime.py`, `prepare_romfs_backend.py`, `prepare_shared_fonts.py` | `games/mvdk/scripts` (6233eb3), changed to read this folder's work-directory layout | `scripts/build_kirby.py` |
| `totk-scripts/` | `games/totk/scripts` (daf270e): extract_exefs, extract_romfs, extract_shared_fonts, prepare_shader_seed, shader_list | not used by the build (kept from the first version of the port) |
| `by3-scripts/` | `games/by3/scripts` (daf270e): merge_romfs (with its extract_exefs / extract_romfs), prepare_modules | not used by the build (kept from the first version of the port) |
