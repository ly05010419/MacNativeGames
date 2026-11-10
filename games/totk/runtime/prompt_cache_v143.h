/* Tears of the Kingdom 1.4.3 scalable-font cache adapter.
   The guarded flush calls publish glyph rectangles after rasterization. The
   renderer replaces only those rectangles in its own texture copy; it never
   writes into the game's font cache or changes text metrics.

   Layout from main+0xcd8810: cache+0x28/0x2c are atlas dimensions;
   +0x1880 counts 24-byte row lists starting at +0x80. Their nodes are
   glyph+0x10; glyph+0x30 is the code point, +0x40/42 the atlas coordinates,
   and +0x44/46 the bitmap extent. The atlas uses bottom-origin coordinates.
   These offsets must be re-verified before enabling another game version. */
#pragma once
#include "runtime.h"
#include "text_padding_v143.h"
#include "prompt_bindings.h"
#include "../third_party/d2r_gfx/gfx/nvn/totk_prompt_icons.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !TARGET_OS_IPHONE
static void (*prompt_cache_original)(void *);
static void *(*prompt_cache_map)(void *);
static uint64_t prompt_cache_u64(const void *p, size_t off) {
    uint64_t v; memcpy(&v, (const char *)p + off, sizeof v); return v;
}
static uint32_t prompt_cache_u32(const void *p, size_t off) {
    uint32_t v; memcpy(&v, (const char *)p + off, sizeof v); return v;
}
static uint16_t prompt_cache_u16(const void *p, size_t off) {
    uint16_t v; memcpy(&v, (const char *)p + off, sizeof v); return v;
}
static int prompt_cache_region_compare(const void *left, const void *right) {
    const TotkPromptRegion *a = left, *b = right;
    if (a->y != b->y) return a->y < b->y ? -1 : 1;
    if (a->x != b->x) return a->x < b->x ? -1 : 1;
    if (a->codepoint != b->codepoint) return a->codepoint < b->codepoint ? -1 : 1;
    return 0;
}
static void prompt_cache_update(void *cache) {
    prompt_cache_original(cache);
    unsigned width = prompt_cache_u32(cache, 0x28), height = prompt_cache_u32(cache, 0x2c);
    unsigned rows = prompt_cache_u32(cache, 0x1880), count = 0;
    if (!width || !height || width > 8192 || height > 16384 || rows > 256) return;
    TotkPromptRegion regions[512];
    PromptBindings b = {.device = PROMPT_KEYBOARD};
    for (unsigned row = 0; row < rows; row++) {
        uintptr_t head = (uintptr_t)cache + 0x80 + row * 24;
        uintptr_t node = prompt_cache_u64((void *)head, 8);
        unsigned visited = 0;
        while (node != head && node && visited++ < 4096) {
            const void *glyph = (void *)(node - 0x10);
            unsigned cp = prompt_cache_u32(glyph, 0x30);
            PromptIcon icon;
            if (count < 512 && prompt_resolve(&b, cp, &icon)) {
                unsigned x = prompt_cache_u16(glyph, 0x40) + 1, y = prompt_cache_u16(glyph, 0x42);
                unsigned w = prompt_cache_u16(glyph, 0x44), h = prompt_cache_u16(glyph, 0x46);
                if (w && h && w <= width && h <= height && x <= width - w && y <= height - h)
                    regions[count++] = (TotkPromptRegion){cp, x, height - y - h, w, h};
            }
            node = prompt_cache_u64((void *)node, 8);
        }
    }
    const uint8_t *memory = prompt_cache_map((void *)prompt_cache_u64(cache, 0x1e98));
    if (!memory) return;
    memory += prompt_cache_u64(cache, 0x1ea0);
    /* Row lists are ordered by use. Their ordering must not invalidate GPU
       bindings when the same cached glyphs are merely drawn in another order. */
    qsort(regions, count, sizeof *regions, prompt_cache_region_compare);
    totk_prompt_set_atlas(memory, width, height, width, regions, count);
}
static void prompt_cache_install(Module *m) {
    const char *enabled = getenv("TOTK_PROMPT_ICONS");
    if ((enabled && !strcmp(enabled, "0")) || getenv("TOTK_EXPORT_IOS")) return;
    const size_t calls[] = {0xc624e8, 0xcd87fc};
    const size_t original = 0xcd8810, stub = ((m->text_end + 15) & ~15ULL) + PAD_V143_PROMPT;   /* text_padding_v143.h */
    if (stub + 16 > m->ro_off) runtime_fail("prompt cache: no veneer space");
    for (unsigned i = 0; i < sizeof calls / sizeof *calls; i++) {
        uint32_t expected = 0x94000000u | ((uint32_t)((int64_t)original - calls[i]) / 4 & 0x03ffffff);
        if (prompt_cache_u32(m->base, calls[i]) != expected)
            runtime_fail("prompt cache: unexpected 1.4.3 flush call");
    }
    for (unsigned i = 0; i < 16; i++)
        if (m->base[stub + i]) runtime_fail("prompt cache: veneer space occupied");
    prompt_cache_original = (void (*)(void *))(m->base + original);
    prompt_cache_map = (void *(*)(void *))(m->base + 0x10b6344);
    uint32_t code[4] = {0x58000050, 0xd61f0200, 0, 0};
    void *hook = runtime_hle_wrap_named(prompt_cache_update, "prompt cache update");
    memcpy(code + 2, &hook, sizeof hook); memcpy(m->base + stub, code, sizeof code);
    for (unsigned i = 0; i < sizeof calls / sizeof *calls; i++) {
        uint32_t branch = 0x94000000u | ((uint32_t)((int64_t)stub - calls[i]) / 4 & 0x03ffffff);
        memcpy(m->base + calls[i], &branch, sizeof branch);
    }
    fprintf(stderr, "[totk-input] live prompt cache hook enabled (1.4.3)\n");
}
#else
static void prompt_cache_install(Module *m) { (void)m; }
#endif
