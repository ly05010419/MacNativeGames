# MacNativeGames

<p align="center"><b>A technical exploration: can a Nintendo Switch game's NVN graphics be ported to Metal?</b><br>
Tested with your own copies of Tears of the Kingdom and Breath of the Wild on Mac, iPhone and iPad. Not an emulator: the game's own ARM64 code runs directly on Apple chips.</p>

[English](#english) | [中文](#chinese)

# English

## Legal

**You must own the game.** Use this project only with games that you bought, and with keys and system files dumped from your own Nintendo Switch. This is an independent, non-commercial project. It is not affiliated with, endorsed by, or sponsored by Nintendo. The Legend of Zelda and Nintendo Switch are trademarks of Nintendo.

The repository contains no game content and distributes none: no game program, art, text, audio, shaders or keys. Everything the game needs is made on your own computer from your own files when you build the app. The app you build contains the game, so it is for your own use only; do not share it. What the repository does hold is the information needed to make the port work: the patch code, the addresses and hashes used to check your files, and the [shared shader list](docs/shader-list.md), which names shaders and bind states but contains no shader code.

Apple, Mac, iPhone, iPad, Apple Vision Pro and Metal are trademarks of Apple Inc.; this project is not affiliated with Apple. Other game titles and trademarks named here belong to their respective owners.

Whether dumping games, keys and system files is legal depends on where you live; check and follow your local laws.

Issues and pull requests that contain or ask for game files, keys or firmware will be deleted.

## About

**This project is a technical exploration, mainly feasibility experiments and analysis.** The Nintendo Switch is an ARM system (an ARM64 CPU in NVIDIA's Tegra X1), and Apple chips are ARM64 as well, so we want to test whether its games can, technically, be ported to Apple chips. It started from one question: can the NVN graphics a Switch game was written for be rebuilt faithfully on Apple's Metal, and can the game's own ARM64 code run directly on Apple chips without an emulator? Tears of the Kingdom and Breath of the Wild are the test cases, because they push the graphics API hard. The repository records the experiments, measurements and findings along the way; it is a study of the technique, not a game distribution. **It does not encourage piracy:** use only your own game data, dumped from a game you bought and from your own Switch, never game files, keys or firmware downloaded from the internet or obtained from anyone else.

How it works: bring your game file (XCI or NSP) and your `prod.keys`; one script builds a Mac app or an unsigned iPhone/iPad IPA. The game's own ARM64 code runs directly on Apple chips (rewritten where the operating system requires it), the Switch system services are answered by a small host, and the game's NVN graphics are rebuilt on Metal. The iOS build requires iOS/iPadOS 26 and iPhone 14 or newer; version 1.4.3 has been played on an iPhone 14 and an iPad, see [iPhone and iPad](docs/install.md#iphone-and-ipad). Details are in [How it works](docs/how-it-works.md).

| Game | Version | Platform | Status | Folder |
|---|---|---|---|---|
| The Legend of Zelda: Tears of the Kingdom | 1.0.0, 1.2.1, 1.4.3 (1.1.2 builds too) | Mac, iPhone, iPad; Apple Vision Pro (windowed, early) | Supported | [`games/totk`](docs/install.md) ([Vision Pro](docs/install.md#apple-vision-pro)) |
| The Legend of Zelda: Breath of the Wild | 1.6.0, 1.9.0 | Mac; 1.6.0 also on the iPhone | Supported | [`games/totk`](docs/install.md#breath-of-the-wild) (shared runtime), list in `games/botw` |
| Super Smash Bros. Ultimate | 13.0.2, 13.0.5 | Mac | Community port | [`games/ssbu`](games/ssbu/README.md) |
| Super Mario Odyssey | 1.3.0, 1.4.1 | Mac | Community port, early | [`games/odyssey`](games/odyssey/README.md) |
| Bayonetta 3 | 1.2.0 | Mac | Community port, work in progress | [`games/by3`](games/by3/README.md) |
| Super Mario Bros. Wonder | 1.0.0, 1.2.1 | Mac | Community port, early | [`games/wonder`](games/wonder/README.md) |
| Ori and the Blind Forest: Definitive Edition | 1.0.1 | Mac | Community port | [`games/ori`](games/ori/README.md) |
| Super Mario Party Jamboree | 2.3.0 | Mac | Community port, experimental | [`games/jamboree`](games/jamboree/README.md) |
| Mario vs. Donkey Kong | 1.0.1 | Mac | Community port, early | [`games/mvdk`](games/mvdk/README.md) |
| Metroid Dread | 1.0.0 | Mac | Community port, early | [`games/dread`](games/dread/README.md) |
| Unicorn Overlord (Asian release) | 1.05 | Mac | Community port, early; DLC optional | [`games/unicorn`](games/unicorn/README.md) |
| The Legend of Zelda: Echoes of Wisdom | 1.1.0 | Mac | Community port, early | [`games/eow`](games/eow/README.md) |
| Animal Crossing: New Horizons | 1.2.1; 3.0.1 (experimental) | Mac | Community port | [`games/acnh`](games/acnh/README.md) |
| Octopath Traveler 0 | 1.0.3, 1.0.4, 1.0.5 | Mac | Community port, work in progress | [`games/ot0`](games/ot0/README.md) |
| Hyrule Warriors: Age of Calamity | 1.3.0 | Mac | Community port, experimental | [`games/aoc`](games/aoc/README.md) |
| Mario Kart 8 Deluxe | 4.0.1 | Mac | Community port, early | [`games/mk8d`](games/mk8d/README.md) |
| Astral Chain | 1.0.1 | Mac | Community port, work in progress; the first frame is presented and the title flow runs | [`games/alc`](games/alc/README.md) |
| Xenoblade Chronicles 3 | 1.0.0, 2.2.1 | Mac | Community port, work in progress | [`games/xc3`](games/xc3/README.md) |

Community ports keep their own runtime, graphics layer and scripts in their folder and are built with the steps there.
- 2026-10-31: [Tomodachi Life: Living the Dream](games/tomodachi) 1.0.0 — experimental native Mac port with native controls and text entry; validation and known limitations are documented in its folder.

### Windows games

[`windows_games`](windows_games) holds Windows PC games, one folder each. They are statically recompiled into arm64,
and their runtime draws with Metal. As with the Switch games, you bring your own copy and nothing from the game is in the
repository.

| Game | Version | Platform | Status | Folder |
|---|---|---|---|---|
| Command & Conquer: Red Alert 2 and Yuri's Revenge | 1.006 and 1.001 (Steam release) | Mac, iPhone, iPad | Community port; a skirmish match plays on the Mac and iPhone 14, saves work on the Mac; English, 繁體中文, 简体中文 | [`windows_games/ra2`](windows_games/ra2/install.md) |

### Recommended install

> **Hand it to an AI assistant.** Open this repository in Claude Code, Codex, or any other AI assistant you like, tell it where your own game file and `prod.keys` are, and ask it to build and install the app and to sort out whatever comes up on your Mac. It follows the [install skill](.claude/skills/install-zelda/SKILL.md), runs every step and checks the result; you just wait with a coffee. Installing and testing by hand is not recommended: there are many steps, and every Mac, game version and dump is a little different. That's why I haven't provided a one-click build option: an AI assistant is my one-click build option. For Red Alert 2 it needs only your own Steam install (no `prod.keys`) and follows [its own skill](.claude/skills/install-ra2/SKILL.md).

### Install skill

> **The install skill** ([`.claude/skills/install-zelda/SKILL.md`](.claude/skills/install-zelda/SKILL.md)) is a short guide written for the assistant: which of your files goes where, the command for your case, how long each step takes (a build is 20–40 minutes; it must not be stopped), and the fix for each common error. Claude Code finds it by itself (or type `/install-zelda`). With any other assistant, start with: *"Read .claude/skills/install-zelda/SKILL.md and follow it to install Tears of the Kingdom for me."*
>
> **Red Alert 2** has its own: [`.claude/skills/install-ra2/SKILL.md`](.claude/skills/install-ra2/SKILL.md) (`/install-ra2`), for the Mac and iPhone / iPad builds, the movies made smaller for the phone and the Chinese languages. *"Read .claude/skills/install-ra2/SKILL.md and follow it to install Red Alert 2 for me."*

## Let's maintain the shader list together: no more stutter

> [!TIP]
> **Every shader in the shared list is ready before you play.** On a Mac the game does not stop to compile it, and on an iPhone or iPad nothing is missing from the picture. The more places players cover, the smoother the game gets for everyone.

The [shared shader list](docs/shader-list.md) now holds **14,575 shader variants** (12,541 shaders) from the Mac, iPhone and iPad: only states a build does not make by itself, since every build makes each shader's default variant. Every build makes them from the player's own game files, so the list holds identifiers only, never game data. Helping takes no setup:

1. **Play.** Every build carries each shader of the game for its default bind state. When your game meets a shader in a state that variant does not serve, or builds a render pipeline the app's list lacks, it notes it in `shader-list-contribution.txt`; default variants are never noted.
2. **On a Mac, every 100 new rows (shaders and pipelines together) it asks you once, before the game starts.** Click "Send": the GitHub form opens and the file is shown, ready to drag in. If the GitHub CLI (`gh`) is installed and signed in, the dialog offers "Submit automatically" instead, which sends this batch for you (a private gist and an issue) without opening the browser. An iPhone or iPad never asks during play: while it is connected to the Mac that built its app (cable or the same Wi-Fi), the Mac asks instead. "Don't remind me again" stops the reminders.
3. **Everyone's next build carries them.** Nobody stutters there again.

**[Send your shaders →](https://github.com/ly05010419/MacNativeGames/issues/new?template=shader-list.yml)** · The game never uploads anything by itself.

**Stuttering when you first start playing or enter a new scene is expected while new shaders compile—don't be surprised.** Performance typically improves as the cache warms up, and every shader the [shared list](#lets-maintain-the-shader-list-together-no-more-stutter) gains is one nobody stutters on again.

## Install and play

Everything from building the app to playing is in **[docs/install.md](docs/install.md)**. The recommended way is to hand it to an AI assistant with the [install skill](.claude/skills/install-zelda/SKILL.md) (see the top of this page) rather than run the steps yourself:

- [What you need](docs/install.md#what-you-need) · [Build with a window](docs/install.md#build-with-a-window) · [Build the app](docs/install.md#build-the-app) · [Breath of the Wild](docs/install.md#breath-of-the-wild) · [iPhone and iPad](docs/install.md#iphone-and-ipad) · [Apple Vision Pro](docs/install.md#apple-vision-pro)
- [Controls](docs/controls.md) · [Where things are](docs/install.md#where-things-are) · [Known issues](docs/install.md#known-issues)
- [Test records](docs/test-records.md)
- Red Alert 2 and Yuri's Revenge: [windows_games/ra2/install.md](windows_games/ra2/install.md)

## How it works

See [docs/how-it-works.md](docs/how-it-works.md).

Recorded performance experiments, remaining work and comparison methods: [Mac performance follow-ups](docs/performance-followups.md).

Measuring by game state with the Metal performance tools of macOS 27 (`TOTK_STATE_REPORTING=1`, `metal_perf_report.py`): [Metal performance diagnostics](docs/metal-performance-diagnostics.md).

## Acknowledgements

- **[d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile)** (MIT) developed this way of running Switch games natively (for Diablo II: Resurrected): the NSO rewriter, the Horizon host and the NVN-on-Metal layer here started as its generic parts.
- **[nxnative](https://github.com/seedds/nxnative)** (MIT): the one-script packaging and NSP common-ticket import follow its approach, and code of it is adapted here: the SignalToAddress, SystemEvent and signal-wake rules in `games/totk/runtime/threads.c` (via PR #159) and the spin-wait search in `games/totk/third_party/d2r/patch_nso.py`.
- **Ryujinx** (MIT): its shader translator (`vendor/ryujinx`) turns the game's Maxwell shaders into SPIR-V, and its system-service code is a reference. With this project's patches and service it is compiled ahead of time (.NET NativeAOT, which builds in .NET's runtime libraries, MIT) into `games/totk/prebuilt/macos-arm64/totk_xlate`; `games/totk/tools/totk_xlate/publish_native.sh` rebuilds it (needs the .NET 10 SDK, only for developers). The audio effects (delay, reverb, compressor) and the mixer in `games/totk/runtime` are ported from Ryujinx.Audio.
- **[SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross)** (Apache-2.0) turns SPIR-V into Metal Shading Language; it is fetched at a pinned commit and compiled with clang during the build.
- **[hactool](https://github.com/SciresM/hactool)** (ISC, with its bundled mbedTLS, Apache-2.0): merges the base game and its update at build time; fetched at a pinned commit.
- **deko3d** (zlib): texture layout sizes. **[bcdec](https://github.com/iOrange/bcdec)** (MIT or public domain): BC texture decoding. **libopus** and **libvpx** (BSD) from Homebrew: audio and the pre-rendered movies.
- **[x86recomp](https://github.com/JTM-rootstorm/x86recomp)** (MIT) is the recompiler and Win32 runtime of the Windows games (`windows_games/ra2/third_party/x86recomp`, with our changes); it builds on **Berkeley SoftFloat 3** (BSD-3-Clause) and **CORE-MATH** (MIT), and on iOS on **SDL3** (zlib), **glslang**, **SPIRV-Cross** and a decoders-only **ffmpeg** (LGPL). Its Simplified Chinese is converted with **[OpenCC](https://github.com/BYVoid/OpenCC)** (Apache-2.0), the added glyphs drawn from **[Noto Sans CJK](https://github.com/notofonts/noto-cjk)** (Source Han Sans, SIL OFL 1.1).
- Community ports that carry code from elsewhere list it in their own folder (`third_party/COPIES.md`). The main project's and the Windows games' copied code, with each license, is in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## License

MIT (see [LICENSE](LICENSE)). Third-party code keeps its own license: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

---

<a id="chinese"></a>

# 中文说明

## 法律声明

**你必须拥有正版游戏。** 请只配合你自己购买的游戏，以及从你自己的 Nintendo Switch 导出的密钥和系统文件使用本项目。这是一个独立的非商业项目，与任天堂没有关联，也未获其认可或赞助。The Legend of Zelda（塞尔达传说）和 Nintendo Switch 是任天堂的商标。

本仓库不含、也不分发任何游戏内容：没有游戏程序、美术、文字、音频、着色器或密钥。游戏需要的一切，都是打包时在你自己的电脑上、用你自己的文件生成的。打包出的 App 里含有游戏，只供你自己使用，请不要分享。仓库里有的是让移植能运行的信息：补丁代码、用来核对你的文件的地址和哈希，以及[公共着色器清单](docs/shader-list.md)（只记着色器和绑定状态的标识，不含着色器代码）。

Apple、Mac、iPhone、iPad、Apple Vision Pro 和 Metal 是 Apple Inc. 的商标，本项目与苹果没有关联。这里提到的其他游戏名称和商标归各自的权利人所有。

导出游戏、密钥和系统文件是否合法，取决于你所在地区的法律，请自行确认并遵守。

附带或索要游戏文件、密钥、固件的 issue 和 PR 会被删除。

## 项目介绍

**这是一个技术探索项目，主要做可行性实验和分析。** Switch 是 ARM 系统（NVIDIA Tegra X1 里的 ARM64 处理器），苹果芯片同样是 ARM64，所以我们想测试：技术上能不能把它的游戏移植到苹果芯片上。它起源于一个问题：Switch 游戏所用的 NVN 图形接口，能不能在苹果的 Metal 上忠实地重建出来？游戏自己的 ARM64 代码，能不能不经模拟器、直接在苹果芯片上运行？《王国之泪》和《旷野之息》对图形接口的要求很高，所以用它们来检验。仓库记录的是这一路的实验、测量和结论；它研究的是技术，不是分发游戏。**本项目不鼓励盗版：**请只使用你自己的游戏数据，也就是从你自己购买的游戏和你自己的 Switch 导出的数据，不要使用从网上下载或从他人处得到的游戏文件、密钥或固件。

做法：准备好游戏文件（XCI 或 NSP）和你的 `prod.keys`，一个脚本即可构建 Mac App 或未签名的 iPhone / iPad IPA。游戏自己的 ARM64 代码直接运行在苹果芯片上（按操作系统要求改写），Switch 的系统服务由一个小宿主应答，游戏的 NVN 图形在 Metal 上重建。iOS 构建需要 iOS / iPadOS 26，最低支持 iPhone 14；1.4.3 已在 iPhone 14 和 iPad 上实际游玩，见 [iPhone 和 iPad](docs/install.zh-CN.md#iphone-和-ipad)。详细见[工作原理](docs/how-it-works.md)。

| 游戏 | 版本 | 平台 | 状态 | 目录 |
|---|---|---|---|---|
| 塞尔达传说 王国之泪 | 1.0.0、1.2.1、1.4.3（1.1.2 也能打包） | Mac、iPhone、iPad；Apple Vision Pro（窗口内游玩，早期） | 正式支持 | [`games/totk`](docs/install.zh-CN.md)（[Vision Pro](docs/install.zh-CN.md#apple-vision-pro)） |
| 塞尔达传说 旷野之息 | 1.6.0、1.9.0 | Mac；1.6.0 也能在 iPhone 上玩 | 正式支持 | [`games/totk`](docs/install.zh-CN.md#旷野之息)（共用运行时），清单在 `games/botw` |
| 任天堂明星大乱斗 特别版 | 13.0.2、13.0.5 | Mac | 社区移植 | [`games/ssbu`](games/ssbu/README.md) |
| 超级马力欧 奥德赛 | 1.3.0、1.4.1 | Mac | 社区移植，早期 | [`games/odyssey`](games/odyssey/README.md) |
| 猎天使魔女 3 | 1.2.0 | Mac | 社区移植，开发中 | [`games/by3`](games/by3/README.md) |
| 超级马力欧兄弟 惊奇 | 1.0.0、1.2.1 | Mac | 社区移植，早期 | [`games/wonder`](games/wonder/README.md) |
| 奥日与黑暗森林 终极版 | 1.0.1 | Mac | 社区移植 | [`games/ori`](games/ori/README.md) |
| 超级马力欧派对 空前盛会 | 2.3.0 | Mac | 社区移植，实验 | [`games/jamboree`](games/jamboree/README.md) |
| 马力欧 vs. 大金刚 | 1.0.1 | Mac | 社区移植，早期 | [`games/mvdk`](games/mvdk/README.md) |
| 密特罗德 生存恐惧 | 1.0.0 | Mac | 社区移植，早期 | [`games/dread`](games/dread/README.md) |
| 圣兽之王（亚洲版） | 1.05 | Mac | 社区移植，早期；DLC 可选 | [`games/unicorn`](games/unicorn/README.md) |
| 塞尔达传说 智慧的再现 | 1.1.0 | Mac | 社区移植，早期 | [`games/eow`](games/eow/README.md) |
| 集合啦！动物森友会 | 1.2.1；3.0.1（实验性） | Mac | 社区移植 | [`games/acnh`](games/acnh/README.md) |
| 歧路旅人0 | 1.0.3、1.0.4、1.0.5 | Mac | 社区移植，开发中 | [`games/ot0`](games/ot0/README.md) |
| 塞尔达无双 灾厄启示录 | 1.3.0 | Mac | 社区移植，实验 | [`games/aoc`](games/aoc/README.md) |
| 马力欧卡丁车8 豪华版 | 4.0.1 | Mac | 社区移植，早期 | [`games/mk8d`](games/mk8d/README.md) |
| 异界锁链 | 1.0.1 | Mac | 社区移植，开发中；能出第一帧，标题流程能走 | [`games/alc`](games/alc/README.md) |
| 异度神剑3 | 1.0.0、2.2.1 | Mac | 社区移植，开发中 | [`games/xc3`](games/xc3/README.md) |

社区移植的游戏各自在自己的目录里带一份运行时、图形层和脚本，按那里的步骤构建。

### Windows 游戏

[`windows_games`](windows_games) 放 Windows 电脑游戏，一个游戏一个目录。静态重编译成 arm64，它的运行时调用 Metal。和 Switch
游戏一样，游戏由你自己提供，仓库里没有任何游戏内容。

| 游戏 | 版本 | 平台 | 状态 | 目录 |
|---|---|---|---|---|
| 命令与征服：红色警戒 2 及尤里的复仇 | 1.006 和 1.001（Steam 版） | Mac、iPhone、iPad | 社区移植；Mac 和 iPhone 14 上能打遭遇战，Mac 上能存读档；英文、繁體中文、简体中文 | [`windows_games/ra2`](windows_games/ra2/install.zh-CN.md) |

### 推荐的安装

> **交给 AI 助理。** 用 Claude Code、Codex 或其他你喜欢的 AI 助理打开这个仓库，告诉它你自己的游戏文件和 `prod.keys` 在哪，让它帮你打包、安装 App，遇到这台 Mac 上的问题也让它处理。它会按[安装 skill](.claude/skills/install-zelda/SKILL.md) 一步步跑完并检查结果，你只需要喝杯咖啡等着。不推荐自己手动安装测试：步骤多，每台 Mac、每个游戏版本、每份导出的游戏文件都有些差别。这也是我没有提供一键打包的原因：AI 助理就是我的一键打包。红警 2 只需要你自己的 Steam 版游戏（不要 `prod.keys`），按[它自己的 skill](.claude/skills/install-ra2/SKILL.md) 走。

### 安装 skill

> **安装 skill**（[`.claude/skills/install-zelda/SKILL.md`](.claude/skills/install-zelda/SKILL.md)）是写给 AI 助理的简短指南：你的哪个文件放在哪个参数、你这种情况用哪条命令、每一步要多久（一次打包 20–40 分钟，中途不能停），以及常见报错各怎么解决。Claude Code 会自己找到它（也可以输入 `/install-zelda`）。用其他 AI 助理时，第一句这样说：*“读 .claude/skills/install-zelda/SKILL.md，按它帮我安装王国之泪。”*
>
> **红警 2** 有自己的 skill：[`.claude/skills/install-ra2/SKILL.md`](.claude/skills/install-ra2/SKILL.md)（`/install-ra2`），包括 Mac 和 iPhone / iPad 打包、给手机缩小的动画和中文。*“读 .claude/skills/install-ra2/SKILL.md，按它帮我安装红警 2。”*

## 让我们一起维护着色器清单：游戏不再卡顿

> [!TIP]
> **公共清单里的每个着色器，开玩之前就已经准备好。** Mac 上不用停下来现场编译，iPhone 和 iPad 上画面也不会缺东西。大家玩过的地方越多，所有人玩得越流畅。

[公共着色器清单](docs/shader-list.md)现在有 **14,575 个着色器变体**（12,541 个着色器），来自 Mac、iPhone 和 iPad：只收打包自己做不出的状态，因为每次打包都会给每个着色器做好默认变体。每次打包都用玩家自己的游戏文件生成，所以清单里只有标识，不含任何游戏数据。参与不需要任何设置：

1. **照常玩。** 每次打包都会把游戏里每个着色器按默认绑定状态各备一份。游戏遇到默认那份用不了的状态，或者建了 App 清单里没有的渲染管线，才会记进 `shader-list-contribution.txt`；默认变体不记。
2. **在 Mac 上，着色器和管线合计每攒够 100 条，它会在游戏启动前问你一次。** 点“发送”，GitHub 提交页面会打开，文件也会显示出来，拖进去就行。装了 GitHub 命令行工具（`gh`）并已登录时，对话框里会多一个“自动提交”，点了就直接替你把这一批发出去（私密 gist 加一个 issue），不用打开浏览器。iPhone 和 iPad 玩的时候从不提醒：设备连着打包它的那台 Mac（线连或同一 Wi‑Fi）时，由 Mac 来问。点“不要提醒了”以后就不再问。
3. **下次大家打包的 App 都会带上。** 谁到那里都不会再卡顿。

**[发送你的着色器 →](https://github.com/ly05010419/MacNativeGames/issues/new?template=shader-list.yml)** · 游戏不会自己上传任何东西。

**第一次进入游戏或新场景时，新着色器的编译可能造成卡顿，这是预期现象，不必惊讶。** 随着着色器缓存逐步预热，表现通常会改善；[公共清单](#让我们一起维护着色器清单游戏不再卡顿)每多一个着色器，就少一处卡顿。

## 安装与游玩

从打包 App 到开始游玩的全部步骤见 **[docs/install.zh-CN.md](docs/install.zh-CN.md)**。推荐交给 AI 助理按[安装 skill](.claude/skills/install-zelda/SKILL.md) 去跑（见本页开头），不推荐自己一步步手动操作：

- [需要准备](docs/install.zh-CN.md#需要准备) · [打包 App](docs/install.zh-CN.md#打包-app) · [旷野之息](docs/install.zh-CN.md#旷野之息) · [iPhone 和 iPad](docs/install.zh-CN.md#iphone-和-ipad) · [Apple Vision Pro](docs/install.zh-CN.md#apple-vision-pro)
- [操作](docs/controls.zh-CN.md) · [文件位置](docs/install.zh-CN.md#文件位置) · [已知问题](docs/install.zh-CN.md#已知问题)
- [测试记录](docs/test-records.zh-CN.md)
- 红警 2 和尤里的复仇：[windows_games/ra2/install.zh-CN.md](windows_games/ra2/install.zh-CN.md)

## 工作原理

见 [docs/how-it-works.md](docs/how-it-works.md)。

已做的性能实验、待办与对比方法见 [Mac 性能待做](docs/performance-followups.md)。

用 macOS 27 的 Metal 性能工具按游戏状态测量（`TOTK_STATE_REPORTING=1`、`metal_perf_report.py`）：见 [Metal 性能诊断](docs/metal-performance-diagnostics.md#macos-27-上的-metal-性能诊断)。

## 致谢

- **[d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile)**（MIT）为《暗黑破坏神 II：狱火重生》开发了这套原生运行 Switch 游戏的方法：这里的 NSO 改写器、Horizon 宿主和 Metal 上的 NVN 层都从它的通用部分起步。
- **[nxnative](https://github.com/seedds/nxnative)**（MIT）：“用户自己的游戏文件 + 一个脚本出包”和 NSP 通用票据读取的做法借鉴自它；也移植了它的代码：`games/totk/runtime/threads.c` 里 SignalToAddress、SystemEvent 和按对象唤醒的规则（经 PR #159），以及 `games/totk/third_party/d2r/patch_nso.py` 的自旋等待查找。
- **Ryujinx**（MIT）：它的着色器翻译器（`vendor/ryujinx`）把游戏的 Maxwell 着色器转成 SPIR-V，它的系统服务代码也是参考。加上本项目的补丁和着色器服务，用 .NET NativeAOT 预先编成原生程序 `games/totk/prebuilt/macos-arm64/totk_xlate`（其中编入了 .NET 的运行库，MIT）；改了翻译器用 `games/totk/tools/totk_xlate/publish_native.sh` 重编（要 .NET 10 SDK，只有开发者需要）。`games/totk/runtime` 里的音效（延迟、混响、压缩器）和混音器移植自 Ryujinx.Audio。
- **[SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross)**（Apache-2.0）把 SPIR-V 转成 Metal 着色语言；打包时按固定提交取得，用 clang 编。
- **[hactool](https://github.com/SciresM/hactool)**（ISC，自带的 mbedTLS 是 Apache-2.0）：打包时合并本体和更新包；按固定提交取得。
- **deko3d**（zlib）：贴图布局尺寸。**[bcdec](https://github.com/iOrange/bcdec)**（MIT 或公有领域）：BC 贴图解码。**libopus**、**libvpx**（BSD，来自 Homebrew）：音频和预渲染影片。
- **[x86recomp](https://github.com/JTM-rootstorm/x86recomp)**（MIT）是 Windows 游戏的重编译器和 Win32 运行时（`windows_games/ra2/third_party/x86recomp`，含我们的改动）；它用到 **Berkeley SoftFloat 3**（BSD-3-Clause）和 **CORE-MATH**（MIT），iOS 上还用到 **SDL3**（zlib）、**glslang**、**SPIRV-Cross** 和只带解码器的 **ffmpeg**（LGPL）。简体中文用 **[OpenCC](https://github.com/BYVoid/OpenCC)**（Apache-2.0）转换，补的字形取自 **[思源黑体 / Noto Sans CJK](https://github.com/notofonts/noto-cjk)**（SIL OFL 1.1）。
- 带了别处代码的社区移植，在各自目录的 `third_party/COPIES.md` 里列出。主项目和 Windows 游戏拷进来的代码及各自的许可，列在 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

## 许可证

MIT（见 [LICENSE](LICENSE)）。第三方代码保留各自的许可证：[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
