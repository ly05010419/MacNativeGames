/* Included by nvn_mtl.m after its texture, encoder and residency helpers. */
static pthread_mutex_t g_hdr_mu = PTHREAD_MUTEX_INITIALIZER;
static struct { uint64_t serial, frame; id<MTLTexture> texture; } g_hdr_swap[8];
static unsigned g_hdr_nswap;
static int hdr_on(void) {
#if TARGET_OS_OSX && !defined(GAME_BOTW)
    return totk_hdr_available();
#else
    return 0;
#endif
}
static void hdr_note_swap(const tex_t *t) {
    if (!hdr_on() || !t) return;
    pthread_mutex_lock(&g_hdr_mu);
    unsigned i=0;for (;i<g_hdr_nswap;i++) if(g_hdr_swap[i].serial==t->serial)break;
    if(i==g_hdr_nswap && i<8)g_hdr_swap[g_hdr_nswap++].serial=t->serial;
    pthread_mutex_unlock(&g_hdr_mu);
}
static id<MTLTexture> hdr_swap_texture(uint64_t serial, uint64_t frame) {
    id<MTLTexture> result=nil;
    if(hdr_on()) { pthread_mutex_lock(&g_hdr_mu);
        for(unsigned i=0;i<g_hdr_nswap;i++)if(g_hdr_swap[i].serial==serial && (frame==UINT64_MAX || g_hdr_swap[i].frame==frame))result=g_hdr_swap[i].texture;
        pthread_mutex_unlock(&g_hdr_mu); }
    return result;
}
/* Preserve the float canvas before the exact final copy writes to the 8-bit
 * sRGB swapchain. Never promote arbitrary guest textures or their CPU storage. */
static void hdr_capture(NvProg *pr) {
    if(!hdr_on() || !atomic_load(&g_hdr_frame_enabled) || !pr || !pr->sh[0] || !pr->sh[1] || R.rt.n!=1 || !R.rt.color[0])return;
    if(strncmp(pr->sh[1]->key,"dd3d1e0ebf73242845970b5655fac199a30f5acf747975c5e771caa49af8b12b-",65) ||
       strncmp(pr->sh[0]->key,"1a40e5f5ff257e13293c01b8e293a619d2ae8cdcffd8e4083e04dd7f59fd2273-",65))return;
    unsigned slot=8;pthread_mutex_lock(&g_hdr_mu);
    for(unsigned i=0;i<g_hdr_nswap;i++)if(g_hdr_swap[i].serial==R.rt.color[0]->serial)slot=i;
    pthread_mutex_unlock(&g_hdr_mu);if(slot==8)return;
    int tid=(int)(S.tex[1][0]&0xfffff);
    id<MTLTexture> src=tid>0?(__bridge id<MTLTexture>)tex_fast(tid):nil;
    id<MTLTexture> dst=tex_mtl(R.rt.color[0]);
    if(!src || !dst || src.pixelFormat!=MTLPixelFormatRG11B10Float || src.textureType!=MTLTextureType2D ||
       src.width>dst.width || src.height>dst.height || !S.has_vp || S.vp[0][0]!=0 || S.vp[0][1]!=0 ||
       S.vp[0][2]!=(float)src.width || S.vp[0][3]!=(float)src.height || (S.color.blend_en&1) || S.ds.dt || S.ds.st)return;
    rp_end();blit_close();rs_sync();
    id<MTLTexture> out=hdr_swap_texture(R.rt.color[0]->serial,UINT64_MAX);
    if(!out || out.width!=dst.width || out.height!=dst.height) {
        MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:dst.width height:dst.height mipmapped:NO];
        d.storageMode=MTLStorageModePrivate;d.usage=MTLTextureUsageShaderRead|MTLTextureUsageRenderTarget;
        if(out)defer_obj_release(out);
        out=[g_dev newTextureWithDescriptor:d];if(!out)MDIE("HDR float swapchain allocation failed");
        res_add(out);res_commit();pthread_mutex_lock(&g_hdr_mu);g_hdr_swap[slot].texture=out;pthread_mutex_unlock(&g_hdr_mu);
        MLOG("[hdr] float swapchain %llu: %lux%lu",R.rt.color[0]->serial,(unsigned long)dst.width,(unsigned long)dst.height);
    }
    /* A render copy handles RG11B10F -> RGBA16F without a format-changing blit.
     * present_encode uses the current viewport below, with no output upscale. */
    static id<MTLRenderPipelineState> pso;
    if(!pso) {
        NSString *msl=@"#include <metal_stdlib>\nusing namespace metal;\nstruct V{float4 p [[position]];float2 uv;};vertex V vs(uint i [[vertex_id]]){float2 u=float2((i<<1)&2,i&2);return{float4(u*float2(2,-2)+float2(-1,1),0,1),u};}fragment float4 fs(V v [[stage_in]],texture2d<float> t [[texture(0)]]){return float4(t.read(uint2(v.p.xy)).rgb,1);}";
        NSError *e=nil;id<MTLLibrary> l=[g_dev newLibraryWithSource:msl options:nil error:&e];
        MTLRenderPipelineDescriptor *pd=[MTLRenderPipelineDescriptor new];pd.vertexFunction=[l newFunctionWithName:@"vs"];pd.fragmentFunction=[l newFunctionWithName:@"fs"];pd.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA16Float;
        pso=[g_dev newRenderPipelineStateWithDescriptor:pd error:&e];if(!pso)MDIE("HDR copy pipeline: %s",e.localizedDescription.UTF8String);
    }
    MTL4RenderPassDescriptor *pd=[MTL4RenderPassDescriptor new];pd.colorAttachments[0].texture=out;pd.colorAttachments[0].loadAction=MTLLoadActionClear;pd.colorAttachments[0].clearColor=MTLClearColorMake(0,0,0,1);pd.colorAttachments[0].storeAction=MTLStoreActionStore;
    id<MTL4RenderCommandEncoder> en=[g_cb renderCommandEncoderWithDescriptor:pd];
    [en barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
    [g_bloomq_args setTexture:src.gpuResourceID atIndex:0];[en setArgumentTable:g_bloomq_args atStages:MTLRenderStageFragment];
    [en setViewport:(MTLViewport){0,0,src.width,src.height,0,1}];[en setRenderPipelineState:pso];[en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];m4_end(en);
    pthread_mutex_lock(&g_hdr_mu);g_hdr_swap[slot].frame=atomic_load(&g_cur_frame);pthread_mutex_unlock(&g_hdr_mu);
    static uint64_t n;if(++n%1200==1)MLOG("[hdr] preserved float canvas before SDR copy (%llu frames)",n);
}
