// TOTK: MetalFX temporal anti-aliasing (TOTK_TAA=1; not in D2R).
// The game renders without anti-aliasing of its own beyond FXAA after tone mapping, so edges stair-step at the scene
// size. A MetalFX temporal scaler at 1:1 (input and output the scene size) runs on the linear HDR scene just before the
// game's compose shader 0ac9a549 (tone mapping and bloom) samples it; the replay then binds our output instead (as
// MFXUP_SCENE does with the spatial scaler).
//  - Jitter: every frame the geometry drawn into the G-buffer depth has its viewport moved by a sub-pixel offset
//    (Halton 2, 3; no shader changes; full-screen triangles and quads stay put). MetalFX gets the same offset.
//  - The settings panel (key 1, runtime/frontend.m) turns it on and off while playing; TOTK_TAA=1 starts it on.
//  - Motion vectors: from the camera only, as for frame interpolation (totk_mfx.m): the G-buffer depth reprojected with
//    this and the previous frame's view-projection. What moves on its own (Link, enemies) gets the camera's motion;
//    MetalFX rejects what does not match, at the cost of less smoothing there. The game keeps no motion vectors of its
//    own (the scene-size R32Float target its G-buffer passes write, a4eb9e94, is linear depth).
//  - Reactive mask (TOTK_TAA_REACTIVE=0 turns it off): MetalFX's colour clamp alone let walking characters leave trails.
//    The same kernel keeps the depth of each frame and checks the surface the camera motion points to: when last frame's
//    depth there is not the depth that surface should have had (nearest of 3x3, so edges and jitter still pass), something
//    moved in or out of that pixel, and MetalFX is told to take the current frame there instead of the history.
//  - Reset: the first frame, a size change and a camera cut (more than 8 units or 25 degrees in one frame).
// Temporal upscaling (TOTK_MFX_TEMPORAL=1 or flag mfx_temporal; experimental, off by default): the same jitter, camera,
// motion vectors and reactive mask, but the scaler goes from the scene size to the canvas, at the game's own upscale draw
// (EASU ac58fc18, the draw MetalFX spatial upscaling replaces, nvn_mtl.m tup_dock) instead of the compose draw:
//  - Input: the composed picture (tone mapping and bloom done, display values) before the game's FXAA pass 22a61cb1,
//    whose blur of the jittered edges would only hide what the scaler resolves; the FXAA output when the compose output
//    is not at hand. The picture, the G-buffer depth and our motion texture share the scene's pixel grid.
//  - Output: RGBA16Float at the canvas size, drawn into the canvas before the UI (the UI stays at full resolution, and
//    frame interpolation takes this picture as its scene). One temporal pass per frame: it replaces key 5's while the
//    game draws an upscale it can take (a scene as large as the canvas, or the handheld chain, keep key 5's).
//  - Jitter: ceil(8 x scale^2) Halton phases (21 for 1600x896 -> 2560x1440) rather than 16.
//  - Sharpening: the copy into the canvas applies FSR 1's RCAS (TOTK_MFX_TEMPORAL_SHARPEN, default 0.5; see up_sharpen).
//  - Scalers are built on a background queue, where the system may compile MetalFX's network first (seconds once), and
//    kept per size (the game's dynamic resolution reallocates the scene); meanwhile the caller keeps its own upscale.
//  - On M5 Pro / M5 Max (macOS 27) the scaler runs its network on the Neural Engine: that work is on the command buffer's
//    timeline (the GPU waits for it) but not on the GPU's cores.
#import <MetalFX/MetalFX.h>
#include <dispatch/dispatch.h>
#include <math.h>
#include <os/lock.h>
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
void taa_set(int on) { (void)on; }
int taa_pass(MTL4RenderPassDescriptor *pd) { (void)pd; return 0; }
int taa_jittered(void) { return 0; }
void taa_jitter(double *x, double *y) { *x = 0; *y = 0; }
void taa_note_camera(const float *cam, int n) { (void)cam; (void)n; }
id<MTLTexture> taa_encode(id<MTL4CommandBuffer> cb, id<MTLTexture> scene) { (void)cb; (void)scene; return nil; }
id<MTLResidencySet> taa_residency(void) { return nil; }
void taa_frame_end(void) {}
void taa_stats(char *out, size_t n) { snprintf(out, n, "not available on visionOS"); }
int tup_enabled(void) { return 0; }
void tup_set(int on) { (void)on; }
float tup_mip_bias(void) { return 0; }
int tup_ready(id<MTLTexture> color, NSUInteger cw, NSUInteger ch, id<MTLTexture> dst) { (void)color; (void)cw; (void)ch; (void)dst; return 0; }
int tup_encode(id<MTL4CommandBuffer> cb, id<MTLTexture> color, NSUInteger cw, NSUInteger ch, id<MTLTexture> dst, void (^mark)(char kind)) {
    (void)cb; (void)color; (void)cw; (void)ch; (void)dst; (void)mark; return 0;
}
void tup_stats(char *out, size_t n) { snprintf(out, n, "not available on visionOS"); }
void tup_log_frames(long n) { (void)n; }
void tup_note_site(void) {}
int tup_active(void) { return 0; }
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
    int up;                         // latched with active: this frame's temporal pass is the upscale (tup_encode), not key 5's
    uint64_t retry_at;              // a scaler that could not be built is tried again from this frame on
    unsigned jit_i, phases; double jx, jy;   // phases: of the Halton jitter, 16 for key 5, ceil(8 x scale^2) when upscaling
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
/* The settings panel: on / off while playing (the main thread). The replay takes it at the next frame end, so a frame is
   either jittered in every pass on the G-buffer depth or in none (foliage drawn twice with an equal depth test needs both
   alike). */
