# 安装

[← 返回 README](../README.md) · [English](install.md)

本页只讲一件事：用**你自己的**游戏文件，在 Mac、iPhone 或 iPad 上打包出 App。一次打包 20–40 分钟，大部分时间是等待。操作与设置见 [controls.zh-CN.md](controls.zh-CN.md)，测过什么见 [test-records.zh-CN.md](test-records.zh-CN.md)。AI 助理：同样的步骤写成了清单，在[安装 skill](../.claude/skills/install-zelda/SKILL.md) 里。

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
| `0100F2C0115B6800` | 王国之泪更新 **1.1.2、1.2.1 或 1.4.3**，NSP 或 NSZ（可选） | `--update` |
| `01007EF00011E000` | 旷野之息本体，或 **1.6.0** 卡带镜像（本体、更新、DLC 在同一个 XCI 里） | 第一个参数 |
| `01007EF00011E800` | 旷野之息更新 **1.6.0 或 1.9.0**，NSP 或 NSZ | `--update` |
| `01007EF00011F001`、`01007EF00011F002` | 旷野之息的两个 DLC 包，NSP（都可选） | `--dlc`，每个包一次 |

- 王国之泪推荐 **1.4.3**：测得最多，也是唯一在 iPhone / iPad 上测过的版本。不需要中间版本的更新。不加 `--update` 打出来的是 1.0.0。
- 本体不能是压缩格式。NSZ / XCZ 本体先解压（`nsz -D`）。
- 卡带镜像里自带了更新也可以，版本仍由 `--update` 决定。

**`prod.keys`**：从同一台 Switch 导出（Lockpick_RCM）。在旧固件上导出的密钥，可能解不开较新的更新。

**系统字体**：本体是 **NSP** 时需要，**旷野之息**一律需要。二选一：
- `--firmware <文件夹>`：你导出的固件，一个装着 `.nca` 文件的文件夹（压缩包先解开），或已安装固件的 `registered` 文件夹。
- `--shared-fonts <文件夹>`：以前打包王国之泪时提取出的六个 `.bfttf` 文件，在 `TOTK_build/totk/extracted/firmware/shared-fonts`。

王国之泪的 XCI 自带字体，不用另给。

**磁盘空间**（工作目录所在的卷）：

| | Mac | iPhone / iPad |
|---|---|---|
| 只有本体 | 25 GB | 45 GB |
| 加更新 | 40 GB | 60 GB |

工作目录默认建在游戏文件旁边（`TOTK_build`；旷野之息是 `BotW_build`）。工作目录和 App 放在**同一个 APFS 卷**上：17 GB 的游戏数据是克隆过去的，不额外占空间。游戏文件在 exFAT / FAT 盘或快满的盘上时，用 `--work <文件夹>` 和 `-o <App>` 放到有空间的 APFS 盘上。

## 打包 App

**1. 装工具**（只需一次，要联网）：

```sh
xcode-select --install
# 没有 Homebrew 的话，按 https://brew.sh 安装（必须装在 /opt/homebrew）
brew install opus libvpx python@3.14
```

