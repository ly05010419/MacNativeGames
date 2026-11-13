#pragma once
#include "runtime.h"
#include <string.h>

/* The NLG limiter is specific to this complete 1.4.0 main build. These are
   data/consumer guards, independent of the generic instruction rewriter. */
enum { LM3_FRAME_MS_OFFSET = 0x13c94dc };
static const uint8_t lm3_frame_build_id[32] = {
    0x79,0xe5,0x95,0x0f,0xfa,0x85,0xac,0xf6,0x3e,0x28,0xc9,0xae,0xc0,0x51,0xea,0xc2,
    0x7d,0x6f,0x7f,0x8d,0,0,0,0,0,0,0,0,0,0,0,0
};
static inline int lm3_frame_guard(const uint8_t *image, size_t size, const uint8_t build[32]) {
    static const struct { size_t offset; uint32_t word; } guards[] = {
        {0x7a3e9c, 0xd000612c}, /* adrp x12, 0x13c9000 */
        {0x7a3ea8, 0xbd44dd80},
        {0x7a3f18, 0xbd44dd82},
        {LM3_FRAME_MS_OFFSET, 0x42055555}
    };
    if (!image || !build || memcmp(build, lm3_frame_build_id, 32)) return 0;
    for (unsigned i = 0; i < sizeof guards / sizeof *guards; i++) {
        uint32_t word;
        if (guards[i].offset > size || size - guards[i].offset < sizeof word) return 0;
        memcpy(&word, image + guards[i].offset, sizeof word);
        if (word != guards[i].word) return 0;
    }
    return 1;
}
/* A host cap can lower the 30/60 startup target, never raise it. This covers
   high-refresh screens and cap=0 as well as the normal control-menu policy. */
static inline uint64_t lm3_frame_period_ns(int engine_fps, int vsync, int cap, int hz) {
    if (engine_fps != 30 && engine_fps != 60) return 0;
    int fps = engine_fps;
    if (cap > 0 && cap < fps) fps = cap;
    if (vsync && hz > 0 && hz < fps) fps = hz;
    return UINT64_C(1000000000) / (unsigned)fps;
}
static inline int lm3_frame_interval(uint64_t period) {
    uint64_t interval = (period * 60 + UINT64_C(500000000)) / UINT64_C(1000000000);
    return (int)(interval ? interval : 1);
}
int runtime_lm3_target_fps(void); /* 0: an older build, its original policy. */
void runtime_lm3_frame_patch(Module *module);
