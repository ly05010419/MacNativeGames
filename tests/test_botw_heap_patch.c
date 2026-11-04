#include "../games/totk/runtime/botw_heap_patch.h"
#include <assert.h>
#include <stdio.h>

static uint8_t image[0x3000], before[sizeof image];
static uint32_t word(unsigned at) { uint32_t w; memcpy(&w, image + at, 4); return w; }
static int64_t target(unsigned at) {
    int32_t imm = (int32_t)(word(at) << 6) >> 6;
    return at + (int64_t)imm * 4;
}
static uint64_t value(unsigned at, unsigned reg) {
    assert((word(at) & 0xffe0001f) == (0xd2a00000u | reg));
    assert((word(at + 4) & 0xffe0001f) == (0xf2c00000u | reg));
    return (uint64_t)((word(at) >> 5) & 0xffff) << 16 |
           (uint64_t)((word(at + 4) >> 5) & 0xffff) << 32;
}
int main(void) {
    const uint32_t original = 0x52b86000;
    const unsigned end = 0x1111, stub = 0x1220;
    for (unsigned reg = 0; reg <= 21; reg += 7) {
        for (uint64_t mib = 4096; mib <= 6144; mib += 32) {
            memset(image, 0, sizeof image);
            memcpy(image + 0x100, &original, 4);
            assert(botw_heap_patch(image, sizeof image, end, 0x100, original, reg, 0, mib << 20));
            assert(target(0x100) == stub && target(stub + 8) == 0x104);
            assert(value(stub, reg) == mib << 20); /* no 32-bit truncation */
        }
    }
    memset(image, 0, sizeof image);
    memcpy(image + 0x100, &original, 4);
    memcpy(image + 0x120, &original, 4);
    assert(botw_heap_patch(image, sizeof image, end, 0x100, original, 0, 0, 4880ULL << 20));
    assert(botw_heap_patch(image, sizeof image, end, 0x120, original, 21, 1, 4848ULL << 20));
    assert(value(stub, 0) == 4880ULL << 20 && value(stub + 16, 21) == 4848ULL << 20);
    for (unsigned bad = 0; bad < 6; bad++) {
        memset(image, 0, sizeof image);
        memcpy(image + 0x100, &original, 4);
        if (bad == 0) image[0x100] ^= 1; /* wrong game instruction */
        if (bad == 1) image[stub] = 1;   /* another veneer */
        memcpy(before, image, sizeof image);
        assert(!botw_heap_patch(image, bad == 2 ? stub : sizeof image, end,
                               0x100, original, bad == 3 ? 31 : 0, bad == 4 ? 2 : 0,
                               bad == 5 ? (4880ULL << 20) + 1 : 4880ULL << 20));
        assert(!memcmp(before, image, sizeof image)); /* failed checks never write */
    }
    puts("botw 64-bit heap patch ok");
}
