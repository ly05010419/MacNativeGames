# Install and play

[← README](../README.md) · [中文](install.zh-CN.md)

## Audio fix

Streamed music and dialogue lookup for `content://Sound/...` paths has been fixed. Opening dialogue and music were verified locally on version 1.0.0; other versions and scenes still need validation. The renderer also runs all known audio effect types and starts output after 20 ms of queued audio (2026-10-18). Rebuild your app with the original game inputs to apply the fix. See [audio troubleshooting and validation](audio-troubleshooting.md) for details.

## Local testing

Tested on **MacBook Air M4 and Mac mini M4**. Local testing has collected **8,582 shaders**, with performance at approximately **90 FPS after shader warm-up, reaching 120 FPS in menus**.

Those are earlier local observations, not a stable frame-rate promise for every version or scene. Version 1.4.3 measurements and compilation stalls are recorded separately in [the test notes](v1.4.3-port.md).

Shaders are automatically collected, compiled and cached as you play. Returning to previously visited scenes typically brings noticeable improvements in frame rates and smoothness because cached shaders can be reused. **8,582 is the number collected so far in local testing, not the game's total shader count or a collection limit**; the cache continues to grow as you explore.

**Shader prewarming on the first launch:** Building the app prepares shaders from your selected game data and includes them in the app: every shader in the game data for its default bind state, plus every variant named in the [shared shader list](shader-list.md). Even with an empty user cache, the first launch imports these shaders. The title appears once all of them and every compatible entry of the pipeline manifest are warmed and the pipelines are in the pipeline archive (the start screen shows the progress): after an install or a larger shared list that takes a while once (Tears of the Kingdom 1.4.3 on an M4 about 90 seconds, Breath of the Wild 1.6.0 about 50), later starts about 10 seconds, so play starts with every listed shader and pipeline ready. `TOTK_WARM_BEFORE_TITLE=0` brings back the old order: only what the title screen uses before the first frame, the rest behind the title, and the first trip into the world waits for it (on the Mac until it is done, on an iPhone or iPad up to 15 seconds). No preliminary play session or manual cache copy is needed. The current 1.4.3 build prepares 15,266. Building the app also compiles them for your Mac's GPU. With Xcode's Metal toolchain installed they are compiled into the app (about 6 minutes on an M4 for 1.4.3) and work wherever the app goes. Without it they are compiled into macOS's Metal cache (a few minutes), and Metal finds them only while the game runs from where the app was built: if you move the app afterwards (to /Applications, for example), if macOS clears the cache (it does when the system disk fills up), or if another program was using Metal's cache while the app was built (a running game, for example; the build says so), the background warm-up compiles them again, which takes a few minutes, and new scenes can stutter until it finishes. To place the app elsewhere, build it there with `-o`; see [preparation and test details](first-launch-shaders.md). Places no player has reported yet can still need new variants, which the Mac compiles while you play (a short stutter the first time). Playing adds them to your local cache, which is reused on later launches. The game also notes every variant the shared list does not have yet in `shader-list-contribution.txt`, and every 500 of them asks once whether to send the file in a [GitHub issue](https://github.com/ly05010419/MacNativeGames/issues/new?template=shader-list.yml) ("Send" opens the form and shows the file to drag in). It never uploads anything by itself. With git, you can also merge your cache and open a pull request, as [the list's guide](shader-list.md) explains. The list holds shader and pipeline identifiers only, never game data, saves or compiled shaders; every build makes the shaders from the player's own game files.


## What you need

- A Mac with Apple silicon and **macOS 26** or later (the graphics use Metal 4); macOS 27 is supported. 16 GB of memory or more is recommended.
  - Still on macOS 15? A community fork by [1918662410-ship-it](https://github.com/1918662410-ship-it/MacNativeGames) adds a Metal 3 backend for macOS 15. It is maintained there; this repository does not test it.
- About **25 GB** of free disk space for the work folder, in addition to your game file. Keep the output app on the **same APFS volume** to share the large data files through cloning. If cloning is unavailable or the app is on another volume, allow roughly **17 GB more** for the app copy.
- Your Tears of the Kingdom **base game 1.0.0** (`0100F2C0115B6000`) as an **XCI or NSP**, and your `prod.keys`. To build **1.1.2, 1.2.1 or 1.4.3**, supply its separate cumulative update as NSP or NSZ with `--update`; no intermediate update is required. NSZ is decompressed and its NCA hashes are verified before extraction. A card image that also holds the update it shipped with is accepted: its base game is used, and the version still comes from `--update`. DLC and other combined packages are unsupported.
- For NSP: its common ticket must be included unless the NCAs are rights-free. NSPs do not contain system shared fonts: supply an unpacked firmware NCA directory with `--firmware`, or previously extracted fonts with `--shared-fonts`. The six fonts are checked against this project’s reference dump; a valid BFTTF font from another firmware version is accepted and bound by its own bytes. Valid fonts already in the work folder are reused.

## Build the app

**1. Tools** (once): Xcode command line tools and [Homebrew](https://brew.sh). An internet connection is needed to install build dependencies and fetch pinned sources:

```sh
xcode-select --install
brew install opus libvpx python@3.14
```

Alternatively, use [uv](https://docs.astral.sh/uv/) to provide Python 3.14. Install the native dependencies with `brew install opus libvpx`, then use `uv run --python 3.14 build_app.py` in place of `python3.14 build_app.py` in the commands below. For example, from the repository folder:

```sh
uv run --python 3.14 build_app.py ~/Games/totk.xci -k ~/keys/prod.keys
```

The build finds Python alongside the interpreter selected by uv, so a Homebrew Python installation is not required. The build still creates its own virtual environment and installs its dependencies there.

For an iPhone/iPad build, install full **Xcode 26 or later** (including the iOS and Metal toolchains), then install the additional tools:

```sh
brew install xcodegen cmake ninja
```

Optional: with Xcode's Metal toolchain (`xcodebuild -downloadComponent MetalToolchain`), the build compiles the startup shaders into the app. It finds an Xcode kept outside `/Applications` too (on another volume, or set `DEVELOPER_DIR`). Without it, the build compiles them into the Mac's own Metal cache for the app's location instead, which does not move with the app: after moving it, the first launch compiles them again.

**2. Download or clone this repository**, then open Terminal in its `MacNativeGames` folder and run the script with your game file and keys:

```sh
python3.14 build_app.py ~/Games/totk.xci -k ~/keys/prod.keys
```

For an NSP, supply the shared fonts too:

```sh
python3.14 build_app.py ~/Games/totk.nsp -k ~/keys/prod.keys --firmware ~/Games/firmware
# Or reuse the six .bfttf files from a previous build:
python3.14 build_app.py ~/Games/totk.nsp -k ~/keys/prod.keys --shared-fonts /path/to/TOTK_build/totk/extracted/firmware/shared-fonts
```

`--firmware` accepts a directory of `.nca` files (unpack a firmware archive first), or an installed firmware `registered` directory. `--shared-fonts` accepts the extracted directory containing `0-standard.bfttf` through `5-nintendo-extension.bfttf`. No fonts or keys are downloaded. For an XCI, fonts are read from its system update partition by default; if that partition is missing, use one of these font options too. By default both input formats produce a macOS `.app`.

To build an unsigned iPhone/iPad IPA instead, add `--platform ios` and use an `.ipa` output path:

```sh
python3.14 build_app.py ~/Games/totk.xci -k ~/keys/prod.keys --platform ios -o ~/Games/TOTK.ipa
```

Add `--update ~/Games/1.4.3.nsp` to select an update, and keep the `--firmware` or `--shared-fonts` option when the base game is an NSP. The first iOS build downloads and compiles device versions of Opus and libvpx and compiles the shader seed for iOS, so it needs an internet connection and substantial temporary disk space (allow at least **45 GB free** for a fresh base build, or **60 GB** for a fresh build with an update). The resulting IPA is unsigned: sign and install it with your sideloading tool, keeping its entitlements (it uses the increased memory limit). Or let Xcode sign it with your own Apple ID (Xcode › Settings › Accounts; a free Personal Team works) and install it on the iPhone or iPad connected by cable: `python3.14 games/totk/ios/tools/install_device.py ~/Games/TOTK.ipa --device "<device name>"`. A free Apple ID signs for 7 days, a paid membership for a year; run the same command again to renew, which keeps your saves. iOS/iPadOS 26 and iPhone 14 or newer are required. See [iPhone and iPad](#iphone-and-ipad) for what has been tested.

Without `-o`, the selected output appears next to the game file (`Tears of the Kingdom.app` on Mac or `Tears of the Kingdom.ipa` for iOS). Before full shader preparation was added, a local clean base-1.0.0 Mac build took about 6 minutes on an M4 Mac mini (this is not a timing estimate for the current build; hardware, disk and network conditions affect the time): about a minute each to rewrite the main code module and to decrypt the 17 GB of game data, and two to three minutes for macOS to check the finished app once. The current build then also prepares the startup shaders (about 3 minutes for 1.4.3 on an M4) and compiles every prepared shader for your GPU (about 12 minutes for 1.4.3 on an M4 Max, longer on an 8 GB M1 MacBook Air, which runs out of memory and swaps during this step; without the Metal toolchain this step is skipped, with a message, while a game is running). A Mac build needs no .NET or CMake, and the finished game needs no Python: its shader translator is a native program that comes with this repository (`games/totk/prebuilt`). Every intermediate file is in a work folder next to the game file (`TOTK_build`, or `--work`); running the script again reuses what is already done.

The extracted code modules, rewritten modules, RomFS and shared fonts are checked against the selected version's `games/totk/assets/expected-v1.0.0.json`, `expected-v1.1.2.json`, `expected-v1.2.1.json` or `expected-v1.4.3.json`; the prebuilt shader translator has its own SHA-256 check. A mismatch stops the build, with one exception: a different genuine retail dump of the same version is accepted — its `main.npdm` is checked by Nintendo’s ACID signature (`npdm_signed()`) instead of the pinned hash, and its firmware’s shared fonts are checked for a valid BFTTF structure and bound by their actual SHA-256. Key values are never printed.

Signing and verifying a large updated app can add several minutes. Keep the build running while it continues to report progress.

**3. Play:** open the app from Finder. The build starts the launcher in `--check` mode, which checks that the game executable exists and allows macOS to perform its first-launch checks; it does not run a gameplay test. You can keep the app where it was built or move it to `/Applications`; the first start after a move compiles the shaders again in the background (macOS's Metal cache does not follow the app to its new place), so to keep it in `/Applications`, build it there with `-o`. Copying it to a different volume can use additional disk space and trigger another macOS check, during which no window may appear. The first game launch verifies the game data (about 20 seconds in local testing). Newly encountered shaders can cause stutters; cached shaders reduce this work on later visits.


### Update the runtime without the game files

After pulling a newer version of this repository, you do not have to build the app again from your game file. The app has two parts: the **runtime**, which this repository makes on its own (the game binary with the loader, system services and graphics; the launcher; the shader tools; the warm-up lists from the shared shader list), and the **game files** made from your copy (the rewritten code, the 17 GB RomFS, the shared fonts, the startup shaders and a small game data folder with the tables and hashes the runtime reads at start). `update_runtime.py` builds the runtime and puts it into the app in place of the old one. It reads no game file or keys and leaves the game files as they are:

```sh
python3.14 update_runtime.py "~/Games/Tears of the Kingdom.app"
```

It took 46 seconds for a 1.4.3 app on an M4 Mac. Quit the game first, or the script waits until it closes. An app built before this split has no game data folder yet: the script makes the folder once from the work folder of the build that made the app (`TOTK_build` next to the app, or `--work <folder>`). Whatever the source, the folder is checked against the app's own code modules, RomFS index and fonts before anything is replaced. When a new runtime changes the shader translation chain, the app's startup shaders no longer fit it and the script stops; add `--seed` to make them again from the work folder's RomFS and modules (a few minutes, still without the game file or keys; the folder is checked to hold the app's files). The game's code modules are kept too, so they must be what this repository's code rewriter (`patch_nso.py`) makes: the rewrites it pins for the version, or, for a build with a mod's code patches, modules made by the same `patch_nso.py`. Otherwise the script also stops, because a runtime can rely on what the rewriter puts into the code, and the modules must be rewritten with `build_app.py`. A new game version or a mod's code patches need `build_app.py` as well. Before anything is replaced, the new runtime is run against the app's own game files, and if the updated app then fails its start check, the previous one is put back. The runtime holds nothing from the game, so `make -C games/totk binary-v143` (or `binary`, `binary-v112`, …) builds it with no game files at all.


### Build version 1.1.2, 1.2.1 or 1.4.3

```sh
python3.14 build_app.py ~/Games/totk.xci -k ~/keys/prod.keys --update ~/Games/1.1.2.nsp
# NSZ updates are also accepted:
python3.14 build_app.py ~/Games/totk.xci -k ~/keys/prod.keys --update ~/Games/1.1.2.nsz
# Or build 1.2.1 or 1.4.3 directly from the same base:
python3.14 build_app.py ~/Games/totk.xci -k ~/keys/prod.keys --update ~/Games/1.2.1.nsp
python3.14 build_app.py ~/Games/totk.xci -k ~/keys/prod.keys --update ~/Games/1.4.3.nsz
# With a base NSP, keep --firmware or --shared-fonts as described above.
```

This produces the same **Tears of the Kingdom.app**, containing only the selected version. Quit the running game, then rebuild to the same `-o` path to replace the old app; there is no second game entry or version-selection menu. The window title shows the selected version, such as **Ver. 1.4.3**. Without `--update`, the script still builds 1.0.0.

You do not need to download separate 1.2.1 or 1.4.3 runtime tables or a shader cache. The build generates the version-specific runtime tables and first-launch shader seed from the game data you provide, then includes the shader seed in the app. The repository supplies the patch code and verified hashes of each supported update; only your base game, update and keys are external inputs.

Allow about **40 GB free for a fresh update build**, or **19 GB extra** when the base data is already cached, with the app on the same APFS volume. The merge reads the base NCA through a bounded range of the original container, temporarily extracts the much smaller update NCA, and verifies every IVFC level. It fetches a pinned [hactool](https://github.com/SciresM/hactool) source and builds it with clang/make. Temporary ticket files are private and removed after the merge; key values are not passed on the command line or printed.

If replacing a Mac app fails while it is signed or moved, the build puts the installed app and its reused game data back before it reports the error (if that fails too, the error names the folders to recover).

Saves stay in the same profile folder. Before the first updated launch, the app makes a complete backup beside it, such as `profile-before-1.1.2`, `profile-before-1.2.1` or `profile-before-1.4.3`; failure stops startup. Keep this backup if you need to return to an older version—do not assume an updated save is backward compatible.

### Build with a mod's code patches

```sh
python3.14 build_app.py ~/Games/totk.xci -k ~/keys/prod.keys --update ~/Games/1.4.3.nsp \
  --mod "/path/to/The Second Quest 1.4.3" --install-mod-romfs
```

`--mod` takes the mod folder as it comes, `exefs/` and `romfs/` together (either half alone works too). Why a build
flag at all: the app has no module loader - at start it maps the three code modules it was built from (main, sdk,
subsdk0), rewritten for Apple silicon and bound by SHA-256, and nothing in it reads a mod's `exefs/` the way Ryujinx or
a Switch does. So the code side goes in one step earlier: `--mod` merges the mod's main-module patches into the verified
retail module before it is rewritten.

The data side is a separate choice, because the version's mod folder is yours: `--install-mod-romfs` says the build may
replace it. With the flag, at the end of the build the folder
`~/Library/Application Support/MacNativeGames/TOTK/mods/<version>` is replaced by the mod's `romfs/` with a fresh
manifest written against the app just built (`make_mod_manifest.py`) - one command, an app that plays with the mod as it
stands. What the folder held is not deleted: it is moved next to it as `<version>.backup-<date>-<time>` (the game never
loads it), and the build says so when it starts and names the backup at the end, so add the flag only when that is what
you want; without it the build leaves the folder alone and prints the two commands that fill
it by hand. Several mods with code patches: merge them in the mod manager first (TKMM's output is one folder with one
`exefs/` and one `romfs/`), then build with `--mod <that folder> --install-mod-romfs`.
The patches are applied to the verified retail main of the version
being built - a `.pchtxt` as the game's own patch loader reads it, a `.ips` / `.ips32` with the emulator offset
convention - after checking that no edit lands on a site the app patches itself, that no two patches change the same
bytes and that every edit is inside main's own segments. The merged module is then rewritten for Apple silicon and the
app is bound to its SHA-256. `analysis/phase12-validation.json` (1.0.0) or `analysis/<version>-port/phase12-validation.json`
in `--work` records the mod folder, each patch file's SHA-256 and the addresses it changed, under `mod_exefs`. An extra
code module a mod ships (`subsdk9`, a hooking framework's) is not loaded and the build says so: such a mod keeps its
main patches and its RomFS, but not the code changes that live in that module. Step by step: [docs/mods.md](mods.md); a manager's merged `exefs/` + `romfs/` folder in practice: [docs/merged-mod-pack.md](merged-mod-pack.md).

To keep one desktop entry, link the packaged app with the optional installer:

```sh
python3.14 games/totk/tools/install_desktop_entry.py --app "/path/to/Tears of the Kingdom.app"
```

When replacing the older development launcher, the installer copies and verifies its saves into the standard user profile listed below and creates an upgrade backup. It preserves the original profile and archives the old launcher without adding another app entry. An existing, different standard profile is never overwritten.

The source runtime passed basic Mac checks: load, movement/jumps, menus/map, 2K, save/reload and death retry. Full-map, long-session and fair 1.0/1.1.2 performance comparisons remain pending; the earlier FPS observations are not a promise of 1.1.2 performance. Packaging validation is recorded in [the build notes](how-it-works.md#112-validation).

The 1.4.3 port has separately passed scene loading, movement/jumps, menus/map, save/reload, native 2K canvas switching and MetalFX upscaling/interpolation on an M4 Mac. The single packaged app has been tested, including save/reload, death/retry and shrine entry/exit. A three-hour earlier build and a 30-minute final-build run are recorded separately; first-use compilation stalls, long-session memory growth and audio underruns remain under investigation. See [the 1.4.3 test record](v1.4.3-port.md) for the scope and follow-ups; this is not full-game or stable-FPS certification.

The 1.2.1 port (October 19) passed the same rewrite verification (600 of 600 negative controls) and the loader, RomFS and startup probes. On an M4 Mac it loaded saves in the 1.0.0 and 1.1.2 formats and passed movement/jumps, menus/map/quick menus, manual save and reload in a new process, fall death and retry, and the three key-3 picture modes; the packaged app was also opened through its launcher. See [the 1.2.1 test record](v1.2.1-port.md); this is not full-game or long-session certification.

### Breath of the Wild 1.6.0 and 1.9.0 (early)

The same script builds **The Legend of Zelda: Breath of the Wild 1.6.0** (`01007EF00011E000`) from a card image that holds the base game, the 1.6.0 update and the two add-on content titles together (no `--update`; the game is recognised by the title IDs in the file, never by its name). This card has no system update partition, so the six shared fonts come from an unpacked firmware (`--firmware`, as for a Tears of the Kingdom NSP), a Tears of the Kingdom build's work folder (`--shared-fonts TOTK_build/totk/extracted/firmware/shared-fonts`) or another verified font folder. The startup shaders come from the game data and the game's own [shared shader list](shader-list.md) (`games/botw/assets/shader_list.tsv`), as for Tears of the Kingdom: every shader in the game data for its default bind state (since October 23; before, only those of the boot and title packages), plus every bind state the list names, compiled ahead and warmed before the title.

```sh
python3.14 build_app.py ~/Games/botw.xci -k ~/keys/prod.keys --shared-fonts ~/Games/TOTK_build/totk/extracted/firmware/shared-fonts
```

A separate base NSP or XCI and a **1.6.0 update NSP or NSZ** also work. DLC is not required: add each add-on content NSP with `--dlc`, as for 1.9.0 below, and the app holds only the packs you give. Common tickets must be included for rights-managed NSPs. Other update versions are rejected by the verified executable hashes.

```sh
python3.14 build_app.py ~/Games/botw-base.nsp -k ~/keys/prod.keys --update ~/Games/botw-1.6.0.nsp --shared-fonts ~/Games/TOTK_build/totk/extracted/firmware/shared-fonts
```

The work folder is `BotW_build` next to the game file; the app is `Breath of the Wild.app` (bundle `org.macnativegames.botw`), with its own data folder `~/Library/Application Support/MacNativeGames/BotW` and log `~/Library/Logs/MacNativeGames/botw.log`. Every step is checked against `games/botw/assets/expected-v1.6.0.json`; a fresh build takes about 16 minutes on an M4 (the 15 GB RomFS merge is most of it).

**1.9.0** builds from your base game NSP and the 1.9.0 update NSP (`--update`) instead of a card image. Each of the two add-on content packs comes from its own NSP with `--dlc` (`01007EF00011F001`, `01007EF00011F002`); each pack is optional, and the app holds only the packs you give. An NSP holds no shared fonts, so add `--firmware` (or `--shared-fonts`), as for a Tears of the Kingdom NSP. A file given on the wrong option stops the build and names the option it belongs on.

```sh
python3.14 build_app.py ~/Games/botw-base.nsp -k ~/keys/prod.keys --update ~/Games/botw-1.9.0.nsp --dlc ~/Games/botw-dlc-1.nsp --dlc ~/Games/botw-dlc-2.nsp --firmware ~/Games/firmware
```

Both versions use the work folder `BotW_build` next to the game file and make `Breath of the Wild.app` (bundle `org.macnativegames.botw`), with its own data folder `~/Library/Application Support/MacNativeGames/BotW` and log `~/Library/Logs/MacNativeGames/botw.log`. Every step is checked against `games/botw/assets/expected-v1.6.0.json` or `expected-v1.9.0.json`; a fresh 1.6.0 build takes about 16 minutes on an M4 (the 15 GB RomFS merge is most of it).

What works on 1.6.0 (October 20, an M4 Mac, a Great Plateau save): title, the add-on content list, loading and saving, the plateau at about 90–120 fps (the game advances by real time; held-button repeats count by real time too), the two DLC packs mounted and read, key 1 (overlays), key 2 (MetalFX frame interpolation: off → 30+30 → half the refresh rate → unlimited, every game frame interpolated on the plateau) and key 3 (MetalFX 2K on / off: MetalFX replaces the game's own upscale of the 1600x900 picture into the 1080p canvas and the window output above 1080p); a 30-minute session on the plateau at about 95 fps with memory steady (+170 MB). Since October 23 (Mac): key 2 works again (its scene detection had kept a shader key from before an October 20 change and found no frame, #177) and gives what moves with the camera, such as Link, no camera motion, so he no longer shakes at 30+30; key 3's MetalFX again replaces the game's own upscale; key 4 switches 21:9 as on 1.9.0 (1.6.0's camera writes 16:9 every frame, so the game's own write is changed); key 5 smooths edges (#178). Key 3 now cycles off → MetalFX 2K → native 2K → off as on Tears of the Kingdom 1.4.3 (native 2K renders the scene and the canvas at 2560x1440; the app starts with a 2560x1440 swapchain and bigger heaps, `TOTK_OUTPUT_RES=0` keeps the shipped 1080p only). Not yet: the DLC quests themselves (only verified as mounted and read). This is an early port: expect rough edges.

What works on 1.9.0 (October 20, an M1 Pro Mac with its built-in 120 Hz display, both DLC packs): title, loading saves, autosaves, six places from the Shrine of Resurrection to the Hyrule Castle exit at 58.4–77.5 fps standing still and 58.6–81.4 fps walking with a warm shader cache, a 25-minute run in Korok Forest with Link moving, the game advancing by real time at any frame rate (held-button counts too), the two DLC packs (the Kakariko route starts the DLC quest "EX Treasure: Garb of Winds"), key 2 (MetalFX frame interpolation: off → 30+30 → half the refresh rate → unlimited; on the Great Plateau, standing still, the game ran at 69, 30, 58 and 61 fps and the screen showed about 67, 60, 115 and 115 pictures a second) and key 3 (MetalFX replaces the game's own upscale). Since October 21, 1.9.0 also has 1.6.0's fixes above 30 fps (menus and the HUD at normal speed, six physics steps a frame, movies held at 30 fps), renders docked play at native 1080p, and has key 5 (MetalFX temporal anti-aliasing: smoother edges, about 9 fps less on the plateau at 1600x900, 67.7 to 58.6). Key 4 switches a 21:9 ultrawide view on 1.9.0 (Mac: the camera sees more at the sides and the picture fills a wider window; `TOTK_ASPECT=32:9` or `3440x1440` picks another shape and starts with it on; the HUD and menus keep their shape in the middle 16:9, the map with black bars at the sides; markers that follow objects in the world are not moved out yet). Both versions now run uncapped on a 120 Hz display too, up to 120 fps (measured on 1.9.0); before, the game's present interval held them at 60 fps there. Walking into a place for the first time stutters while new pipelines compile. Not yet: native 2K, iPhone / iPad, and a fix on main for Link's legs, which sink into the stone path after a save at one Great Plateau spot loads (the old autosave and a new save made there); the cause is known, pull request #511 (open) fixes the legs, and #239 on main cures the clear Bottomless Bog water but not the legs. See [the 1.9.0 test record](botw-v1.9.0-port.md).

### Apple Vision Pro

Tears of the Kingdom can be built as a native visionOS 26 app with `--platform visionos`. It uses a resizable 16:9 window, docked 1920×1080 output and a game controller. This is windowed play; immersive VR, head tracking and experimental stereo are not included. MetalFX frame interpolation and temporal anti-aliasing are unavailable on visionOS and stay off.

Use the existing separate-data installation to keep the large game image and shader pack across app updates:

```sh
python3.14 build_app.py ~/Games/totk.xci -k ~/keys/prod.keys --platform visionos --ios-game-data documents -o ~/Games/TOTK-Vision.app
python3.14 games/totk/ios/tools/install_device.py ~/Games/TOTK-Vision.app --device "<Vision Pro name>" --game-data ~/Games/TOTK_build/totk/extracted/v0/romfs/romfs.bin
```

Install the visionOS platform in Xcode Settings › Components, pair the headset with Xcode and enable Developer Mode. For a build with `--update`, use that version's extracted image (`v112`, `v121` or `v143`) instead of `v0`; with `--work`, use the chosen work folder. The installer selects the native visionOS signing profile from the app. Game data uses the same existing 1 GiB parts and shader packs as iPhone/iPad. Native visionOS support currently covers Tears of the Kingdom only. A contributor reported playing 1.2.1 on an M2 Vision Pro with a DualSense; the current integration has not been tested on a headset.

### iPhone and iPad

Version 1.4.3 has been played on an **iPhone 14 (iOS 26.6.1)** and an **iPad with the A16 chip (iPadOS 26.7.1)**, both signed with a personal development team in Xcode. Versions 1.0.0, 1.1.2 and 1.2.1 have not been tested on iOS. Breath of the Wild 1.6.0 has been played on the iPhone 14 (title, loading a save, walking around; October 23): build it with `--platform ios --ios-game-data documents` and install it with `games/totk/ios/tools/install_device.py` like Tears of the Kingdom. It fills the screen and renders at 960x540 with the game's own upscale to 1280x720, as Tears of the Kingdom does on the phone. 1.9.0 builds for the iPhone the same way (960x540 too) but has not been played on one yet. On the phone both games run at up to 60 fps, the screen's rate.

- **First launch:** the app checks the game data (about 15 seconds) and imports the shaders it carries, with a progress bar. On an iPhone 14 this takes about 3 minutes before the title appears. Later launches show the title in about 8 seconds.
- **Picture:** the game runs in handheld mode, as a Switch does without its dock: 1280×720.
- **Shaders:** an iPhone or iPad cannot translate shaders while playing. A draw whose shader variant the app does not carry is skipped, so objects can be missing in places no player has reported yet. The device records what it missed in `shader-list-contribution.txt` (Files app › On My iPhone › Tears of the Kingdom); the game never asks about it during play. While the device is connected to the Mac that built the app (cable or the same Wi-Fi), the Mac checks every 10 minutes and every 500 new shaders asks once to send the file in a [GitHub issue](https://github.com/ly05010419/MacNativeGames/issues/new?template=shader-list.yml) (`games/totk/ios/tools/shader_contrib_watch.py`, installed by the iOS build; `--no-shader-reminder` skips it, `shader_contrib_watch.py uninstall` removes it). Once merged into the [shared shader list](shader-list.md), the next build carries them for everyone. With a Mac, `games/totk/ios/tools/fill_shaders.py` also translates them and sends them to the device right away.
- **Memory:** the game uses close to 4 GB on an iPhone 14, near what the phone can give it. In busy areas iOS can close the game after several minutes of play when the phone runs out of memory (seen twice on an iPhone 14, about 8 minutes in). Reducing the game's memory on iOS is the next piece of work.

## Controls

On Mac, controllers recognized by macOS (such as DualSense and Xbox controllers) are mapped to a Switch Pro Controller. PlayStation and Xbox face buttons map by position, so each one sits where the game's prompt shows it: the right button is A, the bottom B, the top X and the left Y (Circle / Cross / Triangle / Square; Xbox B / A / Y / X). Nintendo controllers map by label. Key **6** picks the controller type by hand (Xbox or PlayStation: by position, Switch: by label); `TOTK_FACE_BUTTONS=label` maps every controller by label while the type is auto. Rumble and motion require the corresponding features to be exposed by the controller and macOS; not every controller has a gyroscope. Mac keyboard mappings use physical key positions corresponding to a US layout:

| Key | Switch | | Key | Switch |
|---|---|---|---|---|
| W A S D | left stick | | arrow keys | right stick |
| J | A | | K, Space | B |
| I | X (jump) | | U | Y (attack) |
| Q / E | L / R | | Option / Shift | ZL / ZR |
| F T H G | d-pad left / up / right / down | | Z / C | left / right stick press |
| Return | + (menu) | | Tab, Esc | − (map) |

Controller layout (Mac): by default the face buttons follow the Switch layout by position: the right button is A, the bottom B, the top X and the left Y on every controller (Nintendo controllers by their labels, which are the same; `TOTK_FACE_BUTTONS=label` maps PlayStation and Xbox controllers by label instead). To change any button, put lines `physical = Switch [mode]` in `~/Library/Application Support/MacNativeGames/TOTK/controls.txt`; lines not given keep the default, `#` starts a comment and `none` leaves a button unused. Physical buttons are named by position: `south` `east` `west` `north` (face buttons), `l1` `r1` `l2` `r2` `l3` `r3`, `menu` (Options / Menu), `view` (Create / View), `up` `down` `left` `right` (d-pad) and `touchpad` (the touchpad click of a DualSense or DualShock 4, unused by default: `touchpad = minus` opens the Purah Pad); with mouse mode on (the settings panel), `mouse_left` `mouse_right` `mouse_middle` `mouse4` `mouse5` (the side buttons, unused by default). Switch buttons: `a` `b` `x` `y` `l` `r` `zl` `zr` `plus` `minus` `lstick` `rstick` `up` `down` `left` `right`. The optional mode says how long the Switch button is held: `hold` (default) while the physical button is down, `tap` for 400 ms after each press, and `toggle` on and off at each press, cleared when the left stick returns to center after moving, and `aim <fire button>` while the button is down except while the fire button is also down: `l2 = zr aim r2` draws the bow with L2 and shoots with R2, and R2's own line applies only while L2 is up. The default is the same as this file, which maps by position:

```
east = a
south = b
north = x
west = y
mouse_left = y
mouse_right = zr aim mouse_left
mouse_middle = zl
```

To run without holding a button, two lines can drive the run button: `south = b tap` (tap the bottom button again and again, as in GTA) or `l3 = b toggle` (click the stick once and move; it stops when the stick returns to center). The Switch's B is contextual: while climbing, a held B lets go of the wall, so a toggle that is still on when you climb drops the player. Release the stick before climbing, or use `tap`.

The launcher checks the file at every start and names the first bad line in an alert. Delete the file for the default layout.

**Rumble (Mac, the settings panel):** the game's HD rumble plays on the controller's haptics (for controllers macOS gives haptics, such as the DualSense), on by default. The settings panel (key 1) has a Rumble switch, a Rumble strength menu (0.5× to 4×) and a Gyro aiming switch. A switch acts at once and the strength applies to the next rumble. Turning rumble or gyro on when the game started without it takes effect from the next launch, which the row says. The panel keeps the choices in three one-word files next to `controls.txt`, like `controller-type.txt` and `mouse.txt`: `rumble.txt` (`on` or `off`), `rumble-gain.txt` (the strength, a number above 0 and at most 4, default 1: `2` plays the game's rumble twice as strong, and anything past full strength plays at full strength) and `gyro.txt` (`on` or `off`, see Gyro aiming below). The files can also be written by hand, for example `off` in `rumble.txt` in `~/Library/Application Support/MacNativeGames/TOTK` (`BotW` for Breath of the Wild). `TOTK_RUMBLE`, `TOTK_RUMBLE_GAIN` and `TOTK_GYRO` still win over the files when they are set, and the panel then shows their value greyed out. A file with a word the game does not know keeps the default and says so in the log; it never stops the game.

**Gyro aiming (Mac):** a controller's motion sensor reaches the game as the Switch Pro Controller's, on by default (the settings panel's Gyro aiming switch, `gyro.txt` saying `off`, or `TOTK_GYRO=0` turns it off). It works with controllers that report motion to macOS (DualSense, DualShock) and with Switch-style Pro Controllers, including 8BitDo pads in Switch mode, whose motion is read from their own HID reports with their factory calibration. Xbox controllers have no gyroscope, and the generic HID pads macOS does not adopt give buttons and sticks only. Turn on motion controls in the game's options, then aim (a bow, Ultrahand) by turning the controller. `TOTK_GYRO_DEADZONE=<deg/s>` sets the dead zone for small movements, `TOTK_GYRO_AXES` remaps the axes (for example `-x,y,-z`) if a controller turns the wrong way. iPhone and iPad have no gyro aiming yet. Tested on Mac with Tears of the Kingdom.

**DualSense adaptive triggers (Mac):** with a DualSense, R2 (ZR) resists like a bowstring while Link draws the bow, more the further it is pressed, and is slack again the moment the arrow flies or the draw ends; at any other time both triggers are slack, and L2 never resists. The game tells a controller nothing about the bow, so the runtime reads it from the game's memory every 10 ms (`runtime/bow_state.h`, read only; Breath of the Wild 1.9.0 and Tears of the Kingdom 1.4.3, other versions leave the triggers alone). It works over USB and Bluetooth; over Bluetooth the very start of the pull is slack for some 30–45 ms. On by default: the settings panel's (key 1) DualSense bow trigger switch turns it off and on at once and keeps the choice in `dualsense-triggers.txt` next to `controls.txt` (`off` or `on`, which can also be written by hand). `TOTK_DUALSENSE_TRIGGERS=on` / `off` overrides it, and the switch then shows that value greyed out. Known limit: Tears of the Kingdom keeps the bow drawn while its menu is open, so R2 keeps resisting there until the menu closes. `TOTK_TRIGGER_LOG=1` logs the triggers' mode and status. Tested on Mac with both games, DualSense over Bluetooth.

**Mouse (Mac, the settings panel):** the settings panel's Mouse switch turns mouse mode on and off; the choice is kept for the next launch in `mouse.txt` next to `controls.txt`. While it is on and the game window is in front, the window holds the pointer (hidden, kept in the window); Command-Tab or the settings panel lets it go, and so does a text entry such as naming a horse. Moving the mouse turns the camera: it drives the right stick the way yuzu's "mouse panning" does (written anew here): movement adds to a stick deflection that a 10 ms tick shows to the game and then lets decay, so the camera keeps turning while the mouse moves and settles when it stops. The same movement also turns the controller's gyro, so with motion controls on in the game's options, aiming a bow or Ultrahand follows the mouse directly; while mouse mode is on the mouse has the gyro to itself (a controller's motion comes back when it is off). Buttons: left attacks (Y), right draws the bow (ZR) and a left click while it is held shoots, middle targets (ZL); change them in `controls.txt` like any controller button. Settings: `TOTK_MOUSE_SENSITIVITY` 1–100 (50: 20 points of movement is a full stick deflection), `TOTK_MOUSE_INVERT_Y=1`, `TOTK_MOUSE_COUNTERWEIGHT` (20: the smallest movement turns at least 20 % of the stick, past the game's dead zone), `TOTK_MOUSE_DECAY` (18) and `TOTK_MOUSE_MIN_DECAY` (6) for how fast the turn settles, `TOTK_MOUSE_GYRO=0` for the stick only.

**Settings panel (Mac, key 1):** key **1** opens and closes the settings panel (Esc or its ✕ close it too; drag its background to move it, and it opens there next time). While it is open the keyboard and mouse do not reach the game; a controller still plays. Each choice applies at once:
- **Overlay:** all (the box in the window's top-right corner: FPS with the output and, in brackets, the game's internal 3D resolution, e.g. `FPS 60  1920x1080 (1920x1080)`; shader and render pipeline counts; the load line, GPU first, then the CPU cores; and the commit the app was built from), FPS only, or off. `TOTK_HUD=corners` puts the readings in the picture's corners instead.
- **Frame lock:** off, 30, 40 or 60 fps, paced exactly (by display refreshes where the rate divides the display's, by its own clock otherwise).
- **Frame interpolation (MetalFX):** off, 30+30, 40+40, 50+50 (the game held at 30 / 40 / 50 fps, MetalFX makes a frame between each two) or unlimited. Picking frame interpolation turns the frame lock off and the other way round.
- **Resolution:** 720p, 1080p, 1080p + MetalFX upscaling to the window, 2K (2560×1440, the game's canvas and UI) or 2K + MetalFX upscaling (for 4K screens). The game starts at 1080p; 1.4.3 keeps the choice for the next start in `~/Library/Application Support/MacNativeGames/TOTK/picture-mode.txt`. In a window, the window takes that size at once (as far as the screen holds it). Breath of the Wild 1.9.0 has 1080p and MetalFX only.
- **Aspect ratio:** 16:9, 16:10, 21:9 or 32:9 (1.2.1 and 1.4.3, Breath of the Wild 1.6.0 and 1.9.0). The camera sees more at the sides (or, at 16:10, keeps its vertical view; `TOTK_ASPECT_VERT=1`, 1.4.3, experimental, sees more above and below instead, with some UI effects in the wrong place); the HUD, menus and map keep their shape in the middle 16:9; markers that follow objects in the world are not moved yet. In a window the window takes the shape; in full screen the picture keeps it, with black bars. `TOTK_ASPECT=2.39` (or `3440x1440`) starts on that ratio and adds it to the list.
- **Edge smoothing (MetalFX Temporal):** temporal anti-aliasing of the 3D scene, off at start (`TOTK_TAA=1` starts it on; 1.4.3 and Breath of the Wild); `TOTK_ANISO=16` also turns on 16x anisotropic filtering.
- **Controller type:** auto (it says what it took the controller for, such as "Auto (PlayStation)"), Xbox, PlayStation or Switch. Xbox and PlayStation map the face buttons by position, Switch by label. Kept in `controller-type.txt` next to `controls.txt`; generic HID pads stay Switch-style. It does not change the button icons in the game's prompts (see `make_button_icons.py`).
- **DualSense bow trigger (R2):** on / off (see DualSense adaptive triggers above; Breath of the Wild 1.9.0 and Tears of the Kingdom 1.4.3).
- **Mouse:** mouse mode on / off (see Mouse above).
- **Rumble, Rumble strength (0.5× to 4×) and Gyro aiming:** see Rumble and Gyro aiming above.
- Two buttons at the bottom: **Contribute shaders and pipelines** sends the ones your game met that the shared list lacks (with the GitHub CLI `gh` when it is signed in, in the background; otherwise the GitHub form opens and Finder shows the file to drag in), and **Star the project** opens the GitHub page.

Keys 2–7 are no shortcuts any more. Text entry (naming a horse) uses the Mac keyboard. iPhone/iPad uses touch controls or a compatible game controller and has no settings panel. The touch layout follows a Switch controller: L and ZL at the top left, R and ZR at the top right, − and + between them, the d-pad on the left and A, B, X, Y on the right. The d-pad sits above the left stick and A, B, X, Y above the right stick, and L3 and R3 (stick presses) are in the bottom corners. Touch the left half away from the buttons and drag for the left stick, the right half for the camera (right stick); each stick is centred where your finger goes down. With a game controller connected the touch controls are hidden: touch the screen to show and use them, and they hide again 3 seconds after you lift your fingers.

**Ultrawide layouts (optional, 1.4.3):** with a wide aspect ratio the HUD and menus stay in the middle 16:9. To put the HUD in the corners of a 21:9 or 32:9 display instead, make layouts for its shape from your own game files and start with them: `.venv/bin/python scripts/make_romfs_patches.py --version v143 --aspect 3440x1440` (writes `extracted/v143/romfs-patches-3440x1440`; `--hud center` keeps the HUD in the middle) and `scripts/make_output_res_overlay.py 3440x1440 --version v143`, then `TOTK_OUTPUT_RES=3440x1440 TOTK_ASPECT=3440x1440 TOTK_UI_SQUEEZE=0 TOTK_ROMFS_PATCHES=extracted/v143/romfs-patches-3440x1440` (in the work folder). The layouts are read at start, so such a session stays wide; the panel's aspect ratio is not meant for it. Every edited pane is found by name and checked against the value the game ships before it is written.

The menu bar shows the live frame rate as the game's own menu, on the left after the app menu ("60 FPS", or "50+50 FPS" while frame interpolation adds generated frames), whatever the overlay shows in the window; in full screen it is hidden with the menu bar. `TOTK_MENUBAR_FPS=0` leaves the menu bar alone; `TOTK_TITLE_FPS=1` also ends the window's title in the same readout ("… Ver. 1.4.3 — 60 FPS").

Native 2K changes the game's composition canvas and UI to 2560×1440. The 3D scene still follows the game's separate render scaling; this does not force every internal render target to 1440p. The manual MetalFX mode also works in smaller windows and scales to the actual output size, including 4K.

Language: the game and the launcher's own text (startup screen, shader warm-up progress, overlays, keyboard prompt) follow the Mac's language. The launcher text is Chinese on a Chinese Mac and English otherwise. Regional variants follow the console: English outside the Americas (en-GB, en-AU, ...) selects British English, which shows temperatures in Celsius (American English shows Fahrenheit), French in Canada selects Canadian French, Spanish in the Americas Latin American Spanish, and in a 1.4.3 build Brazilian Portuguese selects the game's official pt-BR text. Simplified and Traditional Chinese are told apart by the script (zh-Hans / zh-Hant) or, without one, by the region (Taiwan, Hong Kong and Macau are Traditional). To give only this game another language, choose it in System Settings › General › Language & Region › Applications, or set its preferred language, for example `defaults write org.macnativegames.totk AppleLanguages -array pt-BR` (`defaults delete org.macnativegames.totk AppleLanguages` removes it). To override, launch with `TOTK_LANG` for the game (for example `open --env TOTK_LANG=en-US "Tears of the Kingdom.app"`) or `TOTK_UI_LANG=zh` / `en` for the launcher text.

## Where things are

- Saves: `~/Library/Application Support/MacNativeGames/TOTK/profile`
- Shader cache: `~/Library/Application Support/MacNativeGames/TOTK/shader-cache` (delete it to translate everything again)
- Shaders the shared list does not have yet: `~/Library/Application Support/MacNativeGames/TOTK/shader-list-contribution.txt`
- Log: off by default, nothing is recorded. Turn it on with `defaults write org.macnativegames.totk TOTK_DEBUG_LOG -bool YES` (`org.macnativegames.botw` for Breath of the Wild; `-bool NO` turns it off) and it goes to `~/Library/Logs/MacNativeGames/totk.log` (the run before: `totk.previous.log`). On the iPhone: Settings › TOTK › Debug log, written to the app's `Documents/log.txt`. Both take effect at the next start.
- Mods: `~/Library/Application Support/MacNativeGames/TOTK/mods/<v0|v112|v121|v143>`. Its `manifest.tsv` lists each RomFS file the mod replaces or adds as tab-separated path, size, SHA-256 and the original's SHA-256 (`-` for a file the game does not have); the files sit under the same paths. `python3 games/totk/scripts/make_mod_manifest.py <mods folder> --app <Tears of the Kingdom.app>` writes it, adding files the game lacks; a path that differs from the game's only in upper and lower case stops it (paths are case-sensitive). The RomFS (data) side is loaded while the game runs. A mod's code patch for the main module (`exefs/*.ips`, `*.ips32`, `*.pchtxt`) is merged at build time with `--mod <mod folder>` instead, because the code modules are rewritten for Apple silicon and bound by SHA-256 when the app is built; a patch that would land on a site the app patches itself (60 FPS, resolutions, ultrawide, cloth) is refused. Add `--install-mod-romfs` to that build and it also replaces this folder with the mod's `romfs/` and writes the manifest itself (what the folder held is kept next to it as `<version>.backup-<date>-<time>`; without the flag the build leaves it alone). An extra code module a mod ships (`subsdk9`) is not loaded - the app runs main, sdk and subsdk0 - and the build prints that. The game checks every listed file at start and stops on a mismatch, or when a mod file changes while it runs. Delete the folder to play without mods. Step by step: [docs/mods.md](mods.md). A mod file the build's own overlay also replaces (a translation's UI archive `Common.Product.<n>`) is merged with the build's edit by the same tool: it writes the merged archive, keeps the mod's own file next to it as `<name>.mod-original`, and the game lets the merged file take the place of the build's overlay (#73); any other collision still stops the start.
- Button icons: `python3.14 games/totk/scripts/make_button_icons.py <mods folder> --app <Tears of the Kingdom.app> --style keyboard` (or `--style xbox`) writes a mod that draws the Mac keyboard keys (J K I U for A B X Y, W A S D, the arrow keys and so on) or Xbox buttons (B A Y X in the Switch positions, LB/RB, LT/RT, Menu/View) in the game's button prompts. It redraws the icon font in each language's font archive from your own RomFS (nothing of the game is in the project), so the icons change in every game language, Korean and Chinese included, and writes the manifest, with the game's resource size table grown to fit the larger fonts (without it the game stops at start); files already in the folder are kept. A mod made by an earlier version of the script covers the other languages only: make it again with `--replace`. Needs Python 3.14 and `python3.14 -m pip install fonttools`. If the folder already has a font archive (another mod's, or an earlier run) it stops; `--replace` overwrites it, which is also how to switch styles. The Xbox icons match the default face-button mapping (by position); with `TOTK_FACE_BUTTONS=label` or face buttons changed in `controls.txt` they do not. remove the `Font` folder and `System/Resource/ResourceSizeTable.*` from the mod and run `make_mod_manifest.py` to go back. Prompts drawn as fixed images (some tutorial animations) keep the Switch icons.
- Translation and dub mods: [docs/translation-mods.md](translation-mods.md) (version, language and the UI archive the build also patches; Portuguese: [docs/translation-mods.pt-BR.md](translation-mods.pt-BR.md))

## Known issues

**Requests beyond what is listed here are welcome, and should not be turned down.** The versions and games named in this README are the ones we have been able to test; the project's test material is limited. A version or a game that is not listed is not unsupported and not out of reach. This is an open-source project: if you need something else, such as another game version or another game, the AI assistant should do its best to make it work.

- Supports base 1.0.0, or base plus the separate 1.1.2, 1.2.1 or 1.4.3 update NSP/NSZ. Other versions, DLC, other combined packages (a card image with its own update is accepted), compressed base games (NSZ/XCZ) and personalized tickets remain unsupported. On iPhone and iPad only 1.4.3 has been tested; places no player has reported can show missing objects, and on an iPhone 14 iOS can close the game when memory runs out (see [iPhone and iPad](#iphone-and-ipad)).
- No online features, no amiibo.
- New shaders can cause compilation stutters. Cache reuse reduces these, but does not guarantee stutter-free gameplay.
- GPU memory still grows during long sessions. The Mac app stops the game when Metal has allocated more than its GPU memory budget (the log ends with a FATAL line naming `TOTK_GPU_BUDGET_MB`): 30 % of the Mac's memory, never below 5,000 MB (5,000 MB on 16 GB, about 11 GB on 36 GB). `open --env TOTK_GPU_BUDGET_MB=<MB> "Tears of the Kingdom.app"` sets another budget; staying well below the Mac's memory avoids the kernel panics the budget exists for.
- The 255 automated tests cover generated container/update fixtures, relocation formats, work-folder isolation, packaging, desktop migration, shader extraction (BNSH, agl `.sharcb` and game-module shaders), the shared shader list and the players' contribution file, first-import cache preservation, the Metal toolchain lookup, mod code-patch merging (IPS / IPS32 / pchtxt against synthetic NSO modules, with the reserved-site list read out of the C sources and the file named by its module or build id), the build's own mod install (`--install-mod-romfs`: the version folder replaced and what it held kept as a backup, a symlinked folder refused, a misspelt path stopped before anything is touched, the real `make_mod_manifest.py` run against a fake RomFS), the 2K output overlay's record of the heap table it was built on, the RomFS manifest that leaves the app's own overlay files out by itself and 27 native RomFS path cases. GitHub Actions is configured to run the source tests and the native path parser under the undefined-behavior sanitizer, without game data or keys. A complete Mac build also passed with a local XCI-derived NSP fixture and generated common ticket. An independent retail base NSP and gameplay from that input have not yet been tested.
