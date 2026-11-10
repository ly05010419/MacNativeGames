/* Exercise the production game-pass encoder policy, recorded NVN barriers and
   shader write classification with independently checked GPU pixels and memory. */
static MTL4RenderPassDescriptor *oot_bar_probe_pass(id<MTLTexture> tx, int load) {
    MTL4RenderPassDescriptor *rp=[MTL4RenderPassDescriptor new];
    rp.colorAttachments[0].texture=tx;
    rp.colorAttachments[0].loadAction=load ? MTLLoadActionLoad : MTLLoadActionClear;
    rp.colorAttachments[0].storeAction=MTLStoreActionStore;
    rp.renderTargetWidth=tx.width;rp.renderTargetHeight=tx.height;
    return rp;
}
static void oot_bar_probe_record(uint32_t bits) {
    struct { op_hdr h; uint32_t bits,pad; op_hdr end; } c={{OP_BARRIER,16},bits,0,{OP_END,8}};
    mtl_exec((const uint8_t *)&c);
}
static void oot_bar_probe_draw(NvProg *pr,id<MTLRenderPipelineState> ps,id<MTLBuffer> state,
                               id<MTLTexture> tx,int load,int side,int helper) {
    oot_bar_before_draw(pr);
    if(!g_enc) {
        MTL4RenderPassDescriptor *rp=oot_bar_probe_pass(tx,load);
        g_enc=helper ? enc_r_begin(rp) : oot_bar_render_begin(rp);
    }
    enc_r_pso(g_enc,ps);enc_r_cull(g_enc,MTLCullModeNone);
    enc_r_viewport(g_enc,(MTLViewport){0,0,(double)tx.width,(double)tx.height,0,1});
    NSUInteger x=side==2 ? tx.width/2 : 0, w=side ? tx.width/2 : tx.width;
    enc_r_scissor(g_enc,(MTLScissorRect){x,0,w,tx.height});
    enc_r_args(g_enc,MTLRenderStageVertex,state.gpuAddress,0);
    enc_r_args(g_enc,MTLRenderStageFragment,state.gpuAddress,0);
    enc_r_draw(g_enc,MTLPrimitiveTypeTriangle,0,3);oot_bar_note_draw(pr);
}
int mtl_probe_draw_barriers(void) {
    mtl_init();
    const char *ne=getenv("TOTK_PROBE_NEGATIVE");g_oot_bar_probe_negative=ne ? atoi(ne) : 0;
    atomic_store(&g_oot_bar_request,1);
    if(!oot_bar_allowed()) MDIE("draw-barrier probe requires OOT Mac");
    NSString *src=@"#include <metal_stdlib>\nusing namespace metal;\n"
        "struct V {float4 p [[position]];float value;};\n"
        "inline float4 pos(uint id){float2 q[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};return float4(q[id],0,1);}\n"
        "vertex V vb(uint id [[vertex_id]]){return {pos(id),0};}\n"
        "vertex V vr(uint id [[vertex_id]],device const uint*s [[buffer(0)]]){return {pos(id),float(s[0])/255.0f};}\n"
        "vertex V vw(uint id [[vertex_id]],device uint*s [[buffer(0)]]){if(id==0)s[0]=192;return {pos(id),0};}\n"
        "fragment float4 fr(V v [[stage_in]],device const uint*s [[buffer(0)]]){return float4(float(s[0])/255.0f,0,0,1);}\n"
        "fragment float4 fv(V v [[stage_in]]){return float4(v.value,0,0,1);}\n"
        "fragment float4 fw(V v [[stage_in]],device uint*s [[buffer(0)]]){if(uint(v.p.x)==0&&uint(v.p.y)==0)s[0]=128;return float4(0,0,0,1);}\n"
        "fragment float4 fc(){return float4(1,0,0,1);}\n"
        "kernel void k(device uint*s [[buffer(0)]]){s[0]=64;}\n";
    NSError *err=nil;id<MTLLibrary> lib=totk_msl_library(src,&err);
    if(!lib) MDIE("draw-barrier probe compile: %s",err.localizedDescription.UTF8String);
    const char *vn[]={"vb","vb","vr","vw"},*fn[]={"fr","fw","fv","fc"};
    id<MTLRenderPipelineState> ps[4];NvProg *pr[4];
    for(int i=0;i<4;i++) {
        MTLRenderPipelineDescriptor *pd=[MTLRenderPipelineDescriptor new];
        pd.vertexFunction=[lib newFunctionWithName:@(vn[i])];pd.fragmentFunction=[lib newFunctionWithName:@(fn[i])];
        pd.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;
        ps[i]=[g_dev newRenderPipelineStateWithDescriptor:pd error:&err];
        if(!ps[i]) MDIE("draw-barrier probe PSO %d: %s",i,err.localizedDescription.UTF8String);
        pr[i]=[NvProg new];
        for(int st=0;st<2;st++) {
            NvShader *sh=[NvShader new];sh->rf.nb=1;sh->rf.b[0].kind=NVN_RB_SBUFP;
            sh->rf.b[0].rw=(i==1&&st==1)||(i==3&&st==0);pr[i]->sh[st]=sh;
        }
    }
    id<MTLComputePipelineState> cs=[g_dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"k"] error:&err];
    if(!cs) MDIE("draw-barrier probe compute PSO: %s",err.localizedDescription.UTF8String);
    MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:16 height:16 mipmapped:NO];
    td.storageMode=MTLStorageModePrivate;td.usage=MTLTextureUsageRenderTarget;
    id<MTLTexture> tx=[g_dev newTextureWithDescriptor:td];res_add(tx);
    id<MTLBuffer> state=probe_buf(256),copy=probe_buf(256),out=probe_buf(1024);
    *(uint32_t *)copy.contents=96;int checked=0;
    for(int round=0;round<16;round++)for(int c=0;c<8;c++) {
        *(uint32_t *)state.contents=32;mtl_sub_begin();
        oot_bar_probe_draw(pr[0],ps[0],state,tx,0,0,0);rp_end();
        uint64_t full=g_oot_bar_full,skip=g_oot_bar_skip;
        if(c==0) {
            oot_bar_probe_draw(pr[0],ps[0],state,tx,0,0,0);
            if(g_oot_bar_skip!=skip+1) MDIE("draw-barrier probe: independent pass did not overlap");
        } else if(c==1) {
            oot_bar_probe_draw(pr[1],ps[1],state,tx,0,0,0);
            oot_bar_probe_record(NVN_BARRIER_ORDER_PRIMITIVES_BIT|NVN_BARRIER_INVALIDATE_SHADER_BIT);
            if(g_enc) MDIE("draw-barrier probe: shader write before native barrier was not ordered");
            full=g_oot_bar_full;
            oot_bar_probe_draw(pr[2],ps[2],state,tx,0,0,0);
            if(g_oot_bar_full!=full+1) MDIE("draw-barrier probe: native dependency did not keep full consumer barrier");
        } else if(c==2) {
            oot_bar_probe_draw(pr[0],ps[0],state,tx,0,1,0);
            oot_bar_probe_record(NVN_BARRIER_ORDER_PRIMITIVES_BIT|NVN_BARRIER_ORDER_FRAGMENTS_BIT);
            oot_bar_before_draw(pr[3]);
            if(g_enc) MDIE("draw-barrier probe: later shader write was not ordered after earlier readers");
            full=g_oot_bar_full;
            oot_bar_probe_draw(pr[3],ps[3],state,tx,1,2,0);
            if(g_oot_bar_full!=full+1) MDIE("draw-barrier probe: read-before-write dependency lost");
        } else if(c==3) {
            id<MTL4ComputeCommandEncoder> ce=enc_c_begin(NULL);enc_c_pso(ce,cs);enc_c_args(ce,state.gpuAddress,0);
            [ce dispatchThreads:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];enc_c_end(ce);
            oot_bar_probe_draw(pr[2],ps[2],state,tx,0,0,0);
            if(g_oot_bar_full!=full+1) MDIE("draw-barrier probe: compute dependency lost");
        } else if(c==4) {
            enc_b_copy_buffer(blit(),copy,0,state,0,4);
            oot_bar_probe_draw(pr[2],ps[2],state,tx,0,0,0);
            if(g_oot_bar_full!=full+1) MDIE("draw-barrier probe: copy dependency lost");
        } else if(c==5) {
            oot_bar_probe_draw(pr[0],ps[0],state,tx,0,0,1);rp_end();
            oot_bar_probe_draw(pr[0],ps[0],state,tx,0,0,0);
            if(g_oot_bar_full!=full+1) MDIE("draw-barrier probe: helper dependency lost");
        } else if(c==6) {
            /* fragment write -> native ORDER_FRAGMENTS|INVALIDATE_TEXTURE -> fragment read: the pass end barrier orders
               it, the consumer pass begins without a barrier, and the GPU must still read the written value */
            oot_bar_probe_draw(pr[1],ps[1],state,tx,0,0,0);
            oot_bar_probe_record(NVN_BARRIER_ORDER_FRAGMENTS_BIT|NVN_BARRIER_INVALIDATE_TEXTURE_BIT);
            if(g_enc) MDIE("draw-barrier probe: fragment write before a fragment-order barrier was not split");
            uint64_t fs=g_oot_bar_frag_skip;full=g_oot_bar_full;
            oot_bar_probe_draw(pr[0],ps[0],state,tx,0,0,0);
            if(g_oot_bar_frag_skip!=fs+1||g_oot_bar_full!=full) MDIE("draw-barrier probe: fragment-only native dependency kept the consumer barrier");
        } else {
            /* no writes -> native ORDER_FRAGMENTS|INVALIDATE_TEXTURE -> a new pass: no consumer barrier either */
            oot_bar_probe_draw(pr[0],ps[0],state,tx,0,0,0);
            oot_bar_probe_record(NVN_BARRIER_ORDER_FRAGMENTS_BIT|NVN_BARRIER_INVALIDATE_TEXTURE_BIT);rp_end();
            uint64_t fs=g_oot_bar_frag_skip;full=g_oot_bar_full;
            oot_bar_probe_draw(pr[2],ps[2],state,tx,0,0,0);
            if(g_oot_bar_frag_skip!=fs+1||g_oot_bar_full!=full) MDIE("draw-barrier probe: fragment-order barrier without writes kept the consumer barrier");
        }
        rp_end();enc_b_copy_texture_buffer(blit(),tx,0,0,MTLOriginMake(0,0,0),MTLSizeMake(16,16,1),out,0,64,1024);
        mtl_sub_commit();mtl_finish();
        uint32_t want=c==1?128:c==2?192:c==3?64:c==4?96:c==6?128:32;
        if(*(uint32_t *)state.contents!=want) MDIE("draw-barrier probe: case %d state=%u expected=%u",c,*(uint32_t *)state.contents,want);
        checked++;
        for(unsigned i=0;i<256;i++) {
            const uint8_t *p=(const uint8_t *)out.contents+4*i;
            uint32_t red=c==2 ? (i%16<8?32:255) : want;
            if(abs((int)p[0]-(int)red)>1||p[1]||p[2]||p[3]!=255)
                MDIE("draw-barrier probe: round %d case %d pixel %u=%u,%u,%u,%u expected red=%u",round,c,i,p[0],p[1],p[2],p[3],red);
            checked++;
        }
    }
    for(id o in @[tx,state,copy,out])defer_obj_release(o);
    g_oot_bar_probe_negative=0;atomic_store(&g_oot_bar_request,0);return checked;
}
