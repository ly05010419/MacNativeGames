/* Sead row-major perspective projection -> OpenXR ray angles. Shared by the
 * native game-frame metadata and its compositor; no guessed field of view. */
#pragma once
#include <math.h>
#include <float.h>
#include <stdint.h>

/* A source can use the headset's world path only when its actual ray grid
   satisfies that eye's native projection contract. A menu preview's valid
   perspective matrix is not a headset eye. Tolerance is float roundoff. */
static inline int vr_projection_eye_grid(const float source[4],const float eye[4],float overscan){
    if(!source||!eye||!isfinite(overscan)||overscan<=0)return 0;
    for(int i=0;i<4;i++){
        float want=overscan*tanf(eye[i]),actual=tanf(source[i]);
        if(!isfinite(want)||!isfinite(actual)||fabsf(actual-want)>16*FLT_EPSILON*fmaxf(1,fabsf(want)))return 0;
    }return 1;
}

enum { VR_PROJECTION_NO_WRITE,VR_PROJECTION_CAMERA,VR_PROJECTION_COPY,VR_PROJECTION_INHERIT };
/* A depth-only draw never writes color provenance. A blended auxiliary draw
   composes into the established image grid; its sampled pictures do not own it. */
static inline int vr_projection_color_action(uint32_t mask,unsigned attachment,uint32_t blend,int camera,int image_post){
    if(attachment>=8||!((mask>>(4*attachment))&15u))return VR_PROJECTION_NO_WRITE;
    if(camera)return VR_PROJECTION_CAMERA;
    if((blend>>attachment)&1u)return VR_PROJECTION_NO_WRITE;
    return image_post?VR_PROJECTION_COPY:VR_PROJECTION_INHERIT;
}

/* Source image plane in its eye's coordinates, before tracking rotation. */
static inline void vr_projection_plane(const float fov[4], float c[3], float x[3], float y[3]) {
    float l=tanf(fov[0]),r=tanf(fov[1]),u=tanf(fov[2]),d=tanf(fov[3]);
    c[0]=(l+r)*.5f;c[1]=(u+d)*.5f;c[2]=-1;
    x[0]=r-l;x[1]=x[2]=0;y[0]=y[2]=0;y[1]=u-d;
}

static inline int vr_projection_fov(const float p[16], float fov[4]) {
    for (int i = 0; i < 16; i++) if (!isfinite(p[i])) return 0;
    if (!(p[0] > 0 && p[5] > 0) || p[1] || p[3] || p[4] || p[7] ||
        p[12] || p[13] || p[14] != -1 || p[15]) return 0;
    const float l = (p[2] - 1) / p[0], r = (p[2] + 1) / p[0];
    const float d = (p[6] - 1) / p[5], u = (p[6] + 1) / p[5];
    if (!(l < 0 && r > 0 && d < 0 && u > 0)) return 0;
    fov[0] = atanf(l); fov[1] = atanf(r); fov[2] = atanf(u); fov[3] = atanf(d);
    return 1;
}
