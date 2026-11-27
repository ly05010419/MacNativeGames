#pragma once
/* Argument IDs name slots, not binding records. No allocation and no remapping:
   retain the translator's ID ranges, detect overlap/overflow, and require dense
   ranges before using raw 8-byte Metal resource IDs (Metal can compact holes). */
#include <stddef.h>
#include <stdint.h>

typedef struct { uint32_t first, count; } nvn_arg_span;
typedef struct { uint32_t extent; size_t bytes; int dense; } nvn_arg_layout;

static inline int nvn_arg_layout_plan(const nvn_arg_span *spans, size_t n,
                                      nvn_arg_layout *out) {
    if (!out || (n && !spans)) return 0;
    *out = (nvn_arg_layout){0};
    uint64_t occupied = 0, extent = 0;
    for (size_t i = 0; i < n; i++) {
        uint64_t end = (uint64_t)spans[i].first + spans[i].count;
        if (!spans[i].count || end > UINT32_MAX) return 0;
        for (size_t j = 0; j < i; j++) {
            uint64_t other = (uint64_t)spans[j].first + spans[j].count;
            if (spans[i].first < other && spans[j].first < end) return 0;
        }
        occupied += spans[i].count;
        if (end > extent) extent = end;
    }
    if (extent > SIZE_MAX / sizeof(uint64_t)) return 0;
    *out = (nvn_arg_layout){(uint32_t)extent, (size_t)extent * sizeof(uint64_t), occupied == extent};
    return 1;
}
static inline int nvn_arg_layout_raw_compatible(const nvn_arg_layout *layout,
                                                size_t encoded_length) {
    return layout && layout->dense && layout->bytes == encoded_length;
}

/* Size each arena for its request. In particular, real pool-array blocks can
   exceed the ordinary 4 MiB block; never return a smaller allocation or truncate.
   A device limit failure is explicit and leaves both outputs zero. */
static inline int nvn_arg_arena_size(size_t bytes, size_t device_limit,
                                     size_t *aligned, size_t *capacity) {
    if (!aligned || !capacity) return 0;
    *aligned = *capacity = 0;
    if (bytes > SIZE_MAX - 255) return 0;
    size_t n = (bytes + 255) & ~(size_t)255;
    size_t cap = n > (4u << 20) ? n : (4u << 20);
    if (cap > device_limit) return 0;
    *aligned = n; *capacity = cap;
    return 1;
}
