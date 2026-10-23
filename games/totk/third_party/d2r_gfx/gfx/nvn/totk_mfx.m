// TOTK: MetalFX frame interpolation (TOTK_MFX=interp30 / interp / interpmax, or key 2 / flag mfx_interp at run time; not in
// D2R). MetalFX makes one more frame between two game frames. Modes (totk_mfx.h): the game at 30 fps ("30+30"), at every
// second display refresh ("50+50" on the user's 100 Hz display: every refresh a new picture, steadier than the uncapped
// 65-97 fps, about one refresh more latency), or uncapped with every frame interpolated. First planned for the phone
// (60 fps on a GPU about a third of the M4's).
// The replay (nvn_mtl.m) hands over per game frame:
//  - the scene before the UI: the canvas copied before the first UI draw (docked the upscaled 1600x900 scene, handheld the
//    1280x720 scene drawn in the canvas itself);
//  - the UI drawn over it: attachment 7 of the later canvas passes, premultiplied (the game's colour blend, alpha
//    accumulating coverage);
//  - the G-buffer depth (D32F, not reversed, infinite far plane) and the camera block.
// Motion vectors come from the camera: depth plus this and the previous frame's view-projection. A pixel whose depth does
// not match the previous frame's there (the previous depth around the reprojected position) is not still world: it gets
// no motion on screen (mfx_mv_dyn, 2026-10-23). Before, what moves with the camera (Link) got the background's camera
// motion and was put off his place in every interpolated frame, by the camera's step between two game frames: at 25+25
// or 30+30 twice the step of 50+50 and 25-30 times a second, so he shook while the world stayed right (Breath of the Wild
// 1.6.0). Zero motion everywhere shook the whole picture: MetalFX relies on the motion it is given. Conventions measured by the probe: motion in pixels of the motion texture, pointing to the
// previous position; the UI given apart and premultiplied (the "composited" mode returns a black frame).
#import <MetalFX/MetalFX.h>
#import <QuartzCore/QuartzCore.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "totk_mfx.h"
#include <TargetConditionals.h>

#define MLOG(...) do { fprintf(stderr, "[dm] [nvn-mtl] [mfx] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define MDIE(...) do { MLOG("FATAL " __VA_ARGS__); abort(); } while (0)

#if TARGET_OS_VISION
/* visionOS has no MetalFX frame interpolator. Keep every entry point off, including environment initialization. */
int mfx_enabled(void) { return 0; }
const char *mfx_mode_name(int mode) { return mode == MFX_OFF ? "off" : "not available on visionOS"; }
void mfx_set(int mode) { if (mode) MLOG("frame interpolation is not available on visionOS"); }
void mfx_set_mv_dyn(int on) { (void)on; }
int mfx_latch(void) { return 0; }
id<MTLTexture> mfx_scene_target(id<MTLDevice> dev, NSUInteger w, NSUInteger h, MTLPixelFormat format) { (void)dev; (void)w; (void)h; (void)format; return nil; }
id<MTLTexture> mfx_ui_target(id<MTLDevice> dev, NSUInteger w, NSUInteger h) { (void)dev; (void)w; (void)h; return nil; }
void mfx_note_scene(void) {}
void mfx_note_depth(id<MTLTexture> depth) { (void)depth; }
void mfx_note_camera(const float *cam, int n) { (void)cam; (void)n; }
id<MTLResidencySet> mfx_residency(void) { return nil; }
int mfx_frame_prepare(double frame_s) { (void)frame_s; return -1; }
NSArray *mfx_frame_encode(id<MTL4CommandBuffer> cb, int slot, id<MTL4CounterHeap> heap, NSUInteger stamp) { (void)cb; (void)slot; (void)heap; (void)stamp; return nil; }
void mfx_note_gpu(double ms) { (void)ms; }
void mfx_frame_abort(void) {}
id<MTLTexture> mfx_output(int slot) { (void)slot; return nil; }
void mfx_stats(char *out, size_t n) { snprintf(out, n, "not available on visionOS"); }
void mfx_mv_log(void) {}
int mfx_dump_due(void) { return 0; }
void mfx_dump(long frame, id<MTLTexture> prev, id<MTLTexture> cur) { (void)frame; (void)prev; (void)cur; }
#else

