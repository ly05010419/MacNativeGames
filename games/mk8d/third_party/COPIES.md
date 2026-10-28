# Copies from other games in this repository

Mario Kart 8 Deluxe carries its own copy of everything it builds and runs with, so a change to another game never changes
this one, and its build never reads another game's folder.

## Base: `games/odyssey` at `ff63a16` (2026-10-27)

The runtime, its graphics layer and the build scripts were copied from `games/odyssey` at this repository's commit
`ff63a16` (Odyssey's last change there: `bb1940f`). Odyssey is the closest game in this tree: the same 2017 era, the
sead / agl engine on NVN, H.264 movies on VideoToolbox, 60 fps. What Odyssey had itself copied came along unchanged:

| Folder | Odyssey's source (see its `third_party/COPIES.md` at `ff63a16`) |
|---|---|
| `d2r/` (MIT, see its LICENSE) | `games/by3/third_party/d2r`: the ahead-of-time ARM64 rewriter (`patch_nso.py`, `equiv.py`, `structck.py`) |
| `d2r_gfx/` (MIT / BSD, licences inside) | `games/totk/third_party/d2r_gfx`, with Odyssey's changes: NVN on Metal 4, texture decoding, frame pacing |
| `totk-scripts/` | `games/totk/scripts`: `extract_exefs`, `extract_romfs`, `extract_shared_fonts`; `prepare_shader_seed`, `shader_list` from Odyssey's copy at `e29aa90` (see Shader seed below) |
| `by3-scripts/` | `games/by3/scripts`: `merge_romfs` (with its `extract_exefs` / `extract_romfs`), `prepare_modules` |
| `smaa/` (MIT) | SMAA area and search textures, as Odyssey carries them |
| `licenses/` | the licences of the above |
| `../prebuilt/macos-arm64/` | `games/totk/prebuilt/macos-arm64`: `totk_xlate` and its SHA-256 (see its README) |

Left out of the copy: Odyssey's research notes and changelog, its Xcode project, its shader list and input pins, its key 3
render-size table scripts (`make_render_res_overlay.py`, `make_resolution_overlay.py`), its survey and probe scripts (several
read `games/totk` directly) and its shader-seed wrapper `prepare_odyssey_shader_seed.py` (this game has its own, below).
Its app packaging came back adapted (below).

## Changes made here

Each change is to this game's copy only.

**Rewriter (`d2r/patch_nso.py`).** `ldrsw_table` accepts two forms of the signed-offset jump table in Mario Kart 8 Deluxe
4.0.1's `subsdk0` (the movie library): moves that leave x18 alone between the `add` and the `br`, and a table base set once in
the function's prologue (taken only when that `adrp` / `add` pair is the only write to the register in the whole function
and the function makes no calls). Every rewritten site of `main`, `sdk` and `subsdk0` passed `equiv.py` (3,627 + 6,957 +
8,362 sites, 0 divergent, 300 of 300 mutations caught per module).

**Graphics (`d2r_gfx`).**
- Odyssey's `GAME_ODYSSEY` branches for older NVN titles (NVN 53 device version, default depth mode, queue-side acquire,
  asynchronous shader compiles, concurrent Metal compiles, the pipeline-archive idle gate) are `GAME_MK8D`.
- Rendering into a block-compressed texture: cubes and cube arrays are accepted (their faces are slices, as in a 2D array),
  and the auxiliary block texture is one texture per level, exactly that level's blocks, because the small levels of a BC
  texture all hold one block, which one auxiliary mip chain cannot (Mario Kart 8 Deluxe renders every level of its compressed
  cubes). `blk_wb` keeps the auxiliary level apart from the compressed texture's level.
- A colour slot holding the same image level and layer as an earlier slot is left unattached, and its clears go to
  the earlier slot (`rt_dup_of` in `nvn_obj.h`). This was already there for Breath of the Wild (`GAME_BOTW`, from
  nxnative's `rt_dup_slot`) and now applies to `GAME_MK8D`. Mario Kart 8 Deluxe's G-buffer pass attaches one texture
  at slots 3 and 4 with every draw masked to slot 3, and the stored slot 4 left tile-sized squares in races.
- Odyssey's picture modules (`odyssey_gfx.h`, `odyssey_mfx.h`, `odyssey_gfx_present.inc`: 60+60 frame generation, the TAA
  camera, the FXAA detection) are kept as copied. They act only on Odyssey's shaders and settings and stay off here; this
  game's own picture options replace them later.

