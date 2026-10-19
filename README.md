# MacNativeGames

# English

> **Use your AI assistant to help you build the Mac app. We recommend Claude Code, Codex, or any other AI assistant you prefer.**

[English](#english) | [中文](#chinese)

## Legal

**You must own the game.** Use this project only with a copy of The Legend of Zelda: Tears of the Kingdom that you bought, and with keys and system files dumped from your own Nintendo Switch. This is an independent, non-commercial project. It is not affiliated with, endorsed by, or sponsored by Nintendo. The Legend of Zelda and Nintendo Switch are trademarks of Nintendo.

The repository contains no game content and distributes none: no game program, art, text, audio, shaders or keys. Everything the game needs is made on your own computer from your own files when you build the app. The app you build contains the game, so it is for your own use only; do not share it. What the repository does hold is the information needed to make the port work: the patch code, the addresses and hashes used to check your files, and the [shared shader list](docs/shader-list.md), which names shaders and bind states but contains no shader code.

Play your own copy of **The Legend of Zelda: Tears of the Kingdom** natively on Apple silicon Mac, iPhone, and iPad. Bring your game file (XCI or NSP) and your `prod.keys`; one script builds a Mac app or an unsigned iPhone/iPad IPA. The iOS build requires iOS/iPadOS 26 and iPhone 14 or newer. It is not an emulator: the game's own ARM64 code runs directly on Apple chips (rewritten where the operating system requires it), the Switch system services are answered by a small host, and the game's NVN graphics are rebuilt on Metal. Nothing from the game is in this repository, and the project is not affiliated with Nintendo. Version 1.4.3 has been played on an iPhone 14 and an iPad; see [iPhone and iPad](#iphone-and-ipad).

## Let's maintain the shader list together: no more stutter

> [!TIP]
> **Every shader in the shared list is ready before you play.** On a Mac the game does not stop to compile it, and on an iPhone or iPad nothing is missing from the picture. The more places players cover, the smoother the game gets for everyone.

The [shared shader list](docs/shader-list.md) now holds **26,832 shader variants** (23,867 shaders) from the Mac, iPhone and iPad. Every build makes them from the player's own game files, so the list holds identifiers only, never game data. Helping takes no setup:

1. **Play.** When your game meets a shader the list does not have yet, it notes it in `shader-list-contribution.txt`.
2. **On a Mac, every 500 new shaders it asks you once, before the game starts.** Click "Send": the GitHub form opens and the file is shown, ready to drag in. An iPhone or iPad never asks during play: while it is connected to the Mac that built its app (cable or the same Wi-Fi), the Mac asks instead. "Don't remind me again" stops the reminders.
3. **Everyone's next build carries them.** Nobody stutters there again.

**[Send your shaders →](https://github.com/ly05010419/MacNativeGames/issues/new?template=shader-list.yml)** · The game never uploads anything by itself.

Update history:

| Date | Platform | Update |
|---|---|---|
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
- About **25 GB** of free disk space for the work folder, in addition to your game file. Keep the output app on the **same APFS volume** to share the large data files through cloning. If cloning is unavailable or the app is on another volume, allow roughly **17 GB more** for the app copy.
- Your Tears of the Kingdom **base game 1.0.0** (`0100F2C0115B6000`) as an **XCI or NSP**, and your `prod.keys`. To build **1.1.2, 1.2.1 or 1.4.3**, supply its separate cumulative update as NSP or NSZ with `--update`; no intermediate update is required. NSZ is decompressed and its NCA hashes are verified before extraction. DLC and combined packages are unsupported.
- For NSP: its common ticket must be included unless the NCAs are rights-free. NSPs do not contain system shared fonts: supply an unpacked firmware NCA directory with `--firmware`, or previously extracted fonts with `--shared-fonts`. The six fonts must match this project’s verified hashes; valid fonts already in the work folder are reused.

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

Without `-o`, the selected output appears next to the game file (`Tears of the Kingdom.app` on Mac or `Tears of the Kingdom.ipa` for iOS). Before full shader preparation was added, a local clean base-1.0.0 Mac build took about 6 minutes on an M4 Mac mini (this is not a timing estimate for the current build; hardware, disk and network conditions affect the time): about a minute each to rewrite the main code module and to decrypt the 17 GB of game data, and two to three minutes for macOS to check the finished app once. The current build then also prepares the startup shaders (about 3 minutes for 1.4.3 on an M4) and compiles them for your GPU (a few more minutes; without the Metal toolchain this step is skipped, with a message, while a game is running). A Mac build needs no .NET or CMake, and the finished game needs no Python: its shader translator is a native program that comes with this repository (`games/totk/prebuilt`). Every intermediate file is in a work folder next to the game file (`TOTK_build`, or `--work`); running the script again reuses what is already done.

The extracted code modules, rewritten modules, RomFS and shared fonts are checked against the selected version's `games/totk/assets/expected-v1.0.0.json`, `expected-v1.1.2.json`, `expected-v1.2.1.json` or `expected-v1.4.3.json`; the prebuilt shader translator has its own SHA-256 check. A mismatch stops the build. Key values are never printed.

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

### iPhone and iPad

Version 1.4.3 has been played on an **iPhone 14 (iOS 26.6.1)** and an **iPad with the A16 chip (iPadOS 26.7.1)**, both signed with a personal development team in Xcode. Versions 1.0.0, 1.1.2 and 1.2.1 have not been tested on iOS.

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

Mac-only extra keys: **1** show or hide all on-screen overlays (FPS, shader count, system load and the shortcut reminder, together in one small box in the window's top-right corner; `TOTK_HUD=corners` puts the readings back in the picture's corners), **2** frame interpolation (off → 30+30 → half the refresh rate → unlimited), **3** cycle picture mode (MetalFX 2K → native 2K → off), **4** ultrawide 21:9 on / off (1.2.1 and 1.4.3: the camera sees more at the sides and the picture fills a wider window; `TOTK_ASPECT=32:9` or `3440x1440` picks another shape and starts with it on; the HUD, menus and map keep their shape in the middle 16:9 of the window, the map with black bars at the sides; markers that follow objects in the world are not moved out yet). MetalFX is off by default and only starts when you select its mode with **3**; resizing the window never enables it automatically. Text entry (naming a horse) uses the Mac keyboard. iPhone/iPad uses touch controls or a compatible game controller and does not show the Mac 1/2/3/4 shortcut prompts. The touch layout follows a Switch controller: L and ZL at the top left, R and ZR at the top right, − and + between them, the d-pad on the left and A, B, X, Y on the right. The d-pad sits above the left stick and A, B, X, Y above the right stick, and L3 and R3 (stick presses) are in the bottom corners. Touch the left half away from the buttons and drag for the left stick, the right half for the camera (right stick); each stick is centred where your finger goes down. With a game controller connected the touch controls are hidden: touch the screen to show and use them, and they hide again 3 seconds after you lift your fingers.

Native 2K changes the game's composition canvas and UI to 2560×1440. The 3D scene still follows the game's separate render scaling; this does not force every internal render target to 1440p. The manual MetalFX mode also works in smaller windows and scales to the actual output size, including 4K.

Language: the game and the launcher's own text (startup screen, shader warm-up progress, overlays, keyboard prompt) follow the Mac's language. The launcher text is Chinese on a Chinese Mac and English otherwise. To override, launch with `TOTK_LANG` for the game (for example `open --env TOTK_LANG=en-US "Tears of the Kingdom.app"`) or `TOTK_UI_LANG=zh` / `en` for the launcher text.

## Where things are

- Saves: `~/Library/Application Support/MacNativeGames/TOTK/profile`
- Shader cache: `~/Library/Application Support/MacNativeGames/TOTK/shader-cache` (delete it to translate everything again)
- Shaders the shared list does not have yet: `~/Library/Application Support/MacNativeGames/TOTK/shader-list-contribution.txt`
- Log: `~/Library/Logs/MacNativeGames/totk.log` (the run before: `totk.previous.log`)

## Known issues

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

> **请使用你的 AI 助理帮你完成 Mac App 的构建。推荐 Claude Code、Codex，或其他你喜欢的 AI 助理。**

[英文说明](#english) | [中文说明](#chinese)

## 法律声明

**你必须拥有正版游戏。** 请只配合你自己购买的《塞尔达传说 王国之泪》，以及从你自己的 Nintendo Switch 导出的密钥和系统文件使用本项目。这是一个独立的非商业项目，与任天堂没有关联，也未获其认可或赞助。The Legend of Zelda（塞尔达传说）和 Nintendo Switch 是任天堂的商标。

本仓库不含、也不分发任何游戏内容：没有游戏程序、美术、文字、音频、着色器或密钥。游戏需要的一切，都是打包时在你自己的电脑上、用你自己的文件生成的。打包出的 App 里含有游戏，只供你自己使用，请不要分享。仓库里有的是让移植能运行的信息：补丁代码、用来核对你的文件的地址和哈希，以及[公共着色器清单](docs/shader-list.md)（只记着色器和绑定状态的标识，不含着色器代码）。

在 Apple 芯片的 Mac、iPhone 和 iPad 上原生运行你自己拥有的**《塞尔达传说 王国之泪》**。准备好游戏文件（XCI 或 NSP）和你的 `prod.keys`，一个脚本即可构建 Mac App 或未签名的 iPhone / iPad IPA。iOS 构建需要 iOS / iPadOS 26，最低支持 iPhone 14。这不是模拟器：游戏自己的 ARM64 代码直接运行在苹果芯片上（按操作系统要求改写），Switch 的系统服务由一个小宿主应答，游戏的 NVN 图形在 Metal 上重建。本仓库不含任何游戏内容，项目与任天堂无关。1.4.3 已在 iPhone 14 和 iPad 上实际游玩，见 [iPhone 和 iPad](#iphone-和-ipad)。

## 让我们一起维护着色器清单：游戏不再卡顿

> [!TIP]
> **公共清单里的每个着色器，开玩之前就已经准备好。** Mac 上不用停下来现场编译，iPhone 和 iPad 上画面也不会缺东西。大家玩过的地方越多，所有人玩得越流畅。

[公共着色器清单](docs/shader-list.md)现在有 **26,832 个着色器变体**（23,867 个着色器），来自 Mac、iPhone 和 iPad。每次打包都用玩家自己的游戏文件生成，所以清单里只有标识，不含任何游戏数据。参与不需要任何设置：

1. **照常玩。** 游戏遇到清单里还没有的着色器，会自动记进 `shader-list-contribution.txt`。
2. **在 Mac 上，每攒够 500 个，它会在游戏启动前问你一次。** 点“发送”，GitHub 提交页面会打开，文件也会显示出来，拖进去就行。iPhone 和 iPad 玩的时候从不提醒：设备连着打包它的那台 Mac（线连或同一 Wi‑Fi）时，由 Mac 来问。点“不要提醒了”以后就不再问。
3. **下次大家打包的 App 都会带上。** 谁到那里都不会再卡顿。

**[发送你的着色器 →](https://github.com/ly05010419/MacNativeGames/issues/new?template=shader-list.yml)** · 游戏不会自己上传任何东西。

更新历史：

| 日期 | 平台 | 更新 |
|---|---|---|
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
- 除游戏文件外，为工作目录预留约 **25 GB** 空闲空间。成品 App 与工作目录放在**同一个 APFS 卷**上时，大型数据文件可通过克隆共享磁盘空间；无法克隆或输出到其他卷时，还需为 App 副本另留约 **17 GB**。
- 你的《王国之泪》**本体 1.0.0**（`0100F2C0115B6000`），格式为 **XCI 或 NSP**，以及你的 `prod.keys`。构建 **1.1.2、1.2.1 或 1.4.3** 时，通过 `--update` 提供单独的累计更新 NSP 或 NSZ；无需先安装中间版本。NSZ 会先解压并校验其中的 NCA 哈希。DLC 和合并包不支持。
- 使用 NSP 时：除不依赖票据的 NCA 外，包内必须包含通用票据。NSP 不含系统共享字体，需要通过 `--firmware` 指定已解包的固件 NCA 目录，或通过 `--shared-fonts` 提供以前提取的字体。六个字体必须符合本项目已验证的哈希；工作目录中已有且校验通过的字体会自动复用。

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

不使用 `-o` 时，选定的平台成品会放在游戏文件旁边（Mac 为 `Tears of the Kingdom.app`，iOS 为 `Tears of the Kingdom.ipa`）。加入完整着色器准备前，本地一次本体 1.0.0 Mac 全新构建在 M4 Mac mini 上约 6 分钟（这不是当前构建的耗时预估；耗时受硬件、磁盘和网络影响）：改写主代码模块、解密 17 GB 的游戏数据各约 1 分钟，macOS 把做好的 App 检查一遍要 2–3 分钟。现在的打包还会准备启动着色器（1.4.3 在 M4 上约 3 分钟），再为本机 GPU 编译（再要几分钟；没有 Metal 工具链时，有游戏开着会跳过这一步，并提示）。Mac 打包不需要 .NET 或 CMake，做好的游戏运行时也不需要 Python：着色器翻译器是本仓库自带的原生程序（`games/totk/prebuilt`）。所有中间文件都在游戏文件旁边的工作目录里（`TOTK_build`，或用 `--work` 指定），再运行一次会复用已经做好的部分。

提取的程序模块、改写后的模块、RomFS 和共享字体会与所选版本的 `games/totk/assets/expected-v1.0.0.json`、`expected-v1.1.2.json`、`expected-v1.2.1.json` 或 `expected-v1.4.3.json` 中的哈希核对；预编译着色器翻译器另有 SHA-256 校验。不匹配时停止构建。密钥的值不会被打印。

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

### iPhone 和 iPad

1.4.3 已在 **iPhone 14（iOS 26.6.1）** 和 **A16 芯片的 iPad（iPadOS 26.7.1）** 上实际游玩，两台都是在 Xcode 里用个人开发团队签名安装的。1.0.0、1.1.2 和 1.2.1 没有在 iOS 上测过。

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

仅限 Mac 的额外按键：**1** 同时隐藏或重新显示所有屏幕提示（FPS、着色器计数、系统负载和快捷键说明，集中在窗口右上角的一个小框里；`TOTK_HUD=corners` 恢复为画面四角显示）；**2** 补帧（关 → 30+30 → 刷新率一半 → 不限帧），**3** 循环切换画质（MetalFX 2K → 原生 2K → 关闭），**4** 21:9 超宽屏开 / 关（1.2.1 和 1.4.3：镜头两侧看到更多，画面铺满更宽的窗口；`TOTK_ASPECT=32:9` 或 `3440x1440` 换成别的比例并以宽屏启动；HUD、菜单和地图保持原比例，放在窗口中间的 16:9 区域，地图两侧是黑边；跟着世界物体走的标记还没往两侧挪）。MetalFX 默认关闭，只有按 **3** 手动切换到该模式才会启用；放大窗口不会自动开启。文字输入（给马取名）直接用 Mac 键盘。iPhone / iPad 使用触屏控件或兼容手柄，不会显示 Mac 的 1 / 2 / 3 / 4 快捷键提示。触屏按 Switch 手柄排布：左上 L、ZL，右上 R、ZR，中间 − 和 +，左边十字键，右边 A、B、X、Y。十字键在左摇杆上方，A、B、X、Y 在右摇杆上方，L3、R3（按下摇杆）在左下角和右下角。在左半屏避开按钮的地方按住拖动是左摇杆，在右半屏是转镜头（右摇杆）；摇杆以手指按下的位置为中心。接上手柄时触屏控件自动隐藏：碰一下屏幕就会显示，可以照常使用，手指离开 3 秒后再次隐藏。

原生 2K 将游戏合成画布和 UI 改为 2560×1440，3D 场景仍由游戏独立的渲染比例控制，并非强制所有内部通道以 1440p 渲染。手动 MetalFX 在较小窗口中也可使用，输出放大会跟随实际窗口尺寸，包括 4K。

语言：游戏和启动器自身的文字（启动画面、着色器预热进度、提示叠层、键盘输入提示）跟随 Mac 的系统语言。启动器文字在中文系统上显示中文，其他语言显示英文。如需指定，可用 `TOTK_LANG` 设置游戏语言（例如 `open --env TOTK_LANG=zh-Hans "Tears of the Kingdom.app"`），用 `TOTK_UI_LANG=zh` / `en` 设置启动器文字。

## 文件位置

- 存档：`~/Library/Application Support/MacNativeGames/TOTK/profile`
- 着色器缓存：`~/Library/Application Support/MacNativeGames/TOTK/shader-cache`（删掉会全部重新翻译）
- 公共清单里还没有的着色器：`~/Library/Application Support/MacNativeGames/TOTK/shader-list-contribution.txt`
- 日志：`~/Library/Logs/MacNativeGames/totk.log`（上一次运行：`totk.previous.log`）

## 已知问题

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
