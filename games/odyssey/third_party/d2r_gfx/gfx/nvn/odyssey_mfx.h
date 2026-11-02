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
/* Display link mode (nvn_mtl.m link_sync): while 60+60 is on, the 60 Hz link (mode 1) gives way to the 120 Hz range
   (mode 2), so the real and the generated picture each get a refresh. Only the effective mode changes: the request
   (g_dm_present_link) is kept, so turning interpolation off returns to it. */
static inline int odyssey_link_mode(int requested, int mfx) {
    return requested == 1 && mfx == MFX_HALF ? 2 : requested;
}

/* The canvas counts as this frame's scene only once its FXAA draw was encoded. The draw is recognised before its
   pipeline is fetched, and a draw without a pipeline is skipped (none yet, or still compiling in the background,
   #503): then the canvas holds no FXAA, the frame has no scene input and shows the real picture. */
typedef struct { uint64_t pending; int ready; } odyssey_scene;
static inline void odyssey_scene_draw(odyssey_scene *s) { s->pending = 0; }   // a new draw: an earlier FXAA draw that was skipped does not count
static inline void odyssey_scene_fxaa(odyssey_scene *s, uint64_t canvas) { s->pending = canvas; }
static inline uint64_t odyssey_scene_drawn(odyssey_scene *s) {   // the draw was encoded: its canvas, or 0
    uint64_t c = s->pending; s->pending = 0; if (c) s->ready = 1; return c;
}
static inline void odyssey_scene_frame(odyssey_scene *s) { s->pending = 0; s->ready = 0; }

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
    /* Check the complete affine V * inverse(V), including translation. A model block can pass the
       orthonormal / near-far checks: slot 3 did so in Cap Kingdom, but its inverse error exceeded 18,000 cm.
       Double accumulation avoids adding cancellation error to the float uniforms. Rotation has a fixed
       tolerance; translation allows 0.05 cm plus float rounding proportional to the terms being cancelled. */
    for (int r = 0; r < 3; r++) for (int c = 0; c < 4; c++) {
        double v = c == 3 ? h[4*r+3] : 0, scale = fabs(v);
        for (int k = 0; k < 3; k++) {
            double term = (double)h[4*r+k] * h[12+4*k+c];
            v += term; scale += fabs(term);
        }
        double tolerance = c == 3 ? 0.05 + 1e-5 * scale : 1e-3;
        if (fabs(v - (r == c ? 1 : 0)) > tolerance) return 0;
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
