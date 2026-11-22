/* libnx's virtual memory manager (virtmem.c, _memregionFindRandom) picks the place of every mapping it makes -- thread
   stacks (svcMapMemory), shared memory (hid), and the JIT's two code-memory views (svcControlCodeMemory) -- as a random
   4 KiB page. On Apple silicon a mapping can only alias other memory at 16 KiB granularity, and the JIT's writable and
   executable views of the same code must share physical pages at the same offset within a host page. So the random
   page index is scaled to 16 KiB pages: the bound is computed in 16 KiB pages (lsr #14) and the index scaled by 16 KiB
   (lsl #14). Every address libnx then chooses is 16 KiB aligned; sizes stay whole 4 KiB pages, as on Horizon.
   The port's present decision (main+0xa6e0, called from its main loop between emulation slices) shows a new picture only
   when its DirectDraw frame counter (ctx+0x150, moved by Flip) changed. The menus after the title draw through GDI
   (SetDIBitsToDevice in ten 800x60 strips, MJ2013.exe 0x41a073) and never move it: every input on the title then left the
   title on screen while the game ran on in its menu (CHANGELOG, 2026-11-22). With the branch gone, the counter only no
   longer gates it: a picture is shown whenever 16.6 ms passed since the last one, which the title's own 30 fps keeps.
   The words were read from this game's main (build in assets/expected.json) at these offsets. */
#include "game_patches.h"
#include "runtime.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

const GamePatch runtime_game_patch_list[] = {
    {0x95e5c, 0xd34cfe94u, 0xd34efe94u, "virtmem random finder: lsr x20, x20, #12 -> #14 (free range in 16 KiB pages)"},
    {0x95ea0, 0x8b133013u, 0x8b133813u, "virtmem random finder: add x19, x0, x19, lsl #12 -> lsl #14 (16 KiB aligned address)"},
    {0xa7d0, 0x540000e0u, 0xd503201fu, "present decision: a frame is shown every 16.6 ms even when the DirectDraw frame counter did not move (b.eq -> nop)"},
};
const unsigned runtime_game_patch_count = sizeof runtime_game_patch_list / sizeof *runtime_game_patch_list;

void runtime_game_patches(uint8_t *base, uint64_t text_end) {
    for (unsigned i = 0; i < runtime_game_patch_count; i++) {
        const GamePatch *p = &runtime_game_patch_list[i]; uint32_t w;
        if (p->offset + 4 > text_end) runtime_fail("game patch outside the text");
        memcpy(&w, base + p->offset, 4);
        if (w != p->original) { fprintf(stderr, "[totk] game patch at main+%#llx: found %08x, expected %08x\n", (unsigned long long)p->offset, w, p->original); runtime_fail("game patch: different build"); }
    }
    for (unsigned i = 0; i < runtime_game_patch_count; i++) {
        const GamePatch *p = &runtime_game_patch_list[i];
        memcpy(base + p->offset, &p->replacement, 4);
        fprintf(stderr, "[totk] game patch main+%#llx: %s\n", (unsigned long long)p->offset, p->why);
    }
}
