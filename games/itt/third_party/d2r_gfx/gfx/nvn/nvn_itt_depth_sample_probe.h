/* Included by nvn_mtl.m. Synthetic Metal4 equivalence fixture, not guest shaders.
   Compare stencil-update/depth-sample pairs with and without encoder boundaries.
   Production start/end barriers remain unchanged. No runtime optimization here. */
int mtl_probe_depth_sample(void) {
    mtl_init(); const NSUInteger W = 32, H = 32;
    NSError *err = nil;
    NSString *src = @"#include <metal_stdlib>\nusing namespace metal;\n"
        "vertex float4 vs(uint i [[vertex_id]]) { float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)}; return float4(p[i],0.25,1); }\n"
        "fragment float4 fs(float4 p [[position]], texture2d<float> z [[texture(0)]]) { return float4(z.read(uint2(p.xy)).r,0.5,0.75,1); }\n";
    id<MTLLibrary> lib = totk_msl_library(src, &err);
    if (!lib) MDIE("depth sample probe MSL: %s", err.localizedDescription.UTF8String);
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [lib newFunctionWithName:@"vs"];
    pd.depthAttachmentPixelFormat = pd.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    pd.colorAttachments[0].writeMask = MTLColorWriteMaskNone;
    id<MTLRenderPipelineState> stencilPS = [g_dev newRenderPipelineStateWithDescriptor:pd error:&err];
    pd.fragmentFunction = [lib newFunctionWithName:@"fs"]; pd.colorAttachments[0].writeMask = MTLColorWriteMaskAll;
    id<MTLRenderPipelineState> samplePS = [g_dev newRenderPipelineStateWithDescriptor:pd error:&err];
    if (!stencilPS || !samplePS) MDIE("depth sample probe PSO: %s", err.localizedDescription.UTF8String);
    MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
    dd.depthCompareFunction = MTLCompareFunctionAlways; dd.depthWriteEnabled = NO;
    MTLStencilDescriptor *st = [MTLStencilDescriptor new]; st.stencilCompareFunction = MTLCompareFunctionAlways;
    st.depthStencilPassOperation = MTLStencilOperationReplace; st.writeMask = 15;
    dd.frontFaceStencil = dd.backFaceStencil = st;
    id<MTLDepthStencilState> write = [g_dev newDepthStencilStateWithDescriptor:dd];
    st.stencilCompareFunction = MTLCompareFunctionEqual; st.depthStencilPassOperation = MTLStencilOperationKeep;
    st.readMask = 15; st.writeMask = 0;
    id<MTLDepthStencilState> sample = [g_dev newDepthStencilStateWithDescriptor:dd];
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:W height:H mipmapped:NO];
    td.storageMode = MTLStorageModePrivate; td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLTexture> color = [g_dev newTextureWithDescriptor:td]; td.pixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    id<MTLTexture> depth = [g_dev newTextureWithDescriptor:td];
    if (!color || !depth || !write || !sample) MDIE("depth sample probe resources");
    res_add(color); res_add(depth);
    id<MTLBuffer> rc = probe_buf(W*H*4), rd = probe_buf(W*H*4), rs = probe_buf(W*H);
    MTL4ArgumentTableDescriptor *ad = [MTL4ArgumentTableDescriptor new]; ad.maxTextureBindCount = 1;
    id<MTL4ArgumentTable> at = [g_dev newArgumentTableWithDescriptor:ad error:&err];
    if (!at) MDIE("depth sample probe arguments");
    [at setTexture:depth.gpuResourceID atIndex:0];
    uint8_t baseline[32*32*9]; int checked = 0;
    for (int merged = 0; merged < 2; merged++) {
        mtl_sub_begin(); MTL4RenderPassDescriptor *rp = [MTL4RenderPassDescriptor new];
        rp.colorAttachments[0].texture = color; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        rp.depthAttachment.texture = depth; rp.depthAttachment.loadAction = MTLLoadActionClear; rp.depthAttachment.storeAction = MTLStoreActionStore; rp.depthAttachment.clearDepth = 0.625;
        rp.stencilAttachment.texture = depth; rp.stencilAttachment.loadAction = MTLLoadActionClear; rp.stencilAttachment.storeAction = MTLStoreActionStore; rp.stencilAttachment.clearStencil = 0xa0;
        /* Always materialize initial depth. Testing stencil-only dependencies, not reads of pending depth clears. */
        id<MTL4RenderCommandEncoder> en = enc_r_begin(rp); enc_r_end(en);
        rp.colorAttachments[0].loadAction = rp.depthAttachment.loadAction = rp.stencilAttachment.loadAction = MTLLoadActionLoad;
        en = nil;
        for (unsigned round = 0; round < 32; round++) {
            if (!en) en = enc_r_begin(rp);
            enc_r_viewport(en, (MTLViewport){0,0,W,H,0,1}); enc_r_scissor(en, (MTLScissorRect){(round%4)*8,0,8,H});
            enc_r_cull(en, MTLCullModeNone); enc_r_stencil_ref(en, (round%15)+1, (round%15)+1);
            enc_r_pso(en, stencilPS); enc_r_dss(en, write); enc_r_draw(en, MTLPrimitiveTypeTriangle, 0, 3);
            if (!merged) { enc_r_end(en); en = enc_r_begin(rp); }
            enc_r_viewport(en, (MTLViewport){0,0,W,H,0,1}); enc_r_scissor(en, (MTLScissorRect){(round%4)*8,0,8,H});
            enc_r_cull(en, MTLCullModeNone); enc_r_stencil_ref(en, (round%15)+1, (round%15)+1);
            enc_r_pso(en, samplePS); enc_r_dss(en, sample);
            /* This fixture uses a private argument table on the direct encoding path. */
            [en setArgumentTable:at atStages:MTLRenderStageFragment];
            if (!(merged && getenv("TOTK_PROBE_NEGATIVE"))) enc_r_draw(en, MTLPrimitiveTypeTriangle, 0, 3);
            if (!merged) { enc_r_end(en); en = nil; }
        }
        if (en) enc_r_end(en);
        id<MTL4ComputeCommandEncoder> b = blit();
        enc_b_copy_texture_buffer(b,color,0,0,MTLOriginMake(0,0,0),MTLSizeMake(W,H,1),rc,0,W*4,W*H*4);
        [b copyFromTexture:depth sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(W,H,1) toBuffer:rd destinationOffset:0 destinationBytesPerRow:W*4 destinationBytesPerImage:W*H*4 options:MTLBlitOptionDepthFromDepthStencil];
        [b copyFromTexture:depth sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(W,H,1) toBuffer:rs destinationOffset:0 destinationBytesPerRow:W destinationBytesPerImage:W*H options:MTLBlitOptionStencilFromDepthStencil];
        mtl_sub_commit(); mtl_finish();
        for (NSUInteger i = 0; i < W*H; i++) {
            const uint8_t *c = (const uint8_t *)rc.contents + i*4;
            unsigned last = 28+(i%W)/8, want = 0xa0 | (last%15)+1;
            if (abs(c[0]-159)>1 || c[1]!=128 || c[2]!=191 || c[3]!=255 || ((float *)rd.contents)[i]!=0.625f || ((uint8_t *)rs.contents)[i]!=want)
                MDIE("depth sample probe mismatch merged=%d pixel=%lu color=%u,%u,%u,%u depth=%g stencil=%u want=%u", merged,i,c[0],c[1],c[2],c[3],((float *)rd.contents)[i],((uint8_t *)rs.contents)[i],want);
            checked += 6;
        }
        size_t offset = 0;
        for (id<MTLBuffer> r in @[rc,rd,rs]) { size_t n = r == rs ? W*H : W*H*4;
            if (!merged) memcpy(baseline+offset,r.contents,n);
            else if (memcmp(baseline+offset,r.contents,n)) MDIE("depth sample probe split/merged bytes differ");
            offset += n;
        }
    }
    for (id r in @[color,depth,rc,rd,rs]) defer_obj_release(r);
    return checked;
}