void taa_set(int on) {
    on = !!on;
    if (on == taa_enabled()) return;
    atomic_store(&g_on, on);
    TLOG("settings: temporal anti-aliasing %s", on ? "on" : "off");
}
int taa_active(void) { return g.active; }
int taa_ready(void) { return g.active && !g.up && !g.failed && g.frame >= g.retry_at && g.depth && g.cam_ok; }
static double halton(unsigned i, unsigned b) { double f = 1, r = 0; while (i) { f /= b; r += f * (i % b); i /= b; } return r; }

static int up_known(NSUInteger w, NSUInteger h);
int taa_pass(MTL4RenderPassDescriptor *pd) {
    if (!g.active) return 0;
    id<MTLTexture> c0 = pd.colorAttachments[0].texture, dt = pd.depthAttachment.texture;
    if (!g.depth && dt && c0 && dt.pixelFormat == MTLPixelFormatDepth32Float && dt.textureType == MTLTextureType2D &&
        dt.width == c0.width && dt.height == c0.height && dt.width >= 320) {
        int nc = 0; for (int i = 0; i < 8; i++) nc += pd.colorAttachments[i].texture != nil;
        if (nc >= 3) {
            if (g.up && !up_known(dt.width, dt.height)) g.jit_ok = 0;   // a new scene size: its scaler is built in the background, the frame goes without
            g.depth = dt; g.jit_pass = g.jit_ok; return 2;
        }
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
static MTLTextureUsage react_usage(id<MTLFXTemporalScalerBase> sc) {   // what MetalFX needs of the mask: renamed reactiveMaskTextureUsage in the 27 SDKs, older SDKs lack it
#if defined(__MAC_27_0)
    if (@available(macOS 27.0, iOS 27.0, *)) return sc.reactiveMaskTextureUsage;
#endif
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    return sc.reactiveTextureUsage;
#pragma clang diagnostic pop
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
    MTLTextureUsage ru = react_usage(sc);
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
    if (!g.active || g.up || g.frame < g.retry_at || !cb || !src || src.textureType != MTLTextureType2D) return nil;
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
/* ---- Temporal upscaling (TOTK_MFX_TEMPORAL; see the header) ---- */
#define UP_CACHE 3   // scalers kept with their outputs, by size: a dynamic-resolution level that comes back needs no build
typedef struct { id<MTL4FXTemporalScaler> sc; id<MTLTexture> out; NSUInteger w, h, ow, oh; MTLPixelFormat fmt; uint64_t used, enc_ns; double ms, warm_ms; } up_t;
static struct {
    up_t cache[UP_CACHE]; int ncache, cur;                          // the replay thread's; cur: the scaler in use, -1 none
    id<MTLTexture> mv, react, dh[2]; NSUInteger iw, ih; int dh_i;   // at the scene's size, shared by the kept scalers
    id<MTLRenderPipelineState> copy_ps[2]; MTLPixelFormat copy_fmt[2]; id<MTL4ArgumentTable> cargs[TAA_SLOTS];
    NSUInteger cw, ch;                                              // the content of the last frame upscaled
    const char *reset_why;                                          // why the history is gone, for the next frame's log line
    uint64_t done, resets, missing, waited, declined, switches, builds, rewarms, bad_until_ms;
    uint64_t site_frame; int site_seen;                             // the last frame the replay found an upscale draw it could take
    up_t bad;                                                       // the size of a build that failed: not tried again for a while
} u = {.cur = -1};
#define UP_COLD_NS 1000000000ull   // a kept scaler idle this long is encoded again in the background before its next frame (up_rewarm)
static os_unfair_lock u_lock = OS_UNFAIR_LOCK_INIT;
static struct { int busy, has; up_t b; const char *why; } u_pend;   // the background build and its result (u_lock)
static struct { int busy; const void *sc; uint64_t done_ns; double ms; } u_warm;   // the background warm-up of a kept scaler (u_lock)
static _Atomic int g_up_on = -1;
static uint64_t up_now_ms(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000; }

int tup_enabled(void) {
    int on = atomic_load_explicit(&g_up_on, memory_order_relaxed);
    if (on < 0) {
        const char *e = getenv("TOTK_MFX_TEMPORAL"); on = e && strcmp(e, "0");
#ifdef GAME_BOTW
        if (on) TLOG("MetalFX temporal upscaling (TOTK_MFX_TEMPORAL) is wired for Tears of the Kingdom's upscale draw only");
        on = 0;
#endif
        atomic_store(&g_up_on, on);
        TLOG("MetalFX temporal upscaling (TOTK_MFX_TEMPORAL, flag mfx_temporal; experimental): %s", on ? "on" : "off");
    }
    return on;
}
/* flag mfx_temporal: taken at the next frame end, like key 5 (a frame is jittered for one temporal pass or none). */
void tup_set(int on) {
    (void)tup_enabled();
#ifdef GAME_BOTW
    on = 0;
#endif
    atomic_store(&g_up_on, !!on); TLOG("[flag] mfx_temporal = %d", !!on);
}
/* TOTK_MFX_TEMPORAL_MIPBIAS=<lod> (-4..0, with TOTK_MFX_TEMPORAL=1 at launch): added to the level of detail of the
   material samplers (nvn_mtl.m samp_by_id, the ones TOTK_ANISO raises), so that a scene rendered below the output samples
   the finer mip levels the output needs; the scaler resolves the extra aliasing over its jitter phases. Read as the
   samplers are made: a launch setting, which flag mfx_temporal does not change. */
float tup_mip_bias(void) {
    static float b = NAN;
    if (isnan(b)) {
        const char *e = getenv("TOTK_MFX_TEMPORAL_MIPBIAS"), *on = getenv("TOTK_MFX_TEMPORAL"); float v = e ? strtof(e, NULL) : 0;
        b = on && strcmp(on, "0") && tup_enabled() && v >= -4 && v <= 0 ? v : 0;   // with the upscale on at launch only (never Breath of the Wild)
        if (e) TLOG("material samplers' mip bias (TOTK_MFX_TEMPORAL_MIPBIAS): %.2f%s", b, b != v ? " (needs TOTK_MFX_TEMPORAL=1 at launch)" : "");
    }
    return b;
}
static MTLFXTemporalScalerDescriptor *up_desc(NSUInteger w, NSUInteger h, MTLPixelFormat f, NSUInteger ow, NSUInteger oh) {
    MTLFXTemporalScalerDescriptor *d = [MTLFXTemporalScalerDescriptor new];
    d.colorTextureFormat = f; d.outputTextureFormat = MTLPixelFormatRGBA16Float;
    d.depthTextureFormat = MTLPixelFormatDepth32Float; d.motionTextureFormat = MTLPixelFormatRG16Float;
    d.inputWidth = w; d.inputHeight = h; d.outputWidth = ow; d.outputHeight = oh;
    d.autoExposureEnabled = NO;   // display values after tone mapping; key 5's reasons too (ensure_scaler)
    d.reactiveMaskTextureEnabled = YES; d.reactiveMaskTextureFormat = MTLPixelFormatR16Float;
    /* The content may be smaller than the texture (a scene texture kept over a size change, PR #42's case). The largest
       scale stays at 2 where it can: MetalFX sizes its network for the smallest content it is told of (twice that, 2026-10-26
       probe), so a wider range would cost detail in every frame. */
    float sx = (float)ow / w, sy = (float)oh / h;
    float lo = fmaxf(fminf(sx, sy), [MTLFXTemporalScalerDescriptor supportedInputContentMinScaleForDevice:g.dev]);
    float hi = fminf(fmaxf(fmaxf(sx, sy), 2.0f), [MTLFXTemporalScalerDescriptor supportedInputContentMaxScaleForDevice:g.dev]);
    d.inputContentPropertiesEnabled = YES; d.inputContentMinScale = lo; d.inputContentMaxScale = hi > lo ? hi : lo;
    d.requiresSynchronousInitialization = YES;   // built off the frame path: what is handed over runs at full speed at once
    return d;
}
/* Background queue: one encode of a new scaler on a command buffer of its own, waited for. MetalFX finishes setting a
   scaler up at its first encode (about 14 ms of CPU on the encoding thread, 2026-10-26 probe): done here, that is not
   the replay's (a size change showed one 33 ms frame with 16 ms of submit time without it). The textures are throwaway;
   the first real frame resets the history anyway. */
static double up_warm(id<MTL4FXTemporalScaler> sc, id<MTLTexture> out, NSUInteger w, NSUInteger h, MTLPixelFormat f,
                      id<MTLTexture> mv, id<MTLTexture> react) {   // mv, react: the replay's own, when not in use (up_rewarm)
    static id<MTL4CommandQueue> q; static id<MTL4CommandAllocator> al;
    if (!q) { q = [g.dev newMTL4CommandQueue]; al = [g.dev newCommandAllocator]; }
    id<MTLResidencySet> rs = [g.dev newResidencySetWithDescriptor:[MTLResidencySetDescriptor new] error:NULL];
    MTLPixelFormat fs[4] = {f, MTLPixelFormatDepth32Float, MTLPixelFormatRG16Float, MTLPixelFormatR16Float};
    MTLTextureUsage us[4] = {sc.colorTextureUsage, sc.depthTextureUsage, sc.motionTextureUsage, react_usage(sc)};
    id<MTLTexture> t[4] = {nil, nil, mv, react};
    for (int i = 0; i < 4; i++) {
        if (t[i]) { [rs addAllocation:t[i]]; continue; }
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fs[i] width:w height:h mipmapped:NO];
        td.usage = us[i] | MTLTextureUsageShaderRead; td.storageMode = MTLStorageModePrivate;
        if (!(t[i] = [g.dev newTextureWithDescriptor:td])) return -1;
        [rs addAllocation:t[i]];
    }
    if (!q || !al || !rs) return -1;
    [rs addAllocation:out]; [rs commit];
    uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    [al reset];
    id<MTL4CommandBuffer> cb = [g.dev newCommandBuffer];
    [cb beginCommandBufferWithAllocator:al]; [cb useResidencySet:rs];
    sc.colorTexture = t[0]; sc.depthTexture = t[1]; sc.motionTexture = t[2]; sc.reactiveMaskTexture = t[3]; sc.outputTexture = out;
    /* content a real frame can have: within the scaler's scale range (a scene texture taller or wider than the canvas) */
    sc.inputContentWidth = MIN(w, (NSUInteger)(out.width / sc.inputContentMinScale));
    sc.inputContentHeight = MIN(h, (NSUInteger)(out.height / sc.inputContentMinScale));
    sc.jitterOffsetX = 0; sc.jitterOffsetY = 0;
    sc.motionVectorScaleX = 1; sc.motionVectorScaleY = 1; sc.depthReversed = NO; sc.reset = YES; sc.fence = nil;
    [sc encodeToCommandBuffer:cb];
    sc.colorTexture = nil; sc.depthTexture = nil; sc.motionTexture = nil; sc.reactiveMaskTexture = nil; sc.outputTexture = nil;
    [cb endCommandBuffer];
    double enc = (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0) / 1e6;
    dispatch_semaphore_t done = dispatch_semaphore_create(0); __block int bad = 0;
    MTL4CommitOptions *o = [MTL4CommitOptions new];
    [o addFeedbackHandler:^(id<MTL4CommitFeedback> fb) { bad = fb.error != nil; dispatch_semaphore_signal(done); }];
    [q commit:&cb count:1 options:o];
    /* However long it takes: the throwaway textures, the residency set and the allocator may go or be reused only then, and the
       scaler and its output go to the replay after it. Only the next build waits on this queue (the game's path meanwhile). */
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
    return bad ? -1 : enc;
}
/* Background queue: one scaler and its output texture (the residency set is the replay thread's: up_adopt adds it). */
static void up_build(up_t b) {
    uint64_t t0 = up_now_ms(); const char *why = NULL;
    @autoreleasepool {
        b.sc = [up_desc(b.w, b.h, b.fmt, b.ow, b.oh) newTemporalScalerWithDevice:g.dev compiler:g.comp];
        if (!b.sc) why = "scaler not created";
        else {
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:b.ow height:b.oh mipmapped:NO];
            td.usage = b.sc.outputTextureUsage | MTLTextureUsageShaderRead; td.storageMode = MTLStorageModePrivate;
            b.out = [g.dev newTextureWithDescriptor:td];
            if (!b.out) why = "output texture not created";
            else if ((b.warm_ms = up_warm(b.sc, b.out, b.w, b.h, b.fmt, nil, nil)) < 0) why = "first encode failed";
        }
    }
    b.ms = (double)(up_now_ms() - t0);
    if (why) { b.sc = nil; b.out = nil; }
    os_unfair_lock_lock(&u_lock); u_pend.b = b; u_pend.why = why; u_pend.has = 1; u_pend.busy = 0; os_unfair_lock_unlock(&u_lock);
}
static int up_rewarm_on(void) {   // TOTK_MFX_TEMPORAL_REWARM=0: a kept scaler comes back without the warm-up (up_rewarm), for comparisons
    static int v = -1;
    if (v < 0) { const char *e = getenv("TOTK_MFX_TEMPORAL_REWARM"); v = !(e && !strcmp(e, "0")); if (!v) TLOG("temporal upscale: kept scalers come back without a warm-up (TOTK_MFX_TEMPORAL_REWARM=0)"); }
    return v;
}
static int up_cold(const up_t *e) { return up_rewarm_on() && e->enc_ns && clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - e->enc_ns > UP_COLD_NS; }
static int up_known(NSUInteger w, NSUInteger h) {   // a kept scaler takes a scene of this size, now (taa_pass: no jitter for one that does not)
    for (int i = 0; i < u.ncache; i++) if (u.cache[i].w == w && u.cache[i].h == h) return !up_cold(&u.cache[i]);
    return 0;
}
static int up_same(const up_t *e, NSUInteger w, NSUInteger h, MTLPixelFormat f, NSUInteger ow, NSUInteger oh) {
    return e->w == w && e->h == h && e->fmt == f && e->ow == ow && e->oh == oh;
}
static void up_adopt(void) {   // a finished build goes into the kept scalers (the least recently used one other than the current makes room)
    up_t b; const char *why; int has;
    os_unfair_lock_lock(&u_lock); has = u_pend.has; b = u_pend.b; why = u_pend.why; u_pend.has = 0; u_pend.b = (up_t){0}; os_unfair_lock_unlock(&u_lock);
    if (!has) return;
    if (why) {
        u.bad = b; u.bad_until_ms = up_now_ms() + 10000;
        TLOG("temporal upscaler %lux%lu (format %lu) -> %lux%lu: %s after %.0f ms; the game's own upscale stays, trying again in 10 s",
             (unsigned long)b.w, (unsigned long)b.h, (unsigned long)b.fmt, (unsigned long)b.ow, (unsigned long)b.oh, why, b.ms);
        return;
    }
    [g.rs addAllocation:b.out]; [g.rs commit];
    int slot = u.ncache;
    if (slot == UP_CACHE) {
        slot = -1;
        for (int i = 0; i < UP_CACHE; i++) if (i != u.cur && (slot < 0 || u.cache[i].used < u.cache[slot].used)) slot = i;
        os_unfair_lock_lock(&u_lock);
        if (u_warm.sc == (__bridge const void *)u.cache[slot].sc) u_warm.sc = NULL;   // a warm-up done for it is not the new one's
        os_unfair_lock_unlock(&u_lock);
        keep_old(u.cache[slot].sc); keep_old(u.cache[slot].out);   // the GPU may still use them for TAA_OLD frames
    } else u.ncache++;
    b.used = g.frame; u.cache[slot] = b; u.builds++;
    TLOG("temporal upscaler %lux%lu (format %lu) -> %lux%lu RGBA16Float, built in the background in %.0f ms (first encode there %.1f ms) (%s): "
         "content scale %.3f..%.3f, camera motion vectors in input pixels (scale 1), depth not reversed, exposure 1, reactive mask %s",
         (unsigned long)b.w, (unsigned long)b.h, (unsigned long)b.fmt, (unsigned long)b.ow, (unsigned long)b.oh, b.ms, b.warm_ms,
         NSStringFromClass([b.sc class]).UTF8String, b.sc.inputContentMinScale, b.sc.inputContentMaxScale,
         reactive_on() ? "from depth" : "off (TOTK_TAA_REACTIVE=0)");
}
static dispatch_queue_t up_queue(void) {   // the background builds and warm-ups, one at a time
    static dispatch_queue_t q;
    if (!q) q = dispatch_queue_create("totk.taa.upscaler", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0));
    return q;
}
/* A kept scaler that has not run for UP_COLD_NS (a menu, the other canvas size). Its first frame back cost the game 17-38 ms
   (2026-10-27, prologue: after 10 s menus and 5 s at the other canvas size, MetalFX's Neural Engine and GPU networks alike;
   none after 2 s menus): the replay waited in the submission, or the GPU on the queue, while the GPU work itself was as
   usual. A new scaler, whose first encode is made in the background, has no such frame. So a cold one is encoded once more
   in the background first, with the replay's motion and mask textures when they fit (they idled as long); the game's own
   path draws meanwhile, a few frames whose history would be reset anyway. 1 when it is warm again. */
