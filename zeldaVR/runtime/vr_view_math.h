/* Row-major camera view math used by the BotW adapter and CPU regression test. */
#pragma once
#include <math.h>
#include <string.h>
static inline float vr_dot3(const float a[3], const float b[3]) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
static inline void vr_view_eye(const float m[12], float p[3]) {
    for (int j=0;j<3;j++) p[j]=-(m[j]*m[3]+m[4+j]*m[7]+m[8+j]*m[11]);
}
static inline void vr_view_translation(float m[12], const float p[3]) {
    for (int i=0;i<3;i++) m[4*i+3]=-vr_dot3(m+4*i,p);
}
static inline void vr_view_turn(const float m[12], const float h[12], float out[12]) {
    float r[12];
    for (int i=0;i<3;i++) for (int j=0;j<4;j++)
        r[4*i+j]=h[4*i]*m[j]+h[4*i+1]*m[4+j]+h[4*i+2]*m[8+j];
    memcpy(out,r,sizeof r);
}
static inline int vr_rotation_valid(const float r[9]) {
    for(int i=0;i<9;i++)if(!isfinite(r[i]))return 0;
    for(int i=0;i<3;i++)for(int j=0;j<3;j++)
        if(fabsf(vr_dot3(r+3*i,r+3*j)-(i==j?1.0f:0.0f))>.002f)return 0;
    const float cross[3]={r[4]*r[8]-r[5]*r[7],r[5]*r[6]-r[3]*r[8],r[3]*r[7]-r[4]*r[6]};
    return vr_dot3(r,cross)>.998f;
}
/* Rays in the source camera become rays in the current game camera. Neither
   translation nor an assumed headset aspect participates in angular timewarp. */
static inline int vr_view_reprojection(const float source[9],const float target[12],float delta[9]) {
    float t[9];for(int i=0;i<3;i++)memcpy(t+3*i,target+4*i,3*sizeof(float));
    if(!vr_rotation_valid(source)||!vr_rotation_valid(t))return 0;
    for(int i=0;i<3;i++)for(int j=0;j<3;j++)delta[3*i+j]=vr_dot3(t+3*i,source+3*j);
    return 1;
}
static inline void vr_rotation_apply(const float r[9],float v[3]) {
    float p[3];for(int i=0;i<3;i++)p[i]=vr_dot3(r+3*i,v);memcpy(v,p,sizeof p);
}
static inline int vr_view_level(float m[12]) {
    float p[3]; vr_view_eye(m,p);
    float x=m[0],z=m[2],n=sqrtf(x*x+z*z); if (!(n>0) || !isfinite(n)) return 0;
    x/=n;z/=n;
    const float r[12]={x,0,z,0,0,1,0,0,-z,0,x,0};memcpy(m,r,sizeof r);
    vr_view_translation(m,p);return 1;
}
static inline int vr_eye_intrinsics(const float f[4], float *fovy, float *aspect, float shift[2]) {
    for(int i=0;i<4;i++) if(!isfinite(f[i]) || fabsf(f[i])>=1.55f)return 0;
    float l=tanf(f[0]),r=tanf(f[1]),u=tanf(f[2]),d=tanf(f[3]);
    if(!(l<0&&r>0&&u>0&&d<0))return 0;
    *fovy=2*atanf((u-d)*.5f);*aspect=(r-l)/(u-d);
    shift[0]=(r+l)/(2*(r-l));shift[1]=(u+d)/(2*(u-d));return 1;
}

/* Expand the actual headset rays for rotation reprojection without changing
 * the optical mapping of the final compositor. */
static inline int vr_eye_intrinsics_overscan(const float f[4],float scale,float out[4]) {
    if(!f||!out||!isfinite(scale)||scale<1||scale>2)return 0;
    float expanded[4];for(int i=0;i<4;i++){if(!isfinite(f[i])||fabsf(f[i])>=1.55f)return 0;expanded[i]=atanf(tanf(f[i])*scale); }
    return vr_eye_intrinsics(expanded,&out[0],&out[1],&out[2]);
}
