// TOTK: MetalFX temporal anti-aliasing (TOTK_TAA=1; not in D2R).
// The game renders without anti-aliasing of its own beyond FXAA after tone mapping, so edges stair-step at the scene
// size. A MetalFX temporal scaler at 1:1 (input and output the scene size) runs on the linear HDR scene just before the
// game's compose shader 0ac9a549 (tone mapping and bloom) samples it; the replay then binds our output instead (as
// MFXUP_SCENE does with the spatial scaler).
//  - Jitter: every frame the geometry drawn into the G-buffer depth has its viewport moved by a sub-pixel offset
//    (Halton 2, 3; no shader changes; full-screen triangles and quads stay put). MetalFX gets the same offset.
//  - Key 5 turns it on and off while playing (runtime/input.c); TOTK_TAA=1 starts it on.
//  - Motion vectors: from the camera only, as for frame interpolation (totk_mfx.m): the G-buffer depth reprojected with
//    this and the previous frame's view-projection. What moves on its own (Link, enemies) gets the camera's motion;
//    MetalFX rejects what does not match, at the cost of less smoothing there. The game keeps no motion vectors of its
//    own (the scene-size R32Float target its G-buffer passes write, a4eb9e94, is linear depth).
//  - Reactive mask (TOTK_TAA_REACTIVE=0 turns it off): MetalFX's colour clamp alone let walking characters leave trails.
//    The same kernel keeps the depth of each frame and checks the surface the camera motion points to: when last frame's
//    depth there is not the depth that surface should have had (nearest of 3x3, so edges and jitter still pass), something
//    moved in or out of that pixel, and MetalFX is told to take the current frame there instead of the history.
//  - Reset: the first frame, a size change and a camera cut (more than 8 units or 25 degrees in one frame).
#import <MetalFX/MetalFX.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "totk_taa.h"
#include <TargetConditionals.h>

#if TARGET_OS_VISION
/* visionOS has no MetalFX temporal scaler: anti-aliasing stays off there (it is off by default everywhere, key 5). */
int taa_enabled(void) { return 0; }
int taa_active(void) { return 0; }
int taa_ready(void) { return 0; }
void taa_toggle(void) {}
int taa_pass(MTL4RenderPassDescriptor *pd) { (void)pd; return 0; }
int taa_jittered(void) { return 0; }
void taa_jitter(double *x, double *y) { *x = 0; *y = 0; }
void taa_note_camera(const float *cam, int n) { (void)cam; (void)n; }
id<MTLTexture> taa_encode(id<MTL4CommandBuffer> cb, id<MTLTexture> scene) { (void)cb; (void)scene; return nil; }
id<MTLResidencySet> taa_residency(void) { return nil; }
void taa_frame_end(void) {}
void taa_stats(char *out, size_t n) { snprintf(out, n, "not available on visionOS"); }
#else

#define TLOG(...) do { fprintf(stderr, "[dm] [nvn-mtl] [taa] " __VA_ARGS__); fputc('\n', stderr); } while (0)
/* Per-frame parameter buffers and argument tables: the replay can encode several frames before the GPU runs the first
   one, and a slot rewritten too early gives MetalFX the wrong camera for that frame (the history then lands beside
   itself; it showed as a purple shimmer on hazy rocks that faded whenever a screenshot waited for the GPU). */
#define TAA_SLOTS 16
#define TAA_OLD TAA_SLOTS   // replaced scalers and textures are kept as long (Metal 4 command buffers do not retain)

