/* PLA: the shared font is served as the decoded BFTTF header + TTF; a caller that hands the header address to the
   SDK's font library must get the TTF payload. The reference nxnative build forces LoadFont for exactly this.
   The SDK's exported font loaders are thin thunks over an internal C library:
     ScalableFontEngine::LoadFont (sdk+0x167778): mov x5,x1 ; mov x1,xzr ; b <FSS_load_font PLT>
     FSS_load_font               (sdk+0x16e400): mov w6,wzr ; mov w7,wzr ; b <PLT sdk+0x575160>
   The C++ method only reaches FSS_load_font through the first thunk, but the SDK's own font/pl integration can
   call FSS_load_font directly, so that is the entry to hook: every caller passes through it. This stub keeps
   x0..x5, zeroes x6/x7 like the thunk it replaces, strips the shared font's 8-byte header and continues at the
   original PLT. The sdk build is pinned (assets/expected-v1.1.1.json); the thunk shape is verified. The modules
   are writable until seal(), which runs after this install. */
#include "runtime.h"
#include <libkern/OSCacheControl.h>
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

extern const void *runtime_shared_font_region(void);

#define SDK_FSS_LOAD_FONT 0x16e400ULL
#define SDK_FSS_PLT       0x575160ULL

/* The stub runs on the calling guest thread's stack (a br from guest code, not through the HLE stack switch),
   and the font init threads have small stacks: a C frame hung the boot. A naked leaf keeps the caller's stack
   and return address untouched — it only adjusts x2 and continues at the original PLT with w6/w7 zeroed, which
   is exactly what the replaced thunk did. The globals are plain symbols so the asm can address them. */
void *runtime_font_real_ptr;
const void *runtime_font_region_ptr;
__attribute__((naked)) void runtime_font_load_stub(void) {
    __asm__ volatile(
        "adrp x16, _runtime_font_region_ptr@PAGE\n"
        "ldr x16, [x16, _runtime_font_region_ptr@PAGEOFF]\n"
        "cbz x16, 1f\n"
        "cmp x16, x2\n"
        "b.ne 1f\n"
        "add x2, x2, #8\n"
        "1:\n"
        "adrp x16, _runtime_font_real_ptr@PAGE\n"
        "ldr x16, [x16, _runtime_font_real_ptr@PAGEOFF]\n"
        "mov w6, wzr\n"
        "mov w7, wzr\n"
        "br x16\n");
}

static void emit_adrp_br(uint32_t *at, uintptr_t pc, const void *target) {
    uintptr_t t = (uintptr_t)target;
    int64_t imm = ((int64_t)(t & ~0xfffULL) - (int64_t)(pc & ~0xfffULL)) >> 12;
    at[0] = 0x90000010u | ((uint32_t)(imm & 3) << 29) | (((uint32_t)(imm >> 2) & 0x7ffff) << 5);   /* adrp x16, target page */
    at[1] = 0x91000210u | ((uint32_t)(t & 0xfff) << 10);                                           /* add x16, x16, #target & 0xfff */
    at[2] = 0xd61f0200u;                                                                          /* br x16 */
}

void runtime_font_hook_set_region(const void *p) { runtime_font_region_ptr = p; }   /* called by the pl:u address override */
void runtime_font_hook_install(void) {
    static int done;
    if (done) return;
    /* Compatibility fix, on by default: the shared fonts are stand-in TTFs behind the decoded BFTTF header, and
       the game's font path hands that header to FSS_load_font; the real loader rejects it and every CJK glyph
       lookup returns NULL (blank text everywhere except baked UI). PLA_FONT_LOAD_HOOK=0 disables the hook. */
    { const char *e = getenv("PLA_FONT_LOAD_HOOK"); if (e && !strcmp(e, "0")) return; }
    done = 1;
    uint8_t *fn = (uint8_t *)runtime_symbol("FSS_load_font");
    if (!fn) { fprintf(stderr, "[font] FSS hook: symbol not found\n"); return; }
    uint32_t w0 = *(uint32_t *)fn, w1 = *(uint32_t *)(fn + 4), w2 = *(uint32_t *)(fn + 8);
    if (w0 != 0x2a1f03e6u || w1 != 0x2a1f03e7u || (w2 & 0xfc000000u) != 0x14000000u) {
        fprintf(stderr, "[font] FSS hook: thunk shape changed (%#x %#x %#x)\n", w0, w1, w2); return; }
    uint8_t *base = fn - SDK_FSS_LOAD_FONT;
    runtime_font_real_ptr = (void *)(base + SDK_FSS_PLT);
    runtime_font_region_ptr = NULL;   /* set by the pl:u address override when the game asks for the shared font */

    uint32_t patch[3] = {0, 0, 0};
    emit_adrp_br(patch, (uintptr_t)fn, (const void *)runtime_font_load_stub);
    /* Installed before seal(): the module text is still writable; seal() then makes it RX and flushes the cache. */
    memcpy(fn, patch, 12);
    sys_icache_invalidate(fn, 12);
    fprintf(stderr, "[font] FSS_load_font hook installed: %p -> %p, PLT %p (lazy shared font)\n",
            (void *)fn, (void *)runtime_font_load_stub, (void *)(base + SDK_FSS_PLT));
}
