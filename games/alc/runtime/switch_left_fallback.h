/* Some Switch-compatible controllers send live 12-bit stick reports while
   GameController leaves the left stick at zero. Use raw input only after a
   sustained discrepancy, and abandon it as soon as native input works. */
#pragma once
#include <math.h>
#include <stdint.h>

typedef struct {
    int x, y, valid, active;
    uint64_t at, zeroSince;
} SwitchLeftFallback;

static inline void switch_left_feed(SwitchLeftFallback *s, int x, int y, uint64_t now) {
    s->valid = x >= 0 && x <= 4095 && y >= 0 && y <= 4095;
    s->x = x; s->y = y; s->at = now;
}

static inline float switch_left_axis(int raw) {
    int delta = raw - 2048;
    return delta / (delta < 0 ? 2048.0f : 2047.0f);
}

static inline int switch_left_get(SwitchLeftFallback *s, float nativeX, float nativeY,
                                  uint64_t now, float *x, float *y) {
    if (!s->valid || now < s->at || now - s->at > 250000000ULL) {
        s->active = 0; s->zeroSince = 0; return 0;
    }
    if (fabsf(nativeX) > 0.0001f || fabsf(nativeY) > 0.0001f) {
        s->active = 0; s->zeroSince = 0; return 0;
    }
    *x = switch_left_axis(s->x); *y = switch_left_axis(s->y);
    if (fmaxf(fabsf(*x), fabsf(*y)) > 0.5f) {
        if (!s->zeroSince) s->zeroSince = now;
        if (now - s->zeroSince >= 200000000ULL) s->active = 1;
    } else s->zeroSince = 0;
    return s->active;
}
