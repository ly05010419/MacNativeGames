# 圣兽之王：macOS ARM64 原生运行

基于 MacNativeGames 的 Bayonetta 3 运行层建立独立副本。当前输入为
`010054B01AD92000` 的 **1.05（本体＋v262144 更新）**，两个 DLC 均可选，运行 `main` 和 `sdk`
两个 ARM64 模块。数字美术集属于独立应用，没有加入；尚未验证完整流程。

## 启动与存档

双击构建输出的 `圣兽之王.app`。App 引用
`$HOME/Games/Unicorn-build-1.05` 中的原生程序、重写模块和资源，请保留工作目录。
默认 App 也输出到该工作目录，每次启动的日志为其中的 `boot.log`。

| 键盘 | 游戏手柄 |
| --- | --- |
| J / K | A 确认 / B 返回 |
| I / U | X / Y |
| W A S D | 左摇杆 |
| T G F H | 方向键：上、下、左、右 |
| Return / Tab | + / − |
| Q / E | L / R |
| Option / Shift | ZL / ZR |
| 1 | 显示或隐藏性能与快捷键提示 |

## 从原始文件构建

仅支持上述亚洲版 1.05，无需 DLC 即可构建。不是通用模拟器，也不支持其他地区、
其他更新版本或赠品应用。游戏、更新、DLC 和密钥均由玩家自行提供，不随代码分发。
需要 Apple silicon、macOS 26 或更新版本、Command Line Tools/macOS 26 SDK、Homebrew。
依赖安装完成后，提取、改写、编译和打包均不访问网络。SPIRV-Cross 固定版本源码和
带 SHA-256 的翻译器均由本目录自带。打包结果引用工作目录，移动工作目录后应重新运行 `make app`。

在仓库根目录安装依赖，再进入本游戏目录。下面的提取命令要求新的输出目录；重新验证时另选
位于源码树以外的工作目录，不要覆盖正在使用的工作目录或存档。

```sh
brew install opus libvpx python@3.14 llvm@23
cd games/unicorn
WORK="$HOME/Games/Unicorn-build-1.05"
python3.14 -m venv "$WORK/venv"
PY="$WORK/venv/bin/python"
"$PY" -m pip install -r requirements.txt

BASE="/absolute/path/base.nsp"
UPDATE="/absolute/path/update-1.05.nsp"
KEYS="/absolute/path/prod.keys"
export DM_ROOT="$WORK"
export LLVM_BIN="$(brew --prefix llvm@23)/bin"

"$PY" scripts/extract_exefs.py "$UPDATE" --update --keys "$KEYS" --output "$WORK/extracted/v262144/exefs"
"$PY" scripts/merge_romfs.py "$BASE" "$UPDATE" --keys "$KEYS" --output "$WORK/extracted/v262144/romfs"
mkdir -p "$WORK/rewritten/v262144"
for MODULE in main sdk; do
  "$PY" third_party/d2r/patch_nso.py "$WORK/extracted/v262144/exefs/$MODULE" "$WORK/rewritten/v262144" "$MODULE" || exit 1
  "$PY" third_party/d2r/structck.py "$WORK/rewritten/v262144" "$MODULE" "$WORK/extracted/v262144/exefs/$MODULE" || exit 1
done
make prepare WORK="$WORK" PY="$PY"
make -j4 runtime shader-tools app WORK="$WORK"
shasum -a 256 Makefile requirements.txt scripts/*.py third_party/d2r/*.py assets/expected-v1.05.json > "$WORK/build-inputs.sha256"
```

可选 DLC：在 `make prepare` 前提取自己拥有的一个或两个包；没有 DLC 时跳过此命令。
输出目录必须是新的，运行层只报告已提取且通过校验的编号。

```sh
"$PY" scripts/extract_addons.py "/absolute/path/dlc-010054B01AD93001.nsp" \
  "/absolute/path/dlc-010054B01AD93002.nsp" --keys "$KEYS" --output "$WORK/extracted/v262144/aoc"
```

仅有一个 DLC 时，只传该包。更改安装的 DLC 后重新运行 `make prepare` 和构建命令。

NSZ 先转换为 NSP，在对应输入变量中使用转换后的路径：

```sh
mkdir -p "$WORK/containers"
"$WORK/venv/bin/nsz" -D --keys "$KEYS" -o "$WORK/containers" "/absolute/path/game-or-update-or-dlc.nsz"
```

