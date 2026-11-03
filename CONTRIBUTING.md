# Contributing

Thanks for helping. This page says how the project works and what a pull request needs. The easiest way to help needs no code at all: play, and [send your shaders](https://github.com/ly05010419/MacNativeGames/issues/new?template=shader-list.yml) (see the [shared shader list](docs/shader-list.md)).

[中文说明在下面](#参与贡献)

## Legal: no game content, ever

- Never commit game content: no game program or code, RomFS files, art, text, audio, translated shaders or libraries, keys or saves. The shader list holds shader keys and bind states only.
- The repository is MIT. Write your own code. Do not copy GPL code or code from other mods (UltraCam, Dynamic FPS, TOTK-Ultrawide and so on); reading them for ideas is fine. Third-party code you bring in keeps its license, and its source and license go into [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
- We do not help anyone obtain keys or game files.
- `git config core.hooksPath .githooks` once per clone turns on a check that stops a commit with keys, game containers or extracted ExeFS / RomFS files.

## How the port works, and what it must not become

- **The game's own ARM64 code runs natively on Apple silicon.** It is rewritten ahead of time, loaded and relocated, host calls go through gates, system services are answered by a small host (HLE), and NVN graphics are rebuilt on Metal. See [how it works](docs/how-it-works.md).
- **No CPU emulation.** No interpreter, JIT, dynamic binary translation or CPU emulator (Unicorn, QEMU, the CPU backends of Ryujinx or yuzu). Do not copy Horizon services, IPC or schedulers from an emulator. An emulator may be used outside the port as a reference (to compare a picture or confirm a behavior); it is not a result of the port.
- **Fix the platform, not the symptom.** ABI, TLS, loading, memory, threads, input, files and saves, audio, NVN on Metal and the app life cycle come first. A platform bug is fixed in the platform layer, not hidden behind another patch in game code.
- **Game patches for features are welcome** (frame rate, aspect ratio, output resolution). Every patch checks the original instruction at its address first and stops with an error when it differs; it never writes to an unchecked place.
- **The game is only ever rebuilt from the original files.** `build_app.py` starts from the unmodified ExeFS/NSO, applies the rules declared in the repository, and checks the input hashes (`expected-*.json`, `game_version.h`). Never patch an already rewritten binary or edit bytes by hand. When you add or change a build step, record what it used and produced (tool and rule fingerprints, options, output hashes) in the existing checks rather than in a new mechanism.
- **Change game logic as little as possible.** Do not change the player's or the world's state to make something pass. Gameplay changes are separate, optional features that can be switched off.

## Repository layout

- Tears of the Kingdom and Breath of the Wild share `games/totk`: one runtime, with switches for each game.
- Every other game lives in `games/<name>/` and carries its own copy of everything it builds and runs with: runtime, graphics layer, rewriter, shader translator (`prebuilt/`), and every script the build calls or imports. A change for one game never edits another game's files.
  - Its build and scripts also never **read** another game's folder: no `../totk` or `../by3` paths in the Makefile, no importing or running another game's scripts, no copying another game's prebuilt tools. Copy what you need into your folder instead, and list where each copy came from (folder and commit), for example in `third_party/COPIES.md`. Otherwise a change to that game can break yours without anyone touching your files.
  - Outside your folder, change only one line in the root README's update history. Ignore rules go in `games/<name>/.gitignore`, not the root one.
  - Check before opening the PR: `git grep -nE '\.\./(totk|botw|by3)|games/(totk|botw|by3)' -- games/<name>` finds nothing but comments, and `git diff --stat origin/main -- . ':!games/<name>' ':!README.md'` is empty.
- The NVN layer maps NVN semantics one by one (resources, shaders, state, draws, synchronization, presentation) and contains no camera, scene or gameplay decisions. Game-specific handling sits apart, says why it exists, is behind a switch, and is never needed for the game to run.

## Picture quality and performance

- **The picture must be correct before it is fast.** Missing objects, black blocks, wrong geometry or changed gameplay are never an acceptable price for frames.
- **Mac: no loss of picture quality or effects.** A performance change must leave the picture as it was; only floating-point differences of the kind the original already has are acceptable.
- **iPhone and iPad: a slight loss of picture quality is acceptable for performance.**
  - Resolution is never lowered (neither the render nor the output resolution).
  - Lower precision and reasonable floating-point differences are fine; pixel-exact output is not required.
  - An expensive effect may be reduced or removed when the picture as a whole only gets slightly worse. A clearly worse picture needs the maintainer's agreement first.
  - Keep iOS-only trade-offs behind a platform condition or an iOS switch, so the Mac picture does not change.
  - Say what was given up and what it bought, with before and after screenshots.
- **Measure.** Decide what to optimize from measurements of these games, not from another project's list of optimizations. Use existing tools first: Instruments (Metal System Trace, Time Profiler, Game Performance), Xcode GPU frame capture, the Metal HUD (`MTL_HUD_ENABLED=1`) and the repository's own diagnostic switches. Write your own timer only for what the tools cannot show, and say what it measures.
- **An optimization without an immediate frame-rate gain can still be right.** Explain when it applies, what work it removes (computation, memory traffic, synchronization, allocations) and what it adds. Keep it when the mechanism is clear and nothing regresses, even if another bottleneck hides the gain for now. Report the saving in that work, the added cost and the whole-frame change separately; do not call a local saving a frame-rate gain.
- Unfinished or faulty experiments stay switched off by default until they are fixed.

## Pull requests

- **One topic per pull request.** Unrelated features, debug code and reformatting go elsewhere. Do not change existing keys, defaults or verified version tables (`expected-*.json`, `game_version.h`) unless that is the point of the pull request.
- **Build what you touched.** Build the app for every game version you changed. Code shared with iOS (the runtime, `d2r_gfx`) must build for iOS too (`build_app.py --platform ios`); files used only by Breath of the Wild are excluded from the iOS project in `games/totk/ios/project.yml`. If you could not build one of them, say so.
- **Run the tests.** `python -m unittest discover -s tests` (what GitHub Actions runs), plus the probes and tests for the part you changed (`probe-*` targets in `games/totk/Makefile`, `tests/`).
- **Say how you tested, per platform:** built only, played on a Mac, or played on an iPhone or iPad. Name the game version and where in the game. Tests and probes passing do not replace playing the game.
- **Diagnostics are off by default,** switched on by an environment variable or an input-file command.
- **Commit messages:** `<type>: <description>`, with type one of feat, fix, refactor, docs, test, chore, perf, ci.

---

# 参与贡献

感谢参与。本页说明项目的做法和提交 pull request 的要求。最简单的帮忙不用写代码：照常玩游戏，然后[发送你的着色器](https://github.com/ly05010419/MacNativeGames/issues/new?template=shader-list.yml)（见[公共着色器清单](docs/shader-list.md)）。

## 法律：不放任何游戏内容

- 不提交任何游戏内容：游戏程序或代码、RomFS 文件、美术、文字、音频、翻译出的着色器或库、密钥、存档都不行。着色器清单只记着色器键和绑定状态。
- 仓库采用 MIT 许可，代码自己写。GPL 代码和其他模组（UltraCam、Dynamic FPS、TOTK-Ultrawide 等）的代码不要拷贝，只参考思路。引入的第三方代码保留原许可，来源和许可写进 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
- 我们不帮任何人获取密钥或游戏文件。
- 每个克隆执行一次 `git config core.hooksPath .githooks`，提交里有密钥、游戏容器或解出的 ExeFS / RomFS 文件时会被拦下。

## 移植的路线，和不能走的路

- **游戏自己的 ARM64 代码在苹果芯片上原生执行。**代码离线改写、装载并重定位，宿主调用经过宿主门，系统服务由一个小宿主应答（HLE），NVN 图形在 Metal 上重建。见[工作原理](docs/how-it-works.md)。
- **不做 CPU 模拟。**不用解释器、JIT、动态二进制翻译或 CPU 模拟器（Unicorn、QEMU、Ryujinx / yuzu 的 CPU 后端）。不从模拟器拷 Horizon 服务、IPC、调度器。模拟器只能在移植之外当对照（对比画面、确认行为），它的结果不算移植的成果。
- **修平台，不掩盖症状。**ABI、TLS、装载、内存、线程、输入、文件和存档、音频、NVN → Metal、App 生命周期优先。平台层的 bug 在平台层修，不靠往游戏代码里再加补丁盖住。
- **欢迎做功能性的游戏补丁**（帧率、画面比例、输出分辨率）。每处补丁先核对该地址的原指令，不对就报错停下，绝不往没核对过的位置写。
- **游戏只从原始文件重新生成。**`build_app.py` 从未改写的 ExeFS / NSO 出发，按仓库里声明的规则生成，并核对输入哈希（`expected-*.json`、`game_version.h`）。不在已改写的二进制上叠补丁，也不手改字节。新增或修改构建步骤时，在现有的核对里记下它用了什么、产出了什么（工具和规则的指纹、选项、产物哈希），不另起一套。
- **尽量不改游戏逻辑。**不为通过测试去改玩家或世界的状态。玩法改动做成单独、可以关掉的功能。

## 仓库结构

- 王国之泪和旷野之息共用 `games/totk`：一份运行时，按游戏加开关。
- 其他游戏各自放在 `games/<名字>/`，构建和运行用到的东西都自带一份：运行时、图形层、改写器、着色器翻译器（`prebuilt/`），以及构建会调用或 import 的每个脚本。改一个游戏不动别的游戏的文件。
  - 构建和脚本也**不读**别的游戏的目录：Makefile 里不写 `../totk`、`../by3` 这类路径，不 import 或调用别的游戏的脚本，不复制别的游戏的预编译工具。需要的东西复制进自己的目录，写明每份从哪个目录、哪个提交复制来的（例如 `third_party/COPIES.md`）。否则别的游戏一改，没人碰你的文件，你的游戏也可能坏掉。
  - 自己目录以外只改根 README 更新历史里的一行。忽略规则写在 `games/<名字>/.gitignore`，不改根目录的。
  - 开 PR 前自查：`git grep -nE '\.\./(totk|botw|by3)|games/(totk|botw|by3)' -- games/<名字>` 除了注释什么都搜不到，`git diff --stat origin/main -- . ':!games/<名字>' ':!README.md'` 是空的。
- NVN 层逐项对应 NVN 语义（资源、着色器、状态、绘制、同步、出图），不写相机、场景或玩法判断。游戏专用的处理单独放、写明原因、用开关控制，不能成为游戏运行的前提。

## 画质与性能

- **先画面正确，再谈速度。**缺东西、黑块、几何错误、改玩法，都不能拿来换帧率。
- **Mac：不牺牲任何画质和特效。**性能改动必须画面不变，只允许原本就有的那种浮点误差。
- **iPhone 和 iPad：允许轻微牺牲画质换性能。**
  - 不降低分辨率（渲染分辨率和输出分辨率都不行）。
  - 可以降精度、有合理的浮点误差，不要求逐像素一致。
  - 昂贵的特效可以降质量或去掉，前提是整体画面只是轻微变差。明显变差的，先和维护者商量。
  - 只在 iOS 上做的取舍用平台条件或 iOS 开关隔开，Mac 的画面不能变。
  - 写明牺牲了什么、换来多少，附前后对比截图。
- **以实测为准。**优化什么，按这些游戏自己的实测决定，不照搬别的项目的优化清单。先用现成工具：Instruments（Metal System Trace、Time Profiler、Game Performance）、Xcode GPU 帧捕获、Metal HUD（`MTL_HUD_ENABLED=1`），以及仓库自带的诊断开关。工具看不到的才自己写计时，并说明它测的是什么。
- **没马上涨帧的优化也可能是对的。**说明它在什么条件下成立、省掉了哪些工作（运算、访存、同步、分配）、新增了什么成本。机制清楚、没有退化的就保留，即使暂时被别的瓶颈盖住了收益。分开报告这部分省了多少、额外花了多少、整帧变了多少；局部节省不要说成帧率提升。
- 没做完或有问题的实验，修好之前默认关闭。

## Pull request

- **一个 PR 只做一件事。**无关的功能、调试代码、格式化放到别处。除非这正是 PR 的目的，否则不要改已有的按键、默认值和已验证的版本表（`expected-*.json`、`game_version.h`）。
- **改到哪里就编译哪里。**改了哪个游戏版本，就打包哪个版本。和 iOS 共用的代码（运行时、`d2r_gfx`）也要能编译 iOS（`build_app.py --platform ios`）；只给旷野之息用的文件在 `games/totk/ios/project.yml` 里从 iOS 工程排除。哪个没能编译，请写明。
- **跑测试。**`python -m unittest discover -s tests`（GitHub Actions 跑的就是它），以及改到部分的探针和测试（`games/totk/Makefile` 里的 `probe-*` 目标、`tests/`）。
- **按平台写清怎么测的：**只编译过、在 Mac 上玩过，还是在 iPhone / iPad 上玩过。写明游戏版本和在游戏里的哪里测的。测试和探针通过，不能代替实际玩一遍。
- **诊断默认关闭，**用环境变量或输入文件命令打开。
- **提交信息：**`<type>: <description>`，type 取 feat、fix、refactor、docs、test、chore、perf、ci。