#define MFX_OUT 3
static struct {
    id<MTLDevice> dev;
    id<MTL4FXFrameInterpolator> fi;
    NSUInteger in_w, in_h, out_w, out_h; MTLPixelFormat scene_fmt;
    id<MTLTexture> scene[2], ui, mv, out[MFX_OUT], outv[MFX_OUT];
    id<MTLTexture> pdepth; uint64_t pdepth_frame;   // the previous interpolated frame's depth (copied after its motion), and its g.frames
    id<MTLResidencySet> rs;
    id<MTLComputePipelineState> mv_ps;
    id<MTL4ArgumentTable> mv_args[MFX_OUT];
    id<MTLBuffer> mv_par[MFX_OUT];
    id<MTLFence> fence;
    int cur;                       // scene[cur] is written by this frame
    int scene_ok, cam_ok;          // this frame
    __weak id<MTLTexture> depth;   // this frame's G-buffer depth (owned by the game's texture table)
    double cam[84], prev_cam[84];
    int prev_ok, hist_ok, slot;    // prev_ok: scene[cur ^ 1] / prev_cam hold the previous frame; hist_ok: MetalFX saw it
    double yflip;                  // clip-space y of the game: +1 = top row (TOTK_MFX_YFLIP=-1 tries the other way)
    id<MTLTexture> enc_depth;      // depth of a prepared frame, until it is encoded
    int scene_ok_was, cam_ok_was;
    double mv_prev_yaw;
    id<MTLTexture> last_depth;     // for mfx_dump
    int last_slot;
    uint64_t frames, made, skip_input, skip_first, skip_cut, skip_busy, miss[4];   // miss: scene, camera, depth, UI
    double gpu_ms[300]; int ngpu;   // GPU time of the last interpolations (MetalFX part)
    double last_prepare_s, dt;      // time of the previous game frame's prepare; this pair's interval (MetalFX deltaTime)
} g;
/* Flag mfx_mv_dyn (default on): pixels that are not still world get no screen motion instead of the camera's. */
static _Atomic int g_mv_dyn = 1;
void mfx_set_mv_dyn(int on) { atomic_store(&g_mv_dyn, on != 0); MLOG("[flag] mfx_mv_dyn = %d: %s", on != 0, on ? "moving pixels get no camera motion" : "camera motion everywhere"); }

/* Reconstructing identical MetalFX interpolators repeatedly grows device
 * allocations on the tested Mac, even after the old Objective-C object dies.
 * Keep a bounded set of exact configurations without eviction/rebuild churn.
 * In-flight frames retain their own resource payload independently of the
 * cache. Once full, new configurations use the previous uncached path. This does not establish or
 * fix every source of long-session memory growth. Diagnostic opt-out below. */
#define MFX_FI_CACHE 4
static id<MTL4FXFrameInterpolator> g_fi_cache[MFX_FI_CACHE];
static unsigned g_fi_cache_count;
static int fi_cache_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("TOTK_MFX_FI_CACHE"); on = !e || strcmp(e, "0"); }
    return on;
}
static id<MTL4FXFrameInterpolator> fi_cached(NSUInteger iw, NSUInteger ih) {
    if (!fi_cache_on()) return nil;
    for (unsigned i = 0; i < g_fi_cache_count; i++) {
        id<MTL4FXFrameInterpolator> fi = g_fi_cache[i];
        if (fi.inputWidth == iw && fi.inputHeight == ih && fi.outputWidth == g.out_w &&
            fi.outputHeight == g.out_h && fi.colorTextureFormat == g.scene_fmt) {
            MLOG("reuse cached interpolator %u: %lux%lu -> %lux%lu; reset history", i,
                 (unsigned long)iw, (unsigned long)ih, (unsigned long)g.out_w, (unsigned long)g.out_h);
            return fi;
        }
    }
    return nil;
}

/* g_on: the mode of this game frame (read by the replay, the present path and the pacing in hle_nvn.c); g_req: what the
   user asked for (key 2 / flag mfx_interp), taken over at the next present. */
static _Atomic int g_on, g_req;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
const char *mfx_mode_name(int m) {
    static const char *const names[MFX_NMODES] = {"off", "30+30 (the game at 30 fps)", "half the refresh rate (50+50 on a 100 Hz display)", "uncapped (every game frame interpolated)"};
    return m >= 0 && m < MFX_NMODES ? names[m] : "?";
}
static void mfx_init(void) {
    const char *e = getenv("TOTK_MFX"); int v;
    if (!e || !*e || !strcmp(e, "0")) v = MFX_OFF;
    else if (!strcmp(e, "interp30")) v = MFX_30;
    else if (!strcmp(e, "interp")) v = MFX_HALF;
    else if (!strcmp(e, "interpmax")) v = MFX_MAX;
    else MDIE("TOTK_MFX must be interp30, interp, interpmax or 0, got %s", e);
    atomic_store(&g_req, v); atomic_store(&g_on, v);
    if (v) MLOG("TOTK_MFX=%s: MetalFX frame interpolation, %s", e, mfx_mode_name(v));
}
int mfx_enabled(void) { pthread_once(&g_once, mfx_init); return atomic_load_explicit(&g_on, memory_order_relaxed); }
void mfx_set(int m) {
    pthread_once(&g_once, mfx_init);
    if (m >= MFX_NMODES) { MLOG("[flag] mfx_interp takes 0..%d (-1 cycles), got %d", MFX_NMODES - 1, m); return; }
    int v = m < 0 ? (atomic_load(&g_req) + 1) % MFX_NMODES : m;
    atomic_store(&g_req, v);
    MLOG("[flag] mfx_interp = %d: %s (from the next game frame)", v, mfx_mode_name(v));
}
int mfx_latch(void) {
    pthread_once(&g_once, mfx_init);
    int r = atomic_load(&g_req), was = atomic_load(&g_on);
    if (r == was) return 0;
    g.prev_ok = g.hist_ok = 0; g.scene_ok = g.cam_ok = 0; g.depth = nil; g.last_prepare_s = 0;   // the next interpolated pair starts fresh
    atomic_store(&g_on, r);
    MLOG("frame interpolation %s -> %s", mfx_mode_name(was), mfx_mode_name(r));
    return 1;
}

