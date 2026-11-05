#pragma once
/* Host-only FP16 companions for the scene's post-tonemap dataflow. Guest formats,
 * byte sizes and CPU-visible storage stay intact. No generic format promotion.
 * This file is included after the replay, residency and encoder helpers. */
#define HDR_SHADOW_MAX 24
static struct {
    id<MTLTexture> base, texture;
    NSMutableDictionary<NSNumber *,id<MTLTexture>> *reads;
    uint64_t frame, used;
} g_hdr_shadow[HDR_SHADOW_MAX];
static struct { id<MTLTexture> base, texture; } g_hdr_stores[8];
static unsigned g_hdr_nstores;
static uint64_t g_hdr_shadow_clock;
static int hdr_shadow_enabled(void) {
    /* mtl_set_frame already gates this by platform, game version and user mode. */
    return atomic_load_explicit(&g_hdr_frame_enabled,memory_order_relaxed);
}
static id<MTLTexture> hdr_shadow_find(id<MTLTexture> source) {
    if(!source || !hdr_shadow_enabled() || source.pixelFormat!=MTLPixelFormatRG11B10Float)return nil;
    id<MTLTexture> base=source.parentTexture?:source;
    if(source.width!=base.width || source.height!=base.height || source.mipmapLevelCount!=1)return nil;
    uint64_t frame=atomic_load(&g_cur_frame);
    for(unsigned i=0;i<HDR_SHADOW_MAX;i++)
        if(g_hdr_shadow[i].base==base && g_hdr_shadow[i].frame==frame)return g_hdr_shadow[i].texture;
    return nil;
}
/* R11G11B10F has no stored alpha: reading A yields one. A promoted RGBA16
 * target can contain the compose shader's A=0, or a UI shader's opacity. Keep
 * that storage private and expose the original RGB-only sampling contract.
 * The registered view's RGB swizzle is retained, including explicit constants. */
static id<MTLTexture> hdr_shadow_sample(id<MTLTexture> source) {
    id<MTLTexture> hdr=hdr_shadow_find(source);if(!hdr)return nil;
    MTLTextureSwizzleChannels sw=source.swizzle;
    if(sw.red==MTLTextureSwizzleAlpha)sw.red=MTLTextureSwizzleOne;
    if(sw.green==MTLTextureSwizzleAlpha)sw.green=MTLTextureSwizzleOne;
    if(sw.blue==MTLTextureSwizzleAlpha)sw.blue=MTLTextureSwizzleOne;
    if(sw.alpha==MTLTextureSwizzleAlpha)sw.alpha=MTLTextureSwizzleOne;
    uint64_t bits=(uint64_t)sw.red|((uint64_t)sw.green<<8)|((uint64_t)sw.blue<<16)|((uint64_t)sw.alpha<<24)|((uint64_t)source.textureType<<32);
    for(unsigned i=0;i<HDR_SHADOW_MAX;i++)if(g_hdr_shadow[i].texture==hdr){
        if(!g_hdr_shadow[i].reads)g_hdr_shadow[i].reads=[NSMutableDictionary dictionary];
        id<MTLTexture> view=g_hdr_shadow[i].reads[@(bits)];
        if(!view){
            view=[hdr newTextureViewWithPixelFormat:MTLPixelFormatRGBA16Float textureType:source.textureType levels:NSMakeRange(0,1) slices:NSMakeRange(0,1) swizzle:sw];
            if(!view)MDIE("HDR RGB-only sampling view creation failed");
            res_add(view);g_hdr_shadow[i].reads[@(bits)]=view;
        }
        return view;
    }
    return nil;
}
/* Blend hardware reads the attachment, not its sampling view. In an RGB-only
 * destination Ad=1: DstAlpha -> One, OneMinusDstAlpha -> Zero, and saturated
 * source alpha min(As,1-Ad) -> Zero. Source-alpha opacity remains unchanged. */
static int hdr_shadow_rgb_factor(int factor) {
    switch(factor){
        case NVN_BLEND_FUNC_DST_ALPHA:return NVN_BLEND_FUNC_ONE;
        case NVN_BLEND_FUNC_ONE_MINUS_DST_ALPHA:
        case NVN_BLEND_FUNC_SRC_ALPHA_SATURATE:return NVN_BLEND_FUNC_ZERO;
        default:return factor;
    }
}
/* A render copy converts formats, unlike a blit. The original is mirrored at
 * pass end for guest copies/readbacks; subsequent rendering samples FP16. */
