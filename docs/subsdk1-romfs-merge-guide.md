# subsdk1 + RomFS mod in practice: a merged (TKMM) pack on the Mac port

一条 mod 从「合并产物」走到「游戏里看得见」的完整流程与踩坑记录，用王国之泪的
「大师模式 + 联机」合并包（Tkmm 输出的 `exefs/` + `romfs/`）实测跑通后整理。

**What this document is.** A field report of installing a mod-manager's merged output (an `exefs/`
patch plus a full `romfs/`) into the Mac app: how the content splits across four delivery paths, the
IPS → pchtxt arithmetic, the overlay manifest rules, two collisions with the build's own 2K overlay,
a zstd frame-header trap that looked like a game crash, a Resource Size Table repair, and the exact
criteria that prove a mod is actually live. It contains no game content, no keys and no third-party
mod code: hashes, patch bytes and file names of the mod are left out on purpose, and the numbers are
only what was measured locally. Sections marked *local* describe a private build tree, not this
repository. See [Player mods](mods.md) for the supported path; this file is the "why it failed" companion.

---

## 0. 先分清四类目录，别混

| 目录 | 作用 | 谁读它 |
|---|---|---|
| mod 管理器的输出（`exefs/` + `romfs/`） | 只是**输入** | 游戏不读 |
| `~/Library/Application Support/MacNativeGames/TOTK/mods/<version>/` | 运行时的 RomFS 覆盖层，加 `input.txt`、截图 | 运行时读 |
| 打包好的 `*.app/Contents/Resources/totk/`（`build/`、`extracted/<version>/`、`rewritten/<version>/`） | App 自己的载荷 | 运行时读 |
| 源码树 `games/totk/` | 重新构建时才用 | 只有构建用 |

**铁律**：改管理器输出目录本身对游戏没有任何影响。游戏只读 `mods/<version>/manifest.tsv` 和 App 包里的文件。

`extracted/<version>/romfs/romfs.bin` 是游戏数据分区（RomFS）的完整解密镜像，十几 GB 一个文件，
全部资源平铺在里面；旁边 `index.json` 给人和工具按路径查偏移/大小，`runtime-index.bin` 才是运行时真正
加载的紧凑索引。构建链读源码树里那份，运行时读 App 包内那份，互不往来。

---

## 1. 一个 mod 的内容要拆成四条落地路径

| 内容 | 落地位置 | 加载开关 |
|---|---|---|
| 额外代码模块（`subsdk1` 这类 NSO） | ——（本仓库不加载，见 §2） | —— |
| 主模块的 exefs 补丁 | 构建时 `--mod` 并进 `main`；或运行时 `TOTK_PCHTXT=<file>` | 见 §3 |
| 普通 RomFS 资源 | `mods/<version>/**` + `mods/<version>/manifest.tsv` | 启动器设 `TOTK_ROMFS_MODS` |
| 和构建自身覆盖层**撞路径**的资源 | `extracted/<version>/romfs-patches/`、`romfs-patches-out/<宽>x<高>/` | `TOTK_ROMFS_PATCHES` / `TOTK_ROMFS_PATCHES_OUT` |

> **同一个 RomFS 路径只能由一个覆盖层提供**，两处都有 → 启动直接停止。所以第四类不是"排除掉就完事"，
> 得做**内容合并**（§5）。

---

## 2. 额外代码模块：本仓库跑不了，别指望它

**游戏本体里没有 `subsdk1`**：`extracted/<version>/exefs/` 只有 `rtld / main / main.npdm / subsdk0 / sdk`，
`subsdk1` 完全由 mod 提供，是一个**注入**进来的模块。

[docs/mods.md](mods.md) 说得很明确：App 只跑构建时改写好的 `main`、`sdk`、`subsdk0`，mod 自带的额外
NSO 模块不会被加载，构建会明确提示这一点。要让这类模块跑起来，等于给 App 加一个模块加载器
（改写该模块 + 补它调用的 SVC / HLE 接口 + 扩模块表），**这是本仓库之外的改动**，本文不覆盖。
所以这条线上的 mod 只有「主模块补丁 + RomFS 数据」两部分会生效，依赖那个模块的代码改动不会。