static id<MTLTexture> mk(MTLPixelFormat f, NSUInteger w, NSUInteger h, MTLTextureUsage u) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f width:w height:h mipmapped:NO];
    d.usage = u; d.storageMode = MTLStorageModePrivate;
    id<MTLTexture> t = [g.dev newTextureWithDescriptor:d];
    if (!t) MDIE("texture %lux%lu format %lu", (unsigned long)w, (unsigned long)h, (unsigned long)f);
    if (!g.rs) { NSError *e = nil; g.rs = [g.dev newResidencySetWithDescriptor:[MTLResidencySetDescriptor new] error:&e]; if (!g.rs) MDIE("residency set: %s", e.localizedDescription.UTF8String); }
    [g.rs addAllocation:t]; [g.rs commit];
    return t;
}
static void drop(id<MTLTexture> __strong *t) { if (*t) { [g.rs removeAllocation:*t]; *t = nil; } }

// Scene / UI / output textures follow the canvas size; the interpolator also depends on the depth size.
static void ensure_out(id<MTLDevice> dev, NSUInteger w, NSUInteger h, MTLPixelFormat fmt) {
    if (!g.dev) g.dev = dev;
    if (g.out_w == w && g.out_h == h && g.scene[0] && (!fmt || fmt == g.scene_fmt)) return;
    if (!fmt) fmt = g.scene_fmt ? g.scene_fmt : MTLPixelFormatRG11B10Float;
    for (int i = 0; i < 2; i++) drop(&g.scene[i]);
    drop(&g.ui);
    for (int i = 0; i < MFX_OUT; i++) { drop(&g.out[i]); g.outv[i] = nil; }
    MTLTextureUsage rt = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    for (int i = 0; i < 2; i++) g.scene[i] = mk(fmt, w, h, MTLTextureUsageShaderRead);   // blit copies of the canvas
    g.scene_fmt = fmt;
    g.ui = mk(MTLPixelFormatRGBA16Float, w, h, rt);
    for (int i = 0; i < MFX_OUT; i++) {
        g.out[i] = mk(MTLPixelFormatRGBA8Unorm_sRGB, w, h, MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget | MTLTextureUsagePixelFormatView);
        g.outv[i] = [g.out[i] newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm];
    }
    g.out_w = w; g.out_h = h; g.fi = nil; g.prev_ok = g.hist_ok = 0;
    MLOG("scene (format %lu) / UI / output textures %lux%lu", (unsigned long)fmt, (unsigned long)w, (unsigned long)h);
}
id<MTLTexture> mfx_scene_target(id<MTLDevice> dev, NSUInteger w, NSUInteger h, MTLPixelFormat fmt) { ensure_out(dev, w, h, fmt); return g.scene[g.cur]; }
id<MTLTexture> mfx_ui_target(id<MTLDevice> dev, NSUInteger w, NSUInteger h) { ensure_out(dev, w, h, 0); return g.ui; }
id<MTLResidencySet> mfx_residency(void) { return g.rs; }
void mfx_note_scene(void) { g.scene_ok = 1; }
void mfx_note_depth(id<MTLTexture> d) { if (d && d.pixelFormat == MTLPixelFormatDepth32Float && d.textureType == MTLTextureType2D) g.depth = d; }
void mfx_note_camera(const float *c, int n) {
    if (n < 84) return;
    for (int i = 0; i < 84; i++) g.cam[i] = c[i];
    g.cam_ok = c[56] > 0 && c[57] > c[56] && c[62] > 0 && c[70] > 0;   // near, far, aspect, vertical field of view (radians)
}

