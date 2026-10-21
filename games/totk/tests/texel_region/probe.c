/* CPU only: nvn_deswizzle_region / nvn_swizzle_region / nvn_region_span (ReadTexels / WriteTexels on CPU-pool textures)
 * against the whole-level nvn_deswizzle / nvn_swizzle, on random storage and random textures (formats with 1-16 bytes a
 * block, 2D, 3D, 2D arrays, linear rectangles, several levels). For each random region:
 *   read  = that crop of the whole-level read;
 *   write = whole-level read, patch the region, whole-level write (byte for byte over the whole storage);
 *   span  holds every byte the region write changed.
 * Controls: the whole level as a region equals nvn_deswizzle; a region one block row lower reads other bytes; a region
 * past the level is refused. */
#include "nvn_tex.h"
#include "nvn_deswizzle.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static uint32_t seed = 12345;
static uint32_t pick(uint32_t n) { seed = seed * 1103515245u + 12345u; return n ? (seed >> 8) % n : 0; }
static int checks, failed;
static void check(int ok, const char *what, const nvn_texdesc *d, unsigned level) {
    checks++;
    if (!ok && failed++ < 20) printf("FAIL %s: fmt %#x target %d %ux%ux%u levels %u, level %u\n", what, d->format, d->target, d->width, d->height, d->depth, d->levels, level);
}

int main(void) {
    int formats[48], nformats = 0;
    for (int f = 1; f < 0x110 && nformats < 48; f++) {
        int bw, bh, bpb = nvn_fmt_bpb(f, &bw, &bh);
        if (bpb == 1 || bpb == 2 || bpb == 4 || bpb == 8 || bpb == 16) formats[nformats++] = f;
    }
    static const int targets[] = {1, 1, 1, 1, 2, 4, 7, 7};   /* 2D (most), 3D, 2D array, rectangle (linear or not) */
    int textures = 0, regions = 0;
    for (int it = 0; it < 500; it++) {
        nvn_texdesc d; memset(&d, 0, sizeof d);
        d.format = formats[pick(nformats)]; d.target = targets[pick(8)];
        d.width = 1 + pick(it % 4 ? 260 : 1200); d.height = 1 + pick(it % 4 ? 260 : 600);
        d.depth = d.target == 2 ? 1 + pick(20) : d.target == 4 ? 1 + pick(5) : 1;
        if (d.target == 7 && pick(2)) d.flags = 0x10;   /* linear storage */
        unsigned levels = 1; while ((d.width >> levels) || (d.height >> levels)) levels++;
        d.levels = nvn_tex_is_linear(&d) ? 1 : 1 + pick(levels);
        nvn_texlayout L; nvn_tex_layout(&d, &L);
        if (!L.size || L.size > (48u << 20)) continue;
        uint8_t *orig = malloc(L.size), *byregion = malloc(L.size), *bylevel = malloc(L.size);
        for (uint64_t i = 0; i < L.size; i++) orig[i] = (uint8_t)pick(256);
        int bw, bh, bpb = nvn_fmt_bpb(d.format, &bw, &bh);
        textures++;
        for (int k = 0; k < 8; k++) {
            unsigned level = pick(d.levels), layer = d.target == 4 ? pick(d.depth) : 0;
            uint32_t lw = d.width >> level ? d.width >> level : 1, lh = d.height >> level ? d.height >> level : 1;
            uint32_t lz = d.target == 2 ? (d.depth >> level ? d.depth >> level : 1) : 1;
            uint32_t cols = (lw + bw - 1) / bw, rows = (lh + bh - 1) / bh, lp = cols * bpb;
            size_t limg = (size_t)lp * rows;
            uint8_t *level_bytes = malloc(limg * lz);
            if (nvn_deswizzle(&d, level, layer, orig, level_bytes, lp)) { free(level_bytes); continue; }
            regions++;
            /* the whole level as a region */
            uint8_t *got = malloc(limg * lz);
            check(!nvn_deswizzle_region(&d, level, layer, orig, got, lp, limg, 0, 0, 0, lp, rows, lz) && !memcmp(got, level_bytes, limg * lz), "whole level as a region", &d, level);
            free(got);
            uint32_t cx = pick(cols), cy = pick(rows), cz = pick(lz), cw = 1 + pick(cols - cx), ch = 1 + pick(rows - cy), cn = 1 + pick(lz - cz);
            uint32_t x0 = cx * bpb, w = cw * bpb;
            size_t pitch = w + 8 * pick(4), img = pitch * ch + 32 * pick(3);
            uint8_t *region = malloc(img * cn), *patch = malloc(img * cn);
            /* read */
            int ok = !nvn_deswizzle_region(&d, level, layer, orig, region, pitch, img, x0, cy, cz, w, ch, cn);
            for (uint32_t z = 0; z < cn && ok; z++) for (uint32_t y = 0; y < ch && ok; y++)
                ok = !memcmp(region + z * img + y * pitch, level_bytes + (cz + z) * limg + (size_t)(cy + y) * lp + x0, w);
            check(ok, "region read", &d, level);
            /* write */
            for (size_t i = 0; i < img * cn; i++) patch[i] = (uint8_t)pick(256);
            memcpy(byregion, orig, L.size); memcpy(bylevel, orig, L.size);
            check(!nvn_swizzle_region(&d, level, layer, patch, pitch, img, byregion, x0, cy, cz, w, ch, cn), "region write accepted", &d, level);
            for (uint32_t z = 0; z < cn; z++) for (uint32_t y = 0; y < ch; y++)
                memcpy(level_bytes + (cz + z) * limg + (size_t)(cy + y) * lp + x0, patch + z * img + y * pitch, w);
            nvn_swizzle(&d, level, layer, level_bytes, lp, bylevel);
            check(!memcmp(byregion, bylevel, L.size), "region write", &d, level);
            /* span */
            uint64_t lo, hi;
            ok = !nvn_region_span(&d, level, layer, cy, ch, cz, cn, &lo, &hi) && lo < hi && hi <= L.size;
            for (uint64_t i = 0; i < L.size && ok; i++) if (byregion[i] != orig[i] && (i < lo || i >= hi)) ok = 0;
            check(ok, "span holds the write", &d, level);
            /* controls */
            if (cy + ch < rows) {
                nvn_deswizzle_region(&d, level, layer, orig, region, pitch, img, x0, cy + 1, cz, w, ch, 1);
                int same = 1;
                for (uint32_t y = 0; y < ch && same; y++) same = !memcmp(region + y * pitch, level_bytes + (size_t)(cy + y) * lp + x0, w);
                check(!same || w * ch < 4, "control: one block row lower reads other bytes", &d, level);
            }
            check(nvn_deswizzle_region(&d, level, layer, orig, region, pitch, img, x0, rows, 0, w, 1, 1) != 0, "control: past the level refused", &d, level);
            free(region); free(patch); free(level_bytes);
        }
        free(orig); free(byregion); free(bylevel);
    }
    printf("%d textures, %d regions, %d checks, %d failed\n", textures, regions, checks, failed);
    printf("{\"status\":\"%s\",\"checks\":%d,\"failed\":%d}\n", failed ? "fail" : "pass", checks, failed);
    return failed != 0;
}