判断"包里的模块是不是我要的那个"时，**别指望 md5 相等**：mod 的原始 NSO、构建产物 `rewritten/<version>/*.patched.bin`、
以及日志里 `mapped <module> size=…` 报的镜像大小（含 `.bss`、按页对齐）三者互不相等。可用的判据是三条：

1. 包内存在对应的 `rewritten/<version>/<module>.patched.bin`，且 `extracted/<version>/exefs/` 里没有基线同名模块
   → 说明这个模块只可能来自 mod；
2. 运行日志里 `mapped <module> size=… symbols=…` 的大小与 mod 模块的镜像一致；
3. **功能证据**（最硬的一条，前两条只是必要条件）：mod 自己的画面变化真的出现了。

大小对不上说明包里是旧模块，走 `scripts/prepare_modules.py` 重新生成镜像，**不要**手改 `*.patched.bin`。

---

## 3. exefs IPS → pchtxt：这一步最容易白干

mod 目录的 `exefs/` 里除了模块，常有一个几十字节的 IPS，**文件名就是它目标版本的 build-id**
（管理器约定 `exefs/<build id>.ips`）。它改的是 `main`，RomFS 覆盖层完全碰不到，必须走代码补丁通道。

**换算规则（逐字节核对过）**

```
IPS32 记录 = 4 字节大端 offset + 2 字节大端 length + length 字节数据
pchtxt 行  = (offset - 0x100) 的 8 位十六进制 + 空格 + 数据字节原序的十六进制
```

一个 pchtxt 长这样（占位值，**不要照抄数字**）：

```
@nsobid-<main 的 build id，40 个十六进制>
@enabled
@flag offset_shift 0x100
<hex main 地址> <hex 字节>
@stop
```

- **`@flag offset_shift 0x100` 是必填项**，缺了或写别的 shift 值直接拒绝启动
  （以前缺了只是静默偏 0x100，补丁打在错误的位置上——不报错、不崩溃，只是没生效，最难查的那类）。
  它的含义是「作者已经把 0x100 字节的 NSO 头减掉了」，运行时不会再减，所以写进去的必须是
  **main 地址 = IPS offset − 0x100**。`@flag print_values` 会被接受并忽略。
- `@nsobid-` 后面的 build-id 从 `extracted/<version>/exefs/main` 偏移 **0x40** 的 20 字节现读；
  每个游戏版本都不同，必须现读现填。
- 运行时日志判据：`[totk] TOTK_PCHTXT <file>: N patches, M bytes in main (build id …)`。

**两条通道，选一条：**

| 通道 | 时机 | 用法 |
|---|---|---|
| 构建时合并（推荐，仓库文档的主线） | 打包 | `python3 build_app.py <游戏文件> -k <keys> --mod <mod 目录>`，`scripts/mod_exefs.py` 自动做 0x100 换算、检查补丁是否落在 text/rodata/data、是否撞 App 自己的补丁位（60 帧、分辨率、超宽屏、布料） |
| 运行时 `TOTK_PCHTXT` | 启动 | Mac only，用来试社区补丁；改一次不用重构建 |

**绝对不要直接改 `rewritten/<version>/main.patched.bin`。** `runtime/loader.c` 会拿镜像的 SHA-256 和
构建时生成的摘要比对，不一致就 `FAIL: image SHA256 differs from verified rewrite`（踩过：手改二进制 →
启动即 FAIL）。正确做法就是上面那两条通道之一。

---

## 4. RomFS 迁移：平铺 + 生成 manifest