static void ensure_fi(NSUInteger iw, NSUInteger ih) {
    if (g.fi && g.in_w == iw && g.in_h == ih && g.fi.colorTextureFormat == g.scene_fmt && g.fi.outputWidth == g.out_w && g.fi.outputHeight == g.out_h) return;
    NSError *e = nil;
    g.fi = fi_cached(iw, ih);
    if (!g.fi) {
        MTLFXFrameInterpolatorDescriptor *d = [MTLFXFrameInterpolatorDescriptor new];
        d.colorTextureFormat = g.scene_fmt; d.outputTextureFormat = MTLPixelFormatRGBA8Unorm_sRGB;
        d.depthTextureFormat = MTLPixelFormatDepth32Float; d.motionTextureFormat = MTLPixelFormatRG16Float; d.uiTextureFormat = MTLPixelFormatRGBA16Float;
        d.inputWidth = iw; d.inputHeight = ih; d.outputWidth = g.out_w; d.outputHeight = g.out_h;
        id<MTL4Compiler> comp = [g.dev newCompilerWithDescriptor:[MTL4CompilerDescriptor new] error:&e];
        if (!comp) MDIE("MTL4 compiler: %s", e.localizedDescription.UTF8String);
        if (![MTLFXFrameInterpolatorDescriptor supportsMetal4FX:g.dev]) MDIE("this GPU has no Metal 4 MetalFX frame interpolation");
        g.fi = [d newFrameInterpolatorWithDevice:g.dev compiler:comp];
        if (!g.fi) MDIE("frame interpolator %lux%lu -> %lux%lu not created", (unsigned long)iw, (unsigned long)ih, (unsigned long)g.out_w, (unsigned long)g.out_h);
        if (fi_cache_on() && g_fi_cache_count < MFX_FI_CACHE) g_fi_cache[g_fi_cache_count++] = g.fi;
    }
    drop(&g.mv); g.mv = mk(MTLPixelFormatRG16Float, iw, ih, MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead);
    drop(&g.pdepth); g.pdepth = mk(MTLPixelFormatDepth32Float, iw, ih, MTLTextureUsageShaderRead); g.pdepth_frame = 0;
    if (!g.mv_ps) {
        static const char *src =
            "#include <metal_stdlib>\nusing namespace metal;\n"
            "struct P { float4x4 reproj; float2 size; float2 inv_size; float yflip; float dyn, pad1, pad2; };\n"
            // view distance from a depth value (not reversed, infinite far plane: z = near / (1 - d)), in units of near
            "static float vz(float d) { return 1.0 / max(1.0 - d, 1e-7); }\n"
            // camera motion of each depth pixel: where it was in the previous frame, in pixels of this texture. With dyn, a
            // pixel whose distance at that place in the previous frame is not what a still point would have had moved
            // itself: no motion on screen. Still: within the 3x3 range of the previous depth there (1 % margin) where that
            // depth is continuous (a slope), and within 1 % of one of the nine where it is not (an edge: the range of Link
            // and the land behind him would take in any distance between them)
            "kernel void mfx_mv(depth2d<float, access::read> d [[texture(0)]], texture2d<float, access::write> mv [[texture(1)]],\n"
            "                   depth2d<float, access::read> pd [[texture(2)]], constant P &p [[buffer(0)]], uint2 g [[thread_position_in_grid]]) {\n"
            "  if (g.x >= uint(p.size.x) || g.y >= uint(p.size.y)) return;\n"
            "  float2 uv = (float2(g) + 0.5) * p.inv_size;\n"
            "  float d0 = d.read(g);\n"
            "  float4 c = p.reproj * float4(uv.x * 2.0 - 1.0, p.yflip * (1.0 - uv.y * 2.0), d0 * 2.0 - 1.0, 1.0);\n"
            "  float2 m = 0.0;\n"
            "  if (c.w > 1e-7) {\n"
            "    float2 n = c.xy / c.w, prev = float2(n.x * 0.5 + 0.5, 0.5 - 0.5 * p.yflip * n.y);\n"
            "    m = (prev - uv) * p.size;\n"
            "    int2 q = int2(floor(prev * p.size));\n"
            "    if (p.dyn > 0.5 && d0 < 1.0 && all(q >= 1) && all(q < int2(p.size) - 1)) {\n"
            "      float ze = vz(c.z / c.w * 0.5 + 0.5), zmin = 3.4e38, zmax = 0.0; bool near = false;\n"
            "      for (int y = -1; y <= 1; y++) for (int x = -1; x <= 1; x++) {\n"
            "        float z = vz(pd.read(uint2(q + int2(x, y)))); zmin = min(zmin, z); zmax = max(zmax, z); near = near || fabs(z - ze) <= ze * 0.01; }\n"
            "      bool still = zmax <= zmin * 1.05 ? (ze >= zmin * 0.99 && ze <= zmax * 1.01) : near;\n"
            "      if (!still) m = 0.0;\n"
            "    }\n"
            "  }\n"
            "  if (any(isnan(m)) || any(isinf(m))) m = 0.0;\n"
            "  mv.write(float4(m, 0.0, 0.0), g);\n"
            "}\n";
        MTLCompileOptions *o = [MTLCompileOptions new]; o.mathMode = MTLMathModeSafe;
        id<MTLLibrary> lib = [g.dev newLibraryWithSource:@(src) options:o error:&e];
        id<MTLFunction> fn = lib ? [lib newFunctionWithName:@"mfx_mv"] : nil;
        g.mv_ps = fn ? [g.dev newComputePipelineStateWithFunction:fn error:&e] : nil;
        if (!g.mv_ps) MDIE("motion vector kernel: %s", e.localizedDescription.UTF8String);
        MTL4ArgumentTableDescriptor *ad = [MTL4ArgumentTableDescriptor new]; ad.maxBufferBindCount = 1; ad.maxTextureBindCount = 3;
        for (int i = 0; i < MFX_OUT; i++) {
            g.mv_args[i] = [g.dev newArgumentTableWithDescriptor:ad error:&e]; if (!g.mv_args[i]) MDIE("argument table: %s", e.localizedDescription.UTF8String);
            g.mv_par[i] = [g.dev newBufferWithLength:256 options:MTLResourceStorageModeShared]; [g.rs addAllocation:g.mv_par[i]];
        }
        [g.rs commit];
        g.fence = [g.dev newFence];
        const char *yf = getenv("TOTK_MFX_YFLIP"); g.yflip = yf ? atof(yf) : 1.0;   // diagnostic: -1 tries the other clip-space y convention
    }
    g.in_w = iw; g.in_h = ih; g.prev_ok = g.hist_ok = 0;
    MLOG("frame interpolator: depth / motion %lux%lu -> %lux%lu (scene format %lu, output RGBA8 sRGB, UI RGBA16F premultiplied)", (unsigned long)iw, (unsigned long)ih, (unsigned long)g.out_w, (unsigned long)g.out_h, (unsigned long)g.scene_fmt);
}