static int up_rewarm(int i) {
    up_t e = u.cache[i]; const void *key = (__bridge const void *)e.sc; uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    os_unfair_lock_lock(&u_lock);
    int busy = u_warm.busy, warm = u_warm.sc == key && u_warm.done_ns && now - u_warm.done_ns < UP_COLD_NS; double ms = u_warm.ms;
    if (!busy && !warm) { u_warm.busy = 1; u_warm.sc = key; u_warm.done_ns = 0; }
    os_unfair_lock_unlock(&u_lock);
    if (warm) {   // this scaler, encoded in the background a moment ago
        u.cache[i].enc_ns = now; u.rewarms++;
        if (u.rewarms <= 20) TLOG("temporal upscaler %lux%lu -> %lux%lu warm again at frame %llu (the background encode took %.1f ms)",
                                  (unsigned long)e.w, (unsigned long)e.h, (unsigned long)e.ow, (unsigned long)e.oh, (unsigned long long)g.frame, ms);
        return 1;
    }
    if (busy) return 0;
    id<MTLTexture> mv = u.mv && u.iw == e.w && u.ih == e.h ? u.mv : nil, react = mv ? u.react : nil;
    if (u.rewarms < 20) TLOG("temporal upscaler %lux%lu -> %lux%lu idle %.0f ms: encoding it again in the background from frame %llu; the game's own upscale meanwhile",
                             (unsigned long)e.w, (unsigned long)e.h, (unsigned long)e.ow, (unsigned long)e.oh,
                             (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - e.enc_ns) / 1e6, (unsigned long long)g.frame);
    dispatch_async(up_queue(), ^{
        up_warm(e.sc, e.out, e.w, e.h, e.fmt, mv, react);   // a failed one only leaves the slow frame
        uint64_t end = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        os_unfair_lock_lock(&u_lock); u_warm.busy = 0; u_warm.done_ns = end; u_warm.ms = (end - now) / 1e6; os_unfair_lock_unlock(&u_lock);
    });
    return 0;
}
/* 1 when the scaler for this input and canvas is current; otherwise it is built (or warmed) in the background (0 meanwhile). */
static int ensure_up(id<MTLTexture> src, id<MTLTexture> dst) {
    NSUInteger w = src.width, h = src.height, ow = dst.width, oh = dst.height; MTLPixelFormat f = src.pixelFormat;
    if (u.cur >= 0 && up_same(&u.cache[u.cur], w, h, f, ow, oh)) {
        if (up_cold(&u.cache[u.cur]) && !up_rewarm(u.cur)) return 0;
        u.cache[u.cur].used = g.frame; return 1;
    }
    up_adopt();
    for (int i = 0; i < u.ncache; i++) if (up_same(&u.cache[i], w, h, f, ow, oh)) {
        if (up_cold(&u.cache[i]) && !up_rewarm(i)) return 0;
        if (u.cur >= 0) {
            u.switches++;
            if (u.switches <= 20) TLOG("temporal upscaler: back to the kept %lux%lu -> %lux%lu scaler at frame %llu (history reset)",
                                       (unsigned long)w, (unsigned long)h, (unsigned long)ow, (unsigned long)oh, (unsigned long long)g.frame);
        }
        u.cur = i; u.cache[i].used = g.frame; g.prev_ok = 0; u.reset_why = "another scaler (a size change)";
        return 1;
    }
    if (u.bad_until_ms && up_now_ms() < u.bad_until_ms && up_same(&u.bad, w, h, f, ow, oh)) return 0;
    if (![MTLFXTemporalScalerDescriptor supportsMetal4FX:g.dev]) { g.failed = 1; TLOG("this GPU has no Metal 4 MetalFX temporal scaler"); return 0; }
    os_unfair_lock_lock(&u_lock); int start = !u_pend.busy && !u_pend.has; if (start) u_pend.busy = 1; os_unfair_lock_unlock(&u_lock);
    if (start) {   // a build for another size still running ends first; the next frame asks again
        up_t b = {.w = w, .h = h, .ow = ow, .oh = oh, .fmt = f};
        TLOG("temporal upscaler %lux%lu -> %lux%lu: building in the background from frame %llu; the game's own upscale (or MetalFX spatial) meanwhile",
             (unsigned long)w, (unsigned long)h, (unsigned long)ow, (unsigned long)oh, (unsigned long long)g.frame);
        dispatch_async(up_queue(), ^{ up_build(b); });
    }
    return 0;
}
static int up_inputs(id<MTL4FXTemporalScaler> sc, NSUInteger w, NSUInteger h) {   // motion, mask and depth history at the scene's size
    MTLTextureUsage mu = sc.motionTextureUsage, ru = react_usage(sc);
    if (u.mv && u.iw == w && u.ih == h && (u.mv.usage & mu) == mu && (u.react.usage & ru) == ru) return 1;
    keep_old(u.mv); keep_old(u.react); keep_old(u.dh[0]); keep_old(u.dh[1]);
    MTLTextureUsage rw = MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead;
    u.mv = mk(MTLPixelFormatRG16Float, w, h, mu | rw); u.react = mk(MTLPixelFormatR16Float, w, h, ru | rw);
    for (int i = 0; i < 2; i++) u.dh[i] = mk(MTLPixelFormatR32Float, w, h, rw);
    [g.rs commit];
    if (!u.mv || !u.react || !u.dh[0] || !u.dh[1]) {   // what was made goes at the next call (keep_old above); not tried again for a while
        u.iw = u.ih = 0; g.retry_at = g.frame + 300;
        TLOG("temporal upscale: textures for %lux%lu not created; trying again in 300 frames", (unsigned long)w, (unsigned long)h); return 0;
    }
    u.iw = w; u.ih = h; g.prev_ok = 0; u.reset_why = "new input textures (a size change)";
    return 1;
}
/* TOTK_MFX_TEMPORAL_SHARPEN=<0..1> (default 0.5): the copy into the canvas sharpens with FSR 1's robust contrast-adaptive
   sharpening (RCAS: a cross of 5 texels, the negative lobe limited so that the result does not clip out of [0, 1]), at that
   share of its full strength; 0 copies texel for texel. MetalFX's temporal scaler has no sharpening of its own, and from
   1600x896 to 2560x1440 its picture came out softer than the game's EASU and the spatial scaler, which both sharpen.
   Against native 2560x1440 (2026-10-26, prologue ruins, world frozen, whole picture): PSNR 37.5 / 38.5 / 36.5 dB and SSIM
   0.949 / 0.957 / 0.955 at 0 / 0.5 / 1 (spatial 36.5 dB / 0.941, EASU 36.8 dB / 0.949); 1 sharpens still surfaces to native's
   edge contrast but Link's hair and outline lose most of their match. A launch setting. */
