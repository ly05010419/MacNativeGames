/* Production pso_get/derived functions/encode_args, with an independent GPU pixel oracle.
 * Separate process and private profile/cache required. No game assets are modified. */
int mtl_probe_sampler_border(void) {
    mtl_init(); int checked=0; NSError *error=nil;
    const char *which=getenv("TOTK_BORDER_PROBE_CASE");
    if(which && strcmp(which,"vertex-colour") && strcmp(which,"mixed-axis"))MDIE("unknown border probe case %s",which);
    NSMutableArray *program_lifetimes=[NSMutableArray array];
    memset(&S,0,sizeof S); S.chmask.mask=15;
    for(int kind=0;kind<3;kind++) {
    S.chmask.mask=15;
    const uint8_t base_pixel[4]={64,128,192,255}, layer_pixel[4]={192,64,128,255};
    const uint8_t *pixel=kind?layer_pixel:base_pixel;
    const uint8_t mip_pixel[4]={32,96,160,224};
    MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:2 height:2 mipmapped:YES];
    td.storageMode=MTLStorageModeShared;td.usage=MTLTextureUsageShaderRead;
    if(kind==1){td.textureType=MTLTextureType2DArray;td.arrayLength=2;}
    if(kind==2){td.textureType=MTLTextureType3D;td.depth=2;}
    id<MTLTexture> input=[g_dev newTextureWithDescriptor:td];res_add(input);
    uint8_t planes[32];
    for(int plane=0;plane<2;plane++)for(int px=0;px<4;px++)memcpy(planes+plane*16+px*4,plane?layer_pixel:base_pixel,4);
    if(kind==2){[input replaceRegion:MTLRegionMake3D(0,0,0,2,2,2) mipmapLevel:0 slice:0 withBytes:planes bytesPerRow:8 bytesPerImage:16];[input replaceRegion:MTLRegionMake3D(0,0,0,1,1,1) mipmapLevel:1 slice:0 withBytes:mip_pixel bytesPerRow:4 bytesPerImage:4];}
    else {for(int layer=0;layer<(kind==1?2:1);layer++){[input replaceRegion:MTLRegionMake2D(0,0,2,2) mipmapLevel:0 slice:layer withBytes:planes+layer*16 bytesPerRow:8 bytesPerImage:16];[input replaceRegion:MTLRegionMake2D(0,0,1,1) mipmapLevel:1 slice:layer withBytes:mip_pixel bytesPerRow:4 bytesPerImage:4];}}
    tex_t t={0};t.serial=0xffffeb10+kind;t.d=(nvn_texdesc){.width=2,.height=2,.depth=kind?2:1,.levels=2,.format=0x25,.target=kind==2?2:kind==1?4:1,.swizzle_r=2,.swizzle_g=3,.swizzle_b=4,.swizzle_a=5};
    serial_put(t.serial,input);mtl_reg_tex(71,&t,NULL);
    td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:4 height:4 mipmapped:NO];
    td.storageMode=MTLStorageModePrivate;td.usage=MTLTextureUsageRenderTarget;
    id<MTLTexture> output=[g_dev newTextureWithDescriptor:td];res_add(output);
    id<MTLBuffer> readback=probe_buf(1024);
    const float colours[][4]={{.5,.5,.5,.5},{.25,.4,.75,.9},{.5,.5,.5,.5},{0,0,0,0},{1,1,1,1}};
    for(int phase=0;phase<2;phase++) for(int outside=0;outside<7;outside++) {
        int stage=phase?0:1;
        if(which && (!strcmp(which,"vertex-colour") ? !(stage==0 && outside==1) : !(stage==1 && outside==2)))continue;
        NSString *sample=[NSString stringWithFormat:@"spvDescriptorSet0.tex.sample(spvDescriptorSet0.sam,%@(%s,%s%@)%@,level(%s))",kind==2?@"float3":@"float2",outside==3||outside==4?"0.0":outside==6?"1.0":outside==1?"-0.5":"0.5",outside==2?"1.5":outside==5?"-1.5":"0.5",kind==2?@",0.75":@"",kind==1?@",1":@"",outside==4?"0.5":"0"];
        NSString *args=[NSString stringWithFormat:@"struct Args\n{\n%@<float> tex [[id(0)]];\nsampler sam [[id(1)]];\n};\n",kind==2?@"texture3d":kind==1?@"texture2d_array":@"texture2d"];
        NSString *vs_source=[NSString stringWithFormat:@"#include <metal_stdlib>\nusing namespace metal;\n%@struct main0_out\n{\nfloat4 gl_Position [[position]];\nfloat4 colour [[user(locn0)]];\n};\nvertex main0_out main0(uint i [[vertex_id]]%@)\n{\nmain0_out out = {};\nfloat2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};\nout.gl_Position=float4(p[i],0,1);\nout.colour=%@;\nreturn out;\n}\n",stage==0?args:@"",stage==0?@",constant Args &spvDescriptorSet0 [[buffer(0)]]":@"",stage==0?sample:@"float4(1)"];
        NSString *fs_source=[NSString stringWithFormat:@"#include <metal_stdlib>\nusing namespace metal;\n%@struct main0_in\n{\nfloat4 colour [[user(locn0)]];\n};\nfragment float4 fs0(main0_in in [[stage_in]]%@) {return %@;}\n",stage==1?args:@"",stage==1?@",constant Args &spvDescriptorSet0 [[buffer(0)]]":@"",stage==1?sample:@"in.colour"];
        NvShader *sh[2];NvProg *program=[NvProg new];[program_lifetimes addObject:program];
        for(int st=0;st<2;st++) {
            NSString *source=st?fs_source:vs_source;
            id<MTLLibrary> lib=[g_dev newLibraryWithSource:source options:nil error:&error];if(!lib)MDIE("border probe shader: %s",error.localizedDescription.UTF8String);
            sh[st]=[NvShader new];[program_lifetimes addObject:sh[st]];sh[st]->stage=st;sh[st]->src=source;sh[st]->fn=[lib newFunctionWithName:st?@"fs0":@"main0"];
            snprintf(sh[st]->key,sizeof sh[st]->key,"probe-border-%d-%d-%d-%d",kind,stage,outside,st);
            if(st==stage){
                sh[st]->rf.nb=2;sh[st]->rf.b[0]=(nvn_rbind){.kind=NVN_RB_TEX,.aid=0,.reg=0,.dtt=input.textureType};
                sh[st]->rf.b[1]=(nvn_rbind){.kind=NVN_RB_SAMPT,.aid=1,.reg=0};
                sh[st]->ae=[sh[st]->fn newArgumentEncoderWithBufferIndex:0];sh[st]->arg_len=sh[st]->ae.encodedLength;
            }
            program->sh[st]=sh[st];
        }
        id<MTLRenderPipelineState> first_gray=nil,first_native=nil;
        for(unsigned order=0;order<sizeof colours/sizeof *colours;order++) {
            const unsigned native_first[5]={3,4,0,1,2};unsigned c=outside>=3?native_first[order]:order;
            samp_par sampler={.minf=outside==4?5:outside==3||outside==6?1:0,.magf=outside==3||outside==4||outside==6?1:0,.ws=4,.wt=4,.wr=4,.lod_max=1,.aniso=1};memcpy(sampler.border,colours[c],sizeof sampler.border);if(outside==2 && c==2)sampler.wt=1;if(outside==4 && c==2)sampler.minf=1;if(outside==5)sampler.wt=c==2?2:6;mtl_reg_samp(81,&sampler);
            p_bind_handles tex={.stage=stage,.n=1,.h={71|(UINT64_C(81)<<20)}};
            mtl_sub_begin();op_hdr end={OP_END,8};mtl_exec((const uint8_t *)&end);state_op(OP_BIND_TEX,&tex);
            memset(g_pass.cfmt,0,sizeof g_pass.cfmt);g_pass.cfmt[0]=MTLPixelFormatRGBA32Float;g_pass.dfmt=0;g_pass.sfmt=0;g_pass.samples=1;g_pass.layered=0;g_pass.serial++;
            id<MTLRenderPipelineState> pipeline=pso_get(program,nil);if(!pipeline)MDIE("border probe PSO");
            {   // the recipe the pipeline is recorded with (pso_adopt B row) reads back exactly and names the same variant (warm_border)
                border_in b,r;NSString *key=border_key(sh[stage],stage,b.c,b.a,b.x,b.p,&b.n);
                if(key){
                    if(!border_parse(key,&r)||r.n!=b.n)MDIE("border probe recipe %s does not read back",key.UTF8String);
                    for(int i=0;i<b.n;i++){const samp_par *bp=&b.p[i],*rp=&r.p[i];int f=border_filtered(bp);
                        if(f!=border_filtered(rp)||(f&&(rp->ws!=bp->ws||rp->wt!=bp->wt||rp->wr!=bp->wr||rp->magf!=bp->magf||rp->minf!=bp->minf||rp->aniso!=bp->aniso||rp->lod_min!=bp->lod_min||rp->lod_max!=bp->lod_max)))MDIE("border probe recipe %s lost filter state",key.UTF8String);
                        if(f)checked++;}
                    for(int i=0;i<b.n;i++)if(r.a[i]!=b.a[i]||r.x[i]!=b.x[i]||memcmp(r.c[i],b.c[i],sizeof r.c[i]))MDIE("border probe recipe %s read back differently",key.UTF8String);
                    id<MTLFunction> from_draw=stage?fs_fn_border(sh[1],0,NULL):vs_fn_for(sh[0],sh[1],0,1,NULL);
                    id<MTLFunction> from_recipe=stage?fs_fn_border_with(sh[1],0,key,r,NULL):vs_fn_variant(sh[0],sh[1],0,key,r,NULL);
                    if(!from_draw||from_draw!=from_recipe)MDIE("border probe recipe %s names another variant",key.UTF8String);
                    checked+=2;
                }
            }
            if(c==0)first_gray=pipeline;
            if(c==2 && outside!=2 && outside!=4 && outside!=5){if(pipeline!=first_gray)MDIE("border probe lost grey PSO cache hit");checked++;}
            if(c==2 && outside==4){if(pipeline==first_gray)MDIE("border probe filter state reused stale PSO");checked++;}
            if(c==3)first_native=pipeline;
            if(c==4){if(pipeline!=first_native)MDIE("border probe native sampler colours changed the PSO");checked++;}
            MTL4RenderPassDescriptor *rp=[MTL4RenderPassDescriptor new];rp.colorAttachments[0].texture=output;rp.colorAttachments[0].loadAction=MTLLoadActionClear;rp.colorAttachments[0].storeAction=MTLStoreActionStore;rp.renderTargetWidth=4;rp.renderTargetHeight=4;
            id<MTL4RenderCommandEncoder> en=enc_r_begin(rp);enc_r_pso(en,pipeline);enc_r_cull(en,MTLCullModeNone);enc_r_viewport(en,(MTLViewport){0,0,4,4,0,1});enc_r_scissor(en,(MTLScissorRect){0,0,4,4});
            encode_args((id<MTLCommandEncoder>)en,sh[stage],stage,stage==0);enc_r_draw(en,MTLPrimitiveTypeTriangle,0,3);enc_r_end(en);
            enc_b_copy_texture_buffer(blit(),output,0,0,MTLOriginMake(0,0,0),MTLSizeMake(4,4,1),readback,0,256,1024);mtl_sub_commit();mtl_finish();
            for(int y=0;y<4;y++)for(int x=0;x<4;x++)for(int ch=0;ch<4;ch++) {
                float inside=pixel[ch]/255.0f;
                if(outside==4 && c!=2)inside=.5f*inside+.5f*mip_pixel[ch]/255.0f;
                float want=outside==3||outside==4||outside==6?.5f*(inside+colours[c][ch]):outside==1 || (outside==2 && c!=2)?colours[c][ch]:inside;
                // At mip1 the 3D depth is one: z=.75 also blends 25% R border.
                if(kind==2 && outside==4 && c!=2)want=.25f*pixel[ch]/255.0f+.1875f*mip_pixel[ch]/255.0f+.5625f*colours[c][ch];
                float got=((float *)((uint8_t *)readback.contents+y*256))[x*4+ch];
                if(fabsf(got-want)>.002f)MDIE("border probe mismatch kind%d stage%d outside%d colour%u channel%d got%g want%g",kind,stage,outside,c,ch,got,want);
                checked++;
            }
        }
    }
    mtl_reg_tex(71,NULL,NULL);serial_del(t.serial);res_remove(input);res_remove(output);res_remove(readback);memset(&S,0,sizeof S);args_dirty_all();
    }
    return checked;
}
