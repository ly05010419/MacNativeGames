//
// mtl_backend.m - implementation of mtl_backend.h (ObjC, -fobjc-arc)
// Render targets use storage mode MTLStorageModeShared: this machine is Apple Silicon with unified memory,
// a Shared texture can be used directly as a render attachment, and once the GPU is done the CPU reads it directly with getBytes, with no extra
// blit copy or staging buffer, so the readback path is the shortest. Cost: Shared textures cannot use lossless framebuffer compression;
// when taking the performance route later, switch to Private + blit readback (only affects mtlb_target / mtlb_readback).

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>
#include "mtl_backend.h"

// ---- Internal objects (held in ObjC classes, ARC manages Metal object lifetimes; exposed as opaque C pointers) ----

@interface MTLBCtx : NSObject
@property(nonatomic, strong) id<MTLDevice> dev;
@property(nonatomic, strong) id<MTLCommandQueue> queue;
@property(nonatomic, strong) id<MTLCommandBuffer> frame;   // current frame, created lazily, committed at readback
@property(nonatomic, strong) id<MTLLibrary> triLib;
@property(nonatomic, strong) NSMutableDictionary<NSNumber *, id<MTLRenderPipelineState>> *triPso; // cached by pixel format
@end
@implementation MTLBCtx
@end

@interface MTLBRt : NSObject
@property(nonatomic, strong) id<MTLTexture> tex;
@property(nonatomic) uint32_t w, h;
@end
@implementation MTLBRt
@end

#define CTX(p) ((__bridge MTLBCtx *)(void *)(p))
#define RT(p)  ((__bridge MTLBRt *)(void *)(p))

static void err(const char *where, NSError *e) {
    fprintf(stderr, "[mtlb] %s failed%s%s\n", where, e ? ": " : "",
            e ? e.localizedDescription.UTF8String : "");
}

static id<MTLCommandBuffer> frame_cb(MTLBCtx *c) {
    if (!c.frame) c.frame = [c.queue commandBuffer];
    return c.frame;
}

// ---- Device ----

mtlb_ctx *mtlb_init(void) {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) { err("MTLCreateSystemDefaultDevice", nil); return NULL; }
        if (!dev.hasUnifiedMemory) { err("unified-memory device check (needed for Shared render targets)", nil); return NULL; }
        MTLBCtx *c = [MTLBCtx new];
        c.dev = dev;
        c.queue = [dev newCommandQueue];
        if (!c.queue) { err("newCommandQueue", nil); return NULL; }
        c.triPso = [NSMutableDictionary dictionary];
        return (mtlb_ctx *)CFBridgingRetain(c);
    }
}

void mtlb_shutdown(mtlb_ctx *ctx) {
    if (!ctx) return;
    @autoreleasepool {
        MTLBCtx *c = CTX(ctx);
        if (c.frame) { [c.frame commit]; [c.frame waitUntilCompleted]; c.frame = nil; }
        CFBridgingRelease(ctx);
    }
}

// ---- Render targets ----

static MTLPixelFormat pixfmt(mtlb_fmt f) {
    switch (f) {
    case MTLB_FMT_BGRA8: return MTLPixelFormatBGRA8Unorm;
    case MTLB_FMT_RGBA8: return MTLPixelFormatRGBA8Unorm;
    }
    return MTLPixelFormatInvalid;
}

mtlb_rt *mtlb_target(mtlb_ctx *ctx, uint32_t w, uint32_t h, mtlb_fmt fmt) {
    @autoreleasepool {
        MTLPixelFormat pf = pixfmt(fmt);
        if (!ctx || !w || !h || pf == MTLPixelFormatInvalid) { err("mtlb_target argument check", nil); return NULL; }
        MTLTextureDescriptor *d =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf width:w height:h mipmapped:NO];
        d.storageMode = MTLStorageModeShared;
        d.usage = MTLTextureUsageRenderTarget;
        MTLBRt *r = [MTLBRt new];
        r.tex = [CTX(ctx).dev newTextureWithDescriptor:d];
        if (!r.tex) { err("newTextureWithDescriptor", nil); return NULL; }
        r.w = w; r.h = h;
        return (mtlb_rt *)CFBridgingRetain(r);
    }
}

void mtlb_target_free(mtlb_rt *rt) {
    if (rt) CFBridgingRelease(rt);
}

// ---- Pass recording ----

static MTLRenderPassDescriptor *pass_desc(MTLBRt *r, MTLLoadAction load, MTLClearColor cc) {
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    p.colorAttachments[0].texture = r.tex;
    p.colorAttachments[0].loadAction = load;
    p.colorAttachments[0].storeAction = MTLStoreActionStore;
    p.colorAttachments[0].clearColor = cc;
    return p;
}

