# 红色警戒 2 和尤里的复仇：安装步骤

[English](install.md)

用你自己的《红色警戒 2》（`game.exe`，1.006）和《尤里的复仇》（`gamemd.exe`，1.001），把 x86 代码静态重编译成原生 arm64
程序：Mac 上各一个可执行文件，iPhone / iPad 上一个 App 装两个游戏。这个目录只有适配代码：构建脚本、启动器和游戏专用的钩子、
重编译器用的函数表和清单、测试，以及重编译器本身。**这里没有任何游戏内容**：没有游戏代码（原版或重编译后）、美术、文字、
声音、MIX 文件或密钥。游戏由你自己提供，从它生成的一切都只留在你的 Mac 上。

做法：[x86recomp](https://github.com/JTM-rootstorm/x86recomp)（MIT）把游戏可执行文件的每条 x86 指令翻成 C++（精确的 x87 和
标志位模型），再编译成 arm64；它的运行时接管 Win32 调用（HLE）。DirectDraw 经 SDL_GPU 画到 Metal。x86recomp 放在
`third_party/x86recomp`（上游 `1c90fe3`，我们的改动已经在里面，清单见它的 `UPSTREAM.md`）：MMX、DirectDraw v1/v2 的 2D Blt、
DirectSound v1、按 [MS-CFB] 规范自写的 OLE 复合文件存档、IMM、代码页、触控等。Wine、Bochs 和 GPL 代码只作行为参考，从不复制。

## 需要

- Apple 芯片的 Mac；Xcode 命令行工具、`cmake`、`ninja`、LLVM（`brew install llvm`）、SDL3、带 Pillow 的 Python 3。
- 你自己的《红色警戒 2》和《尤里的复仇》，例如 Steam 版（app 2229850），装到本目录的 `game/install`（`game/` 不进 git）。
  用 steamcmd 登录你自己的账号：
  `steamcmd +@sSteamCmdForcePlatformType windows +force_install_dir "$PWD/game/install" +login <你的账号> +app_update 2229850 validate +quit`

## 构建

```sh
cd windows_games/ra2
scripts/build.sh            # 或者：scripts/build.sh <你的安装目录>
```

它先编出 x86recomp 的重编译器，把你的可执行文件重编译到 `game/generated/`，再编出 `build/main/ra2`；安装目录里也有
`gamemd.exe` 时连 `build/main/yr` 一起。再跑一次只重编改过的部分（第一次要一阵：重编译出的代码有几百 MB 的 C++，M4 上两个游戏
约 7 分钟）。`BUILD_DIR`、`GENERATED_DIR`、`JOBS`（默认 6，按 16 GB 内存）可以改位置和并行数。

重编译（`scripts/lift.sh`）先用 `config/symbols/sha256.txt` 核对你的可执行文件的 SHA-256，再用 `config/symbols/<模块>/` 里的
函数表和跳转表（Steam 版可执行文件的 Ghidra 导出，只有地址和标志，不带函数名）。`config/<模块>.manifest` 逐条写明理由：
启动器接管的游戏函数（日志、限帧器、侧栏布局、战场每帧更新）和重编译器的修正；只有地址和标志。

## 其他版本

这些表对应 Steam 版的红警 2 1.006 和尤里的复仇 1.001；可执行文件不是这两个版本时，`build.sh` 会停下并说明。用 Ghidra 12
（`brew install ghidra openjdk@21`）自己导出，每个模块一份（`game` 对应 GAME.EXE、`yr` 对应 gamemd.exe、`blowfish` 对应
Blowfish.dll），再用它们构建：

```sh
JAVA_HOME=/opt/homebrew/opt/openjdk@21 MAXMEM=6G /opt/homebrew/opt/ghidra/libexec/support/analyzeHeadless \
  game/ghidra ra2 -import game/install/GAME.EXE -overwrite -scriptPath third_party/x86recomp/tools/ghidra \
  -postScript ExportFunctions.java game/symbols/game/functions.tsv \
  -postScript ExportJumpTables.java game/symbols/game/jumptables.tsv
SYMBOLS_DIR=game/symbols scripts/build.sh
```

`config/*.manifest` 和 `src/` 里游戏专用的地址也是这两个版本的，换版本时这些也要一并核对。

## 运行

`build/main/ra2` 或 `build/main/yr`。游戏目录依次取 `--game <目录>`、`RA2_GAME_DIR`、构建时用的安装目录。选项：
`--resolution auto|WxH|ini`、`--fps-cap <n>`、`--sidebar-scale <倍数>`、`--no-command-bar`；环境变量 `RA2_*` / `YR_*`
（如 `RA2_RESOLUTION`）作用相同。游戏写回的文件（INI、注册表）放在覆盖层 `~/Library/Application Support/ra2/install/`
（尤里的复仇是 `yr`）。
游戏在窗口里运行，窗口可以拉大缩小（画面保持比例），点绿色按钮全屏；`RA2_FULLSCREEN=desktop` 一启动就全屏。
触控控件只在 iPhone / iPad 上有，`RA2_SCREEN_CONTROLS=1` 在 Mac 上也显示。`scripts/run_attempt.sh <名字> [秒]` 限时跑一次，日志在 `game/attempts/`。

## iPhone 和 iPad

```sh
scripts/build.sh                   # 先在 Mac 上生成重编译的代码
scripts/ios/build_app.sh           # 一个 App，启动时问玩哪个游戏（第一次会先为 iOS 编 SDL3、glslang、SPIRV-Cross 和只带解码器的 ffmpeg）
python3 -I scripts/ios/install_device.py --game game/install   # 用你自己的团队签名，数据线安装
```

`--game` 在签名前把你自己的游戏文件放进 App，装好就能玩（约 1.3 GB，数据线不到一分钟）。App 里的游戏文件只读，存档和设置写在
App 的“文稿”里，重装不丢。装着动画的 MIX 文件放的是在 Mac 上做一次的紧凑版（`scripts/ios/compact_movies.py`，需要
`brew install ffmpeg`，第一次约 10 分钟）：每段动画的画面从 Bink 重新编码成 VP9 和 Opus，游戏文件约 1.1 GB，原来是 1.9 GB；
`--full-movies` 放原文件。不带 `--game` 装的 App 从“文稿”读游戏：把安装目录拷进去并命名为 `RA2`（访达 › iPhone › 文件）。
对局中会出现触控控件（摇杆、编队按钮、强制攻击、取消）。App 图标由 `scripts/ios/make_icon.py` 画出（原创图形，不含游戏素材或
发行商标志）。

## 语言

English、繁體中文（Steam 版官方繁体）和简体中文（从官方繁体转换）。中文来自 Steam 的语言包（depot 2229855；用 steamcmd 登录你自己的
账号：`steamcmd +@sSteamCmdForcePlatformType windows +login <你的账号> +download_depot 2229850 2229855 +quit`）：

```sh
python3 -I scripts/languages.py game/install <语言包目录>
```

写出 `game/install/lang/zh-Hant`（官方字符串表原样）和 `lang/zh-Hans`（用 OpenCC 转成简体，`brew install opencc`；游戏字体补上
Big5 没有的字形，从思源黑体 / Noto Sans CJK SC 画，SIL OFL 1.1 许可，按固定版本下载并核对）。游戏默认跟系统语言；Mac 上用 `--language en|zh-Hant|zh-Hans`（或 `RA2_LANGUAGE` / `YR_LANGUAGE`）
选，iPhone / iPad 上在第一个对话框里点语言按钮（下次启动记住）。MIX 公钥从你自己的 `game.exe` 里读（`scripts/mixls.py`）。

## 测试

`tests/mmx_test.cpp` 用按 Intel 手册手算的期望值检查每条 MMX 指令：
`clang++ -std=c++20 -I third_party/x86recomp/src tests/mmx_test.cpp -o build/mmx_test && build/mmx_test`。x86recomp 自己的
测试（`ctest --test-dir build/main`）包括我们加的复合文件、代码页、帧布局、触控手势和紧凑动画测试；
`python3 -I -B -m unittest discover -s tests` 检查紧凑动画的 MIX 文件。

## 状态

两个游戏在 Mac 和 iPhone 14 上都能进遭遇战对局；Mac 上存档、读档正常。细节、数据和未解决的问题见
[reference/FEASIBILITY.md](reference/FEASIBILITY.md)。

第三方代码：x86recomp（MIT，`third_party/x86recomp/LICENSE`），含我们的改动；配置时它会拉取 Berkeley SoftFloat 3（BSD-3-Clause）
和 CORE-MATH（MIT）。SDL3（zlib）、ffmpeg（LGPL，只有解码器）、glslang 和 SPIRV-Cross 由 iOS 脚本拉取并编译。见仓库的
`THIRD_PARTY_NOTICES.md`。