// 4x4 inverse (Gauss-Jordan, partial pivoting) and product, row-major, in double: the view-projection matrices hold
// world coordinates in the thousands, and only the combined reprojection goes to the GPU in float.
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
// A camera jump (teleport, cutscene cut) or a turn of more than 25 degrees in one game frame is not interpolated.
static int camera_cut(void) {
    double dx = g.cam[47] - g.prev_cam[47], dy = g.cam[51] - g.prev_cam[51], dz = g.cam[55] - g.prev_cam[55];
    double f = g.cam[8] * g.prev_cam[8] + g.cam[9] * g.prev_cam[9] + g.cam[10] * g.prev_cam[10];   // view z axes
    return sqrt(dx * dx + dy * dy + dz * dz) > 8.0 || f < cos(25.0 * M_PI / 180.0);
}

int mfx_frame_prepare(double frame_s) {
    g.frames++;
    id<MTLTexture> depth = g.depth;
    int ok = g.scene_ok && g.cam_ok && depth && g.ui;
    /* MetalFX deltaTime: the real interval between the two game frames (20 ms at 100 Hz when the game keeps up), else the
       pacing period. The pacing itself is held at two display refreshes by present_ticks (hle_nvn.c). */
    double now = CACurrentMediaTime(), dt = g.last_prepare_s > 0 ? now - g.last_prepare_s : frame_s;
    g.last_prepare_s = now; g.dt = dt > 1.0 / 250 && dt < 0.1 ? dt : frame_s > 0 ? frame_s : 1.0 / 30;
    g.scene_ok_was = g.scene_ok; g.cam_ok_was = g.cam_ok; g.scene_ok = g.cam_ok = 0; g.depth = nil;
    if (!ok) {
        g.skip_input++; g.miss[0] += !g.scene_ok_was; g.miss[1] += !g.cam_ok_was; g.miss[2] += !depth; g.miss[3] += !g.ui;
        g.prev_ok = g.hist_ok = 0; return -1;
    }
    ensure_fi(depth.width, depth.height);
    if (!g.prev_ok || camera_cut()) {
        if (g.prev_ok) g.skip_cut++; else g.skip_first++;
        memcpy(g.prev_cam, g.cam, sizeof g.cam); g.cur ^= 1; g.prev_ok = 1; g.hist_ok = 0;
        return -1;
    }
    double vc[16], vp[16], ic[16], m[16];
    for (int i = 0; i < 16; i++) { vc[i] = g.cam[12 + i]; vp[i] = g.prev_cam[12 + i]; }
    if (!inv4(vc, ic)) { g.skip_input++; g.prev_ok = g.hist_ok = 0; return -1; }
    mul4(vp, ic, m);
    int s = g.slot = (g.slot + 1) % MFX_OUT;
    float *par = g.mv_par[s].contents;
    for (int c = 0; c < 4; c++) for (int r = 0; r < 4; r++) par[c * 4 + r] = (float)m[r * 4 + c];   // float4x4 is column-major
    par[16] = (float)depth.width; par[17] = (float)depth.height; par[18] = 1.0f / depth.width; par[19] = 1.0f / depth.height; par[20] = (float)g.yflip;
    par[21] = atomic_load(&g_mv_dyn) && g.pdepth_frame && g.pdepth_frame == g.frames - 1 ? 1.0f : 0.0f;   // the previous game frame's depth is there
    g.enc_depth = depth;
    return s;
}
void mfx_frame_abort(void) { g.enc_depth = nil; g.prev_ok = g.hist_ok = 0; g.skip_busy++; }
NSArray *mfx_frame_encode(id<MTL4CommandBuffer> cb, int s, id<MTL4CounterHeap> heap, NSUInteger stamp) {
    id<MTLTexture> depth = g.enc_depth; g.enc_depth = nil; g.last_depth = depth; g.last_slot = s;
    [g.mv_args[s] setTexture:depth.gpuResourceID atIndex:0]; [g.mv_args[s] setTexture:g.mv.gpuResourceID atIndex:1]; [g.mv_args[s] setAddress:g.mv_par[s].gpuAddress atIndex:0];
    [g.mv_args[s] setTexture:g.pdepth.gpuResourceID atIndex:2];
    id<MTL4ComputeCommandEncoder> e = [cb computeCommandEncoder];
    [e barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageDispatch | MTLStageBlit visibilityOptions:MTL4VisibilityOptionDevice];   // the game's frame is done
    [e setComputePipelineState:g.mv_ps]; [e setArgumentTable:g.mv_args[s]];
    [e dispatchThreads:MTLSizeMake(depth.width, depth.height, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
    [e updateFence:g.fence afterEncoderStages:MTLStageDispatch];
    // this frame's depth for the next one's still-world test, once the motion has read the previous one
    [e barrierAfterEncoderStages:MTLStageDispatch beforeEncoderStages:MTLStageBlit visibilityOptions:MTL4VisibilityOptionDevice];
    [e copyFromTexture:depth toTexture:g.pdepth];
    [e endEncoding];
    g.pdepth_frame = g.frames;
    if (heap) [cb writeTimestampIntoHeap:heap atIndex:stamp];   // after the motion vectors (which waited for the frame)
    id<MTL4FXFrameInterpolator> fi = g.fi;
    fi.colorTexture = g.scene[g.cur]; fi.prevColorTexture = g.scene[g.cur ^ 1]; fi.depthTexture = depth; fi.motionTexture = g.mv;
    fi.uiTexture = g.ui; fi.uiTextureComposited = NO; fi.outputTexture = g.out[s]; fi.fence = g.fence;
    fi.motionVectorScaleX = 1; fi.motionVectorScaleY = 1; fi.jitterOffsetX = 0; fi.jitterOffsetY = 0;
    fi.deltaTime = (float)g.dt; fi.nearPlane = (float)g.cam[56]; fi.farPlane = (float)g.cam[57];
    fi.fieldOfView = (float)(g.cam[70] * 180.0 / M_PI); fi.aspectRatio = (float)g.cam[62]; fi.depthReversed = NO;
    fi.shouldResetHistory = !g.hist_ok;
    [fi encodeToCommandBuffer:cb];
    // Metal 4 work needs explicit resource lifetime. Hold this exact frame's
    // bindings through completion, while the configuration cache retains only
    // the interpolator, not old scene/pool textures between configurations.
    NSArray *resources = @[fi, fi.colorTexture, fi.prevColorTexture, depth,
                           g.mv, g.ui, g.out[s], g.mv_par[s], g.pdepth];
    fi.colorTexture = nil; fi.prevColorTexture = nil; fi.depthTexture = nil;
    fi.motionTexture = nil; fi.uiTexture = nil; fi.outputTexture = nil;
    if (heap) [cb writeTimestampIntoHeap:heap atIndex:stamp + 1];
    g.mv_prev_yaw = atan2(g.prev_cam[2], g.prev_cam[0]) * 180 / M_PI;   // yaw of the previous frame, for mfx_mv_log
    memcpy(g.prev_cam, g.cam, sizeof g.cam); g.cur ^= 1; g.hist_ok = 1; g.made++;
    return resources;
}
/* TOTK_MFX_MVLOG=<every n frames>: read the motion texture back after the interpolation and log its median and the
   camera step (diagnostic; waits for the GPU). */
void mfx_mv_log(void) {
    static int every = -1; if (every < 0) { const char *e = getenv("TOTK_MFX_MVLOG"); every = e ? atoi(e) : 0; }
    if (every <= 0 || !g.mv || g.made % (uint64_t)every) return;
    static id<MTLCommandQueue> q; if (!q) q = [g.dev newCommandQueue];
    NSUInteger w = g.mv.width, h = g.mv.height;
    id<MTLBuffer> b = [g.dev newBufferWithLength:w * h * 4 options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
    [e copyFromTexture:g.mv sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1) toBuffer:b destinationOffset:0 destinationBytesPerRow:w * 4 destinationBytesPerImage:w * h * 4];
    [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
    const _Float16 *v = b.contents; enum { N = 4096 }; static float xs[N], ys[N]; int n = 0;
    for (NSUInteger y = h / 8; y < h * 7 / 8 && n < N; y += h / 64) for (NSUInteger x = w / 8; x < w * 7 / 8 && n < N; x += w / 64) { xs[n] = (float)v[(y * w + x) * 2]; ys[n] = (float)v[(y * w + x) * 2 + 1]; n++; }
    for (int i = 1; i < n; i++) { float t = xs[i]; int j = i - 1; while (j >= 0 && xs[j] > t) { xs[j + 1] = xs[j]; j--; } xs[j + 1] = t; }
    for (int i = 1; i < n; i++) { float t = ys[i]; int j = i - 1; while (j >= 0 && ys[j] > t) { ys[j + 1] = ys[j]; j--; } ys[j + 1] = t; }
    double yaw = atan2(g.cam[2], g.cam[0]) * 180 / M_PI, pyaw = atan2(g.prev_cam[2], g.prev_cam[0]) * 180 / M_PI;   // prev_cam is this frame's camera after encode
    MLOG("motion (input px, median of %d samples): x %.2f (p10 %.2f p90 %.2f) y %.2f; camera yaw %.3f deg, step to previous %.3f deg; position %.2f %.2f %.2f",
         n, xs[n / 2], xs[n / 10], xs[n * 9 / 10], ys[n / 2], yaw, yaw - g.mv_prev_yaw, g.cam[47], g.cam[51], g.cam[55]);
    (void)pyaw;
}
/* TOTK_MFX_DUMP=<dir>: while the camera turns (more than 1 degree between the two frames), save the previous and this
   frame's scene, the depth, the motion vectors and the interpolated output of the 30th, 60th and 90th such frame as raw
   files (scene RGBA16F, depth float32, motion 2 x float16, output RGBA8 sRGB), with a line in the log. Diagnostic: it
   waits for the GPU and is meant for unpaced runs. */
static void dump_tex(id<MTLCommandQueue> q, id<MTLTexture> t, NSUInteger bpp, const char *dir, const char *name, uint64_t n) {
    NSUInteger w = t.width, h = t.height;
    id<MTLBuffer> b = [g.dev newBufferWithLength:w * h * bpp options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
    [e copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1) toBuffer:b destinationOffset:0 destinationBytesPerRow:w * bpp destinationBytesPerImage:w * h * bpp];
    [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
    char path[1024]; snprintf(path, sizeof path, "%s/mfx-%llu-%s-%lux%lu.bin", dir, (unsigned long long)n, name, (unsigned long)w, (unsigned long)h);
    FILE *f = fopen(path, "wb"); if (f) { fwrite(b.contents, 1, w * h * bpp, f); fclose(f); } else MLOG("【红】dump: cannot write %s", path);
}
static const char *g_dump_dir; static uint64_t g_turning; static double g_dump_step;
int mfx_dump_due(void) {   // called after each encode; 1 = dump this frame (the caller first waits for the GPU)
    static int init; if (!init) { init = 1; g_dump_dir = getenv("TOTK_MFX_DUMP"); }
    if (!g_dump_dir || !g.last_depth || !g.mv || g.last_slot < 0) return 0;
    g_dump_step = fabs(remainder(atan2(g.prev_cam[2], g.prev_cam[0]) * 180 / M_PI - g.mv_prev_yaw, 360.0));
    if (g_dump_step < 1.0) return 0;
    g_turning++;
    return 1;
}
/* After the GPU has finished: compare the centre of the previous and this frame's scene; a very different pair (or the
   30th turning frame as a reference) is dumped with the presented images of both frames. Returns 1 when dumped. */
static float uf11(uint32_t v) { uint32_t e = (v >> 6) & 31, m = v & 63; return e == 0 ? ldexpf(m / 64.0f, -14) : e == 31 ? 65504.0f : ldexpf(1 + m / 64.0f, (int)e - 15); }
static float uf10(uint32_t v) { uint32_t e = (v >> 5) & 31, m = v & 31; return e == 0 ? ldexpf(m / 32.0f, -14) : e == 31 ? 65504.0f : ldexpf(1 + m / 32.0f, (int)e - 15); }
static void scene_rgb(const void *base, NSUInteger i, float c[3]) {   // one pixel of a scene copy (RG11B10F or RGBA16F)
    if (g.scene_fmt == MTLPixelFormatRGBA16Float) { const _Float16 *p = (const _Float16 *)base + i * 4; for (int k = 0; k < 3; k++) c[k] = (float)p[k]; return; }
    uint32_t v = ((const uint32_t *)base)[i]; c[0] = uf11(v & 0x7ff); c[1] = uf11((v >> 11) & 0x7ff); c[2] = uf10(v >> 22);
}
static double centre_diff(id<MTLCommandQueue> q) {
    NSUInteger w = 320, h = 180, x0 = (g.out_w - w) / 2, y0 = (g.out_h - h) / 2, bpp = g.scene_fmt == MTLPixelFormatRGBA16Float ? 8 : 4;
    id<MTLBuffer> b = [g.dev newBufferWithLength:w * h * bpp * 2 options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
    for (int k = 0; k < 2; k++) [e copyFromTexture:g.scene[g.cur ^ k] sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(x0, y0, 0) sourceSize:MTLSizeMake(w, h, 1) toBuffer:b destinationOffset:k * w * h * bpp destinationBytesPerRow:w * bpp destinationBytesPerImage:w * h * bpp];
    [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
    const uint8_t *p = b.contents; double se = 0;
    for (NSUInteger i = 0; i < w * h; i++) { float a[3], c[3]; scene_rgb(p, i, a); scene_rgb(p + w * h * bpp, i, c);
        for (int k = 0; k < 3; k++) { double d = fmin(fmax(a[k], 0), 4) - fmin(fmax(c[k], 0), 4); se += d * d; } }
    return se / (w * h * 3);
}
void mfx_dump(long frame, id<MTLTexture> swap_prev, id<MTLTexture> swap_cur) {
    const char *dir = g_dump_dir; uint64_t turning = g_turning; double step = g_dump_step;
    static id<MTLCommandQueue> q; if (!q) q = [g.dev newCommandQueue];
    // every 10th turning frame from the 10th to the 160th (sixteen samples); very different pairs are logged and kept too
    double mse = centre_diff(q);
    if (turning % 10 || turning < 10 || turning > 160) return;
    MLOG("frame pair %llu (game frame %ld): centre mean squared difference %.4f%s", (unsigned long long)turning, frame, mse, mse > 0.05 ? " -- very different" : "");
    if (swap_prev) dump_tex(q, swap_prev, 4, dir, "swapprev", turning);
    if (swap_cur) dump_tex(q, swap_cur, 4, dir, "swapcur", turning);
    // after encode, cur was flipped: scene[cur ^ 1] is this frame, scene[cur] the previous one
    NSUInteger sb = g.scene_fmt == MTLPixelFormatRGBA16Float ? 8 : 4;   // RG11B10F canvas copies: 4 bytes
    dump_tex(q, g.scene[g.cur], sb, dir, "prev", turning); dump_tex(q, g.scene[g.cur ^ 1], sb, dir, "cur", turning);
    dump_tex(q, g.last_depth, 4, dir, "depth", turning); dump_tex(q, g.mv, 4, dir, "motion", turning); dump_tex(q, g.out[g.last_slot], 4, dir, "out", turning);
    MLOG("dump %llu (game frame %ld): yaw step %.3f deg, camera %.3f %.3f %.3f -> %s", (unsigned long long)turning, frame, step, g.cam[47], g.cam[51], g.cam[55], dir);
}
id<MTLTexture> mfx_output(int slot) { return slot >= 0 && slot < MFX_OUT ? g.outv[slot] : nil; }
void mfx_note_gpu(double ms) { if (ms > 0 && ms < 100) g.gpu_ms[g.ngpu++ % 300] = ms; }
static double gpu_pct(double q) {
    int n = g.ngpu < 300 ? g.ngpu : 300; if (!n) return -1;
    double a[300]; memcpy(a, g.gpu_ms, sizeof a);
    for (int i = 1; i < n; i++) { double v = a[i]; int j = i - 1; while (j >= 0 && a[j] > v) { a[j + 1] = a[j]; j--; } a[j + 1] = v; }
    return a[(int)(q * (n - 1))];
}
void mfx_stats(char *out, size_t n) {
    snprintf(out, n, "插值帧 %llu / 游戏帧 %llu（MetalFX GPU p50 %.2f / p95 %.2f ms；两帧间隔 %.1f ms）；未插：缺输入 %llu（画面 %llu 相机 %llu 深度 %llu UI %llu）、首帧 / 历史重置 %llu、镜头切换 %llu、命令缓冲忙 %llu",
             (unsigned long long)g.made, (unsigned long long)g.frames, gpu_pct(0.5), gpu_pct(0.95), g.dt * 1e3, (unsigned long long)g.skip_input, (unsigned long long)g.miss[0], (unsigned long long)g.miss[1],
             (unsigned long long)g.miss[2], (unsigned long long)g.miss[3],
             (unsigned long long)g.skip_first, (unsigned long long)g.skip_cut, (unsigned long long)g.skip_busy);
}

#endif  // TARGET_OS_VISION