```bash
D="$HOME/Library/Application Support/MacNativeGames/TOTK/mods/<version>"
# 1) 平铺：mod 的 romfs 内容直接放 mods/<version> 下（不要多一层 romfs/）
rsync -a --delete "<mod 目录>/romfs/" "$D/"
# 2) 撞路径的文件按 §5 处理（合并，不是排除）
# 3) 生成 manifest（必须带 --app 或 --romfs + --version，用来算"原版 sha256"）
python3 games/totk/scripts/make_mod_manifest.py "$D" \
  --romfs extracted/<version>/romfs --version <version>      # 或 --app "<绝对路径>/xxx.app"
```

- `--romfs` 指向任何**同时有 `romfs.bin` 和 `runtime-index.bin`** 的目录；`--app` 从包里取
  `Contents/Resources/totk/extracted/<version>/romfs`。两者互斥。
- **先跑 `--check` 再写**：`--check` 一个字都不改，只报告清单会是什么、哪些路径和构建覆盖层冲突，
  是校验"磁盘内容"和"清单"是否一致的最便宜手段。
- manifest 在**覆盖层根目录**，不是放在被覆盖文件旁边。格式：

```
# path\tsize\tsha256\toriginal_sha256
AS/<Actor>.root.asb.zs	33431	<新文件sha>	<原版sha>
AI/<新增>.ainb	1711	<sha>	-      ← "-" 表示游戏里没有的新文件
```

- 日志判据：`RomFS overlay: … checked, N files` 且没有 `FAIL` = 清单和磁盘完全一致；
  `N files added to the RomFS` 对应清单里 `original_sha256` 为 `-` 的行数。

> **不要对着已经装好的覆盖层重跑第 1 步的 `rsync --delete`。** 一份活了很久的覆盖层里通常有两类东西是
> mod 目录里没有的：手工并回的 RESTBL（§7），以及工具自动合并留下的 `<name>.mod-original`（下次重跑要靠它）。
> 整体重拷会把这些一起删掉。

> 想关掉 mod：把 `manifest.tsv` 改名，不要删 mod 文件。

---

## 5. 和构建自身覆盖层撞路径：合并，不是二选一

为了真 2K 输出，构建自己覆盖了 `System/HeapSize/Product.Nin_NX_NVN.HeapDef.byml.zs`，另外所有版本还会覆盖
`UI/LayoutArchive/Common.Product.<n>.Nin_NX_NVN.blarc.zs`（快捷菜单时序）。这两条路径恰好是 mod 常改的：

- **`blarc.zs`（UI 归档）**：现在**由工具自动合并**（issue #73）。工具把构建那处改动（quick-menu 的
  `capture_frame` 1.0 → 0.0）打到 **mod 的归档**上，检查方式和 `make_romfs_patches.py` 一致：mod 不能已经改过
  构建要改的成员，且结果必须正好带上构建的改动。合并后的归档顶替 mod 的文件，mod 原件留在旁边叫
  `<name>.mod-original`，manifest 那一行的 `original_sha256` 写的是构建补丁版的 SHA，于是 `runtime/romfs.c`
  允许它替换构建的覆盖层而不是拒绝启动。
- **`HeapDef.byml.zs`（堆表）**：**不会**自动合并。用 `--mod` 构建时，2K 覆盖层是直接**建在 mod 的堆表之上**的，
  所以正常路径是带 `--mod` 重打包；不带 `--mod` 的构建会退回零售堆表。`make_mod_manifest.py` 对这条路径只
  **报告**构建的 2K 覆盖层是否已经带着 mod 要的堆大小（`already carries these heap sizes`），或者提示要带 `--mod` 构建。
  本包当年是在没有这条通道时手工合并的：端口版抬高 DTA/Graphics、mod 版把 **RSDB 从 9728 抬到 40000 KiB**，
  合并版 = 端口的 DTA/Graphics + mod 的 RSDB。**只留端口版的后果是 actor 资源表饥饿 → 启动就 SIGSEGV**，
  这就是"排除掉撞路径文件"这个直觉在这里会害人的地方。
  HeapDef 格式：BYML v7 小端（magic `YB`），`Heaps` → 堆名 → `Size`(KiB) + 可选 `Parent` 索引；
  节点 = `[u8 type][u24 count]`，载荷从 `节点+4` 开始；容器里存的是**绝对偏移**，要解引用再递归。
