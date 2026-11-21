// NVN 贴图存储布局（Tegra X1 块线性），见 nvn_tex.c。
// NVN texture storage layout (Tegra X1 block-linear), see nvn_tex.c.
#pragma once
#include <stdint.h>

typedef struct {
    int target, format, flags, samples;   // NVNtextureTarget / NVNformat / NVNtextureFlags / 采样数（0 或 1 = 单采样） | NVNtextureTarget / NVNformat / NVNtextureFlags / sample count (0 or 1 = single-sampled)
    uint32_t width, height, depth, levels, stride;
    signed int pkg_block_h:8; unsigned int swizzle_r:3,swizzle_g:3,swizzle_b:3,swizzle_a:3,swizzle_pad:12;                      // 预排布贴图布局参数第 0 字节（块高 log2）+ 1；0 = 按自动算法 | Byte 0 of the packaged texture layout parameters (log2 of block height) + 1; 0 = use the automatic algorithm
} nvn_texdesc;

typedef struct {
    uint64_t size, layer_size;            // 总存储字节、每层字节 | Total storage bytes, bytes per layer
    uint32_t align, layers, bpb;          // 对齐、层数、每块字节 | Alignment, layer count, bytes per block
    unsigned tileH, tileD;                // 块高 / 块深（log2 GOB 数） | Block height / block depth (log2 of the GOB count)
} nvn_texlayout;

void nvn_tex_layout(const nvn_texdesc *d, nvn_texlayout *o);
uint64_t nvn_tex_level_offset(const nvn_texdesc *d, unsigned level);
int nvn_fmt_bpb(int format, int *bw, int *bh);   // 每块字节（0 = 未知格式），块宽高 | Bytes per block (0 = unknown format), block width and height
// 第 level 级实际使用的块高 / 块深（log2 GOB 数）：贴图级块高按该级行数（块行）收缩，解排布逐级用它
// Block height / block depth actually used by level `level` (log2 of the GOB count): the per-level block height shrinks with that level's row count (block rows), and deswizzling uses it level by level
void nvn_tex_level_tiles(const nvn_texdesc *d, unsigned level, unsigned *tileH, unsigned *tileD);

/* LINEAR_RENDER_TARGET (0x40) uses pitch storage even without LINEAR (0x10).
   TOTK's CPU-loaded water height/mask textures use that flag alone. */
static inline int nvn_tex_is_linear(const nvn_texdesc *d) { return (d->flags & 0x50) != 0; }
static inline uint32_t nvn_tex_linear_stride(const nvn_texdesc *d) {
    if (d->stride) return d->stride;
    int bw, bh; int bpb = nvn_fmt_bpb(d->format, &bw, &bh);
    uint32_t align = (d->flags & 0x40) ? 128 : 32;
    return (((d->width + bw - 1) / bw) * bpb + align - 1) & ~(align - 1);
}
