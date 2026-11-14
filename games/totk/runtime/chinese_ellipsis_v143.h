/* Tears of the Kingdom 1.4.3, macOS: use the Chinese text face for U+2026.
   CNzh/TWzh's FCPX font lists route U+2026 to Symbol.bfotf. The Chinese text
   faces supply a centred U+2026; keep that code point throughout selection
   and rendering so the existing glyph coverage and message strings apply.

   main+0x10bdac0 selects a physical face for (cache, out_face, logical_font,
   codepoint). For an ellipsis, first resolve U+4E2D (中) in that same logical font,
   then check that the selected engine also has U+2026. Only the physical face
   changes: cache keys, rasterization, advances and kerning still use U+2026.
   If either lookup fails, use the original selection for U+2026.

   All ten direct callers are guarded below; the selector has no address
   relocations. A guest leaf handles other characters without a host call.
   The original selector stays intact, so the host callback can call it without
   recursion. The two veneers occupy the remaining text-padding gaps. */
#pragma once
#include "runtime.h"
#include "text_padding_v143.h"
#include <TargetConditionals.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !TARGET_OS_IPHONE
static int (*chinese_ellipsis_original)(void *, uint32_t *, unsigned, unsigned);
static int (*chinese_ellipsis_exists)(void *, unsigned);

static uint32_t chinese_ellipsis_u32(const void *p, size_t off) {
    uint32_t v; memcpy(&v, (const uint8_t *)p + off, sizeof v); return v;
}
static uintptr_t chinese_ellipsis_pointer(const void *p, size_t off) {
    uintptr_t v; memcpy(&v, (const uint8_t *)p + off, sizeof v); return v;
}
static int chinese_ellipsis_has_glyph(void *cache) {
    /* The selector sets the face on this thread's ScalableFontEngine at
       cache+0x10 + worker*0xb8. Its optional worker callback takes no arguments
       (also called after the selector at main+0x10be1c8). */
    unsigned worker = 0;
    if (((const uint8_t *)cache)[0x1bd1]) {
        unsigned (*current_worker)(void) = (void *)chinese_ellipsis_pointer(cache, 0x1bd8);
        if (!current_worker) return 0;
        worker = current_worker();
        if (worker >= chinese_ellipsis_u32(cache, 0x1bd4)) return 0;
    }
    uint8_t *engines = (void *)chinese_ellipsis_pointer(cache, 0x10);
    return engines && (chinese_ellipsis_exists(engines + (size_t)worker * 0xb8, 0x2026) & 1);
}
static int chinese_ellipsis_select(void *cache, uint32_t *face, unsigned font, unsigned cp) {
    if (cp == 0x2026) {
        uint32_t text_face;
        int found = chinese_ellipsis_original(cache, &text_face, font, 0x4e2d);
        if ((found & 1) && chinese_ellipsis_has_glyph(cache)) {
            *face = text_face;
            return found;
        }
    }
    return chinese_ellipsis_original(cache, face, font, cp);
}

