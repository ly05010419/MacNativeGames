# Source copies

This game is isolated as the root CONTRIBUTING.md requires: its build and scripts never read, import, run or copy tools from another game's folder at build time. Everything it builds and runs with is a copy inside `games/ot0`.

## Imported source

Base: `games/by3` (Bayonetta 3) of this repository at commit **33e8fa83052ea1ae09a3583137008e9797e12950** (2026-10-25), MIT, copied whole on 2026-10-25. `games/by3` last changed at `a0d8611` before that commit, so the copy is the same as `games/by3` at `a0d8611`. `games/by3` is itself a fork of `games/totk`; its own `prebuilt/macos-arm64/README.md` says where the shader translator came from.

| Destination in this game | Copied from (at that commit) | Changes for Octopath Traveler 0 |
|---|---|---|
| `runtime/` | `games/by3/runtime/` | 16 of 55 files: title, version, names; the address space above 2^39 (`GUEST_ALIAS_HIGH`), time zone answers from the Mac, file system and save sizes from 1.0.4's `control.nacp`, `bsd:s`, `aoc`, `oe`, `vi` frame calls, offline `ldn` / `nsd` / `ssl`, six-axis calls with motion off, three `AudioOut` streams, text entry in the Mac text bar; see `CHANGELOG.md` |
| `third_party/d2r/` | `games/by3/third_party/d2r/` | `patch_nso.py`, `equiv.py`, `structck.py`: function regions also start at every `bl` target and relocated code pointer, jump tables whose base is set before a loop, and a patch area placed before the module so a `b` reaches it; see `CHANGELOG.md` |
| `third_party/d2r_gfx/` | `games/by3/third_party/d2r_gfx/` | 7 of 85 files, all in `gfx/nvn/`: driver constants read per component, base instance, bindless texture-buffer arrays, textures filled by GPU copies, the image handle table, unaligned buffer textures, shaders built in the background; see `CHANGELOG.md` |
| `prebuilt/macos-arm64/` | `games/by3/prebuilt/macos-arm64/` | `totk_xlate` rebuilt from `tools/totk_xlate` below, checked against `totk_xlate.sha256`; `README.md` unchanged |
| `tools/totk_xlate/` | `games/totk/tools/totk_xlate/` (Bayonetta 3 has no copy of the translator's source) | Patches `0006-bindless-handle-through-phi` and `0007-nan-aware-float-min-max-clamp` added; `Program.cs` reads image handles, `Service.cs` decodes texture slots from bound-texture names, `publish_native.sh` writes to this folder's `prebuilt/` |
| `scripts/` | `games/by3/scripts/` | `extract_code.py`, `extract_exefs.py` (a trimmed card image), `merge_romfs.py` (the base or update NCA by section type), `make_mod_manifest.py`, `prepare_patches.py`, `prepare_runtime.py`, `symbolize.py`; Bayonetta-only `make_res_patch.py` removed |
| `app/launcher.m` | `games/by3/app/launcher.m` | This game's own settings window: frame rate up to 120, monitor, window or full screen, size, output resolution, MetalFX, HUD; runs the game from inside the app and reads mods from the data folder |
| `Makefile`, `requirements.txt`, `.gitignore` | the same files in `games/by3` | Names, paths, game versions and a self-contained app target; `requirements.txt` adds `cryptography` and `unicorn` and drops `nsz` |

### Pieces from `games/ssbu`

Super Smash Bros. Ultimate had already met some of the same problems. These were brought over from `games/ssbu` at the same commit and adapted:

- The wide jump-table search in the rewriter (`third_party/d2r/patch_nso.py`), with one more condition: no other instruction in the region may write the base register.
- `GUEST_ALIAS_HIGH`: the alias region above 2^39 for a game that reserves 62 GiB (`runtime/game_version.h`, `runtime/services.c`).
- The file system answers `SetAllocator` and `GetDirectoryEntryCount` (`runtime/fs_host.c`).
- Thread stacks in a module's writable data (`runtime/`).

`assets/expected-v1.0.3.json`, `assets/expected-v1.0.4.json`, `scripts/versions.py`, `README.md` and `CHANGELOG.md` were written for this game.

## Updating

Copy newer code from another game explicitly into this folder, record the source commit here, and rebuild and play the game again (see `README.md`). Never make the Makefile or a script read another game's folder.
