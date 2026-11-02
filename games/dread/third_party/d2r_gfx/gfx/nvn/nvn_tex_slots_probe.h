/* Included by nvn_mtl.m: production encode_args, descriptor publication and GPU
   pixel checks. Inputs are immutable until mtl_finish. Probe caches stay isolated. */
int mtl_probe_tex_slots(void) {
    mtl_init(); int checked = 0;
    const char *ne = getenv("TOTK_PROBE_NEGATIVE"); int negative = ne ? atoi(ne) : 0;
    void *mem = NULL;
    if (posix_memalign(&mem, 16384, 16384) || !mem) MDIE("texture slots probe memory");
    memset(mem, 0, 16384); mtl_pool_add(mem, 16384);
    for (int i = 0; i < 16; i++) ((float *)((uint8_t *)mem + 16*i))[0] = i & 1 ? .75f : .25f;
    const uint8_t pixels[3][8] = {{64,128,192,255,192,64,128,255},
        {32,96,160,255,160,32,96,255}, {96,160,32,255,32,96,160,255}};
    samp_par clamp = {.lod_max=16,.aniso=1}, repeat = clamp; repeat.ws = 1;
    id<MTLTexture> sources[3], output;
    id<MTLBuffer> typed = probe_buf(3*256), rb = probe_buf(2048);
    NSError *error = nil;
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:64 height:8 mipmapped:NO];
    td.storageMode = MTLStorageModePrivate; td.usage = MTLTextureUsageRenderTarget;
    output = [g_dev newTextureWithDescriptor:td]; res_add(output);
    op_hdr end = {OP_END,8};
    for (int kind = 0; kind < 3; kind++) {
        tex_t textures[2] = {0};
        for (int i = 0; i < 3; i++) {
            if (kind == 2) {
                memcpy((uint8_t *)typed.contents + i*256, pixels[i], 8);
                MTLTextureDescriptor *d = [MTLTextureDescriptor textureBufferDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:2 resourceOptions:MTLResourceStorageModeShared usage:MTLTextureUsageShaderRead];
                sources[i] = [typed newTextureWithDescriptor:d offset:i*256 bytesPerRow:256];
            } else {
                MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:2 height:1 mipmapped:NO];
                d.storageMode = MTLStorageModeShared; d.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
                sources[i] = [g_dev newTextureWithDescriptor:d];
                [sources[i] replaceRegion:MTLRegionMake2D(0,0,2,1) mipmapLevel:0 withBytes:pixels[i] bytesPerRow:8];
            }
            if (!sources[i]) MDIE("texture slots probe resource"); res_add(sources[i]);
        }
        for (int i = 0; i < 2; i++) {
            textures[i].serial = 0xffffe100 + kind*4 + i;
            textures[i].d = (nvn_texdesc){.width=2,.height=1,.depth=1,.levels=1,.format=0x25,.target=kind==2?10:1,
                .swizzle_r=2,.swizzle_g=3,.swizzle_b=4,.swizzle_a=5};
        }
        NSString *fields = kind == 2 ? @"texture_buffer<float> t [[id(0)]]; constant float *u [[id(1)]];" :
            [NSString stringWithFormat:@"%@<float> t [[id(0)]]; sampler a [[id(1)]]; sampler b [[id(2)]]; constant float *u [[id(3)]];",kind ? @"texture2d_array" : @"texture2d"];
        NSString *body = kind == 2 ? @"return float4(a.t.read(0u).rgb,a.u[0]);" :
            [NSString stringWithFormat:@"return float4(a.t.sample(a.a,float2(1.25,0.5)%@).r,a.t.sample(a.b,float2(1.25,0.5)%@).g,a.t.read(uint2(0,0)%@).b,a.u[0]);",kind?@",0":@"",kind?@",0":@"",kind?@",0":@""];
        NSString *source = [NSString stringWithFormat:@"#include <metal_stdlib>\nusing namespace metal; struct A {%@}; vertex float4 vs(uint i [[vertex_id]]){float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};return float4(p[i],0,1);} fragment float4 fa(constant A &a [[buffer(0)]]){%@} fragment float4 fb(constant A &a [[buffer(0)]]){%@}",fields,body,body];
        id<MTLLibrary> lib = [g_dev newLibraryWithSource:source options:nil error:&error];
        if (!lib) MDIE("texture slots probe shader: %s",error.localizedDescription.UTF8String);
        NvShader *shaders[2]; id<MTLRenderPipelineState> pipelines[2]; prog_t programs[2] = {0};
        for (int i = 0; i < 2; i++) {
            NvShader *sh = [NvShader new]; shaders[i] = sh; sh->fn = [lib newFunctionWithName:i?@"fb":@"fa"]; sh->stage=1;
            snprintf(sh->key,sizeof sh->key,"probe-tex-slots-%d-%d",kind,i);
            sh->rf.nb = kind==2?2:4;
            sh->rf.b[0]=(nvn_rbind){.kind=kind==2?NVN_RB_TBUF:NVN_RB_TEX,.aid=0,.reg=0,.dtt=kind?MTLTextureType2DArray:MTLTextureType2D};
            if(kind!=2) {
                sh->rf.b[1]=(nvn_rbind){.kind=NVN_RB_SAMPT,.aid=1,.reg=0};
                sh->rf.b[2]=(nvn_rbind){.kind=NVN_RB_SAMP,.aid=2,.reg=0};
            }
            sh->rf.b[sh->rf.nb-1]=(nvn_rbind){.kind=NVN_RB_CBUF,.aid=sh->rf.nb-1,.reg=0};
            sh->ae=[sh->fn newArgumentEncoderWithBufferIndex:0]; sh->arg_len=sh->ae.encodedLength;
            if(sh->arg_len != (NSUInteger)sh->rf.nb*8) MDIE("texture slots probe argument ABI");
            MTLRenderPipelineDescriptor *pd=[MTLRenderPipelineDescriptor new]; pd.vertexFunction=[lib newFunctionWithName:@"vs"];pd.fragmentFunction=sh->fn;pd.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;
            pipelines[i]=[g_dev newRenderPipelineStateWithDescriptor:pd error:&error];
            if(!pipelines[i]) MDIE("texture slots probe pipeline: %s",error.localizedDescription.UTF8String);
        }
        for(int mode=0;mode<3;mode++) for(int submission=0;submission<2;submission++) {
            atomic_store(&g_tex_slots_on,mode!=0);atomic_store(&g_tex_slots_check,mode==2);
            atomic_store(&g_arg_epoch_on,0);g_tex_slots_negative=mode?negative:0;
            mtl_sub_begin();mtl_exec((const uint8_t *)&end);
            for(int i=0;i<2;i++){serial_put(textures[i].serial,sources[i]);mtl_reg_tex(51+i,&textures[i],NULL);}
            mtl_reg_samp(61,&clamp);mtl_reg_samp(62,&repeat);
            // Keep sampler objects alive through queued draws even when re-registering their IDs.
            registry_lock();id __attribute__((objc_precise_lifetime)) sampler0=samp_by_id(61),sampler1=samp_by_id(62);registry_unlock();
            uint8_t expected[64*8*4]; int current=0,combined=0,separate=0,unbound=0;
            g_pass.serial++;args_dirty_all();
            MTL4RenderPassDescriptor *rp=[MTL4RenderPassDescriptor new];rp.colorAttachments[0].texture=output;rp.colorAttachments[0].loadAction=MTLLoadActionClear;rp.colorAttachments[0].storeAction=MTLStoreActionStore;rp.renderTargetWidth=64;rp.renderTargetHeight=8;
            id<MTL4RenderCommandEncoder> en=enc_r_begin(rp);enc_r_cull(en,MTLCullModeNone);enc_r_viewport(en,(MTLViewport){0,0,64,8,0,1});
            p_bind_handles tex={.stage=1,.n=1,.h={51|(UINT64_C(61)<<20)}},samp={.stage=1,.n=1,.h={UINT64_C(61)<<20}};
            for(int stripe=0;stripe<16;stripe++) {
                int shader=stripe==2?1:0;
                if(stripe==4){tex.h[0]=52|(UINT64_C(61)<<20);current=1;}
                if(stripe==5){tex.h[0]=52|(UINT64_C(62)<<20);combined=1;}
                if(stripe==6){samp.h[0]=UINT64_C(62)<<20;separate=1;}
                if(stripe==7){mtl_reg_tex(52,&textures[0],NULL);current=0;}
                if(stripe==9){serial_put(textures[0].serial,sources[2]);current=2;}
                if(stripe==10){mtl_reg_samp(62,&clamp);combined=separate=0;}
                if(stripe==11){tex.h[0]=0;unbound=1;}
                if(stripe==12){tex.h[0]=51|(UINT64_C(61)<<20);unbound=0;}
                if(stripe==13){samp.h[0]=0;}
                if(stripe==14){enc_r_end(en);rp.colorAttachments[0].loadAction=MTLLoadActionLoad;en=enc_r_begin(rp);enc_r_cull(en,MTLCullModeNone);enc_r_viewport(en,(MTLViewport){0,0,64,8,0,1});g_pass.serial++;}
                p_bind_program program={&programs[shader],0x1f};state_op(OP_BIND_PROGRAM,&program);
                p_bind_buf u={1,0,(uint64_t)(uintptr_t)mem+16*stripe,16};state_op(OP_BIND_UBO,&u);
                state_op(OP_BIND_TEX,&tex);state_op(OP_BIND_SAMP,&samp);
                uint64_t hits=g_tex_slots_hits;
                enc_r_pso(en,pipelines[shader]);enc_r_scissor(en,(MTLScissorRect){stripe*4,0,4,8});
                encode_args((id<MTLCommandEncoder>)en,shaders[shader],1,0);enc_r_draw(en,MTLPrimitiveTypeTriangle,0,3);
                if(mode && !negative && (stripe==1||stripe==3||stripe==8||stripe==15) && g_tex_slots_hits<=hits) MDIE("texture slots probe did not exercise reuse kind%d stripe%d",kind,stripe);
                if(mode && !negative && (stripe==7||stripe==9||stripe==10||stripe==14) && g_tex_slots_hits!=hits) MDIE("texture slots probe failed invalidation kind%d stripe%d",kind,stripe);
                for(int y=0;y<8;y++)for(int x=stripe*4;x<stripe*4+4;x++) {
                    uint8_t *p=expected+(y*64+x)*4;
                    p[0]=unbound?0:pixels[current][kind==2||combined?0:4];
                    p[1]=unbound?0:pixels[current][kind==2||separate?1:5];
                    p[2]=unbound?0:pixels[current][2];p[3]=stripe&1?191:64;
                }
                checked++;
            }
            enc_r_end(en);enc_b_copy_texture_buffer(blit(),output,0,0,MTLOriginMake(0,0,0),MTLSizeMake(64,8,1),rb,0,256,2048);mtl_sub_commit();mtl_finish();
            for(int i=0;i<2048;i++){if(abs((int)((uint8_t *)rb.contents)[i]-expected[i])>1) MDIE("texture slots probe GPU mismatch kind%d mode%d stripe%d channel%d got%d want%d",kind,mode,(i/4)%64/4,i%4,((uint8_t *)rb.contents)[i],expected[i]);checked++;}
            (void)sampler0;(void)sampler1;
        }
        for(int i=0;i<2;i++){mtl_reg_tex(51+i,NULL,NULL);serial_del(textures[i].serial);}
        for(int i=0;i<3;i++)res_remove(sources[i]);
    }
    if(!g_tex_slots_hits||!g_tex_slots_verified) MDIE("texture slots probe missed cache/shadow paths");
    MLOG("texture slots probe: hits=%llu verified=%llu",(unsigned long long)g_tex_slots_hits,(unsigned long long)g_tex_slots_verified);
    atomic_store(&g_tex_slots_on,0);atomic_store(&g_tex_slots_check,0);g_tex_slots_negative=0;
    memset(&S,0,sizeof S);args_dirty_all();mtl_pool_del(mem);free(mem);res_remove(typed);res_remove(rb);res_remove(output);
    return checked;
}
