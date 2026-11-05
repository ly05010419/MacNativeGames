/* Scene-referred Mac HDR, ported from the RenoDX rendering approach. Apple EDR
 * is relative to the current system SDR white; it is not Windows' 80-nit scRGB.
 * This is a new tone map of the game's lighting, not an HDR-authored game master. */
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
/* Parameters follow RenoDX's scene/graphics whites and grading separation.
 * Apple EDR uses relative SDR-white units; peak 0 selects the current display.
 * Keep the host and Metal uniform layout at four groups of four floats. */
enum {
    TOTK_HDR_PEAK, TOTK_HDR_GAME_WHITE, TOTK_HDR_UI_WHITE, TOTK_HDR_EXPOSURE,
    TOTK_HDR_HIGHLIGHTS, TOTK_HDR_SHADOWS, TOTK_HDR_CONTRAST, TOTK_HDR_SATURATION,
    TOTK_HDR_BLOOM, TOTK_HDR_SDR_PIVOT, TOTK_HDR_HUE, TOTK_HDR_PURITY,
    TOTK_HDR_VANILLA_SATURATION, TOTK_HDR_PARAMETER_COUNT
};
typedef struct { const char *file; float minimum, maximum, initial; } TotkHDRParameter;
static const TotkHDRParameter totk_hdr_parameters[TOTK_HDR_PARAMETER_COUNT]={
    {"hdr-peak.txt",0,16,0}, {"hdr-game-white.txt",0.25f,4,1}, {"hdr-ui-white.txt",0.25f,4,1},
    {"hdr-exposure.txt",-3,3,0}, {"hdr-highlights.txt",0.25f,2,1}, {"hdr-shadows.txt",0.25f,2,1},
    {"hdr-contrast.txt",0.5f,1.5f,1}, {"hdr-saturation.txt",0,2,1}, {"hdr-bloom.txt",0,2,1},
    {"hdr-sdr-pivot.txt",0,0.6f,0.18f}, {"hdr-hue.txt",0,1,0.7f}, {"hdr-purity.txt",0,1,0.65f},
    {"hdr-vanilla-saturation.txt",0,1,1}
};
typedef struct { float v[16]; } TotkHDRUniform;
#if defined(__APPLE__) && TARGET_OS_OSX && !defined(GAME_BOTW) && !defined(TOTK_HDR_FRONTEND)
extern void front_hdr_parameters(float *values) __attribute__((weak_import));
#endif
static inline float totk_hdr_uniform_snapshot(TotkHDRUniform *out,float headroom,int enabled) {
    float p[TOTK_HDR_PARAMETER_COUNT];
    for(int i=0;i<TOTK_HDR_PARAMETER_COUNT;i++)p[i]=totk_hdr_parameters[i].initial;
#if defined(__APPLE__) && TARGET_OS_OSX && !defined(GAME_BOTW) && !defined(TOTK_HDR_FRONTEND)
    if(front_hdr_parameters)front_hdr_parameters(p);
#endif
    float peak=fmaxf(headroom,1);
    if(p[TOTK_HDR_PEAK]>0)peak=fminf(peak,fmaxf(p[TOTK_HDR_PEAK],1));
    float scene=fminf(p[TOTK_HDR_GAME_WHITE],peak),ui=fminf(p[TOTK_HDR_UI_WHITE],peak);
    *out=(TotkHDRUniform){{enabled?1.0f:0.0f,peak/scene,scene/ui,ui,
        exp2f(p[TOTK_HDR_EXPOSURE]),p[TOTK_HDR_HIGHLIGHTS],p[TOTK_HDR_SHADOWS],p[TOTK_HDR_CONTRAST],
        p[TOTK_HDR_SATURATION],p[TOTK_HDR_BLOOM],p[TOTK_HDR_SDR_PIVOT],p[TOTK_HDR_HUE],
        p[TOTK_HDR_PURITY],p[TOTK_HDR_VANILLA_SATURATION],0,0}};
    return peak;
}
