// Storage size / alignment / per-level offsets of NVN textures (Tegra X1 block-linear layout).
// The algorithm is ported from deko3d source/dk_image.cpp (zlib license, Copyright (C) 2018-2020 fincs):
//   one GOB = 64 bytes × 8 rows = 512 bytes; the block height (tileH, in GOBs) is picked by 'height×1.5'; levels are accumulated with block alignment.
// The game's packaged textures (SetPackagedTextureLayout) carry 8 bytes of layout parameters: byte 0's low nibble = log2 of the block height (derived by comparing 16 cases
// from the real game, 4 of which differ from the automatic algorithm), its high nibble = log2 of the block depth (3D); when present they take precedence.
// Linear storage counts pixel rows, also for block-compressed formats. The block depth nibble and the pixel rows were found by digitalhivehub against the original
// NVN driver (nxnative PR #11) and taken from MacNativeGames 3c482a7, with the shapes of t_tex_layout.
#include "nvn_tex.h"

typedef struct { uint8_t bpb, bw, bh; } fmt_t;   // Bytes per block, block width, block height (for uncompressed formats block width and height = 1)

static fmt_t fmt_of(int f) {
    static const uint8_t astc[14][2] = {{4,4},{5,4},{5,5},{6,5},{6,6},{8,5},{8,6},{8,8},{10,5},{10,6},{10,8},{10,10},{12,10},{12,12}};
    if (f >= 0x79 && f <= 0x94) { int k = (f - 0x79) % 14; return (fmt_t){16, astc[k][0], astc[k][1]}; }
    if (f >= 0x41 && f <= 0x50) {                         // DXT / RGTC / BPTC: 4×4 blocks
        int b8 = (f <= 0x42) || (f == 0x45 || f == 0x46) || (f == 0x49 || f == 0x4A);
        return (fmt_t){b8 ? 8 : 16, 4, 4};
    }
    static const struct { uint8_t lo, hi, bpb; } r[] = {
        {0x01,0x04,1},{0x05,0x09,2},{0x0A,0x0C,4},{0x0D,0x10,2},{0x11,0x15,4},{0x16,0x18,8},
        {0x19,0x1C,3},{0x1D,0x21,6},{0x22,0x24,12},{0x25,0x28,4},{0x29,0x2D,8},{0x2E,0x30,16},
        {0x31,0x31,1},{0x32,0x32,2},{0x33,0x35,4},{0x36,0x36,8},{0x37,0x38,4},{0x39,0x3C,2},{0x3D,0x40,4},
        {0x51,0x52,1},{0x53,0x54,2},{0x55,0x56,4},{0x57,0x58,2},{0x59,0x5A,4},{0x5B,0x5C,8},
        {0x5D,0x5E,3},{0x5F,0x60,6},{0x61,0x62,12},{0x63,0x64,4},{0x65,0x66,8},{0x67,0x68,16},
        {0x69,0x6C,4},{0x6D,0x70,4},{0x71,0x75,8},{0x76,0x78,16},{0x95,0x98,2},{0x99,0x9C,4},
    };
    for (unsigned i = 0; i < sizeof r / sizeof *r; i++)
        if (f >= r[i].lo && f <= r[i].hi) return (fmt_t){r[i].bpb, 1, 1};
    return (fmt_t){0, 1, 1};
}

int nvn_fmt_bpb(int f, int *bw, int *bh) { fmt_t t = fmt_of(f); if (bw) *bw = t.bw; if (bh) *bh = t.bh; return t.bpb; }

static uint32_t mip(uint32_t s, unsigned l) { s >>= l; return s ? s : 1; }
static uint32_t blk(uint32_t s, uint32_t b) { return (s + b - 1) / b; }
static unsigned pick_tile(uint32_t gobs) { return gobs >= 16 ? 4 : gobs >= 8 ? 3 : gobs >= 4 ? 2 : gobs >= 2 ? 1 : 0; }
// The automatic block height (no packaged layout): NVN truncates height in blocks × 1.5 / 8 before picking the power of two (found by MacNativeGames; Touchable checked
//   16 such textures of Rhythm Heaven Groove). Rounding it up, as this file did until then, gave one step more at 6–10, 17–21, 38–42 and 81–85 block rows: Rhythm Heaven
//   Groove's 672×160 and 1232×160 BC7 textures (40 block rows) need 4 GOBs, not 8 (the calibration screen's beat numbers came out torn); its 112×112, 288×208 and 880×432 agree
static unsigned adj_tile(unsigned shift, uint32_t unit, uint32_t dim) {
    if (!shift) return 0;
    uint32_t x = unit << (shift - 1);
    if (x >= dim) while (--shift) { x >>= 1; if (x < dim) break; }
    return shift;
}

