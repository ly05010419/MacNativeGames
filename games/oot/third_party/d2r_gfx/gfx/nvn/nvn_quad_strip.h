/* NVN quad strips: each successive pair completes (a,b,d,c). Metal has no quads.
   Both triangles keep d as their last vertex, preserving flat last-vertex inputs. */
#ifndef NVN_QUAD_STRIP_H
#define NVN_QUAD_STRIP_H
#include <stdint.h>
#include <string.h>

static inline uint32_t nvn_quad_strip_quads(uint32_t vertices) {
    return vertices >= 4 ? vertices / 2 - 1 : 0;
}

static inline void nvn_quad_strip_indices(uint32_t *indices, uint32_t quads) {
    for (uint32_t q = 0; q < quads; q++) {
        uint32_t a = q * 2;
        indices[q * 6 + 0] = a;
        indices[q * 6 + 1] = a + 1;
        indices[q * 6 + 2] = a + 3;
        indices[q * 6 + 3] = a + 2;
        indices[q * 6 + 4] = a;
        indices[q * 6 + 5] = a + 3;
    }
}

/* Expand host-visible 8/16/32-bit indices, splitting at the type's restart index.
   An odd tail is discarded independently in each strip, rather than joining it to the next strip. */
static inline size_t nvn_quad_strip_indexed(uint32_t *out, const void *input,
                                             uint32_t count, unsigned width) {
    const uint8_t *bytes = input;
    uint32_t pair[4], used = 0;
    size_t written = 0;
    uint32_t restart = width == 1 ? UINT8_MAX : width == 2 ? UINT16_MAX : UINT32_MAX;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t value = 0;
        memcpy(&value, bytes + (size_t)i * width, width);
        if (value == restart) { used = 0; continue; }
        pair[used++] = value;
        if (used != 4) continue;
        out[written++] = pair[0]; out[written++] = pair[1]; out[written++] = pair[3];
        out[written++] = pair[2]; out[written++] = pair[0]; out[written++] = pair[3];
        pair[0] = pair[2]; pair[1] = pair[3]; used = 2;
    }
    return written;
}
#endif
