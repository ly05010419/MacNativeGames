/* The 1.4.3 pouch player's perspective view belongs to the 16:9 UI layout.
   Its aspect already follows the picture, but its lens shift stays at the
   original canvas position. Keep the whole preview in the UI's coordinate
   frame, before the native view copy derives its matrices and camera block. */
#pragma once
#include <math.h>

/* near, far, fovy, sin/cos/tan(fovy/2), aspect, shift x/y: PerspectiveProjection
   +0x98..+0xb8. The captured pouch profile is near 1, far 10, fovy 30 degrees,
   shift y -0.005. The horizontal shift remains game-owned (rotation/layout).
   Only this captured profile is adjusted; other camera profiles pass through.
   This is a parameter filter, not a scene-ownership check. */
static inline int pouch_projection_adjust(float p[9], float aspect) {
    if (!isfinite(aspect) || aspect < 0.5f || aspect > 8.0f || fabsf(aspect / (16.0f / 9.0f) - 1.0f) < 0.001f) return 0;
    for (unsigned i = 0; i < 9; i++) if (!isfinite(p[i])) return 0;
    if (fabsf(p[0] - 1.0f) > 1e-5f || fabsf(p[1] - 10.0f) > 1e-5f ||
        fabsf(p[2] - 0.5235987756f) > 1e-5f || fabsf(p[8] + 0.005f) > 1e-6f ||
        fabsf(p[7]) > 0.5f || fabsf(p[6] - aspect) > 1e-4f) return 0;
    float f = aspect / (16.0f / 9.0f);
    if (f > 1.0f) {
        p[7] /= f;
    } else {
        /* Vert+ only for this UI preview: its x projection stays as at 16:9,
           and both its y projection and lens shift follow the flattened UI. */
        float half = atanf(tanf(p[2] * 0.5f) / f);
        p[2] = half * 2.0f;
        p[3] = sinf(half);
        p[4] = cosf(half);
        p[5] = tanf(half);
        p[8] *= f;
    }
    return 1;
}
