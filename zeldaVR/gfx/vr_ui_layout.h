/* Fit the UI camera's authored layout into its actual eye viewport. The UI
 * camera is independent of the world camera; no headset or 16:9 constant is
 * substituted for either projection. Scissors use the same affine mapping. */
#pragma once
#include <math.h>
#include <stdint.h>
typedef struct { double vp[4]; uint32_t sc[4]; double aspect,scale[2]; } vr_ui_layout;
static inline int vr_ui_layout_fit(const float p[16],const double vp[4],const uint32_t sc[4],
                                    uint32_t width,uint32_t height,vr_ui_layout *out) {
    if(!p||!vp||!sc||!out||!width||!height)return 0;
    for(int i=0;i<16;i++)if(!isfinite(p[i]))return 0;
    for(int i=0;i<4;i++)if(!isfinite(vp[i]))return 0;
    if(!(p[0]>0&&p[5]>0)||p[1]||p[4]||p[12]||p[13]||
       !((p[14]==-1&&p[15]==0)||(p[14]==0&&p[15]==1))||!(vp[2]>0&&vp[3]>0))return 0;
    double a=(double)p[5]/p[0],w=fmin(vp[2],vp[3]*a),h=fmin(vp[3],vp[2]/a);
    if(!(a>0&&isfinite(a)&&w>0&&h>0))return 0;
    out->aspect=a;out->vp[0]=vp[0]+(vp[2]-w)*.5;out->vp[1]=vp[1]+(vp[3]-h)*.5;
    out->vp[2]=w;out->vp[3]=h;out->scale[0]=w/vp[2];out->scale[1]=h/vp[3];
    for(int i=0;i<2;i++){
        double lo=out->vp[i]+((double)sc[i]-vp[i])*out->scale[i];
        double hi=out->vp[i]+((double)sc[i]+sc[i+2]-vp[i])*out->scale[i];
        double limit=i?height:width;
        lo=fmax(0,fmin(limit,floor(lo)));hi=fmax(lo,fmin(limit,ceil(hi)));
        out->sc[i]=(uint32_t)lo;out->sc[i+2]=(uint32_t)(hi-lo);
    }
    return 1;
}
