/* Pixel regressions for the production argument cache, included beside the
 * argument-epoch probe. Serial replacement and snapshot retirement change the
 * referenced resource without changing the program or uniform binding.
 * No dependency-audit code or game assets are needed. */
static int probe_args_invalidation(void) {
    int checked=0;void *mem=NULL;
    if(posix_memalign(&mem,16384,16384) || !mem)MDIE("argument invalidation probe: memory");
    memset(mem,0,16384);mtl_pool_add(mem,16384);
    const float colors[4][4]={{.25,.5,.75,1},{.75,.25,.5,1},{.5,.75,.25,1},{.25,.75,.5,1}};
    const uint8_t texels[2][4]={{255,255,255,255},{128,255,128,255}};
    tex_t tt[2]={0};id<MTLTexture> sampled[2];
    for(int i=0;i<2;i++) {
        MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1 height:1 mipmapped:NO];td.storageMode=MTLStorageModeShared;td.usage=MTLTextureUsageShaderRead;
        sampled[i]=[g_dev newTextureWithDescriptor:td];if(!sampled[i])MDIE("argument invalidation probe: texture");res_add(sampled[i]);
        [sampled[i] replaceRegion:MTLRegionMake2D(0,0,1,1) mipmapLevel:0 withBytes:texels[i] bytesPerRow:4];
        tt[i].serial=0xffffe900+i;tt[i].d=(nvn_texdesc){.width=1,.height=1,.depth=1,.levels=1,.format=0x25,.target=1,.swizzle_r=2,.swizzle_g=3,.swizzle_b=4,.swizzle_a=5};
    }
    NSError *error=nil;
    NSString *source=@"#include <metal_stdlib>\nusing namespace metal; struct A { constant float4 *c [[id(0)]]; texture2d<float> t [[id(1)]]; }; struct O { float4 p [[position]]; float4 c; }; vertex O va(uint i [[vertex_id]],constant A &a [[buffer(0)]]) { float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};return {float4(p[i],0,1),*a.c*a.t.read(uint2(0))}; } vertex O vb(uint i [[vertex_id]],constant A &a [[buffer(0)]]) { float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};return {float4(p[i],0,1),*a.c*a.t.read(uint2(0))}; } fragment float4 fs(O o [[stage_in]]) { return o.c; }";
    id<MTLLibrary> lib=[g_dev newLibraryWithSource:source options:nil error:&error];if(!lib)MDIE("argument invalidation probe: shader %s",error.localizedDescription.UTF8String);
    NvShader *sh[2];id<MTLRenderPipelineState> ps[2];prog_t programs[2]={0};
    for(int i=0;i<2;i++) {
        sh[i]=[NvShader new];sh[i]->stage=0;sh[i]->fn=[lib newFunctionWithName:i?@"vb":@"va"];
        sh[i]->rf.nb=2;sh[i]->rf.b[0]=(nvn_rbind){.kind=NVN_RB_CBUF,.aid=0,.reg=0};sh[i]->rf.b[1]=(nvn_rbind){.kind=NVN_RB_TEX,.aid=1,.reg=0,.dtt=MTLTextureType2D,.dpf=NVN_DPF_FLOAT};
        snprintf(sh[i]->key,sizeof sh[i]->key,"probe-args-invalidation-%d",i);sh[i]->ae=[sh[i]->fn newArgumentEncoderWithBufferIndex:0];sh[i]->arg_len=sh[i]->ae.encodedLength;
        MTLRenderPipelineDescriptor *pd=[MTLRenderPipelineDescriptor new];pd.vertexFunction=sh[i]->fn;pd.fragmentFunction=[lib newFunctionWithName:@"fs"];pd.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;
        ps[i]=[g_dev newRenderPipelineStateWithDescriptor:pd error:&error];if(!ps[i])MDIE("argument invalidation probe: pipeline %s",error.localizedDescription.UTF8String);
    }
    MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:24 height:8 mipmapped:NO];td.storageMode=MTLStorageModePrivate;td.usage=MTLTextureUsageRenderTarget;
    id<MTLTexture> target=[g_dev newTextureWithDescriptor:td];res_add(target);id<MTLBuffer> rb=probe_buf(24*8*4);
    op_hdr end={OP_END,8};
    for(int mode=0;mode<5;mode++)for(int snapshot=0;snapshot<2;snapshot++) {
        atomic_store(&g_arg_epoch_on,mode==1||mode==2);atomic_store(&g_arg_epoch_check,mode==2);atomic_store(&g_tex_slots_on,mode>=3);atomic_store(&g_tex_slots_check,mode==4);
        memcpy(mem,colors[0],16);memcpy((uint8_t *)mem+16,colors[1],16);
        mtl_sub_begin();mtl_exec((const uint8_t *)&end);
        for(int i=0;i<2;i++){serial_put(tt[i].serial,sampled[i]);mtl_reg_tex(71+i,&tt[i],NULL);}
        if(snapshot) {
            p_bind_buf u0={0,0,(uintptr_t)mem,16},u1={0,0,(uintptr_t)mem+16,16};uint8_t cmd[128];
            uint8_t *w=lp_rec(cmd,OP_BIND_UBO,&u0,sizeof u0);w=lp_rec(w,OP_BIND_UBO,&u1,sizeof u1);lp_end(w);uint64_t h=(uintptr_t)cmd;mtl_sub_snapshot_ubos(&h,1);
            /* The standalone snapshot helper may copy asynchronously. Materialize
             * both captured ranges before deliberately mutating their sources. */
            if(!usnap_find(u0.addr,u0.size) || !usnap_find(u1.addr,u1.size))MDIE("argument invalidation probe: snapshot setup not materialized");
            memcpy(mem,colors[2],16);memcpy((uint8_t *)mem+16,colors[3],16);
        }
        g_pass.serial++;args_dirty_all();
        MTL4RenderPassDescriptor *rp=[MTL4RenderPassDescriptor new];rp.colorAttachments[0].texture=target;rp.colorAttachments[0].loadAction=MTLLoadActionClear;rp.colorAttachments[0].storeAction=MTLStoreActionStore;rp.renderTargetWidth=24;rp.renderTargetHeight=8;
        id<MTL4RenderCommandEncoder> en=enc_r_begin(rp);enc_r_cull(en,MTLCullModeNone);enc_r_viewport(en,(MTLViewport){0,0,24,8,0,1});
        uint8_t expected[768];int current=0,unbound=0;
        for(int stripe=0;stripe<12;stripe++) {
            int which=stripe==2?1:0;p_bind_program program={&programs[which],0x1f};state_op(OP_BIND_PROGRAM,&program);
            p_bind_buf u={0,0,(uintptr_t)mem+(stripe>=4?16:0),16};state_op(OP_BIND_UBO,&u);
            if(stripe==6)current=1;if(stripe==8){serial_put(tt[1].serial,sampled[0]);current=0;}
            if(stripe==9)usnap_drop(u.addr,u.size);
            if(stripe==10)unbound=1;if(stripe==11)unbound=0;
            p_bind_handles handle={.stage=0,.n=1,.h={unbound?0:(stripe>=6?72:71)}};state_op(OP_BIND_TEX,&handle);
            enc_r_pso(en,ps[which]);enc_r_scissor(en,(MTLScissorRect){stripe*2,0,2,8});
            encode_args((id<MTLCommandEncoder>)en,sh[which],0,1);
            enc_r_draw(en,MTLPrimitiveTypeTriangle,0,3);
            int color=stripe>=4?1:0;if(snapshot && stripe>=9)color+=2;
            for(int y=0;y<8;y++)for(int x=stripe*2;x<stripe*2+2;x++)for(int c=0;c<4;c++)expected[(y*24+x)*4+c]=unbound?0:(uint8_t)lroundf(colors[color][c]*texels[current][c]);
            checked++;
        }
        enc_r_end(en);enc_b_copy_texture_buffer(blit(),target,0,0,MTLOriginMake(0,0,0),MTLSizeMake(24,8,1),rb,0,96,768);mtl_sub_commit();mtl_finish();
        for(int i=0;i<768;i++){if(abs((int)((uint8_t *)rb.contents)[i]-expected[i])>1)MDIE("argument invalidation probe: GPU pixel mode%d snapshot%d stripe%d channel%d got%d want%d",mode,snapshot,(i/4)%24/2,i%4,((uint8_t *)rb.contents)[i],expected[i]);checked++;}
    }
    for(int i=0;i<2;i++){mtl_reg_tex(71+i,NULL,NULL);serial_del(tt[i].serial);res_remove(sampled[i]);}
    mtl_pool_del(mem);free(mem);res_remove(target);res_remove(rb);memset(&S,0,sizeof S);args_dirty_all();
    atomic_store(&g_arg_epoch_on,0);atomic_store(&g_arg_epoch_check,0);
    atomic_store(&g_tex_slots_on,0);atomic_store(&g_tex_slots_check,0);
    MLOG("argument invalidation probe: serial replacement and snapshot retirement passed (5 cache modes)");
    return checked;
}