- **其它任何撞路径仍然直接拒绝启动**（两个覆盖层不能同时替换同一个文件）。

> 以后用管理器合并别的 mod：先 `make_mod_manifest.py --check`，看它对撞路径说什么。只有 UI 归档那类会自动处理，
> 堆表要靠带 `--mod` 的构建。

---

## 6. zstd 帧头：闪退的真凶（务必记住这条）

**游戏的 RomFS 解压器要求帧头声明内容大小。** `FHD`（帧头第 5 字节）的 bit7-6 是 `fcs_flag`，必须非 0：

```
FHD = 0x60 / 0x80  → 有内容大小，能用
FHD = 0x00         → 没有 → 解压失败 → 目标指针为空 → SIGSEGV
```

`zstd` 命令行从**标准输入**压缩（`zstd -c`，某些 `--no-check` 组合也一样）会写出 `FHD=0x00` 的帧。
当时的症状：合并包一进游戏就死在 `ENTER nnMain` 之后，`str x27,[x4],#8` 且 `x4=0`——
排查了半天 UI 归档、app pool，最后发现真凶是**压缩帧头**，和 mod 内容毫无关系。

正确做法：用带内容大小的帧重新压缩，并当场断言。

```python
import zstandard
out = zstandard.ZstdCompressor(level=19).compress(data)
assert (out[4] >> 6) & 3, f'frame header {out[4]:#x} declares no content size'
```

**帧参数还要和原版一致**（窗口大小、有无内容校验和）。用 `zstd -l -v <文件>` 和原版逐字段对比再写回去。
任何自己生成的 `.zs` 都要过这一关。

---

## 7. RESTBL（ResourceSizeTable）槽位补齐

mod 管理器有个系统性缺陷：**它只往记录数组里追加行，一个 Actor 槽都不写。** 于是合并表的槽位数
比参与合并的单体表还少，被丢掉的那些槽位（大型 BOSS、多人相关的资源）在游戏里就是尺寸未知。

布局（全小端）：

```
0  'RESTBL'   6  u32 version   10 u32 署名宽 W   14 u32 nrec   18 u32 npath
22 nrec × 8 字节记录 (u32 key = 路径 CRC32, u32 size)，按 key 升序
   npath 个定长槽：名字(ASCII, NUL 补零到 W) + u32 size
```

本包的实测（槽数）：原版 32（只有打包资源，**没有** `Actor/…`、`Component/…` 这些逻辑路径）、
大师模式单体 223、联机单体 57（含 25 个多人相关槽）、**管理器合并产物 192**（比大师单体还少 31，
多人槽一个都没有）、并回两张单体表之后 **248**。

要点：

- 槽名是**逻辑资源路径**（运行时从 `Pack/Actor/*.pack` 里取），在 RomFS 索引里查不到属正常，
  **别据此判断"文件不存在"**。
- **槽尾顺序无所谓**——没有任何一张能跑的表是按名字排序的；但**记录数组必须按 key 升序**（游戏二分查找）。
- 补法：按单体表做并集，两级追加（先补缺失的那批，再补另一批），**值逐字节照抄单体表**。
  本包补回 56 个值，其中 20 个能被合并表自己的记录数组印证，**0 冲突**。
- 自检四件套：解析→重编码能还原原字节（自反）、记录数组 key 有序、槽数对得上、zstd 帧头合规。
- 改之前先备份原表和 `manifest.tsv` 里那一行，改完**只重签那一行**。

---

## 8. 打包与签名的几个坑

- App 是 ad-hoc 签名、无 quarantine 属性 → 改 `Contents/Resources/` 里的资源不影响启动；换二进制要重签
  （内层二进制和整个 bundle 都要）。`codesign -f -s -` 会**重写 Mach-O**，签完再和未签名的源二进制 `cmp`
  一定不相等，属正常。`spctl --assess` 对本地构建一律说 *rejected*，别理它；看
  `codesign --verify --deep --strict`。