// target: NVNtextureTarget; the number of dims and whether it is layered are determined by target
static void shape(const nvn_texdesc *d, unsigned *dpl, int *layered, uint32_t *w, uint32_t *h, uint32_t *z) {
    *w = d->width; *h = d->height; *z = d->depth; *layered = 0;
    switch (d->target) {
        case 0: *dpl = 1; *h = 1; *z = 1; break;                       // 1D
        case 3: *dpl = 1; *z = d->height; *h = 1; *layered = 1; break; // 1D_ARRAY (layer count is in height)
        case 2: *dpl = 3; break;                                        // 3D
        case 4: case 6: *dpl = 2; *layered = 1; break;                  // 2D_ARRAY / MS_ARRAY
        case 8: *dpl = 2; *z = 6; *layered = 1; break;                  // CUBEMAP
        case 9: *dpl = 2; *layered = 1; break;                          // CUBEMAP_ARRAY: the depth counts layer-faces, as in OpenGL (DRAGON QUEST XI S: 510 for 85 cubes)
        default: *dpl = 2; *z = 1; break;                               // 2D / MS / RECTANGLE
    }
    if (*z == 0) *z = 1;
}

static void samples_xy(int s, uint32_t *sx, uint32_t *sy) {
    *sx = *sy = 1;
    if (s == 2) *sx = 2; else if (s == 4) *sx = *sy = 2; else if (s == 8) { *sx = 4; *sy = 2; }
}

// Sparse textures (flag SPARSE, block-linear) are mapped in 64 KB tiles: one row of blocks, 128 GOBs / (block height × block depth) wide. A level at least one tile wide, high and deep
// is laid out with its width padded to whole tiles, so it fills whole tiles; the levels from the first smaller one on form the mip tail, packed as usual from there, and the
// storage is whole tiles, 64 KB aligned. Monster Hunter Rise's streamed textures come pre-swizzled that way in its files and are placed at the level offsets the host reports: laid out unpadded,
// its 1024×1024 ASTC 6×6 textures (48 GOBs wide instead of 43, level 1 at 0x90000 instead of 0x81000) read 2,183 all-zero blocks in level 0 and 3,043 in level 1, which ASTC
// decodes as its error colour (magenta clothes)
// Yoshi's Crafted World ships its streamed textures pre-swizzled in the 64 KB tile shapes below (sparse_tile_h): a 384×384 DXT5 level is 32 GOBs × two 8-GOB block rows (0x40000 bytes, 16-GOB blocks
// gave 0x30000), a 1350×270 BGRA8 one 88 GOBs × three 16-GOB block rows. UE sizes its upload buffer from the host's layout, and the larger level data overran it into the heap
enum { SPARSE_TILE = 0x10000 };
static int sparse_of(const nvn_texdesc *d) { return (d->flags & 0x20) && !(d->flags & 0x50) && d->target != 10; }
// A sparse 2D texture without a packaged layout whose level 0 is at least one tile wide and high takes the block height of its 64 KB tile instead of the automatic one: the tile's shape in blocks is the standard one,
// 256×256 / 256×128 / 128×128 / 128×64 / 64×64 blocks for 1 / 2 / 4 / 8 / 16 bytes per block, so its rows make a block height of 32 / 16 / 16 / 8 / 8 GOBs
// (log2 5 / 4 / 4 / 3 / 3); -1 for other block sizes. OCTOPATH TRAVELER's world map (4096×2048 BC1, 512 block rows) is laid out in 8 GOBs where the automatic rule gives 16:
// read with 16, its 64 KB tiles came out shuffled into a patchwork; its 512×512 RGBA8 sprite sheets read right with 16, and Monster Hunter Rise's packaged ASTC 6×6 (16 bytes
// per block) carry 8. Checked for 4, 8 and 16 bytes per block; the 1- and 2-byte heights follow the same tile shapes. A texture smaller than one tile is all mip tail and keeps the automatic
// height: Yoshi's Crafted World's 192×160 DXT5 loading icon (12 × 5 GOBs) is laid out in 4 GOBs, its 384×384 DXT1 textures (12 GOBs wide, 96 block rows) in 16; read with 8, the icon lost
// its right half and bottom to the padding
static int sparse_tile_h(unsigned bpb) { return bpb == 1 ? 5 : bpb == 2 || bpb == 4 ? 4 : bpb == 8 || bpb == 16 ? 3 : -1; }
// The block height that shapes a sparse texture's 64 KB tiles: the standard one for its block size, or its own when it has a packaged layout or another block size
static unsigned tile_shape_h(const nvn_texdesc *d, unsigned dpl, fmt_t t, unsigned tileH) {
    return dpl == 2 && !d->pkg_block_h && sparse_tile_h(t.bpb) >= 0 ? (unsigned)sparse_tile_h(t.bpb) : tileH;
}
// Offset of level `level` from the start of its layer, and the GOB columns it is laid out with (*gobs, may be NULL)
static uint64_t level_walk(const nvn_texdesc *d, unsigned level, unsigned dpl, uint32_t w, uint32_t h, uint32_t z,
                           unsigned tileH, unsigned tileD, fmt_t t, uint32_t *gobs) {
    uint32_t sx, sy; samples_xy(d->samples, &sx, &sy);
    unsigned sh = tile_shape_h(d, dpl, t, tileH);
    uint32_t tw = 128u >> (sh + tileD); if (!tw) tw = 1;   // GOB columns of a 64 KB tile
    int tiled = sparse_of(d);   // still in a sparse texture's tiled levels
    uint64_t off = 0;
    for (unsigned i = 0;; i++) {
        uint32_t lw = blk(mip(w * sx, i), t.bw);
        uint32_t lh = dpl >= 2 ? blk(mip(h * sy, i), t.bh) : 1;
        uint32_t ld = dpl >= 3 ? mip(z, i) : 1;
        uint32_t lwb = lw * t.bpb;
        unsigned th = adj_tile(tileH, 8, lh), td = adj_tile(tileD, 1, ld);
        uint32_t wg = (lwb + 63) / 64, hg = (lh + 7) / 8;
        tiled = tiled && lwb >= tw * 64 && lh >= 8u << sh && ld >= 1u << tileD;
        if (tiled) wg = (wg + tw - 1) / tw * tw;
        if (i == level) { if (gobs) *gobs = wg; return off; }
        uint32_t ht = (hg + (1u << th) - 1) >> th, dt = (ld + (1u << td) - 1) >> td;
        off += (uint64_t)wg * ht * dt << (9 + th + td);
    }
}
static uint64_t level_offset(const nvn_texdesc *d, unsigned level, unsigned dpl, uint32_t w, uint32_t h, uint32_t z, unsigned tileH, unsigned tileD, fmt_t t) {
    return level_walk(d, level, dpl, w, h, z, tileH, tileD, t, 0);
}

