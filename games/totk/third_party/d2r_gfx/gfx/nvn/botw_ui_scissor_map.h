#pragma once
#if defined(GAME_BOTW) && !defined(GAME_BOTW190)
#include "botw_ui_scissor_math.h"

static int botw_ui_scissor_apply(NvProg *pr, MTLViewport vp, MTLScissorRect *sc,
                                 NSUInteger tw, NSUInteger th) {
    static int enabled=-1;
    if (enabled<0) enabled=strcmp(getenv("BOTW_UI_SCISSOR_MAP") ?: "1","0")!=0;
    if (!enabled || !R.has_sc || R.sc.ui_basis[0]<=0 || !pr || !pr->sh[0] ||
        g_ui_moff!=0 || S.ds.dt || S.ds.dw || S.ds.st || S.poly.cull ||
        vp.originX!=0 || vp.originY!=0 || vp.width!=tw || vp.height!=th ||
        R.sc.ui_basis[2]!=tw || R.sc.ui_basis[3]!=th) return 0;
    if (S.nattr!=1 || S.attr[0].fmt!=22 || S.attr[0].stream || S.attr[0].offset ||
        S.stream[0].stride!=8 || S.stream[0].divisor) return 0;
    if (S.ubo[0][0].size!=560 || S.ubo[0][10].size!=32) return 0;
    NvShader *sh=pr->sh[0]; const uint8_t *bytes[2]={NULL,NULL};
    for (int i=0;i<sh->rf.nb;i++) {
        const nvn_rbind *b=&sh->rf.b[i];
        if (b->kind!=NVN_RB_CBUF || (b->reg!=0 && b->reg!=10)) continue;
        unsigned slot=b->reg==0?0:1; size_t size=slot?16:112;
        const p_bind_buf *u=&S.ubo[0][b->reg];
        NSUInteger at=sh->ab_off+(NSUInteger)b->aid*8;
        if (!sh->ab_buf || !sh->ab_buf.contents || at>sh->ab_buf.length || 8>sh->ab_buf.length-at) return 0;
        uint64_t gpu=0,mo=0; void *mb=NULL;
        memcpy(&gpu,(const uint8_t *)sh->ab_buf.contents+at,8);
        if (!u->addr || pend_hits(u->addr,size) || !mtl_addr(u->addr,size,&mb,&mo)) return 0;
        id<MTLBuffer> original=(__bridge id<MTLBuffer>)mb;
        if (original.storageMode==MTLStorageModePrivate || !original.contents) original=nil;
        bytes[slot]=bound_uniform_bytes(gpu,size,original);
    }
    if (!bytes[0] || !bytes[1]) return 0;
    float matrix[28],offset[4]; memcpy(matrix,bytes[0],sizeof matrix); memcpy(offset,bytes[1],sizeof offset);
    for (unsigned i=0;i<4;i++) if (offset[i]!=0) return 0;
    int32_t input[4]={R.sc.x,R.sc.y,R.sc.w,R.sc.h},mapped[4];
    int result=botw_ui_scissor_map_rect(input,R.sc.ui_basis,matrix,mapped);
    if (result==1) *sc=(MTLScissorRect){mapped[0],mapped[1],mapped[2],mapped[3]};
    return result;
}
#endif
