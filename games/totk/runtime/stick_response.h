#ifndef TOTK_STICK_RESPONSE_H
#define TOTK_STICK_RESPONSE_H
#include <math.h>
#include <stdint.h>
/* Calibrated axes in [-1,1], circular output with an angle-preserving radial
   deadzone. The raw HID/GameController values are never overwritten. */
static inline float stick_response_gain(float x, float y, float deadzone) {
    float radius = hypotf(x, y);
    if (!isfinite(radius) || !isfinite(deadzone) || deadzone < 0 || deadzone >= 1 || radius <= deadzone) return 0;
    return (fminf(radius, 1.0f) - deadzone) / ((1.0f - deadzone) * radius);
}
/* Per-axis source merging may combine two different controllers' full axes. */
static inline void stick_response_limit(int32_t *x, int32_t *y) {
    float radius = hypotf((float)*x, (float)*y);
    if (radius > 32767.0f) {
        *x = (int32_t)(*x * (32767.0f / radius));
        *y = (int32_t)(*y * (32767.0f / radius));
    }
}
#endif
