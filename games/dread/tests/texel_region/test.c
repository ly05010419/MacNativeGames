/* nvn_swizzle_region (WriteTexels into mirrored storage) against the whole-level path it replaces: deswizzle the level,
 * copy the region, swizzle it back. Random storage and regions over 2D, 3D and array textures, mip levels, RGBA8, R8
 * and BC1; the storage bytes must be identical. Synthetic data only. */
#include "nvn_tex.h"
#include "nvn_deswizzle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rng = 12345;
static uint32_t rnd(void) { rng = rng * 1103515245u + 12345u; return rng >> 8; }
static uint32_t mip(uint32_t s, unsigned l) { s >>= l; return s ? s : 1; }

int main(void) {
    struct { int target, format; uint32_t w, h, d, levels; } kinds[] = {
        {1, 0x25, 160, 160, 1, 5}, {1, 0x25, 40, 33, 1, 3}, {1, 0x01, 260, 260, 1, 6}, {1, 0x42, 128, 64, 1, 4},
        {2, 0x25, 32, 32, 16, 3}, {4, 0x25, 64, 64, 4, 2}, {1, 0x25, 1280, 720, 1, 1}, {1, 0x37, 512, 256, 1, 2}};
    unsigned cases = 0;
    for (size_t k = 0; k < sizeof kinds / sizeof *kinds; k++) {
        nvn_texdesc d; memset(&d, 0, sizeof d);
        d.target = kinds[k].target; d.format = kinds[k].format; d.width = kinds[k].w; d.height = kinds[k].h;
        d.depth = kinds[k].d; d.levels = kinds[k].levels;
        nvn_texlayout L; nvn_tex_layout(&d, &L);
        uint8_t *a = malloc(L.size), *b = malloc(L.size);
        for (uint64_t i = 0; i < L.size; i++) a[i] = (uint8_t)rnd();
        memcpy(b, a, L.size);
        int bw, bh, bpb = nvn_fmt_bpb(d.format, &bw, &bh);
        if (!bpb) { fprintf(stderr, "FAIL: format %#x unknown\n", d.format); return 1; }
        int is3d = d.target == 2, layers = d.target == 4 ? (int)d.depth : 1;
        for (int t = 0; t < 40; t++) {
            unsigned level = rnd() % d.levels;
            uint32_t lw = mip(d.width, level), lh = mip(d.height, level), lz = is3d ? mip(d.depth, level) : 1;
            uint32_t cw = (lw + bw - 1) / bw, ch = (lh + bh - 1) / bh;          // level size in blocks
            uint32_t bx = rnd() % cw, by = rnd() % ch, nbx = 1 + rnd() % (cw - bx), nby = 1 + rnd() % (ch - by);
            unsigned layer = is3d ? 0 : rnd() % layers; uint32_t z = is3d ? rnd() % lz : 0;
            uint32_t wb = nbx * bpb, hb = nby, x0 = bx * bpb, y0 = by;
            size_t row = wb + (rnd() % 3) * 16;                                  // source rows may be padded
            uint8_t *src = malloc(row * hb);
            for (size_t i = 0; i < row * hb; i++) src[i] = (uint8_t)rnd();
            /* old path on a */
            size_t lp = (size_t)cw * bpb, lr = ch;
            uint8_t *lin = malloc(lp * lr * lz);
            if (nvn_deswizzle(&d, level, layer, a, lin, lp)) { fprintf(stderr, "FAIL: deswizzle kind %zu\n", k); return 1; }
            for (uint32_t y = 0; y < hb; y++) memcpy(lin + (size_t)z * lp * lr + (y0 + y) * lp + x0, src + y * row, wb);
            if (nvn_swizzle(&d, level, layer, lin, lp, a)) { fprintf(stderr, "FAIL: swizzle kind %zu\n", k); return 1; }
            /* new path on b */
            if (nvn_swizzle_region(&d, level, layer, z, x0, y0, wb, hb, src, row, b)) { fprintf(stderr, "FAIL: region refused kind %zu level %u\n", k, level); return 1; }
            if (memcmp(a, b, L.size)) { fprintf(stderr, "FAIL: storage differs kind %zu level %u layer %u z %u region %u,%u %ux%u\n", k, level, layer, z, x0, y0, wb, hb); return 1; }
            free(lin); free(src); cases++;
        }
        free(a); free(b);
    }
    /* negative control: a region outside the level is refused */
    nvn_texdesc d; memset(&d, 0, sizeof d); d.target = 1; d.format = 0x25; d.width = 64; d.height = 64; d.depth = 1; d.levels = 1;
    uint8_t s[16] = {0}, st[1 << 16];
    if (!nvn_swizzle_region(&d, 0, 0, 0, 64 * 4 - 8, 0, 16, 1, s, 16, st)) { fputs("FAIL: out-of-level region accepted\n", stderr); return 1; }
    printf("PASS: %u texel regions identical to the whole-level path\n", cases);
    return 0;
}
