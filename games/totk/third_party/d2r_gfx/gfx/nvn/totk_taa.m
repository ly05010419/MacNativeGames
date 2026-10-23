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
//    MetalFX rejects what does not match, at the cost of less smoothing there.
//  - Reset: the first frame, a size change and a camera cut (more than 8 units or 25 degrees in one frame).
#import <MetalFX/MetalFX.h>
#include <dispatch/dispatch.h>
#include <math.h>
#include <os/lock.h>
#include <time.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "totk_taa.h"

#define TLOG(...) do { fprintf(stderr, "[dm] [nvn-mtl] [taa] " __VA_ARGS__); fputc('\n', stderr); } while (0)
/* Per-frame parameter buffers and argument tables: the replay can encode several frames before the GPU runs the first
   one, and a slot rewritten too early gives MetalFX the wrong camera for that frame (the history then lands beside
   itself; it showed as a purple shimmer on hazy rocks that faded whenever a screenshot waited for the GPU). */
#define TAA_SLOTS 16
#define TAA_OLD TAA_SLOTS   // replaced scalers and textures are kept as long (Metal 4 command buffers do not retain)

static struct {
    id<MTLDevice> dev; id<MTL4Compiler> comp; id<MTLResidencySet> rs; id<MTLFence> fence;
    id<MTL4FXTemporalScaler> sc; NSUInteger w, h; MTLPixelFormat fmt;
    id<MTLTexture> out, mv;
    id<MTLComputePipelineState> mv_ps; id<MTL4ArgumentTable> args[TAA_SLOTS]; id<MTLBuffer> par[TAA_SLOTS]; int slot;
    __unsafe_unretained id<MTLTexture> depth;   // this frame's G-buffer depth (the game's texture, alive for the frame)
    double cam[84], prev[84]; int cam_ok, prev_ok, jit_pass, failed;
    int active, jit_ok, jit_used;   // active: latched from key 5 at frame end; jit_ok: this frame may jitter (see taa_frame_end)
    uint64_t retry_at;              // a scaler that could not be built is tried again from this frame on
    unsigned jit_i; double jx, jy;
    __unsafe_unretained id<MTLTexture> last_src; uint64_t frame, encoded_frame;
    uint64_t done, resets, missing;
    struct { id obj; uint64_t frame; } old[TAA_OLD * 3]; int nold;
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
        "struct P { float4x4 reproj; float2 size; float2 inv_size; };\n"
        // camera motion of each depth pixel: where it was in the previous frame, in pixels of this texture
        "kernel void taa_mv(depth2d<float, access::read> d [[texture(0)]], texture2d<float, access::write> mv [[texture(1)]],\n"
        "                   constant P &p [[buffer(0)]], uint2 g [[thread_position_in_grid]]) {\n"
        "  if (g.x >= uint(p.size.x) || g.y >= uint(p.size.y)) return;\n"
        "  float2 uv = (float2(g) + 0.5) * p.inv_size;\n"
        "  float4 c = p.reproj * float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, d.read(g) * 2.0 - 1.0, 1.0);\n"
        "  float2 m = 0.0;\n"
        "  if (c.w > 1e-7) { float2 n = c.xy / c.w; m = (float2(n.x * 0.5 + 0.5, 0.5 - 0.5 * n.y) - uv) * p.size; }\n"
        "  if (any(isnan(m)) || any(isinf(m))) m = 0.0;\n"
        "  mv.write(float4(m, 0.0, 0.0), g);\n"
        "}\n";
    MTLCompileOptions *o = [MTLCompileOptions new]; o.mathMode = MTLMathModeSafe;
    id<MTLLibrary> lib = [dev newLibraryWithSource:@(src) options:o error:&e];
    id<MTLFunction> fn = lib ? [lib newFunctionWithName:@"taa_mv"] : nil;
    g.mv_ps = fn ? [dev newComputePipelineStateWithFunction:fn error:&e] : nil;
    MTL4ArgumentTableDescriptor *ad = [MTL4ArgumentTableDescriptor new]; ad.maxBufferBindCount = 1; ad.maxTextureBindCount = 2;
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
static id<MTLTexture> mk(MTLPixelFormat f, NSUInteger w, NSUInteger h, MTLTextureUsage u) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f width:w height:h mipmapped:NO];
    d.usage = u; d.storageMode = MTLStorageModePrivate;
    id<MTLTexture> t = [g.dev newTextureWithDescriptor:d];
    if (t) [g.rs addAllocation:t];
    return t;
}
/* A scaler was built on the submit thread at every size change; the game's dynamic resolution changes size in play
   (1600x900 -> 1436x808 -> 1600x900), and two such builds came with 930 / 906 ms frames (Tears of the Kingdom 1.1.2,
   2026-10-23; the build's own time was not measured then, later ones took 24-49 ms). Built on a background queue
   instead, the frames meanwhile go without anti-aliasing (as frames without a camera do); the scalers of the last
   TAA_CACHE sizes are kept, so a resolution that comes back needs no build. TOTK_TAA_SYNC=1 builds on the submit
   thread as before (for comparison). */
