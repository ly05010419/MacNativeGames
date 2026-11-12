# 安装 时之笛

[← README](../README.md) · [English](install.md)

本页只讲一件事：把**你自己**导出的 Switch 2 版《塞尔达传说 时之笛》做成 Mac 上的 App。打包大约半小时，大部分时间在等；第一次还要加上复制 26 GB 游戏数据的时间。

基本规则：只用你自己的主机和你自己的游戏。不会替你下载任何东西，本仓库里也没有任何游戏内容。不要为了通过检查去改 `games/oot/assets/expected-v131072.json` 或脚本：报错信息会说清哪里不对。做出来的 App 含有游戏，只供你自己使用。

目录：[需要什么](#需要什么) · [打包](#打包) · [第一次启动](#第一次启动) · [更新 App](#更新-app) · [打包停下时](#打包停下时) · [文件在哪](#文件在哪)

## 需要什么

**一台 Mac**：Apple 芯片，**macOS 26** 或更新（图形用 Metal 4）。建议 16 GB 或更多内存。

**你导出的游戏**，带 **v131072** 更新，title ID `0400486024DEC000`，两种形式都行：
- 一个 `.nup` 文件；或者
- 一个导出目录，里面有 `base/` 和 `updates/131072/`（各自带 `exefs/` 和 `romfs/`）。

打包会按 `expected-v131072.json` 核对游戏代码和数据，别的版本会被拒绝。不需要 `prod.keys`：导出的文件已经解密。

**系统字体**：本仓库打包《王国之泪》或《旷野之息》时解出的 6 个 `.bfttf` 文件和它们的 `manifest.json`，在 `TOTK_build/totk/extracted/firmware/shared-fonts`（或 `BotW_build/botw/...`）。用 `--shared-fonts` 指定这个目录。时之笛的打包没有 `--firmware` 选项。第一次打包后工作目录会保留字体，以后再打包不用再给 `--shared-fonts`。

**工具**（装一次，需要联网）：

```sh
xcode-select --install
# Python 3.14：从 https://www.python.org/downloads/macos/ 下载安装包（或 brew install python@3.14）
# opus、libvpx、dav1d：打包时从固定版本的源码自己编；不用 llvm 和 ffmpeg
```

另外要装完整的 **Xcode** 和它的 Metal 工具链（Xcode › 设置 › 组件 › Metal Toolchain，或 `xcodebuild -downloadComponent MetalToolchain`）：打包要用苹果的离线 Metal 编译器编译游戏的着色器，命令行工具里没有它。打包还会在工作目录自己的环境里装三个 Python 包（`lz4`、`capstone`、`zbic`）。

**磁盘空间**（工作目录所在的盘）：大约 40 GB。

工作目录是导出文件旁边的 `OcarinaOfTime_build`。让它和 App 在**同一个 APFS 盘**上，游戏数据就是克隆而不是复制。要放到别的 APFS 盘，用 `--work <目录>` 和 `-o <App>`。

## 打包

**1. 本仓库**：`git clone https://github.com/ly05010419/MacNativeGames.git`（或下载 ZIP），在 `MacNativeGames` 目录打开终端。已经有的话先 `git pull`。

**2. 打包。** 每个路径都加引号。现在就用 `-o` 定好 App 的最终位置；不给 `-o` 时 App 写在导出文件旁边。

```sh
# 用 .nup 文件
python3.14 build_app.py "<Ocarina of Time.nup>" --shared-fonts "<…/TOTK_build/totk/extracted/firmware/shared-fonts>" -o "/Applications/Ocarina of Time.app"

# 用导出目录（base/ 和 updates/131072/）
python3.14 build_app.py "<导出目录>" --shared-fonts "<…/shared-fonts>" -o "/Applications/Ocarina of Time.app"
```

如果 `/Applications` 和工作目录不在同一个盘，就选工作目录所在盘上的一个目录。

可选：`--mac-game-data library` 把 26 GB 的游戏数据放在 App 外面，在 `~/Library/Application Support/MacNativeGames/OcarinaOfTime/GameData`（或用 `--mac-game-data-folder <目录>` 指定别处）。这样 App 只有 5–6 GB，以后再打包装得更快。

**3. 等它跑完。** 在 M4 上，游戏文件已经导入过的完整打包用了约 31 分钟；第一次打包还要复制并核对 26 GB 游戏数据。日志会写出每一步的名字：

| 步骤 | 在做什么 |
|---|---|
| `import-original-nup` / `import-original-dump` | 把游戏文件复制进工作目录并核对 |
| `verify-original-modules`、`rewrite-…`、`verify-rewrite-…` | 把游戏代码改写成能在 Apple 芯片上运行，并核对结果 |
| `native-runtime` | 编译运行时 |
| `prepare-shader-seed` | 把游戏的全部着色器（约 42,800 个）翻译成 Metal |
| `compile-shader-libraries` | 为 GPU 编译它们 |
| `Preparing … pipelines` | 按着色器清单建管线存档 |
| `Built <App 路径>` | 完成 |

- 长时间没有输出是正常的，不要中断。
- 被打断了就**原样**再运行同一条命令：已经做完的步骤会复用。
- 打包前先退出游戏；不要在同一个工作目录上同时跑两个打包。

## 第一次启动

在访达里打开 App。游戏和主机上一样，先播标题视频和开场。按数字键 **1** 打开设置面板（分辨率、放大方式、锁帧、补帧，见 [README](../README.md#settings)）。第一次到一个新地方，如果有还没被玩家记录过的管线，现场建管线时会短暂卡一下。

## 更新 App

- **仓库有新版本**（`git pull` 之后）：先退出游戏，再运行

  ```sh
  python3.14 update_runtime.py "<App>" --work "<工作目录>"
  ```

  它重新编译运行时、重建管线存档，复用已有的着色器，几分钟就好；工作目录默认是 App 旁边的 `OcarinaOfTime_build`。如果它说着色器需要完整打包，就再运行一次[打包](#打包)里的 `build_app.py` 命令，工作目录里其他东西都会复用。
- 两种方式都不会改动你的存档和设置。

## 打包停下时

看 `ERROR:` 那一行和 `<工作目录>/logs/` 里对应的日志，排除原因后原样再运行同一条命令。

| 报错里写着 | 怎么办 |
|---|---|
| `unsupported or damaged original ExeFS`、`RomFS is not update 131072: missing … / different …` | 不是 v131072 更新，或导出文件损坏：重新导出游戏。RomFS 按文件逐个核对，报错会列出是哪些文件；清单里没有的多余文件不算错（同一个 131072 的两份导出可能差几个文件）。 |
| `… compute pipeline(s) could not be built on this Mac (…)` | 这台 Mac 的 Metal 编译器编不过报错里列出的着色器（Apple M5 + macOS 26.6.2 上见过，#2070）。有系统更新就先更新再打包；仍然失败的话，开 issue 附上着色器键和报错指出的 `logs/compute-pipelines.log` 里的那几行。 |
| `use the complete original update 131072 in the dump/NUP`、`leave out --update, --dlc and --mod` | 只给 `.nup` 或导出目录，它已经包含更新。 |
| `pass --shared-fonts with the six verified firmware fonts` | 加上 `--shared-fonts`（见[需要什么](#需要什么)）。 |
| `no Metal compiler (Xcode -> Settings -> Components: Metal Toolchain)` | 安装完整的 Xcode 和它的 Metal 工具链。 |
| `… must remain outside the source checkout` | 把 `--work` 和 `-o` 放到 `MacNativeGames` 目录外面。 |
| `unfinished App staging directory exists; left untouched` | 上次打包在写 App 时停下了：删掉 App 旁边的 `.Ocarina of Time.app.ready` 目录，再打包。 |
| `this title currently uses its original rendering options` | 去掉 `--native-4k`、`--native-5k` 和 `--vr`。 |
| `game did not finish its normal close; App left untouched` | 退出游戏后再打包。 |

还是不行？开一个 [issue](https://github.com/ly05010419/MacNativeGames/issues)，写上命令（路径可以缩短）、Mac 型号、macOS 版本、`git log -1 --oneline`、`ERROR:` 那一行和它指出的日志的结尾。不要附上游戏文件、字体或 App。

## 文件在哪

- 存档：`~/Library/Application Support/MacNativeGames/OcarinaOfTime/profile`
- 设置：同一目录下的 `graphics-*.txt`
- 着色器缓存：`~/Library/Application Support/MacNativeGames/OcarinaOfTime/shader-cache`
- 公共清单里还没有的着色器和管线：`~/Library/Application Support/MacNativeGames/OcarinaOfTime/shader-list-contribution.txt`
- 游戏数据（只在用了 `--mac-game-data library` 时）：`~/Library/Application Support/MacNativeGames/OcarinaOfTime/GameData`
- 日志：默认关。`defaults write org.macnativegames.oot TOTK_DEBUG_LOG -bool YES` 从下次启动起打开，写到 `~/Library/Logs/MacNativeGames/ocarina.log`。