int mtlb_clear(mtlb_ctx *ctx, mtlb_rt *rt, float r, float g, float b, float a) {
    if (!ctx || !rt) return -1;
    @autoreleasepool {
        MTLRenderPassDescriptor *p =
            pass_desc(RT(rt), MTLLoadActionClear, MTLClearColorMake(r, g, b, a));
        id<MTLRenderCommandEncoder> e = [frame_cb(CTX(ctx)) renderCommandEncoderWithDescriptor:p];
        if (!e) { err("renderCommandEncoder(clear)", nil); return -1; }
        [e endEncoding];
        return 0;
    }
}

static NSString *const kTriMSL =
    @"#include <metal_stdlib>\n"
     "using namespace metal;\n"
     "struct V { float4 pos; float4 col; };\n"
     "struct O { float4 pos [[position]]; float4 col; };\n"
     "vertex O tri_vs(uint vid [[vertex_id]], constant V *v [[buffer(0)]]) {\n"
     "    O o; o.pos = v[vid].pos; o.col = v[vid].col; return o;\n"
     "}\n"
     "fragment float4 tri_fs(O in [[stage_in]]) { return in.col; }\n";

// Matches the layout of struct V in kTriMSL (two float4, 32 bytes)
typedef struct { float pos[4]; float col[4]; } TriVert;

// Vertices (NDC): top red, bottom-left green, bottom-right blue
static const TriVert kTri[3] = {
    {{ 0.0f,  0.5f, 0.0f, 1.0f}, {1, 0, 0, 1}},
    {{-0.5f, -0.5f, 0.0f, 1.0f}, {0, 1, 0, 1}},
    {{ 0.5f, -0.5f, 0.0f, 1.0f}, {0, 0, 1, 1}},
};

static id<MTLRenderPipelineState> tri_pso(MTLBCtx *c, MTLPixelFormat pf) {
    id<MTLRenderPipelineState> pso = c.triPso[@(pf)];
    if (pso) return pso;
    NSError *e = nil;
    if (!c.triLib) {
        c.triLib = [c.dev newLibraryWithSource:kTriMSL options:nil error:&e];
        if (!c.triLib) { err("newLibraryWithSource", e); return nil; }
    }
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [c.triLib newFunctionWithName:@"tri_vs"];
    d.fragmentFunction = [c.triLib newFunctionWithName:@"tri_fs"];
    d.colorAttachments[0].pixelFormat = pf;
    pso = [c.dev newRenderPipelineStateWithDescriptor:d error:&e];
    if (!pso) { err("newRenderPipelineState", e); return nil; }
    c.triPso[@(pf)] = pso;
    return pso;
}

int mtlb_triangle(mtlb_ctx *ctx, mtlb_rt *rt) {
    if (!ctx || !rt) return -1;
    @autoreleasepool {
        MTLBCtx *c = CTX(ctx);
        MTLBRt *r = RT(rt);
        id<MTLRenderPipelineState> pso = tri_pso(c, r.tex.pixelFormat);
        if (!pso) return -1;
        MTLRenderPassDescriptor *p = pass_desc(r, MTLLoadActionLoad, MTLClearColorMake(0, 0, 0, 0));
        id<MTLRenderCommandEncoder> e = [frame_cb(c) renderCommandEncoderWithDescriptor:p];
        if (!e) { err("renderCommandEncoder(triangle)", nil); return -1; }
        [e setRenderPipelineState:pso];
        [e setVertexBytes:kTri length:sizeof kTri atIndex:0];
        [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [e endEncoding];
        return 0;
    }
}

// ---- Commit and readback ----

int mtlb_readback(mtlb_ctx *ctx, mtlb_rt *rt, void *buf, size_t buf_size) {
    if (!ctx || !rt || !buf) return -1;
    @autoreleasepool {
        MTLBCtx *c = CTX(ctx);
        MTLBRt *r = RT(rt);
        size_t row = (size_t)r.w * 4;
        if (buf_size < row * r.h) { err("mtlb_readback (buffer too small)", nil); return -1; }
        if (c.frame) {
            id<MTLCommandBuffer> cb = c.frame;
            c.frame = nil;
            [cb commit];
            [cb waitUntilCompleted];
            if (cb.status != MTLCommandBufferStatusCompleted) { err("commandBuffer execution", cb.error); return -1; }
        }
        [r.tex getBytes:buf bytesPerRow:row fromRegion:MTLRegionMake2D(0, 0, r.w, r.h) mipmapLevel:0];
        return 0;
    }
}