static uint32_t chinese_ellipsis_branch(size_t pc, size_t target, int link) {
    int64_t delta = (int64_t)target - (int64_t)pc;
    if ((delta & 3) || delta < -(1 << 27) || delta >= (1 << 27))
        runtime_fail("Chinese ellipsis: branch outside range");
    return (link ? 0x94000000u : 0x14000000u) | ((uint32_t)(delta >> 2) & 0x03ffffffu);
}
static void chinese_ellipsis_install(Module *m) {
    const char *enabled = getenv("TOTK_CHINESE_ELLIPSIS");
    if ((enabled && !strcmp(enabled, "0")) || getenv("TOTK_EXPORT_IOS")) return;
    uint64_t (*language)(void) = runtime_override("_ZN2nn2oe18GetDesiredLanguageEv");
    if (!language) runtime_fail("Chinese ellipsis: language service missing");
    uint64_t code = language();
    char name[8]; memcpy(name, &code, sizeof name);
    if (name[0] != 'z' || name[1] != 'h' || (name[2] && name[2] != '-')) return;

    static const struct { size_t off; uint32_t word; } calls[] = {
        {0x1d8ddc, 0x943b9339}, {0x1d8e30, 0x943b9324}, {0x1d8e9c, 0x943b9309},
        {0x10bdfd4, 0x97fffebb}, {0x10bdfec, 0x97fffeb5}, {0x10be1b0, 0x97fffe44},
        {0x10be3d8, 0x97fffdba}, {0x10be3f0, 0x97fffdb4},
        {0x10be448, 0x97fffd9e}, {0x10be498, 0x97fffd8a},
    };
    /* Guard the cache layout and the selected-engine / CheckGlyphExist path,
       in addition to the callers, before using the offsets above. */
    static const uint32_t entry[] = {
        0xa9ba7bfd, 0xf9000bfb, 0xa90267fa, 0xa9035ff8, 0xa90457f6, 0xa9054ff4,
        0x910003fd, 0x8b224008, 0x52831109, 0xaa0103f3, 0x8b090117, 0x394002e8,
        0x34000848, 0x8b224c09, 0x52837a2a, 0x2a0303f4, 0xaa0003f5, 0xaa1f03f8,
        0xf94c5539, 0x8b0a001a, 0x5280171b,
    };
    static const uint32_t engine[] = {
        0x14000002, 0x2a1f03f6, 0xaa1503e0, 0x2a1603e2, 0x940001ba,
        0xf9400aa8, 0x2a1403e1, 0x9bbb22c0, 0x946be865, 0x37000100,
    };
    const size_t original = 0x10bdac0;
    size_t padding = (m->text_end + 15) & ~15ULL;
    size_t first = padding + PAD_V143_ELLIPSIS, last = padding + PAD_V143_ELLIPSIS_HOST;
    if (0x10bdc28 > m->text_end || first + 16 > m->ro_off || last + 16 > m->ro_off)
        runtime_fail("Chinese ellipsis: unexpected 1.4.3 layout");
    if (memcmp(m->base + original, entry, sizeof entry) ||
        memcmp(m->base + 0x10bdbbc, engine, sizeof engine))
        runtime_fail("Chinese ellipsis: unexpected 1.4.3 font selector");
    uint32_t jumps[sizeof calls / sizeof *calls];
    for (unsigned i = 0; i < sizeof calls / sizeof *calls; i++) {
        if (calls[i].off + 4 > m->text_end || chinese_ellipsis_u32(m->base, calls[i].off) != calls[i].word)
            runtime_fail("Chinese ellipsis: unexpected 1.4.3 font selection call");
        jumps[i] = chinese_ellipsis_branch(calls[i].off, first, 1);
    }
    for (unsigned i = 0; i < 16; i++)
        if (m->base[first + i] || m->base[last + i])
            runtime_fail("Chinese ellipsis: text padding occupied");
    chinese_ellipsis_original = (void *)(m->base + original);
    chinese_ellipsis_exists = runtime_symbol("_ZN2nn6fontll18ScalableFontEngine15CheckGlyphExistEj");
    if (!chinese_ellipsis_exists) runtime_fail("Chinese ellipsis: glyph availability function missing");

    int64_t delta = (int64_t)last - (int64_t)(first + 8);
    if ((delta & 3) || delta < -(1 << 20) || delta >= (1 << 20))
        runtime_fail("Chinese ellipsis: conditional branch outside range");
    uint32_t head[] = {
        0x528404c8,                 /* mov w8, #0x2026 */
        0x6b08007f,                 /* cmp w3, w8 */
        0x54000000u | (((uint32_t)(delta >> 2) & 0x7ffffu) << 5), /* b.eq host */
        chinese_ellipsis_branch(first + 12, original, 0),
    };
    uint32_t host[4] = {0x58000050, 0xd61f0200, 0, 0}; /* ldr x16, #8; br x16; address */
    _Static_assert(sizeof head == PAD_V143_ELLIPSIS_LEN, "1.4.3 padding: ellipsis dispatch size");
    _Static_assert(sizeof host == PAD_V143_ELLIPSIS_HOST_LEN, "1.4.3 padding: ellipsis host veneer size");
    void *hook = runtime_hle_wrap_named(chinese_ellipsis_select, "Chinese ellipsis font selection");
    memcpy(host + 2, &hook, sizeof hook);
    memcpy(m->base + first, head, sizeof head);
    memcpy(m->base + last, host, sizeof host);
    for (unsigned i = 0; i < sizeof calls / sizeof *calls; i++)
        memcpy(m->base + calls[i].off, jumps + i, sizeof *jumps);
    fprintf(stderr, "[totk] Chinese ellipsis: original U+2026 uses the Chinese text face when supported (1.4.3)\n");
}
#else
static void chinese_ellipsis_install(Module *m) { (void)m; }
#endif
