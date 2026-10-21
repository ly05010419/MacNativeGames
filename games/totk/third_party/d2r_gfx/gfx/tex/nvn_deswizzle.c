// Tegra X1 块线性排布（公开描述见 deko3d / Ryujinx BlockLinearLayout）：
//   GOB = 64 字节 × 8 行 = 512 字节；GOB 内 (x 字节, y 行) 的偏移 =
//     (x%64/32)·256 + (y%8/2)·64 + (x%32/16)·32 + (y%2)·16 + x%16
//   块 = 1 个 GOB 宽 × 2^tileH 个 GOB 高 × 2^tileD 个 GOB 深，块内先排 y 再排 z；块按 x → y → z 行优先铺满本级。
// 级 / 层起点与本级实际块高块深取自 nvn_tex.c，这里只做级内寻址与搬运。
// Tegra X1 block-linear layout (public descriptions: deko3d / Ryujinx BlockLinearLayout):
//   GOB = 64 bytes × 8 rows = 512 bytes; offset of (x byte, y row) within a GOB =
//     (x%64/32)·256 + (y%8/2)·64 + (x%32/16)·32 + (y%2)·16 + x%16
//   block = 1 GOB wide × 2^tileH GOBs high × 2^tileD GOBs deep, ordered y first then z within a block; blocks fill the level in x → y → z row-major order.
// Level / layer start offsets and the actual block height and block depth of this level come from nvn_tex.c; here we only do intra-level addressing and copying.
#include "nvn_deswizzle.h"
#include <string.h>

static uint32_t gob_off(uint32_t x, uint32_t y) {
    return (x % 64 / 32) * 256 + (y % 8 / 2) * 64 + (x % 32 / 16) * 32 + (y % 2) * 16 + x % 16;
}

uint64_t nvn_bl_offset(uint32_t x, uint32_t y, uint32_t z, uint32_t row_bytes, uint32_t rows, unsigned th, unsigned td) {
    uint32_t bh = 1u << th, bd = 1u << td;
    uint64_t gobs_x = (row_bytes + 63) / 64, blocks_y = (rows + 8 * bh - 1) / (8 * bh);
    uint64_t block = 512ull * bh * bd;                                  // 一个块的字节 | bytes in one block
    return (z / bd) * gobs_x * blocks_y * block                        // 第几层块（深度方向） | which block layer (along depth)
         + (y / (8 * bh)) * gobs_x * block                             // 第几行块 | which block row
         + (x / 64) * block                                            // 第几列块 | which block column
         + ((z % bd) * bh + (y % (8 * bh)) / 8) * 512ull               // 块内第几个 GOB | which GOB within the block
         + gob_off(x, y);
}

static uint32_t mip(uint32_t s, unsigned l) { s >>= l; return s ? s : 1; }

// 本级几何：块宽字节数、块行数、切片数，以及本级本层在存储里的起点；不支持的贴图返回 -1
// Geometry of this level: width-in-blocks byte count, block row count, slice count, and where this level and layer starts in storage; returns -1 for unsupported textures
int nvn_deswizzle_geom(const nvn_texdesc *d, unsigned level, unsigned layer,
                       uint32_t *row_bytes, uint32_t *rows, uint32_t *slices, uint64_t *base, unsigned *th, unsigned *td, uint32_t *bpb) {
    if (d->target == 5 || d->target == 6 || d->target == 10 || nvn_tex_is_linear(d) || d->samples > 1) return -1;   // MS / BUFFER / LINEAR
    int bw, bh; *bpb = (uint32_t)nvn_fmt_bpb(d->format, &bw, &bh);
    if (!*bpb || level >= (d->levels ? d->levels : 1)) return -1;
    nvn_texlayout L; nvn_tex_layout(d, &L);
    if (layer >= L.layers) return -1;
    int one_d = d->target == 0 || d->target == 3;
    uint32_t w = mip(d->width, level), h = one_d ? 1 : mip(d->height, level);
    *slices = d->target == 2 ? mip(d->depth ? d->depth : 1, level) : 1;
    *row_bytes = (w + bw - 1) / bw * *bpb; *rows = (h + bh - 1) / bh;
    *base = (uint64_t)layer * L.layer_size + nvn_tex_level_offset(d, level);
    nvn_tex_level_tiles(d, level, th, td);
    return 0;
}

/* The bytes of one region of a level / layer: x0 / w in bytes, y0 / h in block rows, z0 / nz slices (3D only; other
   targets take 0 / 1). Linear storage has no tiles: stride is its row pitch (0 for block-linear). */
