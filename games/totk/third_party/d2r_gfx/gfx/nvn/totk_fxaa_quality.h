/* Optional full FXAA Quality, before the game's upscale and UI, without temporal inputs. */
#if TARGET_OS_OSX && !defined(GAME_BOTW)
#include "totk_spatial_aa_options.h"
#include "totk_fxaa_shader.h"
static struct {
    id<MTLLibrary> lib;
    id<MTLSamplerState> sampler;
    id<MTLBuffer> srgb[2];
    id<MTL4ArgumentTable> args[3];
    struct { MTLPixelFormat format; id<MTLRenderPipelineState> state; } ps[8];
    int nps,failed;
} g_fxq;
static id<MTLRenderPipelineState> fxq_ps(MTLPixelFormat format) {
    for(int i=0;i<g_fxq.nps;i++)if(g_fxq.ps[i].format==format)return g_fxq.ps[i].state;
    if(g_fxq.failed||g_fxq.nps==8)return nil;
    NSError *err=nil;
    if(!g_fxq.lib)g_fxq.lib=[g_dev newLibraryWithSource:@(totk_fxaa_quality_msl) options:spatial_aa_compile_options() error:&err];
    if(!g_fxq.lib){MLOG("[fxaa-quality] shader failed: %s",err.localizedDescription.UTF8String);g_fxq.failed=1;return nil;}
    MTLRenderPipelineDescriptor *d=[MTLRenderPipelineDescriptor new];d.vertexFunction=[g_fxq.lib newFunctionWithName:@"fxq_vs"];
    d.fragmentFunction=[g_fxq.lib newFunctionWithName:@"fxq_fs"];d.colorAttachments[0].pixelFormat=format;
    id<MTLRenderPipelineState> p=[g_dev newRenderPipelineStateWithDescriptor:d error:&err];
    if(!p){MLOG("[fxaa-quality] pipeline failed: %s",err.localizedDescription.UTF8String);g_fxq.failed=1;return nil;}
    int i=g_fxq.nps++;g_fxq.ps[i].format=format;g_fxq.ps[i].state=p;return p;
}
static int fxaa_quality_prepare(id<MTLTexture> src,id<MTLTexture> dst,unsigned slot) {
    (void)src;
    if(!fxq_ps(dst.pixelFormat))return 0;
    if(!g_fxq.sampler){
        MTLSamplerDescriptor *s=[MTLSamplerDescriptor new];s.minFilter=s.magFilter=MTLSamplerMinMagFilterLinear;
        s.sAddressMode=s.tAddressMode=MTLSamplerAddressModeClampToEdge;s.supportArgumentBuffers=YES;
        id<MTLSamplerState> sampler=[g_dev newSamplerStateWithDescriptor:s];if(!sampler)return 0;
        for(uint32_t i=0;i<2;i++){g_fxq.srgb[i]=[g_dev newBufferWithBytes:&i length:sizeof i options:MTLResourceStorageModeShared];if(!g_fxq.srgb[i])return 0;res_add(g_fxq.srgb[i]);}
        g_fxq.sampler=sampler;
    }
    if(!g_fxq.args[slot]){MTL4ArgumentTableDescriptor *a=[MTL4ArgumentTableDescriptor new];a.maxTextureBindCount=1;a.maxSamplerStateBindCount=1;a.maxBufferBindCount=1;
        NSError *err=nil;g_fxq.args[slot]=[g_dev newArgumentTableWithDescriptor:a error:&err];}
    return g_fxq.args[slot]!=nil;
}
static int fxaa_quality_encode(id<MTL4CommandBuffer> cb,id<MTLTexture> src,id<MTLTexture> dst,unsigned slot) {
    id<MTLRenderPipelineState> ps=fxq_ps(dst.pixelFormat);if(!ps)return 0;
    MTL4RenderPassDescriptor *d=[MTL4RenderPassDescriptor new];d.colorAttachments[0].texture=dst;
    d.colorAttachments[0].loadAction=MTLLoadActionDontCare;d.colorAttachments[0].storeAction=MTLStoreActionStore;
    id<MTL4RenderCommandEncoder> e=[cb renderCommandEncoderWithDescriptor:d];if(!e)return 0;
    [e barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageFragment visibilityOptions:MTL4VisibilityOptionDevice];
    id<MTL4ArgumentTable> a=g_fxq.args[slot];int srgb=src.pixelFormat==MTLPixelFormatRGBA8Unorm_sRGB||src.pixelFormat==MTLPixelFormatBGRA8Unorm_sRGB;
    [a setTexture:src.gpuResourceID atIndex:0];[a setSamplerState:g_fxq.sampler.gpuResourceID atIndex:0];[a setAddress:g_fxq.srgb[srgb].gpuAddress atIndex:0];
    [e setArgumentTable:a atStages:MTLRenderStageFragment];[e setRenderPipelineState:ps];[e setViewport:(MTLViewport){0,0,(double)dst.width,(double)dst.height,0,1}];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];[e endEncoding];return 1;
}
#else
static int fxaa_quality_prepare(id<MTLTexture> src,id<MTLTexture> dst,unsigned slot){(void)src;(void)dst;(void)slot;return 0;}
static int fxaa_quality_encode(id<MTL4CommandBuffer> cb,id<MTLTexture> src,id<MTLTexture> dst,unsigned slot){(void)cb;(void)src;(void)dst;(void)slot;return 0;}
#endif