- **合并了上游之后，光换二进制不够**：启动器 `Contents/MacOS/TOTK` 里编译进了 `DMSH_CHAIN`
  （着色器翻译链版本号），它决定 `TOTK_SHADER_CACHE` 指向哪个 `shader-cache/rt-<链版本>` 目录。
  旧启动器配新二进制会在读档瞬间 FATAL，日志写着「翻译链版本 totk-native-vN-unverified 与本宿主期望的 … 不符」。
  这时要一起做三件事：**重建启动器**、重出 `build/pso_manifest.tsv` / `build/pso_title.tsv`、重出 shader seed
  （`scripts/prepare_shader_seed.py`）。别去动二进制本体。
- **永远不要覆盖官方 App**，另建一份带后缀的，方便 A/B。
- 改了源码树才需要重新构建；构建产物在 `build/`、`rewritten/`，只换资源不用构建。

---

## 9. 怎么算验证通过（判据清单）

**启动日志按顺序出现**（行内容按你的 mod 变化，关键是"有没有"和"数字对不对"）：

```
[totk] mapped subsdk0 size=… symbols=…
[totk] TOTK_PCHTXT <file>: N patches, M bytes in main (build id …)
[totk] output resolution patch: … (HeapDef overlay)
[totk] RomFS overlay: extracted/<version>/romfs-patches checked, …
[totk] RomFS overlay: …/mods/<version> checked, N files, … MB hashed in …
[totk] RomFS overlay: K files added to the RomFS
[totk-boot] … first presented frame
```

**画面判据**：标题页从原版配色变成 mod 自己的主题色，就是 RomFS + 代码补丁都生效了；没变 = 没加载。

**驱动游戏**（不需要鼠标键盘，走运行时命令通道，`runtime/input.c`；通道文件由启动器设为
`<数据目录>/input.txt`，也可以用 `TOTK_INPUT_FILE` 指到别处）：

```bash
D="$HOME/Library/Application Support/MacNativeGames/TOTK"
printf 'key 38 120\n' >> "$D/input.txt"     # A 键确认（继续→选档→载入）
echo "shot $D/shot.png" >> "$D/input.txt"   # 截图
printf 'close\n' >> "$D/input.txt"          # 干净退出
```

语法：`key <macOS键码> <毫秒>`，**毫秒必须在 1..5000**——`key 0 0` 会走到
`runtime_fail("…invalid diagnostic input command")` **直接杀掉正在跑的游戏**（解析器写死了 `ms > 0 && ms <= 5000`）。
常用键码：38=A，0/1/2/13 = A/S/D/W，123/124/125/126 = 方向键。

> **最大的一个坑**：`runtime/input.c` 里 **WASD 才是左摇杆（移动），方向键是右摇杆（镜头）**
> （`state.lx = keys[D]-keys[A]`、`state.ly = keys[W]-keys[S]`、`rx/ry` 用方向键）。
> 只按方向键的"压测"人物一步都不会走，镜头转到平台底下看着像穿模——那次误判就是这么来的。
> 要测移动和碰撞必须按 `0=A 1=S 2=D 13=W`。

**长跑**：一段只转镜头、一段真走路+战斗，结束看进程信号数为 0 且日志无 `FAIL:`。

**两个"不是 bug"**：

1. 偶发启动失败 `reserve guest stack, heap and alias regions below 2^39`：macOS 把大块系统保留区
   随机落在低地址空洞里，**重开一次即可**，与 mod 无关。
2. `shot` 截图在 1920×1080 游戏区外面有一圈品红：那是内部 2560×1440 的 present 纹理，窗口里看不到。

**日志轮转**：启动器每次 `open` 会把 `totk.log` 转成 `totk.previous.log`。所以**别在启动前记录行号、
再按偏移读**（会读不到新行而误判"卡住"），启动后直接 grep 整个文件。

---

## 10. 复用清单