static struct {
    id<MTLDevice> dev; id<MTL4Compiler> comp; id<MTLResidencySet> rs; id<MTLFence> fence;
    id<MTL4FXTemporalScaler> sc; NSUInteger w, h; MTLPixelFormat fmt;
    id<MTLTexture> out, mv, react, dh[2]; int dh_i;   // dh: the depth of this and of the last frame (reactive mask)
    id<MTLComputePipelineState> mv_ps; id<MTL4ArgumentTable> args[TAA_SLOTS]; id<MTLBuffer> par[TAA_SLOTS]; int slot;
    __unsafe_unretained id<MTLTexture> depth;   // this frame's G-buffer depth (the game's texture, alive for the frame)
    double cam[84], prev[84]; int cam_ok, prev_ok, jit_pass, failed;
    int active, jit_ok, jit_used;   // active: latched from key 5 at frame end; jit_ok: this frame may jitter (see taa_frame_end)
    uint64_t retry_at;              // a scaler that could not be built is tried again from this frame on
    unsigned jit_i; double jx, jy;
    __unsafe_unretained id<MTLTexture> last_src; uint64_t frame, encoded_frame;
    uint64_t done, resets, missing;
    /* Each size change retires the scaler and five textures. Keep every one alive for the in-flight frames. */
    struct { id obj; uint64_t frame; } old[TAA_OLD * 6]; int nold;
} g;
static void keep_old(id obj) {   // released (and dropped from the residency set) TAA_OLD frames later, when the GPU is done with it
    if (!obj) return;
    int dropped = 0;
    for (int i = 0; i < g.nold; i++) if (g.frame - g.old[i].frame >= TAA_OLD) {
        if ([g.old[i].obj conformsToProtocol:@protocol(MTLAllocation)]) { [g.rs removeAllocation:g.old[i].obj]; dropped = 1; }
        g.old[i] = g.old[--g.nold]; i--;
    }
    if (dropped) [g.rs commit];
    if (g.nold < (int)(sizeof g.old / sizeof *g.old)) g.old[g.nold++] = (__typeof__(g.old[0])){obj, g.frame};
}

static _Atomic int g_on = -1;
/* Frames anti-aliased so far: the panel shows key 5 as on but not in effect while none is (a game version whose compose
   draw TAA_COMPOSE does not know, or no world frame yet). */
int taa_effective(void) { return g.done > 0; }
int taa_enabled(void) {
    int on = atomic_load_explicit(&g_on, memory_order_relaxed);
    if (on < 0) {
        const char *e = getenv("TOTK_TAA"); on = e && strcmp(e, "0");
        atomic_store(&g_on, on);
        TLOG("MetalFX temporal anti-aliasing (TOTK_TAA, key 5): %s", on ? "on" : "off");
    }
    return on;
}
/* Key 5: on / off while playing (the input thread). The replay takes it at the next frame end, so a frame is either
   jittered in every pass on the G-buffer depth or in none (foliage drawn twice with an equal depth test needs both alike). */
void taa_toggle(void) {
    int on = !taa_enabled();
    atomic_store(&g_on, on);
    TLOG("key 5: temporal anti-aliasing %s", on ? "on" : "off");
}
int taa_active(void) { return g.active; }
int taa_ready(void) { return g.active && !g.failed && g.frame >= g.retry_at && g.depth && g.cam_ok; }
static double halton(unsigned i, unsigned b) { double f = 1, r = 0; while (i) { f /= b; r += f * (i % b); i /= b; } return r; }

int taa_pass(MTL4RenderPassDescriptor *pd) {
    if (!g.active) return 0;
    id<MTLTexture> c0 = pd.colorAttachments[0].texture, dt = pd.depthAttachment.texture;
    if (!g.depth && dt && c0 && dt.pixelFormat == MTLPixelFormatDepth32Float && dt.textureType == MTLTextureType2D &&
        dt.width == c0.width && dt.height == c0.height && dt.width >= 320) {
        int nc = 0; for (int i = 0; i < 8; i++) nc += pd.colorAttachments[i].texture != nil;
        if (nc >= 3) { g.depth = dt; g.jit_pass = g.jit_ok; return 2; }
    }
    g.jit_pass = g.jit_ok && dt && dt == g.depth;
    return g.jit_pass;
}
int taa_jittered(void) { return g.jit_pass; }
void taa_jitter(double *x, double *y) { *x = g.jx; *y = g.jy; g.jit_used = 1; }
void taa_note_camera(const float *c, int n) {
    if (n < 84) return;
    for (int i = 0; i < 84; i++) g.cam[i] = c[i];
    g.cam_ok = c[56] > 0 && c[57] > c[56] && c[62] > 0 && c[70] > 0;
}
id<MTLResidencySet> taa_residency(void) { return g.rs; }