static float up_sharpen(void) {
    static float v = NAN;
    if (isnan(v)) { const char *e = getenv("TOTK_MFX_TEMPORAL_SHARPEN"); float x = e ? strtof(e, NULL) : 0.5f; v = x > 0 && x <= 1 ? x : 0;
                    TLOG("temporal upscale: RCAS sharpening %.2f (TOTK_MFX_TEMPORAL_SHARPEN)", v); }
    return v;
}
static id<MTLRenderPipelineState> up_copy_ps(MTLPixelFormat f) {   // our output into the canvas, texel for texel (or sharpened)
    for (int i = 0; i < 2; i++) if (u.copy_ps[i] && u.copy_fmt[i] == f) return u.copy_ps[i];
    static id<MTLLibrary> lib; NSError *e = nil;
    if (!lib) {
        char src[2400];
        snprintf(src, sizeof src,
            "#include <metal_stdlib>\nusing namespace metal;\n"
            "struct V { float4 p [[position]]; };\n"
            "vertex V tup_vs(uint id [[vertex_id]]) { float2 uv = float2((id << 1) & 2, id & 2); V o; o.p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
            "constant float SHARP = %.4f;\n"
            "fragment float4 tup_fs(V in [[stage_in]], texture2d<float, access::read> t [[texture(0)]]) {\n"
            "  uint2 p = uint2(in.p.xy); float4 c = t.read(p);\n"
            "  if (SHARP <= 0.0) return c;\n"
            "  uint2 hi = uint2(t.get_width() - 1, t.get_height() - 1);\n"
            "  float3 b = t.read(uint2(p.x, p.y > 0 ? p.y - 1 : 0)).rgb, d = t.read(uint2(p.x > 0 ? p.x - 1 : 0, p.y)).rgb;\n"
            "  float3 f = t.read(uint2(min(p.x + 1, hi.x), p.y)).rgb, h = t.read(uint2(p.x, min(p.y + 1, hi.y))).rgb, e = c.rgb;\n"
            "  if (!all(isfinite(b + d + f + h))) return c;\n"   // a NaN / Inf neighbour stays its own pixel (0 * Inf in the lobe)
            // FSR 1 RCAS (FsrRcasF; MIT, games/totk/third_party/licenses/AMD-FidelityFX-MIT.txt): the strongest negative lobe for which the cross (centre included) does not clip out of [0, 1]
            "  float3 mn = saturate(min(min(b, d), min(f, h))), mx = saturate(max(max(b, d), max(f, h))), ec = saturate(e);\n"
            "  float3 lo = max(-(min(mn, ec) / max(4.0 * mx, 1e-4)), (1.0 - max(mx, ec)) / min(4.0 * mn - 4.0, -1e-4));\n"
            "  float lobe = max(-0.1875, min(max(lo.r, max(lo.g, lo.b)), 0.0)) * SHARP;\n"
            "  return float4((lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0), c.a);\n"
            "}\n", up_sharpen());
        MTLCompileOptions *o = [MTLCompileOptions new]; o.mathMode = MTLMathModeSafe;   // isfinite must not be folded away
        lib = [g.dev newLibraryWithSource:@(src) options:o error:&e];
        if (!lib) { TLOG("temporal upscale: copy shader not compiled: %s", e.localizedDescription.UTF8String); return nil; }
    }
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [lib newFunctionWithName:@"tup_vs"]; pd.fragmentFunction = [lib newFunctionWithName:@"tup_fs"];
    pd.colorAttachments[0].pixelFormat = f;
    id<MTLRenderPipelineState> ps = [g.dev newRenderPipelineStateWithDescriptor:pd error:&e];
    if (!ps) { TLOG("temporal upscale: copy pipeline for format %lu not created: %s", (unsigned long)f, e.localizedDescription.UTF8String); return nil; }
    int i = !u.copy_ps[0] ? 0 : 1;
    keep_old(u.copy_ps[i]);   // a copy pass of the last frames may still use it
    u.copy_ps[i] = ps; u.copy_fmt[i] = f;
    return ps;
}
int tup_ready(id<MTLTexture> color, NSUInteger cw, NSUInteger ch, id<MTLTexture> dst) {
    if (!g.active || !g.up || g.failed || g.frame < g.retry_at) return 0;
    if (g.encoded_frame == g.frame + 1) { u.declined++; return 0; }   // a second upscale draw in one frame: the game's own
    if (!color || !dst || color.textureType != MTLTextureType2D || dst.textureType != MTLTextureType2D || !cw || !ch ||
        cw > color.width || ch > color.height || cw > dst.width || ch > dst.height || (cw == dst.width && ch == dst.height)) { u.declined++; return 0; }   // nothing to upscale
    id<MTLTexture> depth = g.depth;
    if (!depth || !g.cam_ok || depth.width != color.width || depth.height != color.height) {
        u.missing++; g.prev_ok = 0; u.reset_why = "a frame without the G-buffer depth or camera"; return 0;
    }
    if (!setup(color.device) || !ensure_up(color, dst)) { u.waited++; return 0; }
    up_t *e = &u.cache[u.cur];
    if (fmaxf((float)dst.width / cw, (float)dst.height / ch) > e->sc.inputContentMaxScale + 1e-3f) { u.declined++; return 0; }   // content too small for this scaler
    if (!up_inputs(e->sc, color.width, color.height) || !up_copy_ps(dst.pixelFormat)) { u.declined++; return 0; }
    MTL4ArgumentTableDescriptor *ad = nil; NSError *err = nil;
    for (int i = 0; i < TAA_SLOTS; i++) if (!u.cargs[i]) {
        if (!ad) { ad = [MTL4ArgumentTableDescriptor new]; ad.maxTextureBindCount = 1; }
        if (!(u.cargs[i] = [g.dev newArgumentTableWithDescriptor:ad error:&err])) { u.declined++; return 0; }
    }
    return 1;
}
/* The replay found an upscale draw the temporal upscale can take (nvn_mtl.m tup_dock): the temporal pass is the upscale's
   while such draws come (taa_frame_end), key 5's otherwise (a scene as large as the canvas, the handheld chain). */
