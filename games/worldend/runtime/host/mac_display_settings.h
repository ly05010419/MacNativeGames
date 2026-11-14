// Output sizing only: preserves the view's aspect ratio and never changes guest render targets.
#pragma once
#include <math.h>
#include <stdint.h>
typedef struct { uint32_t width, height; } DmOutputSize;
static inline DmOutputSize dm_output_size(uint32_t width, uint32_t height, int preset) {
    static const uint32_t heights[] = {0, 720, 1080, 1440, 2160};
    if (!width || !height) return (DmOutputSize){1, 1};
    if (preset <= 0 || preset >= 5) return (DmOutputSize){width, height};
    double h = heights[preset], w = h * 16.0 / 9.0;
    double scale = fmin(1.0, fmin(w / width, h / height));
    return (DmOutputSize){(uint32_t)fmax(1, floor(width * scale)), (uint32_t)fmax(1, floor(height * scale))};
}