static int setup(id<MTLDevice> dev) {
    if (g.dev) return 1;
    if (g.failed) return 0;
    NSError *e = nil;
    g.comp = [dev newCompilerWithDescriptor:[MTL4CompilerDescriptor new] error:&e];
    MTLResidencySetDescriptor *rd = [MTLResidencySetDescriptor new]; rd.label = @"TOTK TAA";
    g.rs = [dev newResidencySetWithDescriptor:rd error:&e];
    static const char *src =
        "#include <metal_stdlib>\nusing namespace metal;\n"
        "struct P { float4x4 reproj; float2 size; float2 inv_size; float prev_valid; };\n"
        // view distance up to the near plane's factor: the depth has an infinite far plane (distance = near / (1 - depth))
        "static float lin(float z) { return 1.0 / max(1.0 - z, 1e-7); }\n"
        // camera motion of each depth pixel: where it was in the previous frame, in pixels of this texture; this frame's
        // depth for the next one; and the reactive mask (see the header)
        "kernel void taa_mv(depth2d<float, access::read> d [[texture(0)]], texture2d<float, access::write> mv [[texture(1)]],\n"
        "                   texture2d<float, access::read> pd [[texture(2)]], texture2d<float, access::write> cd [[texture(3)]],\n"
        "                   texture2d<float, access::write> rm [[texture(4)]],\n"
        "                   constant P &p [[buffer(0)]], uint2 g [[thread_position_in_grid]]) {\n"
        "  if (g.x >= uint(p.size.x) || g.y >= uint(p.size.y)) return;\n"
        "  float z = d.read(g);\n"
        "  float2 uv = (float2(g) + 0.5) * p.inv_size;\n"
        "  float4 c = p.reproj * float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, z * 2.0 - 1.0, 1.0);\n"
        "  float2 m = 0.0; float r = 0.0;\n"
        "  if (c.w > 1e-7) {\n"
        "    float3 n = c.xyz / c.w; float2 puv = float2(n.x * 0.5 + 0.5, 0.5 - 0.5 * n.y);\n"
        "    m = (puv - uv) * p.size;\n"
        "    if (p.prev_valid > 0.5 && z < 1.0 && all(puv >= 0.0) && all(puv < 1.0)) {\n"
        "      float le = lin(n.z * 0.5 + 0.5), best = 1e9; int2 q = int2(puv * p.size), hi = int2(p.size) - 1;\n"
        "      for (int y = -1; y <= 1; y++) for (int x = -1; x <= 1; x++)\n"
        "        best = min(best, abs(lin(pd.read(uint2(clamp(q + int2(x, y), int2(0), hi))).x) - le) / le);\n"
        "      r = saturate((best - 0.04) / 0.06);\n"   // 4 % apart starts it, 10 % is the current frame only
        "    }\n"
        "  }\n"
        "  if (any(isnan(m)) || any(isinf(m))) m = 0.0;\n"
        "  if (isnan(r)) r = 0.0;\n"
        "  mv.write(float4(m, 0.0, 0.0), g); cd.write(float4(z, 0.0, 0.0, 0.0), g); rm.write(float4(r, 0.0, 0.0, 0.0), g);\n"
        "}\n";
    MTLCompileOptions *o = [MTLCompileOptions new]; o.mathMode = MTLMathModeSafe;
    id<MTLLibrary> lib = [dev newLibraryWithSource:@(src) options:o error:&e];
    id<MTLFunction> fn = lib ? [lib newFunctionWithName:@"taa_mv"] : nil;
    g.mv_ps = fn ? [dev newComputePipelineStateWithFunction:fn error:&e] : nil;
    MTL4ArgumentTableDescriptor *ad = [MTL4ArgumentTableDescriptor new]; ad.maxBufferBindCount = 1; ad.maxTextureBindCount = 5;
    for (int i = 0; i < TAA_SLOTS && g.rs; i++) {
        g.args[i] = [dev newArgumentTableWithDescriptor:ad error:&e];
        g.par[i] = [dev newBufferWithLength:256 options:MTLResourceStorageModeShared];
        if (g.par[i]) [g.rs addAllocation:g.par[i]];
    }
    if (!g.comp || !g.rs || !g.mv_ps || !g.args[TAA_SLOTS - 1] || !g.par[TAA_SLOTS - 1]) {
        TLOG("not available: %s", e ? e.localizedDescription.UTF8String : "MetalFX / Metal 4 objects");
        g.failed = 1; return 0;
    }
    [g.rs commit];
    g.fence = [dev newFence];
    g.dev = dev;
    return 1;
}
static int reactive_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("TOTK_TAA_REACTIVE"); v = !(e && !strcmp(e, "0")); } return v; }
static id<MTLTexture> mk(MTLPixelFormat f, NSUInteger w, NSUInteger h, MTLTextureUsage u) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f width:w height:h mipmapped:NO];
    d.usage = u; d.storageMode = MTLStorageModePrivate;
    id<MTLTexture> t = [g.dev newTextureWithDescriptor:d];
    if (t) [g.rs addAllocation:t];
    return t;
}
static int ensure_scaler(id<MTLTexture> src) {
    if (g.sc && g.w == src.width && g.h == src.height && g.fmt == src.pixelFormat) return 1;
    if (![MTLFXTemporalScalerDescriptor supportsMetal4FX:g.dev]) { g.failed = 1; TLOG("this GPU has no Metal 4 MetalFX temporal scaler"); return 0; }
    MTLFXTemporalScalerDescriptor *d = [MTLFXTemporalScalerDescriptor new];
    /* The output is RGBA16Float, not the scene's RG11B10Float: written back into 11 / 11 / 10 bits, red and blue rounded
       differently from frame to frame (the hazy distance flickered purple: red / blue flicker 3.9 / 4.0 against green 3.0;
       with RGBA16Float 3.1 / 2.9 against 2.6, below the game's own 3.8 / 3.2 / 3.0 without anti-aliasing). */
    MTLPixelFormat of = MTLPixelFormatRGBA16Float;
    d.colorTextureFormat = src.pixelFormat; d.outputTextureFormat = of;
    d.depthTextureFormat = MTLPixelFormatDepth32Float; d.motionTextureFormat = MTLPixelFormatRG16Float;
    d.inputWidth = d.outputWidth = src.width; d.inputHeight = d.outputHeight = src.height;
    /* No auto exposure: MetalFX's own estimate jumped now and then (after a few minutes of play, worst at dusk and at
       night), and the game's tone mapping turned that into a different tint of the whole frame for a moment: the sky
       went from (28, 98, 122) to (46, 83, 76) between two captures and hazy rocks seemed to glow. With the exposure
       left at 1 the same five minutes stayed smooth and flickered less than without anti-aliasing. */
    d.autoExposureEnabled = NO;
    d.reactiveMaskTextureEnabled = YES; d.reactiveMaskTextureFormat = MTLPixelFormatR16Float;   // all zero with TOTK_TAA_REACTIVE=0
    id<MTL4FXTemporalScaler> sc = [d newTemporalScalerWithDevice:g.dev compiler:g.comp];
    if (!sc) { g.retry_at = g.frame + 300; TLOG("temporal scaler %lux%lu (format %lu) not created; trying again in 300 frames", (unsigned long)src.width, (unsigned long)src.height, (unsigned long)src.pixelFormat); return 0; }
    keep_old(g.out); keep_old(g.mv); keep_old(g.react); keep_old(g.dh[0]); keep_old(g.dh[1]);
    keep_old(g.sc);
    MTLTextureUsage ru;   // what MetalFX needs of the mask: renamed reactiveMaskTextureUsage in the 27 SDKs, older SDKs lack it
#if defined(__MAC_27_0)
    if (@available(macOS 27.0, iOS 27.0, *)) ru = sc.reactiveMaskTextureUsage; else
#endif
    {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        ru = sc.reactiveTextureUsage;
#pragma clang diagnostic pop
    }
    g.out = mk(of, src.width, src.height, sc.outputTextureUsage | MTLTextureUsageShaderRead);
    g.mv = mk(MTLPixelFormatRG16Float, src.width, src.height, sc.motionTextureUsage | MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead);
    g.react = mk(MTLPixelFormatR16Float, src.width, src.height, ru | MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead);
    for (int i = 0; i < 2; i++) g.dh[i] = mk(MTLPixelFormatR32Float, src.width, src.height, MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead);
    [g.rs commit];
    if (!g.out || !g.mv || !g.react || !g.dh[0] || !g.dh[1]) { g.sc = nil; g.retry_at = g.frame + 300; TLOG("textures for %lux%lu not created; trying again in 300 frames", (unsigned long)src.width, (unsigned long)src.height); return 0; }
    g.sc = sc; g.w = src.width; g.h = src.height; g.fmt = src.pixelFormat; g.prev_ok = 0;
    TLOG("temporal scaler %lux%lu, scene format %lu, output RGBA16Float, 1:1 with jitter, reactive mask from depth %s", (unsigned long)g.w, (unsigned long)g.h, (unsigned long)g.fmt,
         reactive_on() ? "on" : "off (TOTK_TAA_REACTIVE=0)");
    return 1;
}
// 4x4 inverse (Gauss-Jordan, partial pivoting) and product, row-major, in double (world coordinates in the thousands).
static int inv4(const double a_[16], double r[16]) {
    double a[16]; memcpy(a, a_, sizeof a);
    for (int i = 0; i < 16; i++) r[i] = i % 5 == 0;
    for (int c = 0; c < 4; c++) {
        int p = c; for (int k = c + 1; k < 4; k++) if (fabs(a[k * 4 + c]) > fabs(a[p * 4 + c])) p = k;
        if (fabs(a[p * 4 + c]) < 1e-12) return 0;
        if (p != c) for (int k = 0; k < 4; k++) { double t = a[c * 4 + k]; a[c * 4 + k] = a[p * 4 + k]; a[p * 4 + k] = t; t = r[c * 4 + k]; r[c * 4 + k] = r[p * 4 + k]; r[p * 4 + k] = t; }
        double s = 1.0 / a[c * 4 + c];
        for (int k = 0; k < 4; k++) { a[c * 4 + k] *= s; r[c * 4 + k] *= s; }
        for (int q = 0; q < 4; q++) if (q != c) { double f = a[q * 4 + c]; if (f != 0) for (int k = 0; k < 4; k++) { a[q * 4 + k] -= f * a[c * 4 + k]; r[q * 4 + k] -= f * r[c * 4 + k]; } }
    }
    return 1;
}
static void mul4(const double a[16], const double b[16], double r[16]) {
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) { double s = 0; for (int k = 0; k < 4; k++) s += a[i * 4 + k] * b[k * 4 + j]; r[i * 4 + j] = s; }
}
static int camera_cut(void) {
    double dx = g.cam[47] - g.prev[47], dy = g.cam[51] - g.prev[51], dz = g.cam[55] - g.prev[55];
    double f = g.cam[8] * g.prev[8] + g.cam[9] * g.prev[9] + g.cam[10] * g.prev[10];   // view z axes
    return sqrt(dx * dx + dy * dy + dz * dz) > 8.0 || f < cos(25.0 * M_PI / 180.0);
}

