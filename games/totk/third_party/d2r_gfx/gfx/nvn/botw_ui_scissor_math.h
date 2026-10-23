#pragma once
#include <math.h>
#include <stdint.h>

/* pane 的 scissor 用 framebuffer 单位，顶点用 UI 投影；基准随原父框记录，不猜 shader。
   The pane builder uses framebuffer units; its vertices use the UI projection.
   ui_basis is recorded with that pane's scissor, never inferred from a shader key. */
static int botw_ui_scissor_map_rect(const int32_t rect[4], const float basis[4],
                                    const float matrix[28], int32_t mapped[4]) {
    for (int i=0;i<4;i++) if (!isfinite(basis[i]) || basis[i]<=0 || basis[i]>16384) return 0;
    for (int i=0;i<28;i++) if (!isfinite(matrix[i])) return 0;
    static const unsigned zero[]={1,2,3,4,6,7,8,9,12,13,15,24,25};
    for (unsigned i=0;i<sizeof zero/sizeof *zero;i++) if (matrix[zero[i]]!=0) return 0;
    if (matrix[0]<=0 || matrix[5]<=0 || matrix[14]!=-1 || matrix[26]!=1 || matrix[27]>=0) return 0;
    if (rect[2]<=0 || rect[3]<=0) return 0;
    double sx=basis[0]*(double)matrix[0]/(-2.0*matrix[27]);
    double sy=basis[1]*(double)matrix[5]/(-2.0*matrix[27]);
    if (!isfinite(sx) || !isfinite(sy) || sx<=0 || sy<=0) return 0;
    if (fabs(sx-1)<1e-6) sx=1;
    if (fabs(sy-1)<1e-6) sy=1;
    if (sx==1 && sy==1) return 0;
    double cx=basis[2]*0.5,cy=basis[3]*0.5;
    double x0=fmax(0,floor(cx+(rect[0]-cx)*sx));
    double y0=fmax(0,floor(cy+(rect[1]-cy)*sy));
    double x1=fmin(basis[2],ceil(cx+((double)rect[0]+rect[2]-cx)*sx));
    double y1=fmin(basis[3],ceil(cy+((double)rect[1]+rect[3]-cy)*sy));
    if (x1<=x0 || y1<=y0) return -1;
    mapped[0]=(int32_t)x0; mapped[1]=(int32_t)y0;
    mapped[2]=(int32_t)(x1-x0); mapped[3]=(int32_t)(y1-y0);
    return 1;
}
