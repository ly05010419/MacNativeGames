# Source copies and update policy

The game is isolated as the root [CONTRIBUTING.md](../../../CONTRIBUTING.md) requires: its build never reads, imports or runs another game's folder. Everything it builds and runs with is copied here, and this page says where each copy came from.

## Imported source

Base: [seedds/nxnative](https://github.com/seedds/nxnative/tree/062cd06e6fd79ec0bcd52d59f778fd6438994a2a), commit **062cd06e6fd79ec0bcd52d59f778fd6438994a2a** (2026-10-28), MIT. Imported on 2026-10-28. The original license is [nxnative/LICENSE](nxnative/LICENSE). The original authors keep their copyrights; this contribution does not claim authorship of the imported host, graphics layer, rewriter, translator or game profile. nxnative registered Mario Kart 8 Deluxe 4.0.0 on 2026-10-23 (`6ec201c`) and 4.0.1 on 2026-10-26 (`c443ba1`); the notes in [runtime/games/0100152000022000/README.md](../runtime/games/0100152000022000/README.md) are upstream's bring-up record, kept as they are.

| Destination in this game | Original at that commit | Adaptation |
|---|---|---|
| `runtime/host` | `dm/host` | none |
| `runtime/gfx` | `dm/gfx`, without the prebuilt binaries (`tools/dmsh_xlate_bin`, `tools/spirv_cross_bin`) and the translator's C# sources (`tools/dmsh_xlate`) | `tools/dmsh_chain.py`: the binary-only translator (below); `nvn/dmsh_chain.h` regenerated from it |
| `runtime/tools` | `dm/tools`, without upstream's multi-game `tests/` and the `ghidra/` tool | none |
| `runtime/mac` | `dm/mac` (the Mac app packer, launcher and settings window) | none |
| `runtime/ios/tools`, `runtime/ios/Sources/Launcher{Row,Settings}.swift` | `dm/ios/tools`, and the two launcher sources `dm/mac/pack_app.py` links into the Mac launcher | none; no iOS app target is offered |
| `runtime/games/{game.py,profile_guess.py,settings.py,settings.tsv,README.md}` | `dm/games` | none; no other game's profile is copied |
| `runtime/games/0100152000022000` | `dm/games/0100152000022000` | `game.tsv` registers 4.0.1 only, with a generic `romfs_marker` and `shader_groups`; the shader tables `shader_map.tsv`, `shader_facts.tsv`, `shader_pairs.tsv` and `pso_pairs.tsv` are left out; the game notes `README.md` get a header and two rows changed to match (below) |
| `runtime/third_party` | `dm/third_party`, without FLIP (verification only) and the Ryujinx sources | none |
| `prebuilt/macos-arm64/dmsh_xlate` | `dm/gfx/tools/dmsh_xlate_bin` | 12 files byte-identical; `.pdb` and Humanizer satellite resources left out; hashes in `SHA256SUMS` |
| `prebuilt/macos-arm64/spirv_cross` | `dm/gfx/tools/spirv_cross_bin` | byte-identical; hashes in `SHA256SUMS` |
| `third_party/nxnative/build_ipa_from_nsp.py` | `scripts/build_ipa_from_nsp.py` | the target's input gates (below) |
| `third_party/licenses/ryujinx.txt`, `spv-generator.txt`, `ryujinx-build.md` | `dm/third_party/ryujinx/LICENSE.txt`, `src/Spv.Generator/LICENSE`, `README.md` | the README's title and last paragraph name the binary-only copy |

[nxnative/import-manifest.json](nxnative/import-manifest.json) lists every copied file with its upstream path and SHA-256 **at import time**, and whether this target changed it. [nxnative/local-changes.patch](nxnative/local-changes.patch) is the diff of the changed files against that commit: a provenance record, not a build step, since the final source is already in `runtime/` and `third_party/nxnative/`. The two changed profile files, `game.tsv` and the game notes `README.md`, are left out of it because the upstream lines they replace name RomFS files; the manifest marks them, and the changes are described below.

### The changes

- **Input gates** (`build_ipa_from_nsp.py`, each marked `MacNativeGames mk8d`): the folder must hold Mario Kart 8 Deluxe (`0100152000022000`) with exactly the 4.0.1 update (`v1507328`) and no add-on content (`target_inputs`, before extraction). After extraction and before the rewriter reads them, every file of the 4.0.1 ExeFS must have the size, SHA-256 and build ID in [assets/expected-4.0.1.json](../assets/expected-4.0.1.json) (`verify_originals`). A filename or a build ID alone does not pass. `glslangValidator` is no longer a prerequisite: it compiles only an OpenGL game's GLSL, and this is an NVN game.
- **No shader tables in the repository.** Upstream's `shader_map.tsv` has a fifth column naming the RomFS path where each key first appears, and at this commit its readers need that column too (`dmsh_build.py` puts the whole row into each shader's cache key; the build script's shader watch and DLC check read column 5). `games/jamboree` dropped the column from the writer of its older snapshot; rather than change the writer and every reader here, this target commits no shader tables at all: with `TARGET_TABLES = False` the build script always takes upstream's derived-profile path for 4.0.1, and `shader_split.py` writes the four tables from the player's own files into the work folder, outside the checkout. This is the path upstream reports a full 4.0.1 race with (a derived profile). The prewarm list `pso_manifest.tsv` (shader and pipeline keys only), `bf_want.tsv` and the empty conflict table stay.
- **No RomFS names in the profile.** Upstream's `romfs_marker` names one RomFS file and its `shader_groups` sort the shaders by RomFS top-level folder. Here the marker is the extraction root (`.`: nx_extract writes the image into a `.partial` folder and swaps it in whole, so the root is there only when the image is complete) and one generic rule, `/=archive`, puts every RomFS shader in one group (`main`'s keep theirs), as `games/jamboree` does. The groups only name the split's packages; the shader keys are the same. The game notes say so in a header and in the two rows that named them.
- **Binary-only translator** (`dmsh_chain.py`): the chain version no longer hashes `dmsh_xlate/Program.cs`, `dmsh_xlate.csproj` and twelve Ryujinx source files, which are not copied, and the translator's expected embedded source hash is read from `dmsh_chain.lock`. The lock still pins the DLL fingerprints and that source hash, so a different translator still fails `--check-tools`. The chain version is therefore this target's own (`d9fda03e8b0d`; upstream's is `a78f966a7b38` at the same commit), and `runtime/gfx/nvn/dmsh_chain.h` carries it.
- **SPIRV-Cross built at packaging** (`dmsh_chain.py`, `dmsh_chain.lock`, after PR #1226): upstream's `spirv_cross_bin/spirv-cross` and its `build.sh` are not copied (the manifest lists them as left out). `scripts/build_mk8d.py` builds the converter from the same release, `vulkan-sdk-1.4.363.0` at `f11ba9f`, and the lock pins its `--revision` instead of a hash of its bytes, which change with the compiler; `--check-tools` still stops on another release.

## Copied from this repository

| Destination | Original | Adaptation |
|---|---|---|
| `scripts/build_mk8d.py`, `scripts/test_gpu.py`, `tests/test_build.py`, `Makefile`, `.gitignore` | `games/jamboree/scripts/build_jamboree.py`, `scripts/test_gpu.py`, `tests/test_build.py`, `Makefile`, `.gitignore` at MacNativeGames commit `5ab07b7` (Microbai, PR #684) | MK8D's title, version, bundle ID and inputs. Inputs are chosen by their ticket (the base game and the 4.0.1 update; add-on content is left out) and linked under tagged names. A local `NuGet.Config` with `<clear/>` and `NuGetAudit=false` in the environment replace the script-side restore change. Upstream's own Mac app packer (`runtime/mac/pack_app.py`, `--platform mac-app`) builds the app instead of Jamboree's launcher |
| `third_party/licenses/{deko3d,gommon,humanizer,libhac,msgpack-cli,recyclablememorystream,system.codedom}.txt` | `games/jamboree/third_party/licenses/` at `5ab07b7` | none (the dependency versions are the same) |

## Updating

Update the copies explicitly, inside this folder only. Record the new nxnative commit and hashes (regenerate the import manifest and the patch), keep the licenses, re-apply and re-check the changes above, regenerate `dmsh_chain.h` and the prebuilt checksums, rebuild the whole app from the original inputs (extraction, rewrite, shader split, shaders, host) and play the documented scenes again. Do not reuse a shader library made with another chain. A new game update needs its own pins in `assets/` and its own row in `game.tsv`; an unsupported build must stop rather than inherit 4.0.1's pins. Never commit original or extracted game data, rewritten modules, generated shader tables, translated shader code or libraries, keys, saves or game screenshots.
