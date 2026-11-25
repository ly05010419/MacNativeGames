// GPU probe: edge smoothing's reactive mask kernels (games/totk/third_party/d2r_gfx/gfx/nvn/taa_kernels.h) on synthetic
// depth. The mask tells MetalFX (and the fast resolve) to take the current frame where the history belongs to another
// surface; a mask that fires on a static scene throws away the anti-aliasing, one that misses a moved surface leaves a
// trail. Prints "<case> ok|FAIL <value>"; exit 0 all ok, 1 a failure, 77 no Metal device.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "taa_kernels.h"

enum { W = 64, H = 64 };
static id<MTLDevice> dev; static id<MTLComputePipelineState> mv_ps; static id<MTLCommandQueue> q; static int failures;

typedef struct { float reproj[16]; float size[2]; float inv_size[2]; float prev_valid; float pad[3]; } P;   // taa_kernels.h struct P

static float lin_to_z(float d) { return 1.0f - 1.0f / d; }   // the kernel's lin(z) = 1 / (1 - z): view distance in units of near
static id<MTLTexture> tex(MTLPixelFormat f, MTLTextureUsage u) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f width:W height:H mipmapped:NO];
    d.usage = u; d.storageMode = f == MTLPixelFormatDepth32Float ? MTLStorageModePrivate : MTLStorageModeShared;
    return [dev newTextureWithDescriptor:d];
}
static id<MTLTexture> depth_from(const float *z) {   // a Depth32Float texture (the kernel reads depth2d) filled through a blit
    id<MTLTexture> t = tex(MTLPixelFormatDepth32Float, MTLTextureUsageShaderRead);
    id<MTLBuffer> b = [dev newBufferWithBytes:z length:W * H * 4 options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
    [e copyFromBuffer:b sourceOffset:0 sourceBytesPerRow:W * 4 sourceBytesPerImage:W * H * 4 sourceSize:MTLSizeMake(W, H, 1) toTexture:t destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
    [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
    return t;
}
/* Runs taa_mv with an identity reprojection (a still camera) on this frame's depth zc and last frame's zp (both as z,
   not distance); writes the mask into m. */
static void run_mask(const float *zc, const float *zp, int prev_valid, float *m) {
    id<MTLTexture> d = depth_from(zc), mv = tex(MTLPixelFormatRG16Float, MTLTextureUsageShaderWrite),
                   pd = tex(MTLPixelFormatR32Float, MTLTextureUsageShaderRead), cd = tex(MTLPixelFormatR32Float, MTLTextureUsageShaderWrite),
                   rm = tex(MTLPixelFormatR16Float, MTLTextureUsageShaderWrite);
    [pd replaceRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0 withBytes:zp bytesPerRow:W * 4];
    P p = {0}; for (int i = 0; i < 4; i++) p.reproj[i * 5] = 1;
    p.size[0] = W; p.size[1] = H; p.inv_size[0] = 1.0f / W; p.inv_size[1] = 1.0f / H; p.prev_valid = prev_valid ? 1 : 0;
    id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
    [e setComputePipelineState:mv_ps];
    [e setTexture:d atIndex:0]; [e setTexture:mv atIndex:1]; [e setTexture:pd atIndex:2]; [e setTexture:cd atIndex:3]; [e setTexture:rm atIndex:4];
    [e setBytes:&p length:sizeof p atIndex:0];
    [e dispatchThreads:MTLSizeMake(W, H, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
    [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
    __fp16 h[W * H]; [rm getBytes:h bytesPerRow:W * 2 fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];
    for (int i = 0; i < W * H; i++) m[i] = (float)h[i];
}
static void fill(float *z, float dist) { for (int i = 0; i < W * H; i++) z[i] = lin_to_z(dist); }
static void check(const char *name, int ok, float v) { printf("%s %s %.3f\n", name, ok ? "ok" : "FAIL", v); if (!ok) failures++; }
static float max_of(const float *m) { float v = 0; for (int i = 0; i < W * H; i++) v = fmaxf(v, m[i]); return v; }

int main(void) {
    @autoreleasepool {
        dev = MTLCreateSystemDefaultDevice(); if (!dev) { printf("no Metal device\n"); return 77; }
        q = [dev newCommandQueue];
        MTLCompileOptions *o = [MTLCompileOptions new]; o.mathMode = MTLMathModeSafe;   // as totk_taa.m setup()
        NSError *err = nil; id<MTLLibrary> lib = [dev newLibraryWithSource:@(taa_kernels_msl) options:o error:&err];
        if (!lib) { printf("compile FAIL %s\n", err.localizedDescription.UTF8String); return 1; }
        mv_ps = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"taa_mv"] error:&err];
        if (!mv_ps) { printf("pipeline FAIL %s\n", err.localizedDescription.UTF8String); return 1; }
        static float zc[W * H], zp[W * H], m[W * H];

        // a still scene keeps its history everywhere: any mask here would throw the anti-aliasing away
        fill(zc, 100); fill(zp, 100); run_mask(zc, zp, 1, m); check("static_scene_zero", max_of(m) == 0, max_of(m));
        // last frame's surface 20 % farther everywhere: something moved in, the current frame only
        fill(zc, 100); fill(zp, 120); run_mask(zc, zp, 1, m); check("jump_20pct_full", m[32 * W + 32] > 0.99f, m[32 * W + 32]);
        // 7 % apart is inside the ramp (4 % on, 10 % full): (0.07 - 0.04) / 0.06 = 0.5
        fill(zc, 100); fill(zp, 107); run_mask(zc, zp, 1, m); check("jump_7pct_partial", fabsf(m[32 * W + 32] - 0.5f) < 0.02f, m[32 * W + 32]);
        // the first frame (or after a reset) has no history to compare with: no mask
        fill(zc, 100); fill(zp, 120); run_mask(zc, zp, 0, m); check("prev_invalid_zero", max_of(m) == 0, max_of(m));
        /* A known limit (findings 2026-11-25): a 1-px near strand moved by 1 px finds itself in the 3x3 of last frame's
           depth, so neither its new nor its old column is masked. Swaying grass behaves alike; the depth test cannot see it. */
        fill(zc, 100); fill(zp, 100);
        for (int y = 0; y < H; y++) { zc[y * W + 32] = lin_to_z(10); zp[y * W + 31] = lin_to_z(10); }
        run_mask(zc, zp, 1, m);
        float strand = fmaxf(m[32 * W + 32], m[32 * W + 31]);
        check("thin_strand_1px_shift_not_masked_known_limit", strand == 0, strand);
        return failures ? 1 : 0;
    }
}
