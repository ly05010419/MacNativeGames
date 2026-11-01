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
/* Only the Mac frontend built for TOTK 1.4.3 has verified this compose hash and
 * source layout. Shared graphics objects must ask it: they serve other versions too. */
#if defined(__APPLE__) && TARGET_OS_OSX && !defined(GAME_BOTW) && !defined(TOTK_HDR_FRONTEND)
extern int front_hdr_supported(void) __attribute__((weak_import));
extern int front_hdr_enabled(void) __attribute__((weak_import));
#endif
static inline int totk_hdr_available(void) {
#if defined(__APPLE__) && TARGET_OS_OSX && !defined(GAME_BOTW)
#ifdef TOTK_HDR_FRONTEND
#if defined(TOTK_GAME_V143)
    static _Atomic int cached=-1;int v=atomic_load(&cached);
    if(v<0){const char *e=getenv("TOTK_HDR_CONTROL");v=!(e && !strcmp(e,"0"));atomic_store(&cached,v);}return v;
#else
    return 0;
#endif
#else
    return front_hdr_supported && front_hdr_supported();
#endif
#else
    return 0;
#endif
}
#ifndef TOTK_HDR_FRONTEND
static inline int totk_hdr_active(void) {
#if defined(__APPLE__) && TARGET_OS_OSX && !defined(GAME_BOTW)
    return totk_hdr_available() && front_hdr_enabled && front_hdr_enabled();
#else
    return 0;
#endif
}
#endif
static inline float totk_hdr_rolloff(float peak, float headroom) {
    if (!isfinite(peak) || peak < 0) return 0;
    if (!isfinite(headroom) || headroom < 1) headroom = 1;
    if (headroom <= 1) return fminf(peak,1);
    float knee = fmaxf(1,headroom * 0.75f);
    return peak <= knee ? peak : knee + (headroom-knee) * -expm1f(-(peak-knee)/(headroom-knee));
}
