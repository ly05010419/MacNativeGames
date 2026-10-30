/* Asset-free readback regression for the production indexed-quad converter. */
int mtl_probe_indexed_quads(void) {
    mtl_init(); int checked = 0;
    const uint32_t counts[] = {0,1,3,4,5,8,257,4099}, shifts[] = {0,1,5,15};
    id<MTLBuffer> ref = probe_buf(32768), src = probe_buf(32768);
    for (unsigned width = 1; width <= 4; width *= 2) for (unsigned restart = 0; restart < 3; restart++)
    for (unsigned c = 0; c < sizeof counts / sizeof *counts; c++) for (unsigned k = 0; k < sizeof shifts / sizeof *shifts; k++) {
        uint32_t n = counts[c], sh = shifts[k], marker = restart == 2 ? 17 : UINT32_MAX;
        uint32_t mask = width == 4 ? UINT32_MAX : (1u << (width*8))-1;
        memset(src.contents, 0x66, src.length); memset(ref.contents, 0xcc, ref.length);
        uint32_t input[4099], expected[6144], q[4], used = 0, nq = 0;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t v = (i*31337u+7u)&mask;
            if (i%11 == 5 || i%11 == 6) v = marker&mask;
            input[i] = v; memcpy((uint8_t *)ref.contents+sh+i*width, &v, width);
        }
        for (uint32_t i = 0; i < n; i++) {
            if (restart && input[i] == (marker&mask)) { nq = 0; continue; }
            q[nq++] = input[i]; if (nq != 4) continue;
            /* Two triangles share the diagonal from corner 1 to corner 3.
               Both retain the quad's fourth corner as provoking vertex. */
            const unsigned corners[6] = {3,0,1,3,1,2};
            for (unsigned j = 0; j < 6; j++) expected[used++] = q[corners[j]];
            nq = 0;
        }
        mtl_sub_begin();
        enc_b_copy_buffer(blit(), ref, 0, src, 0, src.length);
        quad_gpu out = quad_expand_gpu(src.gpuAddress+sh, n, width, (p_primitive_restart){restart != 0, marker}, -7, 3, 9);
        mtl_sub_commit(); mtl_finish();
        const uint32_t *args = (const void *)((const uint8_t *)out.args.contents+out.arg_off);
        uint32_t want[5] = {used,3,0,(uint32_t)-7,9};
        if (memcmp(args,want,sizeof want)) MDIE("quad argument readback width %u count %u offset %u restart %u",width,n,sh,restart);
        const uint32_t *actual = (const void *)((const uint8_t *)out.indices.contents+out.off);
        if (memcmp(actual,expected,used*4)) MDIE("quad index readback width %u count %u offset %u restart %u",width,n,sh,restart);
        checked += 5 + used;
    }
    /* Real rasterization verifies winding, last-corner flat values, base vertex,
       instance/base-instance arguments and untouched pixels on the actual draw path. */
    NSError *e = nil;
    NSString *msl = @"#include <metal_stdlib>\nusing namespace metal;\n"
      "struct V {float4 p [[position]]; float4 c [[flat]];};"
      "vertex V vs(uint id [[vertex_id]], uint inst [[instance_id]]) {"
      " uint v=id-3; float2 p[4]={float2(-.9,-.8),float2(-.1,-.8),float2(-.1,.8),float2(-.9,.8)};"
      " V o;o.p=float4(p[v]+float2(float(inst-5)*1.0,0),0,1);o.c=v==3?float4(1,0,0,1):float4(0,1,0,1);return o;}"
      "fragment float4 fs(V in [[stage_in]]){return in.c;}";
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:msl options:nil error:&e]; if (!lib) MDIE("quad pixel shader: %s",e.localizedDescription.UTF8String);
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new]; pd.vertexFunction=[lib newFunctionWithName:@"vs"];pd.fragmentFunction=[lib newFunctionWithName:@"fs"];pd.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;
    id<MTLRenderPipelineState> ps=[g_dev newRenderPipelineStateWithDescriptor:pd error:&e];if(!ps)MDIE("quad pixel pipeline: %s",e.localizedDescription.UTF8String);
    MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:64 height:64 mipmapped:NO];td.usage=MTLTextureUsageRenderTarget;td.storageMode=MTLStorageModePrivate;
    id<MTLTexture> target=[g_dev newTextureWithDescriptor:td];res_add(target);id<MTLBuffer> rb=probe_buf(64*64*4);
    void *mem=NULL;if(posix_memalign(&mem,16384,16384))MDIE("quad probe pool");memset(mem,0,16384);mtl_pool_add(mem,16384);
    for (unsigned width=1; width<=4; width*=2) for(unsigned cull=0;cull<2;cull++) {
        for(unsigned j=0;j<4;j++)memcpy((uint8_t*)ref.contents+1+j*width,&j,width);
        void *mb;uint64_t mo,gpu;if(!mtl_gpu_addr((uintptr_t)mem,16384,&mb,&mo,&gpu))MDIE("quad probe pool lookup");
        p_draw_idx packet={.prim=7,.itype=width==1?0:width==2?1:2,.count=4,.base_vertex=3,.base_instance=5,.instances=2,.ibuf=(uintptr_t)mem+1};
        mtl_sub_begin();enc_b_copy_buffer(blit(),ref,0,(__bridge id<MTLBuffer>)mb,mo,32);S.restart=(p_primitive_restart){0};quad_prepare(&packet);
        MTL4RenderPassDescriptor *rp=[MTL4RenderPassDescriptor new];rp.colorAttachments[0].texture=target;rp.colorAttachments[0].loadAction=MTLLoadActionClear;rp.colorAttachments[0].clearColor=MTLClearColorMake(0,0,0,1);rp.colorAttachments[0].storeAction=MTLStoreActionStore;rp.renderTargetWidth=64;rp.renderTargetHeight=64;
        id<MTL4RenderCommandEncoder> en=enc_r_begin(rp);enc_r_pso(en,ps);enc_r_viewport(en,(MTLViewport){0,0,64,64,0,1});enc_r_scissor(en,(MTLScissorRect){0,0,64,64});enc_r_cull(en,MTLCullModeBack);enc_r_winding(en,cull?MTLWindingClockwise:MTLWindingCounterClockwise);
        if(!getenv("TOTK_PROBE_NEGATIVE"))quad_draw(en,&packet);enc_r_end(en);
        enc_b_copy_texture_buffer(blit(),target,0,0,MTLOriginMake(0,0,0),MTLSizeMake(64,64,1),rb,0,256,16384);enc_b_end(g_blit);g_blit=nil;mtl_sub_commit();mtl_finish();
        const uint8_t *px=rb.contents;
        for(unsigned y=0;y<64;y++)for(unsigned x=0;x<64;x++) {
            /* Pixel centers are strictly inside/outside the synthetic rectangles. */
            int lit=!cull && y>=6 && y<58 && ((x>=3&&x<29)||(x>=35&&x<61));
            const uint8_t *v=px+4*(y*64+x);
            if(v[0]!=(lit?255:0)||v[1]||v[2]||v[3]!=255)MDIE("quad pixel mismatch width %u winding %u at %u,%u got %u,%u,%u,%u",width,cull,x,y,v[0],v[1],v[2],v[3]);checked++;
        }
    }
    mtl_pool_del(mem);mtl_finish();free(mem);defer_obj_release(ref);defer_obj_release(src);defer_obj_release(target);defer_obj_release(rb);
    return checked;
}