**Runtime (`../runtime`).**
- `game_version.h`, `game_patches.c`: Mario Kart 8 Deluxe 4.0.1, no game-code patches. `odyssey_aspect.c` (Odyssey's camera
  addresses) is removed; key 4 reports itself unavailable.
- `frontend.m`: the display link is made for the window's screen when the session lists no active displays
  (`CVDisplayLinkCreateWithActiveCGDisplays` returns `kCVReturnInvalidArgument`); `nn::vi::SetLayerScalingMode`. Names and
  switches say Mario Kart 8 Deluxe (`MK8D_*`).
- `mii_host.c` (new): `nn::mii::Database` and the Mii editor applet answered by the host (an empty database, the six default
  Miis, random Miis, every CharInfo checked with the SDK's own `CharInfoRaw::Verify`) and the MiiModel system data stand-in.
- `fs_host.c`: device save data as a save of its own (`MountDeviceSaveData`), `IsSaveDataExisting(uid)`, and read-only system
  data (`QueryMountSystemDataCacheSize`, `MountSystemData`) for the MiiModel stand-in only.
- `account.c`: `ListAllUsers`, `ListOpenUsers`, `OpenUser`, and `LoadCachedNintendoAccountInfo` (unavailable: no Nintendo
  Account is linked).
- `services.c`: `nn::oe` exit-request section, display version and pseudo device ID; the add-on content list-changed event.
- `shared_font.c`: `RequestSharedFontLoad` and `GetSharedFont` (the six fonts in type order).
- `input.c`: Joy-Con assignment (single, merge, swap, query) and the touch screen state history, at `nn::hid::detail`, which
  sdk 4.4.0's public functions call through relocations.
- `audio_renderer.c`: the render info section (elapsed frame count) only from revision 5 on; this game speaks REV4.
- `system_services.c`: offline answers for friends (presence `Commit`, an empty friend list, online sessions, play
  history, a zeroed registration key), nifm (`SetLocalNetworkMode`, `GetCurrentIpConfigInfo`) and prepo
  (`GetTransmissionStatus`). Time Trial needs `friend:u`.
- `movie_host.c`:
  - sdk 4.4.0 builds decoders with the three-argument `movie::Decoder` constructor (C1 / C2), which is answered too.
  - H.264 fixes:
    - Held pictures go to free output slots before an input waits for room. Before, the decoder stopped after 8 frames
      for good.
    - The game seeks inside one movie without Flush: its menu previews are segments of one file. An input time jump of
      more than a second drops the pictures not yet handed over, as a Flush would, and the pacing clock restarts there.
      Before, the new position's frames were due minutes later, and the menu waited for them for good.
  - `TOTK_MOVIE_TRACE=<n>` logs the first n calls of each step (1: 400).
- `ipc.c`: `TOTK_IPC_TRACE=1` logs the first use of each service command, with its time (off by default).

**Scripts (`../scripts`).** The title ID, version and pins come from `mk8d.py` and `../assets/expected-4.0.1.json`.
`prepare_runtime.py` imports this game's own `d2r` (Odyssey's imported `games/by3`'s). `build_mk8d.py` replaces
`build_odyssey.py`:
- NSZ inputs are expanded with `nsz -D`, as the repository's `build_app.py` does.
- The work folder must be outside the checkout and outside iCloud Drive (Desktop and Documents when synced).
- Steps 0–1 are skipped once the game is extracted.

**Shader seed (`../scripts`, `totk-scripts/`, `../assets/shader_list.tsv`).**
- `totk-scripts/prepare_shader_seed.py` is Odyssey's copy, unchanged. `totk-scripts/shader_list.py` is Odyssey's copy with
  two changes: a header for this game's list (`HEADERS['mk8d']`, chain `totk-native-v4-unverified`), and `export`'s default
  source names this game's list instead of `games/totk`'s.
- `prepare_mk8d_shader_seed.py` (new, after Odyssey's `prepare_odyssey_shader_seed.py`) runs the shared tool with this
  game's differences: the runtime's chain; every RomFS file and `main` scanned, by one process per core (shaders sit in
  each course's and object's archives); SARC members without a name read by index (the UI layouts; the shared reader
  rejected those archives whole, which hid 29 UI shaders); vertex `.variants` rows marked `depth_minus_w`; no
  `shader-keys.json` (the importer rejects it). An unchanged seed is reused.
- `precompile_shader_libs.py` is Odyssey's `precompile_cached_shaders.py`, reading the seed and keeping only libraries
  the seed still names.
- `../assets/shader_list.tsv` (new): this game's list, shader keys and bind states only. Its 142 rows are the states met in
  play that the seed's default translations do not serve (textures bound as depth textures, `uint` colour outputs),
  from `shader_list.py merge` of the play caches and `prune` against the seed.
- `build_mk8d.py` step 8 runs both (`--skip-seed` leaves them out); `package_mk8d_app.py` clones the seed and the
  libraries into the app; the launcher and `make run` default to `TOTK_WARM_SCOPE=observed`, so a start warms what this
  Mac has used rather than all ~30,000 shaders.

**App (`../scripts`, `make app`).**
- `package_mk8d_app.py` and `mk8d_app_launcher.c` are adapted from Odyssey's `package_native_app.py` and
  `odyssey_app_launcher.c` at `ff63a16`.
- Kept:
  - the bundle of APFS clones;
  - the runtime started through a symlink in `Contents/MacOS`;
  - the data folder outside the bundle, with the RomFS proof file there;
  - save backups for the last five sessions, the previous log and crash logs;
  - full screen unless `windowed` exists;
  - `controls.txt` and `face-buttons.txt`;
  - the freeze watchdog;
  - ad-hoc signing.
- Changed:
  - the environment is `make run`'s;
  - the data folder is named in `Contents/Resources/data-dir.txt`, so the app can live in `/Applications` while its data
    stays in the work folder;
  - it is seeded once from `make run`'s save;
  - the launcher passes the first preferred language as `TOTK_LOCALE`. Inside a bundle without localizations, the
    runtime's current locale is English.
- Left out (none applies to this game yet): Odyssey's settings window (`odyssey_launcher_ui.m`), its render-size tables,
  MetalFX defaults and the 20-minute session limit.
- `extract_app_icon.py` accepts the update alone: an NSZ keeps its small Control NCA as is. `build_mk8d.py` extracts the
  icon in step 7.

**`../Makefile`.** `make run` presents without the display link (`DM_PRESENT_LINK=0`), falling back if presents stop
(`DM_PRESENT_LINK_FALLBACK=1`). Started from some shells, the session lists no active display, and the display link went
silent for tens of seconds at a time.

**Tests (`../tests`).** Odyssey's probes of the code this game runs are kept and named for it: `test_input.py` (now
also `controller_reach`) and `test_runtime.py` (Odyssey's `test_pr256_regressions.py`, now also the freeze watchdog and
the shader seed importer). Left out: the frame-interpolation camera test (Odyssey's camera only) and the
`system_event_bind` probe (built from `games/totk`'s Makefile). `test_build.py` (new) checks the 4.0.1 pins, the work
folder rules and that the build reads no other game's folder; it found `build_mk8d.py`'s checkout check failing with a
`TypeError` instead of its message, fixed.

## Knowledge from nxnative

No code is imported from nxnative for this port (the runtime copied from Odyssey keeps Odyssey's nxnative MIT notice,
`licenses/nxnative.txt`). [seedds/nxnative](https://github.com/seedds/nxnative) (MIT) ran this game first, and its notes
(`dm/games/0100152000022000/README.md`) pointed at what the host needs: the Mii database and the MiiModel stand-in layout, the
device and account saves kept apart, sdk 4's audio renderer. Each was implemented here in this runtime's own way and checked
against the game; the code comments say where an idea came from.

## Updating

Update copies explicitly inside this game and record the source commit here. After a change to the rewriter, rewrite the
modules again, update `../assets/expected-4.0.1.json`'s `rewritten_sha256` and run `d2r/equiv.py` on every module. A
fixed-address game patch must name main's build ID and check the original instruction first; an unsupported build stops.
Never commit original or extracted game data, rewritten modules, translated shaders, keys, saves or game screenshots.
