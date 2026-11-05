/* TOTK_PCHTXT as the runtime applies it (runtime/pchtxt.h through patch_pchtxt in game_patches.c, or in
   game_patches_botw.c when built with -DGAME_BOTW190 or -DGAME_BOTW160, the production code) on a synthetic main: prints the 16 bytes at 0x40 after the patch, or
   "FAIL <reason>" with exit code 3 when the start would stop.
   Run from a folder holding extracted/v0/exefs/main (extracted/botw190/exefs/main for GAME_BOTW190), the build id at
   0x40, with TOTK_PCHTXT set. The synthetic main is 0x3000000 bytes, so it holds every Breath of the Wild site the reader
   refuses. */
#if defined(GAME_BOTW190) || defined(GAME_BOTW160)
#include "../games/totk/runtime/game_patches_botw.c"
#else
#include "../games/totk/runtime/game_patches.c"
#endif
#include <stdio.h>
#include <stdlib.h>

_Noreturn void runtime_fail(const char *message) { printf("FAIL %s\n", message); exit(3); }

int main(void) {
    enum { SIZE = 0x3000000 };
    uint8_t *image = calloc(1, SIZE);
    if (!image) return 2;
    Module m = {0};
    m.name = "main"; m.base = image; m.file_size = m.size = SIZE;
    patch_pchtxt(&m);
    for (int i = 0; i < 16; i++) printf("%02x", image[0x40 + i]);
    printf("\n");
    return 0;
}