static void hdr_shadow_copy(id<MTLTexture> src,id<MTLTexture> dst) {
    static id<MTLRenderPipelineState> to16,to11;
    id<MTLRenderPipelineState> pso=dst.pixelFormat==MTLPixelFormatRGBA16Float?to16:to11;
    if(!pso) {
        NSString *msl=totk_hdr_copy_source();
        NSError *e=nil;id<MTLLibrary> l=[g_dev newLibraryWithSource:msl options:nil error:&e];
        MTLRenderPipelineDescriptor *p=[MTLRenderPipelineDescriptor new];
        p.vertexFunction=[l newFunctionWithName:@"vs"];p.fragmentFunction=[l newFunctionWithName:@"fs"];
        p.colorAttachments[0].pixelFormat=dst.pixelFormat;
        pso=[g_dev newRenderPipelineStateWithDescriptor:p error:&e];
        if(!pso)MDIE("HDR format conversion compile: %s",e.localizedDescription.UTF8String);
        if(dst.pixelFormat==MTLPixelFormatRGBA16Float)to16=pso;else to11=pso;
    }
    blit_close();rs_sync();
    MTL4RenderPassDescriptor *p=[MTL4RenderPassDescriptor new];
    p.colorAttachments[0].texture=dst;p.colorAttachments[0].loadAction=MTLLoadActionDontCare;p.colorAttachments[0].storeAction=MTLStoreActionStore;
    id<MTL4RenderCommandEncoder> e=[g_cb renderCommandEncoderWithDescriptor:p];
    [e barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
    [g_bloomq_args setTexture:src.gpuResourceID atIndex:0];[e setArgumentTable:g_bloomq_args atStages:MTLRenderStageFragment];
    [e setViewport:(MTLViewport){0,0,dst.width,dst.height,0,1}];[e setRenderPipelineState:pso];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];m4_end(e);
}
static id<MTLTexture> hdr_shadow_make(id<MTLTexture> source) {
    id<MTLTexture> parent=source.parentTexture;
    id<MTLTexture> base=parent && source.width==parent.width && source.height==parent.height?parent:source;
    if(!base || base.textureType!=MTLTextureType2D || base.mipmapLevelCount!=1 || base.sampleCount!=1 ||
       base.pixelFormat!=MTLPixelFormatRG11B10Float)return nil;
    id<MTLTexture> found=hdr_shadow_find(base);if(found)return found;
    unsigned slot=HDR_SHADOW_MAX;uint64_t oldest=UINT64_MAX,frame=atomic_load(&g_cur_frame);
    for(unsigned i=0;i<HDR_SHADOW_MAX;i++) {
        if(g_hdr_shadow[i].base==base){slot=i;break;}
        if((!g_hdr_shadow[i].base || g_hdr_shadow[i].frame!=frame) && g_hdr_shadow[i].used<oldest){oldest=g_hdr_shadow[i].used;slot=i;}
    }
    if(slot==HDR_SHADOW_MAX)MDIE("HDR postprocess resource limit exceeded");
    rp_end();
    if(g_hdr_shadow[slot].base!=base) {
        for(id<MTLTexture> view in g_hdr_shadow[slot].reads.allValues)defer_obj_release(view);
        g_hdr_shadow[slot].reads=nil;
        if(g_hdr_shadow[slot].texture)defer_obj_release(g_hdr_shadow[slot].texture);
        MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:base.width height:base.height mipmapped:NO];
        d.storageMode=MTLStorageModePrivate;d.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;
        id<MTLTexture> t=[g_dev newTextureWithDescriptor:d];
        if(!t)MDIE("HDR postprocess FP16 allocation failed");
        res_add(t);res_commit();g_hdr_shadow[slot].base=base;g_hdr_shadow[slot].texture=t;
        MLOG("[hdr] FP16 postprocess companion %lux%lu",(unsigned long)base.width,(unsigned long)base.height);
    }
    /* Keep untouched pixels on a partial first draw. The original remains a
     * compatible copy for all non-render consumers of the guest resource. */
    hdr_shadow_copy(base,g_hdr_shadow[slot].texture);
    g_hdr_shadow[slot].frame=frame;g_hdr_shadow[slot].used=++g_hdr_shadow_clock;
    return g_hdr_shadow[slot].texture;
}
static void hdr_shadow_store_note(id<MTLTexture> base,id<MTLTexture> texture) {
    for(unsigned i=0;i<g_hdr_nstores;i++)if(g_hdr_stores[i].base==base)return;
    if(g_hdr_nstores<8){g_hdr_stores[g_hdr_nstores].base=base;g_hdr_stores[g_hdr_nstores++].texture=texture;}
}
static void hdr_shadow_store(void) {
    unsigned n=g_hdr_nstores;g_hdr_nstores=0;
    for(unsigned i=0;i<n;i++) {
        hdr_shadow_copy(g_hdr_stores[i].texture,g_hdr_stores[i].base);
        g_hdr_stores[i].base=nil;g_hdr_stores[i].texture=nil;
    }
}
