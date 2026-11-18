# dm/gfx/tex — 贴图格式映射、块线性解排布、BC 解压

English: [README.md](README.md)

图形层里和贴图数据打交道的部分，供回放层 `../nvn/nvn_mtl.m` 调用。做三件事：

1. 把 NVN 的贴图格式号换成 Metal 的像素格式；
2. 把 Switch 的「块线性」排布解成普通的逐行排布；
3. 在不支持 BC 压缩贴图的显卡上把 BC 贴图解压。

块线性（block linear）是 Tegra X1 显卡存贴图的方式：数据不是一行接一行，而是按 64 字节 × 8 行的小块（GOB）交错排列，再按「块高」「块深」把若干 GOB 叠成更大的块。游戏打包好的贴图文件里就是这种排布，上传给 Metal 之前必须先解开。

## 文件

| 文件 | 内容 |
|---|---|
| `nvn_fmt_mtl.h`、`nvn_fmt_mtl.c` | `nvn_fmt_to_mtl()`：NVN 格式（0x01–0x9C）→ Metal 像素格式 + 分量重映射。返回 1 表示精确对应，2 表示替代（精度、位序或读法不同，需要转换），0 表示 Metal 没有对应格式。每个格式的处理办法写在表内注释里。纯 C，不依赖 Metal 头文件 |
| `nvn_deswizzle.h`、`nvn_deswizzle.c` | `nvn_deswizzle()`：把某一级、某一层从块线性解到线性；`nvn_swizzle()`：反向，单测往返用；`nvn_bl_offset()`：单点的块线性地址。纯 C |
| `nvn_deswizzle_gpu.h`、`nvn_deswizzle_gpu.m` | 同一个解排布算法的 Metal 计算着色器版本，回放层上传打包贴图时用；初始化失败或遇到不支持的格式时回放层退回 CPU 版 |
| `nvn_bcdec.h`、`nvn_bcdec.c` | `nvn_bc_unpacked()` / `nvn_bc_decode()`：BC 贴图在 CPU 上解压成不压缩格式，解码用 `dm/third_party/bcdec` |
| `tests/` | 单元测试 |

各级、各层在存储里的偏移，以及每一级实际使用的块高、块深，一律取自 `../nvn/nvn_tex.c`（`nvn_tex_layout`、`nvn_tex_level_offset`、`nvn_tex_level_tiles`），这里只做级内的寻址与搬运。游戏的打包贴图自带布局参数，其第 0 字节是第 0 级块高的 log2，有它时以它为准（`nvn_texdesc` 的 `pkg_block_h` 字段）。

## 不支持 BC 的设备

一部分较老的苹果显卡不支持 BC 压缩贴图（由 `MTLDevice` 的 `supportsBCTextureCompression` 判断）。在这些设备上：

- 游戏贴图可以用可选的补充包事先离线换成 ASTC 格式（`dm/tools/astc_pack.py`，依赖见 `docs/DEPENDENCIES.zh-CN.md` 的「可选」一节）；
- 剩下的 BC 贴图由回放层建成解压后的格式，上传时解压，CPU 上的解压走本目录的 `nvn_bcdec.c`。

环境变量 `DM_TEST_NO_BC=1` 可以在任何机器上强制走这条路径。

## 测试

```sh
make -C dm/gfx/tex/tests samples   # 取真数据样本（见下）
make -C dm/gfx/tex/tests run       # 跑 t_fmt_mtl 与 t_tex
```

- **`t_fmt_mtl`**：对 0x01–0x9C 每个 NVN 格式取映射，凡映射不为 0 的，都在本机的 Metal 设备上真建一张贴图，必须全部建成；另核名字带 sRGB 的 NVN 格式都落到 Metal 的 sRGB 格式。不需要游戏文件。
- **`t_tex`**：解排布的测试，分几组：

  | 组 | 判据 |
  |---|---|
  | A 往返 | 往各级、各层写入随机数据再解出，逐字节相等。覆盖 2D、2D 数组、3D，多种每块字节数与非 2 的幂尺寸 |
  | B 地址公式 | `nvn_bl_offset()` 的结果与按公开排布规则手算的地址相等 |
  | E 手工 3D | 在手算地址放标记，解排布后核对落点，并核块高、块深与存储尺寸 |
  | C 真数据 | 同一张贴图的 Switch 版（块线性）解排布后与 PC 版（线性）比：同分辨率的逐字节相等；Switch 版降了分辨率的是重新压缩过的，改比解码后的 PSNR（峰值信噪比，衡量两张图有多像）不低于 22 dB。同时核文件里的逐级偏移与 `nvn_tex_level_offset` 一致 |
  | D 负对照 | 故意把块高错一级再解，结果必须不再逐字节相等、PSNR 低于 22 dB，证明 C 组的判据能分辨错误的排布 |

  A 往返只能证明「一一对应、不越界」，证明不了排布公式是对的；真正兜底的是 B 组的手算地址与 C 组同分辨率样本的逐字节相等。

**关于真数据样本**：C、D 两组需要同一张贴图的 Switch 版与 PC 版各一份。`tests/fetch_samples.py` 从你工作区里的游戏文件取样，Switch 版读 `$DM_ROOT/data/switch/3.3.94012/Update/RomFS/data`，PC 版读 `$DM_ROOT/data/assets-src/raw`，输出到 `$DM_ROOT/build/cache/dm/tex_samples`。没有样本时 `t_tex` 的真数据项算失败，不会跳过；A、B、E 三组不需要游戏文件。样本不进仓库。

## 贴图文件头里与排布有关的字段

`t_tex.c` 解析游戏的 `.texture` 文件时用到的事实：

- 文件以 `<DE(` 开头，Switch 版与 PC 版是同一种容器。
- 经过 Switch 转换的文件，偏移 0x14 起的四个字节是 `00 00 00 07`（PC 版为 0），表示像素区是块线性排布；偏移 0x13 是第 0 级块高的 log2，与游戏运行时传给 `SetPackagedTextureLayout` 的布局参数第 0 字节含义相同。
- 没有这个标记的文件是线性存放的，与 PC 版逐字节相同。

## 局限

- `nvn_deswizzle()` 不支持多重采样贴图、带 LINEAR 标志的贴图（本来就是线性的）和 BUFFER 目标，遇到返回 -1。
- 立方体与立方体数组按分层贴图处理，没有单独用真数据核对。
- 3D 贴图只有往返与手算地址的核对，没有真数据样本。
- 16 位打包格式（565 / 5551 / 4444）的分量位序是按格式名对应的，没有用真数据核过，在格式表里标为「替代」。
- 真数据样本集小，覆盖 RGBA8、BC1、BC3、BC4 四种格式。
