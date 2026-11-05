/* Bound presentation work on Retina/full-screen displays. This scales only the
   Metal drawable; it does not alter guest render targets, camera or game ticks. */
#pragma once
#include <math.h>
static inline void xcx_drawable_size(double *width, double *height) {
    double w = *width, h = *height;
    if (!isfinite(w) || !isfinite(h) || w < 1 || h < 1) { *width = *height = 0; return; }
    double k = fmin(1, fmin(1920 / w, 1080 / h));
    *width = fmax(1, floor(w * k)); *height = fmax(1, floor(h * k));
}
