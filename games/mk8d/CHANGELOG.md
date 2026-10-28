# Mario Kart 8 Deluxe: changelog

## Status

What works and what does not: [README.md](README.md#status). This page says what changed, why, and how each change was checked; the detailed runs are in the [validation record](docs/validation.md).

## Where the code comes from

The runtime, graphics layer, rewriter, shader tools, Mac app packer and game profile are nxnative's ([seedds/nxnative](https://github.com/seedds/nxnative), commit `062cd06`, 2026-10-28), where Mario Kart 8 Deluxe was brought up (4.0.0 on 2026-10-23, 4.0.1 registered on 2026-10-26). The build wrapper, the input pins and the tests follow `games/jamboree`. What was copied and changed: [third_party/COPIES.md](third_party/COPIES.md).

All measurements on 28 October 2026: MacBook Pro, Apple M1 Max (GPU `applegpu_g13s`), 32 GB, macOS 27.0.1 (26A434), Xcode 27.1; the app built with `scripts/build_mk8d.py` from a base NSP and the 4.0.1 update NSP.

## Log

### 2026-10-28: games/mk8d, Mario Kart 8 Deluxe 4.0.1

- nxnative imported at `062cd06` into `runtime/`, `prebuilt/` and `third_party/nxnative/`, in `games/jamboree`'s layout; the import manifest records every file's upstream path and SHA-256 and marks the seven this target changes; the local patch holds the diffs of five of them (the profile's two are described in COPIES.md, since the lines they replace name RomFS files).
- **Inputs.** `build_mk8d.py` links only the base game and the 4.0.1 update from the input folder, chosen by their tickets; anything else, such as add-on content, is reported as ignored and never reaches the build. The build script stops before extraction on another title, another update or add-on content, and before the rewrite on any 4.0.1 ExeFS file whose size, SHA-256 or build ID differs from `assets/expected-4.0.1.json`.
- **No shader tables in Git.** Upstream's `shader_map.tsv` names the RomFS path each key first appears in, and at this commit its readers need that column. The profile carries no shader tables; every build splits them from the player's own 4.0.1 files into the work folder (upstream's derived-profile path, the one its full 4.0.1 race was reported with). The port's split gave the same shaders as an unmodified upstream build of the same files: 29,651 (12,530 vertex, 17,092 pixel, 28 compute, 1 geometry), the same stage and key in every map row, and byte-identical facts and 18,747 pairs; only the package names differ, by the generic group below. Its rewritten modules are byte-identical to upstream's too.
- **No RomFS names in the profile.** The extraction marker is the extraction root and one generic shader group replaces upstream's groups by RomFS folder, as in `games/jamboree`; the shader keys do not change.
- **Binary-only translator.** The Ryujinx-based translator is carried as checksummed binaries; `dmsh_chain.py` takes the translator's source hash from its lock instead of hashing a source checkout, so the chain version is this target's own (`d9fda03e8b0d`).
- **SPIRV-Cross is built at packaging** (after PR #1226): `build_mk8d.py` fetches release `vulkan-sdk-1.4.363.0` (commit `f11ba9f`), checks the commit, and compiles its 11 sources with clang; the lock pins its `--revision` rather than its bytes, which differ between compilers. On 1,204 shaders its MSL is byte-identical to the binary the PR carried.
- **Offline build.** LibHac 0.19.0 comes from a local, SHA-256-pinned package through a NuGet configuration that clears every other feed, with NuGet auditing off; .NET's caches stay in the work folder. glslang is no longer required (an OpenGL game's tool).
- **Online and local wireless** are cut by the host as upstream has them: nifm reports no network, ldn never initializes.
- **Checked:** scripted runs of the built app (the title, the Mii pick, a full 50cc Grand Prix race to the results at 60 fps, the online and wireless menus) and two sessions by hand with a DualSense. The race by hand was in the unmodified upstream build used for the comparison, and the start by hand was in this target's app: [validation record](docs/validation.md).
