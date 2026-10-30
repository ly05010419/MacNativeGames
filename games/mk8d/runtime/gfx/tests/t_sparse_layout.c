// Storage sizes of sparse (flag 0x20) 2D textures with the automatic block height, against the sizes of the pre-swizzled level data the games ship: Yoshi's Crafted World's
// streamed DXT and BGRA8 textures (one level each) and Monster Hunter Rise's 1024×1024 ASTC 6×6 (48 GOBs wide, level 1 at 0x90000). A non-sparse texture keeps the automatic block height
#include <stdio.h>
#include "../nvn/nvn_tex.h"

static int g_n, g_fail;

static void check_size(uint32_t w, uint32_t h, int format, int flags, uint64_t want) {
    nvn_texdesc d = { .target = 1, .format = format, .flags = flags, .width = w, .height = h, .depth = 1, .levels = 1 };
    nvn_texlayout L; nvn_tex_layout(&d, &L);
    g_n++; if (L.size != want) g_fail++;
    printf("%4ux%-4u format %#x flags %#x: size %#llx (want %#llx) %s\n", w, h, format, flags, (unsigned long long)L.size, (unsigned long long)want, L.size == want ? "✓" : "✗");
}

int main(void) {
    static const struct { uint32_t w, h; int format; uint64_t size; } yoshi[] = {
        {384, 384, 0x48, 0x40000}, {384, 384, 0x45, 0x20000}, {960, 384, 0x48, 0x80000}, {480, 480, 0x48, 0x40000}, {900, 420, 0x48, 0x80000},
        {1920, 912, 0x48, 0x200000}, {1920, 864, 0x48, 0x200000}, {1440, 480, 0x48, 0xc0000}, {840, 448, 0x48, 0x80000}, {840, 840, 0x48, 0x100000},
        {1350, 270, 0x9c, 0x210000}, {240, 270, 0x9c, 0x60000}, {720, 270, 0x9c, 0x120000}, {1080, 174, 0x9c, 0x120000}, {2010, 390, 0x9c, 0x400000},
    };
    for (unsigned i = 0; i < sizeof yoshi / sizeof *yoshi; i++) check_size(yoshi[i].w, yoshi[i].h, yoshi[i].format, 0x20, yoshi[i].size);
    check_size(384, 384, 0x48, 0, 0x30000);
    nvn_texdesc mh = { .target = 1, .format = 0x7d, .flags = 0x20, .width = 1024, .height = 1024, .depth = 1, .levels = 11 };
    uint64_t l1 = nvn_tex_level_offset(&mh, 1); uint32_t g0 = nvn_tex_level_gobs(&mh, 0);
    g_n++; if (l1 != 0x90000 || g0 != 48) g_fail++;
    printf("1024x1024 ASTC 6x6 sparse: level 0 %u GOBs wide, level 1 at %#llx (want 48, 0x90000) %s\n", g0, (unsigned long long)l1, l1 == 0x90000 && g0 == 48 ? "✓" : "✗");
    printf("== %d checks, %d failed ==\n", g_n, g_fail);
    return g_fail != 0;
}
