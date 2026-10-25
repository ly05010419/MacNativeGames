# Copies from other games in this repository

Animal Crossing: New Horizons carries its own copy of what it builds and runs with, so a change
to another game never changes this one.

| Component | Copied from | Description |
|---|---|---|
| `third_party/d2r/` (MIT) | `games/totk/third_party/d2r` | Ahead-of-time ARM64 instruction rewriter (`patch_nso.py`, `structck.py`, `equiv.py`) |
| `third_party/d2r_gfx/` (MIT/BSD) | `games/totk/third_party/d2r_gfx` | NVN-on-Metal graphics layer, texture decoding (`bcdec`), frame pace, pipeline cache |
| `tools/extract_shared_fonts.py` | `games/totk/scripts/extract_shared_fonts.py` | Bounded system font extractor from firmware data NCAs |
| `tools/extract_exefs.py`, `extract_romfs.py` | `games/by3/scripts/` | Container and filesystem extraction utilities |
