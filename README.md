# MacNativeGames

# English

**Breath of the Wild 1.6.0 is now officially supported on Mac!** (Mac only for now: no iPhone or iPad build yet.)
**Tears of the Kingdom 1.0.0, 1.2.1 and 1.4.3 are now officially supported on Mac, iPhone and iPad!**

> **Use your AI assistant to help you build the Mac app. We recommend Claude Code, Codex, or any other AI assistant you prefer.**

[English](#english) | [中文](#chinese)

## Legal

**You must own the game.** Use this project only with a copy of The Legend of Zelda: Tears of the Kingdom that you bought, and with keys and system files dumped from your own Nintendo Switch. This is an independent, non-commercial project. It is not affiliated with, endorsed by, or sponsored by Nintendo. The Legend of Zelda and Nintendo Switch are trademarks of Nintendo.

The repository contains no game content and distributes none: no game program, art, text, audio, shaders or keys. Everything the game needs is made on your own computer from your own files when you build the app. The app you build contains the game, so it is for your own use only; do not share it. What the repository does hold is the information needed to make the port work: the patch code, the addresses and hashes used to check your files, and the [shared shader list](docs/shader-list.md), which names shaders and bind states but contains no shader code.

Play your own copy of **The Legend of Zelda: Tears of the Kingdom** natively on Apple silicon Mac, iPhone, and iPad. Bring your game file (XCI or NSP) and your `prod.keys`; one script builds a Mac app or an unsigned iPhone/iPad IPA. The iOS build requires iOS/iPadOS 26 and iPhone 14 or newer. It is not an emulator: the game's own ARM64 code runs directly on Apple chips (rewritten where the operating system requires it), the Switch system services are answered by a small host, and the game's NVN graphics are rebuilt on Metal. Nothing from the game is in this repository, and the project is not affiliated with Nintendo. Version 1.4.3 has been played on an iPhone 14 and an iPad; see [iPhone and iPad](#iphone-and-ipad).

## Let's maintain the shader list together: no more stutter

> [!TIP]
> **Every shader in the shared list is ready before you play.** On a Mac the game does not stop to compile it, and on an iPhone or iPad nothing is missing from the picture. The more places players cover, the smoother the game gets for everyone.

The [shared shader list](docs/shader-list.md) now holds **50,628 shader variants** (43,333 shaders) from the Mac, iPhone and iPad. Every build makes them from the player's own game files, so the list holds identifiers only, never game data. Helping takes no setup:

1. **Play.** When your game meets a shader the list does not have yet, it notes it in `shader-list-contribution.txt`.
2. **On a Mac, every 500 new shaders it asks you once, before the game starts.** Click "Send": the GitHub form opens and the file is shown, ready to drag in. If the GitHub CLI (`gh`) is installed and signed in, the dialog offers "Submit automatically" instead, which sends this batch for you (a private gist and an issue) without opening the browser. An iPhone or iPad never asks during play: while it is connected to the Mac that built its app (cable or the same Wi-Fi), the Mac asks instead. "Don't remind me again" stops the reminders.
3. **Everyone's next build carries them.** Nobody stutters there again.

**[Send your shaders →](https://github.com/ly05010419/MacNativeGames/issues/new?template=shader-list.yml)** · The game never uploads anything by itself.

Update history:

| Date | Platform | Update |
|---|---|---|
| Oct 21 | Mac | `TOTK_FIXED_RES=1` holds 1.4.3's dynamic resolution at 100 % (#122); the mouse cursor hides after 1.5 s idle in native full screen (#121); 16:10 UI layouts with `make_romfs_patches.py --aspect` (#123); a translation mod's UI archive is merged with the build's quick-menu edit instead of refusing to start (#134, #73) |
| Oct 21 | Mac, iPhone, iPad | Vertex shaders with a custom sampler border colour get their own pipeline per colour, and a sampler that repeats on one axis no longer returns the border there (#148); Breath of the Wild's Magnesis and Stasis pick targets are written back to the CPU (#140); macOS 27 start-up reserves the guest regions separately when no single gap is left (#138); 4,648 new variants in the shared shader list |
| Oct 21 | Mac, iPhone, iPad | When the game reads or writes part of a texture, only that region is converted, on the GPU, instead of the whole level on the CPU, which was behind long hitches on the Lanayru sky islands (#143) |
| Oct 21 | Mac | With key 5 (edge smoothing, MetalFX temporal), ground decals no longer leave purple glows on hazy cliffs and lamps no longer pulse while standing still (#157) |
| Oct 21 | Mac | Key 4 cycles 16:9 → fit to the window or display → 21:9. Fitting gives 16:10 on a MacBook in full screen (a little less at the sides; `TOTK_ASPECT_VERT=1` sees more above and below instead, 1.4.3, experimental: some UI effects such as the button-press effect sit off), 21:9 or 32:9 on an ultrawide; the HUD keeps its shape on all of them (#120) |
| Oct 20 | iPhone, iPad | Sound plays with the silent switch on and comes back after a call, Siri or an alarm; the game follows the system language; a fresh install reaches the title screen (#97) |
| Oct 20 | iPhone, iPad | First launch after installing shows the game in about 7 seconds instead of about 4½ minutes: the shaders that come with the app are read where they are, without checking or copying them again (iPhone 14) |
| Oct 20 | Mac | Breath of the Wild above 30 fps: menus and the HUD at normal speed, physics steady, movies at their own speed; docked play renders at native 1080p (`TOTK_RENDER_RES=900` keeps 1600x900) (#103) |
| Oct 20 | Mac | Faster app builds: files are cloned in one call and the 16 GB of game data is signed once (#100) |
| Oct 20 | All | Shader translation update: vertex outputs a shader does not write start as (0, 0, 0, 1), which brings back missing markers and effects (#91), and shadow compares and depth reads match the Switch, which removes dark patches on the ground and walls and the dark halo behind Link (#89, #104). The first launch after updating translates shaders again, because the old cache is from the previous translation (on iPhone and iPad the old cache is removed) |
| Oct 20 | Mac | High frame rates (all four versions): trampolines bounce above 100 FPS (#61), and hair and cloth sway at the right speed above 30 FPS (#79) |
| Oct 20 | Mac | Key 5 smooths the stair-stepped edges of the 3D scene with MetalFX temporal anti-aliasing (1.4.3); `TOTK_ANISO=16` sharpens ground and wall textures seen at a grazing angle |
| Oct 20 | Mac | The live FPS in the menu bar; key 1 gains an FPS-only state |
| Oct 20 | Mac | Optional UI layouts for 21:9 and 32:9 displays with the HUD in the screen's corners (1.4.3, made from your own game files) |
| Oct 20 | Mac | Background warm-up prepares only the shaders this version of the game uses (about half the time); `TOTK_WARM_ALL=1` warms the whole cache |
| Oct 20 | Mac | Two memory leaks fixed for both games |
| Oct 20 | Mac | **Breath of the Wild 1.6.0** (early): a card image holding the base game, the update and the add-on content builds its own app with the same command; title, saves, the Great Plateau and the DLC packs work, keys 2 (frame interpolation) and 3 (MetalFX 2K) work |
| Oct 20 | iPhone, iPad | The launcher adopts the UIScene lifecycle, so an IPA built with Xcode 27 starts on iOS 27 (which ends apps that still make their window in the app delegate) |
| Oct 19 | Mac | Key 4 switches to a true 21:9 ultrawide view and back (1.2.1, 1.4.3); the HUD keeps its shape in the middle 16:9 |
| Oct 19 | iPhone, iPad | Tested with 1.4.3: a fresh install shows the game from the first launch, without a played cache |
| Oct 19 | iPhone, iPad | Touch controls laid out like a Switch controller, with every button and both sticks |
| Oct 19 | All | The game notes the shaders the shared list lacks; on a Mac, every 500 it asks to send them in a [GitHub issue](https://github.com/ly05010419/MacNativeGames/issues/new?template=shader-list.yml) |
| Oct 19 | All | One shared [shader list](docs/shader-list.md); the build reads every shader in the game data (15,266 prepared for 1.4.3) |
| Oct 19 | Mac | 1.2.1 support (the most widely used update), with the same build command |
| Oct 19 | Mac | FPS, shader and load readings and the shortcut reminder in one small box in the window's top-right corner (`TOTK_HUD=corners` keeps the old layout) |
| Oct 18 | iPhone, iPad | IPA build target |
| Oct 18 | Mac | 1.4.3 support and shader prewarming: the title screen waits only for its own shaders (about 2 seconds), the rest warm in the background |
| Oct 17 | Mac | 1.0.0 and 1.1.2 support; no first-launch shader prewarming yet, so new scenes could stutter |

**Stuttering when you first start playing or enter a new scene is expected while new shaders compile—don't be surprised.** Performance typically improves as the cache warms up, and every shader the [shared list](#lets-maintain-the-shader-list-together-no-more-stutter) gains is one nobody stutters on again.

## Audio fix

Streamed music and dialogue lookup for `content://Sound/...` paths has been fixed. Opening dialogue and music were verified locally on version 1.0.0; other versions and scenes still need validation. The renderer also runs all known audio effect types and starts output after 20 ms of queued audio (2026-10-18). Rebuild your app with the original game inputs to apply the fix. See [audio troubleshooting and validation](docs/audio-troubleshooting.md) for details.

## Local testing

Tested on **MacBook Air M4 and Mac mini M4**. Local testing has collected **8,582 shaders**, with performance at approximately **90 FPS after shader warm-up, reaching 120 FPS in menus**.

Those are earlier local observations, not a stable frame-rate promise for every version or scene. Version 1.4.3 measurements and compilation stalls are recorded separately in [the test notes](docs/v1.4.3-port.md).

Shaders are automatically collected, compiled and cached as you play. Returning to previously visited scenes typically brings noticeable improvements in frame rates and smoothness because cached shaders can be reused. **8,582 is the number collected so far in local testing, not the game's total shader count or a collection limit**; the cache continues to grow as you explore.

**Shader prewarming on the first launch:** Building the app prepares shaders from your selected game data and includes them in the app: every shader in the game data for its default bind state, plus every variant named in the [shared shader list](docs/shader-list.md). Even with an empty user cache, the first launch imports these shaders. Only what the title screen itself uses is warmed before the first frame (about 2 seconds once macOS has compiled them before); the rest, together with compatible entries in the pipeline manifest, warms in the background behind the title, and the first trip into the world waits up to 15 seconds for it. No preliminary play session or manual cache copy is needed. The current 1.4.3 build prepares 15,266. Building the app also compiles them for your Mac's GPU. With Xcode's Metal toolchain installed they are compiled into the app (about 6 minutes on an M4 for 1.4.3) and work wherever the app goes. Without it they are compiled into macOS's Metal cache (a few minutes), and Metal finds them only while the game runs from where the app was built: if you move the app afterwards (to /Applications, for example), if macOS clears the cache (it does when the system disk fills up), or if another program was using Metal's cache while the app was built (a running game, for example; the build says so), the background warm-up compiles them again, which takes a few minutes, and new scenes can stutter until it finishes. To place the app elsewhere, build it there with `-o`; see [preparation and test details](docs/first-launch-shaders.md). Places no player has reported yet can still need new variants, which the Mac compiles while you play (a short stutter the first time). Playing adds them to your local cache, which is reused on later launches. The game also notes every variant the shared list does not have yet in `shader-list-contribution.txt`, and every 500 of them asks once whether to send the file in a [GitHub issue](https://github.com/ly05010419/MacNativeGames/issues/new?template=shader-list.yml) ("Send" opens the form and shows the file to drag in). It never uploads anything by itself. With git, you can also merge your cache and open a pull request, as [the list's guide](docs/shader-list.md) explains. The list holds shader and pipeline identifiers only, never game data, saves or compiled shaders; every build makes the shaders from the player's own game files.


## What you need

- A Mac with Apple silicon and **macOS 26** or later (the graphics use Metal 4); macOS 27 is supported. 16 GB of memory or more is recommended.
  - Still on macOS 15? A community fork by [1918662410-ship-it](https://github.com/1918662410-ship-it/MacNativeGames) adds a Metal 3 backend for macOS 15. It is maintained there; this repository does not test it.
- About **25 GB** of free disk space for the work folder, in addition to your game file. Keep the output app on the **same APFS volume** to share the large data files through cloning. If cloning is unavailable or the app is on another volume, allow roughly **17 GB more** for the app copy.
- Your Tears of the Kingdom **base game 1.0.0** (`0100F2C0115B6000`) as an **XCI or NSP**, and your `prod.keys`. To build **1.1.2, 1.2.1 or 1.4.3**, supply its separate cumulative update as NSP or NSZ with `--update`; no intermediate update is required. NSZ is decompressed and its NCA hashes are verified before extraction. DLC and combined packages are unsupported.
- For NSP: its common ticket must be included unless the NCAs are rights-free. NSPs do not contain system shared fonts: supply an unpacked firmware NCA directory with `--firmware`, or previously extracted fonts with `--shared-fonts`. The six fonts are checked against this project’s reference dump; a valid BFTTF font from another firmware version is accepted and bound by its own bytes. Valid fonts already in the work folder are reused.

## Build the app

**1. Tools** (once): Xcode command line tools and [Homebrew](https://brew.sh). An internet connection is needed to install build dependencies and fetch pinned sources:

```sh
xcode-select --install
brew install opus libvpx python@3.14
```

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

Add `--update ~/Games/1.4.3.nsp` to select an update, and keep the `--firmware` or `--shared-fonts` option when the base game is an NSP. The first iOS build downloads and compiles device versions of Opus and libvpx and compiles the shader seed for iOS, so it needs an internet connection and substantial temporary disk space (allow at least **45 GB free** for a fresh base build, or **60 GB** for a fresh build with an update). The resulting IPA is unsigned: sign and install it with your sideloading tool, keeping its entitlements (it uses the increased memory limit). iOS/iPadOS 26 and iPhone 14 or newer are required. See [iPhone and iPad](#iphone-and-ipad) for what has been tested.

Without `-o`, the selected output appears next to the game file (`Tears of the Kingdom.app` on Mac or `Tears of the Kingdom.ipa` for iOS). Before full shader preparation was added, a local clean base-1.0.0 Mac build took about 6 minutes on an M4 Mac mini (this is not a timing estimate for the current build; hardware, disk and network conditions affect the time): about a minute each to rewrite the main code module and to decrypt the 17 GB of game data, and two to three minutes for macOS to check the finished app once. The current build then also prepares the startup shaders (about 3 minutes for 1.4.3 on an M4) and compiles them for your GPU (a few more minutes on an M4, but about 25 minutes on an 8 GB M1 MacBook Air, which runs out of memory and swaps during this step; without the Metal toolchain this step is skipped, with a message, while a game is running). A Mac build needs no .NET or CMake, and the finished game needs no Python: its shader translator is a native program that comes with this repository (`games/totk/prebuilt`). Every intermediate file is in a work folder next to the game file (`TOTK_build`, or `--work`); running the script again reuses what is already done.

The extracted code modules, rewritten modules, RomFS and shared fonts are checked against the selected version's `games/totk/assets/expected-v1.0.0.json`, `expected-v1.1.2.json`, `expected-v1.2.1.json` or `expected-v1.4.3.json`; the prebuilt shader translator has its own SHA-256 check. A mismatch stops the build, with one exception: a different genuine retail dump of the same version is accepted — its `main.npdm` is checked by Nintendo’s ACID signature (`npdm_signed()`) instead of the pinned hash, and its firmware’s shared fonts are checked for a valid BFTTF structure and bound by their actual SHA-256. Key values are never printed.

Signing and verifying a large updated app can add several minutes. Keep the build running while it continues to report progress.

**3. Play:** open the app from Finder. The build starts the launcher in `--check` mode, which checks that the game executable exists and allows macOS to perform its first-launch checks; it does not run a gameplay test. You can keep the app where it was built or move it to `/Applications`; the first start after a move compiles the shaders again in the background (macOS's Metal cache does not follow the app to its new place), so to keep it in `/Applications`, build it there with `-o`. Copying it to a different volume can use additional disk space and trigger another macOS check, during which no window may appear. The first game launch verifies the game data (about 20 seconds in local testing). Newly encountered shaders can cause stutters; cached shaders reduce this work on later visits.


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

Saves stay in the same profile folder. Before the first updated launch, the app makes a complete backup beside it, such as `profile-before-1.1.2`, `profile-before-1.2.1` or `profile-before-1.4.3`; failure stops startup. Keep this backup if you need to return to an older version—do not assume an updated save is backward compatible.

To keep one desktop entry, link the packaged app with the optional installer:

```sh
python3.14 games/totk/tools/install_desktop_entry.py --app "/path/to/Tears of the Kingdom.app"
```

When replacing the older development launcher, the installer copies and verifies its saves into the standard user profile listed below and creates an upgrade backup. It preserves the original profile and archives the old launcher without adding another app entry. An existing, different standard profile is never overwritten.

The source runtime passed basic Mac checks: load, movement/jumps, menus/map, 2K, save/reload and death retry. Full-map, long-session and fair 1.0/1.1.2 performance comparisons remain pending; the earlier FPS observations are not a promise of 1.1.2 performance. Packaging validation is recorded in [the build notes](docs/how-it-works.md#112-validation).

The 1.4.3 port has separately passed scene loading, movement/jumps, menus/map, save/reload, native 2K canvas switching and MetalFX upscaling/interpolation on an M4 Mac. The single packaged app has been tested, including save/reload, death/retry and shrine entry/exit. A three-hour earlier build and a 30-minute final-build run are recorded separately; first-use compilation stalls, long-session memory growth and audio underruns remain under investigation. See [the 1.4.3 test record](docs/v1.4.3-port.md) for the scope and follow-ups; this is not full-game or stable-FPS certification.

The 1.2.1 port (October 19) passed the same rewrite verification (600 of 600 negative controls) and the loader, RomFS and startup probes. On an M4 Mac it loaded saves in the 1.0.0 and 1.1.2 formats and passed movement/jumps, menus/map/quick menus, manual save and reload in a new process, fall death and retry, and the three key-3 picture modes; the packaged app was also opened through its launcher. See [the 1.2.1 test record](docs/v1.2.1-port.md); this is not full-game or long-session certification.

### Breath of the Wild 1.6.0 (early)

The same script builds **The Legend of Zelda: Breath of the Wild 1.6.0** (`01007EF00011E000`) from a card image that holds the base game, the 1.6.0 update and the two add-on content titles together (no `--update`; the game is recognised by the title IDs in the file, never by its name). This card has no system update partition, so the six shared fonts must come from a Tears of the Kingdom build's work folder (`TOTK_build/totk/extracted/firmware/shared-fonts`) or another verified font folder. The startup shaders come from the game data and the game's own [shared shader list](docs/shader-list.md) (`games/botw/assets/shader_list.tsv`), as for Tears of the Kingdom: every shader of the boot and title packages for its default bind state, plus every bind state the list names, compiled ahead and waited for only as far as the title screen needs.

```sh
python3.14 build_app.py ~/Games/botw.xci -k ~/keys/prod.keys --shared-fonts ~/Games/TOTK_build/totk/extracted/firmware/shared-fonts
```

A separate base NSP or XCI and a **1.6.0 update NSP or NSZ** also work. This path builds the game without the expansion pass; DLC is not required. Common tickets must be included for rights-managed NSPs. Other update versions are rejected by the verified executable hashes.

```sh
python3.14 build_app.py ~/Games/botw-base.nsp -k ~/keys/prod.keys --update ~/Games/botw-1.6.0.nsp --shared-fonts ~/Games/TOTK_build/totk/extracted/firmware/shared-fonts
```

The work folder is `BotW_build` next to the game file; the app is `Breath of the Wild.app` (bundle `org.macnativegames.botw`), with its own data folder `~/Library/Application Support/MacNativeGames/BotW` and log `~/Library/Logs/MacNativeGames/botw.log`. Every step is checked against `games/botw/assets/expected-v1.6.0.json`; a fresh build takes about 16 minutes on an M4 (the 15 GB RomFS merge is most of it).

What works (October 20, an M4 Mac, a Great Plateau save): title, the add-on content list, loading and saving, the plateau at about 90–120 fps (the game advances by real time; held-button repeats count by real time too), the two DLC packs mounted and read, key 1 (overlays), key 2 (MetalFX frame interpolation: off → 30+30 → half the refresh rate → unlimited, every game frame interpolated on the plateau) and key 3 (MetalFX 2K on / off: MetalFX replaces the game's own upscale of the 1600x900 picture into the 1080p canvas and the window output above 1080p); a 30-minute session on the plateau at about 95 fps with memory steady (+170 MB). Not yet: native 2K, iPhone / iPad, the DLC quests themselves (only verified as mounted and read). This is an early port: expect rough edges.

### iPhone and iPad

Version 1.4.3 has been played on an **iPhone 14 (iOS 26.6.1)** and an **iPad with the A16 chip (iPadOS 26.7.1)**, both signed with a personal development team in Xcode. Versions 1.0.0, 1.1.2 and 1.2.1 have not been tested on iOS. Breath of the Wild has no iPhone/iPad build yet: `--platform ios` with its card image stops with an error.

- **First launch:** the app checks the game data (about 15 seconds) and imports the shaders it carries, with a progress bar. On an iPhone 14 this takes about 3 minutes before the title appears. Later launches show the title in about 8 seconds.
- **Picture:** the game runs in handheld mode, as a Switch does without its dock: 1280×720.
- **Shaders:** an iPhone or iPad cannot translate shaders while playing. A draw whose shader variant the app does not carry is skipped, so objects can be missing in places no player has reported yet. The device records what it missed in `shader-list-contribution.txt` (Files app › On My iPhone › Tears of the Kingdom); the game never asks about it during play. While the device is connected to the Mac that built the app (cable or the same Wi-Fi), the Mac checks every 10 minutes and every 500 new shaders asks once to send the file in a [GitHub issue](https://github.com/ly05010419/MacNativeGames/issues/new?template=shader-list.yml) (`games/totk/ios/tools/shader_contrib_watch.py`, installed by the iOS build; `--no-shader-reminder` skips it, `shader_contrib_watch.py uninstall` removes it). Once merged into the [shared shader list](docs/shader-list.md), the next build carries them for everyone. With a Mac, `games/totk/ios/tools/fill_shaders.py` also translates them and sends them to the device right away.
- **Memory:** the game uses close to 4 GB on an iPhone 14, near what the phone can give it. In busy areas iOS can close the game after several minutes of play when the phone runs out of memory (seen twice on an iPhone 14, about 8 minutes in). Reducing the game's memory on iOS is the next piece of work.

## Controls

On Mac, controllers recognized by macOS (such as DualSense and Xbox controllers) are mapped to a Switch Pro Controller. PlayStation and Xbox face buttons map by position, so each one sits where the game's prompt shows it: the right button is A, the bottom B, the top X and the left Y (Circle / Cross / Triangle / Square; Xbox B / A / Y / X). Nintendo controllers map by label. `TOTK_FACE_BUTTONS=label` maps every controller by label instead. Rumble and motion require the corresponding features to be exposed by the controller and macOS; not every controller has a gyroscope. Mac keyboard mappings use physical key positions corresponding to a US layout:

| Key | Switch | | Key | Switch |
|---|---|---|---|---|
| W A S D | left stick | | arrow keys | right stick |
| J | A | | K, Space | B |
| I | X (jump) | | U | Y (attack) |
| Q / E | L / R | | Option / Shift | ZL / ZR |
| F T H G | d-pad left / up / right / down | | Z / C | left / right stick press |
| Return | + (menu) | | Tab, Esc | − (map) |

Controller layout (Mac): by default the face buttons follow the Switch layout by position: the right button is A, the bottom B, the top X and the left Y on every controller (Nintendo controllers by their labels, which are the same; `TOTK_FACE_BUTTONS=label` maps PlayStation and Xbox controllers by label instead). To change any button, put lines `physical = Switch [mode]` in `~/Library/Application Support/MacNativeGames/TOTK/controls.txt`; lines not given keep the default, `#` starts a comment and `none` leaves a button unused. Physical buttons are named by position: `south` `east` `west` `north` (face buttons), `l1` `r1` `l2` `r2` `l3` `r3`, `menu` (Options / Menu), `view` (Create / View), `up` `down` `left` `right` (d-pad) and `touchpad` (the touchpad click of a DualSense or DualShock 4, unused by default: `touchpad = minus` opens the Purah Pad). Switch buttons: `a` `b` `x` `y` `l` `r` `zl` `zr` `plus` `minus` `lstick` `rstick` `up` `down` `left` `right`. The optional mode says how long the Switch button is held: `hold` (default) while the physical button is down, `tap` for 400 ms after each press, and `toggle` on and off at each press, cleared when the left stick returns to center after moving, and `aim <fire button>` while the button is down except while the fire button is also down: `l2 = zr aim r2` draws the bow with L2 and shoots with R2, and R2's own line applies only while L2 is up. The default is the same as this file, which maps by position:

```
east = a
south = b
north = x
west = y
```

To run without holding a button, two lines can drive the run button: `south = b tap` (tap the bottom button again and again, as in GTA) or `l3 = b toggle` (click the stick once and move; it stops when the stick returns to center). The Switch's B is contextual: while climbing, a held B lets go of the wall, so a toggle that is still on when you climb drops the player. Release the stick before climbing, or use `tap`.

The launcher checks the file at every start and names the first bad line in an alert. Delete the file for the default layout.

Mac-only extra keys: **1** cycles the on-screen overlays: all of them (FPS, shader count, system load and the shortcut reminder, together in one small box in the window's top-right corner; `TOTK_HUD=corners` puts the readings back in the picture's corners), then minimal (the FPS only), then none; **2** frame interpolation (off → 30+30 → half the refresh rate → unlimited), **3** cycle picture mode (MetalFX 2K → native 2K → off), **4** cycles the picture's shape: 16:9 → fit to the window / display → 21:9 → 16:9 (1.2.1 and 1.4.3; the shortcut reminder and the log say which one is on). Fitting fills the window instead of keeping 16:9 with black bars: on a 21:9 or 32:9 monitor the camera sees more at the sides, on a MacBook in full screen (16:10 below the notch) it keeps the vertical view and sees a little less at the sides (`TOTK_ASPECT_VERT=1`, 1.4.3, experimental: sees more above and below instead; known issue: some UI effects such as the button-press effect sit in the wrong place); the view follows the window while you resize it. Fitting is skipped while the window is within 1% of 16:9 or 21:9, where it would show the same. `TOTK_ASPECT=32:9`, `16:10` or `3440x1440` takes the place of 21:9 and starts on it, `TOTK_ASPECT=display` starts on fitting the window; the HUD, menus and map keep their shape in the middle 16:9 of the window, with black bars at the sides (wider windows) or above and below (16:10) of the map; markers that follow objects in the world are not moved yet), **5** edge smoothing on / off (1.4.3: MetalFX temporal anti-aliasing of the 3D scene, off at start, `TOTK_TAA=1` starts with it on; it smooths the stair-stepped edges of the scene and works together with the picture modes of **3**; `TOTK_ANISO=16` also turns on 16x anisotropic filtering for the ground, walls and foliage). The shortcut reminder shows the current interpolation, picture and aspect ratio modes and refreshes twice a second in both HUD layouts. MetalFX is off by default and only starts when you select its mode with **3**; resizing the window never enables it automatically. Text entry (naming a horse) uses the Mac keyboard. iPhone/iPad uses touch controls or a compatible game controller and does not show the Mac 1/2/3/4/5 shortcut prompts. The touch layout follows a Switch controller: L and ZL at the top left, R and ZR at the top right, − and + between them, the d-pad on the left and A, B, X, Y on the right. The d-pad sits above the left stick and A, B, X, Y above the right stick, and L3 and R3 (stick presses) are in the bottom corners. Touch the left half away from the buttons and drag for the left stick, the right half for the camera (right stick); each stick is centred where your finger goes down. With a game controller connected the touch controls are hidden: touch the screen to show and use them, and they hide again 3 seconds after you lift your fingers.

**Ultrawide layouts (optional, 1.4.3):** with key 4 the HUD and menus stay in the middle 16:9. To put the HUD in the corners of a 21:9 or 32:9 display instead, make layouts for its shape from your own game files and start with them: `.venv/bin/python scripts/make_romfs_patches.py --version v143 --aspect 3440x1440` (writes `extracted/v143/romfs-patches-3440x1440`; `--hud center` keeps the HUD in the middle) and `scripts/make_output_res_overlay.py 3440x1440 --version v143`, then `TOTK_OUTPUT_RES=3440x1440 TOTK_ASPECT=3440x1440 TOTK_UI_SQUEEZE=0 TOTK_ROMFS_PATCHES=extracted/v143/romfs-patches-3440x1440` (in the work folder). The layouts are read at start, so such a session stays wide; key 4 is not meant for it. Every edited pane is found by name and checked against the value the game ships before it is written.

The menu bar shows the live frame rate as the game's own menu, on the left after the app menu ("60 FPS", or "50+50 FPS" while frame interpolation adds generated frames), whatever key 1 shows in the window; in full screen it is hidden with the menu bar. `TOTK_MENUBAR_FPS=0` leaves the menu bar alone; `TOTK_TITLE_FPS=1` also ends the window's title in the same readout ("… Ver. 1.4.3 — 60 FPS").

Native 2K changes the game's composition canvas and UI to 2560×1440. The 3D scene still follows the game's separate render scaling; this does not force every internal render target to 1440p. The manual MetalFX mode also works in smaller windows and scales to the actual output size, including 4K.

Language: the game and the launcher's own text (startup screen, shader warm-up progress, overlays, keyboard prompt) follow the Mac's language. The launcher text is Chinese on a Chinese Mac and English otherwise. In a 1.4.3 build, Brazilian Portuguese selects the game's official pt-BR text. To give only this game another language, set its preferred language, for example `defaults write org.macnativegames.totk AppleLanguages -array pt-BR` (`defaults delete org.macnativegames.totk AppleLanguages` removes it). To override, launch with `TOTK_LANG` for the game (for example `open --env TOTK_LANG=en-US "Tears of the Kingdom.app"`) or `TOTK_UI_LANG=zh` / `en` for the launcher text.

## Where things are

- Saves: `~/Library/Application Support/MacNativeGames/TOTK/profile`
- Shader cache: `~/Library/Application Support/MacNativeGames/TOTK/shader-cache` (delete it to translate everything again)
- Shaders the shared list does not have yet: `~/Library/Application Support/MacNativeGames/TOTK/shader-list-contribution.txt`
- Log: `~/Library/Logs/MacNativeGames/totk.log` (the run before: `totk.previous.log`)
- Mods: `~/Library/Application Support/MacNativeGames/TOTK/mods/<v0|v112|v121|v143>`. Its `manifest.tsv` lists each RomFS file the mod replaces or adds as tab-separated path, size, SHA-256 and the original's SHA-256 (`-` for a file the game does not have); the files sit under the same paths. `python3 games/totk/scripts/make_mod_manifest.py <mods folder> --app <Tears of the Kingdom.app>` writes it, adding files the game lacks; a path that differs from the game's only in upper and lower case stops it (paths are case-sensitive). Only the RomFS (data) side of a mod is loaded: ExeFS parts (extra code modules, IPS patches) are not supported. The game checks every listed file at start and stops on a mismatch, or when a mod file changes while it runs. Delete the folder to play without mods. Step by step: [docs/mods.md](docs/mods.md). A mod file the build's own overlay also replaces (a translation's UI archive `Common.Product.<n>`) is merged with the build's edit by the same tool: it writes the merged archive, keeps the mod's own file next to it as `<name>.mod-original`, and the game lets the merged file take the place of the build's overlay (#73); any other collision still stops the start.
- Button icons: `python3.14 games/totk/scripts/make_button_icons.py <mods folder> --app <Tears of the Kingdom.app> --style keyboard` (or `--style xbox`) writes a mod that draws the Mac keyboard keys (J K I U for A B X Y, W A S D, the arrow keys and so on) or Xbox buttons (B A Y X in the Switch positions, LB/RB, LT/RT, Menu/View) in the game's button prompts. It redraws the icon font from your own RomFS (nothing of the game is in the project) and writes the manifest; files already in the folder are kept. Needs Python 3.14 and `python3.14 -m pip install fonttools`. If the folder already has a font archive (another mod's, or an earlier run) it stops; `--replace` overwrites it, which is also how to switch styles. The Xbox icons match the default face-button mapping (by position); with `TOTK_FACE_BUTTONS=label` or face buttons changed in `controls.txt` they do not. remove the font file and run `make_mod_manifest.py` to go back. Prompts drawn as fixed images (some tutorial animations) keep the Switch icons.

## Known issues

**Requests beyond what is listed here are welcome, and should not be turned down.** The versions and games named in this README are the ones we have been able to test; the project's test material is limited. A version or a game that is not listed is not unsupported and not out of reach. This is an open-source project: if you need something else, such as another game version or another game, the AI assistant should do its best to make it work.

- Supports base 1.0.0, or base plus the separate 1.1.2, 1.2.1 or 1.4.3 update NSP/NSZ. Other versions, DLC, combined packages, compressed base games (NSZ/XCZ) and personalized tickets remain unsupported. On iPhone and iPad only 1.4.3 has been tested; places no player has reported can show missing objects, and on an iPhone 14 iOS can close the game when memory runs out (see [iPhone and iPad](#iphone-and-ipad)).
- No online features, no amiibo.
- New shaders can cause compilation stutters. Cache reuse reduces these, but does not guarantee stutter-free gameplay.
- The 79 automated tests cover generated container/update fixtures, relocation formats, work-folder isolation, packaging, desktop migration, shader extraction (BNSH, agl `.sharcb` and game-module shaders), the shared shader list and the players' contribution file, first-import cache preservation, the Metal toolchain lookup and 27 native RomFS path cases. GitHub Actions is configured to run the source tests and the native path parser under the undefined-behavior sanitizer, without game data or keys. A complete Mac build also passed with a local XCI-derived NSP fixture and generated common ticket. An independent retail base NSP and gameplay from that input have not yet been tested.

## How it works

See [docs/how-it-works.md](docs/how-it-works.md).

Recorded performance experiments, remaining work and comparison methods: [Mac performance follow-ups](docs/performance-followups.md).

## Acknowledgements

- **[d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile)** (MIT) developed this way of running Switch games natively (for Diablo II: Resurrected): the NSO rewriter, the Horizon host and the NVN-on-Metal layer here started as its generic parts.
- **[nxnative](https://github.com/seedds/nxnative)**: the one-script packaging and NSP common-ticket import follow its approach.
- **Ryujinx** (MIT): its shader translator (`vendor/ryujinx`) turns the game's Maxwell shaders into SPIR-V, and its system-service code is a reference. With this project's patches and service it is compiled ahead of time (.NET NativeAOT) into `games/totk/prebuilt/macos-arm64/totk_xlate`; `games/totk/tools/totk_xlate/publish_native.sh` rebuilds it (needs the .NET 10 SDK, only for developers).
- **[SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross)** (Apache-2.0) turns SPIR-V into Metal Shading Language; it is fetched at a pinned commit and compiled with clang during the build.
- **deko3d** (zlib): texture layout sizes. **bcdec**: BC texture decoding. **libopus** and **libvpx** (BSD) from Homebrew: audio and the pre-rendered movies.

## License

MIT (see [LICENSE](LICENSE)). Third-party code keeps its own license: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

---

<a id="chinese"></a>

# 中文说明

**正式支持旷野之息 1.6.0 mac了！**（目前只有 Mac 版，还不能打包 iPhone / iPad 版。）
**正式支持王国之泪 1.0.0,1.2.1，1.4.3 Mac iPhone iPad了！**

> **请使用你的 AI 助理帮你完成 Mac App 的构建。推荐 Claude Code、Codex，或其他你喜欢的 AI 助理。**

[英文说明](#english) | [中文说明](#chinese)

## 法律声明

**你必须拥有正版游戏。** 请只配合你自己购买的《塞尔达传说 王国之泪》，以及从你自己的 Nintendo Switch 导出的密钥和系统文件使用本项目。这是一个独立的非商业项目，与任天堂没有关联，也未获其认可或赞助。The Legend of Zelda（塞尔达传说）和 Nintendo Switch 是任天堂的商标。

本仓库不含、也不分发任何游戏内容：没有游戏程序、美术、文字、音频、着色器或密钥。游戏需要的一切，都是打包时在你自己的电脑上、用你自己的文件生成的。打包出的 App 里含有游戏，只供你自己使用，请不要分享。仓库里有的是让移植能运行的信息：补丁代码、用来核对你的文件的地址和哈希，以及[公共着色器清单](docs/shader-list.md)（只记着色器和绑定状态的标识，不含着色器代码）。

在 Apple 芯片的 Mac、iPhone 和 iPad 上原生运行你自己拥有的**《塞尔达传说 王国之泪》**。准备好游戏文件（XCI 或 NSP）和你的 `prod.keys`，一个脚本即可构建 Mac App 或未签名的 iPhone / iPad IPA。iOS 构建需要 iOS / iPadOS 26，最低支持 iPhone 14。这不是模拟器：游戏自己的 ARM64 代码直接运行在苹果芯片上（按操作系统要求改写），Switch 的系统服务由一个小宿主应答，游戏的 NVN 图形在 Metal 上重建。本仓库不含任何游戏内容，项目与任天堂无关。1.4.3 已在 iPhone 14 和 iPad 上实际游玩，见 [iPhone 和 iPad](#iphone-和-ipad)。

## 让我们一起维护着色器清单：游戏不再卡顿

> [!TIP]
> **公共清单里的每个着色器，开玩之前就已经准备好。** Mac 上不用停下来现场编译，iPhone 和 iPad 上画面也不会缺东西。大家玩过的地方越多，所有人玩得越流畅。

[公共着色器清单](docs/shader-list.md)现在有 **50,628 个着色器变体**（43,333 个着色器），来自 Mac、iPhone 和 iPad。每次打包都用玩家自己的游戏文件生成，所以清单里只有标识，不含任何游戏数据。参与不需要任何设置：

1. **照常玩。** 游戏遇到清单里还没有的着色器，会自动记进 `shader-list-contribution.txt`。
2. **在 Mac 上，每攒够 500 个，它会在游戏启动前问你一次。** 点“发送”，GitHub 提交页面会打开，文件也会显示出来，拖进去就行。装了 GitHub 命令行工具（`gh`）并已登录时，对话框里会多一个“自动提交”，点了就直接替你把这一批发出去（私密 gist 加一个 issue），不用打开浏览器。iPhone 和 iPad 玩的时候从不提醒：设备连着打包它的那台 Mac（线连或同一 Wi‑Fi）时，由 Mac 来问。点“不要提醒了”以后就不再问。
3. **下次大家打包的 App 都会带上。** 谁到那里都不会再卡顿。

**[发送你的着色器 →](https://github.com/ly05010419/MacNativeGames/issues/new?template=shader-list.yml)** · 游戏不会自己上传任何东西。

更新历史：

| 日期 | 平台 | 更新 |
|---|---|---|
| 10 月 21 日 | Mac | `TOTK_FIXED_RES=1` 把 1.4.3 的动态分辨率固定在 100%（#122）；原生全屏时鼠标 1.5 秒不动就隐藏（#121）；`make_romfs_patches.py --aspect` 支持 16:10 界面布局（#123）；翻译模组的界面档案和构建自带的快捷菜单修改合并，不再拒绝启动（#134，#73） |
| 10 月 21 日 | Mac、iPhone、iPad | 顶点着色器的自定义采样器边框色按颜色各建管线，单轴重复的采样器在该轴不再返回边框色（#148）；旷野之息磁力、静止的选取目标回写到 CPU（#140）；macOS 27 启动时没有整块空间就分开保留客体区域（#138）；公共着色器清单新增 4,648 条 |
| 10 月 21 日 | Mac、iPhone、iPad | 游戏读写贴图的一部分时，只在 GPU 上转换那一块，不再在 CPU 上转换整层；拉聂尔一带空岛上的长卡顿就来自这里（#143） |
| 10 月 21 日 | Mac | 开 5 键（边缘平滑，MetalFX 时间性）时，地面贴花不再在雾蒙蒙的山崖上留下紫色光斑，站着不动时灯也不再忽明忽暗（#157） |
| 10 月 21 日 | Mac | 按 4 在 16:9 → 适配窗口或显示器 → 21:9 之间切换。适配时 MacBook 全屏是 16:10（两侧略少；`TOTK_ASPECT_VERT=1` 改为上下看到更多，1.4.3，实验性：按键特效等部分界面特效位置偏了），超宽屏是 21:9 或 32:9；HUD 在各种比例下都保持原形状（#120） |
| 10 月 20 日 | iPhone、iPad | 打开静音键也有声音，来电、Siri、闹钟之后声音会恢复；游戏跟随系统语言；全新安装能进标题画面（#97） |
| 10 月 20 日 | iPhone、iPad | 安装后第一次启动约 7 秒出画面，原来要约 4 分半：随 App 的着色器直接原地读取，不再校验和复制（iPhone 14） |
| 10 月 20 日 | Mac | 旷野之息 30 帧以上：菜单和 HUD 速度正常、物理稳定、过场动画按原速播放；底座模式按原生 1080p 渲染（`TOTK_RENDER_RES=900` 保持 1600x900）（#103） |
| 10 月 20 日 | Mac | 打包更快：文件一次克隆，16 GB 游戏数据只签名一次（#100） |
| 10 月 20 日 | 全部 | 着色器翻译更新：着色器没写的顶点输出从 (0, 0, 0, 1) 开始，找回了缺失的标记和特效（#91）；阴影比较和深度读取改成和 Switch 一样，去掉了地面、墙面的暗斑和林克身后的暗边（#89、#104）。更新后第一次启动会重新翻译着色器，因为旧缓存是上一版翻译的（iPhone、iPad 上旧缓存会被删掉） |
| 10 月 20 日 | Mac | 高帧率（四个版本）：100 FPS 以上蹦床能弹起来（#61），30 FPS 以上头发和衣服摆动速度正常（#79） |
| 10 月 20 日 | Mac | 按 5 用 MetalFX 时间性抗锯齿让 3D 画面边缘的锯齿变平滑（1.4.3）；`TOTK_ANISO=16` 让斜着看的地面、墙面纹理更清楚 |
| 10 月 20 日 | Mac | 菜单栏显示实时帧率；1 键多了“只显示 FPS”一档 |
| 10 月 20 日 | Mac | 可选的 21:9、32:9 界面布局，HUD 贴到屏幕四角（1.4.3，用你自己的游戏文件生成） |
| 10 月 20 日 | Mac | 后台预热只准备这个版本游戏会用到的着色器（时间约减半）；`TOTK_WARM_ALL=1` 预热整个缓存 |
| 10 月 20 日 | Mac | 修了两处两个游戏都有的内存泄漏|
| 10 月 20 日 | Mac | **旷野之息 1.6.0**（早期）：本体 + 更新 + 追加内容合在一起的卡带镜像用同一条命令打成单独的 App；标题、存档、初始台地和 DLC 内容包都能用，2 键（补帧）和 3 键（MetalFX 2K）都能用 |
| 10 月 20 日 | iPhone、iPad | 启动器改用 UIScene 生命周期：用 Xcode 27 构建的 IPA 能在 iOS 27 上启动（iOS 27 会结束仍在 app delegate 里创建窗口的 App） |
| 10 月 19 日 | Mac | 按 4 切换真正的 21:9 超宽画面（1.2.1、1.4.3），HUD 保持原比例、在中间 16:9 区域 |
| 10 月 19 日 | iPhone、iPad | 用 1.4.3 实测：全新安装、没有玩过的缓存，第一次启动就能看到游戏画面 |
| 10 月 19 日 | iPhone、iPad | 触屏控件按 Switch 手柄排布，所有按键和两个摇杆都有 |
| 10 月 19 日 | 全部 | 游戏会记下公共清单里还没有的着色器；Mac 上每攒够 500 个提醒一次用 [GitHub Issue](https://github.com/ly05010419/MacNativeGames/issues/new?template=shader-list.yml) 发送 |
| 10 月 19 日 | 全部 | 所有平台共用一份[着色器清单](docs/shader-list.md)；打包时读取游戏数据里的全部着色器（1.4.3 准备 15,266 个） |
| 10 月 19 日 | Mac | 支持 1.2.1（玩家最多的版本），构建命令不变 |
| 10 月 19 日 | Mac | FPS、着色器、负载读数和快捷键说明集中到窗口右上角一个小框里（`TOTK_HUD=corners` 保留原来的布局） |
| 10 月 18 日 | iPhone、iPad | 加入 IPA 构建目标 |
| 10 月 18 日 | Mac | 支持 1.4.3，并加入着色器预热：标题画面只等它自己用到的着色器（约 2 秒），其余在后台预热 |
| 10 月 17 日 | Mac | 支持 1.0.0 和 1.1.2；当时还没有首次启动着色器预热，进入新场景可能卡顿 |

**第一次进入游戏或新场景时，新着色器的编译可能造成卡顿，这是预期现象，不必惊讶。** 随着着色器缓存逐步预热，表现通常会改善；[公共清单](#让我们一起维护着色器清单游戏不再卡顿)每多一个着色器，就少一处卡顿。

## 音频修复

已修复 `content://Sound/...` 路径导致的流式音乐和对白读取失败。已在 1.0.0 开场实测对白和配乐，其他版本和场景仍需验证。使用原有游戏输入重新构建 App 即可应用修复，详见[音频排障与验证说明](docs/audio-troubleshooting.md)。

## 本机测试

已在 **MacBook Air M4 和 Mac mini M4** 上测试。本机测试已收集 **8,582 个着色器**，着色器预热后约 **90 帧/秒，菜单中可达 120 帧/秒**。

以上是此前本机观察，不代表所有版本和场景都能稳定达到该帧率。1.4.3 的场景测量及首次编译卡顿另见[测试记录](docs/v1.4.3-port.md)。

着色器会在游玩过程中自动收集、编译并缓存。再次进入已经去过的场景时，可以复用已有缓存，帧数和流畅度通常会明显改善。**8,582 只是本机测试目前收集到的数量，不是游戏的着色器总数，也不是收集上限**；随着探索更多场景，缓存还会继续增加。

**首次启动的着色器预热：** 构建时从你提供的所选版本游戏数据准备着色器并装进 App：游戏数据里的每个着色器按默认绑定状态各准备一个，再加上[公共着色器清单](docs/shader-list.md)里列出的全部变体。即使用户缓存为空，第一次打开也会导入这批着色器。第一帧前只预热标题画面自己用到的那部分（macOS 编过之后约 2 秒）；其余连同兼容的管线清单，在标题画面后面继续预热，第一次进入世界时最多再等 15 秒。不需要先玩一遍或手动复制缓存。当前 1.4.3 准备 15,266 个。打包时还会把它们为本机的 GPU 编一遍。装有 Xcode 的 Metal 工具链时直接编进 App（1.4.3 在 M4 上约 6 分钟），App 挪到哪里都能用；没有工具链时编进 macOS 的 Metal 缓存（几分钟），这时游戏要从打包时的位置运行，Metal 才找得到它们：之后挪动 App（比如挪进 /Applications）、macOS 清掉了缓存（系统盘写满时会清）、或打包时有别的程序正在用 Metal 的缓存（比如开着游戏，打包时会提示），后台预热都要重新编，需要几分钟，做完之前进入新场景可能卡顿。想放在别处，就用 `-o` 直接打包到那里，详见[准备过程与实测记录](docs/first-launch-shaders.md)。还没有玩家报告过的地方仍可能需要新的变体，Mac 会在游玩时现编（第一次会短暂卡顿），并存进本机缓存，以后启动时复用。游戏还会把公共清单里还没有的变体记进 `shader-list-contribution.txt`，每攒够 500 个问一次要不要用 [GitHub Issue](https://github.com/ly05010419/MacNativeGames/issues/new?template=shader-list.yml) 发送这个文件（点“发送”会打开提交页面并显示文件，拖进去即可）。游戏不会自己上传任何东西。会用 git 的话，也可以把本机缓存并入清单再提 pull request，做法见[清单说明](docs/shader-list.md)。清单只有着色器和管线的标识，不含游戏数据、存档或编译后的着色器；每次打包都用玩家自己的游戏文件生成着色器。


## 需要准备

- Apple 芯片的 Mac，**macOS 26** 或更高（图形用 Metal 4），支持 macOS 27。建议使用 16 GB 或更多内存。
  - 还在用 macOS 15？[1918662410-ship-it 的社区 fork](https://github.com/1918662410-ship-it/MacNativeGames) 加了 macOS 15 用的 Metal 3 后端。它由作者在那边维护，本仓库不做测试。
- 除游戏文件外，为工作目录预留约 **25 GB** 空闲空间。成品 App 与工作目录放在**同一个 APFS 卷**上时，大型数据文件可通过克隆共享磁盘空间；无法克隆或输出到其他卷时，还需为 App 副本另留约 **17 GB**。
- 你的《王国之泪》**本体 1.0.0**（`0100F2C0115B6000`），格式为 **XCI 或 NSP**，以及你的 `prod.keys`。构建 **1.1.2、1.2.1 或 1.4.3** 时，通过 `--update` 提供单独的累计更新 NSP 或 NSZ；无需先安装中间版本。NSZ 会先解压并校验其中的 NCA 哈希。DLC 和合并包不支持。
- 使用 NSP 时：除不依赖票据的 NCA 外，包内必须包含通用票据。NSP 不含系统共享字体，需要通过 `--firmware` 指定已解包的固件 NCA 目录，或通过 `--shared-fonts` 提供以前提取的字体。六个字体会与本项目的参考转储核对；来自其他固件版本的有效 BFTTF 字体会按它自身的字节接受并绑定。工作目录中已有且校验通过的字体会自动复用。

## 打包 App

**1. 工具**（只需一次）：Xcode 命令行工具和 [Homebrew](https://brew.sh)。安装构建依赖和获取固定版本源码需要联网：

```sh
xcode-select --install
brew install opus libvpx python@3.14
```

构建 iPhone / iPad App 还需要完整的 **Xcode 26 或更高版本**（包含 iOS 和 Metal 工具链），并安装以下工具：

```sh
brew install xcodegen cmake ninja
```

可选：装有 Xcode 的 Metal 工具链（`xcodebuild -downloadComponent MetalToolchain`）时，构建会把启动着色器预先编译进 App。Xcode 不在 `/Applications` 也能找到（放在其他卷上，或设置 `DEVELOPER_DIR`）。没有它时，构建会改为把它们编进本机 Metal 缓存里属于 App 所在位置的那一份，这份缓存不跟着 App 走：挪动 App 之后，第一次启动会重新编译。

**2. 下载或克隆本仓库**，在终端中进入 `MacNativeGames` 目录，再运行脚本，给出游戏文件和密钥：

```sh
python3.14 build_app.py ~/Games/totk.xci -k ~/keys/prod.keys
```

使用 NSP 时，还要提供共享字体：

```sh
python3.14 build_app.py ~/Games/totk.nsp -k ~/keys/prod.keys --firmware ~/Games/firmware
# 或复用以前构建时提取的六个 .bfttf 文件：
python3.14 build_app.py ~/Games/totk.nsp -k ~/keys/prod.keys --shared-fonts /path/to/TOTK_build/totk/extracted/firmware/shared-fonts
```

`--firmware` 接受包含 `.nca` 文件的目录（固件压缩包需先解压），也接受已安装固件的 `registered` 目录。`--shared-fonts` 接受包含 `0-standard.bfttf` 至 `5-nintendo-extension.bfttf` 的提取目录。工具不会下载字体或密钥。XCI 默认从其系统更新分区提取字体；如果该分区缺失，也可以使用上述字体选项。默认情况下，两种输入格式最终都生成 macOS `.app`。

要构建 iPhone / iPad IPA，请添加 `--platform ios`，并使用 `.ipa` 输出路径：

```sh
python3.14 build_app.py ~/Games/totk.xci -k ~/keys/prod.keys --platform ios -o ~/Games/TOTK.ipa
```

构建更新版时添加 `--update ~/Games/1.4.3.nsp`；本体为 NSP 时也要继续提供 `--firmware` 或 `--shared-fonts`。首次 iOS 构建会下载并编译适用于设备的 Opus 和 libvpx，并为 iOS 编译着色器种子，因此需要联网和较多临时磁盘空间（全新本体构建建议至少预留 **45 GB**，全新本体加更新包建议预留 **60 GB**）。生成的 IPA 未签名，需使用侧载工具签名和安装，签名时要保留它的权限（它用到提高内存上限）。需要 iOS / iPadOS 26，最低支持 iPhone 14。实测情况见 [iPhone 和 iPad](#iphone-和-ipad)。

不使用 `-o` 时，选定的平台成品会放在游戏文件旁边（Mac 为 `Tears of the Kingdom.app`，iOS 为 `Tears of the Kingdom.ipa`）。加入完整着色器准备前，本地一次本体 1.0.0 Mac 全新构建在 M4 Mac mini 上约 6 分钟（这不是当前构建的耗时预估；耗时受硬件、磁盘和网络影响）：改写主代码模块、解密 17 GB 的游戏数据各约 1 分钟，macOS 把做好的 App 检查一遍要 2–3 分钟。现在的打包还会准备启动着色器（1.4.3 在 M4 上约 3 分钟），再为本机 GPU 编译（M4 上再要几分钟；8 GB 内存的 M1 MacBook Air 在这一步内存不够、要用交换空间，约 25 分钟；没有 Metal 工具链时，有游戏开着会跳过这一步，并提示）。Mac 打包不需要 .NET 或 CMake，做好的游戏运行时也不需要 Python：着色器翻译器是本仓库自带的原生程序（`games/totk/prebuilt`）。所有中间文件都在游戏文件旁边的工作目录里（`TOTK_build`，或用 `--work` 指定），再运行一次会复用已经做好的部分。

提取的程序模块、改写后的模块、RomFS 和共享字体会与所选版本的 `games/totk/assets/expected-v1.0.0.json`、`expected-v1.1.2.json`、`expected-v1.2.1.json` 或 `expected-v1.4.3.json` 中的哈希核对；预编译着色器翻译器另有 SHA-256 校验。不匹配时停止构建，但有一个例外：同一版本的另一份真实零售转储会被接受——它的 `main.npdm` 改用任天堂的 ACID 签名校验（`npdm_signed()`），而不是固定的哈希；其固件的共享字体则检查是否为有效的 BFTTF 结构，并按实际 SHA-256 绑定。密钥的值不会被打印。

大型更新版 App 的完整签名和校验还可能多用几分钟；日志持续报告进度时，请等待构建完成。

**3. 游玩：** 在访达里打开 App。构建结束时会以 `--check` 模式启动启动器，检查游戏可执行文件是否存在，并让 macOS 执行首次启动检查；这一步不进行游戏游玩测试。App 可以留在原位置，也可以移到 `/Applications`；挪动之后的第一次启动要在后台重新编着色器（Metal 的缓存不跟着 App 走），想放在 `/Applications` 就用 `-o` 直接打包到那里。拷贝到另一个卷可能额外占用磁盘空间，并触发 macOS 再次检查，期间可能没有窗口。第一次进入游戏会校验游戏数据（本地测试约 20 秒）。首次遇到的着色器可能造成编译卡顿，缓存可减少后续访问时的重复工作。


### 构建 1.1.2、1.2.1 或 1.4.3

```sh
python3.14 build_app.py ~/Games/totk.xci -k ~/keys/prod.keys --update ~/Games/1.1.2.nsp
# 也可以使用 NSZ 更新包：
python3.14 build_app.py ~/Games/totk.xci -k ~/keys/prod.keys --update ~/Games/1.1.2.nsz
# 也可从同一本体直接构建 1.2.1 或 1.4.3：
python3.14 build_app.py ~/Games/totk.xci -k ~/keys/prod.keys --update ~/Games/1.2.1.nsp
python3.14 build_app.py ~/Games/totk.xci -k ~/keys/prod.keys --update ~/Games/1.4.3.nsz
# 本体为 NSP 时，继续提供上文的 --firmware 或 --shared-fonts。
```

仍然只生成同一个 **Tears of the Kingdom.app**，包内只含选定版本。先退出正在运行的游戏，再用相同 `-o` 路径重新构建即可替换旧 App，不增加第二个游戏入口或版本选择菜单。窗口标题显示选定版本，例如 **Ver. 1.4.3**。不加 `--update` 时仍构建 1.0.0。

无需另外下载 1.2.1 / 1.4.3 运行时表或着色器缓存。构建流程会根据你提供的游戏数据生成对应版本的运行时表和首启着色器种子，并将种子放进 App。仓库已包含各个支持的更新版的补丁代码和校验哈希；外部只需准备自己的本体、更新包和密钥。

首次完整更新构建建议预留约 **40 GB**，已缓存本体数据时还需约 **19 GB** 空间，App 需放在同一 APFS 卷。合并时通过原始容器的限定区间读取本体 NCA，只临时提取较小的更新 NCA，并校验全部 IVFC 层；工具会获取固定提交的 [hactool](https://github.com/SciresM/hactool)，用 clang / make 编译。临时票据文件仅当前用户可读，合并结束即清理；密钥值不进入命令行或日志。

存档继续保存在原 profile 目录。首次启动更新版前，App 会在旁边完整备份为 `profile-before-1.1.2`、`profile-before-1.2.1` 或 `profile-before-1.4.3`；备份失败就不启动。需要回退旧版本时保留并使用这份备份，不能假定新版存档向下兼容。

如需保留唯一桌面入口，可用附带的安装脚本建立 App 链接：

```sh
python3.14 games/totk/tools/install_desktop_entry.py --app "/路径/Tears of the Kingdom.app"
```

替换旧开发启动器时，脚本会将旧存档完整复制并校验到下列标准用户目录，同时建立升级备份；原存档保留，旧启动器归档后不再作为第二个 App 入口。已有且内容不同的标准存档不会被覆盖。

来源运行时已通过 Mac 基础检查：读档、移动 / 跳跃、菜单 / 地图、2K、保存重读及死亡重试。全地图、长局与 1.0 / 1.1.2 公平性能对比仍待测试；前面的帧率观察不是对 1.1.2 的性能承诺。打包验证范围见[构建记录](docs/how-it-works.md#112-validation)。

1.4.3 另行通过了 M4 Mac 的场景读档、移动 / 跳跃、菜单 / 地图、保存重读、原生 2K 画布切换和 MetalFX 放大 / 补帧。唯一打包 App 已实际验证，另覆盖死亡重试和神庙进出。旧构建三小时及最终版约 30 分钟分开记录；首次编译卡顿、长局内存增长与音频欠载仍待排查，不能视为全游戏或稳定帧率认证。结果、测试方法和后续项见 [1.4.3 适配记录](docs/v1.4.3-port.md)。

1.2.1（10 月 19 日）通过了同样的改写验证（600 个反向对照全部抓到）和加载、RomFS、启动探针。在 M4 Mac 上读过 1.0.0 和 1.1.2 两种格式的存档，并通过移动 / 跳跃、菜单 / 地图 / 快捷菜单、手动存档后新进程读档、摔死重试和 3 键的三种画质；打包后的 App 也从启动器打开测试过。详见 [1.2.1 测试记录](docs/v1.2.1-port.md)；这不代表全流程或长局验收。

### 旷野之息 1.6.0（早期）

同一个脚本也能打包**《塞尔达传说 旷野之息》1.6.0**（`01007EF00011E000`）：卡带镜像里要同时有本体、1.6.0 更新和两个追加内容（不用 `--update`；认的是文件里的标题 ID，不看文件名）。这张卡没有系统更新分区，六个共享字体要从王国之泪打包的工作目录拿（`TOTK_build/totk/extracted/firmware/shared-fonts`），或别的已校验的字体目录。启动着色器和王国之泪一样来自游戏数据和这个游戏自己的[公共着色器清单](docs/shader-list.md)（`games/botw/assets/shader_list.tsv`）：启动包和标题包里的每个着色器按默认绑定状态翻译，清单里的每个绑定状态也翻译，打包时预先编好，第一帧只等标题要的那些。

```sh
python3.14 build_app.py ~/Games/botw.xci -k ~/keys/prod.keys --shared-fonts ~/Games/TOTK_build/totk/extracted/firmware/shared-fonts
```

也支持单独的本体 NSP 或 XCI，加 **1.6.0 更新 NSP 或 NSZ**。这种方式不需要 DLC，生成的 App 不包含扩展票。需要标题密钥的 NSP 必须带通用票据；其他更新版本会因可执行文件哈希不匹配而被拒绝。

```sh
python3.14 build_app.py ~/Games/botw-base.nsp -k ~/keys/prod.keys --update ~/Games/botw-1.6.0.nsp --shared-fonts ~/Games/TOTK_build/totk/extracted/firmware/shared-fonts
```

工作目录是游戏文件旁边的 `BotW_build`，App 是 `Breath of the Wild.app`（`org.macnativegames.botw`），数据目录 `~/Library/Application Support/MacNativeGames/BotW`，日志 `~/Library/Logs/MacNativeGames/botw.log`。每一步对 `games/botw/assets/expected-v1.6.0.json` 核对；M4 上从头打包约 16 分钟（大头是 15 GB 的 RomFS 合并）。

已验证（10 月 20 日，M4 Mac，初始台地的存档）：标题、追加内容一览、读档和存档、台地约 90–120 帧（游戏按真实时间推进，按住按键的重复也按真实时间数）、两个 DLC 内容包挂载并被读取、1 键（提示框）、2 键（MetalFX 补帧：关 → 30+30 → 刷新率一半 → 不限帧，台地上每个游戏帧都插）和 3 键（MetalFX 2K 开 / 关：MetalFX 代替游戏自己把 1600×900 的画面放大进 1080p 画布，窗口大于 1080p 时出图也放大）；台地上 30 分钟约 95 帧、内存平稳（+170 MB）。还没有：原生 2K、iPhone / iPad、DLC 的任务本身（只验证了挂载和读取）。这是早期移植，会有毛边。

### iPhone 和 iPad

1.4.3 已在 **iPhone 14（iOS 26.6.1）** 和 **A16 芯片的 iPad（iPadOS 26.7.1）** 上实际游玩，两台都是在 Xcode 里用个人开发团队签名安装的。1.0.0、1.1.2 和 1.2.1 没有在 iOS 上测过。旷野之息还没有 iPhone / iPad 版：用它的卡带镜像加 `--platform ios` 会报错退出。

- **第一次启动：** App 先校验游戏数据（约 15 秒），再导入随 App 准备的着色器，有进度条。iPhone 14 上约 3 分钟后出现标题画面；之后的启动约 8 秒出标题。
- **画面：** 游戏以掌机模式运行，和 Switch 不接底座时一样：1280×720。
- **着色器：** iPhone 和 iPad 不能在游玩时翻译着色器。App 里没有的着色器变体，对应的绘制会被跳过，所以还没有玩家报告过的地方可能缺少物体。设备会把缺的记进 `shader-list-contribution.txt`（“文件”App › 我的 iPhone › Tears of the Kingdom），玩的时候游戏从不因此打扰你。设备连着打包它的那台 Mac（线连或同一 Wi‑Fi）时，Mac 每 10 分钟检查一次，每多 500 个问一次要不要用 [GitHub Issue](https://github.com/ly05010419/MacNativeGames/issues/new?template=shader-list.yml) 发送（`games/totk/ios/tools/shader_contrib_watch.py`，打包 iOS 版时自动安装；`--no-shader-reminder` 不装，`shader_contrib_watch.py uninstall` 卸掉）；并入[公共着色器清单](docs/shader-list.md)后，下一次打包时所有人就都有了。有 Mac 的话，也可以用 `games/totk/ios/tools/fill_shaders.py` 马上翻译这些变体并推回设备。
- **内存：** 在 iPhone 14 上游戏占用接近 4 GB，接近手机能给它的上限。在复杂的区域玩几分钟后，手机内存耗尽时 iOS 可能会关掉游戏（iPhone 14 上出现过两次，都在约 8 分钟时）。降低 iOS 上的内存占用是下一步要做的事。

## 操作

在 Mac 上，macOS 能识别的手柄（如 DualSense、Xbox 手柄）会映射为 Switch Pro 手柄。PlayStation 和 Xbox 手柄的四个面键按位置映射，和游戏里的按键提示位置一致：右边是 A、下边是 B、上边是 X、左边是 Y（圆圈 / 叉 / 三角 / 方块；Xbox 的 B / A / Y / X）。任天堂手柄按标签映射。设置 `TOTK_FACE_BUTTONS=label` 可以让所有手柄都按标签映射。震动和体感需要手柄及 macOS 提供对应能力，并非所有手柄都有陀螺仪。Mac 键盘映射按美式布局对应的物理键位：

| 按键 | Switch | | 按键 | Switch |
|---|---|---|---|---|
| W A S D | 左摇杆 | | 方向键 | 右摇杆 |
| J | A | | K、空格 | B |
| I | X（跳） | | U | Y（攻击） |
| Q / E | L / R | | Option / Shift | ZL / ZR |
| F T H G | 十字键 左 / 上 / 右 / 下 | | Z / C | 左 / 右摇杆按下 |
| Return | +（菜单） | | Tab、Esc | −（地图） |

手柄布局（Mac）：默认按位置对应 Switch 的布局，每种手柄都是右边的面键为 A、下边为 B、上边为 X、左边为 Y（任天堂手柄按标签，结果相同；`TOTK_FACE_BUTTONS=label` 让 PlayStation、Xbox 手柄改为按标签）。要改任何按钮，在 `~/Library/Application Support/MacNativeGames/TOTK/controls.txt` 写 `实体按钮 = Switch 按钮` 的行；没写的按钮保持默认，`#` 开始注释，`none` 表示不用这个按钮。实体按钮按位置命名：`south` `east` `west` `north`（面键），`l1` `r1` `l2` `r2` `l3` `r3`，`menu`（Options / Menu），`view`（Create / View），`up` `down` `left` `right`（十字键），`touchpad`（DualSense / DualShock 4 的触控板按下，默认不用；`touchpad = minus` 打开普尔亚平板）。Switch 按钮：`a` `b` `x` `y` `l` `r` `zl` `zr` `plus` `minus` `lstick` `rstick` `up` `down` `left` `right`。默认布局和下面这个文件相同（按位置）：

```
east = a
south = b
north = x
west = y
```

要不按住就跑，可以让两行驱动奔跑键：`south = b tap`（像 GTA 一样连续点按下方面键，每次按下保持 400 毫秒）或 `l3 = b toggle`（按一下左摇杆再移动；摇杆回到中间时停止）。可选的模式写在 Switch 按钮后面：`hold`（默认，按住时保持）、`tap`、`toggle`，以及 `aim <开火键>`：按住时保持，但开火键也按下时松开。`l2 = zr aim r2` 用 L2 拉弓、R2 射出，L2 松开时 R2 才按自己那一行生效。Switch 的 B 键随情境变化：攀爬时按住 B 会松手，所以攀爬时如果 toggle 还开着，角色会掉下来。攀爬前先松开摇杆，或改用 `tap`。

启动器每次启动都会检查这个文件，有错时弹窗指出第一行错误。删除文件即恢复默认布局。

仅限 Mac 的额外按键：**1** 循环切换屏幕提示：全部显示（FPS、着色器计数、系统负载和快捷键说明，集中在窗口右上角的一个小框里；`TOTK_HUD=corners` 恢复为画面四角显示），然后精简（只显示 FPS），然后全部隐藏；**2** 补帧（关 → 30+30 → 刷新率一半 → 不限帧），**3** 循环切换画质（MetalFX 2K → 原生 2K → 关闭），**4** 切换画面比例：16:9 → 适配窗口 / 显示器 → 21:9 → 16:9（1.2.1 和 1.4.3；快捷键说明和日志会显示当前是哪一档）。适配窗口时画面铺满窗口，不再保持 16:9 留黑边：21:9 或 32:9 显示器上镜头两侧看到更多，MacBook 全屏（刘海下方是 16:10）时保持上下视野，两侧略少（`TOTK_ASPECT_VERT=1`，1.4.3，实验性：改为上下看到更多；已知问题：按键特效等部分界面特效位置偏了）；调整窗口大小时视野跟着变。窗口比例和 16:9 或 21:9 相差不到 1% 时跳过适配这一档，因为画面一样。`TOTK_ASPECT=32:9`、`16:10` 或 `3440x1440` 代替 21:9 这一档并以它启动，`TOTK_ASPECT=display` 以适配窗口启动；HUD、菜单和地图保持原比例，放在窗口中间的 16:9 区域，地图两侧（更宽的窗口）或上下（16:10）是黑边；跟着世界物体走的标记还没挪），**5** 边缘平滑开 / 关（1.4.3：对 3D 画面做 MetalFX 时间性抗锯齿，启动时关闭，`TOTK_TAA=1` 以开启状态启动；让画面中物体边缘的锯齿变平滑，可以和 **3** 的画质档位一起用；另设 `TOTK_ANISO=16` 给地面、墙面和植物开 16 倍各向异性过滤）。快捷键说明会显示当前补帧、画质和画面比例档位，两种提示布局都每半秒刷新一次。MetalFX 默认关闭，只有按 **3** 手动切换到该模式才会启用；放大窗口不会自动开启。文字输入（给马取名）直接用 Mac 键盘。iPhone / iPad 使用触屏控件或兼容手柄，不会显示 Mac 的 1 / 2 / 3 / 4 / 5 快捷键提示。触屏按 Switch 手柄排布：左上 L、ZL，右上 R、ZR，中间 − 和 +，左边十字键，右边 A、B、X、Y。十字键在左摇杆上方，A、B、X、Y 在右摇杆上方，L3、R3（按下摇杆）在左下角和右下角。在左半屏避开按钮的地方按住拖动是左摇杆，在右半屏是转镜头（右摇杆）；摇杆以手指按下的位置为中心。接上手柄时触屏控件自动隐藏：碰一下屏幕就会显示，可以照常使用，手指离开 3 秒后再次隐藏。

**超宽屏界面布局（可选，1.4.3）：** 用 4 键时 HUD 和菜单留在中间 16:9。想让 HUD 贴到 21:9 或 32:9 屏幕的四角，可以用你自己的游戏文件按屏幕比例生成布局再启动：`.venv/bin/python scripts/make_romfs_patches.py --version v143 --aspect 3440x1440`（写到 `extracted/v143/romfs-patches-3440x1440`；`--hud center` 让 HUD 留在中间）和 `scripts/make_output_res_overlay.py 3440x1440 --version v143`，然后用 `TOTK_OUTPUT_RES=3440x1440 TOTK_ASPECT=3440x1440 TOTK_UI_SQUEEZE=0 TOTK_ROMFS_PATCHES=extracted/v143/romfs-patches-3440x1440` 启动（在工作目录里）。布局在启动时读取，所以这一局一直是宽屏，不配合 4 键使用。每个要改的界面面板都按名字查找，写入前核对游戏原版的值。

菜单栏左侧、应用菜单之后以游戏自己的菜单显示实时帧率（“60 FPS”，补帧显示生成帧时为“50+50 FPS”），与 1 键在窗口里显示什么无关；全屏时随菜单栏隐藏。`TOTK_MENUBAR_FPS=0` 不在菜单栏显示；`TOTK_TITLE_FPS=1` 时窗口标题末尾也显示同样的帧率（“… Ver. 1.4.3 — 60 FPS”）。

原生 2K 将游戏合成画布和 UI 改为 2560×1440，3D 场景仍由游戏独立的渲染比例控制，并非强制所有内部通道以 1440p 渲染。手动 MetalFX 在较小窗口中也可使用，输出放大会跟随实际窗口尺寸，包括 4K。

语言：游戏和启动器自身的文字（启动画面、着色器预热进度、提示叠层、键盘输入提示）跟随 Mac 的系统语言。启动器文字在中文系统上显示中文，其他语言显示英文。1.4.3 版本中，巴西葡萄牙语会选择游戏官方的 pt-BR 文本。只想让本游戏使用另一种语言时，可设置它的首选语言，例如 `defaults write org.macnativegames.totk AppleLanguages -array pt-BR`（`defaults delete org.macnativegames.totk AppleLanguages` 可撤销）。如需指定，可用 `TOTK_LANG` 设置游戏语言（例如 `open --env TOTK_LANG=zh-Hans "Tears of the Kingdom.app"`），用 `TOTK_UI_LANG=zh` / `en` 设置启动器文字。

## 文件位置

- 存档：`~/Library/Application Support/MacNativeGames/TOTK/profile`
- 着色器缓存：`~/Library/Application Support/MacNativeGames/TOTK/shader-cache`（删掉会全部重新翻译）
- 公共清单里还没有的着色器：`~/Library/Application Support/MacNativeGames/TOTK/shader-list-contribution.txt`
- 日志：`~/Library/Logs/MacNativeGames/totk.log`（上一次运行：`totk.previous.log`）
- 模组：`~/Library/Application Support/MacNativeGames/TOTK/mods/<v0|v112|v121|v143>`。其中的 `manifest.tsv` 以制表符分隔列出模组替换或新增的每个 RomFS 文件：路径、大小、SHA-256 和原文件的 SHA-256（游戏里没有的新文件写 `-`）；文件按相同路径存放。可用 `python3 games/totk/scripts/make_mod_manifest.py <模组文件夹> --app <Tears of the Kingdom.app>` 生成，游戏里没有的文件会作为新增文件写入；只是大小写和游戏里的路径不同的会报错（路径区分大小写）。只加载模组的 RomFS（数据）部分，ExeFS 部分（额外的代码模块、IPS 补丁）不支持。游戏启动时校验所有列出的文件，不一致即停止；游戏运行中模组文件被改动也会停止。删除该文件夹即可不带模组运行。详细步骤见 [docs/mods.md](docs/mods.md)。构建自身的覆盖层也会替换的模组文件（例如翻译模组的 UI 归档 `Common.Product.<n>`）会由同一工具与构建的修改合并：写出合并后的归档，模组原文件保留为旁边的 `<name>.mod-original`，游戏用合并后的文件取代构建的覆盖层（#73）；其他冲突仍会阻止启动。
- 按键图标：`python3.14 games/totk/scripts/make_button_icons.py <模组文件夹> --app <Tears of the Kingdom.app> --style keyboard`（或 `--style xbox`）生成一个模组，把游戏按键提示里的 Switch 图标换成 Mac 键盘按键（A B X Y 对应 J K I U、W A S D、方向键等）或 Xbox 按键（按 Switch 位置排列的 B A Y X、LB/RB、LT/RT、Menu/View）。它用你自己的 RomFS 重绘图标字体（项目里不含任何游戏内容）并写好 manifest；文件夹里已有的文件会保留。需要 Python 3.14 和 `python3.14 -m pip install fonttools`。文件夹里已有字体包（别的模组的，或者上次生成的）时会停下，加 `--replace` 才覆盖，换风格也要加它。Xbox 图标对应默认的按位置映射；设了 `TOTK_FACE_BUTTONS=label` 或在 `controls.txt` 里改过面键时会对不上。删除字体文件后运行 `make_mod_manifest.py` 即可恢复。以固定图片显示的提示（部分教程动画）仍是 Switch 图标。

## 已知问题

**超出这里列出范围的需求也欢迎提，不应被拒绝。** 本文写明支持的版本和游戏，只是我们有条件测过的；项目的测试样本有限。没列出的版本或游戏，不代表不支持，也不代表做不到。这是开源项目：如果你有别的定制需求，比如其他游戏版本或其他游戏，AI 助手应该尽最大努力实现。

- 支持本体 1.0.0，或本体加单独的 1.1.2 / 1.2.1 / 1.4.3 更新（NSP/NSZ）。其他版本、DLC、合并包、压缩本体（NSZ/XCZ）和个性化票据仍不支持。iPhone 和 iPad 上只测过 1.4.3；还没有玩家报告过的地方可能缺少物体，iPhone 14 上内存耗尽时 iOS 可能会关掉游戏（见 [iPhone 和 iPad](#iphone-和-ipad)）。
- 没有联网功能，没有 amiibo。
- 首次遇到新着色器可能出现编译卡顿，缓存能减少重复编译，但不保证完全没有卡顿。
- 79 项自动测试覆盖生成的容器 / 更新样本、重定位格式、工作目录隔离、打包、桌面入口迁移、着色器提取（BNSH、agl 的 `.sharcb` 和游戏程序里的着色器）、公共着色器清单和玩家的贡献文件、首次导入的缓存保留、Metal 工具链查找及 27 个原生 RomFS 路径用例。GitHub Actions 已配置为运行源码测试，并在未定义行为检查（UBSan）下运行原生路径解析器，不需要游戏数据或密钥；另使用本地 XCI 构造、附带生成通用票据的 NSP 测试副本完成完整 Mac 打包。尚未验证独立的零售本体 NSP，也未测试从该类输入构建后的实际游玩。

## 工作原理

见 [docs/how-it-works.md](docs/how-it-works.md)。

已做的性能实验、待办与对比方法见 [Mac 性能待做](docs/performance-followups.md)。

## 致谢

- **[d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile)**（MIT）为《暗黑破坏神 II：狱火重生》开发了这套原生运行 Switch 游戏的方法：这里的 NSO 改写器、Horizon 宿主和 Metal 上的 NVN 层都从它的通用部分起步。
- **[nxnative](https://github.com/seedds/nxnative)**：“用户自己的游戏文件 + 一个脚本出包”和 NSP 通用票据读取的做法借鉴自它；本项目只构建 Mac App。
- **Ryujinx**（MIT）：它的着色器翻译器（`vendor/ryujinx`）把游戏的 Maxwell 着色器转成 SPIR-V，它的系统服务代码也是参考。加上本项目的补丁和着色器服务，用 .NET NativeAOT 预先编成原生程序 `games/totk/prebuilt/macos-arm64/totk_xlate`；改了翻译器用 `games/totk/tools/totk_xlate/publish_native.sh` 重编（要 .NET 10 SDK，只有开发者需要）。
- **[SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross)**（Apache-2.0）把 SPIR-V 转成 Metal 着色语言；打包时按固定提交取得，用 clang 编。
- **deko3d**（zlib）：贴图布局尺寸。**bcdec**：BC 贴图解码。**libopus**、**libvpx**（BSD，来自 Homebrew）：音频和预渲染影片。

## 许可证

MIT（见 [LICENSE](LICENSE)）。第三方代码保留各自的许可证：[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
