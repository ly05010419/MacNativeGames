/* Synthetic GPU checks for the production depth-feedback snapshot and binding.
   No title shaders/data. Exercise pending clears, read-only reuse, pass changes,
   depth writes, source replacement and the depth-only/depth-stencil formats. */
int mtl_probe_depth_feedback(void) {
    mtl_init();atomic_store(&g_att_snapshot,1);int checked=0;
    NSError *error=nil;
    NSString *source=@"#include <metal_stdlib>\nusing namespace metal; vertex float4 vs(uint i [[vertex_id]]) {float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};return float4(p[i],0,1);} fragment float4 fs(float4 p [[position]],depth2d<float> t [[texture(0)]]){float d=t.read(uint2(p.xy));return float4(d,0.5,1-d,1);}";
    id<MTLLibrary> lib=[g_dev newLibraryWithSource:source options:nil error:&error];
    MTLRenderPipelineDescriptor *pd=[MTLRenderPipelineDescriptor new];pd.vertexFunction=[lib newFunctionWithName:@"vs"];pd.fragmentFunction=[lib newFunctionWithName:@"fs"];pd.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;
    id<MTLRenderPipelineState> ps=lib?[g_dev newRenderPipelineStateWithDescriptor:pd error:&error]:nil;
    if(!ps)MDIE("depth feedback probe shader: %s",error.localizedDescription.UTF8String);
    NvShader *fs=[NvShader new];fs->stage=1;fs->rf.nb=1;fs->rf.b[0]=(nvn_rbind){.kind=NVN_RB_TEX,.reg=0};
    NvProg *pr=[NvProg new];pr->sh[1]=fs;
    id<MTLBuffer> rb=probe_buf(256);
    const MTLPixelFormat formats[]={MTLPixelFormatDepth16Unorm,MTLPixelFormatDepth32Float,MTLPixelFormatDepth32Float_Stencil8};
    const uint32_t nvnformats[]={0x32,0x34,0x36};
    for(int format=0;format<3;format++) {
        MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:formats[format] width:8 height:8 mipmapped:NO];td.storageMode=MTLStorageModePrivate;td.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;
        id<MTLTexture> original=[g_dev newTextureWithDescriptor:td];if(!original)MDIE("depth feedback probe allocation");res_add(original);
        td.pixelFormat=MTLPixelFormatRGBA8Unorm;id<MTLTexture> color=[g_dev newTextureWithDescriptor:td];res_add(color);
        tex_t depth={0},ct={0};depth.serial=0xffffd800+format*2;ct.serial=depth.serial+1;
        depth.d=(nvn_texdesc){.width=8,.height=8,.depth=1,.levels=1,.target=1,.format=nvnformats[format],.swizzle_r=2,.swizzle_g=3,.swizzle_b=4,.swizzle_a=5};ct.d=depth.d;ct.d.format=0x25;
        serial_put(depth.serial,original);serial_put(ct.serial,color);mtl_reg_tex(57,&depth,NULL);
        mtl_sub_begin();op_hdr end={OP_END,8};mtl_exec((const uint8_t *)&end);
        memset(&R,0,sizeof R);R.has_rt=1;R.rt.n=1;R.rt.color[0]=&ct;R.rt.depth=&depth;S.tex[1][0]=57;S.ds.dt=1;S.ds.dw=0;S.ds.df=8;
        id<MTLTexture> first=nil;
        for(int step=0;step<5;step++) {
            if(g_att_copy_n){for(int i=0;i<g_att_copy_n;i++)g_att_copy_tex[i]=nil;g_att_copy_n=0;args_dirty_all();}
            if(step==0 || step==2){rp_end();R.clear_d=1;R.cd=step?.75:.25;R.clear_s=format==2;R.cs=19;}
            if(step==3)g_depth_write_epoch++; /* a write in the current pass invalidates the copy */
            if(step==4){rp_end();MTLTextureDescriptor *rd=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:formats[format] width:8 height:8 mipmapped:NO];rd.storageMode=MTLStorageModePrivate;rd.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;id<MTLTexture> replacement=[g_dev newTextureWithDescriptor:rd];if(!replacement)MDIE("depth feedback replacement");res_add(replacement);serial_put(depth.serial,replacement);R.clear_d=1;R.cd=.5;R.clear_s=format==2;original=replacement;}
            snapshot_feedback_inputs(pr);
            registry_lock();id<MTLTexture> input=att_copy_lookup(57);registry_unlock();
            if(!input || input==original)MDIE("depth feedback probe: sampling a live attachment (format %d step %d)",format,step);
            if(step==0)first=input;
            if(step==1 && input!=first)MDIE("depth feedback probe: read-only pass did not reuse");
            if(step==2 && input==first)MDIE("depth feedback probe: stale snapshot after clear");
            checked+=3;
            rp_begin_ex(0);depth_feedback_note_pass();
            if(step==1 || step==2 || step==3 || step==4) {
                rp_end();mtl_sub_commit();mtl_finish();
                MTLRenderPassDescriptor *rp=[MTLRenderPassDescriptor renderPassDescriptor];rp.colorAttachments[0].texture=color;rp.colorAttachments[0].loadAction=MTLLoadActionClear;rp.colorAttachments[0].storeAction=MTLStoreActionStore;
                id<MTLCommandBuffer> cb=q_cb();id<MTLRenderCommandEncoder> en=[cb renderCommandEncoderWithDescriptor:rp];[en setRenderPipelineState:ps];[en setFragmentTexture:input atIndex:0];[en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];[en endEncoding];
                id<MTLBlitCommandEncoder> bl=[cb blitCommandEncoder];[bl copyFromTexture:color sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(8,8,1) toBuffer:rb destinationOffset:0 destinationBytesPerRow:32 destinationBytesPerImage:256];[bl endEncoding];cb_commit(cb);[cb waitUntilCompleted];
                if(cb.status==MTLCommandBufferStatusError)MDIE("depth feedback probe GPU error: %s",cb.error.localizedDescription.UTF8String);
                int red=step==1?64:step==4?128:191;
                for(int pixel=0;pixel<64;pixel++)for(int ch=0;ch<4;ch++){int want=(int[]){red,128,255-red,255}[ch];int got=((uint8_t *)rb.contents)[pixel*4+ch];if(abs(got-want)>1)MDIE("depth feedback probe: format%d step%d pixel%d channel%d=%d expected%d",format,step,pixel,ch,got,want);checked++;}
                mtl_sub_begin();mtl_exec((const uint8_t *)&end);
            }
        }
        rp_end();mtl_sub_commit();mtl_finish();memset(&R,0,sizeof R);state_reset();mtl_reg_tex(57,NULL,NULL);serial_del(depth.serial);serial_del(ct.serial);defer_obj_release(color);defer_obj_release(original);
    }
    g_att_copy_n=0;g_depth_feedback_copy=nil;g_depth_feedback_source=nil;defer_obj_release(rb);
    return checked;
}
