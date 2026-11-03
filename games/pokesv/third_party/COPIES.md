# Source copies

This game is isolated as the root CONTRIBUTING.md requires. Its build and scripts never read, import, run or copy tools from another game's folder at build time. Everything it builds and runs with is a copy inside `games/pokesv`.

## Imported source

**Base:** `games/xc3` (Xenoblade Chronicles 3) of this repository at commit **f1d00772df1ac3230268f3124bdeb6b7cc81a61c** (2026-10-29), MIT, copied whole on 2026-10-29 except its `README.md`, `CHANGELOG.md`, `assets/expected-*.json` and `docs/`.
- `games/xc3` is a copy of `games/ot0`, itself a copy of `games/by3` and `games/totk`. Its own `third_party/COPIES.md` lists what each step brought over. Comments that named an earlier game now say "the earlier port"; this file is the one place that names the sources.

In every file:
- names and identifiers were renamed: `XC3_*` → `PSV_*` (environment variables, the work folder `PSV_build`, `-DPSV_GAME_V*`), `xc3-native` → `psv-native`, the `[xc3]` log prefix → `[psv]`,
- the data folder `MacNativeGames/XC3` → `MacNativeGames/PSV`, the bundle id → `org.macnativegames.pokesv`, the app and window title, and the title IDs (`01008F6008C5E000`, update `01008F6008C5E800`).

| Destination in this game | Copied from (at that commit) | Changes for Pokémon Violet |
|---|---|---|
| `runtime/` | `games/xc3/runtime/` | `game_version.h`: this title, 4.0.0 only, no per-version addresses. `loader.c`: a relative relocation may point up to 1 MiB past its module. `romfs.c`: a folder path may end in `/` (entry type), `PSV_FS_LOG=reads`, reads that hit EFAULT go through host memory. `fs_host.c`: the same for save reads. `audio_out.c`: `PSV_AUDIOOUT_LEVEL=1`. `input.c`: Npad style event, assignment, colour, LED and strap-guide calls. `shared_font.c`: `RequestSharedFontLoad`, payload decoded to TrueType (below). `system_services.c`: BCAT (below), `GenerateCryptographicallyRandomBytes`, Bluetooth LE without an accessory. `frontend.m`: window title. |
| `third_party/d2r/` | `games/xc3/third_party/d2r/` | none |
| `third_party/d2r_gfx/` | `games/xc3/third_party/d2r_gfx/` | names (`PSV_*` diagnostic variables). `hle_nvn.c`, `nvn_obj.h`: queue priority, builder and device getters. `psv_nvn_ext.c`: multisample coverage state. `nvn_mtl.m`: shader functions with function constants specialised (`shader_fn`); vertex shaders that write the layer themselves (topology class, point size kept in a local); `c0+0x640` documented as 0 on purpose; scaled colour and depth copies (`copy_t2t_scaled`); passes into whole 3D textures cover every depth plane; zero-copy pool buffers over tracked pages spanning two pools. `nvn_dirty.c`: `nvn_dirty_tracked_span`. |
| `prebuilt/macos-arm64/` | `games/xc3/prebuilt/macos-arm64/` | the same binary as `totk_xlate` there, renamed `psv_xlate` (checked against `psv_xlate.sha256`, same hash) |
| `tools/psv_xlate/` | `games/xc3/tools/totk_xlate/` | none: byte-identical, so the prebuilt binary matches its source (the earlier port's names kept); `publish_native.sh` writes `prebuilt/macos-arm64/psv_xlate` |
| `scripts/` | `games/xc3/scripts/` | names, this title's ID. `versions.py`: 4.0.0 (base plus update). |
| `app/launcher.m` | `games/xc3/app/launcher.m` | names |
| `Makefile`, `requirements.txt`, `.gitignore` | the same files in `games/xc3` | names, paths and the game version (`v400`) |
| `tests/test_psv_versions.py` | `games/xc3/tests/test_xc3_versions.py` | 4.0.0 as an update of the base game |
| `third_party/licenses/` | `games/xc3/third_party/licenses/` | none |

## Later copies from other games

| Destination | Copied from (commit f1d0077) | What |
|---|---|---|
| `runtime/shared_font.c` (`load`) | `games/eow/runtime/shared_font.c` | the BFTTF payload decoded to plain TrueType after the hash check (key = word0 ^ 0x7f9a0218) |
| `runtime/system_services.c` (BCAT) | `games/acnh/runtime/system_services.c` | the offline delivery cache: no storage mounts, `fail_res`, `fail_count`, `empty_delivery_directories` |
| `third_party/d2r_gfx/gfx/nvn/include/psv_fps_limit.h` | `games/totk/third_party/d2r_gfx/gfx/nvn/include/totk_fps_limit.h` | the frame lock choices, unchanged |
| `third_party/d2r_gfx/gfx/nvn/hle_nvn.c` (frame lock) | `games/totk/third_party/d2r_gfx/gfx/nvn/hle_nvn.c` | `nvn_fps_lock_set`, `game_hold_fps` and the held rate in `present_ticks` / `present_period` |
| `runtime/settings_panel.h` | `games/wonder/runtime/settings_panel.h` | rows and look kept (`PSVSettingRow`, the panel view). Trimmed to frame lock, MetalFX output scaling and overlay, saved in `settings.txt`. No controller navigation, no contribution buttons |
| `third_party/d2r_gfx/gfx/nvn/nvn_mtl.m` type views | `games/odyssey` commit 546123b | unchanged: a type view no draw asked for in 64 submissions leaves the cache, the `[type-views]` line and the churn probe |
| `runtime/game_patches.c` frame step | `games/totk/runtime/game_patches_v143.c` commit 3e9e7f6 | the measured step of `runtime_frame_step_update` (an average over presents, a hitch at most 1/15 s) and `runtime_frame_step_pin`. Here it fills a host copy of the game's frame-duration table instead of TOTK's frame-step store, averaged over 10 presents instead of 5 |

`README.md`, `CHANGELOG.md` and `assets/expected-v4.0.0.json` were written for this game.

## Renamed on 31 October 2026

Every name from the sources now carries this game's prefix, with no change in behaviour: `TOTK_*` environment variables are `PSV_*`, log tags `[totk…]` are `[psv…]`, `totk_*` / `Totk*` / `TOTK*` identifiers and files are `psv_*` / `Psv*` / `PSV*`, `tools/totk_xlate` is `tools/psv_xlate` (its contents keep the old names, see the table), the RomFS index magic is `PSVRFS01`. The dead `GAME_BOTW` branches are removed (`unifdef -UGAME_BOTW`). **One name is kept on purpose:** the shader cache identity `DMSH_CHAIN` (`totk-native-v4-unverified`, `dmsh_chain.h`) is stored in every cached shader; a new text would retranslate all of them.

## Updating

Copy newer code from another game explicitly into this folder, record the source commit here, and rebuild and play the game again (see `README.md`). Never make the Makefile or a script read another game's folder.
