/* Map authored HUD clip coordinates into the actual world eye's ray grid.
 * Both eyes share one reference from this canvas's real source rotations.
 * World angular reprojection then maps corresponding HUD points identically. */
#pragma once
#include "vr_projection.h"
#include "../runtime/vr_view_math.h"

typedef struct { float clip[16]; float native_shift; } vr_ui_rays;
/* A world-cache background is already in eye pixel coordinates. Its actual
   widget must cover all four clip corners before it can bypass HUD mapping. */
static inline int vr_ui_rays_fullscreen(const float camera[140],float shift){
    if(!camera||!isfinite(shift))return 0;
    for(int corner=0;corner<4;corner++){
        float q[4]={camera[134]+(corner&1?camera[132]:0),camera[135]-(corner&2?camera[133]:0),0,1},v[4]={0,0,0,1},clip[4]={0};
        for(int i=0;i<3;i++)for(int j=0;j<4;j++)v[i]+=camera[16+4*i+j]*q[j];
        for(int i=0;i<4;i++)for(int j=0;j<4;j++)clip[i]+=camera[4*i+j]*v[j];
        if(!(clip[3]>0)||!isfinite(clip[3]))return 0;
        float x=clip[0]/clip[3]+shift,y=clip[1]/clip[3];
        if(!isfinite(x)||!isfinite(y)||fabsf(x-(corner&1?1.f:-1.f))>1e-4f||fabsf(y-(corner&2?-1.f:1.f))>1e-4f)return 0;
    }return 1;
}
static inline int vr_ui_rays_make(const float ui[16],const float fov[4],
                                  const float eye[9],const float reference[9],
                                  float shift,vr_ui_rays *out) {
    float unused[4];
    if(!ui||!fov||!eye||!reference||!out||!isfinite(shift)||
       !vr_projection_fov(ui,unused)||!vr_rotation_valid(eye)||!vr_rotation_valid(reference))return 0;
    float fy,aspect,offset[2];if(!vr_eye_intrinsics(fov,&fy,&aspect,offset))return 0;
    float l=tanf(fov[0]),r=tanf(fov[1]),u=tanf(fov[2]),d=tanf(fov[3]);
    float px=2/(r-l),py=2/(u-d),cx=(r+l)/(r-l),cy=(u+d)/(u-d);
    float inverse[3][3]={{1/ui[0],0,ui[2]/ui[0]}, {0,1/ui[5],ui[6]/ui[5]}, {0,0,-1}};
    float rays[3][3]={{0}};
    for(int i=0;i<3;i++)for(int j=0;j<3;j++)for(int k=0;k<3;k++)
        rays[i][j]+=vr_dot3(eye+3*i,reference+3*k)*inverse[k][j];
    memset(out,0,sizeof *out);out->native_shift=shift;
    for(int j=0;j<3;j++){
        int c=j==2?3:j;float w=-rays[2][j];
        /* The original shader adds shift*w after its projection. Cancel it
           here, before clipping, instead of moving already clipped pixels. */
        out->clip[c]=px*rays[0][j]+cx*rays[2][j]-shift*w;
        out->clip[4+c]=py*rays[1][j]+cy*rays[2][j];
        out->clip[12+c]=w;
    }
    /* Only depth-disabled HUD draws use this matrix. A rotated clip w must
       not retain the old camera's clip z and accidentally clip the HUD. */
    return 1;
}
static inline void vr_ui_rays_matrix(const vr_ui_rays *map,const float input[16],float output[16]) {
    float result[16]={0};
    for(int i=0;i<4;i++)for(int j=0;j<4;j++)for(int k=0;k<4;k++)
        result[4*i+j]+=map->clip[4*i+k]*input[4*k+j];
    memcpy(output,result,sizeof result);
}
/* An orthographic UI camera already produces authored clip coordinates.
 * Fit its original viewport into the common reference grid before the eye
 * ray transform, then use the complete eye viewport for rasterization. */
static inline int vr_ui_rays_viewport(vr_ui_rays *map,const double authored[4],const double eye[4]){
    if(!map||!authored||!eye||!(eye[2]>0&&eye[3]>0&&authored[2]>0&&authored[3]>0))return 0;
    float sx=authored[2]/eye[2],sy=authored[3]/eye[3];
    float tx=2*(authored[0]-eye[0])/eye[2]+sx-1,ty=1-sy-2*(authored[1]-eye[1])/eye[3];
    if(!isfinite(sx)||!isfinite(sy)||!isfinite(tx)||!isfinite(ty))return 0;
    for(int i=0;i<4;i++){
        float x=map->clip[4*i],y=map->clip[4*i+1];
        map->clip[4*i]=x*sx;map->clip[4*i+1]=y*sy;map->clip[4*i+3]+=x*tx+y*ty;
    }return 1;
}
/* The hardware scissor is axis aligned: bound the transformed native clip
 * rectangle, and intersect it with this eye so it cannot spill into its peer. */
static inline int vr_ui_rays_scissor(const vr_ui_rays *map,const double vp[4],
                                     const uint32_t sc[4],uint32_t output[4]) {
    if(!map||!vp||!sc||!output||!(vp[2]>0&&vp[3]>0))return 0;
    double lo[2]={INFINITY,INFINITY},hi[2]={-INFINITY,-INFINITY};
    double a[2]={fmax(vp[0],sc[0]),fmax(vp[1],sc[1])};
    double b[2]={fmin(vp[0]+vp[2],(double)sc[0]+sc[2]),fmin(vp[1]+vp[3],(double)sc[1]+sc[3])};
    if(b[0]<a[0]||b[1]<a[1])return 0;
    if(a[0]==vp[0]&&a[1]==vp[1]&&b[0]==vp[0]+vp[2]&&b[1]==vp[1]+vp[3]){
        for(int i=0;i<4;i++)output[i]=(uint32_t)vp[i];return 1;
    }
    for(int c=0;c<4;c++){
        float q[4]={(float)(2*((c&1?b[0]:a[0])-vp[0])/vp[2]-1)-map->native_shift,
                    (float)(1-2*((c&2?b[1]:a[1])-vp[1])/vp[3]),0,1},v[4]={0};
        for(int i=0;i<4;i++)for(int j=0;j<4;j++)v[i]+=map->clip[4*i+j]*q[j];
        if(!(v[3]>0)||!isfinite(v[3]))return 0;
        double p[2]={vp[0]+(.5+.5*(v[0]/v[3]+map->native_shift))*vp[2],vp[1]+(.5-.5*v[1]/v[3])*vp[3]};
        for(int i=0;i<2;i++){if(!isfinite(p[i]))return 0;lo[i]=fmin(lo[i],p[i]);hi[i]=fmax(hi[i],p[i]);}
    }
    for(int i=0;i<2;i++){
        double x=fmax(vp[i],floor(lo[i])),y=fmin(vp[i]+vp[i+2],ceil(hi[i]));
        output[i]=(uint32_t)fmax(0,x);output[i+2]=(uint32_t)fmax(0,y-x);
    }
    return 1;
}
