# Where each copy came from

Everything under `runtime/`, `third_party/d2r`, `third_party/d2r_gfx`, `prebuilt/`, `scripts/` and `app/` is this
folder's own copy; nothing here reads another game's folder. All copies were taken from
MacNativeGames `feat/nier-latest` commit `c6fee464` (by3 line, itself forked from the TotK port), except:

| copy | from | notes |
|---|---|---|
| `runtime/` | `games/by3/runtime` | renamed by3→pla, `game_version.h` rewritten for PLA 1.1.1 |
| `third_party/d2r/` | `games/by3/third_party/d2r` | the NSO rewriter + checkers, unchanged |
| `third_party/d2r_gfx/` | `games/by3/third_party/d2r_gfx` | the NVN→Metal layer, renamed by3→pla strings |
| `prebuilt/` | `games/by3/prebuilt` | `totk_xlate` shader translator + pinned SHA-256 |
| `scripts/extract_exefs.py` | `games/alc/scripts` (`feat/alc-1.0.1` `e569c0ac`) | the card-image variant: parameterized `--title/--update-title`, picks the update Program NCA by its BKTR marker (A1) |
| `scripts/extract_romfs.py` | `games/alc/scripts` | the XCI card-image variant |
| `scripts/merge_romfs.py` | `games/alc/scripts`, extended here | PLA variant: the update reuses untouched base hash-tree blocks (indirect storage-0 rows below the base level-5 offset read the card's base section; see CHANGELOG stage 0) |
| remaining `scripts/` | `games/by3/scripts` | renamed by3→pla, v120→v1101 |
| `app/launcher.m` | `games/by3/app` | renamed by3→pla |
| `requirements.txt` | `games/by3` | unchanged |
