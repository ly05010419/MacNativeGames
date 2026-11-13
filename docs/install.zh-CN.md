# 安装

[← 返回 README](../README.md) · [English](install.md)

本页只讲一件事：用**你自己的**游戏文件，在 Mac、iPhone 或 iPad 上打包出 App。一次打包 20–40 分钟，大部分时间是等待。操作与设置见 [controls.zh-CN.md](controls.zh-CN.md)，测过什么见 [test-records.zh-CN.md](test-records.zh-CN.md)。AI 助理：同样的步骤写成了清单，在[安装 skill](../.claude/skills/install-zelda/SKILL.md) 里。

**时之笛**（Switch 2）有自己的安装页：[games/oot/docs/install.zh-CN.md](../games/oot/docs/install.zh-CN.md)。

目录：[需要准备](#需要准备) · [打包 App](#打包-app) · [旷野之息](#旷野之息) · [iPhone 和 iPad](#iphone-和-ipad) · [Apple Vision Pro](#apple-vision-pro) · [更新 App](#更新-app) · [打包报错](#打包报错) · [文件位置](#文件位置) · [已知问题](#已知问题)

基本规则：
- 只用你自己的 Switch 和你自己的游戏导出的文件。打包不会替你下载任何东西，本仓库里也没有游戏内容。
- 不要为了绕过检查去改 `games/totk/assets/expected-*.json` 或脚本：报错信息会说明哪里不对。
- 打包出的 App 里含有游戏，只供你自己使用。

## 需要准备

**Mac**：Apple 芯片，**macOS 26** 或更高（图形用 Metal 4），建议 16 GB 或更多内存。
还在用 macOS 15 的，用 [1918662410-ship-it 的社区 fork](https://github.com/1918662410-ship-it/MacNativeGames)，它加了 Metal 3 后端（本仓库不测）。

**游戏文件**：从你自己的 Switch 导出。标题 ID 一般写在文件名的方括号里；打包认的是文件内容，不看文件名。

| 标题 ID | 是什么 | 命令里放在 |
|---|---|---|
| `0100F2C0115B6000` | 王国之泪本体 **1.0.0**，XCI 或 NSP | 第一个参数 |
| `0100F2C0115B6800` | 王国之泪更新 **1.2.1 或 1.4.3**，NSP 或 NSZ（可选） | `--update` |
| `01007EF00011E000` | 旷野之息本体，或 **1.6.0** 卡带镜像（本体、更新、DLC 在同一个 XCI 里） | 第一个参数 |
| `01007EF00011E800` | 旷野之息更新 **1.6.0 或 1.9.0**，NSP 或 NSZ | `--update` |
| `01007EF00011F001`、`01007EF00011F002` | 旷野之息的两个 DLC 包，NSP（都可选） | `--dlc`，每个包一次 |

- 王国之泪推荐 **1.4.3**：测得最多，也是唯一在 iPhone / iPad 上测过的版本。不需要中间版本的更新。不加 `--update` 打出来的是 1.0.0。
- 本体不能是压缩格式。NSZ / XCZ 本体先解压（`nsz -D`）。
- 卡带镜像里自带了更新也可以，版本仍由 `--update` 决定。

**`prod.keys`**：和游戏文件来自同一台 Switch。比更新旧的 keys 文件解不开这个更新。

**系统字体**：本体是 **NSP** 时需要，**旷野之息**一律需要。二选一：
- `--firmware <文件夹>`：你导出的固件，一个装着 `.nca` 文件的文件夹（压缩包先解开），或已安装固件的 `registered` 文件夹。
- `--shared-fonts <文件夹>`：以前打包王国之泪时提取出的六个 `.bfttf` 文件，在 `<工作目录>/totk/extracted/firmware/shared-fonts`（默认是 `~/MacNativeGames-work/totk/…`）。

王国之泪的 XCI 自带字体，不用另给。

**磁盘空间**（工作目录所在的卷）：

| | Mac | iPhone / iPad |
|---|---|---|
| 只有本体 | 25 GB | 45 GB |
| 加更新 | 40 GB | 60 GB |

不带 `--work` 时，工作目录用 `-o` 那个 App 记录的；没有记录就用放 App 的文件夹旁边的 `MacNativeGames-work/<游戏>`（App 在 `~/Applications` 或 `/Applications` 时都是 `~/MacNativeGames-work/totk`）。MNG Launcher 用同一条规则。工作目录和 App 放在**同一个 APFS 卷**上：17 GB 的游戏数据是克隆过去的，不额外占空间。游戏文件在 exFAT / FAT 盘或快满的盘上时，用 `--work <文件夹>` 和 `-o <App>` 放到有空间的 APFS 盘上。

## 用 MNG Launcher 或 AI 打包

不管谁来操作，打包都用同一个引擎：MNG Launcher 的窗口，或终端里的 `tools/mng.py`（帮你的 AI 用的就是它）。两者都运行 `tools/builder_jobs`，遵守同一套规则（见下），所以用一种方式打的游戏，可以用另一种方式更新。

### MNG Launcher（窗口）

如果下载的是 GitHub Releases 提供的 Launcher ZIP，解压后把 **MNG Launcher.app** 拖入“应用程序”。
它自带 Python 和配套构建脚本，用户无需编译 Launcher 或手动下载源码。尚未公证的测试版首次打开可能被系统阻止，
确认来源后在“系统设置 → 隐私与安全性”中选择“仍要打开”。打包游戏仍需要开发工具和你自己的游戏文件。
更新下载版时换成新版 Launcher，界面和配套脚本一起更新。

从源码生成开发版：

运行 `python3 tools/builder_app/make_app.py` 生成 `~/Applications/MNG Launcher.app`。它自带 Python，运行的是这个文件夹里的代码，所以 `git pull` 之后不用重新生成，除非窗口本身改了。

- **游戏**（第一页，顶部没有标签栏）：每个游戏一张卡片。打包过的游戏显示它自己 App 的图片；点它出现右上角绿色的大按钮**启动游戏**，以及更新按钮，按能用的最快的一种命名：**极速更新**或**快速更新**（不用游戏文件），或**完整打包**（进入打包步骤，要游戏文件）；还有它的各页（点按钮就在下面展开）。没打包的游戏显示加号：点它进入打包三步，第一步拖入游戏文件，第二步选 `prod.keys`，第三步点**打包并游玩**，全程显示进度。用 `open -a "MNG Launcher" "<游戏文件>" "<更新>" "<prod.keys>"` 打开，会直接进入打包三步，第一、二步自动填好。
- 打包过的游戏在卡片下面展开的各页：**设置**（王国之泪 1.4.3、旷野之息 1.9.0 的开局设置，和游戏里的设置面板是同一份：每项显示你的设置，没设过的显示游戏默认值；点**启动游戏**就按这些设置启动）、**模组**（你自己的模组，对照你的 App 检查，用仓库自己的安装器装；不带任何模组）、**按键**（手柄布局、按键图标、按键测试）、**存储**（工作目录：大小和清理；着色器缓存：位置和大小；点**计算空间占用大小**才计算，之前显示上次算的结果）、**存档**（备份和恢复）。旧窗口（`Build Game.command`）已经去掉，由这几页代替。细节见 [builder-app.md](builder-app.md)；改代码的人看 [builder.md](builder.md)。

### 用 AI 或终端：`tools/mng.py`

任何 `python3` 都能启动它（实际用 MNG Launcher 自带的 Python，没有就用 Python 3.14）。帮你打包的 AI 应该用它，不要自己拼 `build_app.py` 命令，而且**第一步永远是 `mng migrate`**：只有迁移完成的 Mac 才打开 MNG Launcher（在那之前 `mng open` 会拒绝）。

| 命令 | 做什么 |
|---|---|
| `python3 tools/mng.py migrate` | 这台 Mac 迁移完了吗？（`--apply` 把剩下的做完；见[老用户迁移](#老用户迁移到-mng-launcher)） |
| `python3 tools/mng.py status` | 这台 Mac 上已装的游戏，以及各自要哪种更新 |
| `python3 tools/mng.py check "<本体>" "<更新>" --keys "<prod.keys>"` | 文件分别是什么、会打出什么、这台 Mac 还缺什么 |
| `python3 tools/mng.py build "<本体>" "<更新>" --keys "<prod.keys>"` | 打包这个游戏（已装过且版本相同时，就走更新） |
| `python3 tools/mng.py update totk` | 不用文件更新已装的游戏（`botw`、`oot`、名字或 App 路径也行） |
| `python3 tools/mng.py page <settings\|mods\|controls\|storage\|saves> <动作> --game totk` | 窗口里各页面的功能 |
| `python3 tools/mng.py open "<文件>" "<prod.keys>"` | 打开 MNG Launcher 并填好这些文件（只在迁移完成后） |

它会把每个阶段、步骤和日志行打印出来，每分钟报一次进度，最后报结果（加 `--json` 输出窗口读的原始数据）。`mng` 不做的事（iPhone 和 iPad、`--mod`、`--vr`）用[打包 App](#打包-app) 一节里的 `build_app.py` 命令。

### 两者共同遵守的规则

1. **目录规则只有一条。** App 放在你选的地方（MNG Launcher 默认 `~/Applications`）；工作目录用 App 已经记录的，没有就用放 App 的文件夹旁边的 `MacNativeGames-work/<游戏>`（App 在 `~/Applications` 时是 `~/MacNativeGames-work/totk`）。`build_app.py` 不带 `--work` 时也一样。
2. **按顺序选能用的最快的更新：** 极速（`update_runtime.py`，几分钟，不用游戏文件）→ 快速（`--seed`，用工作目录重做启动着色器）→ 完整打包（`build_app.py`，要游戏文件和密钥）。`update_runtime.py "<App>" --plan` 会说该走哪种、为什么；窗口的**更新**和 `mng update` 就是按它走。给已装的游戏拖入同版本的文件也走更新；换了版本就完整打包到原来那个 App。
3. **一台 Mac 同时只跑一个打包或更新**，不管是谁开始的。第二个会被拒绝，并说明正在跑的是哪个、谁开始的。窗口会显示在别处开始的任务（“从 … 开始的”）并能停止它；只有自己开始的任务结束后才会自动开游戏。
4. **你的文件留在原处**：游戏文件和密钥都在原位置读取，不复制，也不上传任何东西。日志在 `~/Library/Logs/MacNativeGames/`；正在跑的任务在 `~/Library/Application Support/MacNativeGames/Builder/jobs/`。
5. **不用 Homebrew**：打包需要的工具和库都由打包自己下载固定版本（见[需要准备](#需要准备)）。
6. **先迁移**：AI 做任何事之前先检查迁移情况，迁移完才打开 MNG Launcher。

老用户从旧安装迁移：见[老用户迁移到 MNG Launcher](#老用户迁移到-mng-launcher)。

## 打包 App

**1. 装工具**（只需一次，要联网）：

```sh
xcode-select --install
# Python 3.14：从 https://www.python.org/downloads/macos/ 下载安装包
#   或者用 Homebrew：brew install python@3.14
# opus、libvpx、dav1d 不用装：打包时从固定版本的源码自己编
```

也可以不装 Python，改用 [uv](https://docs.astral.sh/uv/)：把 `python3.14 build_app.py …` 换成 `uv run --python 3.14 build_app.py …`。

可选（装了完整 Xcode 时）：`xcodebuild -downloadComponent MetalToolchain`。
- 装了它，打包时会把启动着色器直接编进 App：App 之后可以随意挪动，重新打包也更快。
- 不装也能打包，着色器会编进 macOS 的 Metal 缓存，绑定 App 所在的位置。

**2. 取得本仓库**：`git clone https://github.com/ly05010419/MacNativeGames.git`（或下载 ZIP），然后在 `MacNativeGames` 文件夹里打开终端。已经有的，先 `git pull`：很多打包报错在 `main` 上早就修好了。

**3. 打包。** 每个路径都加引号。用 `-o` 一次定好 App 的最终位置：Mac App 之后再挪动，第一次启动会重新编译着色器（装了 Metal 工具链的除外）。不加 `-o` 时，App 放在游戏文件旁边。

```sh
# 王国之泪 1.4.3，本体是 XCI
python3.14 build_app.py "<本体 1.0.0.xci>" -k "<prod.keys>" --update "<1.4.3 更新.nsp>" -o "/Applications/Tears of the Kingdom.app"

# 王国之泪 1.4.3，本体是 NSP：还要给字体
python3.14 build_app.py "<本体 1.0.0.nsp>" -k "<prod.keys>" --update "<1.4.3 更新.nsp>" --firmware "<固件文件夹>" -o "/Applications/Tears of the Kingdom.app"
```

`/Applications` 和工作目录不在同一个卷上时，`-o` 改用工作目录所在卷上的文件夹。

在 16 GB 及以上内存的 Mac 上，1.4.3、1.2.1 和旷野之息的 App 会带上原生 4K（设置面板里的 3840×2160 分辨率），每次启动多预留内存（王国之泪约 1.4 GiB，旷野之息约 0.5 GiB）。加 `--no-native-4k` 不带；内存更小的 Mac 想带就加 `--native-4k`。

Mac 上的王国之泪 1.4.3、旷野之息 1.6.0 / 1.9.0 在构建机器有 **32 GB 及以上内存**时，默认带上原生 5K（5120×2880 的画布、界面和 3D 场景）以及 4K 档位。16 GB、24 GB 的 Mac 保持默认 4K。`--native-5k` 可在更小内存的机器上开启，`--no-native-5k` 不带；`--no-native-4k` 也会关闭自动 5K。这只增加可选档位，保留上次选择的分辨率，不会自动切换到 5K。王国之泪带 5K 的 App 每次启动多预留约 2.7 GiB 游戏内存，即使选择较低分辨率也一样；物理页按需提交。不包含 HDR，也不保证 120 帧。

运行程序更新会保留明确选择的 5K 开关。没有明确选择时，32 GB 及以上内存的 Mac 会在工作目录仍有与此 App 匹配的已验证游戏文件时补齐 4K/5K 资源，不需要重新提供游戏容器或密钥。已有 5K 的 App 在较小内存机器上更新时仍保留 5K。缓存缺失或 App 使用了修改过的堆资源时，自动更新保留现有规格，并说明需要重新构建。王国之泪使用 `update_runtime.py --native-5k` 时要求可用的匹配缓存；旷野之息直接启用代码管理的堆，不生成 RomFS 堆覆盖；`--no-native-5k` 移除 5K、保留 4K。两者都不改写已保存的分辨率文件。

**4. 让它跑完。** M 系列 Mac 上从头打包要 **20–40 分钟**，8 GB 内存的 Mac 更久：

| 步骤 | 时间 |
|---|---|
| `1/4 code`：把游戏代码改写成 Apple 芯片版 | 约 3 分钟 |
| `2/4 data`：提取并校验 17 GB 数据，合并更新 | 几分钟 |
| `3/4 shaders`：准备启动着色器 | 约 3 分钟 |
| 为本机 GPU 编译着色器 | 6–12 分钟 |
| `4/4 app`：macOS 检查一次新 App | 2–3 分钟 |

- 长时间没有新输出、或出现 `... still running (3 min)`，都是正常的，不要停。
- 万一被打断，原样再跑**同一条命令**：已完成的步骤保存在工作目录里，会直接复用。
- 打包前先退出游戏。同一个工作目录不要同时跑两次打包。
- 最后几行出现 `done in N min: <App 路径>` 就是成功了。

**5. 打开 App**（在访达里打开）：
- 启动画面先校验游戏数据（约 20 秒），再准备着色器和管线，有进度条。第一次启动要 1–2 分钟，之后约 12 秒。
- 打包后又把 App 挪到别的卷，macOS 会重新检查一遍：几分钟内不出窗口，等着就行。
- macOS 询问是否允许访问可移除宗卷上的文件时，点“允许”。
- 第一次到新地方可能短暂卡顿。
- **1** 键打开设置面板。操作说明见 [controls.zh-CN.md](controls.zh-CN.md)。

## 旷野之息

同一个脚本也能打包旷野之息，但一律要给字体（见[需要准备](#需要准备)）。工作目录是 `BotW_build`，App 是 `Breath of the Wild.app`。M4 上从头打包约 16 分钟。

```sh
# 1.6.0，卡带镜像（本体、更新、DLC 在同一个 XCI 里）
python3.14 build_app.py "<旷野之息 1.6.0.xci>" -k "<prod.keys>" --shared-fonts "<…/MacNativeGames-work/totk/totk/extracted/firmware/shared-fonts>" -o "/Applications/Breath of the Wild.app"

# 1.9.0（或 1.6.0），用 NSP：本体加更新，DLC 包都可选
python3.14 build_app.py "<旷野之息本体.nsp>" -k "<prod.keys>" --update "<1.9.0 更新.nsp>" --dlc "<DLC 1.nsp>" --dlc "<DLC 2.nsp>" --firmware "<固件文件夹>" -o "/Applications/Breath of the Wild.app"
```

用单独的更新包时，App 里只有你用 `--dlc` 给的 DLC 包。卡带镜像自带 DLC，这时不要加 `--dlc`。

## iPhone 和 iPad

**需要：**
- iOS / iPadOS 26，iPhone 14 或更新的机型。
- Mac 上装好完整的 **Xcode 26** 或更高版本（含 iOS 平台）。不用 Homebrew：XcodeGen、CMake、Ninja 由打包自己下载固定版本。
- 在 Xcode › Settings › Accounts 里登录你的 Apple ID（免费账号也行）。
- 设备上打开开发者模式。

**1. 打包 IPA。** 在上面的 Mac 命令后加 `--platform ios --ios-game-data documents`，`-o` 写成 `.ipa` 文件：

```sh
python3.14 build_app.py "<本体 1.0.0.xci>" -k "<prod.keys>" --update "<1.4.3 更新.nsp>" --platform ios --ios-game-data documents -o "<文件夹>/Tears of the Kingdom.ipa"
```

- 第一次打 iOS 包时，还会下载并编译设备用的库。
- `--ios-game-data documents` 不把 17 GB 的游戏镜像放进 IPA，以后重装只要几分钟。
- 打包时用数据线连着 iPhone / iPad：打包会在 Mac 上为它的 GPU 编好游戏的管线（第一次多约 10 分钟），首次启动进标题前就不用长时间填充。设备没连着时加 `--device-gpu <架构>`（iPhone 14 是 `applegpu_g14p`）。

**2. 安装**，设备用线连着 Mac：

```sh
python3.14 games/totk/ios/tools/install_device.py "<文件夹>/Tears of the Kingdom.ipa" --device "<访达里显示的设备名>" --game-data "<工作目录>/totk/extracted/<版本>/romfs/romfs.bin"
```

- `<版本>` 填那里实际有的文件夹（1.4.3 是 `v143`；用 `ls "<工作目录>/totk/extracted"` 看）。第一次安装会把 17 GB 的镜像拷过去一次。
- iPhone / iPad 只连着 Wi‑Fi 时脚本会停下：插上数据线、解锁设备，再跑一次。加 `--wifi` 也能走 Wi‑Fi 装，但很慢，游戏数据可能传到一半断开。
- 免费 Apple ID 的签名 7 天有效，付费账号一年。到期后再跑同一条命令续签，存档会保留。
- 设备提示时，到“设置 › 通用 › VPN 与设备管理”里信任开发者。

**3. 第一次启动：** iPhone 14 上先校验游戏数据约 15 秒，再导入着色器约 3 分钟，然后出标题。之后约 8 秒出标题。

iPhone 默认掌机模式：内部按 960×540 渲染，放大到 1280×720，最高 60 帧。iPad 默认底座 1080p：画面 1600×900，和 Switch 一样，放大到 1920×1080（开始界面和“设置”App 的 Full HD 可以切换；旷野之息还可选 1080p 高清，画面 1920×1080）。旷野之息 1.6.0 也按同样的方法打包安装。

两个限制：
- 手机不能边玩边翻译着色器。App 里没带的着色器变体会被跳过，所以还没有玩家报告过的地方可能少画一些东西。
- iPhone 14 上，在复杂区域内存耗尽时 iOS 可能会关掉游戏。

## Apple Vision Pro

只支持王国之泪，16:9 窗口内用手柄游玩。

准备：
1. 在 Xcode › Settings › Components 里安装 visionOS 平台。
2. 把头显和 Xcode 配对。
3. 打开开发者模式。

然后打包并安装：

```sh
python3.14 build_app.py "<本体 1.0.0.xci>" -k "<prod.keys>" --platform visionos --ios-game-data documents -o "<文件夹>/TOTK-Vision.app"
python3.14 games/totk/ios/tools/install_device.py "<文件夹>/TOTK-Vision.app" --device "<Vision Pro 名称>" --game-data "<工作目录>/totk/extracted/v0/romfs/romfs.bin"
```

加了 `--update` 时，把 `v0` 换成对应版本的文件夹（`v121` 或 `v143`）。

## 更新 App

用哪种更新，固定按这个顺序（先退出游戏）。一律先跑极速更新，它会告诉你要不要用下一种。

| 更新 | 什么时候 | 命令 | 需要 |
|---|---|---|---|
| **极速更新** | 本仓库每次有新版本（`git pull` 之后）；「更新」页跑的就是它 | `python3.14 update_runtime.py "<App 路径>"` | App 记录的工作目录；不需要游戏文件和密钥 |
| **快速更新** | 极速更新提示启动着色器不再匹配时（王国之泪、旷野之息、大金刚、Pokopia：翻译链换了）。时之笛没有这一档：它的极速更新自己核对着色器 | `python3.14 update_runtime.py "<App 路径>" --seed` | 工作目录里的游戏文件；不需要游戏容器和密钥 |
| **普通更新**（完整打包） | 上面两种提示需要完整打包时：换游戏版本、DLC 或游戏文件，代码改写规则或画面尺寸覆盖变了，时之笛的翻译器或翻译链变了，2026 年 11 月 11 日以前准备着色器的时之笛 App，工作目录不在了或删掉了 | 和第一次打包一样运行 `build_app.py`，`-o "<App 路径>"` 不变，用 App 记录的 `--work` | 游戏文件和密钥 |

大金刚和 Pokopia 从 2026 年 11 月 12 日起也有这三种更新（之前只有普通更新）；每种更新都会复用没变的翻译和管线。

- **本仓库有新版本**（`git pull` 之后）：
  - 先退出游戏，再运行 `python3.14 update_runtime.py "<App 路径>"`，这就是极速更新（见下），不需要游戏文件和密钥。
  - 它提示启动着色器不再匹配时，加上 `--seed`（快速更新）。
- **「更新」页**（设置面板，按 1）显示 App 是不是上游最新的 `main`，不是就列出每个新提交。打开这一页时，会在打包 App 的项目文件夹里拉取上游（不改动其中任何文件）。点**更新**按钮：游戏关闭，项目的 `main` 快进到上游（`git pull --ff-only`），运行 `update_runtime.py`（它要求时加 `--seed`），期间有一个小窗口显示正在更新，完成后重新打开游戏。项目不在 main 分支、有没提交的改动或上游没有的提交、找不到或连不上、或更新失败时，会说明原因，请你自己更新（本列表第一条）。2026 年 10 月 31 日以前打的 App 要先手动运行一次 `update_runtime.py`，之后才知道自己的项目文件夹。
- **极速更新**：对王国之泪、旷野之息、时之笛的 App 运行 `update_runtime.py`，只重编运行时，输入没变的东西一律复用。复用前都会核对，不是直接假定没变：
  - **翻译好的着色器**（时之笛）：每个翻译记着当初发给翻译器的请求。更新时让新运行时把请求重新生成一遍，请求一样的翻译直接复用，不一样的才重新翻译。王国之泪和旷野之息的启动着色器不受运行时影响。
  - **Metal 着色器库**：只重编翻译变了的。
  - **管线存档**：每条清单里的管线都对着上次的存档建一次（Metal 核对函数和状态）。没变的直接从存档取，变了的才现编，写进新文件。
  - **文件核对**：文件还是原来那个（设备、inode、大小、修改时间都没变）的，沿用上次记下的哈希；只读新的或变了的文件。种子的每个文件都还是已发布的那个、来源也相同时，种子原样保留，用它签好的着色器数据子包也直接沿用。
  - 实测（M4 Mac，时之笛，2026-11-11，运行时有改动）：约 9 分钟，42,891 个翻译、全部着色器库和 21,330 条管线都复用；没有这些核对之前同样的更新要 17 分钟，完整打包约 40 分钟。
  - 只有更新程序提示时才需要完整运行 `build_app.py`：翻译链、翻译器、spirv-cross 或深度模式变了，游戏文件变了，代码改写规则变了；另外，2026 年 11 月 11 日以前准备着色器的时之笛 App 需要完整打包一次。
  - 日志里写着复用了多少：`translations: N kept for this runtime's requests, M made again`、`seed files: …, K read again`、`… sources and … libraries read`，以及管线存档的 `按这个运行时核对，X 条和存档里的不一样`。
  - 大金刚和 Pokopia 用 `build_app.py` 重新打包，打包时同样复用翻译和管线。
- **退回更新前的版本**（新版本玩起来有问题时）：
  - 先退出游戏，用 `git reflog` 找到 `git pull` 之前所在的提交（`pull` 那一行下面的一行）。
  - 运行 `git checkout <那个提交>`（git 会提示 "detached HEAD"，这是正常的），再运行一次 `python3.14 update_runtime.py "<App 路径>"`；它要求时加上 `--seed`。
  - 然后运行 `git checkout main`。App 会一直用这个旧的运行时，直到你下次更新。
  - 只能退回到 685f940（2026-10-26）及以后的提交；存档和设置不会改动。
- **换游戏版本**：
  - 换上新的 `--update`，用同一个 `-o` 再跑一次 `build_app.py`，会替换掉原来的 App。
  - 新版本第一次启动前，App 会把存档完整备份到旁边的 `profile-before-<版本>`。以后可能要回到旧版本的话，留着这份备份。
- **模组**：`--mod "<模组文件夹>" --install-mod-romfs` 会把模组的代码补丁打进 App，并装好它的数据。详见 [mods.md](mods.md)、[merged-mod-pack.md](merged-mod-pack.md) 和 [translation-mods.md](translation-mods.md)。
- **只留一个桌面入口**（可选）：`python3.14 games/totk/tools/install_desktop_entry.py --app "<App 路径>"`。

## 老用户迁移到 MNG Launcher

适用于 2026 年 11 月 12 日以前用 `build_app.py` 或旧窗口（`Build Game.command`）打过游戏的玩家。下面的步骤不用重新打包，也不用游戏文件和密钥，除非某一步写明要。先关掉游戏。

**用 AI（或在终端里）迁移只要一条命令，而且必须最先做：** `python3 tools/mng.py migrate` 列出每个游戏 App 和 MNG Launcher 还差什么；`python3 tools/mng.py migrate --apply` 把它做完（把不符合规则的工作目录在同一个盘上改名到 `MacNativeGames-work/<游戏>`，每个游戏用一次极速更新记下来，生成或重新生成 MNG Launcher）。迁移完成指：每个 App 都记录了符合规则的工作目录（不在源码目录和放 App 的文件夹里、和 App 在同一个盘上），并且 MNG Launcher 是由这份源码生成的；下次本来就要完整打包的游戏（大金刚、Pokopia、换版本的）不卡这一步。迁移完成后才打开 MNG Launcher：在那之前 `python3 tools/mng.py open` 会拒绝。下面是手动做的同样步骤。

1. **更新代码。** 在 `MacNativeGames` 文件夹里运行 `git pull`（ZIP 下载的就重新下载）。`Build Game.command` 已经删掉，它的几页都搬进了 MNG Launcher。
2. **Python。** 已经装了 Python 3.14 的照旧用。新 Mac 只要装 <https://www.python.org/downloads/macos/> 的安装包；不再需要 Homebrew（opus、libvpx、dav1d、XcodeGen、CMake 等编译工具由打包自己下载固定版本）。
3. **生成 MNG Launcher：** `python3 tools/builder_app/make_app.py`，然后打开 `~/Applications/MNG Launcher.app`。它运行的是这个文件夹里的代码，以后 `git pull` 就够了；只有窗口本身有改动时才要重新生成。
4. **你的游戏会自动列出来**：第一页的卡片就是它们，带各自的图片（macOS 知道的所有王国之泪、旷野之息、时之笛、大金刚、Pokopia 的 App）。点卡片，按钮写着它能用的更新，比如**极速更新**：不用游戏文件。
5. **把每个游戏的工作目录放到规定的位置**（规则见英文版 [install.md 的 Folders 一节](install.md#folders)）：放 App 的文件夹旁边的 `MacNativeGames-work/<游戏>`，比如 App 在 `~/Applications`，就是 `~/MacNativeGames-work/totk`。打开**存储**页，选游戏，看它的工作目录：

   | 页面上写着 | 怎么做 |
   |---|---|
   | 已记录，而且已经是 `…/MacNativeGames-work/<游戏>` | 不用动 |
   | 已记录，但在别的地方（游戏文件旁边的 `TOTK_build`、`BotW_build`，或你自己选的文件夹） | 照现在这样也能用。要按规则放：点**移动…**选 `MacNativeGames-work/<游戏>`（同一个盘上瞬间完成），再点**现在更新**，App 就改记新位置 |
   | App 没有记录（2026 年 10 月 31 日以前打的） | 把它打包时用的文件夹（一般是 App 或游戏文件旁边的 `TOTK_build`）**移动…**到 `MacNativeGames-work/<游戏>`，再点**现在更新** |
   | 在一个没接上的盘上 | 接上那个盘再打开这一页，或者重新打包（见下一条） |

   App 不记录的另一份旧工作目录，之后可以删掉；**存储**页会显示 App 实际用的是哪一个。
6. **更新提示“要完整打包”时**（换了游戏版本、工作目录没了；大金刚和 Pokopia 一直如此）：在第一步拖入游戏文件、第二步选密钥，点**重新打包**。它会打进原来那个 App，保留存档和 App 记录的工作目录，并复用这个 App 里已经核对过的游戏数据。
7. **旧窗口保存的东西都还在**，各页直接读原来的位置：模组（`<游戏数据目录>/mod-library`，**模组**页；如果提示模组和新 App 对不上，点一次**应用模组**）、手柄选择（**按键**页）、开局选项（**设置**页）、存档备份（`~/Documents/MacNativeGames-Backups`，**存档**页）。只有旧窗口记住的文件路径（`Builder/settings.json`）不再读取：需要完整打包时，重新拖一次文件就行。
8. **可选的清理。** 只为这个项目装的 Homebrew 包可以卸掉（`brew uninstall opus libvpx dav1d ffmpeg llvm xcodegen cmake ninja`，前提是你别的东西不用它们）。在 App 还没有图标时建的程序坞或桌面快捷方式可能一直是空白图标：删掉重建即可。

帮你的 AI 可以替你做完这些：install-zelda skill 会读每个 App 的 `MNGWorkFolder`，运行 `update_runtime.py "<App>" --plan`，并用 `open -a "MNG Launcher" "<文件>" "<prod.keys>"` 打开 Launcher。

## 打包报错

处理步骤：
1. 读 `ERROR:` 那一行，以及它点名的日志（`<工作目录>/logs/…`）。
2. 按下表解决原因。
3. 原样再跑同一条命令。

| 报错里写着 | 解决办法 |
|---|---|
| `needs macOS 26 or later` | 本仓库不支持，见[需要准备](#需要准备)里的 macOS 15 fork。 |
| `clang++ finds no C++ standard library headers` | 装了 Xcode：`sudo xcode-select -s /Applications/Xcode.app/Contents/Developer`；没装：`sudo rm -rf /Library/Developer/CommandLineTools && xcode-select --install`。 |
| `Python 3.14 not found`、`git not found` | 装上它点名的东西。 |
| `[deps] … failed`（opus、libvpx、dav1d、XcodeGen 等） | 这些由打包自己从固定版本源码做出来；看它点名的日志（`~/Library/Caches/MacNativeGames/deps/logs` 下）。下载失败的，再打一次包。 |
| `NSP files do not include system shared fonts`、`needs --firmware or --shared-fonts` | 加上 `--firmware` 或 `--shared-fonts`。 |
| `SHA-256 … is not the expected …`、`ExeFS … is missing; not the supported version` | 先 `git pull`；再确认本体是 1.0.0、更新是支持的版本；都对的话，重新导出游戏。 |
| `unsupported update module hashes` | 这个更新版本不支持。 |
| `the base game must be an uncompressed XCI or NSP` | 先解压 NSZ / XCZ 本体（`nsz -D`）。 |
| `… add-on content (DLC), not the game`、`pass it as …`、`use the files of one game` | 有文件放错了参数，报错里写了该放在哪个。 |
| `the game file holds [...]: expected Tears of the Kingdom … or Breath of the Wild` | 这是别的游戏：按它自己的 `games/<名字>/README.md` 打包。 |
| `needs about N GB free` | 腾出空间，或用 `--work` 和 `-o` 放到有空间的 APFS 卷上。 |
| `BKTR verification failed`、合并更新时出错 | 先 `git pull`。还不行的话，你的密钥可能比更新旧：在新固件上重新导出 `prod.keys`。 |
| `make failed`、`xcrun failed` | 看它点名的日志；通常是开发工具不完整。 |
| iOS 签名或安装出错 | 在 Xcode › Settings › Accounts 登录 Apple ID，设备解锁并信任这台 Mac，打开开发者模式，再装一次。 |

还是不行，就开一个 [issue](https://github.com/ly05010419/MacNativeGames/issues)，附上：
- 你用的命令（路径可以缩写）
- Mac 型号和 macOS 版本
- `git log -1 --oneline` 的输出
- `ERROR:` 那一行，以及它点名的日志的结尾

不要附上密钥、游戏文件、固件或打包出的 App。

## 文件位置

以下是王国之泪的路径；旷野之息把 `TOTK` 换成 `BotW`、`totk` 换成 `botw`。

- 存档：`~/Library/Application Support/MacNativeGames/TOTK/profile`
- 着色器缓存：`~/Library/Application Support/MacNativeGames/TOTK/shader-cache`
- 公共清单里还没有的着色器：`~/Library/Application Support/MacNativeGames/TOTK/shader-list-contribution.txt`。游戏启动前会问要不要发送，见[公共着色器清单](shader-list.md)。
- 模组：`~/Library/Application Support/MacNativeGames/TOTK/mods/<v0|v121|v143>`，见 [mods.md](mods.md)。
- 日志：默认关闭。
  - Mac：`defaults write org.macnativegames.totk TOTK_DEBUG_LOG -bool YES`，下次启动起写到 `~/Library/Logs/MacNativeGames/totk.log`。
  - iPhone：在“设置 › TOTK › 記錄日誌”打开，写到 App 的 `Documents/log.txt`。

## 已知问题

**超出本页范围的需求也欢迎提。** 这里列出的版本和游戏，只是我们有条件测过的。需要别的版本或别的游戏时，AI 助理应该尽力把它做出来。

- 王国之泪：本体 1.0.0，可加 1.2.1 或 1.4.3 更新。旷野之息：1.6.0 和 1.9.0。其他版本、压缩格式的本体和个性化票据不支持。
- 没有联网功能，没有 amiibo。
- 第一次遇到新着色器时可能卡顿；本机缓存和[公共着色器清单](shader-list.md)会减少卡顿。
