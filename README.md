# MacNativeGames

<p align="center"><b>A technical exploration: can a Nintendo Switch game's NVN graphics be ported to Metal?</b><br>
Tested with your own copies of Tears of the Kingdom and Breath of the Wild on Mac, iPhone and iPad, and of Ocarina of Time (Switch 2) on the Mac. Not an emulator: the game's own ARM64 code runs directly on Apple chips.</p>

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

How it works: bring your game file (XCI or NSP) and your `prod.keys`; one script builds a Mac app or an unsigned iPhone/iPad IPA. The game's own ARM64 code runs directly on Apple chips (rewritten where the operating system requires it), the Switch system services are answered by a small host, and the game's NVN graphics are rebuilt on Metal. The iOS build requires iOS/iPadOS 26 and iPhone 14 or newer; Tears of the Kingdom 1.4.3 has been played on an iPhone 14 and an iPad, Breath of the Wild 1.6.0 on an iPhone 14 (on the iPad it reaches the title), see [iPhone and iPad](docs/install.md#iphone-and-ipad). Details are in [How it works](docs/how-it-works.md).

### Switch 2 games (experimental)

Switch 2 games run on the same Tears of the Kingdom runtime: their Ampere (NVN2) shaders go through the same translator,
and their SDK 20 services through the same host ([docs/ns2-port.md](docs/ns2-port.md)). Ocarina of Time carries its own copy of
that runtime in [`games/oot`](games/oot/README.md).

| Game | Version | Platform | Status |
|---|---|---|---|
| The Legend of Zelda: Ocarina of Time | update 131072 | Mac | Playable from the title into Kokiri Forest and the village on an M4, not played through; resolution, MetalFX upscaling, frame lock and frame interpolation in the settings panel; [`games/oot`](games/oot/README.md), [install](games/oot/docs/install.md) |
| Donkey Kong Bananza | update 458752 | Mac | Playable, early; not fully tested |
| Pokémon Pokopia | 2.0.1 | Mac | Early: plays its opening into the first area (scripted test, not fully played). |
| Mario Kart World | 1.0.0 | Mac | Community port, experimental; [build instructions](games/mkw/README.md) |

Ocarina of Time and Pokopia package as a Mac app with `build_app.py`. Not yet: iPhone / iPad (a build path for Ocarina of Time is being tested), DLC.

### Switch 1 games

| Game | Version | Platform | Status | Folder |
|---|---|---|---|---|
| The Legend of Zelda: Tears of the Kingdom | 1.0.0, 1.2.1, 1.4.3 | Mac (up to native 4K on 1.2.1 and 1.4.3; 5K by default on 32 GB-plus Macs with 1.4.3), iPhone, iPad; Apple Vision Pro (windowed, early) | Supported | [`games/totk`](docs/install.md) ([Vision Pro](docs/install.md#apple-vision-pro)) |
| The Legend of Zelda: Breath of the Wild | 1.6.0, 1.9.0 | Mac (up to native 4K); 1.6.0 also on iPhone and iPad | Supported | [`games/totk`](docs/install.md#breath-of-the-wild) (shared runtime), list in `games/botw` |
| Super Smash Bros. Ultimate | 13.0.2, 13.0.5 | Mac | Community port | [`games/ssbu`](games/ssbu/README.md) |
| Super Mario Odyssey | 1.3.0, 1.4.1 | Mac | Community port, early | [`games/odyssey`](games/odyssey/README.md) |
| Bayonetta 3 | 1.2.0 | Mac | Community port, work in progress | [`games/by3`](games/by3/README.md) |
| Super Mario Bros. Wonder | 1.0.0, 1.2.1 | Mac | Community port, early | [`games/wonder`](games/wonder/README.md) |
| Pokémon Legends: Arceus | 1.1.1 | Mac | Community port, early | [`games/pla`](games/pla/README.md) |
| Ori and the Blind Forest: Definitive Edition | 1.0.1 | Mac | Community port | [`games/ori`](games/ori/README.md) |
| Super Mario Party Jamboree | 2.3.0 | Mac | Community port, experimental | [`games/jamboree`](games/jamboree/README.md) |
| Mario vs. Donkey Kong | 1.0.1 | Mac | Community port, early | [`games/mvdk`](games/mvdk/README.md) |
| Metroid Dread | 1.0.0 | Mac | Community port, early | [`games/dread`](games/dread/README.md) |
| Unicorn Overlord (Asian release) | 1.05 | Mac | Community port, early; DLC optional | [`games/unicorn`](games/unicorn/README.md) |
| The Legend of Zelda: Echoes of Wisdom | 1.1.0 | Mac | Community port, early | [`games/eow`](games/eow/README.md) |
| Animal Crossing: New Horizons | 1.2.1; 3.0.1 (experimental) | Mac | Community port | [`games/acnh`](games/acnh/README.md) |
| Octopath Traveler 0 | 1.0.3, 1.0.4, 1.0.5 | Mac | Community port, work in progress | [`games/ot0`](games/ot0/README.md) |
| Hyrule Warriors: Age of Calamity | 1.3.0, 1.4.1 | Mac | Community port, experimental | [`games/aoc`](games/aoc/README.md) |
| Mario Kart 8 Deluxe | 4.0.1 | Mac | Community port, early | [`games/mk8d`](games/mk8d/README.md) |
| Kirby and the Forgotten Land | 1.0.0 | Mac | Community port, early | [`games/kirby`](games/kirby/README.md) |
| Luigi's Mansion 3 | 1.2.0 | Mac | Community port, work in progress; playable | [`games/lm3`](games/lm3/README.md) |
| Astral Chain | 1.0.1 | Mac | Community port, work in progress; the first frame is presented and the title flow runs | [`games/alc`](games/alc/README.md) |
| Xenoblade Chronicles 3 | 1.0.0, 2.2.1 | Mac | Community port, playable; a few shaders translate on first use | [`games/xc3`](games/xc3/README.md) |
| Pokémon Violet | 4.0.0 | Mac | Community port, work in progress | [`games/pokesv`](games/pokesv/README.md) |
| Xenoblade Chronicles X: Definitive Edition | 1.0.0; 2.0.0 (diagnostic) | Mac | Community diagnostic host, early field gameplay; external work folder required | [`games/xcx`](games/xcx/README.md) |
| Tomodachi Life: Living the Dream | 1.0.0 | Mac | Community port, experimental | [`games/tomodachi`](games/tomodachi/README.md) |
| The Legend of Zelda: Link's Awakening | 1.1.0 | Mac | Community port, experimental | [`games/la`](games/la/README.md) |

Community ports keep their own runtime, graphics layer and scripts in their folder and are built with the steps there.

### VR: Tears of the Kingdom and Breath of the Wild on a Meta Quest 3 (6DoF)

> **Play Tears of the Kingdom 1.4.3 in a Meta Quest 3, with full 6DoF.** The game runs on your Mac and draws the view
> for every head pose, streamed to the headset over the USB-C cable: turn your head and the camera turns; lean and crouch
> and it moves (Head position, 6DoF). First or third person, the game's sound in the headset. Quest 3 controllers and
> motion gestures are built in but not yet tested on a headset.

- **Tears of the Kingdom 1.4.3:** the game renders one picture per frame, wide enough for both eyes, and each eye looks
  it up along its own rays; turning the head reprojects it to the newest pose. The picture is what the game draws
  without VR (grass, lighting, shadows). The stream to the headset is HEVC at the render size per eye chosen in the VR page
  (at most 2048×2208); on an M4 at 1536×1656 a Quest 3 showed about 70 fps and about 70 ms from input to display
  (measured 2026-11-10; 2048×2208 H.264, the setting before, gave 53 fps and 132 ms). Both eyes see the same picture, so there is
  **no stereo depth by default**.
- **Stereo depth for Tears of the Kingdom (experimental, off by default):** the game still runs each frame once; the
  graphics layer replays the frame once per eye with the eye moved sideways by half the eye distance, so both eyes are
  real renderings of the same moment. It costs about half the frame rate (about 30 fps against 53 with one picture, at
  2048×2208 on an M4) and has no switch in the VR page yet. The game has no stereo renderer of its own, and an earlier attempt
  that ran the game's render jobs twice was removed because it damaged the picture (no grass, wrong lighting).
- **Breath of the Wild 1.6.0 / 1.9.0:** the game's own same-frame stereo renderer (the one of its Labo VR mode) draws
  both eyes with the headset's projection and eye distance; see the install page for its settings and limits.

One script builds and installs it, the headset side included: [`zeldaVR/install.md`](zeldaVR/install.md) (code in
[`zeldaVR`](zeldaVR)). VR stays off in the normal app.

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

The [shared shader list](docs/shader-list.md) now holds **17,335 shader variants** (14,220 shaders) from the Mac, iPhone and iPad: only states a build does not make by itself, since every build makes each shader's default variant. Every build makes them from the player's own game files, so the list holds identifiers only, never game data. Helping takes no setup:

1. **Play.** Every build carries each shader of the game for its default bind state. When your game meets a shader in a state that variant does not serve, or builds a render pipeline the app's list lacks, it notes it in `shader-list-contribution.txt`; default variants are never noted.
2. **On a Mac, every 100 new rows (shaders and pipelines together) it asks you once, before the game starts.** Click "Send": the GitHub form opens and the file is shown, ready to drag in. If the GitHub CLI (`gh`) is installed and signed in, the dialog offers "Submit automatically" instead, which sends this batch for you (a private gist and an issue) without opening the browser. An iPhone or iPad never asks during play: while it is connected to the Mac that built its app (cable or the same Wi-Fi), the Mac asks instead. "Don't remind me again" stops the reminders.
3. **Everyone's next build carries them.** Nobody stutters there again.

**[Send your shaders →](https://github.com/ly05010419/MacNativeGames/issues/new?template=shader-list.yml)** · The game never uploads anything by itself.

**Stuttering when you first start playing or enter a new scene is expected while new shaders compile—don't be surprised.** Performance typically improves as the cache warms up, and every shader the [shared list](#lets-maintain-the-shader-list-together-no-more-stutter) gains is one nobody stutters on again.

## What's new

- **2026-11-11: Rankings, an anonymous frame-rate board (Tears of the Kingdom, Breath of the Wild, Ocarina of Time, Mac).**
  A settings tab of its own (key 1) shows the latest board, one group per frame lock and interpolation setting. **Join the
  board** is off by default and never asked; once on, the session is kept on your Mac every 5 minutes and sent at start and
  every 30 minutes; your own row is green and everyone shows as an anonymous name ([controls](docs/controls.md)).
  Shader contributions are now sent at start and every 30 minutes as well.
- **2026-11-10: The Legend of Zelda: Ocarina of Time (Switch 2) on the Mac.** Build it from your own dump (a `.nup` file or a
  dump folder with update 131072) with `build_app.py`; no keys needed. On an M4 it plays from the title into Kokiri Forest and the
  village, about 81–83 fps standing in the village at 720p or 1080p (from 720p); 720p is the default. The settings panel (key 1) has the resolution,
  the upscaler (MetalFX or the game's own), a frame lock and MetalFX frame interpolation ([games/oot](games/oot/README.md),
  [install](games/oot/docs/install.md)).
- **2026-11-09: experimental Xenoblade Chronicles X 2.0.0 Mac diagnostic.** Separate integrity pins, cumulative update merge and version-aware packaging; original 30 FPS by default, opt-in checked 60/120 timing experiment awaiting broader acceptance ([details](games/xcx/README.md#update-200-mac-diagnostic)).
- **2026-11-08: The Legend of Zelda: Link's Awakening 1.1.0 on Mac, experimental.** Built from your own files; played by hand around Mabe Village ([games/la](games/la/README.md)).
- **2026-11-07: Hyrule Warriors: Age of Calamity builds from update 1.4.1 as well as 1.3.0 on Mac, experimental.** The builder takes the version from the extracted game folder ([`games/aoc`](games/aoc/README.md)).
- **2026-11-03: optional SMAA and FXAA Quality for Tears of the Kingdom 1.4.3 on Mac.** Choose the method and its quality separately on the Effects page; the game's original FXAA remains the default ([controls](docs/controls.md)).
- **2026-11-03: the first Switch 2 games, experimental.** Donkey Kong Bananza plays on the Mac from a development build;
  Pokémon Pokopia plays its opening into the first area (scripted test). Pokopia 2.0.1 packages as a Mac app from your
  own dump folder: `python3 build_app.py <dump folder> --shared-fonts <the six .bfttf files a TOTK / BotW build
  extracted>` (no keys; [Switch 2 games](#switch-2-games-experimental), [docs/ns2-port.md](docs/ns2-port.md)).
- **2026-11-02: Tears of the Kingdom 1.1.2 is no longer supported.** The supported versions are 1.0.0, 1.2.1 and 1.4.3
  (1.4.3 is the most tested). The builder now refuses a 1.1.2 update: build again with the 1.2.1 or 1.4.3 update and
  the same `-o` ([another game version](docs/install.md)). Your saves stay where they are (`TOTK/profile`); before the
  first start on the new version the app backs them up next to them, 1.2.1 reads 1.1.2 saves as they are and 1.4.3
  migrates them. Mods in `mods/v112` are not loaded any more: put the mods for the version you build in its own
  folder ([mods.md](docs/mods.md)).

## Install and play

Everything from building the app to playing is in **[docs/install.md](docs/install.md)**. The recommended way is to hand it to an AI assistant with the [install skill](.claude/skills/install-zelda/SKILL.md) (see the top of this page) rather than run the steps yourself:

- [What you need](docs/install.md#what-you-need) · [Build with a window](docs/install.md#build-with-a-window) · [Build the app](docs/install.md#build-the-app) · [Breath of the Wild](docs/install.md#breath-of-the-wild) · [iPhone and iPad](docs/install.md#iphone-and-ipad) · [Apple Vision Pro](docs/install.md#apple-vision-pro)
- [Controls](docs/controls.md) · [Where things are](docs/install.md#where-things-are) · [Known issues](docs/install.md#known-issues)
- [Test records](docs/test-records.md)
- Ocarina of Time (Switch 2): [games/oot/docs/install.md](games/oot/docs/install.md)
- Red Alert 2 and Yuri's Revenge: [windows_games/ra2/install.md](windows_games/ra2/install.md)

## How it works

See [docs/how-it-works.md](docs/how-it-works.md).

Recorded performance experiments, remaining work and comparison methods: [Mac performance follow-ups](docs/performance-followups.md).

Measuring by game state with the Metal performance tools of macOS 27 (`TOTK_STATE_REPORTING=1`, `metal_perf_report.py`): [Metal performance diagnostics](docs/metal-performance-diagnostics.md).

## Acknowledgements

- **[d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile)** (MIT) developed this way of running Switch games natively (for Diablo II: Resurrected): the NSO rewriter, the Horizon host and the NVN-on-Metal layer here started as its generic parts.
- **[nxnative](https://github.com/seedds/nxnative)** (MIT): the NSP common-ticket import follows its approach; the Switch 2 support also follows it and carries code of it (Ampere shaders, geometry shaders, SDK 20 services, one texture / sampler ID space per descriptor pool, the non-indexed indirect draw and the copy image stride).
- **[ouncenative](https://github.com/kirakira-dev/ouncenative)** (MIT): Pokémon Pokopia's shader layout — its GLSLC reader showed where the control section and the code area of each shader sit, which took our shader extraction from 2,385 to all 131,090 of Pokopia's shaders.
- **[pokopia-native](https://github.com/xFractals/pokopia-native)** (MIT): reference for Pokémon Pokopia (its run-time shaders, the dump layout, the driver-rewritten control bytes).
- **Ryujinx** (MIT): the shader translator (`vendor/ryujinx`, built ahead into `games/totk/prebuilt/macos-arm64/totk_xlate`) and a reference for the system services; the sound effects and the mixer in `games/totk/runtime` are ported from Ryujinx.Audio.
- **[SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross)** (Apache-2.0) turns SPIR-V into Metal Shading Language; it is fetched at a pinned commit and compiled with clang during the build.
- **[hactool](https://github.com/SciresM/hactool)** (ISC, with its bundled mbedTLS, Apache-2.0): merges the base game and its update at build time; fetched at a pinned commit.
- **deko3d** (zlib): texture layout sizes. **[bcdec](https://github.com/iOrange/bcdec)** (MIT or public domain): BC texture decoding. **libopus** and **libvpx** (BSD) from Homebrew: audio and the pre-rendered movies.
- **[x86recomp](https://github.com/JTM-rootstorm/x86recomp)** (MIT) is the recompiler and Win32 runtime of the Windows games (`windows_games/ra2/third_party/x86recomp`, with our changes); it builds on **Berkeley SoftFloat 3** (BSD-3-Clause) and **CORE-MATH** (MIT), and on iOS on **SDL3** (zlib), **glslang**, **SPIRV-Cross** and a decoders-only **ffmpeg** (LGPL). Its Simplified Chinese is converted with **[OpenCC](https://github.com/BYVoid/OpenCC)** (Apache-2.0), the added glyphs drawn from **[Noto Sans CJK](https://github.com/notofonts/noto-cjk)** (Source Han Sans, SIL OFL 1.1).
- **[BasaltVR](https://github.com/frenchbaguetteman/BasaltVR)** (MIT) streams the VR picture: `zeldaVR/install_vr.sh` fetches it at a pinned commit and applies our changes (`zeldaVR/streamer/basaltvr.patch`). On the headset, the **[ALVR](https://github.com/alvr-org/ALVR)** client (MIT) shows it; it is downloaded from ALVR's releases and checked by its SHA-256. **[BotW-BetterVR](https://github.com/Crementif/BotW-BetterVR)** (MIT), UEVR and R.E.A.L. were references for the camera, first person and motion controls; no code is copied.
- **Ocarina of Time** (`games/oot`; every source is listed in [its COPIES.md](games/oot/third_party/COPIES.md)): the NUP reader is **[nup](https://github.com/a55uka/nup)** (MIT); the AV1 movie decoder is adapted from **nxnative**, and the mesh and task shader stages, the HMMA matrix lowering and the typed pool arrays from **[ChiefSZ13/nxnative](https://github.com/ChiefSZ13/nxnative)** (MIT); **dav1d** (BSD-2-Clause) is bundled as the AV1 fallback; **zbic** (GPLv2) is run as a separate build tool, never copied or shipped. NVIDIA's CUDA Binary Utilities and PTX documents and Mesa's NAK were references for Ampere instruction semantics, and **[Lichii](https://github.com/zortos293/Lichii)** (GPL-3.0) for field meanings only; no code from it is copied.
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

做法：准备好游戏文件（XCI 或 NSP）和你的 `prod.keys`，一个脚本即可构建 Mac App 或未签名的 iPhone / iPad IPA。游戏自己的 ARM64 代码直接运行在苹果芯片上（按操作系统要求改写），Switch 的系统服务由一个小宿主应答，游戏的 NVN 图形在 Metal 上重建。iOS 构建需要 iOS / iPadOS 26，最低支持 iPhone 14；王国之泪 1.4.3 已在 iPhone 14 和 iPad 上实际游玩，旷野之息 1.6.0 已在 iPhone 14 上游玩（iPad 上能进标题），见 [iPhone 和 iPad](docs/install.zh-CN.md#iphone-和-ipad)。详细见[工作原理](docs/how-it-works.md)。

### Switch 2 游戏（实验性）

Switch 2 游戏在《王国之泪》同一套运行时上运行：它们的 Ampere（NVN2）着色器走同一个翻译器，SDK 20 的系统服务由同一个宿主
应答（[docs/ns2-port.md](docs/ns2-port.md)）。时之笛在 [`games/oot`](games/oot/README.md) 里带着这套运行时自己的一份。

| 游戏 | 版本 | 平台 | 状态 |
|---|---|---|---|
| 塞尔达传说 时之笛 | 更新 131072 | Mac | 在 M4 上从标题玩进科奇里森林和村子，没有通关；设置面板里有分辨率、MetalFX 放大、锁帧和补帧；[`games/oot`](games/oot/README.md)，[安装](games/oot/docs/install.zh-CN.md) |
| 大金刚 Bananza | 更新 458752 | Mac | 初步可玩，早期，未完整测试 |
| 宝可梦 Pokopia | 2.0.1 | Mac | 早期：能玩过开场进到第一个区域（脚本测试，没有完整玩过）。 |
| 马力欧卡丁车 世界 | 1.0.0 | Mac | 社区移植，实验性；[构建说明](games/mkw/README.md) |

时之笛和 Pokopia 可以用 `build_app.py` 打包成 Mac App。还没有：iPhone / iPad（时之笛的打包路径在测试中）、DLC。

### Switch 1 游戏

| 游戏 | 版本 | 平台 | 状态 | 目录 |
|---|---|---|---|---|
| 塞尔达传说 王国之泪 | 1.0.0、1.2.1、1.4.3 | Mac（1.2.1 和 1.4.3 最高原生 4K，1.4.3 在 32 GB 及以上内存的 Mac 默认带 5K）、iPhone、iPad；Apple Vision Pro（窗口内游玩，早期） | 正式支持 | [`games/totk`](docs/install.zh-CN.md)（[Vision Pro](docs/install.zh-CN.md#apple-vision-pro)） |
| 塞尔达传说 旷野之息 | 1.6.0、1.9.0 | Mac（最高原生 4K）；1.6.0 也能在 iPhone 和 iPad 上玩 | 正式支持 | [`games/totk`](docs/install.zh-CN.md#旷野之息)（共用运行时），清单在 `games/botw` |
| 任天堂明星大乱斗 特别版 | 13.0.2、13.0.5 | Mac | 社区移植 | [`games/ssbu`](games/ssbu/README.md) |
| 超级马力欧 奥德赛 | 1.3.0、1.4.1 | Mac | 社区移植，早期 | [`games/odyssey`](games/odyssey/README.md) |
| 猎天使魔女 3 | 1.2.0 | Mac | 社区移植，开发中 | [`games/by3`](games/by3/README.md) |
| 超级马力欧兄弟 惊奇 | 1.0.0、1.2.1 | Mac | 社区移植，早期 | [`games/wonder`](games/wonder/README.md) |
| 宝可梦传说 阿尔宙斯 | 1.1.1 | Mac | 社区移植，早期 | [`games/pla`](games/pla/README.md) |
| 奥日与黑暗森林 终极版 | 1.0.1 | Mac | 社区移植 | [`games/ori`](games/ori/README.md) |
| 超级马力欧派对 空前盛会 | 2.3.0 | Mac | 社区移植，实验 | [`games/jamboree`](games/jamboree/README.md) |
| 马力欧 vs. 大金刚 | 1.0.1 | Mac | 社区移植，早期 | [`games/mvdk`](games/mvdk/README.md) |
| 密特罗德 生存恐惧 | 1.0.0 | Mac | 社区移植，早期 | [`games/dread`](games/dread/README.md) |
| 圣兽之王（亚洲版） | 1.05 | Mac | 社区移植，早期；DLC 可选 | [`games/unicorn`](games/unicorn/README.md) |
| 塞尔达传说 智慧的再现 | 1.1.0 | Mac | 社区移植，早期 | [`games/eow`](games/eow/README.md) |
| 集合啦！动物森友会 | 1.2.1；3.0.1（实验性） | Mac | 社区移植 | [`games/acnh`](games/acnh/README.md) |
| 歧路旅人0 | 1.0.3、1.0.4、1.0.5 | Mac | 社区移植，开发中 | [`games/ot0`](games/ot0/README.md) |
| 塞尔达无双 灾厄启示录 | 1.3.0、1.4.1 | Mac | 社区移植，实验 | [`games/aoc`](games/aoc/README.md) |
| 马力欧卡丁车8 豪华版 | 4.0.1 | Mac | 社区移植，早期 | [`games/mk8d`](games/mk8d/README.md) |
| 星之卡比 探索发现 | 1.0.0 | Mac | 社区移植，早期 | [`games/kirby`](games/kirby/README.md) |
| 路易吉洋馆3 | 1.2.0 | Mac | 社区移植，开发中；可以玩 | [`games/lm3`](games/lm3/README.md) |
| 异界锁链 | 1.0.1 | Mac | 社区移植，开发中；能出第一帧，标题流程能走 | [`games/alc`](games/alc/README.md) |
| 异度神剑3 | 1.0.0、2.2.1 | Mac | 社区移植，可玩；少数着色器首次使用时才翻译 | [`games/xc3`](games/xc3/README.md) |
| 宝可梦 紫 | 4.0.0 | Mac | 社区移植，开发中 | [`games/pokesv`](games/pokesv/README.md) |
| 异度神剑 X 终极版 | 1.0.0；2.0.0（诊断） | Mac | 社区诊断宿主，首区域可探索；需要外部工作目录 | [`games/xcx`](games/xcx/README.md) |
| 朋友收集 梦想生活 | 1.0.0 | Mac | 社区移植，实验性 | [`games/tomodachi`](games/tomodachi/README.md) |
| 塞尔达传说 织梦岛 | 1.1.0 | Mac | 社区移植，实验性 | [`games/la`](games/la/README.md) |

社区移植的游戏各自在自己的目录里带一份运行时、图形层和脚本，按那里的步骤构建。

### VR：在 Meta Quest 3 里玩王国之泪和旷野之息（6DoF）

> **在 Meta Quest 3 里玩《王国之泪》1.4.3，完整 6DoF。** 游戏在你的 Mac 上运行，按每一个头部姿态渲染画面，通过 USB-C
> 数据线串流到头显：转头，镜头跟着转；探身、下蹲，镜头跟着移动（头部位置，6DoF）。第一人称或第三人称，游戏声音进头显。
> Quest 3 手柄和体感手势已经做进去，还没在头显里实测。

- **王国之泪 1.4.3**：游戏每帧只渲染一幅画面，视野宽到同时盖住两只眼，每只眼按自己的视线从这幅画面里取图；转头时按最新
  姿态重投影。画面和不开 VR 时一样（草、光照、阴影）。串流到头显用 HEVC，尺寸跟随 VR 页里选的每眼渲染分辨率
  （最大 2048×2208）；M4 上 1536×1656 时 Quest 3 显示约 70 帧，从输入到显示约 70 毫秒（2026-11-10 实测；之前固定的
  2048×2208 H.264 是 53 帧、132 毫秒）。两只眼看到的是同一幅画面，所以**默认没有立体深度**。
- **王国之泪的立体深度（实验性，默认关）**：游戏仍然每帧只运行一遍；图形层把这一帧按每只眼各回放一次，眼睛向两侧各移半个
  瞳距，所以两只眼都是同一时刻的真实渲染。代价是帧率约减半（M4、2048×2208 下约 30 帧，单画面约 53 帧），VR 页里还没有开关。
  王国之泪自己没有双眼渲染器；早先“把游戏的渲染任务跑两遍”的做法会弄坏画面（草消失、光照错误），已经删除。
- **旷野之息 1.6.0 / 1.9.0**：用游戏自带的同帧双眼渲染器（Labo VR 模式的那一个），按头显的投影和瞳距画两只眼；设置和
  限制见安装文档。

一个脚本完成打包和安装，包括头显上的准备：[`zeldaVR/install.md`](zeldaVR/install.md)（代码在 [`zeldaVR`](zeldaVR)）。普通 App 里 VR 不会打开。

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

[公共着色器清单](docs/shader-list.md)现在有 **17,335 个着色器变体**（14,220 个着色器），来自 Mac、iPhone 和 iPad：只收打包自己做不出的状态，因为每次打包都会给每个着色器做好默认变体。每次打包都用玩家自己的游戏文件生成，所以清单里只有标识，不含任何游戏数据。参与不需要任何设置：

1. **照常玩。** 每次打包都会把游戏里每个着色器按默认绑定状态各备一份。游戏遇到默认那份用不了的状态，或者建了 App 清单里没有的渲染管线，才会记进 `shader-list-contribution.txt`；默认变体不记。
2. **在 Mac 上，着色器和管线合计每攒够 100 条，它会在游戏启动前问你一次。** 点“发送”，GitHub 提交页面会打开，文件也会显示出来，拖进去就行。装了 GitHub 命令行工具（`gh`）并已登录时，对话框里会多一个“自动提交”，点了就直接替你把这一批发出去（私密 gist 加一个 issue），不用打开浏览器。iPhone 和 iPad 玩的时候从不提醒：设备连着打包它的那台 Mac（线连或同一 Wi‑Fi）时，由 Mac 来问。点“不要提醒了”以后就不再问。
3. **下次大家打包的 App 都会带上。** 谁到那里都不会再卡顿。

**[发送你的着色器 →](https://github.com/ly05010419/MacNativeGames/issues/new?template=shader-list.yml)** · 游戏不会自己上传任何东西。

**第一次进入游戏或新场景时，新着色器的编译可能造成卡顿，这是预期现象，不必惊讶。** 随着着色器缓存逐步预热，表现通常会改善；[公共清单](#让我们一起维护着色器清单游戏不再卡顿)每多一个着色器，就少一处卡顿。

## 更新说明

- **2026-11-11：排行榜，匿名的帧率榜单（王国之泪、旷野之息、时之笛，Mac）。** 设置面板（按 1）里单独一个标签页显示最新榜单，
  按锁帧和补帧设置分组。**我要上榜**默认关，不弹窗询问；打开后每 5 分钟在本机记一次这局的成绩，开机时和每 30 分钟上传；
  你自己那一行是绿色，所有人都只显示匿名玩家名（[操作与设置](docs/controls.zh-CN.md)）。着色器贡献也改为开机时和每 30 分钟上传。
- **2026-11-10：塞尔达传说 时之笛（Switch 2）登陆 Mac。** 用你自己导出的游戏（`.nup` 文件，或带更新 131072 的导出目录）
  通过 `build_app.py` 打包，不需要 keys。在 M4 上从标题玩进科奇里森林和村子，720p 或 1080p（720p 放大）站在村子里约
  81–83 fps；默认 720p。设置面板（按 1）里有分辨率、放大方式（MetalFX 或游戏自带）、锁帧和 MetalFX 补帧
  （[games/oot](games/oot/README.md)、[安装](games/oot/docs/install.zh-CN.md)）。
- **2026-11-09：异度神剑 X 2.0.0 的 Mac 诊断版，实验性。** 单独的完整性校验、累积更新合并、按版本打包；默认保持原版
  30 FPS，可选的 60 / 120 帧时间实验还在等更多人验收（[详情](games/xcx/README.md#update-200-mac-diagnostic)）。
- **2026-11-08：塞尔达传说 织梦岛 1.1.0 登陆 Mac，实验性。** 用你自己的游戏文件打包；在梅贝村一带手动玩过
  （[games/la](games/la/README.md)）。
- **2026-11-07：塞尔达无双 灾厄启示录除了 1.3.0，也能用 1.4.1 更新打包（Mac，实验性）。** 打包时从解包出的游戏目录
  读取版本（[`games/aoc`](games/aoc/README.md)）。
- **2026-11-03：王国之泪 1.4.3 在 Mac 上可选 SMAA 和 FXAA Quality。** 在效果页分别选择抗锯齿方式和质量；默认仍是
  游戏原版的 FXAA（[操作与设置](docs/controls.zh-CN.md)）。
- **2026-11-03：第一批 Switch 2 游戏，实验性。** 大金刚 Bananza 用开发构建可以在 Mac 上玩；宝可梦 Pokopia 能玩过开场
  进到第一个区域（脚本测试）。Pokopia 2.0.1 可以用你自己的转储目录打包成 Mac App：`python3 build_app.py <转储目录>
  --shared-fonts <王国之泪 / 旷野之息打包时提取的六个 .bfttf 字体>`（不需要 keys；[docs/ns2-port.md](docs/ns2-port.md)）。
- **2026-11-02：王国之泪不再支持 1.1.2。** 支持的版本是 1.0.0、1.2.1 和 1.4.3（1.4.3 测试得最多）。现在打包时会拒绝
  1.1.2 的更新包：请换成 1.2.1 或 1.4.3 的更新包，用同一个 `-o` 重新打包（[换游戏版本](docs/install.zh-CN.md)）。存档还在原处
  （`TOTK/profile`）；新版本第一次启动前，App 会先在旁边备份一份，1.2.1 直接读 1.1.2 的存档，1.4.3 会自动迁移。放在
  `mods/v112` 里的模组不会再加载：把对应版本的模组放进该版本自己的文件夹（[mods.md](docs/mods.md)）。

## 安装与游玩

从打包 App 到开始游玩的全部步骤见 **[docs/install.zh-CN.md](docs/install.zh-CN.md)**。推荐交给 AI 助理按[安装 skill](.claude/skills/install-zelda/SKILL.md) 去跑（见本页开头），不推荐自己一步步手动操作：

- [需要准备](docs/install.zh-CN.md#需要准备) · [打包 App](docs/install.zh-CN.md#打包-app) · [旷野之息](docs/install.zh-CN.md#旷野之息) · [iPhone 和 iPad](docs/install.zh-CN.md#iphone-和-ipad) · [Apple Vision Pro](docs/install.zh-CN.md#apple-vision-pro)
- [操作](docs/controls.zh-CN.md) · [文件位置](docs/install.zh-CN.md#文件位置) · [已知问题](docs/install.zh-CN.md#已知问题)
- [测试记录](docs/test-records.zh-CN.md)
- 时之笛（Switch 2）：[games/oot/docs/install.zh-CN.md](games/oot/docs/install.zh-CN.md)
- 红警 2 和尤里的复仇：[windows_games/ra2/install.zh-CN.md](windows_games/ra2/install.zh-CN.md)

## 工作原理

见 [docs/how-it-works.md](docs/how-it-works.md)。

已做的性能实验、待办与对比方法见 [Mac 性能待做](docs/performance-followups.md)。

用 macOS 27 的 Metal 性能工具按游戏状态测量（`TOTK_STATE_REPORTING=1`、`metal_perf_report.py`）：见 [Metal 性能诊断](docs/metal-performance-diagnostics.md#macos-27-上的-metal-性能诊断)。

## 致谢

- **[d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile)**（MIT）为《暗黑破坏神 II：狱火重生》开发了这套原生运行 Switch 游戏的方法：这里的 NSO 改写器、Horizon 宿主和 Metal 上的 NVN 层都从它的通用部分起步。
- **[nxnative](https://github.com/seedds/nxnative)**（MIT）：NSP 通用票据读取的做法借鉴自它；Switch 2 支持也参照并移植了它的代码（Ampere 着色器、几何着色器、SDK 20 服务、每个描述符池一段贴图 / 采样器编号、不带索引的间接绘制、拷贝的层间距）。
- **[ouncenative](https://github.com/kirakira-dev/ouncenative)**（MIT）：宝可梦 Pokopia 的着色器格式——它的 GLSLC 解析让我们看清了每个着色器的控制段和代码段在哪，着色器提取从 2,385 个变成了全部 131,090 个。
- **[pokopia-native](https://github.com/xFractals/pokopia-native)**（MIT）：宝可梦 Pokopia 的参考（运行时生成的着色器、转储目录结构、驱动改写的控制段字节）。
- **Ryujinx**（MIT）：着色器翻译器（`vendor/ryujinx`，预编成 `games/totk/prebuilt/macos-arm64/totk_xlate`）和系统服务的参考；`games/totk/runtime` 里的音效和混音器移植自 Ryujinx.Audio。
- **[SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross)**（Apache-2.0）把 SPIR-V 转成 Metal 着色语言；打包时按固定提交取得，用 clang 编。
- **[hactool](https://github.com/SciresM/hactool)**（ISC，自带的 mbedTLS 是 Apache-2.0）：打包时合并本体和更新包；按固定提交取得。
- **deko3d**（zlib）：贴图布局尺寸。**[bcdec](https://github.com/iOrange/bcdec)**（MIT 或公有领域）：BC 贴图解码。**libopus**、**libvpx**（BSD，来自 Homebrew）：音频和预渲染影片。
- **[x86recomp](https://github.com/JTM-rootstorm/x86recomp)**（MIT）是 Windows 游戏的重编译器和 Win32 运行时（`windows_games/ra2/third_party/x86recomp`，含我们的改动）；它用到 **Berkeley SoftFloat 3**（BSD-3-Clause）和 **CORE-MATH**（MIT），iOS 上还用到 **SDL3**（zlib）、**glslang**、**SPIRV-Cross** 和只带解码器的 **ffmpeg**（LGPL）。简体中文用 **[OpenCC](https://github.com/BYVoid/OpenCC)**（Apache-2.0）转换，补的字形取自 **[思源黑体 / Noto Sans CJK](https://github.com/notofonts/noto-cjk)**（SIL OFL 1.1）。
- **[BasaltVR](https://github.com/frenchbaguetteman/BasaltVR)**（MIT）负责串流 VR 画面：`zeldaVR/install_vr.sh` 按固定提交取得它，再打上我们的改动（`zeldaVR/streamer/basaltvr.patch`）。头显上显示画面的是 **[ALVR](https://github.com/alvr-org/ALVR)** 客户端（MIT），从 ALVR 的发布页下载并核对 SHA-256。**[BotW-BetterVR](https://github.com/Crementif/BotW-BetterVR)**（MIT）、UEVR 和 R.E.A.L. 是镜头、第一人称和体感操作的参考，没有拷代码。
- **时之笛**（`games/oot`，全部来源列在[它的 COPIES.md](games/oot/third_party/COPIES.md)）：NUP 读取用 **[nup](https://github.com/a55uka/nup)**（MIT）；AV1 影片解码改编自 **nxnative**，网格 / 任务着色器阶段、HMMA 矩阵运算的翻译和按类型的池数组改编自 **[ChiefSZ13/nxnative](https://github.com/ChiefSZ13/nxnative)**（MIT）；**dav1d**（BSD-2-Clause）随 App 带着，作 AV1 的后备解码；**zbic**（GPLv2）只作为独立的打包工具运行，不拷贝、不随 App 发布。Ampere 指令语义参考了 NVIDIA 的 CUDA Binary Utilities、PTX 文档和 Mesa 的 NAK，**[Lichii](https://github.com/zortos293/Lichii)**（GPL-3.0）只参考了字段含义，没有拷它的代码。
- 带了别处代码的社区移植，在各自目录的 `third_party/COPIES.md` 里列出。主项目和 Windows 游戏拷进来的代码及各自的许可，列在 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

## 许可证

MIT（见 [LICENSE](LICENSE)）。第三方代码保留各自的许可证：[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

## Star History / Star 历史

<a href="https://www.star-history.com/?repos=ly05010419%2FMacNativeGames&type=date&legend=bottom-right">
 <picture>
   <source media="(prefers-color-scheme: dark)" srcset="https://api.star-history.com/chart?repos=ly05010419/MacNativeGames&type=date&theme=dark&legend=bottom-right" />
   <source media="(prefers-color-scheme: light)" srcset="https://api.star-history.com/chart?repos=ly05010419/MacNativeGames&type=date&legend=bottom-right" />
   <img alt="Star History Chart" src="https://api.star-history.com/chart?repos=ly05010419/MacNativeGames&type=date&legend=bottom-right" />
 </picture>
</a>
