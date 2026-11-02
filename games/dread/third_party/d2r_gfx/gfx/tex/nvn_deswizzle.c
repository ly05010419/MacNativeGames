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

static int move(const nvn_texdesc *d, unsigned level, unsigned layer, uint8_t *bl, uint8_t *lin, size_t pitch, int to_linear) {
    if (nvn_tex_is_linear(d)) {
        if (level || layer || d->samples > 1 || (d->target != 0 && d->target != 1 && d->target != 7)) return -1;
        int bw, bh, bpb = nvn_fmt_bpb(d->format, &bw, &bh);
        if (!bpb) return -1;
        size_t row = (size_t)((d->width + bw - 1) / bw) * bpb;
        uint32_t rows = d->target == 0 ? 1 : (d->height + bh - 1) / bh, stride = nvn_tex_linear_stride(d);
        if (pitch < row || stride < row) return -1;
        for (uint32_t y = 0; y < rows; y++) {
            if (to_linear) memcpy(lin + y * pitch, bl + (size_t)y * stride, row);
            else memcpy(bl + (size_t)y * stride, lin + y * pitch, row);
        }
        return 0;
    }
    uint32_t row_bytes, rows, slices, bpb; uint64_t base; unsigned th, td;
    if (nvn_deswizzle_geom(d, level, layer, &row_bytes, &rows, &slices, &base, &th, &td, &bpb)) return -1;
    if (pitch < row_bytes) return -1;
    bl += base;
    for (uint32_t z = 0; z < slices; z++)
        for (uint32_t y = 0; y < rows; y++) {
            uint8_t *row = lin + ((size_t)z * rows + y) * pitch;
            for (uint32_t x = 0; x < row_bytes; x += 16) {             // 16 字节一段：GOB 内 16 字节连续，且整除所有块宽 | 16-byte segments: 16 bytes are contiguous within a GOB, and 16 divides all block widths
                uint32_t n = row_bytes - x < 16 ? row_bytes - x : 16;
                uint8_t *p = bl + nvn_bl_offset(x, y, z, row_bytes, rows, th, td);
                if (to_linear) memcpy(row + x, p, n); else memcpy(p, row + x, n);
            }
        }
    return 0;
}

/* A region of one level / layer (slice z of a 3D level) written straight into the storage: hb block rows of wb bytes,
   starting at byte x0 of block row y0, from src with src_row bytes per block row. Block-linear storage is addressed per
   run inside a 16-byte GOB segment (nvn_bl_offset), so only the region's bytes are touched. -1 when unsupported. */
int nvn_swizzle_region(const nvn_texdesc *d, unsigned level, unsigned layer, uint32_t z, uint32_t x0, uint32_t y0,
                       uint32_t wb, uint32_t hb, const void *src, size_t src_row, void *storage) {
    const uint8_t *s = src; uint8_t *st = storage;
    if (nvn_tex_is_linear(d)) {
        if (level || layer || z || d->samples > 1 || (d->target != 0 && d->target != 1 && d->target != 7)) return -1;
        uint32_t stride = nvn_tex_linear_stride(d);
        for (uint32_t y = 0; y < hb; y++) memcpy(st + (size_t)(y0 + y) * stride + x0, s + y * src_row, wb);
        return 0;
    }
    uint32_t row_bytes, rows, slices, bpb; uint64_t base; unsigned th, td;
    if (nvn_deswizzle_geom(d, level, layer, &row_bytes, &rows, &slices, &base, &th, &td, &bpb)) return -1;
    if (x0 + wb > row_bytes || y0 + hb > rows || z >= slices) return -1;
    st += base;
    for (uint32_t y = 0; y < hb; y++)
        for (uint32_t x = x0; x < x0 + wb;) {
            uint32_t n = 16 - x % 16; if (n > x0 + wb - x) n = x0 + wb - x;   // a run never leaves its 16-byte segment
            memcpy(st + nvn_bl_offset(x, y0 + y, z, row_bytes, rows, th, td), s + y * src_row + (x - x0), n);
            x += n;
        }
    return 0;
}

int nvn_deswizzle(const nvn_texdesc *d, unsigned level, unsigned layer, const void *src, void *dst, size_t dst_pitch) {
    return move(d, level, layer, (uint8_t *)src, (uint8_t *)dst, dst_pitch, 1);
}
int nvn_swizzle(const nvn_texdesc *d, unsigned level, unsigned layer, const void *src, size_t src_pitch, void *dst) {
    return move(d, level, layer, (uint8_t *)dst, (uint8_t *)src, src_pitch, 0);
}
