/* Odyssey 1.3.0 inputs for optional host-side frame interpolation.
   The game advances once per frame: extra pictures must never change its 60 Hz clock. */
#pragma once
#include "totk_mfx_mode.h"
#include <math.h>
#include <stdint.h>
#include <string.h>

static inline int odyssey_mfx_mode(int requested, int hz) {
    return requested && hz == 120 ? MFX_HALF : MFX_OFF;
}
static inline uint64_t odyssey_frame_period(void) { return 1000000000ull / 60; }

/* Vertex camera uniform: row-major view +0, inverse view +0x30, VP +0x60,
   near/far +0x160. Material shaders bind it at different uniform slots.
   Recover projection row lengths from VP = P*V; V is orthonormal. */
static inline int odyssey_mfx_camera(const float *h, float cam[84]) {
    for (int i = 0; i < 0x180 / 4; i++) if (!isfinite(h[i])) return 0;
    for (int i = 0; i < 3; i++) {
        const float *a = h + 4 * i;
        float n = a[0]*a[0] + a[1]*a[1] + a[2]*a[2];
        if (fabsf(n - 1) > 0.1f) return 0;
        for (int j = 0; j < i; j++) {
            const float *b = h + 4 * j;
            if (fabsf(a[0]*b[0] + a[1]*b[1] + a[2]*b[2]) > 0.05f) return 0;
        }
    }
    if (!(h[0x160/4] > 0 && h[0x164/4] > h[0x160/4])) return 0;
    float x = sqrtf(h[24]*h[24] + h[25]*h[25] + h[26]*h[26]);
    float y = sqrtf(h[28]*h[28] + h[29]*h[29] + h[30]*h[30]);
    if (!(isfinite(x) && isfinite(y) && x > 1e-4f && y > 1e-4f)) return 0;
    memset(cam, 0, 84 * sizeof *cam);
    memcpy(cam, h, 12 * sizeof *cam);
    memcpy(cam + 12, h + 24, 16 * sizeof *cam);
    cam[47] = h[15]; cam[51] = h[19]; cam[55] = h[23];
    cam[56] = h[88]; cam[57] = h[89];
    cam[62] = y / x; cam[70] = 2 * atanf(1 / y);
    return 1;
}
static inline int odyssey_mfx_cut(const double a[84], const double b[84]) {
    double x = a[47]-b[47], y = a[51]-b[51], z = a[55]-b[55];
    double f = a[8]*b[8] + a[9]*b[9] + a[10]*b[10];
    /* Odyssey positions are centimetres: 8 was ordinary camera follow motion. */
    return !isfinite(x+y+z+f) || sqrt(x*x+y*y+z*z) > 800 || f < cos(25.0 * 3.141592653589793 / 180);
}
static inline int odyssey_mfx_fxaa(const char *key) {
    return key && !strncmp(key, "5345f3231650f0b7", 16);
}