也可以不用 Homebrew 的 Python，改用 [uv](https://docs.astral.sh/uv/)：把 `python3.14 build_app.py …` 换成 `uv run --python 3.14 build_app.py …`。

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
python3.14 build_app.py "<旷野之息 1.6.0.xci>" -k "<prod.keys>" --shared-fonts "<…/TOTK_build/totk/extracted/firmware/shared-fonts>" -o "/Applications/Breath of the Wild.app"

# 1.9.0（或 1.6.0），用 NSP：本体加更新，DLC 包都可选
python3.14 build_app.py "<旷野之息本体.nsp>" -k "<prod.keys>" --update "<1.9.0 更新.nsp>" --dlc "<DLC 1.nsp>" --dlc "<DLC 2.nsp>" --firmware "<固件文件夹>" -o "/Applications/Breath of the Wild.app"
```

用单独的更新包时，App 里只有你用 `--dlc` 给的 DLC 包。卡带镜像自带 DLC，这时不要加 `--dlc`。

## iPhone 和 iPad

**需要：**
- iOS / iPadOS 26，iPhone 14 或更新的机型。
- Mac 上装好完整的 **Xcode 26** 或更高版本（含 iOS 平台），再 `brew install xcodegen cmake ninja`。
- 在 Xcode › Settings › Accounts 里登录你的 Apple ID（免费账号也行）。
- 设备上打开开发者模式。

**1. 打包 IPA。** 在上面的 Mac 命令后加 `--platform ios --ios-game-data documents`，`-o` 写成 `.ipa` 文件：

```sh
python3.14 build_app.py "<本体 1.0.0.xci>" -k "<prod.keys>" --update "<1.4.3 更新.nsp>" --platform ios --ios-game-data documents -o "<文件夹>/Tears of the Kingdom.ipa"
```

- 第一次打 iOS 包时，还会下载并编译设备用的库。
- `--ios-game-data documents` 不把 17 GB 的游戏镜像放进 IPA，以后重装只要几分钟。

**2. 安装**，设备用线连着 Mac：

```sh
python3.14 games/totk/ios/tools/install_device.py "<文件夹>/Tears of the Kingdom.ipa" --device "<访达里显示的设备名>" --game-data "<工作目录>/totk/extracted/<版本>/romfs/romfs.bin"
```

- `<版本>` 填那里实际有的文件夹（1.4.3 是 `v143`；用 `ls "<工作目录>/totk/extracted"` 看）。第一次安装会把 17 GB 的镜像拷过去一次。
- 免费 Apple ID 的签名 7 天有效，付费账号一年。到期后再跑同一条命令续签，存档会保留。
- 设备提示时，到“设置 › 通用 › VPN 与设备管理”里信任开发者。

**3. 第一次启动：** iPhone 14 上先校验游戏数据约 15 秒，再导入着色器约 3 分钟，然后出标题。之后约 8 秒出标题。

手机上内部按 960×540 渲染，放大到 1280×720，最高 60 帧。旷野之息 1.6.0 也按同样的方法打包安装。

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

加了 `--update` 时，把 `v0` 换成对应版本的文件夹（`v112`、`v121` 或 `v143`）。

## 更新 App

- **本仓库有新版本**（`git pull` 之后）：
  - 先退出游戏，再运行 `python3.14 update_runtime.py "<App 路径>"`，约一分钟更新完，不需要游戏文件和密钥。
  - 它提示启动着色器不再匹配时，加上 `--seed`。
- **换游戏版本**：
  - 换上新的 `--update`，用同一个 `-o` 再跑一次 `build_app.py`，会替换掉原来的 App。
  - 新版本第一次启动前，App 会把存档完整备份到旁边的 `profile-before-<版本>`。以后可能要回到旧版本的话，留着这份备份。
- **模组**：`--mod "<模组文件夹>" --install-mod-romfs` 会把模组的代码补丁打进 App，并装好它的数据。详见 [mods.md](mods.md)、[merged-mod-pack.md](merged-mod-pack.md) 和 [translation-mods.md](translation-mods.md)。
- **只留一个桌面入口**（可选）：`python3.14 games/totk/tools/install_desktop_entry.py --app "<App 路径>"`。

## 打包报错

处理步骤：
1. 读 `ERROR:` 那一行，以及它点名的日志（`<工作目录>/logs/…`）。
2. 按下表解决原因。
3. 原样再跑同一条命令。

| 报错里写着 | 解决办法 |
|---|---|
| `needs macOS 26 or later` | 本仓库不支持，见[需要准备](#需要准备)里的 macOS 15 fork。 |
| `clang++ finds no C++ standard library headers` | 装了 Xcode：`sudo xcode-select -s /Applications/Xcode.app/Contents/Developer`；没装：`sudo rm -rf /Library/Developer/CommandLineTools && xcode-select --install`。 |
| `Homebrew packages missing`、`Python 3.14 not found`、`git not found` | 装上它点名的东西。 |
| `NSP files do not include system shared fonts`、`needs --firmware or --shared-fonts` | 加上 `--firmware` 或 `--shared-fonts`。 |
| `SHA-256 … is not the expected …`、`ExeFS … is missing; not the supported version` | 先 `git pull`；再确认本体是 1.0.0、更新是支持的版本；都对的话，重新导出游戏。 |
| `unsupported update module hashes` | 这个更新版本不支持。 |
| `the base game must be an uncompressed XCI or NSP` | 先解压 NSZ / XCZ 本体（`nsz -D`）。 |
| `… add-on content (DLC), not the game`、`pass it as …`、`use the files of one game` | 有文件放错了参数，报错里写了该放在哪个。 |
| `the game file holds [...]: expected Tears of the Kingdom … or Breath of the Wild` | 这是别的游戏：按它自己的 `games/<名字>/README.md` 打包。 |
| `needs about N GB free` | 腾出空间，或用 `--work` 和 `-o` 放到有空间的 APFS 卷上。 |
| `BKTR verification failed`、合并更新时出错 | 先 `git pull`。还不行的话，你的密钥可能比更新旧：在新固件上重新导出 `prod.keys`。 |
| `make failed`、`xcrun failed` | 看它点名的日志；通常是开发工具不完整，或 Homebrew 不在 `/opt/homebrew`。 |
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
- 模组：`~/Library/Application Support/MacNativeGames/TOTK/mods/<v0|v112|v121|v143>`，见 [mods.md](mods.md)。
- 日志：默认关闭。
  - Mac：`defaults write org.macnativegames.totk TOTK_DEBUG_LOG -bool YES`，下次启动起写到 `~/Library/Logs/MacNativeGames/totk.log`。
  - iPhone：在“设置 › TOTK › 記錄日誌”打开，写到 App 的 `Documents/log.txt`。

## 已知问题

**超出本页范围的需求也欢迎提。** 这里列出的版本和游戏，只是我们有条件测过的。需要别的版本或别的游戏时，AI 助理应该尽力把它做出来。

- 王国之泪：本体 1.0.0，可加 1.1.2、1.2.1 或 1.4.3 更新。旷野之息：1.6.0 和 1.9.0。其他版本、压缩格式的本体和个性化票据不支持。
- 没有联网功能，没有 amiibo。
- 第一次遇到新着色器时可能卡顿；本机缓存和[公共着色器清单](shader-list.md)会减少卡顿。
