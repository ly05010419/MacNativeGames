//     (x%64/32)·256 + (y%8/2)·64 + (x%32/16)·32 + (y%2)·16 + x%16
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
    uint64_t block = 512ull * bh * bd;                                  // bytes in one block
    return (z / bd) * gobs_x * blocks_y * block                        // which block layer (along depth)
         + (y / (8 * bh)) * gobs_x * block                             // which block row
         + (x / 64) * block                                            // which block column
         + ((z % bd) * bh + (y % (8 * bh)) / 8) * 512ull               // which GOB within the block
         + gob_off(x, y);
}

static uint32_t mip(uint32_t s, unsigned l) { s >>= l; return s ? s : 1; }

// Geometry of this level: width-in-blocks byte count, block row count, slice count, and where this level and layer starts in storage; returns -1 for unsupported textures
int nvn_deswizzle_geom(const nvn_texdesc *d, unsigned level, unsigned layer,
                       uint32_t *row_bytes, uint32_t *rows, uint32_t *slices, uint64_t *base, unsigned *th, unsigned *td, uint32_t *bpb) {
    if (d->target == 5 || d->target == 6 || d->target == 10 || (d->flags & 0x10) || d->samples > 1) return -1;   // MS / BUFFER / LINEAR
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
    uint32_t row_bytes, rows, slices, bpb; uint64_t base; unsigned th, td;
    if (nvn_deswizzle_geom(d, level, layer, &row_bytes, &rows, &slices, &base, &th, &td, &bpb)) return -1;
    if (pitch < row_bytes) return -1;
    bl += base; uint32_t lb = nvn_tex_level_gobs(d, level) * 64;   // the row as laid out (padded in a sparse texture's tiled levels)
    if (lb < row_bytes) lb = row_bytes;
    for (uint32_t z = 0; z < slices; z++)
        for (uint32_t y = 0; y < rows; y++) {
            uint8_t *row = lin + ((size_t)z * rows + y) * pitch;
            for (uint32_t x = 0; x < row_bytes; x += 16) {             // 16-byte segments: 16 bytes are contiguous within a GOB, and 16 divides all block widths
                uint32_t n = row_bytes - x < 16 ? row_bytes - x : 16;
                uint8_t *p = bl + nvn_bl_offset(x, y, z, lb, rows, th, td);
                if (to_linear) memcpy(row + x, p, n); else memcpy(p, row + x, n);
            }
        }
    return 0;
}

int nvn_deswizzle(const nvn_texdesc *d, unsigned level, unsigned layer, const void *src, void *dst, size_t dst_pitch) {
    return move(d, level, layer, (uint8_t *)src, (uint8_t *)dst, dst_pitch, 1);
}
int nvn_swizzle(const nvn_texdesc *d, unsigned level, unsigned layer, const void *src, size_t src_pitch, void *dst) {
    return move(d, level, layer, (uint8_t *)dst, (uint8_t *)src, src_pitch, 0);
}