本仓库自带（`games/totk/scripts/`）：

| 脚本 | 干什么 |
|---|---|
| `make_mod_manifest.py <mod> --romfs … --check` | 不写盘，先报告清单会是什么、哪些撞路径 |
| `mod_exefs.py apply --exefs … --mod … --version …` | 把 mod 的 IPS/pchtxt 并进 `main`（自动 0x100 换算 + 保留点检查） |
| `mod_exefs.py sites --version <version>` | 列出 mod 补丁不许碰的保留点 |
| `make_romfs_patches.py` / `make_output_res_overlay.py` | 构建自身的覆盖层与 2K 堆表 |
| `prepare_shader_seed.py`、`make build/pso_manifest.tsv` | 合并上游后必须重出的 seed 与 pso 清单 |

下面这些是本机的分析脚本，**没有入库**，这里只留下它们做的事，方便你照着力重写：

| 名称（未入库） | 干什么 |
|---|---|
| `heapdef.py` | BYML v7 小端读取器，用来 diff HeapDef |
| `merge_port_patches.py` | HeapDef / UI 归档与构建 2K 补丁的内容合并 |
| `restbl_union_live.py` / `restbl_install.sh` | 补回缺失的 RESTBL 槽 + 四件套自检；装表并重签 manifest 那一行 |
| `restbl_check.py` | 只读诊断：live 表和两张单体表逐槽对比 |
| `zs_info.py` / `check_zs.py` | 看 zstd 帧头参数（查 `FHD`） |
| `app_soak.sh` / `app_walk_live.sh` / `title_shot.py` | 真 App 长跑（镜头 / 走路+战斗）、标题页颜色分类 |

---

## 11. 以后合并别的 mod：哪些能照抄，哪些必须重算

**能照抄（与具体 mod 无关）**

1. §1 的四条落地路径分层法，和 §2「额外 NSO 模块本仓库不加载」这条边界。
2. §3 的 IPS32 → pchtxt 换算，**外加 `@flag offset_shift 0x100`**（缺了就拒绝启动）；或者干脆 `--mod` 让它算。
3. §4 的 `--check` 先跑 → 写 manifest 的顺序，和"manifest 放覆盖层根目录"这条规矩。
4. §6 的 zstd 帧头铁律：任何 `.zs` 都必须声明内容大小（`FHD` bit7-6 ≠ 0）。
5. §9 的判据清单和 `input.txt` 驱动方式（`key` / `shot` / `close`，WASD 才是走路）。

**必须按新 mod 重算，别抄数字**

| 项 | 为什么 |
|---|---|
| build-id | 每个游戏版本不同，从 `extracted/<version>/exefs/main` 偏移 0x40 现读；写错现在会直接 `runtime_fail` |
| 撞路径清单 | 取决于 mod 改了哪些文件。先 `--check`：UI 归档那类会自动合并（§5），堆表要带 `--mod` 构建 |
| RESTBL 槽 | 「只加记录、不加槽」这个缺陷对**任何**多 mod 合并都存在。拿 live 表和每张单体表逐槽比，缺多少补多少——**248 是本包的数字，不是常量** |
| 额外 NSO 模块 | 本仓库不加载（§2）。别把"mod 没生效"当成 RomFS 装错了 |
| 存档 | 换了 mod 组合就换了 Actor/RESTBL 表，旧存档可能读不出新内容；**测试要用自己的存档**才测得出问题 |

---

## 12. 一句话流程

> **分层**（模块 / 主模块补丁 / RomFS 资源 / 撞路径资源）→ **pchtxt 必须带 `@flag offset_shift 0x100`**
> （或直接带 `--mod` 构建）→ **撞路径合并而不是排除** → **压缩帧必须带内容大小** → **RESTBL 槽位并回** →
> **manifest 重签** → **看日志 + 看标题颜色 + 用 WASD 真走一遍**。
> 合并了上游还要多做三件：**重建启动器、重出 pso 清单、重出 shader seed**。
