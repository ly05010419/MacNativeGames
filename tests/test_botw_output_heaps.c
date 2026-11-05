#include "../games/totk/runtime/botw_output_heaps.h"
#include "../zeldaVR/gfx/vr_resolution_policy.h"
#include <assert.h>
#include <stdio.h>

#if defined(__aarch64__)
static uint64_t backing_arg(uint64_t bytes) {
    uint64_t result;
    __asm__ volatile("mov x24, %1\n.inst 0x93407f03\nmov %0, x3" : "=r"(result) : "r"(bytes) : "x3", "x24");
    return result;
}
#endif

int main(void) {
    unsigned render, gfx, extra;
    botw160_output_heaps(2560, 1440, &render, &gfx, &extra);
    assert(render == 320 && gfx == 64 && extra == 224); /* established desktop budget */
    botw160_output_heaps(3840, 2160, &render, &gfx, &extra);
    assert(render == 912 && gfx == 112 && extra == 864);
    for (unsigned i = 0; i < VR_RENDER_SIZE_COUNT; i++) {
        unsigned w = vr_render_sizes[i][0] * 2, h = vr_render_sizes[i][1];
        botw160_output_heaps(w, h, &render, &gfx, &extra);
        uint64_t bytes = (uint64_t)render << 20;
        assert(bytes < INT32_MAX && !(render & 15)); /* valid original signed-size argument */
        assert(bytes > (uint64_t)w * h * 4);          /* full canvas, no resolution reduction */
        assert((uint64_t)extra << 20 >= bytes - (115ULL << 20) + ((uint64_t)gfx - 48) * (1ULL << 20));
#if defined(__aarch64__)
        assert(backing_arg(bytes) == bytes);
#endif
    }
    assert(render == 2032 && gfx == 400 && extra == 2272);
#if defined(__aarch64__)
    assert(backing_arg(4464ULL << 20) == 368ULL << 20); /* exact original highest-preset failure */
#endif
    puts("botw output heap bounds and native allocation argument ok");
}
