# MacNativeGames

# English

> **Use your AI assistant to help you build the Mac app. We recommend Claude Code, Codex, or any other AI assistant you prefer.**

[English](#english) | [中文](#chinese)

Play your own copy of **The Legend of Zelda: Tears of the Kingdom** natively on an Apple silicon Mac. Bring your game file (XCI or NSP) and your `prod.keys`; one script turns them into a Mac app. This project currently supports **Mac only**. It is not an emulator: the game's own ARM64 code runs directly on the Apple chip (rewritten only where macOS requires it), the Switch system services are answered by a small host, and the game's NVN graphics are rebuilt on Metal 4. Nothing from the game is in this repository, and the project is not affiliated with Nintendo.

Update history:

October 18: Added 1.4.3 support and shader prewarming before the first play session (about 10 minutes).
October 17: Added 1.0.0 and 1.1.2 support. First-launch shader prewarming was not available yet, so new scenes could stutter.


**Stuttering when you first start playing or enter a new scene is expected while new shaders compile—don't be surprised.** Performance typically improves as the cache warms up.

## Audio fix

Streamed music and dialogue lookup for `content://Sound/...` paths has been fixed. Opening dialogue and music were verified locally on version 1.0.0; other versions and scenes still need validation. Rebuild your app with the original game inputs to apply the fix. See [audio troubleshooting and validation](docs/audio-troubleshooting.md) for details.

## Local testing

Tested on **MacBook Air M4 and Mac mini M4**. Local testing has collected **8,582 shaders**, with performance at approximately **90 FPS after shader warm-up, reaching 120 FPS in menus**.

Those are earlier local observations, not a stable frame-rate promise for every version or scene. Version 1.4.3 measurements and compilation stalls are recorded separately in [the test notes](docs/v1.4.3-port.md).

Shaders are automatically collected, compiled and cached as you play. Returning to previously visited scenes typically brings noticeable improvements in frame rates and smoothness because cached shaders can be reused. **8,582 is the number collected so far in local testing, not the game's total shader count or a collection limit**; the cache continues to grow as you explore.

**Shader prewarming on the first launch:** Building the app automatically prepares at least 8,000 shaders from your selected game data and includes them in the app. Even with an empty user cache, the first launch discovers and warms these shaders before the first frame; compatible entries in the pipeline manifest are also warmed. No preliminary play session or manual cache copy is needed. The current 1.4.3 build prepares 8,290 shaders. The first GPU warm-up can take several minutes; see [preparation and test details](docs/first-launch-shaders.md). This is the extractable game shader set, not every binding-dependent variant encountered while playing; new scenes can still compile additional shaders. Playing adds newly encountered shaders and pipelines to your local `pso_collected.tsv`, which is reused on later launches. To share your collected list, send `~/Library/Application Support/MacNativeGames/TOTK/shader-cache/rt-totk-native-v4-unverified/pso_collected.tsv`. It contains shader and pipeline identifiers, not game saves or compiled shader binaries. Lists are tied to the matching translation-chain version.


**iPhone and iPad support is coming soon. iPhone 16 or newer models are recommended for the upcoming iPhone version.**


## What you need

- A Mac with Apple silicon and **macOS 26** or later (the graphics use Metal 4); macOS 27 is supported. 16 GB of memory or more is recommended.
- About **25 GB** of free disk space for the work folder, in addition to your game file. Keep the output app on the **same APFS volume** to share the large data files through cloning. If cloning is unavailable or the app is on another volume, allow roughly **17 GB more** for the app copy.
- Your Tears of the Kingdom **base game 1.0.0** (`0100F2C0115B6000`) as an **XCI or NSP**, and your `prod.keys`. To build **1.1.2 or 1.4.3**, supply its separate cumulative update as NSP or NSZ with `--update`; no intermediate update is required. NSZ is decompressed and its NCA hashes are verified before extraction. DLC and combined packages are unsupported.
- For NSP: its common ticket must be included unless the NCAs are rights-free. NSPs do not contain system shared fonts: supply an unpacked firmware NCA directory with `--firmware`, or previously extracted fonts with `--shared-fonts`. The six fonts must match this project’s verified hashes; valid fonts already in the work folder are reused.

## Build the app

**1. Tools** (once): the Xcode command line tools and [Homebrew](https://brew.sh). An internet connection is needed to install the build dependencies and fetch the pinned SPIRV-Cross source:

```sh
xcode-select --install
brew install opus libvpx python@3.14
```

Optional: with Xcode's Metal toolchain (`xcodebuild -downloadComponent MetalToolchain`), the build compiles the startup shaders into the app. Without it, they are compiled on the first launch, and again after the app is moved or rebuilt.

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

`--firmware` accepts a directory of `.nca` files (unpack a firmware archive first), or an installed firmware `registered` directory. `--shared-fonts` accepts the extracted directory containing `0-standard.bfttf` through `5-nintendo-extension.bfttf`. No fonts or keys are downloaded. For an XCI, fonts are read from its system update partition by default; if that partition is missing, use one of these font options too. Both input formats produce a macOS `.app`.

The app appears next to the game file (`Tears of the Kingdom.app`; `-o` chooses another place). Before full shader preparation was added, a local clean base-1.0.0 build took about 6 minutes on an M4 Mac mini (this is not a timing estimate for the current build; hardware, disk and network conditions affect the time): about a minute each to rewrite the main code module and to decrypt the 17 GB of game data, and two to three minutes for macOS to check the finished app once. Building needs no .NET and no cmake, and the finished game needs no Python: its shader translator is a native program that comes with this repository (`games/totk/prebuilt`). Every intermediate file is in a work folder next to the game file (`TOTK_build`, or `--work`); running the script again reuses what is already done.

The extracted code modules, rewritten modules, RomFS and shared fonts are checked against the selected version's `games/totk/assets/expected-v1.0.0.json`, `expected-v1.1.2.json` or `expected-v1.4.3.json`; the prebuilt shader translator has its own SHA-256 check. A mismatch stops the build. Key values are never printed.

Signing and verifying a large updated app can add several minutes. Keep the build running while it continues to report progress.

**3. Play:** open the app from Finder. The build starts the launcher in `--check` mode, which checks that the game executable exists and allows macOS to perform its first-launch checks; it does not run a gameplay test. You can keep the app where it was built or move it to `/Applications`. Copying it to a different volume can use additional disk space and trigger another macOS check, during which no window may appear. The first game launch verifies the game data (about 20 seconds in local testing). Newly encountered shaders can cause stutters; cached shaders reduce this work on later visits.


### Build version 1.1.2 or 1.4.3

```sh
python3.14 build_app.py ~/Games/totk.xci -k ~/keys/prod.keys --update ~/Games/1.1.2.nsp
# NSZ updates are also accepted:
python3.14 build_app.py ~/Games/totk.xci -k ~/keys/prod.keys --update ~/Games/1.1.2.nsz
# Or build 1.4.3 directly from the same base:
python3.14 build_app.py ~/Games/totk.xci -k ~/keys/prod.keys --update ~/Games/1.4.3.nsz
# With a base NSP, keep --firmware or --shared-fonts as described above.
```

This produces the same **Tears of the Kingdom.app**, containing only the selected version. Quit the running game, then rebuild to the same `-o` path to replace the old app; there is no second game entry or version-selection menu. The window title shows the selected version, such as **Ver. 1.4.3**. Without `--update`, the script still builds 1.0.0.

You do not need to download separate 1.4.3 runtime tables or a shader cache. The build generates the version-specific runtime tables and first-launch shader seed from the game data you provide, then includes the shader seed in the app. The repository supplies the 1.4.3 patch code and verified hashes; only your base game, update and keys are external inputs.

Allow about **40 GB free for a fresh update build**, or **19 GB extra** when the base data is already cached, with the app on the same APFS volume. The merge reads the base NCA through a bounded range of the original container, temporarily extracts the much smaller update NCA, and verifies every IVFC level. It fetches a pinned [hactool](https://github.com/SciresM/hactool) source and builds it with clang/make. Temporary ticket files are private and removed after the merge; key values are not passed on the command line or printed.

Saves stay in the same profile folder. Before the first updated launch, the app makes a complete backup beside it, such as `profile-before-1.1.2` or `profile-before-1.4.3`; failure stops startup. Keep this backup if you need to return to an older version—do not assume an updated save is backward compatible.

To keep one desktop entry, link the packaged app with the optional installer:

```sh
python3.14 games/totk/tools/install_desktop_entry.py --app "/path/to/Tears of the Kingdom.app"
```

When replacing the older development launcher, the installer copies and verifies its saves into the standard user profile listed below and creates an upgrade backup. It preserves the original profile and archives the old launcher without adding another app entry. An existing, different standard profile is never overwritten.

The source runtime passed basic Mac checks: load, movement/jumps, menus/map, 2K, save/reload and death retry. Full-map, long-session and fair 1.0/1.1.2 performance comparisons remain pending; the earlier FPS observations are not a promise of 1.1.2 performance. Packaging validation is recorded in [the build notes](docs/how-it-works.md#112-validation).

The 1.4.3 port has separately passed scene loading, movement/jumps, menus/map, save/reload, native 2K canvas switching and MetalFX upscaling/interpolation on an M4 Mac. The single packaged app has been tested, including save/reload, death/retry and shrine entry/exit. A three-hour earlier build and a 30-minute final-build run are recorded separately; first-use compilation stalls, long-session memory growth and audio underruns remain under investigation. See [the 1.4.3 test record](docs/v1.4.3-port.md) for the scope and follow-ups; this is not full-game or stable-FPS certification.

## Controls

Controllers recognized by macOS (such as DualSense and Xbox controllers) are mapped to a Switch Pro Controller. Rumble and motion require the corresponding features to be exposed by the controller and macOS; not every controller has a gyroscope. Keyboard mappings use physical key positions corresponding to a US layout:

| Key | Switch | | Key | Switch |
|---|---|---|---|---|
| W A S D | left stick | | arrow keys | right stick |
| J | A | | K, Space | B |
| I | X (jump) | | U | Y (attack) |
| Q / E | L / R | | Option / Shift | ZL / ZR |
| F T H G | d-pad left / up / right / down | | Z / C | left / right stick press |
| Return | + (menu) | | Tab, Esc | − (map) |

Extra keys: **1** show or hide all on-screen overlays (FPS, shader count, system load and the shortcut reminder, together in one small box in the top-right corner; `TOTK_HUD=corners` puts the readings back in the picture's corners), **2** frame interpolation (off → 30+30 → half the refresh rate → unlimited), **3** cycle picture mode (MetalFX 2K → native 2K → off). MetalFX is off by default and only starts when you select its mode with **3**; resizing the window never enables it automatically. Text entry (naming a horse) uses the Mac keyboard.

Native 2K changes the game's composition canvas and UI to 2560×1440. The 3D scene still follows the game's separate render scaling; this does not force every internal render target to 1440p. The manual MetalFX mode also works in smaller windows and scales to the actual output size, including 4K.

Language: the game and the launcher's own text (startup screen, shader warm-up progress, overlays, keyboard prompt) follow the Mac's language. The launcher text is Chinese on a Chinese Mac and English otherwise. To override, launch with `TOTK_LANG` for the game (for example `open --env TOTK_LANG=en-US "Tears of the Kingdom.app"`) or `TOTK_UI_LANG=zh` / `en` for the launcher text.

## Where things are

- Saves: `~/Library/Application Support/MacNativeGames/TOTK/profile`
- Shader cache: `~/Library/Application Support/MacNativeGames/TOTK/shader-cache` (delete it to translate everything again)
- Log: `~/Library/Logs/MacNativeGames/totk.log` (the run before: `totk.previous.log`)

## Known issues

- Supports base 1.0.0, or base plus the separate 1.1.2 or 1.4.3 update NSP/NSZ. Other versions, DLC, combined packages, compressed base games (NSZ/XCZ) and personalized tickets remain unsupported. Mac only.
- No online features, no amiibo.
- New shaders can cause compilation stutters. Cache reuse reduces these, but does not guarantee stutter-free gameplay.
- The 38 automated tests cover generated container/update fixtures, relocation formats, work-folder isolation, packaging, desktop migration, shader extraction, first-import cache preservation and 27 native RomFS path cases. GitHub Actions is configured to run the source tests and native path-parser sanitizers without game data or keys. A complete Mac build also passed with a local XCI-derived NSP fixture and generated common ticket. An independent retail base NSP and gameplay from that input have not yet been tested.

## How it works

See [docs/how-it-works.md](docs/how-it-works.md).

Recorded performance experiments, remaining work and comparison methods: [Mac performance follow-ups](docs/performance-followups.md).

## Acknowledgements

- **[d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile)** (MIT) developed this way of running Switch games natively (for Diablo II: Resurrected): the NSO rewriter, the Horizon host and the NVN-on-Metal layer here started as its generic parts.
- **[nxnative](https://github.com/seedds/nxnative)**: the one-script packaging and NSP common-ticket import follow its approach; this project builds only Mac apps.
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

在 Apple 芯片的 Mac 上原生运行你自己拥有的**《塞尔达传说 王国之泪》**。准备好游戏文件（XCI 或 NSP）和你的 `prod.keys`，一个脚本就把它们做成一个 Mac App。项目目前**只支持 Mac**。这不是模拟器：游戏自己的 ARM64 代码直接跑在苹果芯片上（只改写 macOS 不允许的地方），Switch 的系统服务由一个小宿主应答，游戏的 NVN 图形在 Metal 4 上重建。本仓库不含任何游戏内容，项目与任天堂无关。

更新历史：

10 月 18 日：支持 1.4.3，并加入首次游玩前的着色器预热（约 10 分钟）。
10 月 17 日：支持 1.0.0 和 1.1.2；当时尚未加入首次启动着色器预热，进入新场景可能卡顿。

**第一次进入游戏或新场景时，新着色器的编译可能造成卡顿，这是预期现象，不必惊讶。** 随着着色器缓存逐步预热，表现通常会改善。

## 音频修复

已修复 `content://Sound/...` 路径导致的流式音乐和对白读取失败。已在 1.0.0 开场实测对白和配乐，其他版本和场景仍需验证。使用原有游戏输入重新构建 App 即可应用修复，详见[音频排障与验证说明](docs/audio-troubleshooting.md)。

## 本机测试

已在 **MacBook Air M4 和 Mac mini M4** 上测试。本机测试已收集 **8,582 个着色器**，着色器预热后约 **90 帧/秒，菜单中可达 120 帧/秒**。

以上是此前本机观察，不代表所有版本和场景都能稳定达到该帧率。1.4.3 的场景测量及首次编译卡顿另见[测试记录](docs/v1.4.3-port.md)。

着色器会在游玩过程中自动收集、编译并缓存。再次进入已经去过的场景时，可以复用已有缓存，帧数和流畅度通常会明显改善。**8,582 只是本机测试目前收集到的数量，不是游戏的着色器总数，也不是收集上限**；随着探索更多场景，缓存还会继续增加。

**首次启动的着色器预热：** 构建时自动从你提供的所选版本游戏数据准备至少 8,000 个着色器，并装进 App。即使用户缓存为空，第一次打开也会发现并预热这批着色器，准备好后才显示第一帧；兼容的管线清单也会参与预热，不需要先玩一遍或手动复制缓存。当前 1.4.3 准备 8,290 个；首次 GPU 预热可能需要数分钟，详见[准备过程与实测记录](docs/first-launch-shaders.md)。这是从游戏资源提取的集合，并非游玩中所有绑定状态的动态变体，进入新场景仍可能现编。游玩时新遇到的着色器和管线会写入本机的 `pso_collected.tsv`，下次启动时继续预热。如需分享自己收集的清单，请提供 `~/Library/Application Support/MacNativeGames/TOTK/shader-cache/rt-totk-native-v4-unverified/pso_collected.tsv`。它只包含着色器和管线标识，不含游戏存档或编译后的着色器文件。清单与对应的着色器转换链版本绑定。


**即将支持 iPhone 和 iPad。未来的 iPhone 版本推荐使用 iPhone 16 及更新机型。**


## 需要准备

- Apple 芯片的 Mac，**macOS 26** 或更高（图形用 Metal 4），支持 macOS 27。建议使用 16 GB 或更多内存。
- 除游戏文件外，为工作目录预留约 **25 GB** 空闲空间。成品 App 与工作目录放在**同一个 APFS 卷**上时，大型数据文件可通过克隆共享磁盘空间；无法克隆或输出到其他卷时，还需为 App 副本另留约 **17 GB**。
- 你的《王国之泪》**本体 1.0.0**（`0100F2C0115B6000`），格式为 **XCI 或 NSP**，以及你的 `prod.keys`。构建 **1.1.2 或 1.4.3** 时，通过 `--update` 提供单独的累计更新 NSP 或 NSZ；无需先安装中间版本。NSZ 会先解压并校验其中的 NCA 哈希。DLC 和合并包不支持。
- 使用 NSP 时：除不依赖票据的 NCA 外，包内必须包含通用票据。NSP 不含系统共享字体，需要通过 `--firmware` 指定已解包的固件 NCA 目录，或通过 `--shared-fonts` 提供以前提取的字体。六个字体必须符合本项目已验证的哈希；工作目录中已有且校验通过的字体会自动复用。

## 打包 App

**1. 工具**（只需一次）：Xcode 命令行工具和 [Homebrew](https://brew.sh)。安装构建依赖和获取固定版本的 SPIRV-Cross 源码需要联网：

```sh
xcode-select --install
brew install opus libvpx python@3.14
```

可选：装有 Xcode 的 Metal 工具链（`xcodebuild -downloadComponent MetalToolchain`）时，构建会把启动着色器预先编译进 App。没有它时，首次启动会编译这些着色器，移动或重新构建 App 后也会再编译一次。

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

`--firmware` 接受包含 `.nca` 文件的目录（固件压缩包需先解压），也接受已安装固件的 `registered` 目录。`--shared-fonts` 接受包含 `0-standard.bfttf` 至 `5-nintendo-extension.bfttf` 的提取目录。工具不会下载字体或密钥。XCI 默认从其系统更新分区提取字体；如果该分区缺失，也可以使用上述字体选项。两种输入格式最终都生成 macOS `.app`。

App 出现在游戏文件旁边（`Tears of the Kingdom.app`；`-o` 指定别处）。加入完整着色器准备前，本地一次本体 1.0.0 全新构建在 M4 Mac mini 上约 6 分钟（这不是当前构建的耗时预估；耗时受硬件、磁盘和网络影响）：改写主代码模块、解密 17 GB 的游戏数据各约 1 分钟，macOS 把做好的 App 检查一遍要 2–3 分钟。打包不需要 .NET 和 cmake，做好的游戏运行时也不需要 Python：着色器翻译器是本仓库自带的原生程序（`games/totk/prebuilt`）。所有中间文件都在游戏文件旁边的工作目录里（`TOTK_build`，或用 `--work` 指定），再运行一次会复用已经做好的部分。

提取的程序模块、改写后的模块、RomFS 和共享字体会与所选版本的 `games/totk/assets/expected-v1.0.0.json`、`expected-v1.1.2.json` 或 `expected-v1.4.3.json` 中的哈希核对；预编译着色器翻译器另有 SHA-256 校验。不匹配时停止构建。密钥的值不会被打印。

大型更新版 App 的完整签名和校验还可能多用几分钟；日志持续报告进度时，请等待构建完成。

**3. 游玩：** 在访达里打开 App。构建结束时会以 `--check` 模式启动启动器，检查游戏可执行文件是否存在，并让 macOS 执行首次启动检查；这一步不进行游戏游玩测试。App 可以留在原位置，也可以移到 `/Applications`。拷贝到另一个卷可能额外占用磁盘空间，并触发 macOS 再次检查，期间可能没有窗口。第一次进入游戏会校验游戏数据（本地测试约 20 秒）。首次遇到的着色器可能造成编译卡顿，缓存可减少后续访问时的重复工作。


### 构建 1.1.2 或 1.4.3

```sh
python3.14 build_app.py ~/Games/totk.xci -k ~/keys/prod.keys --update ~/Games/1.1.2.nsp
# 也可以使用 NSZ 更新包：
python3.14 build_app.py ~/Games/totk.xci -k ~/keys/prod.keys --update ~/Games/1.1.2.nsz
# 也可从同一本体直接构建 1.4.3：
python3.14 build_app.py ~/Games/totk.xci -k ~/keys/prod.keys --update ~/Games/1.4.3.nsz
# 本体为 NSP 时，继续提供上文的 --firmware 或 --shared-fonts。
```

仍然只生成同一个 **Tears of the Kingdom.app**，包内只含选定版本。先退出正在运行的游戏，再用相同 `-o` 路径重新构建即可替换旧 App，不增加第二个游戏入口或版本选择菜单。窗口标题显示选定版本，例如 **Ver. 1.4.3**。不加 `--update` 时仍构建 1.0.0。

无需另外下载 1.4.3 运行时表或着色器缓存。构建流程会根据你提供的游戏数据生成对应版本的运行时表和首启着色器种子，并将种子放进 App。仓库已包含 1.4.3 补丁代码和校验哈希；外部只需准备自己的本体、更新包和密钥。

首次完整更新构建建议预留约 **40 GB**，已缓存本体数据时还需约 **19 GB** 空间，App 需放在同一 APFS 卷。合并时通过原始容器的限定区间读取本体 NCA，只临时提取较小的更新 NCA，并校验全部 IVFC 层；工具会获取固定提交的 [hactool](https://github.com/SciresM/hactool)，用 clang / make 编译。临时票据文件仅当前用户可读，合并结束即清理；密钥值不进入命令行或日志。

存档继续保存在原 profile 目录。首次启动更新版前，App 会在旁边完整备份为 `profile-before-1.1.2` 或 `profile-before-1.4.3`；备份失败就不启动。需要回退旧版本时保留并使用这份备份，不能假定新版存档向下兼容。

如需保留唯一桌面入口，可用附带的安装脚本建立 App 链接：

```sh
python3.14 games/totk/tools/install_desktop_entry.py --app "/路径/Tears of the Kingdom.app"
```

替换旧开发启动器时，脚本会将旧存档完整复制并校验到下列标准用户目录，同时建立升级备份；原存档保留，旧启动器归档后不再作为第二个 App 入口。已有且内容不同的标准存档不会被覆盖。

来源运行时已通过 Mac 基础检查：读档、移动 / 跳跃、菜单 / 地图、2K、保存重读及死亡重试。全地图、长局与 1.0 / 1.1.2 公平性能对比仍待测试；前面的帧率观察不是对 1.1.2 的性能承诺。打包验证范围见[构建记录](docs/how-it-works.md#112-validation)。

1.4.3 另行通过了 M4 Mac 的场景读档、移动 / 跳跃、菜单 / 地图、保存重读、原生 2K 画布切换和 MetalFX 放大 / 补帧。唯一打包 App 已实际验证，另覆盖死亡重试和神庙进出。旧构建三小时及最终版约 30 分钟分开记录；首次编译卡顿、长局内存增长与音频欠载仍待排查，不能视为全游戏或稳定帧率认证。结果、测试方法和后续项见 [1.4.3 适配记录](docs/v1.4.3-port.md)。

## 操作

macOS 能识别的手柄（如 DualSense、Xbox 手柄）会映射为 Switch Pro 手柄。震动和体感需要手柄及 macOS 提供对应能力，并非所有手柄都有陀螺仪。键盘映射按美式布局对应的物理键位：

| 按键 | Switch | | 按键 | Switch |
|---|---|---|---|---|
| W A S D | 左摇杆 | | 方向键 | 右摇杆 |
| J | A | | K、空格 | B |
| I | X（跳） | | U | Y（攻击） |
| Q / E | L / R | | Option / Shift | ZL / ZR |
| F T H G | 十字键 左 / 上 / 右 / 下 | | Z / C | 左 / 右摇杆按下 |
| Return | +（菜单） | | Tab、Esc | −（地图） |

额外按键：**1** 同时隐藏或重新显示所有屏幕提示（FPS、着色器计数、系统负载和快捷键说明，集中在右上角的一个小框里；`TOTK_HUD=corners` 恢复为画面四角显示）；**2** 补帧（关 → 30+30 → 刷新率一半 → 不限帧），**3** 循环切换画质（MetalFX 2K → 原生 2K → 关闭）。MetalFX 默认关闭，只有按 **3** 手动切换到该模式才会启用；放大窗口不会自动开启。文字输入（给马取名）直接用 Mac 键盘。

原生 2K 将游戏合成画布和 UI 改为 2560×1440，3D 场景仍由游戏独立的渲染比例控制，并非强制所有内部通道以 1440p 渲染。手动 MetalFX 在较小窗口中也可使用，输出放大会跟随实际窗口尺寸，包括 4K。

语言：游戏和启动器自身的文字（启动画面、着色器预热进度、提示叠层、键盘输入提示）跟随 Mac 的系统语言。启动器文字在中文系统上显示中文，其他语言显示英文。如需指定，可用 `TOTK_LANG` 设置游戏语言（例如 `open --env TOTK_LANG=zh-Hans "Tears of the Kingdom.app"`），用 `TOTK_UI_LANG=zh` / `en` 设置启动器文字。

## 文件位置

- 存档：`~/Library/Application Support/MacNativeGames/TOTK/profile`
- 着色器缓存：`~/Library/Application Support/MacNativeGames/TOTK/shader-cache`（删掉会全部重新翻译）
- 日志：`~/Library/Logs/MacNativeGames/totk.log`（上一次运行：`totk.previous.log`）

## 已知问题

- 支持本体 1.0.0，或本体加单独的 1.1.2 / 1.4.3 更新（NSP/NSZ）。其他版本、DLC、合并包、压缩本体（NSZ/XCZ）和个性化票据仍不支持。平台仅限 Mac。
- 没有联网功能，没有 amiibo。
- 首次遇到新着色器可能出现编译卡顿，缓存能减少重复编译，但不保证完全没有卡顿。
- 38 项自动测试覆盖生成的容器 / 更新样本、重定位格式、工作目录隔离、打包、桌面入口迁移、着色器提取、首次导入的缓存保留及 27 个原生 RomFS 路径用例。GitHub Actions 已配置为运行源码测试和路径解析器内存检查，不需要游戏数据或密钥；另使用本地 XCI 构造、附带生成通用票据的 NSP 测试副本完成完整 Mac 打包。尚未验证独立的零售本体 NSP，也未测试从该类输入构建后的实际游玩。

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