void nvn_tex_layout(const nvn_texdesc *d, nvn_texlayout *o) {
    fmt_t t = fmt_of(d->format);
    unsigned dpl; int layered; uint32_t w, h, z;
    shape(d, &dpl, &layered, &w, &h, &z);
    uint32_t sx, sy; samples_xy(d->samples, &sx, &sy);
    o->bpb = t.bpb; o->tileH = o->tileD = 0; o->layers = layered ? z : 1;
    if (!t.bpb) { o->size = 0; o->align = 512; o->layer_size = 0; return; }
    if (d->target == 10) {                                            // BUFFER
        o->layer_size = o->size = (uint64_t)t.bpb * d->width;
        o->align = t.bpb == 12 ? 4 : t.bpb; return;
    }
    // LINEAR_RENDER_TARGET (0x40) is linear by itself. Persona 5 Royal creates
    // a 16x16 RGBA8 target with flags 0x44 and stride 128; block-linear sizing
    // returned 1024 instead of 2048 bytes and tripped its surface-memory assertion.
    if (d->flags & 0x50) {                                            // LINEAR or LINEAR_RENDER_TARGET
        o->align = (d->flags & 0x40) ? 128 : 32;
        uint32_t stride = d->stride ? d->stride : (t.bpb * blk(w, t.bw) + o->align - 1) & ~(o->align - 1);
        o->layer_size = o->size = (uint64_t)stride * h;               // the driver counts pixel rows, also for block-compressed formats
        return;
    }
    unsigned tileH = 0, tileD = 0;
    if (d->pkg_block_h) {                                             // packaged byte 0: low nibble block height, high nibble block depth
        unsigned pl = (unsigned)(d->pkg_block_h - 1) & 0xff;
        if (dpl >= 2) tileH = pl & 0xf;
        if (dpl == 3) tileD = pl >> 4;
    } else if (dpl == 2 && sparse_of(d) && sparse_tile_h(t.bpb) >= 0 && blk(w * sx, t.bw) * t.bpb >= (128u >> sparse_tile_h(t.bpb)) * 64
               && blk(h * sy, t.bh) >= 8u << sparse_tile_h(t.bpb)) tileH = (unsigned)sparse_tile_h(t.bpb);
    else if (dpl == 2) { uint32_t ah = blk(h * sy, t.bh); tileH = pick_tile(ah * 3 / 16); }
    else if (dpl == 3) tileD = pick_tile(z + z / 2);
    o->tileH = tileH; o->tileD = tileD;
    unsigned levels = d->levels ? d->levels : 1;
    uint64_t ls = level_offset(d, levels, dpl, w, h, z, tileH, tileD, t);
    if (sparse_of(d)) ls = (ls + SPARSE_TILE - 1) & ~(uint64_t)(SPARSE_TILE - 1);
    if (layered) {                                                    // Layer size is aligned to the block
        uint32_t hh = blk(h, t.bh); unsigned th = tileH, td = tileD;
        while (th && hh <= (8u << (th - 1))) th--;
        while (td && z <= (1u << (td - 1))) td--;
        unsigned sh = th + td + 9; uint64_t g = ls >> sh;
        if (ls != g << sh) ls = (g + 1) << sh;
    }
    o->layer_size = ls;
    o->size = layered ? ls * z : ls;
    o->align = sparse_of(d) ? SPARSE_TILE : 512;
}

