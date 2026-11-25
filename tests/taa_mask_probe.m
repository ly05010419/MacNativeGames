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
static id<MTLDevice> dev; static id<MTLComputePipelineState> mv_ps, layer_ps; static id<MTLCommandQueue> q; static int failures;

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
typedef struct { float size[2]; float inv_size[2]; float prev_valid; float threshold; } L;   // taa_kernels.h struct L
static id<MTLTexture> tex_rgba(const float *rgb) {   // an RGBA16Float colour texture from W x H rgb triples (the HDR scene)
    id<MTLTexture> t = tex(MTLPixelFormatRGBA16Float, MTLTextureUsageShaderRead);
    static __fp16 h[W * H * 4]; for (int i = 0; i < W * H; i++) { for (int c = 0; c < 3; c++) h[i * 4 + c] = (__fp16)rgb[i * 3 + c]; h[i * 4 + 3] = 1; }
    [t replaceRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0 withBytes:h bytesPerRow:W * 8];
    return t;
}
/* Runs taa_layer twice, as two frames: the first writes its layer (no history), the second compares with it. Frame 2's
   camera motion (pixels, uniform) goes in the motion texture; the mask starts at 0 and is returned in m. */
static void run_layer(const float *in1, const float *lit1, const float *in2, const float *lit2, float mvx, int prev_valid, float *m) {
    id<MTLTexture> lay[2] = {tex(MTLPixelFormatR16Float, MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite), tex(MTLPixelFormatR16Float, MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite)};
    id<MTLTexture> rm = tex(MTLPixelFormatR16Float, MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite), mv = tex(MTLPixelFormatRG16Float, MTLTextureUsageShaderRead);
    static __fp16 z[W * H * 2]; memset(z, 0, sizeof z);
    [rm replaceRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0 withBytes:z bytesPerRow:W * 2];
    for (int i = 0; i < W * H; i++) { z[i * 2] = (__fp16)mvx; z[i * 2 + 1] = 0; }
    [mv replaceRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0 withBytes:z bytesPerRow:W * 4];
    const float *ins[2] = {in1, in2}, *lits[2] = {lit1, lit2};
    for (int f = 0; f < 2; f++) {
        L p = {{W, H}, {1.0f / W, 1.0f / H}, f ? (prev_valid ? 1.0f : 0.0f) : 0.0f, 0.02f};
        id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
        [e setComputePipelineState:layer_ps];
        [e setTexture:tex_rgba(ins[f]) atIndex:0]; [e setTexture:tex_rgba(lits[f]) atIndex:1]; [e setTexture:mv atIndex:2];
        [e setTexture:lay[f ^ 1] atIndex:3]; [e setTexture:rm atIndex:4]; [e setTexture:lay[f] atIndex:5];
        [e setBytes:&p length:sizeof p atIndex:0];
        [e dispatchThreads:MTLSizeMake(W, H, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
        [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
    }
    __fp16 h[W * H]; [rm getBytes:h bytesPerRow:W * 2 fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];
    for (int i = 0; i < W * H; i++) m[i] = (float)h[i];
}
static void flat(float *rgb, float v) { for (int i = 0; i < W * H * 3; i++) rgb[i] = v; }
static void square(float *rgb, int x0, int y0, float v) { for (int y = y0; y < y0 + 4; y++) for (int x = x0; x < x0 + 4; x++) for (int c = 0; c < 3; c++) rgb[(y * W + x) * 3 + c] = v; }
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
        layer_ps = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"taa_layer"] error:&err];
        if (!layer_ps) { printf("layer pipeline FAIL %s\n", err ? err.localizedDescription.UTF8String : "no taa_layer"); return 1; }
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
        /* Transparents (2026-11-25 findings H2): the layer is what the passes after lighting added (compose input minus
           the lit copy); only where it changed since last frame, at the camera-reprojected spot, is the mask set. */
        static float in1[W * H * 3], lit1[W * H * 3], in2[W * H * 3], lit2[W * H * 3];
        // a still transparent (water, fog) under a 3-px camera pan keeps its anti-aliasing: no mask
        flat(lit1, 0.2f); flat(in1, 0.5f); flat(lit2, 0.2f); flat(in2, 0.5f);
        run_layer(in1, lit1, in2, lit2, -3, 1, m); check("static_layer_pan_zero", max_of(m) == 0, max_of(m));
        // a bright particle moved 6 px with a still camera: its new and its old spot take the current frame, nothing else
        flat(lit1, 0.2f); flat(in1, 0.2f); square(in1, 20, 30, 4.0f); flat(lit2, 0.2f); flat(in2, 0.2f); square(in2, 26, 30, 4.0f);
        run_layer(in1, lit1, in2, lit2, 0, 1, m);
        float nw = m[31 * W + 27], old = m[31 * W + 21], far = fmaxf(m[10 * W + 10], m[50 * W + 50]);
        check("moving_particle_masks_both_spots", nw >= 0.9f && old >= 0.9f && far == 0, fminf(nw, old));
        /* Fog over a depth edge: the layer steps where the depth does, and the jitter moves that step by part of a pixel
           each frame (here a whole pixel, the worst case). Masking it would take the anti-aliasing off every edge under
           fog (the first in-game run, 2026-11-25: 17 % of the ground); last frame's 3x3 range holds both sides. */
        flat(lit1, 0.2f); flat(in1, 0.2f); flat(lit2, 0.2f); flat(in2, 0.2f);
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) for (int c = 0; c < 3; c++) {
            if (x >= 32) in1[(y * W + x) * 3 + c] = 0.6f;
            if (x >= 33) in2[(y * W + x) * 3 + c] = 0.6f; }
        run_layer(in1, lit1, in2, lit2, 0, 1, m); check("jittered_fog_edge_zero", max_of(m) == 0, max_of(m));
        // no transparents at all: the lit copy is the compose input
        flat(lit1, 0.3f); flat(in1, 0.3f); flat(lit2, 0.3f); flat(in2, 0.3f);
        run_layer(in1, lit1, in2, lit2, 0, 1, m); check("no_transparents_zero", max_of(m) == 0, max_of(m));
        // after a reset (or the first frame with a copy) last frame's layer is not this history's: no mask
        flat(lit1, 0.2f); flat(in1, 0.2f); square(in1, 20, 30, 4.0f); flat(lit2, 0.2f); flat(in2, 0.2f); square(in2, 26, 30, 4.0f);
        run_layer(in1, lit1, in2, lit2, 0, 0, m); check("reset_gives_no_layer_mask", max_of(m) == 0, max_of(m));
        return failures ? 1 : 0;
    }
}
