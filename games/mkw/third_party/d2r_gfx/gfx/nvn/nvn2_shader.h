#pragma once
#include <stdint.h>
#include <string.h>

/* NVN2 v0xc9 layout, as decoded by nxnative (MIT), commit 65501fbb. */
static inline uint32_t nvn2_word(const void *p, unsigned offset) {
    uint32_t v; memcpy(&v, (const uint8_t *)p + offset, sizeof v); return v;
}
static inline int nvn2_control(const void *p) {
    return p && nvn2_word(p, 0) == 0x98761234u && nvn2_word(p, 4) == 0xc9;
}
static inline int nvn2_stage(const uint8_t *ctl, const uint8_t *code) {
    if (ctl[0x56] == 5) return 5;
    if (ctl[0x56] == 6 || ctl[0x56] == 7) return ctl[0x56];
    unsigned type = (nvn2_word(code, 0) >> 10) & 15;
    /* NVN ordering differs from SPH ordering for pixel/geometry/tessellation. */
    static const int stage[] = {-1, 0, 3, 4, 2, 1};
    return type < sizeof stage / sizeof *stage ? stage[type] : -1;
}
static inline int nvn2_lengths(const uint8_t *ctl, uint32_t *inst, uint32_t *c1, uint32_t *total) {
    *inst = nvn2_word(ctl, 0x38); *c1 = nvn2_word(ctl, 0x40); *total = nvn2_word(ctl, 0x44);
    return *inst && *inst <= *c1 && *c1 <= *total && *total <= (16u << 20);
}
