# Source copies

This game is isolated as the root CONTRIBUTING.md requires: its build and scripts never read, import, run or copy tools
from another game's folder at build time. Everything it builds and runs with is a copy inside `games/eow`.

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
| `app/` | not copied | No launcher yet |

`assets/expected-v1.1.0.json` and `CHANGELOG.md` were written for this game.

## Updating

Copy newer code from another game explicitly into this folder, record the source commit here, and rebuild and play the
game again (see `README.md`). Never make the Makefile or a script read another game's folder.