id<MTLTexture> taa_encode(id<MTL4CommandBuffer> cb, id<MTLTexture> src) {
    if (!g.active || g.frame < g.retry_at || !cb || !src || src.textureType != MTLTextureType2D) return nil;
    if (g.encoded_frame == g.frame + 1 && src == g.last_src) return g.out;   // the compose shader again in the same frame
    id<MTLTexture> depth = g.depth;
    if (!depth || !g.cam_ok || depth.width != src.width || depth.height != src.height) { g.missing++; g.prev_ok = 0; return nil; }
    if (!setup(src.device) || !ensure_scaler(src)) return nil;
    int reset = !g.prev_ok || g.encoded_frame != g.frame || camera_cut();   // also after a frame without anti-aliasing (menus, loading)
    double vc[16], vp[16], ic[16], m[16];
    for (int i = 0; i < 16; i++) { vc[i] = g.cam[12 + i]; vp[i] = reset ? g.cam[12 + i] : g.prev[12 + i]; }
    if (!inv4(vc, ic)) { g.missing++; g.prev_ok = 0; return nil; }
    mul4(vp, ic, m);
    int s = g.slot = (g.slot + 1) % TAA_SLOTS;
    float *par = g.par[s].contents;
    for (int c = 0; c < 4; c++) for (int r = 0; r < 4; r++) par[c * 4 + r] = (float)m[r * 4 + c];   // float4x4 is column-major
    par[16] = (float)src.width; par[17] = (float)src.height; par[18] = 1.0f / src.width; par[19] = 1.0f / src.height;
    par[20] = !reset && reactive_on() ? 1.0f : 0.0f;   // last frame's depth (dh[dh_i ^ 1]) belongs to this history
    [g.args[s] setTexture:depth.gpuResourceID atIndex:0]; [g.args[s] setTexture:g.mv.gpuResourceID atIndex:1]; [g.args[s] setAddress:g.par[s].gpuAddress atIndex:0];
    [g.args[s] setTexture:g.dh[g.dh_i ^ 1].gpuResourceID atIndex:2]; [g.args[s] setTexture:g.dh[g.dh_i].gpuResourceID atIndex:3];
    [g.args[s] setTexture:g.react.gpuResourceID atIndex:4];
    g.dh_i ^= 1;
    id<MTL4ComputeCommandEncoder> e = [cb computeCommandEncoder];
    [e barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageDispatch visibilityOptions:MTL4VisibilityOptionDevice];   // the scene and depth are done
    [e setComputePipelineState:g.mv_ps]; [e setArgumentTable:g.args[s]];
    [e dispatchThreads:MTLSizeMake(src.width, src.height, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
    [e updateFence:g.fence afterEncoderStages:MTLStageDispatch];
    [e endEncoding];
    g.sc.colorTexture = src; g.sc.depthTexture = depth; g.sc.motionTexture = g.mv; g.sc.outputTexture = g.out; g.sc.reactiveMaskTexture = g.react;
    g.sc.inputContentWidth = src.width; g.sc.inputContentHeight = src.height;
    g.sc.jitterOffsetX = g.jit_used ? (float)g.jx : 0; g.sc.jitterOffsetY = g.jit_used ? (float)g.jy : 0;   // what the geometry got
    g.sc.motionVectorScaleX = 1; g.sc.motionVectorScaleY = 1; g.sc.depthReversed = NO; g.sc.reset = reset; g.sc.fence = g.fence;
    [g.sc encodeToCommandBuffer:cb];
    g.sc.colorTexture = nil; g.sc.depthTexture = nil; g.sc.motionTexture = nil; g.sc.outputTexture = nil; g.sc.reactiveMaskTexture = nil;
    memcpy(g.prev, g.cam, sizeof g.cam); g.prev_ok = 1;
    g.resets += reset; g.done++;
    g.last_src = src; g.encoded_frame = g.frame + 1;
    return g.out;
}
void taa_frame_end(void) {
    int want = taa_enabled(); if (want != g.active) { g.active = want; g.prev_ok = 0; }
    g.frame++; g.depth = nil; g.cam_ok = 0; g.jit_pass = 0; g.jit_used = 0;
    /* The geometry is jittered only after a frame MetalFX took: when it cannot run (no Metal 4 temporal scaler, a scaler
       not built yet, no camera), a moved picture with nothing to smooth it would shake. */
    g.jit_ok = g.active && !g.failed && g.frame >= g.retry_at && g.encoded_frame == g.frame;
    g.jit_i = g.jit_i % 16 + 1;   // Halton 2, 3 over 16 frames, centred on the pixel
    g.jx = halton(g.jit_i, 2) - 0.5; g.jy = halton(g.jit_i, 3) - 0.5;
    if (g.frame % 1800 == 0 && taa_enabled()) { char b[200]; taa_stats(b, sizeof b); TLOG("%s", b); }
}
void taa_stats(char *out, size_t n) {
    snprintf(out, n, "frame %llu: anti-aliased %llu, history resets %llu, without depth or camera %llu",
             (unsigned long long)g.frame, (unsigned long long)g.done, (unsigned long long)g.resets, (unsigned long long)g.missing);
}

#endif   // TARGET_OS_VISION
