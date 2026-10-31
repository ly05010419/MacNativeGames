/* Original client for Finally the Controller Works' documented localhost S2UDP protocol.
   Reference: Peterksharma/switch2mac, Output/UDPHub.swift. No SDL dependency or virtual HID entitlement.
   Only player 1 (127.0.0.1:24800). Reject malformed/nonfinite input; silence releases every button. */
#ifndef TOTK_SWITCH2_UDP_H
#define TOTK_SWITCH2_UDP_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <ctype.h>
typedef struct { uint64_t buttons; float axes[4]; int16_t gyro[3], accel[3]; } Switch2UDPState;
static inline uint32_t s2udp_u32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
static inline int s2udp_decode(const unsigned char *p, size_t n, Switch2UDPState *out) {
    if (n != 44 || memcmp(p,"S2B1",4)) return 0;
    Switch2UDPState next = {0};
    /* Wire button bit for each physical button (south, east, west, north, L3, R3, ...). */
    static const unsigned bits[16] = {2,3,0,1,11,10,22,6,23,7,9,8,19,17,18,16};
    uint32_t b = s2udp_u32(p+8);
    for (unsigned i=0;i<16;i++) if (b & (1u<<bits[i])) next.buttons |= 1ull<<i;
    for (unsigned i=0;i<4;i++) {
        uint32_t raw = s2udp_u32(p+12+4*i); float f; memcpy(&f,&raw,4);
        if (!isfinite(f)) return 0;
        next.axes[i] = fmaxf(-1.f,fminf(1.f,f));
    }
    if (p[28]) next.buttons |= 1ull<<8;
    if (p[29]) next.buttons |= 1ull<<9;
    for (unsigned i=0;i<3;i++) {
        next.gyro[i]=(int16_t)((uint16_t)p[32+i*2] | (uint16_t)p[33+i*2]<<8);
        next.accel[i]=(int16_t)((uint16_t)p[38+i*2] | (uint16_t)p[39+i*2]<<8);
    }
    *out = next; return 1;
}
/* Pro 2 hardware: nose-up rotates raw gyro X positive, clockwise yaw rotates Z negative.
   Acceleration is negated to match the runtime's gravity convention (face-up Z=-1).
   The scale is supplied so a measured calibration can replace the nominal gyro range. */
static inline void s2udp_motion(const Switch2UDPState *s, double dps, double acc[3], double gyro[3]) {
    double rad=dps*3.14159265358979323846/180.0;
    acc[0]=-s->accel[0]/4096.0; acc[1]=-s->accel[1]/4096.0; acc[2]=-s->accel[2]/4096.0;
    gyro[0]=s->gyro[0]*rad; gyro[1]=s->gyro[1]*rad; gyro[2]=s->gyro[2]*rad;
}
/* S2R1 is the bridge's two-band strength API, not independent left/right HD waveforms. */
static inline void s2udp_rumble_packet(unsigned char out[6], float low, float high) {
    memcpy(out,"S2R1",4);
    out[4]=(unsigned char)lrintf(isfinite(low)?fmaxf(0,fminf(1,low))*255:0);
    out[5]=(unsigned char)lrintf(isfinite(high)?fmaxf(0,fminf(1,high))*255:0);
}
static inline double s2udp_gyro_scale(const char *text, double fallback) {
    if (!text) return fallback;
    char *end; double v=strtod(text,&end);
    if (end==text) return fallback;
    while (isspace((unsigned char)*end)) end++;
    return !*end && isfinite(v) && v>0 && v<1 ? v : fallback;
}
/* Negotiated S2H1 extension: retain both handles, both bands, and frequencies. */
static inline void s2udp_hd_packet(unsigned char out[36], const float v[2][4]) {
    memcpy(out,"S2H1",4);
    for (unsigned i=0;i<8;i++) {
        uint32_t b; memcpy(&b,&v[i/4][i%4],4);
        for (unsigned j=0;j<4;j++) out[4+i*4+j]=(unsigned char)(b>>(8*j));
    }
}
#endif
