#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <setjmp.h>

#if defined(GAME_BOTW) && !defined(GAME_BOTW190)
/* 两层 guest 栈帧分别为 pane 构造器和 NVN scissor helper；读取前核对固定 ABI。
   The two guarded guest frames are the pane builder and its NVN scissor helper.
   Their fixed ABI is also checked before reading the saved framebuffer pointer. */
static int botw_ui_scissor_basis(uint64_t *fp, float basis[4]) {
    extern int runtime_module_offset(uint64_t, const char **, uint64_t *);
    extern __thread sigjmp_buf *runtime_fault_guard;
    sigjmp_buf jb; sigjmp_buf *previous=runtime_fault_guard;
    if (sigsetjmp(jb,1)) { runtime_fault_guard=previous; return -1; }
    runtime_fault_guard=&jb;
    int found=0,result=0; uintptr_t main_base=0;
    for (unsigned i=0;i<16 && fp && !((uintptr_t)fp&7);i++) {
        uint64_t link[2]; memcpy(link,fp,sizeof link);
        const char *name=NULL; uint64_t off=0;
        if (runtime_module_offset(link[1],&name,&off) && !strcmp(name,"main")) {
            if (!found) {
                if (off!=0x1273724) break;
                main_base=(uintptr_t)(link[1]-off); found=1;
            } else {
                if (off!=0x1365e08 || (uintptr_t)(link[1]-off)!=main_base) break;
                static const uint32_t guards[][2]={
                    {0x1273570,0xf81d0ff5},{0x1273574,0xa9014ff4},
                    {0x1273578,0xa9027bfd},{0x127357c,0x910083fd},
                    {0x1273720,0xd63f0100},{0x1365b30,0xd10243ff},
                    {0x1365b3c,0xa9087bfd},{0x1365b40,0x910203fd},
                    {0x1365e04,0x97fc35db}};
                result=-2;
                for (unsigned j=0;j<sizeof guards/sizeof *guards;j++) {
                    uint32_t word; memcpy(&word,(const void *)(main_base+guards[j][0]),4);
                    if (word!=guards[j][1]) goto done;
                }
                uintptr_t parent=(uintptr_t)link[0],fb=0,vp=0;
                if (parent<0x80 || (parent&7)) goto done;
                /* caller SP=FP-0x80; copied context stores fb at SP+0x28, vp at SP+0x38. */
                memcpy(&fb,(const void *)(parent-0x58),8);
                memcpy(&vp,(const void *)(parent-0x48),8);
                float f[6],v[4]; uint32_t orientation[2];
                memcpy(f,(const void *)(fb+8),sizeof f);
                memcpy(v,(const void *)(vp+8),sizeof v);
                memcpy(orientation,(const void *)(vp+0x18),sizeof orientation);
                result=-3;
                for (unsigned j=0;j<6;j++) if (!isfinite(f[j])) goto done;
                if (f[0]<=0 || f[1]<=0 || f[4]<=0 || f[5]<=0 ||
                    f[0]>16384 || f[1]>16384 || f[4]>16384 || f[5]>16384 || f[2]!=0 || f[3]!=0) goto done;
                result=-4;
                if (orientation[0] || orientation[1] || v[0]!=0 || v[1]!=0 || v[2]!=f[0] || v[3]!=f[1]) goto done;
                basis[0]=f[0]; basis[1]=f[1]; basis[2]=f[4]; basis[3]=f[5];
                result=1; goto done;
            }
        }
        if ((uint64_t *)(uintptr_t)link[0]==fp) break;
        fp=(uint64_t *)(uintptr_t)link[0];
    }
done:
    runtime_fault_guard=previous; return result;
}
#endif
