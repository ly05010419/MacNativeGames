/* Optional SMAA 1x on TOTK's tone-mapped scene, before its upscale and UI. Included by totk_finish.h.
   The caller closes the game's encoders first. These passes use their own pipelines, textures and argument tables;
   no NVN state is changed. Failure leaves the caller a texel-for-texel fallback rather than an unwritten FXAA image. */
#include "totk_smaa_config.h"
#include <string.h>
#if TARGET_OS_OSX && !defined(GAME_BOTW)
#include "totk_spatial_aa_options.h"
#include "../../../smaa/AreaTex.h"
#include "../../../smaa/SearchTex.h"
#include "totk_smaa_shader.h"
static struct {
    id<MTLLibrary> lib;
    id<MTLTexture> area, search, edge[3], weight[3];
    id<MTLSamplerState> linear, point;
    id<MTLBuffer> params[2];
    SmaaParams config;
    id<MTL4ArgumentTable> args[3][3];
    struct { int fn; MTLPixelFormat format; id<MTLRenderPipelineState> state; } ps[12];
    int nps, failed;
    uint64_t done;
} g_smaa;

static id<MTLRenderPipelineState> smaa_ps(int fn, MTLPixelFormat format) {
    for (int i = 0; i < g_smaa.nps; i++) if (g_smaa.ps[i].fn == fn && g_smaa.ps[i].format == format) return g_smaa.ps[i].state;
    if (g_smaa.failed || g_smaa.nps == 12) return nil;
    NSError *err = nil;
    if (!g_smaa.lib) g_smaa.lib = [g_dev newLibraryWithSource:@(totk_smaa_msl) options:spatial_aa_compile_options() error:&err];
    if (!g_smaa.lib) { MLOG("[smaa] shader build failed: %s; keeping the scene", err.localizedDescription.UTF8String); g_smaa.failed = 1; return nil; }
    static NSString *const names[] = {@"smaa_edge", @"smaa_weight", @"smaa_blend"};
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [g_smaa.lib newFunctionWithName:@"smaa_vs"];
    d.fragmentFunction = [g_smaa.lib newFunctionWithName:names[fn]];
    d.colorAttachments[0].pixelFormat = format;
    id<MTLRenderPipelineState> p = [g_dev newRenderPipelineStateWithDescriptor:d error:&err];
    if (!p) { MLOG("[smaa] pipeline failed: %s; keeping the scene", err.localizedDescription.UTF8String); g_smaa.failed = 1; return nil; }
    int i = g_smaa.nps++; g_smaa.ps[i].fn = fn; g_smaa.ps[i].format = format; g_smaa.ps[i].state = p;
    return p;
}
static id<MTLTexture> smaa_target(__strong id<MTLTexture> *old, NSUInteger w, NSUInteger h, MTLPixelFormat format) {
    if (*old && (*old).width == w && (*old).height == h && (*old).pixelFormat == format) return *old;
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:w height:h mipmapped:NO];
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; d.storageMode = MTLStorageModePrivate;
    id<MTLTexture> t = [g_dev newTextureWithDescriptor:d]; if (!t) return nil;
    res_add(t); if (*old) defer_obj_release(*old); *old = t; return t;
}
static int smaa_prepare(id<MTLTexture> src, id<MTLTexture> dst, unsigned slot) {
    if (g_smaa.failed) return 0;
    if (!g_smaa.area) {
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRG8Unorm width:AREATEX_WIDTH height:AREATEX_HEIGHT mipmapped:NO];
        d.usage = MTLTextureUsageShaderRead; d.storageMode = MTLStorageModeShared;
        id<MTLTexture> a = [g_dev newTextureWithDescriptor:d];
        d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm width:SEARCHTEX_WIDTH height:SEARCHTEX_HEIGHT mipmapped:NO];
        d.usage = MTLTextureUsageShaderRead; d.storageMode = MTLStorageModeShared;
        id<MTLTexture> s = [g_dev newTextureWithDescriptor:d];
        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
        sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
        sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge; sd.supportArgumentBuffers = YES;
        g_smaa.linear = [g_dev newSamplerStateWithDescriptor:sd];
        sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterNearest; g_smaa.point = [g_dev newSamplerStateWithDescriptor:sd];
        if (!a || !s || !g_smaa.linear || !g_smaa.point) return 0;
        [a replaceRegion:MTLRegionMake2D(0,0,AREATEX_WIDTH,AREATEX_HEIGHT) mipmapLevel:0 withBytes:areaTexBytes bytesPerRow:AREATEX_PITCH];
        [s replaceRegion:MTLRegionMake2D(0,0,SEARCHTEX_WIDTH,SEARCHTEX_HEIGHT) mipmapLevel:0 withBytes:searchTexBytes bytesPerRow:SEARCHTEX_PITCH];
        res_add(a); res_add(s); g_smaa.search = s; g_smaa.area = a;
    }
    SmaaParams config = smaa_config(atomic_load(&g_smaa_quality),0);
    if (!g_smaa.params[0] || memcmp(&config,&g_smaa.config,sizeof config)) {
        /* Immutable constants: a mode change must never overwrite bytes an earlier GPU frame still reads. */
        id<MTLBuffer> next[2];
        for (unsigned i=0;i<2;i++) {
            config.srgb=i; next[i]=[g_dev newBufferWithBytes:&config length:sizeof config options:MTLResourceStorageModeShared];
            if (!next[i]) return 0;
        }
        config.srgb=0; g_smaa.config=config;
        for (int i=0;i<2;i++) { res_add(next[i]); if(g_smaa.params[i])defer_obj_release(g_smaa.params[i]); g_smaa.params[i]=next[i]; }
    }
    MTL4ArgumentTableDescriptor *ad = [MTL4ArgumentTableDescriptor new];
    ad.maxTextureBindCount = 3; ad.maxSamplerStateBindCount = 2; ad.maxBufferBindCount = 1;
    for (int i = 0; i < 3; i++) if (!g_smaa.args[slot][i]) {
        NSError *err = nil; g_smaa.args[slot][i] = [g_dev newArgumentTableWithDescriptor:ad error:&err];
        if (!g_smaa.args[slot][i]) return 0;
    }
    return smaa_target(&g_smaa.edge[slot],src.width,src.height,MTLPixelFormatRG8Unorm) &&
           smaa_target(&g_smaa.weight[slot],src.width,src.height,MTLPixelFormatRGBA8Unorm) &&
           smaa_ps(0,MTLPixelFormatRG8Unorm) && smaa_ps(1,MTLPixelFormatRGBA8Unorm) && smaa_ps(2,dst.pixelFormat);
}
static int smaa_pass(id<MTL4CommandBuffer> cb, int fn, id<MTLTexture> t0, id<MTLTexture> t1, id<MTLTexture> t2, id<MTLTexture> dst, unsigned slot) {
    id<MTLRenderPipelineState> p = smaa_ps(fn,dst.pixelFormat); if (!p) return 0;
    MTL4RenderPassDescriptor *d = [MTL4RenderPassDescriptor new];
    d.colorAttachments[0].texture = dst; d.colorAttachments[0].loadAction = fn == 0 ? MTLLoadActionClear : MTLLoadActionDontCare;
    d.colorAttachments[0].clearColor = MTLClearColorMake(0,0,0,0); d.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTL4RenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:d]; if (!e) return 0;
    [e barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageFragment visibilityOptions:MTL4VisibilityOptionDevice];
    id<MTL4ArgumentTable> a = g_smaa.args[slot][fn];
    [a setTexture:t0.gpuResourceID atIndex:0];
    if (t1) [a setTexture:t1.gpuResourceID atIndex:1]; if (t2) [a setTexture:t2.gpuResourceID atIndex:2];
    [a setSamplerState:g_smaa.linear.gpuResourceID atIndex:0]; [a setSamplerState:g_smaa.point.gpuResourceID atIndex:1];
    int srgb = t0.pixelFormat == MTLPixelFormatRGBA8Unorm_sRGB || t0.pixelFormat == MTLPixelFormatBGRA8Unorm_sRGB;
    [a setAddress:g_smaa.params[srgb].gpuAddress atIndex:0];
    [e setArgumentTable:a atStages:MTLRenderStageFragment]; [e setRenderPipelineState:p];
    [e setViewport:(MTLViewport){0,0,(double)dst.width,(double)dst.height,0,1}];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [e endEncoding]; return 1;
}
static int smaa_encode(id<MTL4CommandBuffer> cb, id<MTLTexture> src, id<MTLTexture> dst, unsigned slot) {
    int ok = smaa_pass(cb,0,src,nil,nil,g_smaa.edge[slot],slot) &&
             smaa_pass(cb,1,g_smaa.edge[slot],g_smaa.area,g_smaa.search,g_smaa.weight[slot],slot) &&
             smaa_pass(cb,2,src,g_smaa.weight[slot],nil,dst,slot);
    if (ok && !g_smaa.done++) MLOG("[smaa] three spatial passes, scene %lux%lu, before the UI",(unsigned long)src.width,(unsigned long)src.height);
    return ok;
}
#else
static int smaa_prepare(id<MTLTexture> src, id<MTLTexture> dst, unsigned slot) { (void)src; (void)dst; (void)slot; return 0; }
static int smaa_encode(id<MTL4CommandBuffer> cb, id<MTLTexture> src, id<MTLTexture> dst, unsigned slot) { (void)cb; (void)src; (void)dst; (void)slot; return 0; }
#endif
