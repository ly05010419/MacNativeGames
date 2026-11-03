// Expand indexed fans/quads into triangle lists, respecting NVN primitive restart.
#pragma once
#include <stdint.h>
#include <string.h>
static inline uint32_t nvn_expand_primitive(uint32_t *out, const uint8_t *src, uint32_t count,
                                           unsigned bytes, unsigned primitive, int restart, uint32_t restart_index) {
    uint32_t used = 0, have = 0, first = 0, previous = 0, quad[4];
    for (uint32_t i = 0; i < count; i++) {
        uint32_t index = 0;
        if (bytes == 1) index = src[i];
        else if (bytes == 2) { uint16_t value; memcpy(&value, src + (size_t)i * 2, 2); index = value; }
        else memcpy(&index, src + (size_t)i * 4, 4);
        if (restart && index == restart_index) { have = 0; continue; }
        if (primitive == 6) {
            if (!have) first = index;
            else if (have >= 2) { out[used++] = first; out[used++] = previous; out[used++] = index; }
            previous = index;
            if (have < 2) have++;
        } else {
            quad[have++] = index;
            if (have == 4) {
                out[used++] = quad[0]; out[used++] = quad[1]; out[used++] = quad[2];
                out[used++] = quad[0]; out[used++] = quad[2]; out[used++] = quad[3];
                have = 0;
            }
        }
    }
    return used;
}
