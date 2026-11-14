# Worldend Syndrome — experimental native macOS target

[Validation](docs/validation.md) · [Source copies](third_party/COPIES.md) · [Licenses](THIRD_PARTY_NOTICES.md)

Isolated Apple silicon port of **WORLDEND SYNDROME** (`010048900CF64000`, base v0 / 1.0.0). The game's ARM64 code is rewritten ahead of time and runs through the nxnative host, with NVN rebuilt on Metal. **Mac only, experimental:** title through ADV, Name Entry, story choices, and the first-loop fade have been played locally. Not a claim that every route or the true end has been checked.

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

First boot needed no extra game patches. Docked `op_mode=console` fills 1920×1080. Offline MSL compile uses `DM_METAL_RUNTIME=1` (no Xcode Metal toolchain).

## Build

Use an Apple silicon Mac with Metal 4 support, macOS 26 or later, and Xcode with the macOS 26 SDK or later. Mac only.

Build dependencies:

- Python 3.11 or later with `lz4`, `capstone`, `fonttools`, and `backports.zstd` when Python is older than 3.14.
- .NET SDK **10**, `make`, `clang`, `zstd` and `spirv-cross` on `PATH`.
- A **local LibHac 0.19.0 NuGet package** supplied with `--libhac-package`. SHA-256: `1431267861db5ff68216315b164f10c0edab4df162cedf5429bf8a143b76de19`.
- Your own unmodified base NSP or XCI and `prod.keys`. No update is used.

Put the base in one directory. The inventory identifies the title through the container ticket or a `[title ID]` tag in the filename. Executable build IDs and NSO SHA-256 values are checked after extraction against [assets/expected-1.0.0.json](assets/expected-1.0.0.json).

From the repository root:

```sh
python3 games/worldend/scripts/build_worldend.py \
  /path/to/your/Worldend-inputs \
  --keys /path/to/your/prod.keys \
  --libhac-package /path/to/local/libhac.0.19.0.nupkg \
  --work-dir /path/to/private/Worldend-build \
  --output '/path/to/private/Worldend Syndrome.app' \
  --language en-US
```

`--work-dir` and `--output` must be outside the Git checkout. The first shader split fills `shader_map.tsv` in the staged profile; later builds require those tables to match.

Alternatively:

```sh
make -C games/worldend build \
  INPUTS=/path/to/your/Worldend-inputs KEYS=/path/to/your/prod.keys \
  LIBHAC=/path/to/local/libhac.0.19.0.nupkg WORK=/path/to/private/Worldend-build \
  OUTPUT='/path/to/private/Worldend Syndrome.app' LANGUAGE=en-US
```

```sh
make -C games/worldend test
```

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
