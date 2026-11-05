/* BotW 1.6.0's output heaps. The config stores a 64-bit capacity, but its
   original backing-size call sign-extends w24 (main+0x151c68c), and texture
   offsets are u32. Honor the original signed-size contract, with alignment
   room, instead of reserving an unusable arena above 2 GiB. This bounds the
   reservation; it never scales down render targets or texture data. */
#pragma once
#include <math.h>
#include <stdint.h>

enum { BOTW_RENDER_ARENA_MAX_MIB = (INT32_MAX >> 20) & ~15u };

static inline void botw160_output_heaps(unsigned w, unsigned h, unsigned *render_mib,
                                       unsigned *gfx_mib, unsigned *extra_mib) {
    double f = ((double)w * h - 1920.0 * 1080) / (2560.0 * 1440 - 1920.0 * 1080);
    if (f < 0) f = 0;
    double render = ceil((115 + 205 * f) / 16) * 16;
    *render_mib = render > BOTW_RENDER_ARENA_MAX_MIB ? BOTW_RENDER_ARENA_MAX_MIB : (unsigned)render;
    *gfx_mib = (unsigned)ceil((48 + 16 * f) / 16) * 16;
    *extra_mib = (unsigned)ceil((double)(*render_mib - 115 + *gfx_mib - 48) / 32) * 32;
}
