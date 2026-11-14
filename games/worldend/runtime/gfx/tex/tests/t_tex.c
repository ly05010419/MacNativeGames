// Block-linear deswizzle unit tests (seconds). A round trip; B hand-calculated addresses; E hand-built 3D.
// Usage: t_tex
#include "../nvn_deswizzle.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks, fails;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("  ✗ "); printf(__VA_ARGS__); printf("\n"); } } while (0)
static uint32_t mipd(uint32_t s, unsigned l) { s >>= l; return s ? s : 1; }
static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint8_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint8_t)rng; }

// Linear size of this level (computed independently on the test side: width-in-blocks bytes, block rows, slices)
static void dims(const nvn_texdesc *d, unsigned l, uint32_t *rb, uint32_t *rows, uint32_t *sl) {
    int bw, bh, bpb = nvn_fmt_bpb(d->format, &bw, &bh);
    *rb = (mipd(d->width, l) + bw - 1) / bw * bpb; *rows = (mipd(d->height, l) + bh - 1) / bh;
    *sl = d->target == 2 ? mipd(d->depth, l) : 1;
}

// ---------- A round trip: first write different random data into every level / layer, then decode them all and compare (overlap between levels or between layers, or out-of-bounds, all fail) ----------
static void roundtrip(const char *tag, int target, int fmt, uint32_t w, uint32_t h, uint32_t depth, uint32_t levels) {
    nvn_texdesc d = {target, fmt, 0, 0, w, h, depth, levels, 0};
    nvn_texlayout L; nvn_tex_layout(&d, &L);
    size_t guard = 4096; uint8_t *st = malloc(L.size + guard); memset(st, 0, L.size); memset(st + L.size, 0xAB, guard);
    uint8_t *ref[64][16] = {{0}}; uint64_t bytes = 0;
    for (unsigned y = 0; y < L.layers; y++) for (unsigned l = 0; l < levels; l++) {
        uint32_t rb, rows, sl; dims(&d, l, &rb, &rows, &sl);
        size_t n = (size_t)rb * rows * sl; ref[y][l] = malloc(n); for (size_t i = 0; i < n; i++) ref[y][l][i] = rnd();
        CHECK(!nvn_swizzle(&d, l, y, ref[y][l], rb, st), "%s write failed layer %u level %u", tag, y, l);
    }
    int guard_ok = 1; for (size_t i = 0; i < guard; i++) guard_ok &= st[L.size + i] == 0xAB;
    CHECK(guard_ok, "%s wrote past the end of storage (out of bounds)", tag);
    int ok = 1;
    for (unsigned y = 0; y < L.layers; y++) for (unsigned l = 0; l < levels; l++) {
        uint32_t rb, rows, sl; dims(&d, l, &rb, &rows, &sl);
        size_t n = (size_t)rb * rows * sl; uint8_t *out = malloc(n);
        CHECK(!nvn_deswizzle(&d, l, y, st, out, rb), "%s decode failed", tag);
        if (memcmp(out, ref[y][l], n)) { ok = 0; printf("  ✗ %s layer %u level %u round trip differs\n", tag, y, l); }
        bytes += n; free(out); free(ref[y][l]);
    }
    checks++; if (!ok) fails++;
    printf("  A %-34s layers %u × levels %u, round trip %llu bytes %s\n", tag, L.layers, levels, (unsigned long long)bytes, ok ? "identical" : "differ");
    free(st);
}