#define TAA_CACHE 4
static struct { id<MTL4FXTemporalScaler> sc; id<MTLTexture> out, mv; NSUInteger w, h; MTLPixelFormat fmt; } g_cache[TAA_CACHE];
static int g_ncache;   // submit thread only
static os_unfair_lock g_pend_lock = OS_UNFAIR_LOCK_INIT;
static struct { int busy, failed; NSUInteger w, h; MTLPixelFormat fmt; id<MTL4FXTemporalScaler> done; double ms; } g_pend;   // g_pend_lock
static uint64_t g_wait_frames;
static double taa_now_ms(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e6; }
static int taa_sync(void) { static int s = -1; if (s < 0) { const char *e = getenv("TOTK_TAA_SYNC"); s = e && strcmp(e, "0"); } return s; }
static id<MTL4FXTemporalScaler> new_scaler(NSUInteger w, NSUInteger h, MTLPixelFormat fmt);
static int ensure_scaler(id<MTLTexture> src) {
    NSUInteger w = src.width, h = src.height; MTLPixelFormat f = src.pixelFormat;
    if (g.sc && g.w == w && g.h == h && g.fmt == f) return 1;
    if (![MTLFXTemporalScalerDescriptor supportsMetal4FX:g.dev]) { g.failed = 1; TLOG("this GPU has no Metal 4 MetalFX temporal scaler"); return 0; }
    for (int i = 0; i < g_ncache; i++) if (g_cache[i].w == w && g_cache[i].h == h && g_cache[i].fmt == f) {
        g.sc = g_cache[i].sc; g.out = g_cache[i].out; g.mv = g_cache[i].mv; g.w = w; g.h = h; g.fmt = f; g.prev_ok = 0;
        TLOG("temporal scaler %lux%lu again (kept from before, no build)", (unsigned long)w, (unsigned long)h);
        return 1;
    }
    id<MTL4FXTemporalScaler> sc = nil; double ms = 0;
    if (taa_sync()) {
        double t0 = taa_now_ms(); sc = new_scaler(w, h, f); ms = taa_now_ms() - t0;
        if (!sc) { g.retry_at = g.frame + 300; TLOG("temporal scaler %lux%lu (format %lu) not created after %.1f ms; trying again in 300 frames", (unsigned long)w, (unsigned long)h, (unsigned long)f, ms); return 0; }
    } else {
        int start = 0, failed = 0;
        os_unfair_lock_lock(&g_pend_lock);
        int match = g_pend.w == w && g_pend.h == h && g_pend.fmt == f;
        if (g_pend.done && match) { sc = g_pend.done; g_pend.done = nil; ms = g_pend.ms; }
        else if (g_pend.failed && match) { g_pend.failed = 0; failed = 1; ms = g_pend.ms; }
        else if (!g_pend.busy) { g_pend.done = nil; g_pend.failed = 0; g_pend.busy = 1; g_pend.w = w; g_pend.h = h; g_pend.fmt = f; start = 1; }
        os_unfair_lock_unlock(&g_pend_lock);   // busy with another size: that build ends first, the next frame starts this one
        if (failed) { g.retry_at = g.frame + 300; TLOG("temporal scaler %lux%lu (format %lu) not created after %.1f ms in the background; trying again in 300 frames", (unsigned long)w, (unsigned long)h, (unsigned long)f, ms); return 0; }
        if (start) {
            static dispatch_queue_t q; if (!q) q = dispatch_queue_create("totk.taa.scaler", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INITIATED, 0));
            dispatch_async(q, ^{
                double t0 = taa_now_ms(); id<MTL4FXTemporalScaler> s = new_scaler(w, h, f); double dt = taa_now_ms() - t0;
                os_unfair_lock_lock(&g_pend_lock); g_pend.busy = 0; g_pend.done = s; g_pend.failed = !s; g_pend.ms = dt; os_unfair_lock_unlock(&g_pend_lock);
            });
            TLOG("temporal scaler %lux%lu: building in the background, frames go without anti-aliasing meanwhile", (unsigned long)w, (unsigned long)h);
        }
        if (!sc) { g_wait_frames++; return 0; }
    }
    id<MTLTexture> out = mk(MTLPixelFormatRGBA16Float, w, h, sc.outputTextureUsage | MTLTextureUsageShaderRead);
    id<MTLTexture> mv = mk(MTLPixelFormatRG16Float, w, h, sc.motionTextureUsage | MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead);
    [g.rs commit];
    if (!out || !mv) { g.retry_at = g.frame + 300; TLOG("textures for %lux%lu not created; trying again in 300 frames", (unsigned long)w, (unsigned long)h); return 0; }
    if (g_ncache == TAA_CACHE) {   // the oldest size goes; the GPU may still read it for TAA_OLD frames
        keep_old(g_cache[0].out); keep_old(g_cache[0].mv); keep_old(g_cache[0].sc);
        for (int i = 1; i < TAA_CACHE; i++) g_cache[i - 1] = g_cache[i];   // plain assignment: ARC keeps the counts right
        g_cache[TAA_CACHE - 1].sc = nil; g_cache[TAA_CACHE - 1].out = nil; g_cache[TAA_CACHE - 1].mv = nil; g_ncache--;
    }
    g_cache[g_ncache].sc = sc; g_cache[g_ncache].out = out; g_cache[g_ncache].mv = mv; g_cache[g_ncache].w = w; g_cache[g_ncache].h = h; g_cache[g_ncache].fmt = f; g_ncache++;
    g.sc = sc; g.out = out; g.mv = mv; g.w = w; g.h = h; g.fmt = f; g.prev_ok = 0;
    TLOG("temporal scaler %lux%lu, scene format %lu, output RGBA16Float, 1:1 with jitter; built in %.1f ms %s (%llu frames without anti-aliasing so far while waiting)",
         (unsigned long)w, (unsigned long)h, (unsigned long)f, ms, taa_sync() ? "on the submit thread" : "in the background", (unsigned long long)g_wait_frames);
    return 1;
}
static id<MTL4FXTemporalScaler> new_scaler(NSUInteger w, NSUInteger h, MTLPixelFormat fmt) {   // any thread: g.dev and g.comp do not change after setup
    MTLFXTemporalScalerDescriptor *d = [MTLFXTemporalScalerDescriptor new];
    /* The output is RGBA16Float, not the scene's RG11B10Float: written back into 11 / 11 / 10 bits, red and blue rounded
       differently from frame to frame (the hazy distance flickered purple: red / blue flicker 3.9 / 4.0 against green 3.0;
       with RGBA16Float 3.1 / 2.9 against 2.6, below the game's own 3.8 / 3.2 / 3.0 without anti-aliasing). */
    MTLPixelFormat of = MTLPixelFormatRGBA16Float;
    d.colorTextureFormat = fmt; d.outputTextureFormat = of;
    d.depthTextureFormat = MTLPixelFormatDepth32Float; d.motionTextureFormat = MTLPixelFormatRG16Float;
    d.inputWidth = d.outputWidth = w; d.inputHeight = d.outputHeight = h;
    /* No auto exposure: MetalFX's own estimate jumped now and then (after a few minutes of play, worst at dusk and at
       night), and the game's tone mapping turned that into a different tint of the whole frame for a moment: the sky
       went from (28, 98, 122) to (46, 83, 76) between two captures and hazy rocks seemed to glow. With the exposure
       left at 1 the same five minutes stayed smooth and flickered less than without anti-aliasing. */
    d.autoExposureEnabled = NO;
    return [d newTemporalScalerWithDevice:g.dev compiler:g.comp];
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
    [g.args[s] setTexture:depth.gpuResourceID atIndex:0]; [g.args[s] setTexture:g.mv.gpuResourceID atIndex:1]; [g.args[s] setAddress:g.par[s].gpuAddress atIndex:0];
    id<MTL4ComputeCommandEncoder> e = [cb computeCommandEncoder];
    [e barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageDispatch visibilityOptions:MTL4VisibilityOptionDevice];   // the scene and depth are done
    [e setComputePipelineState:g.mv_ps]; [e setArgumentTable:g.args[s]];
    [e dispatchThreads:MTLSizeMake(src.width, src.height, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
    [e updateFence:g.fence afterEncoderStages:MTLStageDispatch];
    [e endEncoding];
    g.sc.colorTexture = src; g.sc.depthTexture = depth; g.sc.motionTexture = g.mv; g.sc.outputTexture = g.out;
    g.sc.inputContentWidth = src.width; g.sc.inputContentHeight = src.height;
    g.sc.jitterOffsetX = g.jit_used ? (float)g.jx : 0; g.sc.jitterOffsetY = g.jit_used ? (float)g.jy : 0;   // what the geometry got
    g.sc.motionVectorScaleX = 1; g.sc.motionVectorScaleY = 1; g.sc.depthReversed = NO; g.sc.reset = reset; g.sc.fence = g.fence;
    [g.sc encodeToCommandBuffer:cb];
    g.sc.colorTexture = nil; g.sc.depthTexture = nil; g.sc.motionTexture = nil; g.sc.outputTexture = nil;
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