本游戏提取器仍核验 NCA 文件系统/ExeFS 哈希和全部六层 RomFS IVFC；运行准备和加载阶段再核对固定产物哈希。
脚本不验证任天堂 RSA 签名，不能将这些完整性检查称作来源认证。DLC 还固定了 NSP 包哈希，重新封装过的包可能不被接受。
本机已验证的字体不是构建前提：本次游玩使用游戏自带字体。若游戏请求系统字体而工作目录中没有它们，运行层会明确报错；
可在 `make prepare` 前用 `scripts/extract_shared_fonts.py --keys "$KEYS" --firmware /path/to/unpacked-firmware --output "$WORK/extracted/firmware/shared-fonts"`
从自己的固件提取，不会下载字体。

已准备好数据时，只需重新执行 `make prepare` 和 `make runtime shader-tools app`。
`assets/expected-v1.05.json` 固定更新后的 ExeFS、重写模块、RomFS 和两个 DLC 的校验值。
准备脚本拒绝不匹配的数据；已创建但不完整的 DLC 目录同样报错，不会被当作未安装。

更新通过 `scripts/extract_exefs.py --update` 提取代码，`scripts/merge_romfs.py` 合并资源。
合并结果的六层 IVFC 全部通过校验，控制数据中的显示版本确认为 1.05。
两个 DLC 经 `scripts/extract_addons.py` 核验，编号为 1、2；其资源区都只有零字节占位文件。
游戏只导入 `ListAddOnContent` 查询编号，运行层验证 DLC 镜像后返回实际安装的编号；没有 DLC 时返回空列表，
由原游戏解锁本体中的内容。没有新增游戏未调用的资源挂载接口。

## 适配与验证

- 支持本体 SDK 请求的文件系统初始化、随机数服务及主模块 BSS 栈映射。
- SDK 弱 C++ 分配器符号服从主模块的强定义，避免分配与释放使用不同分配器。
- Opus 多流音频使用系统侧 libopus 实现；六声道编码、独立解码对照和错误输入检查通过。
- NVN 四边形、三角扇展开为 Metal 三角形；修复启动画面被跳过的问题。
- 客体 GPU 地址按 64 KiB 分配，并在录制时转换为 CPU 存储地址；修复新游戏的
  128 MiB 池因 CPU/GPU 对齐不同而耗尽的问题。没有扩大游戏的缓冲池。

1.05 的两个模块结构检查通过，main 的 3264 个改写点无语义偏差，50 个注入错误全部被检出。
2026-10-27 同步上游后的根目录源码测试共 422 项，7 项因依赖或平台条件跳过；设置面板的
8 个子用例在沙箱外复跑通过，其余在沙箱内通过。独立构建与各轮验证记录详见 [VALIDATION.md](VALIDATION.md)。
实际启动已确认标题、键盘确认与中文开场剧情；游戏已查询并收到两个 DLC 编号。
本轮已实际看到教程交战的 WIN 画面，并通过独立测试 profile 的手动保存、退出和新进程读档。
DLC 的后续纹章选择仍未验证；性能与温度的短时对照见 [VALIDATION.md](VALIDATION.md)。
`probe-addons` 已验证镜像哈希、列表分页与输出边界；损坏或缺失 DLC 的负向检查也已通过。

```sh
cd "$HOME/Games/Unicorn-build-1.05"
./build/unicorn-native rewritten/v262144 probe-addons
```

独立 GPU 探针与游戏共用单实例锁，运行探针前应先退出游戏。
旧 `probe-gfx-final.log` 记录了复制来的预测探针因缺少模拟 control 元数据而崩溃。
本轮修复后，1.05 的 CPU `probe-gfx` 通过，24/26/27 负对照仍拒绝错误行为；
这只验证对象和命令录制，不代表实际 GPU 画面或完整游戏通过。详见下面的验证记录。

本轮补充验证和已知问题见 [VALIDATION.md](VALIDATION.md)，来源与许可见
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

## 历史验证

开发早期 1.0.0 的结构、语义与运行时探针日志仍保存在本机 `UNICORN_build/`。
旧版 App 和 profile 与 1.05 各自独立。这些是历史产物，当前源码和构建说明仅支持 1.05。
