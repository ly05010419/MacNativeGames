/* Experimental Mac HDR. The game is SDR-authored: keep its toe, midtones and UI;
 * extend highlights from the exposed, pre-tonemap scene. Values are relative to
 * SDR reference white, not a claim of Nintendo-authored absolute luminance. */
#pragma once
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
static inline int totk_hdr_requested(void) {
#if defined(__APPLE__) && TARGET_OS_OSX && !defined(GAME_BOTW)
    static _Atomic int cached = -1;
    int v = atomic_load(&cached);
    if (v < 0) { const char *e = getenv("TOTK_HDR"); v = e && !strcmp(e,"1"); atomic_store(&cached,v); }
    return v;
#else
    return 0;
#endif
}
/* Capability stays on across live mode changes, keeping shader/cache formats stable. */
static inline int totk_hdr_available(void) {
#if defined(__APPLE__) && TARGET_OS_OSX && !defined(GAME_BOTW)
    static _Atomic int cached = -1;
    int v=atomic_load(&cached);
    if(v<0){const char *e=getenv("TOTK_HDR_CONTROL");v=totk_hdr_requested() || (e && !strcmp(e,"1"));atomic_store(&cached,v);}
    return v;
#else
    return 0;
#endif
}
static inline float totk_hdr_rolloff(float peak, float headroom) {
    if (!isfinite(peak) || peak < 0) return 0;
    if (!isfinite(headroom) || headroom < 1) headroom = 1;
    if (headroom <= 1) return fminf(peak,1);
    float knee = fmaxf(1,headroom * 0.75f);
    return peak <= knee ? peak : knee + (headroom-knee) * -expm1f(-(peak-knee)/(headroom-knee));
}