int main(void) {
    printf("A round trip (random data, non-power-of-2 sizes)\n");
    roundtrip("2D 100×37 R8 4 levels", 1, 0x01, 100, 37, 1, 4);
    roundtrip("2D 100×37 RG8 4 levels", 1, 0x0D, 100, 37, 1, 4);
    roundtrip("2D 100×37 RGBA8 4 levels", 1, 0x25, 100, 37, 1, 4);
    roundtrip("2D 100×37 RGBA16F 4 levels", 1, 0x29, 100, 37, 1, 4);
    roundtrip("2D 100×37 RGBA32F 4 levels", 1, 0x2E, 100, 37, 1, 4);
    roundtrip("2D 100×37 BC3(16B/block) 4 levels", 1, 0x44, 100, 37, 1, 4);
    roundtrip("2D 300×300 BC1(8B/block) 5 levels", 1, 0x42, 300, 300, 1, 5);
    roundtrip("2D array 100×37×3 layers RGBA8 3 levels", 4, 0x25, 100, 37, 3, 3);
    roundtrip("2D array 64×64×2 layers BC3 4 levels", 4, 0x44, 64, 64, 2, 4);
    roundtrip("3D 100×37×5 RGBA8 3 levels", 2, 0x25, 100, 37, 5, 3);
    roundtrip("3D 64×64×20 R8 4 levels", 2, 0x01, 64, 64, 20, 4);
    roundtrip("3D 40×30×9 RGBA32F 3 levels", 2, 0x2E, 40, 30, 9, 3);

    printf("B address formula (expected values hand-calculated from the public GOB 64B×8-row layout)\n");
    struct { uint32_t x, y, z, rb, rows; unsigned th, td; uint64_t want; const char *how; } B[] = {
        {0, 0, 0, 64, 8, 0, 0, 0, "origin"},
        {17, 3, 0, 64, 8, 0, 0, 113, "within GOB: (3/2)·64 + (17%32/16)·32 + (3%2)·16 + 17%16 = 64+32+16+1"},
        {70, 9, 0, 128, 32, 1, 0, 1558, "block height 2: block column 1 1024 + GOB 1 within the block 512 + within GOB (9%2)·16 + 6 = 22"},
        {0, 0, 1, 64, 8, 0, 1, 512, "block depth 2: GOB 1 within the block (z%2·block height 1) = 512"},
        {0, 0, 2, 64, 8, 0, 1, 1024, "block depth 2: block layer 1 = 1 column × 1 row × 1024"},
        {200, 40, 0, 256, 64, 2, 0, 9284, "block height 4 (32 rows): block row 1 4·2048=8192 + block column 3 3·2048=6144? see below"},
    };
    // Last hand-calculated case: rb=256 → 4 GOBs wide; block height 4 GOBs = 32 rows, block 2048 bytes; y=40 → block row 1: 4×2048 = 8192;
    // x=200 → block column 3: 3×2048 = 6144; GOB within the block = (40%32)/8 = 1 → 512; within the GOB x%64=8, y%8=0 → 8. Total 8192+6144+512+8 = 14856
    B[5].want = 14856; B[5].how = "block height 4: block row 8192 + block column 6144 + GOB within block 512 + within GOB 8";
    for (unsigned i = 0; i < sizeof B / sizeof *B; i++) {
        uint64_t got = nvn_bl_offset(B[i].x, B[i].y, B[i].z, B[i].rb, B[i].rows, B[i].th, B[i].td);
        CHECK(got == B[i].want, "B%u got %llu expected %llu", i, (unsigned long long)got, (unsigned long long)B[i].want);
        printf("  B%u (%u,%u,%u) → %llu %s  %s\n", i, B[i].x, B[i].y, B[i].z, (unsigned long long)got, got == B[i].want ? "✓" : "✗", B[i].how);
    }

    // ---------- E hand-built 3D: place markers into block-linear storage at hand-calculated addresses, decode, and verify where the markers land (round trip A only proves self-consistency, not whether the layout is right) ----------
    // Source of the block height / block depth rule: deko3d dk_image.cpp - for 3D textures block height is fixed at 1 GOB, block depth = pickTileSize(depth + depth/2).
    printf("E hand-built 3D block-linear (markers at hand-calculated addresses, landing points checked after deswizzle)\n");
    {
        nvn_texdesc d = {2, 0x25, 0, 0, 32, 16, 20, 1, 0, 0};   // 3D RGBA8 32×16×20: row 128 bytes = 2 GOBs wide, 16 rows = 2 GOBs high
        nvn_texlayout L; nvn_tex_layout(&d, &L);
        // Block depth: 20 + 10 = 30 ≥ 16 → 16 slices (log2 = 4); block = 512 × 16 = 8192 bytes; one block layer = 2 columns × 2 rows × 8192 = 32768; ceil(20/16) = 2 layers → 65536
        CHECK(L.tileH == 0 && L.tileD == 4, "E 32×16×20 block height/depth = %u/%u, expected 0/4", L.tileH, L.tileD);
        CHECK(L.size == 65536, "E 32×16×20 storage %llu expected 65536", (unsigned long long)L.size);
        struct { uint32_t x, y, z; uint64_t addr; const char *how; } V[] = {
            {0, 0, 0, 0, "origin"},
            {20, 11, 3, 26224, "block row 1·2·8192=16384 + block column (80/64=1)·8192 + GOB within block z%16=3·512=1536 + within GOB (x80%64=16,y3) 64+32+16 = 112"},
            {31, 15, 17, 58364, "depth block 1·32768 + block row 16384 + block column 8192 + GOB within block 1·512 + within GOB (x60,y7) 256+192+32+16+12 = 508"},
            {5, 2, 19, 34404, "depth block 32768 + GOB within block 3·512=1536 + within GOB (x20,y2) 64+32+4 = 100"},
        };
        uint8_t *st = calloc(L.size, 1), *lin = calloc(128 * 16 * 20, 1);
        for (unsigned i = 0; i < 4; i++) for (int k = 0; k < 4; k++) st[V[i].addr + k] = (uint8_t)(0x10 * (i + 1) + k + 1);
        CHECK(!nvn_deswizzle(&d, 0, 0, st, lin, 128), "E deswizzle failed");
        for (unsigned i = 0; i < 4; i++) {
            const uint8_t *p = lin + ((size_t)V[i].z * 16 + V[i].y) * 128 + V[i].x * 4; int ok = 1;
            for (int k = 0; k < 4; k++) ok &= p[k] == (uint8_t)(0x10 * (i + 1) + k + 1);
            CHECK(ok, "E voxel (%u,%u,%u) did not get the marker at hand-calculated address %llu", V[i].x, V[i].y, V[i].z, (unsigned long long)V[i].addr);
            printf("  E (%2u,%2u,%2u) ← storage %5llu %s  %s\n", V[i].x, V[i].y, V[i].z, (unsigned long long)V[i].addr, ok ? "✓" : "✗", V[i].how);
        }
        size_t nz = 0; for (size_t j = 0; j < 128 * 16 * 20; j++) nz += lin[j] != 0;
        CHECK(nz == 16, "E nonzero decoded bytes %zu expected 16 (there should be no data outside the markers)", nz);
        free(st); free(lin);
        // Shape of the real color lookup table on the character select screen: 32³ RGBA16F (ser 17 / 16). Block depth 32 + 16 = 48 ≥ 16 → 16 slices; row 256 bytes = 4 GOBs wide, 32 rows = 4 GOBs high, 2 block layers → 4·4·2·8192 = 262144
        nvn_texdesc lut = {2, 0x29, 0, 0, 32, 32, 32, 1, 0, 0}; nvn_tex_layout(&lut, &L);
        CHECK(L.tileH == 0 && L.tileD == 4 && L.size == 4 * 4 * 2 * 8192ull, "E 32³ RGBA16F block height/depth %u/%u storage %llu, expected 0/4, 262144", L.tileH, L.tileD, (unsigned long long)L.size);
        printf("  E 32³ RGBA16F (the shape of a 32³ color lookup table) block height/depth %u/%u, storage %llu (expected 0/4, 262144)\n", L.tileH, L.tileD, (unsigned long long)L.size);
    }

    // ---------- S sparse layout: Monster Hunter Rise's streamed 1024×1024 ASTC 6×6 sRGB textures (flag SPARSE, packaged block height 8 GOBs), level 0 checked against the game's data ----------
    // A 64 KB tile is 16 GOBs × 8 GOBs here. Level 0 (171 block columns = 43 GOBs) is padded to 48 GOBs × 3 block rows = 9 tiles: its dumped data decodes with no all-zero block that way
    // (2,183 unpadded). Level 1 (86 = 22 GOBs → 32, 2 block rows) is 4 tiles; level 2 (43 block columns, less than a tile's 64) starts the mip tail, packed as before; storage in whole tiles
    printf("S sparse layout (64 KB tiles: tiled levels padded to whole tiles, then the mip tail)\n");
    {
        nvn_texdesc d = {1, 0x8B, 0x20, 0, 1024, 1024, 1, 8, 0, 4}; nvn_texlayout L; nvn_tex_layout(&d, &L);
        static const uint64_t off[8] = {0, 0x90000, 0xD0000, 0xDB000, 0xDE000, 0xDEC00, 0xDF000, 0xDF200};
        static const uint32_t gobs[3] = {48, 32, 11};
        for (unsigned l = 0; l < 8; l++) CHECK(nvn_tex_level_offset(&d, l) == off[l], "S level %u offset %#llx expected %#llx", l, (unsigned long long)nvn_tex_level_offset(&d, l), (unsigned long long)off[l]);
        for (unsigned l = 0; l < 3; l++) CHECK(nvn_tex_level_gobs(&d, l) == gobs[l], "S level %u laid out %u GOBs wide, expected %u", l, nvn_tex_level_gobs(&d, l), gobs[l]);
        CHECK(L.size == 0xE0000 && L.align == 0x10000, "S storage %#llx alignment %#x, expected 0xe0000 / 0x10000", (unsigned long long)L.size, L.align);
        printf("  S 1024×1024 ASTC 6×6 sparse: levels at 0, %#llx, %#llx …, storage %#llx (expected 0x90000, 0xd0000, 0xe0000)\n", (unsigned long long)nvn_tex_level_offset(&d, 1), (unsigned long long)nvn_tex_level_offset(&d, 2), (unsigned long long)L.size);
        d.flags = 0; nvn_tex_layout(&d, &L);   // the same texture not sparse keeps the packed layout
        CHECK(nvn_tex_level_offset(&d, 1) == 0x81000 && L.size == 0xBC400 && nvn_tex_level_gobs(&d, 0) == 43, "S not sparse: level 1 %#llx storage %#llx, expected 0x81000 / 0xbc400",
              (unsigned long long)nvn_tex_level_offset(&d, 1), (unsigned long long)L.size);
    }

    printf("%s: %d/%d checks passed\n", fails ? "RED" : "all passed", checks - fails, checks);
    return fails ? 1 : 0;
}