uint64_t nvn_tex_level_offset(const nvn_texdesc *d, unsigned level) {
    fmt_t t = fmt_of(d->format);
    unsigned dpl; int layered; uint32_t w, h, z;
    shape(d, &dpl, &layered, &w, &h, &z);
    if (!t.bpb || (d->flags & 0x50) || d->target == 10) return 0;
    nvn_texlayout L; nvn_tex_layout(d, &L);
    return level_offset(d, level, dpl, w, h, z, L.tileH, L.tileD, t);
}

uint32_t nvn_tex_level_gobs(const nvn_texdesc *d, unsigned level) {
    fmt_t t = fmt_of(d->format);
    unsigned dpl; int layered; uint32_t w, h, z, g = 0;
    shape(d, &dpl, &layered, &w, &h, &z);
    if (!t.bpb || (d->flags & 0x50) || d->target == 10) return 0;
    nvn_texlayout L; nvn_tex_layout(d, &L);
    level_walk(d, level, dpl, w, h, z, L.tileH, L.tileD, t, &g);
    return g;
}

int nvn_tex_sparse_tiles(const nvn_texdesc *d, uint32_t *tw, uint32_t *th, uint32_t *td) {
    fmt_t t = fmt_of(d->format);
    unsigned dpl; int layered; uint32_t w, h, z;
    shape(d, &dpl, &layered, &w, &h, &z);
    *tw = *th = *td = 0;
    if (!sparse_of(d) || !t.bpb) return 0;
    uint32_t sx, sy; samples_xy(d->samples, &sx, &sy);
    nvn_texlayout L; nvn_tex_layout(d, &L);
    unsigned sh = tile_shape_h(d, dpl, t, L.tileH);
    uint32_t gw = 128u >> (sh + L.tileD); if (!gw) gw = 1;   // GOB columns of a tile, as level_walk
    *tw = gw * 64 / t.bpb * t.bw / sx; *th = dpl >= 2 ? (8u << sh) * t.bh / sy : 1; *td = dpl >= 3 ? 1u << L.tileD : 1;
    unsigned levels = d->levels ? d->levels : 1, n = 0;
    for (; n < levels; n++) {   // the same test as level_walk's: a level at least one tile wide, high and deep
        uint32_t lwb = blk(mip(w * sx, n), t.bw) * t.bpb, lh = dpl >= 2 ? blk(mip(h * sy, n), t.bh) : 1, ld = dpl >= 3 ? mip(z, n) : 1;
        if (lwb < gw * 64 || lh < 8u << sh || ld < 1u << L.tileD) break;
    }
    return (int)n;
}

void nvn_tex_level_tiles(const nvn_texdesc *d, unsigned level, unsigned *tileH, unsigned *tileD) {
    fmt_t t = fmt_of(d->format);
    unsigned dpl; int layered; uint32_t w, h, z;
    shape(d, &dpl, &layered, &w, &h, &z);
    uint32_t sx, sy; samples_xy(d->samples, &sx, &sy);
    nvn_texlayout L; nvn_tex_layout(d, &L);
    uint32_t lh = dpl >= 2 ? blk(mip(h * sy, level), t.bh) : 1;
    uint32_t ld = dpl >= 3 ? mip(z, level) : 1;
    *tileH = adj_tile(L.tileH, 8, lh);
    *tileD = adj_tile(L.tileD, 1, ld);
}

int nvn_tbuf_rows(uint64_t n, uint32_t bpb, uint32_t align, uint32_t *w, uint32_t *h, uint32_t *row) {
    if (!n || !bpb) return 0;
    if (!align) align = 1;
    *w = n < NVN_TBUF_ROW ? (uint32_t)n : NVN_TBUF_ROW;
    *h = (uint32_t)((n + NVN_TBUF_ROW - 1) / NVN_TBUF_ROW);
    uint64_t r = (uint64_t)*w * bpb; r = (r + align - 1) / align * align;
    *row = (uint32_t)r;
    return *h == 1 || r == (uint64_t)NVN_TBUF_ROW * bpb;   // several rows must lie back to back, as the wrapped address assumes
}