typedef struct { uint32_t row_bytes, rows, slices, stride; uint64_t base; unsigned th, td; } region_geom;
static int geom(const nvn_texdesc *d, unsigned level, unsigned layer, region_geom *g) {
    if (nvn_tex_is_linear(d)) {
        if (level || layer || d->samples > 1 || (d->target != 0 && d->target != 1 && d->target != 7)) return -1;
        int bw, bh, bpb = nvn_fmt_bpb(d->format, &bw, &bh);
        if (!bpb) return -1;
        *g = (region_geom){(uint32_t)((d->width + bw - 1) / bw) * (uint32_t)bpb, d->target == 0 ? 1 : (d->height + bh - 1) / bh, 1, nvn_tex_linear_stride(d), 0, 0, 0};
        return g->stride < g->row_bytes ? -1 : 0;
    }
    uint32_t bpb; g->stride = 0;
    return nvn_deswizzle_geom(d, level, layer, &g->row_bytes, &g->rows, &g->slices, &g->base, &g->th, &g->td, &bpb);
}
static int move(const nvn_texdesc *d, const region_geom *g, uint8_t *bl, uint8_t *lin, size_t pitch, size_t img,
                uint32_t x0, uint32_t y0, uint32_t z0, uint32_t w, uint32_t h, uint32_t nz, int to_linear) {
    if (x0 + w > g->row_bytes || y0 + h > g->rows || z0 + nz > g->slices || pitch < w || (nz > 1 && img < pitch * h)) return -1;
    (void)d;
    bl += g->base;
    for (uint32_t z = 0; z < nz; z++)
        for (uint32_t y = 0; y < h; y++) {
            uint8_t *row = lin + (size_t)z * img + (size_t)y * pitch;
            if (g->stride) {   // linear storage: one run per row
                uint8_t *p = bl + (size_t)(y0 + y) * g->stride + x0;
                if (to_linear) memcpy(row, p, w); else memcpy(p, row, w);
                continue;
            }
            for (uint32_t x = 0; x < w;) {   // runs up to the next 16-byte boundary: 16 aligned bytes are contiguous within a GOB
                uint32_t ax = x0 + x, n = 16 - ax % 16; if (n > w - x) n = w - x;
                uint8_t *p = bl + nvn_bl_offset(ax, y0 + y, z0 + z, g->row_bytes, g->rows, g->th, g->td);
                if (to_linear) memcpy(row + x, p, n); else memcpy(p, row + x, n);
                x += n;
            }
        }
    return 0;
}
static int move_level(const nvn_texdesc *d, unsigned level, unsigned layer, uint8_t *bl, uint8_t *lin, size_t pitch, int to_linear) {
    region_geom g; if (geom(d, level, layer, &g) || pitch < g.row_bytes) return -1;
    return move(d, &g, bl, lin, pitch, pitch * g.rows, 0, 0, 0, g.row_bytes, g.rows, g.slices, to_linear);
}
int nvn_deswizzle_region(const nvn_texdesc *d, unsigned level, unsigned layer, const void *src, void *dst, size_t pitch, size_t img,
                         uint32_t x0, uint32_t y0, uint32_t z0, uint32_t w, uint32_t h, uint32_t nz) {
    region_geom g; if (geom(d, level, layer, &g)) return -1;
    return move(d, &g, (uint8_t *)src, (uint8_t *)dst, pitch, img, x0, y0, z0, w, h, nz, 1);
}
int nvn_swizzle_region(const nvn_texdesc *d, unsigned level, unsigned layer, const void *src, size_t pitch, size_t img, void *dst,
                       uint32_t x0, uint32_t y0, uint32_t z0, uint32_t w, uint32_t h, uint32_t nz) {
    region_geom g; if (geom(d, level, layer, &g)) return -1;
    return move(d, &g, (uint8_t *)dst, (uint8_t *)src, pitch, img, x0, y0, z0, w, h, nz, 0);
}
int nvn_region_span(const nvn_texdesc *d, unsigned level, unsigned layer, uint32_t y0, uint32_t h, uint32_t z0, uint32_t nz, uint64_t *lo, uint64_t *hi) {
    region_geom g; if (!h || !nz || geom(d, level, layer, &g) || y0 + h > g.rows || z0 + nz > g.slices) return -1;
    if (g.stride) { *lo = (uint64_t)y0 * g.stride; *hi = (uint64_t)(y0 + h - 1) * g.stride + g.row_bytes; return 0; }
    /* whole blocks: from the block holding (y0, z0) to the end of the block row holding (y0 + h - 1, z0 + nz - 1) */
    uint32_t by = 8u << g.th, bz = 1u << g.td, gobs_x = (g.row_bytes + 63) / 64;
    uint64_t block_row = (uint64_t)gobs_x * 512 * (by / 8) * bz;
    *lo = g.base + nvn_bl_offset(0, y0 / by * by, z0 / bz * bz, g.row_bytes, g.rows, g.th, g.td);
    *hi = g.base + nvn_bl_offset(0, (y0 + h - 1) / by * by, (z0 + nz - 1) / bz * bz, g.row_bytes, g.rows, g.th, g.td) + block_row;
    return 0;
}

int nvn_deswizzle(const nvn_texdesc *d, unsigned level, unsigned layer, const void *src, void *dst, size_t dst_pitch) {
    return move_level(d, level, layer, (uint8_t *)src, (uint8_t *)dst, dst_pitch, 1);
}
int nvn_swizzle(const nvn_texdesc *d, unsigned level, unsigned layer, const void *src, size_t src_pitch, void *dst) {
    return move_level(d, level, layer, (uint8_t *)dst, (uint8_t *)src, src_pitch, 0);
}
