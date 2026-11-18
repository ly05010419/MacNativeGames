# Worldend Syndrome — experimental native macOS target

[Validation](docs/validation.md) · [Source copies](third_party/COPIES.md) · [Licenses](THIRD_PARTY_NOTICES.md)

Isolated Apple silicon port of **WORLDEND SYNDROME** (`010048900CF64000`, base v0 / 1.0.0). The game's ARM64 code is rewritten ahead of time and runs through the nxnative host, with NVN rebuilt on Metal. **Mac only, experimental:** title through ADV, Name Entry, story choices, and the first-loop fade were reported as played by the original contributor. Not a claim that every route or the true end has been checked.

Unity 2017.4 per-stage VS/FS binds work through the host's combined programs. Zero game-code patches (0 CUTS).

**No game content is in this repository.** You need a legal dump of your own base-v0 NSP or XCI and keys from your own Switch.

## Identity

- Title: WORLDEND SYNDROME
- Title ID: `010048900CF64000`
- Version: 1.0.0 (base v0 only)
- Publisher: Arc System Works (developer TOYBOX)
- Languages (NACP): American English, British English
- Master key revision: 7 (NCA); ticket master key revision 8
- SDK: 6.4.0.0
- NPDM: `sys_resource=0`; SVCs include `SetHeapSize`, `SetThreadActivity`, `GetThreadContext3`

## Engine and middleware

- Engine: Unity 2017.4.17f1, IL2CPP (AOT), Switch target.
- ExeFS modules: `main`, `sdk`, `subsdk0`, `rtld`.
- Graphics: NVN on Switch; this tree rebuilds on Metal.
- Audio/video: CRIWARE (Sofdec Prime movies, CRI audio). RomFS carries `movie.cpk`, `sound.cpk`, `voice.cpk`.
- Saves: Unity/mono-style `WorldEndSave` data; redirected to
  `~/Library/Application Support/MacNativeGames/Worldend Syndrome/` without altering the save format.

## What was vendored

Copied from `games/jamboree` at MacNativeGames `92ec79b` (nxnative host / NVN-on-Metal / UnityFS shader tooling). Provenance: [third_party/COPIES.md](third_party/COPIES.md). The donor's Jamboree profile, shader maps and allocator cuts were not kept.

Gleaned Unity 2017.4 needs already present in this host (from Cuphead / AI: THE SOMNIUM FILES notes in the donor):

- `nvnTextureGetMemoryPool` / zero-size pool handling
- IL2CPP GC thread pausing via `SetThreadActivity` / `GetThreadContext3`
- pre-2019.3 Unity shader subprogram layout in `shader_split.py`
- mixed vertex+fragment program binding

First boot needed no extra game patches. Docked `op_mode=console` fills 1920×1080. The build compiles Metal libraries offline with Xcode (`DM_METAL_RUNTIME=0`).

## Build

Use an Apple silicon Mac with Metal 4 support, macOS 26 or later, and Xcode with the macOS 26 SDK or later. Mac only.

Use MNG Launcher, or the same build engine from the repository root:

```sh
python3 tools/mng.py build "<base>.nsp" --keys "<prod.keys>"
python3 tools/mng.py update worldend
```

Python 3.14 or later and Xcode are required. The build supplies its pinned Python packages and the shared
`tools/deps` supplies .NET, SPIRV-Cross, zstd, LibHac and static libvpx. No manual package download is required.
The base v0 is the only supported version. Original module sizes and SHA-256 values must match
[expected-1.0.0.json](assets/expected-1.0.0.json).

The work directory is recorded in the App (`MNGWorkFolder`); `--work` selects an explicit external directory.
Normal rebuilds keep the App directory and reuse matching signed data bundles. Express updates rebuild only the
host and launcher when verified game, rewrite and translation inputs match. Game data are packaged in
`GameData-worldend.bundle`, libraries in `ShaderData-worldend.bundle`; unchanged bundles keep their signatures.
Pipeline archives are checked against actual descriptors using parallel workers outside the App, then verified in a
fresh process. The exact warm-up list is placed beside `build/shader_map.tsv`. Launch waits for the whole list.

The contributed profile currently has **no recorded render/compute pipeline rows**. All 543 mapped libraries are
warmed, but first-use pipeline compilation in game remains unmeasured; the build does not invent pipeline
coverage from an empty list. The offline archive path has a native host regression with synthetic shaders.
No end-to-end game or route validation was performed as part of the packaging integration.
Online shader contributions are not enabled: this profile's Unity package keys need a matching inbox schema before
uploads can be accepted. It does not reuse another game's inbox identity.

Developer entry: `games/worldend/build.py <base> -k <keys> --work <external-work> -o <App>`.
`make -C games/worldend test` checks source staging and tool fingerprints without game files.

## Status

| Done | Details |
|---|---|
| Isolated tree | runtime, rewriter, shader translator, builder, tests |
| Identity pins | NSO sizes, SHA-256, 16-byte build IDs for `main` / `sdk` / `subsdk0` |
| Title gate | NSP builder accepts only `010048900CF64000` v0 |
| Heap / NVN | `mem_used=0x4A00000` (70 MB modules); `device_api=53.399` |
| Shader map | 543 Unity 2017.4 keys; 273 VS paired as single-stage (pre-2019.3) |
| Title + extras + Options | Docked 1080p; Menu bag (Tips/Album/Characters/Map/Missions/Collection/Options); System settings (skip/text/volume tabs) |
| Continue / load / ADV | Title Continue opens Load; slot 0 confirms and resumes ADV. Autosave after the inn. Plus opens Save during ADV (B to dismiss unless a new slot is intended). |
| Name Entry + first choice | Transfer form Confirm works (given name required; Continue? defaults to No). First story choice (two stacked options) reached after school intro. |

| Not done | Details |
|---|---|
| First ending | private playthrough reached the first-loop fade (`TO THE WORLD'S END` card, then June 1 again). Town **gameplay** map is later; extras Map is not that. |
| Picture | title watermark / some UI leftovers; ADV and character scenes draw |
| Game patches | none; add only with asserted original instructions |
| iOS | not built |
| Playable claim | experimental Mac bring-up through the first choice; not every route |

## First-boot notes

1. Rewrite of pinned v0 `main` / `sdk` / `subsdk0` succeeded with 0 CUTS.
2. Default 64 MB used-memory made `SetHeapSize` overflow the 3285 MB pool; `mem_used=0x4A00000` yields a 3.13 GB heap.
3. SDK 6 would report NVN 53.313; Unity 2017.4 traps unless minor > 311, so the profile reports 53.399.
4. Offline `dmsh_build` via `own_compilers.py` + `DM_METAL_RUNTIME=1` (no Xcode Metal toolchain). Host compiles MSL on first use.
5. Unity 2017.4 subprograms are one stage each. `shader_split.py` now tags them `#usub` so `separable_pairs` pads every VS for draw-time FS binds (same shape as Tsukihime's mixed programs). Pass dump: `st0=04d24c2b8553d166` + `st1=d406201564dbf8d5`, 6-vertex blended quads.
