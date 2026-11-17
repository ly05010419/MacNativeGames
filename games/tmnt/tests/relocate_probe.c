#include "relocate.h"
#include <assert.h>
#include <stdint.h>
#include <string.h>

static int resolve(void *context, uint32_t symbol, uintptr_t *out) {
    (void)context;
    if (symbol == 1) { *out = 0x1000; return 1; }
    if (symbol == 2) { *out = UINTPTR_MAX; return 1; }
    if (symbol == 3) return -1;
    return 0;
}
static uint64_t word(const uint8_t *p) { uint64_t v; memcpy(&v, p, sizeof(v)); return v; }
int main(void) {
    uint8_t image[160] = {0};
    TmntRelocation valid[] = {{32,1027,0,12,0}, {40,257,1,7,0}, {48,1025,1,-5,0},
                               {56,1026,1,0,0}, {65,1025,42,0,1}};
    assert(tmnt_relocate(image, sizeof image, 32, 128, valid, 5, resolve, 0) == TMNT_RELOC_OK);
    assert(word(image+32) == (uintptr_t)image+12 && word(image+40) == 0x1007);
    assert(word(image+48) == 0xffb && word(image+56) == 0x1000 && word(image+65) == 0);
    uint8_t before[sizeof image]; memcpy(before, image, sizeof image);
    TmntRelocation fail[] = {{32,1027,0,1,0}, {40,1026,42,0,0}};
    assert(tmnt_relocate(image, sizeof image, 32, 128, fail, 2, resolve, 0) == TMNT_RELOC_UNRESOLVED);
    assert(!memcmp(image, before, sizeof image));
    fail[1] = (TmntRelocation){39,1026,1,0,0};
    assert(tmnt_relocate(image, sizeof image, 32, 128, fail, 2, resolve, 0) == TMNT_RELOC_OVERLAP);
    fail[1] = (TmntRelocation){125,1026,1,0,0};
    assert(tmnt_relocate(image, sizeof image, 32, 128, fail, 2, resolve, 0) == TMNT_RELOC_BOUNDS);
    fail[1] = (TmntRelocation){40,1026,1,1,0};
    assert(tmnt_relocate(image, sizeof image, 32, 128, fail, 2, resolve, 0) == TMNT_RELOC_KIND);
    fail[1] = (TmntRelocation){40,257,2,1,0};
    assert(tmnt_relocate(image, sizeof image, 32, 128, fail, 2, resolve, 0) == TMNT_RELOC_OVERFLOW);
    fail[1] = (TmntRelocation){40,1025,3,0,1};
    assert(tmnt_relocate(image, sizeof image, 32, 128, fail, 2, resolve, 0) == TMNT_RELOC_RESOLUTION);
    assert(!memcmp(image, before, sizeof image));
    return 0;
}
