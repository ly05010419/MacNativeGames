#ifndef NVN_CAPTURE_REGION_H
#define NVN_CAPTURE_REGION_H
#include <stddef.h>
#include <stdint.h>

typedef struct { size_t x, y, w, h; } nvn_capture_region;

/* A crop belongs to its source texture, before presentation/VR transforms. */
static inline int nvn_capture_source_region(size_t width, size_t height, const int crop[4], nvn_capture_region *out) {
    nvn_capture_region r = {0, 0, width, height};
    if (crop) {
        if (crop[0] < 0 || crop[1] < 0 || crop[2] <= 0 || crop[3] <= 0) return 0;
        r = (nvn_capture_region){(size_t)crop[0], (size_t)crop[1], (size_t)crop[2], (size_t)crop[3]};
        if (r.x > width || r.y > height || r.w > width - r.x || r.h > height - r.y) return 0;
    }
    if (!r.w || !r.h || r.w > SIZE_MAX / 4 || r.h > SIZE_MAX / (r.w * 4)) return 0;
    *out = r;
    return 1;
}
#endif
