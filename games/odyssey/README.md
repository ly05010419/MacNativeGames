# Super Mario Odyssey native Mac port

Super Mario Odyssey **1.3.0** running natively on Apple silicon Macs, built from **your own copy** of the game. The
game's own ARM64 code is rewritten ahead of time and runs directly on the Mac's CPU: no interpreter, no JIT, no CPU
emulator. System services are answered by a small native host, and the Switch's NVN graphics are rebuilt on Metal 4,
with the game's GPU shaders translated to Metal.

**No game content is in this repository, and none will ever be.** You need a legal dump of your own cartridge or
download, and keys from your own Switch. We do not help anyone obtain keys or game files.

## Status (23 October 2026)

Tested on one Mac: MacBook Pro, Apple M3, 8 GB, macOS 26.6.

| Works | Details |
|---|---|
| Boot, title menu, save list | resume, new game |
| Play in Cap Kingdom and Bonneton | keyboard and game controllers; cap throw; camera |
| Saves | autosave and reload; the app keeps the saves from before each of the last five sessions |
| Sound and movies | game audio; H.264 tutorial and story movies |
| Performance | locked 60 fps in Bonneton on the M3 (frame time p95 about 17.5 ms); MetalFX upscaling to the window |
| Latency | full screen by default: about 30 ms from game frame to screen (63 ms windowed) |

| Not done or not verified | Details |
|---|---|
| Later kingdoms | not played yet |
| Long sessions | the packaged app closes the game after 20 minutes (`session-minutes.txt`): earlier, longer runs of this renderer caused macOS GPU-driver panics, and longer sessions have not been re-verified |
| VR mode (Labo) | starts without crashing but stops at the game's "remove the console from the dock" screen; not supported |
| Online features | answered as offline |
| Memory | about 3.5 GB in play; on an 8 GB Mac close other large apps |

Exact measurements and logs are in the development log below and in the project's test evidence.

## Optional frame interpolation

Key **2** toggles a request for **60 real + 60 generated pictures** on a 120 Hz
window screen and supported Metal 4 GPU. It is off by default. Odyssey's game
logic stays at 60 Hz; other screen rates suspend generation. Motion comes from
camera reprojection, so moving characters can still show artifacts. See
[frame interpolation](docs/frame-interpolation.md) for inputs, limits and checks.

The modes copied from Tears of the Kingdom are gone: key 2 now toggles instead
of cycling, and `TOTK_MFX=interp30`, `interpmax` and numeric values all request
60+60. 30+30 no longer exists, so on 60, 100 or 144 Hz screens nothing is
generated.

## What you need

- An Apple silicon Mac with macOS 26 or newer, and Xcode with the Metal 4 SDK (tested with Xcode 27.1).
- Super Mario Odyssey with the **1.3.0** update, as a combined XCI or NSP that you dumped yourself. Other versions are
  rejected by hash, including **1.4.1**: it has the same executables but slightly different data, and is not
  supported yet (see "Version naming" below).