void tup_note_site(void) { u.site_seen = 1; u.site_frame = g.frame; }
int tup_active(void) { return g.active && g.up; }
static _Atomic long g_up_log;   // flag tup_log <frames>: one line per upscaled frame (jitter, scale, reset, camera motion)
void tup_log_frames(long n) { atomic_store(&g_up_log, n); TLOG("[flag] tup_log: the next %ld upscaled frames", n); }

int tup_encode(id<MTL4CommandBuffer> cb, id<MTLTexture> color, NSUInteger cw, NSUInteger ch, id<MTLTexture> dst, void (^mark)(char kind)) {
    if (u.cur < 0 || !u.mv || !cb || !g.depth) return 0;   // tup_ready first
    up_t *e = &u.cache[u.cur];
    const char *why = !g.prev_ok ? (u.reset_why ? u.reset_why : "the first frame") : g.encoded_frame != g.frame ?
                      "a frame without the upscale before (menu, loading, scaler being built)" : camera_cut() ? "a camera cut" : NULL;
    int reset = why != NULL, content = cw != u.cw || ch != u.ch;
    if (reset) u.resets++;
    if (content) TLOG("temporal upscale: content %lux%lu of the %lux%lu scene -> %lux%lu at frame %llu", (unsigned long)cw, (unsigned long)ch,
                      (unsigned long)color.width, (unsigned long)color.height, (unsigned long)dst.width, (unsigned long)dst.height, (unsigned long long)g.frame);
    u.reset_why = NULL;
    double vc[16], vp[16], ic[16], m[16];
    for (int i = 0; i < 16; i++) { vc[i] = g.cam[12 + i]; vp[i] = reset ? g.cam[12 + i] : g.prev[12 + i]; }
    if (!inv4(vc, ic)) { u.missing++; g.prev_ok = 0; return 0; }
    mul4(vp, ic, m);
    /* Motion and mask as for key 5, over the content (at the texture's origin): input pixels, pointing to the previous
       position. The depth history only belongs to this one when nothing about the input changed. */
    int s = g.slot = (g.slot + 1) % TAA_SLOTS;
    float *par = g.par[s].contents;
    for (int c = 0; c < 4; c++) for (int r = 0; r < 4; r++) par[c * 4 + r] = (float)m[r * 4 + c];
    par[16] = (float)cw; par[17] = (float)ch; par[18] = 1.0f / cw; par[19] = 1.0f / ch;
    par[20] = !reset && !content && reactive_on() ? 1.0f : 0.0f;
    id<MTL4ArgumentTable> a = g.args[s];
    [a setTexture:g.depth.gpuResourceID atIndex:0]; [a setTexture:u.mv.gpuResourceID atIndex:1]; [a setAddress:g.par[s].gpuAddress atIndex:0];
    [a setTexture:u.dh[u.dh_i ^ 1].gpuResourceID atIndex:2]; [a setTexture:u.dh[u.dh_i].gpuResourceID atIndex:3];
    [a setTexture:u.react.gpuResourceID atIndex:4];
    u.dh_i ^= 1;
    if (mark) mark('T');
    id<MTL4ComputeCommandEncoder> ce = [cb computeCommandEncoder];
    [ce barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageDispatch visibilityOptions:MTL4VisibilityOptionDevice];   // the picture and depth are done
    [ce setComputePipelineState:g.mv_ps]; [ce setArgumentTable:a];
    [ce dispatchThreads:MTLSizeMake(cw, ch, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
    [ce updateFence:g.fence afterEncoderStages:MTLStageDispatch];
    [ce endEncoding];
    if (mark) mark('X');
    float jx = g.jit_used ? (float)g.jx : 0, jy = g.jit_used ? (float)g.jy : 0;   // what the geometry got
    id<MTL4FXTemporalScaler> sc = e->sc;
    sc.colorTexture = color; sc.depthTexture = g.depth; sc.motionTexture = u.mv; sc.outputTexture = e->out; sc.reactiveMaskTexture = u.react;
    sc.inputContentWidth = cw; sc.inputContentHeight = ch;
    sc.jitterOffsetX = jx; sc.jitterOffsetY = jy;
    sc.motionVectorScaleX = 1; sc.motionVectorScaleY = 1; sc.depthReversed = NO; sc.reset = reset; sc.fence = g.fence;
    uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    [sc encodeToCommandBuffer:cb];
    uint64_t t1 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    sc.colorTexture = nil; sc.depthTexture = nil; sc.motionTexture = nil; sc.outputTexture = nil; sc.reactiveMaskTexture = nil;
    if (mark) mark('Y');
    if (reset && u.resets <= 40)
        TLOG("temporal upscale: history reset at frame %llu: %s (encode %.2f ms CPU, this scaler idle %.0f ms)", (unsigned long long)g.frame, why,
             (t1 - t0) / 1e6, e->enc_ns ? (t0 - e->enc_ns) / 1e6 : -1.0);
    e->enc_ns = t1;
    /* Into the canvas, every pixel (the game's upscale draw would have written them all; the UI comes after). The pass
       starts after the scaler's work; the replay's next encoder starts with a queue barrier of its own. */
    MTL4RenderPassDescriptor *rp = [MTL4RenderPassDescriptor new];
    rp.colorAttachments[0].texture = dst; rp.colorAttachments[0].loadAction = MTLLoadActionDontCare; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTL4RenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
    [re barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageFragment visibilityOptions:MTL4VisibilityOptionDevice];
    [u.cargs[s] setTexture:e->out.gpuResourceID atIndex:0];
    [re setArgumentTable:u.cargs[s] atStages:MTLRenderStageFragment];
    [re setViewport:(MTLViewport){0, 0, (double)dst.width, (double)dst.height, 0, 1}];
    [re setRenderPipelineState:up_copy_ps(dst.pixelFormat)];
    [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [re endEncoding];
    /* Jitter phases for the scale: ceil(8 x scale^2) (the GPTK MetalFX guide), from the next frame on. */
    double k = fmax((double)dst.width / cw, (double)dst.height / ch); unsigned ph = (unsigned)ceil(8 * k * k);
    ph = ph < 8 ? 8 : ph > 72 ? 72 : ph;
    if (ph != g.phases) TLOG("temporal upscale: scale %.3f, jitter Halton (2, 3) over %u phases", k, ph);
    g.phases = ph;
    long lg = atomic_load_explicit(&g_up_log, memory_order_relaxed);
    if (lg > 0) {
        atomic_store(&g_up_log, lg - 1);
        double mx = 0; for (int i = 0; i < 16; i++) mx = fmax(mx, fabs(m[i] - (i % 5 == 0)));   // 0 when the camera did not move
        TLOG("[tup-log] frame %llu: content %lux%lu -> %lux%lu, phase %u/%u jitter %+.4f %+.4f px (to MetalFX %+.4f %+.4f, geometry %s), "
             "motion scale 1, depth reversed 0, reset %d, camera reprojection off identity by %.3g",
             (unsigned long long)g.frame, (unsigned long)cw, (unsigned long)ch, (unsigned long)dst.width, (unsigned long)dst.height,
             g.jit_i, ph, g.jx, g.jy, jx, jy, g.jit_used ? "jittered" : "not jittered", reset, mx);
    }
    memcpy(g.prev, g.cam, sizeof g.cam); g.prev_ok = 1;
    u.done++; u.cw = cw; u.ch = ch;
    g.encoded_frame = g.frame + 1;
    return 1;
}
void tup_stats(char *out, size_t n) {
    const up_t *e = u.cur >= 0 ? &u.cache[u.cur] : NULL;
    snprintf(out, n, "frame %llu: temporally upscaled %llu (%lux%lu -> %lux%lu), history resets %llu, without depth or camera %llu, "
             "waiting for a scaler %llu, declined %llu, scalers built %llu, switches between kept scalers %llu, warmed again %llu",
             (unsigned long long)g.frame, (unsigned long long)u.done, (unsigned long)u.cw, (unsigned long)u.ch, (unsigned long)(e ? e->ow : 0),
             (unsigned long)(e ? e->oh : 0), (unsigned long long)u.resets, (unsigned long long)u.missing, (unsigned long long)u.waited,
             (unsigned long long)u.declined, (unsigned long long)u.builds, (unsigned long long)u.switches, (unsigned long long)u.rewarms);
}

void taa_frame_end(void) {
    int up = tup_enabled() && u.site_seen && g.frame - u.site_frame < 120, want = taa_enabled() || up, sw = want != g.active || up != g.up;
    if (sw) { g.active = want; g.up = up; g.prev_ok = 0; u.reset_why = "switched on"; }
    g.frame++; g.depth = nil; g.cam_ok = 0; g.jit_pass = 0; g.jit_used = 0;
    /* The geometry is jittered only after a frame MetalFX took: when it cannot run (no Metal 4 temporal scaler, a scaler
       not built yet, no camera), a moved picture with nothing to smooth it would shake. */
    g.jit_ok = !sw && g.active && !g.failed && g.frame >= g.retry_at && g.encoded_frame == g.frame;   // nor right after a switch: the other pass may not run yet
    g.jit_i = g.jit_i % (g.up && g.phases ? g.phases : 16) + 1;   // Halton 2, 3 over 16 frames (upscaling: more), centred on the pixel
    g.jx = halton(g.jit_i, 2) - 0.5; g.jy = halton(g.jit_i, 3) - 0.5;
    if (g.frame % 1800 == 0 && (taa_enabled() || g.up)) { char b[300]; if (g.up) tup_stats(b, sizeof b); else taa_stats(b, sizeof b); TLOG("%s", b); }
}
void taa_stats(char *out, size_t n) {
    snprintf(out, n, "frame %llu: anti-aliased %llu, history resets %llu, without depth or camera %llu",
             (unsigned long long)g.frame, (unsigned long long)g.done, (unsigned long long)g.resets, (unsigned long long)g.missing);
}

#endif   // TARGET_OS_VISION
