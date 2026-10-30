# Source copies

This game is isolated as the root CONTRIBUTING.md requires. Its build and scripts never read, import, run or copy tools from another game's folder at build time. Everything it builds and runs with is a copy inside `games/xc3`.

## Imported source

**Base:** `games/ot0` (Octopath Traveler 0) of this repository at commit **5ab07b7201f88251c6c2e59aa10be016a3d60f81** (2026-10-28), MIT, copied whole on 2026-10-28 except its `README.md` and `CHANGELOG.md`.
- `games/ot0` last changed at `1b3ef2f` before that commit, so the copy is the same as `games/ot0` at `1b3ef2f`.
- `games/ot0` is a copy of `games/by3`, which is a copy of `games/totk`. What ot0 itself brought over from by3 and ssbu is listed in its own `third_party/COPIES.md`.

In every file:
- names and identifiers were renamed: `OT0_*` → `XC3_*` (environment variables, the work folder `XC3_build`, `-DXC3_GAME_V*`), `ot0-native` → `xc3-native`, the `[ot0]` log prefix → `[xc3]`,
- `games/ot0` → `games/xc3`, the data folder `MacNativeGames/OT0` → `MacNativeGames/XC3`,
- the bundle id → `org.macnativegames.xc3`, the title and its title ID.

| Destination in this game | Copied from (at that commit) | Changes for Xenoblade Chronicles 3 |
|---|---|---|
| `runtime/` | `games/ot0/runtime/` | `game_version.h`: this title, one version (1.0.0) and no per-version addresses yet. `game_patches.c`: the Unreal frame-rate patch (`UGameEngine::GetMaxTickRate`) removed. `system_services.c`: the frame-rate default removed; the inherited graphics settings are marked to be checked again for this game. `fs_host.c`: both `IsSaveDataExisting(Uid)` overloads. `input.c`: `GetTouchScreenStates<16>`. `applet_host.c`: album notifications and the inactivity read-backs. `account.c`: `ListAllUsers`. `applet_host.c`: `prepo::AddSessionId`. `services.c`: the language is one this version supports (control.nacp, through `control_config.h`). `fs_host.c`: save sizes from control.nacp. `system_services.c`: `TOTK_GPU_MIDCLEAR=1` after its probe passed on this host. `romfs.c`: `TOTK_FS_LOG` also logs failed opens. |
| `third_party/d2r/` | `games/ot0/third_party/d2r/` | none yet |
| `third_party/d2r_gfx/` | `games/ot0/third_party/d2r_gfx/` | names (the diagnostic environment variables are `XC3_*`); `nvn_tex.h`, `hle_nvn.c`, `totk_nvn_ext.c`, `nvn_mtl.m`: the depth-stencil mode set on a texture builder samples the stencil plane, like a view's; integer clears (`ClearColori`/`ClearColorui`); transform feedback emulated (`docs/reference/nvn-transform-feedback.md`: records `OP_TF_BIND`/`OP_TF_CTL`, reflection kind `NVN_RB_TFE`, `PSO_NORASTER`, `vs_fn_void`); geometry shaders emulated as compute for point input (`vtg_draw`, reflection kinds `NVN_RB_VBINFO`…`NVN_RB_VBTEX`, `totk_rt_vtg`), tessellation still skippable with `DM_NVN_GS_SKIP=1`; indexed quads expanded on the CPU; blending left off on integer render targets (`pf_integer`); images of block-compressed textures named by block format and written through a block shadow copied into the BC texture after the draw or dispatch (`bc_shadow`, `bc_flush`); control sections copied at their own size (longer than 0x880 in XC3); `TOTK_CTL_DUMP` covers every stage and the whole section |
| `prebuilt/macos-arm64/` | `games/ot0/prebuilt/macos-arm64/` | `totk_xlate` built from `tools/totk_xlate` below by the port's author (.NET 10.0.403 SDK), then rebuilt by the maintainers with the .NET 10.0.401 SDK and replay-checked (`prebuilt/macos-arm64/README.md`), checked against `totk_xlate.sha256` |
| `tools/totk_xlate/` | `games/ot0/tools/totk_xlate/` | `publish_native.sh` writes to this folder's `prebuilt/`; `Program.cs` / `Service.cs`: transform feedback emulation answered from the job's `tfb`; patches `0008-transform-feedback-buffers-in-set-0` and `0009-vertex-and-geometry-as-compute-resources-in-set-0`; `Program.cs` / `Service.cs`: request stages `vsc` / `gsc` / `vpt` (geometry shaders as compute) and the input topology from the job |
| `scripts/` | `games/ot0/scripts/` | names, this title's ID. `versions.py`: 1.0.0, base-only (`identify_base`). `extract_code.py`: code from the base NSP when there is no update. `merge_romfs.py`: the update is optional (base-only mode). `prepare_runtime.py`: modules from the manifest; `control_config.h` from the manifest's control data. `prepare_modules.py`: every NSO in the ExeFS. `symbolize.py`: `subsdk<n>`. |
| `app/launcher.m` | `games/ot0/app/launcher.m` | names; the frame-rate setting no longer sets the Unreal cap |
| `Makefile`, `requirements.txt`, `.gitignore` | the same files in `games/ot0` | names, paths and the game version |
| `third_party/licenses/` | `games/ot0/third_party/licenses/` | none |

`docs/`, `README.md` and `CHANGELOG.md` were written for this game.

## Updating

Copy newer code from another game explicitly into this folder, record the source commit here, and rebuild and play the game again (see `README.md`). Never make the Makefile or a script read another game's folder.