- `prod.keys` dumped from your own Switch.
- Your Switch's **firmware** files (its NCA folder), dumped from your own Switch: the system fonts come from there.
  Some game cards carry them in their system-update partition, but not all (the tested 1.3.0 card doesn't).
- Homebrew `opus` and `libvpx` (`brew install opus libvpx`), and Python 3 with `capstone`, `cryptography` and `lz4`.
- About 12 GB of free disk space.

Everything the build writes goes to a work folder inside `games/odyssey` (default `Odyssey_build/`), which Git
ignores. Never commit or share anything from it: it contains game data.

## Build your own copy

From `games/odyssey/`:

```bash
python3 -m venv .venv && .venv/bin/pip install capstone cryptography lz4
.venv/bin/python scripts/build_odyssey.py --game YOUR_GAME.xci --keys prod.keys --firmware YOUR_FIRMWARE_FOLDER
open Odyssey_build/Odyssey.app
```

The script runs eight steps and stops with a clear message if something is missing:

1. check the input (title, version, integrity; it prints no keys or game data);
2. extract the program modules and merge the base game with the update;
3. extract the system fonts;
4. rewrite the ARM64 modules ahead of time;
5. verify the modules, the rewrite and the game data against the pinned 1.3.0 hashes;
6. build the shader tools (pinned SPIRV-Cross) and the runtime;
7. prepare the whole-game shader seed, so first play stutters much less (`--skip-seed` leaves it out);
8. extract the game's icon and package `Odyssey.app`, with saves, shader cache and logs in `Odyssey Data` beside it.

Each step is skipped when its verified output is already there, so after an error you fix the cause and run the same
command again. Other options: `--work FOLDER` (a folder inside `games/odyssey` whose name ends in `_build`, with no
space in its path below `games/odyssey`, since `make` can't handle spaces), `--developer-dir` for another Xcode.

**Tested 23 October 2026** from an empty folder on the M3 test Mac with a 1.3.0 card dump: about 9 minutes in all
(the shader seed about 6, packaging about 2). The rewritten modules, the merged game data and all 129,602 shader-seed files came
out byte-identical to the known-good build, and the new app started and showed the game's opening screen. That count
was made with the translator the port was developed on; with the translator in `prebuilt/` (main since 069a999) the
seed has 28,131 shaders in 89,892 files (24 October), which is what a correct build from main gives. Others have built
and played it from NSPs (base + 1.3.0 update merged into one PFS0, #657) and with the Command Line Tools only (#655).

Optional, after you've played a while: `scripts/precompile_cached_shaders.py` compiles the shaders your game has
translated into Metal libraries ahead of time; then run the build command again to repackage.

## Playing

- Keyboard: WASD move, J jump (A), K (B), U throw cap (Y), I (X), arrows camera, Return pause (+). Game controllers
  supported by macOS work too.
- Full screen by default; ⌃⌘F or the green button leaves it. Create an empty file `windowed` in `Odyssey Data` to
  start in a window, or `no-metalfx` to turn off MetalFX upscaling.
- The app is built from your own game files: do not share it.

---

# Development log

_The notes below are the port's development record, newest results first; the guide above is the current summary._ This directory starts an Odyssey-specific port using the approach developed for Tears of the Kingdom and Bayonetta 3 in this repository. (Historical, 21 October: it was not playable yet.) The existing `build_app.py` recognizes Zelda titles only; changing a title ID cannot make its game-specific runtime patches work for Odyssey.

## Current result — 22 October 2026

**Later on 22 October (~08:15).** Async shader translation is now default on for Odyssey builds (`TOTK_SHADER_ASYNC=0` to disable). A 270 s run reached Bonneton and got Cappy; the game autosaved and the save reloads at that point (`profiles/bonneton-cappy`). `scripts/package_native_app.py` builds the self-contained `Odyssey_build/Odyssey.app` (launcher `scripts/odyssey_app_launcher.c`; data in `Odyssey_build/Odyssey Data/`), launched from Finder by the user. Current status and next steps: root `README.md`.

**Playable milestone (22 October, ~07:35).** The movie library keeps the avcC record under the format key `avcc`, so `movie_host.c` reads SPS/PPS from there and the PlayGuide H.264 clips play. `runtime/input.c` now sets the NpadButton stick-direction bits (16–23: L left/up/right/down, R left/up/right/down) when an analog stick is past half deflection, as Switch HID does. Odyssey's Cap Kingdom camera tutorial waited on them. `TOTK_INPUT_TRACE=1` also logs stick values. With both fixes, `scripts/run_resume_probe.py boot-resume-play.log 160` resumes the baseline save; Mario wakes, the HUD appears, and keyboard W walks, J jumps and the arrows turn the camera.

**Afternoon update (skip, async shaders, save, H.264).** `scripts/run_skip_probe.py` (new game, empty profile) now reaches “Skip the movie?” and skips. `TOTK_SHADER_ASYNC=1` (opt-in; `TOTK_SHADER_WORKERS`, default 4) moves runtime shader translation off the submit thread: `totk_rt_variant` returns nil while a variant is queued, the iOS skip-draw path skips its draws, and `nvn_mtl.m` `prog_retry_missing` / `prog_fit` look again whenever `totk_rt_completed()` advances. With it, a bounded run reached Cap Kingdom and the game wrote `File1.bin` + `Common.bin`; the copy in `Odyssey_build/profiles/cap-kingdom-start` is the baseline for `scripts/run_resume_probe.py`. That run then aborted opening Switch hardware H.264 (decoder type 1; `NvMMLiteOpenBlock → NvRmChannelOpen → NvOsDrvOpen`). All 44 `MovieData` movies are H.264 (24 PlayGuide, 17 World, 3 snow; 41 also carry AAC audio) (Baseline/Main/High, 480×272 to 640×360, 4-byte NAL lengths, most with B-frames). `runtime/movie_host.c` now runs type 1 on VideoToolbox: SPS/PPS come from `csd-0`/`csd-1` (via `MediaData::FindBuffer`) or in-band, samples may be Annex B or length-prefixed, and pictures are reordered by PTS. It is built but not yet exercised in-game. Pre-change copies are `Odyssey_build/*.pre-async` and `*.pre-avc`.

The native Odyssey 1.3.0 runtime renders its Joy-Con screen, Start menu, story text, and the opening 3D Bowser cutscene in a Mac window. A bounded run accepted A at the Start menu and produced game audio. The ignored `Odyssey_build/Odyssey Test.app` uses a verified title icon from the user's local Control RomFS and a signed native launcher; `../../../Odyssey Test.app` links to it. Launch Services opened the app, displayed the game window, and closed it on command. This is a local 90-second test app that depends on this checkout; gameplay, saves, visual fidelity, and stability beyond the short runs remain unverified.

`Odyssey_build/boot-scene-review.log` and `boot-scene-warm-repeat.log` are the current performance evidence. The first review prewarmed 361 shaders and 403 pipelines; the repeat prewarmed 394 shaders and 434 pipelines. The repeat's 45–55 second cutscene span had no measured new pipeline compilation, but the first large texture upload took 3,382 ms (2,279 ms in texture upload), and one new shader variant took 2,958 ms near 58 seconds. The remaining 300–470 ms CPU stalls are not yet attributed. Both runs had one short audio underrun. The first 3D screenshots are in ignored `Odyssey_build/capture/scene-review/` and `scene-warm-repeat/`; see `../../../TEST_EVIDENCE.md`. Do not infer full visual correctness or playability from these captures.

The next performance work is to profile why the first 304 texture uploads vary from 154 ms to 2,279 ms across runs, keep collecting exact shader variants for startup prewarming, and measure later playable scenes. The current shader cache warms automatically at launch; it does not yet cover every cutscene variant. A later 90-second bounded run rendered the opening cinematic through Cappy at 85 seconds and reported zero audio underruns. A synthetic W press occurred during the cinematic, so controllable gameplay remains unverified; the isolated save folder remained empty. A first cutscene-skip experiment pressed A before the menu was ready and was inconclusive. A corrected run pressed A at 25 seconds and Plus during Bowser's scene at 45 seconds; the 50-second capture still showed the cutscene with a “+ Skip” prompt, so one short Plus press did not establish a skip. See `../../../TEST_EVIDENCE.md` for the exact logs and captures.

Further bounded tests found that a second Plus opens “Skip the movie?”; the next game call initially faulted in the SDK screenshot-orientation service. The exact Odyssey 1.3.0 `main` import `_ZN2nn2oe29SetScreenShotImageOrientationENS_5album16ImageOrientationE` and the `sdk` export/disassembly establish a scalar orientation argument; `runtime/system_services.c` now provides the macOS window answer. A later test observed argument `0` at the handler and closed normally. That run's menu input did not yet confirm skip completion. A local `probe-save` passed 15 generic storage/commit checks, not an in-game save/reload. Optional `DM_UP_PROFILE` instrumentation found seven >10 ms uploads totaling 154.8 ms, of which 154.6 ms was encoding; this sample does not explain all startup hitching.

The Odyssey-specific [smo-recomp](https://github.com/Coz-0/smo-recomp/blob/main/docs/architecture.md) source suggests scripted play-state checkpoints, worker preparation of graphics pipelines, and stricter console thread scheduling as useful investigation paths. It targets Windows/Vulkan and Odyssey 1.4.1; our 1.3.0 module layouts and runtime behavior must be verified locally before adapting any game-specific code. The [MacNativeGames builder](https://github.com/ly05010419/MacNativeGames/blob/main/build_app.py) and [Apple Metal documentation](https://github.com/apple/game-porting-toolkit/blob/main/game-porting-skills/skills/creating-metal4-shader-pipelines/SKILL.md) support offline Metal-library compilation. `scripts/precompile_cached_shaders.py` compiles exact locally seen MSL variants to hash-named libraries already recognized by this runtime. Apple's optional Metal Toolchain is being installed; no compile result or performance improvement is claimed yet.

### Upstream performance backports — 22 October 2026

Upstream MacNativeGames is 359 commits ahead of our `43f53b4` checkout (`origin/main` `30a1b93`). Odyssey's renderer came from Bayonetta 3, which did not receive the newer TotK renderer work. Six self-contained upstream commits were backported:

- `646ce29` shader-variant prefetch: on `nvnProgramSetShaders`, cached variants compile in the background. It is **off by default** (`TOTK_SHADER_PREFETCH=1` turns it on). When on, Odyssey hashes after the first frame on a utility queue with 2 compile slots (`TOTK_PREFETCH_SLOTS`). Hashing all ~26,600 boot-time programs on the game thread had delayed startup.
- `b5cbbca` pipeline-prediction rescans.
- `2fa27d7` smaller GPU chunks.
- `373725b` and `f08266c` replay lookup caches.
- `dab728a` kernel synchronisation fixes in `runtime/threads.c`.

The new Metal compile flag `shouldMaximizeConcurrentCompilation` is also set (`DM_MAX_CONCURRENT_COMPILE=0` turns it off). The renderer objects compile. `tests/system_event_bind/probe.c` passes all three parts against the new `threads.c` and fails against the old one. Four bounded 90 s GPU runs resumed a copy of the Cap Kingdom save and alternated prefetch off and on; all exited 0 and reached the same in-game scene. In run order (off/on/off/on), long frames after 45 s were 26/40/10/4, worst 644/174/104/87 ms, with no in-game compile stalls after the first run. Warming across runs dominates, so prefetch's own effect is not established. The startup critical-pipeline wait swung between 0.2 and 38.9 s with Metal cache state (`boot-ab-prefetch-*.log`). The upstream **pipeline binary archive** was then ported and is on by default (`TOTK_PIPELINE_ARCHIVE=0` turns it off). Five bounded runs cut startup warm-up from 4.4 s (off) to 1.6 s (149 archived pipelines). After a rebuild, though, the first runs can spend more than 90 s compiling shader libraries from source before the first frame. Removing that needs precompiled `.metallib` files and the Metal Toolchain, whose download is still pending. [`PLAYBOOK.md`](PLAYBOOK.md) collects outside techniques for later problems, with sources: emulator bug history for SMO, 1.3.0 mod offsets, test and replay tools, the save format, GPU fault reporting, pacing, audio, input and packaging. [`TECHNIQUES.md`](TECHNIQUES.md) lists each backport with its upstream measurement, the upstream work still to port (pipeline binary archive first), and external techniques with sources.

### Version naming: 1.3.0 and 1.4.1 share their executables, not their data

This port is built and verified against update **1.3.0** (the update's Control NACP, `scripts/inspect_version.py`,
`Odyssey_build/analysis/versions.json`). Update **1.4.1** is a later update with the **same executables** but
**slightly different data**:

- Checked 23 October 2026 against a local 1.4.1 update NSP (metadata and hashes only): its NACP says **1.4.1**;
  `main`, `sdk`, `subsdk0` and `rtld` are byte-identical to 1.3.0's (same SHA-256 and build IDs); `main.npdm` differs
  only in a 512-byte block near its start (the signature area); its program NCA is 16,896 bytes larger, so its RomFS
  patch differs. Which RomFS files differ is not known yet.
- Other projects target the same executables under the 1.4.1 name: **[nxnative](https://github.com/seedds/nxnative)**
  (profile `dm/games/0100000000010000/game.tsv`, `build v393216 1.4.1`, with these build IDs) and probably
  **[smo-recomp](https://github.com/Coz-0/smo-recomp)** (no build IDs published; our main already imports the Labo VR
  calls it lists as new in "1.4.1").

| Module | Build ID (first 16 bytes), 1.3.0 and 1.4.1 |
| --- | --- |
| main | `b424be150a8e7d78701cbe7a439d9ebf` |
| subsdk0 | `93ff8252d975adf07230cb86c83301c5` |
| sdk | `fcf93a41b08c5193a1410d262602dc62` |

What this means:

- Code addresses (`main+0x…`, `sdk+0x…`) from nxnative or smo-recomp apply to this build; per AGENTS.md rule 5,
  still verify each one by disassembly before using it.
- **The build accepts 1.3.0 only.** `scripts/prepare_runtime.py` checks the merged RomFS against
  `assets/expected-local-update.json`, so 1.4.1's different data is rejected until it is examined and added as a
  supported version.

## Open in Xcode

`Odyssey.xcodeproj` is an Xcode navigator for this port's source plus an external-build target named **Odyssey Native**. Its Build action runs `make runtime` in this folder, using the same verified Makefile and ignored `Odyssey_build/` outputs as the command-line build. The project is currently open in Xcode and should appear in Recent Projects. Xcode's `xcodebuild -list` recognizes the target and scheme, and `xcodebuild -project Odyssey.xcodeproj -scheme "Odyssey Native" -configuration Release -derivedDataPath Odyssey_build/DerivedData build` succeeded on 21 October 2026; its ignored log is `Odyssey_build/xcode-project-build.log`. The source tree is generated by `scripts/generate_xcode_project.py`; rerun it when adding source files. The Run button is not a playable game launcher at this stage.

## Current milestone: native boot reaches `nnMain`

`scripts/inspect_input.py` reads XCI/NSP container tables and encrypted NCA headers through the repository's existing parser. It prints only the container kind, NCA counts, and title IDs. It neither extracts nor copies game data, and it never prints key values.

Use a Python environment with `cryptography` installed:

```sh
python3 games/odyssey/scripts/inspect_input.py /path/to/odyssey.xci --keys /path/to/prod.keys
```

The base title ID is `0100000000010000`; the update title ID is `0100000000010800`. The local XCI contains both base and update Program NCAs. Both ExeFS sections were extracted and their FS headers, hash tables and every data block were verified. The update files are `main` (9,894,790 bytes), `main.npdm` (1,508), `rtld` (7,435), `sdk` (5,310,646) and `subsdk0` (3,557,941). Update main NSO build ID begins `b424be150a8e7d78701cbe7a439d9ebf`. `scripts/inspect_version.py` verified the two Control RomFS images' FS headers and all six IVFC levels and read their NACP display versions: base **1.0.0**, update **1.3.0**. The report is in ignored `Odyssey_build/analysis/versions.json`.

`scripts/extract_code.py` performs this local extraction. Pass `--program base` to inspect the base executable. Output must go into an ignored build directory; do not share extracted files or keys.

`scripts/survey_modules.py` verified the update NSO segment hashes, decompressed them, and generated local analysis ELFs and import inventories. These ELFs are not macOS applications. Main has 702 imports, 690 with matching exports in the other modules. The 12 without matches are four linker markers and eight `glslc*` shader compiler functions. Main also imports `nvnBootstrapLoader` and `nv::InitializeGraphics`; additional NVN functions are loaded by name at runtime. A static scan finds 462 NVN-looking string candidates in main, 382 with matching registration macros in the existing Metal layer and 80 without. String matches are only candidates, not proof that Odyssey calls those functions. SDK, account, input, audio, filesystem, graphics and online-related imports are present, but imports do not prove runtime use.

`scripts/audit_graphics.py` narrows that scan to NUL-terminated names and separates the `nvnBootstrapLoader` import from the 461 NVN API candidates. All 461 are in the copied host's lookup table; 391 have a registered implementation macro and 70 currently route to an explicit unimplemented stop if called. The eight `glslc*` imports have weak binding in main. The local list of 70 is in ignored `Odyssey_build/analysis/graphics-audit.json`; it is a work queue for runtime traces, not evidence that all 70 need implementing.

`scripts/survey_shader_assets.py` reads the verified RomFS and inventories the 28 `ShaderData/*.szs` files without exporting shader bytes. All are Yaz0 compressed SARC archives. Across them it found 22 `.sharc`, 22 `.sharcb`, and 7 `.bfsha` entries; one `.bfsha` is an 86.8 MB material archive. The metadata-only report is in ignored `Odyssey_build/analysis/shader-assets.json`. The copied renderer's assumptions about shader control sections still need runtime validation.

`scripts/inventory_shader_keys.py` reused the reference project's `.sharcb` and embedded BNSH extractors. Across those 28 archives it found **19,439 unique shader programs**: 3,041 vertex, 16,373 fragment, and 25 compute. Their hashes and archive provenance are in ignored `Odyssey_build/analysis/shader-keys.json`; this is a static inventory, not a runtime shader trace. `scripts/probe_shader_translation.py` checked the pinned NativeAOT translator and used SPIRV-Cross at the reference commit `aa217aeb6c9f0ace7a0ab233b28807edf45eb165`. Five sampled Odyssey shaders translated successfully to MSL plus reflection: `.sharcb` vertex and fragment, `.sharcb` compute (with thread-group size from its control section), `.bfsha` vertex, and a material fragment shader from the 86.8 MB archive. The last generated 10.5 MB of MSL, so Metal compilation cost needs measuring. Reports and generated MSL stay in ignored `Odyssey_build/analysis/shader-probe/`. The selected Command Line Tools do not include the `metal` compiler; Xcode 27.1's optional Metal Toolchain component is being downloaded. Later bounded runs did compile and render translated shaders through Metal's runtime compiler, as documented above.

The 19,439 count covers only `ShaderData/`, not every archive in the game. Following the [SuperMarioWonderRecomp shader inventory method](https://github.com/BelmanteGu/SuperMarioWonderRecomp/blob/main/docs/nvn/SHADERS.md) and Breath of the Wild's nested archive handling, `scripts/inventory_extra_shader_keys.py` parsed verified `EffectData/`, `LayoutData/` and `SystemData/` SZS payloads. It found shaders in 2 effect, 8 layout and 1 nested system archive: 25,105 stage instances, 8,858 unique keys, 8,845 absent from the `ShaderData/` inventory. Thus the observed subtotal is **28,284 unique stage keys across these four directories**. `ObjectData/` and other directories remain unscanned; do not present this as a whole-game total. The ignored `Odyssey_build/analysis/extra-shader-keys.json` stores only keys, stages and archive provenance, never shader bytes. The reference parser validates BNSH bounds; exact agreement with Odyssey's runtime shader-key transformations still needs comparison against live keys before bulk precompilation.

Bayonetta 3's [work log](https://github.com/ly05010419/MacNativeGames/blob/main/games/by3/CHANGELOG.md) explicitly lists shader stutter as open because it has no shared shader list or startup seed. The more complete [TotK/BotW shader-list design](https://github.com/ly05010419/MacNativeGames/blob/main/docs/shader-list.md) keeps identifiers and bind-state dependencies, distinguishes title shaders from background warm-up, and prebuilds observed pipeline keys. [Apple's Metal 4 compilation guidance](https://developer.apple.com/documentation/metal/using-the-metal-4-compilation-api) offers binary archives and pipeline harvesting as a later path to reduce remaining pipeline creation stalls. Apply those only after measuring exact Odyssey pipeline keys and validating cache identity for the local version; the current 447 cached variants are just those encountered in tests.

The Bayonetta rewriter generated patched images for `main`, `sdk` and `subsdk0` using `DM_RULESET=native DM_PRIVATE_GATES=0`; no game-specific cuts were applied. Independent LLVM structural verification passed: `main` 1,893 sites, `sdk` 11,449, `subsdk0` 11,093; zero violations. Unicorn equivalence passed all three modules outside the sandbox: `main` 1,893 behavior sites, `sdk` 11,357, `subsdk0` 11,093; zero differences. Each module caught 100/100 mutation controls. The sandbox blocks Unicorn's executable-memory mapping, so run that test with the approved unsandboxed tool setting. The host runtime must supply `glslc*` functions or establish how Odyssey obtains them.

The local Odyssey Python environment has `cryptography`, `lz4==4.4.5`, `capstone==5.0.6` and `unicorn==2.1.4`. Homebrew LLVM 23, CMake, opus and libvpx are installed. Generated images and logs are under ignored `Odyssey_build/`. The rewriter and checkers come from `games/by3/third_party/d2r`. Earlier attempts to rebuild Unicorn were unnecessary; the installed wheel works outside the sandbox. Their files remain ignored under `Odyssey_build/tools/`.

`runtime/` and `third_party/d2r_gfx/` are local copies of the Bayonetta host. `runtime/game_version.h` uses Odyssey's title ID and the independently verified update display version 1.3.0; `runtime/game_patches.c` has no game-specific patches. `scripts/prepare_runtime.py` pins the local executable/rewrite and merged RomFS hashes from `assets/expected-local-update.json` and generates loader tables for three modules and 226,799 relocations, plus the RomFS runtime index. `headless/headless_stubs.c` replaces the missing Metal and window functions; it is compiled only into the headless target. Do not use that target as a graphics or playability result.

The headless probe passed: all three modules mapped and relocated; a native ARM64 call from Odyssey `main` through SDK to a Mac timer returned 4,000 times across four threads. `scripts/merge_romfs.py` merged the direct AES-CTR base with the BKTR update from the combined XCI. Both FS headers and every block of all six update IVFC levels passed, including 346,568 data blocks. The 5,678,168,236-byte image has SHA-256 `6bb432a4032512f124485aaa5d08f5902b5d5da5027f43968cfc67cff5a97f1d` and 4,972 indexed files. The runtime independently checked that image's SHA-256 before mounting it.

The bounded headless boot passed SDK initialization, mounted RomFS, ran `main`, `subsdk0`, and `sdk` module initializer loops, and entered Odyssey's original `nnMain`. The next failure is an unimplemented Switch `nvdrv` IPC service request. Its guest stack points from Odyssey `main+0x4e32ec` through `main+0x4bf32c` and `main+0x94132c` into SDK graphics initialization. The full graphics build currently fails because the installed command line SDK reports macOS SDK 15.5 and lacks Metal 4 types such as `MTL4CommandBuffer`. A current Apple Command Line Tools package with macOS 26 SDK may provide the headers; full Xcode is not the only possible source of that SDK. Metal shader tooling may still require additional Xcode components.

### Full Metal host startup — 21 October 2026

After the user accepted the Xcode license, `DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer make -j8 runtime` built `Odyssey_build/build/odyssey-native` successfully with macOS SDK 27.0; the complete build log is ignored at `Odyssey_build/build-full.log`. A bounded full-host check with `TOTK_HEADLESS=1` confirmed that Odyssey's graphics setup selected the Apple M3 Metal device and the host override bypassed `nvdrv`; it stopped at `VI requires native macOS window`, as expected for headless mode (`boot-full.log`).

`runtime/loader.c` now lets `TOTK_VERIFY_STARTUP=1` use the native frontend when `TOTK_HEADLESS` is absent. A bounded windowed check with `TOTK_VERIFY_STARTUP=1` and `TOTK_GPU_ALLOW_SUBMIT=0` initialized the native Metal context and VI display, then crashed at guest `main+0x980718` (read from address `0x30`), before the startup checkpoint or a rendered frame. Trace: ignored `Odyssey_build/boot-window-check.log`. The instruction is `ldr x9, [x8,#0x30]`; `x8` came from a null guest global loaded via relocation at `main+0x1366220` (RELATIVE addend `0x14ae7e8`). The nearby `main+0x9806e0` routine constructs an object, then reads that global. A static cross-reference scan found about 80 main-code reads of the relocation slot at `main+0x1366220`; the pointed-to address `main+0x14ae7e8` is in guest BSS. This may be a graphics singleton left uninitialized because the current `nv::InitializeGraphics` host override only calls `mtl_init`, but that is an inference, not yet established. Identify which initializer normally populates it and whether the host override must reproduce a guest-side effect. Do not patch the read speculatively. The windowed process exited 139; no GPU work was submitted. To reproduce the bounded window check from this folder, launch `Odyssey_build/build/odyssey-native rewritten/local boot` with `TOTK_VERIFY_STARTUP=1`, `TOTK_GPU_ALLOW_SUBMIT=0`, no `TOTK_HEADLESS`, and a 45-second subprocess timeout, using `Odyssey_build/` as the working directory. Native guest memory mapping requires execution outside the tool sandbox. The copied renderer disables GPU submission for full-game runs by default because its upstream authors recorded driver panics in prior games. Do not enable `TOTK_GPU_ALLOW_SUBMIT=1` just to bypass this startup issue.

### Native startup progress after the initial Metal window — 21 October 2026

The null guest global at `main+0x980718` came from Odyssey rejecting the copied TotK NVN version. Odyssey 1.3.0 requires NVN major 53 and minor at least 311 (checks at `main+0x9414c4` and `main+0x9414d0`). `Makefile` now defines `GAME_ODYSSEY`; `third_party/d2r_gfx/gfx/nvn/hle_nvn.c` advertises 53.311 and enables the existing queue texture acquisition for Odyssey. Bounded startup now initializes Apple M3 Metal, a 1600x900 NVN window, and acquires the first swapchain texture.

`runtime/account.c` supplies Odyssey's `OpenPreselectedUser` and `GetLastOpenedUser` from the existing persistent local account. `runtime/input.c` handles the console seven-axis sensor as absent (zero samples), initializes the optional debug pad as absent, and accepts dual Joy-Con assignment for the native full-key controller. Its `GetDebugPadState` writes the 0x20-byte state size verified from SDK disassembly. Normal controller six-axis motion remains in `runtime/hid_feedback.c`. The last bounded boot mounted the local save and reached vibration device initialization, then stopped at an unimplemented `friend:u` request; see ignored `Odyssey_build/boot-debug-state-check.log`. Earlier checkpoints: `boot-seven-axis-check.log`, `boot-debug-pad-check.log`, and `boot-joy-dual-check.log`.

`runtime/system_services.c` now routes optional friends and broadcast delivery-cache initialization to the offline host. The next bounded test reached `fsp-srv` through `nn::bcat::MountDeliveryCacheStorage`; the host now returns an empty local cache mount and unmount, pending a fresh boot check. Latest completed trace: ignored `Odyssey_build/boot-bcat-init-check.log`. These are startup shims, not working network friends or broadcast delivery.

The local `Firmware.22.5.0.zip` in Downloads contained all five required shared-font data NCAs. The existing Bayonetta extractor verified each NCA filesystem header, all IVFC levels and six BFTTF font headers, writing only to ignored `Odyssey_build/extracted/firmware/shared-fonts/`. `scripts/prepare_runtime.py` generated six pinned font rows; the runtime loaded 17,438,108 bytes. The next bounded run (`boot-fonts-check.log`) crashed in guest `main+0x8dea04` while reading a null pointer at offset 0x38. The preceding call `main+0x8e16dc` returned null after font initialization. Investigate the guest font lookup and why it fails; do not assume the firmware fonts are invalid. No frame rendered.

The font crash was caused by a host layout bug: the BFTTF payload is XOR encoded, while `nn::pl::GetSharedFontAddress` must expose decoded sfnt bytes. `runtime/shared_font.c` now verifies each source file hash, decodes the payload in memory, and checks the sfnt magic. `boot-fonts-decoded-check.log` passed the configured startup checkpoint: fonts loaded, CoreAudio opened at 48 kHz, the audio renderer started, and Odyssey continued to its focus setup. The checkpoint exits intentionally at frame 0. Next, run a bounded native process without `TOTK_VERIFY_STARTUP` and with GPU submission still disabled to find the next blocker.

A bounded run beyond the startup checkpoint initially crashed inside SDK `nn::apm::GetPerformanceConfiguration` because the copied host did not override `nn::oe::GetPerformanceConfiguration`. `runtime/services.c` now returns a docked or handheld configuration and remembers game requests. The next run (`boot-performance-config-check.log`) progressed through shader-cache setup and launched the native shader translation service, then crashed at guest `main+0x954fc4` in a virtual call through an invalid pointer while traversing a guest list. This needs a writer/ABI investigation; it is not evidence of rendered output. GPU submission remained off.

The user approved short GPU submission tests after a warning about earlier IOGPUFamily kernel panics in the copied renderer. Bounded runs submitted and presented frames without a Mac crash. Window captures at frames 122–126, 447, 925, and after the depth change stayed solid magenta. A frame-193 dump of intermediate render target serial 30 captured the real Odyssey Joy-Con instruction screen (`../../../screenshots/odyssey-first-actual-game-image.png`); the swapchain target remained magenta. The final copy vertex shader was cached without the near-minus-w depth conversion, so the shader cache key now includes that mode and the native host rebuilds successfully. The next short monitored run regenerated the copy vertex shader and captured the real Odyssey instruction screen in the actual Mac window at frame 175; both game and whole-window captures are in `../../../screenshots/`. The process exited 0 without a Mac crash. A no-input control run reproduced the strap instruction panel exactly, so that transition is timed, not proof of input. `TOTK_INPUT_LOG=1` confirms the game reads full-key controller state for player 1 with supported styles `0x7`. A later no-input run reached the Start menu at frame 981, 19 seconds after launch; the transition was already visible at frame 740. Input tracing confirms the guest receives A, B, and Plus in player 1 full-key state. An A press at the Start menu advanced into the opening story sequence; frame 1206 displayed “In the skies above Peach’s castle…”, and the process exited 0. Next inspect cutscene frames, audio, and progression toward gameplay. `../../../TEST_EVIDENCE.md` indexes the logs and captures.

Early startup checks used `TOTK_VERIFY_STARTUP=1` and GPU submission off. Later approved, bounded GPU runs produced real game imagery in an intermediate texture but not yet in the visible window. The user requested screenshots or video of substantial visual improvements and documented test evidence. Keep GPU runs short and monitored because the copied renderer had prior macOS GPU driver panics.

### Handoff at `nvdrv`

- The headless target intentionally excludes `runtime/gfx_bridge.c` and the Metal NVN layer. Its graphics stub returns no override, so Odyssey calls the Switch SDK implementation of `nv::InitializeGraphics`, which requests `nvdrv`. Odyssey `main` has strong JUMP_SLOT relocations for `_ZN2nv18InitializeGraphicsEPvm` at `0x13604d0` and `nvnBootstrapLoader` at `0x1360510`. The loader checks host overrides before other modules; the full runtime registers replacements for both symbols in `runtime/gfx_bridge.c` and `third_party/d2r_gfx/gfx/nvn/hle_nvn.c`. The next useful check is a full renderer build with Metal 4 headers, then a bounded boot to see whether those overrides bypass `nvdrv` and which Odyssey NVN calls are actually used.
- This Mac has only `/Library/Developer/CommandLineTools` with macOS SDK 15.5. An Apple Command Line Tools 26 package with macOS 26 SDK is a smaller possible route to the missing headers; verify its Metal 4 coverage before rebuilding. Xcode 27.1 has since appeared at `/Applications/Xcode.app`, and its macOS 27 SDK contains Metal 4 headers including `MTL4CommandBuffer.h`. The App Store now shows Open, but `open /Applications/Xcode.app` returned OSStatus -10827 (`kLSNoExecutableErr`) despite the executable being present, so app registration may still be settling. `xcrun` with `DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer` now reports macOS SDK 27.0 and finds the Metal compiler after the user accepted Apple's license. Build `odyssey-native` and test a bounded boot.
- `Odyssey_build/boot.log` is the exact current stopping trace. Rebuild after any source edit and keep test logs under ignored `Odyssey_build/`. The verified merged RomFS and its `index.json` are in `Odyssey_build/extracted/local/romfs/`; the pin is in `assets/expected-local-update.json`. The local Python environment, source XCI, and key file already exist on this Mac. Keep keys and game data out of Git and messages.
- The copied Bayonetta runtime still uses `TOTK_*` environment names and some TotK-specific assumptions. Its NVN present path previously enabled TotK-specific uncapped pacing by default; Odyssey now keeps the game's own pacing unless `TOTK_UNCAPPED=1` is explicitly set. The shared NVN device version table still contains a TotK-specific major/minor override and needs runtime validation. Audit actual Odyssey behavior before adapting or claiming each service works. No rendering, gameplay, input, audio, or save behavior has been verified.

### Continuation commands

From `games/odyssey/`, regenerate loader tables with `.venv/bin/python scripts/prepare_runtime.py`, build the headless target with `make Odyssey_build/build/odyssey-headless`, then run `cd Odyssey_build && ./build/odyssey-headless rewritten/local probe`. To repeat the bounded startup, run `TOTK_VERIFY_STARTUP=1 TOTK_HEADLESS=1 ./build/odyssey-headless rewritten/local boot` from `Odyssey_build/`. Native execution needs executable-memory access outside the tool sandbox. `Odyssey_build/probe.log` and `Odyssey_build/boot.log` hold the latest output. The build's generated headers, extracted modules, rewrites, RomFS, and local tools are ignored by Git. The symlinks `Odyssey_build/rewritten/local` and `Odyssey_build/extracted/local/exefs` route the generic table generator to local output. To regenerate RomFS from the user's local XCI and key file, run `scripts/merge_romfs.py GAME.xci --keys prod.keys --output Odyssey_build/extracted/local/romfs` in the Odyssey Python environment; the output folder must not already exist.

The pinned SPIRV-Cross checkout is in ignored `Odyssey_build/src/SPIRV-Cross/`; `make -j8 shader-tools` verifies and installs the reference native translator and compiles SPIRV-Cross. The full `runtime` target depends on these tools because its graphics layer translates shaders on demand. Check that the checkout still resolves to `aa217aeb6c9f0ace7a0ab233b28807edf45eb165` before rebuilding. To repeat a translation sample, run `.venv/bin/python scripts/probe_shader_translation.py --spirv-cross Odyssey_build/build/spirv-cross/spirv-cross --stage ps` from `games/odyssey/`. Use `--archive ShaderData/alComputeShader.szs --stage cs` for the compute sample and `--archive ShaderData/alPostEffectMask.szs --stage vs` for an embedded `.bfsha` sample. The probe writes only to ignored `Odyssey_build/analysis/shader-probe/`.

## Port milestones

1. **Input survey:** inventory the base and update, identify versions, verify ExeFS and RomFS integrity, and record module sizes and SDK/NVN imports. No game material is committed.
2. **Native code:** adapt the existing ARM64 NSO rewriter for Odyssey's modules and verify each rewrite site. Keep all Odyssey addresses, patches and checks in this directory.
3. **Host runtime:** fork the minimum reusable loader, Horizon services, threads, audio and filesystem code; remove Zelda-specific addresses and behavior. Reach Odyssey's `nnMain` with explicit errors for missing services.
4. **Graphics and playability:** adapt NVN-to-Metal calls and shader translation, then add input, sound, saves and an app bundle. Validate each milestone in the actual game rather than assuming another game's behavior.

The [OdysseyDecomp project](https://github.com/MonsterDruide1/OdysseyDecomp) is a reference for understanding Odyssey, but its build is not a playable port. The [smo-recomp project](https://github.com/coz-0/smo-recomp) is a Windows x64 runtime and may be useful as a behavioral reference; its code and license must be reviewed before reuse.

## Current blockers

- The opening cutscene has severe first-use asset and shader compilation pauses. Some 300–470 ms CPU stalls remain unattributed.
- Later game scenes and controllable play have not been verified. Visual and audio fidelity need comparison against the user's Switch output.
- The copied renderer has caused macOS GPU driver kernel panics in earlier full-game runs. Short monitored tests later exited normally; longer-run stability remains unknown.
- The copied host still needs an Odyssey-specific audit for game services, controls, and saves. `prod.keys` stays outside this repository and is used only in memory during local inspection.
