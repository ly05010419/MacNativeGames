// Depth-reprojected stereo (vr_depth_stereo.h). Our own design; the ideas follow vr研究.md 2026-11-22 (the comparison of
// TotK's removed A with OcarinaVR's approach, ideas only):
//  - a backward search per output pixel along its row, from the largest shift down, so the nearest surface that lands on
//    a pixel wins and no mesh triangle is stretched across an edge (A pushed a grid forward: its edges smeared);
//  - an object's rim follows the object: each pixel's disparity is the largest within +-2 depth pixels (the game's
//    picture blends an edge over a pixel or two, the depth does not);
//  - no near clamp (A clamped at 1 m and lost the near stereo), a cap on the shift instead;
//  - a disoccluded pixel takes the farthest surface the search passed, that pixel itself (the background behind the
//    object; taking the pixel its disparity pointed back to put the object there again: "人物拉伸");
//  - per row tile the largest disparity, so far scenery searches a few pixels only.
// Zero disparity at infinity: each eye's picture is then placed straight ahead of that eye (totk_vr.m place_picture).
#import "vr_depth_stereo.h"
#include <TargetConditionals.h>
#if !TARGET_OS_OSX   /* the headset link is a Mac thing: every call says no */
void vr_ds_note_scene(id<MTLTexture> depth, float near_plane, float cam_w, float cam_h) { (void)depth; (void)near_plane; (void)cam_w; (void)cam_h; }
id<MTLTexture> vr_ds_capture_target(id<MTLTexture> *from) { if (from) *from = nil; return nil; }
void vr_ds_captured(void) {}
uint32_t vr_ds_take_captured(void) { return 0; }
int vr_ds_eyes(id<MTLCommandBuffer> cb, id<MTLTexture> pic, uint64_t seq, uint32_t dref, float tan_y, float aspect, float half, __strong id<MTLTexture> out[2]) {
    (void)cb; (void)pic; (void)seq; (void)dref; (void)tan_y; (void)aspect; (void)half; (void)out; return 0; }
void vr_ds_stats(char *out, size_t n) { if (n) out[0] = 0; }
#else
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define DLOG(...) do { fprintf(stderr, "[dm] [nvn-mtl] [vr] [depth-stereo] " __VA_ARGS__); fputc('\n', stderr); } while (0)

id<MTLResidencySet> vr_residency(void);                                         // totk_vr.m
void vr_residency_swap(id<MTLAllocation> old, id<MTLAllocation> replacement);   // totk_vr.m
int vr_dstereo(void);                                                           // totk_vr.m: option dstereo

/* RING: a depth copy must outlive the presents queued after it (nvn_mtl.m PQ, 8) and the picture copies the VR thread
   may still warp (totk_vr.m SRC_SLOTS, 5). With 8 the stand-in run vr04 lost 39 and 19 pictures in two 10 s lines to
   "depth overwritten" while presents queued (camera hook 370 frames for 335 pictures). */
enum { RING = 16, TILE = 16 };

static struct {
    // replay thread
    id<MTLTexture> noted; float noted_near; int copy_slot;   // the depth noted this frame; the ring slot being copied into + 1
    id<MTLTexture> ring[RING]; float ring_near[RING]; uint32_t ring_gen[RING]; _Atomic uint32_t ring_pub[RING]; int ring_next;
    uint32_t gen; _Atomic uint32_t captured;   // the reference of the slot captured at this present (0 none)
    // VR thread
    id<MTLRenderPipelineState> ps_disp, ps_tile; id<MTLRenderPipelineState> ps_eye[4]; MTLPixelFormat eye_fmt[4];
    id<MTLSamplerState> smp; id<MTLTexture> disp, tile; id<MTLTexture> out[2]; uint64_t out_seq;   // one pair: the VR thread shows the newest picture and waits for each frame
    int said, said_on;
    _Atomic uint64_t warped, mono_none, mono_stale, mono_size, mono_off, captures;
} d;

void vr_ds_note_scene(id<MTLTexture> depth, float near_plane, float cam_w, float cam_h) {
    if (!depth || depth.pixelFormat != MTLPixelFormatDepth32Float || depth.textureType != MTLTextureType2D) return;
    if (!(near_plane > 0) || fabsf(cam_w - (float)depth.width) >= 0.5f || fabsf(cam_h - (float)depth.height) >= 0.5f) return;
    d.noted = depth; d.noted_near = near_plane;
}

id<MTLTexture> vr_ds_capture_target(id<MTLTexture> *from) {
    if (from) *from = nil;
    d.copy_slot = 0;
    if (!d.noted) return nil;
    id<MTLTexture> src = d.noted; const float near_plane = d.noted_near; d.noted = nil;
    if (!vr_dstereo() || !vr_residency()) return nil;
    const int i = d.ring_next; d.ring_next = (d.ring_next + 1) % RING;
    id<MTLTexture> t = d.ring[i];
    if (!t || t.width != src.width || t.height != src.height) {
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:src.width height:src.height mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead; td.storageMode = MTLStorageModePrivate;
        id<MTLTexture> n = [src.device newTextureWithDescriptor:td];
        if (!n) return nil;
        n.label = [NSString stringWithFormat:@"vr depth ring %d", i];
        vr_residency_swap(t, n); d.ring[i] = n; t = n;
    }
    d.ring_near[i] = near_plane;
    d.gen = (d.gen + 1) & 0x0fffffff; if (!d.gen) d.gen = 1;
    d.ring_gen[i] = d.gen;
    atomic_store(&d.ring_pub[i], (d.gen << 4) | (uint32_t)i);   // the VR thread checks a reference against this
    atomic_store(&d.captured, 0);
    if (from) *from = src;
    d.copy_slot = i + 1;   // vr_ds_captured publishes it
    return t;
}
void vr_ds_captured(void) {
    if (d.copy_slot <= 0) return;
    const int i = d.copy_slot - 1; d.copy_slot = 0;
    atomic_store(&d.captured, (d.ring_gen[i] << 4) | (uint32_t)i);
    atomic_fetch_add(&d.captures, 1);
}
uint32_t vr_ds_take_captured(void) { return atomic_exchange(&d.captured, 0); }

// ---- VR thread ----
static const char *k_msl =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "struct DU { float2 pic; float2 dsz; float k; float x0; float x1; float maxpx; };\n"
    "struct EU { float2 pic; float2 dsz; float s; float maxpx; float x0; float x1; };\n"
    "struct VO { float4 pos [[position]]; };\n"
    // one triangle over the target
    "vertex VO ds_vs(uint i [[vertex_id]]) { float2 p = float2((i << 1) & 2, i & 2); VO o; o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0); return o; }\n"
    // the disparity in picture pixels at depth resolution, the largest within +-2 depth pixels (rims go with the object);
    // k = f * half / near, distance = near / (1 - d): disparity = f * half / distance = k * (1 - d); 0 outside the 3D view
    "fragment float ds_disp(VO i [[stage_in]], depth2d<float, access::read> dt [[texture(0)]], constant DU &u [[buffer(0)]]) {\n"
    "  int2 g = int2(i.pos.xy); int w = int(u.dsz.x); float sx = u.pic.x / u.dsz.x, m = 0.0;\n"
    "  for (int j = -2; j <= 2; j++) { int x = clamp(g.x + j, 0, w - 1); float xp = (float(x) + 0.5) * sx;\n"
    "    if (xp < u.x0 || xp > u.x1) continue;\n"
    "    m = max(m, u.k * max(1.0 - dt.read(uint2(x, g.y)), 0.0)); }\n"
    // capped here, not in the search: a nearer thing keeps the cap's shift whole (the search reaches it), where a cap
    // on the search alone cut it to pieces (tests/test_vr_depth_stereo.m)
    "  return min(m, u.maxpx); }\n"
    // the largest disparity of each TILE-pixel run of a row
    "fragment float ds_tile(VO i [[stage_in]], texture2d<float, access::read> dp [[texture(0)]]) {\n"
    "  int2 g = int2(i.pos.xy); int w = int(dp.get_width()); float m = 0.0;\n"
    "  for (int j = 0; j < 16; j++) { int x = g.x * 16 + j; if (x < w) m = max(m, dp.read(uint2(x, g.y)).r); }\n"
    "  return m; }\n"
    // one eye: s = +1 for the left eye (its picture is the source shifted right by the disparity), -1 for the right.
    // The source pixel that lands here is xs = x - s * k with its disparity k; searching k from the largest that can
    // reach this pixel down, the first such pixel is the nearest surface. None: a hole the near object uncovered, filled
    // with the farthest surface the search passed.
    "fragment float4 ds_eye(VO i [[stage_in]], texture2d<float> pic [[texture(0)]], texture2d<float, access::read> dp [[texture(1)]],\n"
    "                       texture2d<float, access::read> tl [[texture(2)]], sampler sm [[sampler(0)]], constant EU &u [[buffer(0)]]) {\n"
    "  float2 p = i.pos.xy;\n"
    "  if (p.x < u.x0 || p.x > u.x1) return pic.sample(sm, p / u.pic, level(0.0));\n"
    "  float sx = u.dsz.x / u.pic.x, sy = u.dsz.y / u.pic.y; int dw = int(u.dsz.x);\n"
    "  uint dy = uint(min(int(p.y * sy), int(u.dsz.y) - 1));\n"
    "  float lo = u.s > 0.0 ? p.x - u.maxpx : p.x, hi = u.s > 0.0 ? p.x : p.x + u.maxpx;\n"
    "  int t0 = max(int(max(lo, 0.0) * sx) / 16, 0), t1 = min(int(min(hi, u.pic.x - 1.0) * sx) / 16, int(tl.get_width()) - 1);\n"
    "  float kmax = 0.0; for (int t = t0; t <= t1; t++) kmax = max(kmax, tl.read(uint2(t, dy)).r);\n"
    "  kmax = min(kmax, u.maxpx);\n"
    "  float hit = -1.0, far = 3.4e38, far_x = p.x;\n"
    "  for (int k = int(ceil(kmax)); k >= 0; k--) {\n"
    "    float xs = p.x - u.s * float(k);\n"
    "    if (xs < 0.0 || xs >= u.pic.x) continue;\n"
    "    float v = dp.read(uint2(min(int(xs * sx), dw - 1), dy)).r;\n"
    "    if (v < far) { far = v; far_x = xs; }\n"
    "    if (fabs(v - float(k)) <= 0.5) { hit = v; break; } }\n"
    // a hole takes the background pixel itself (far_x), not the pixel its disparity points back to: that one is the
    // near object, which then grew into the hole on one side in each eye (user on the Quest: "人物拉伸"; warp_sim.py)
    "  float xs = hit >= 0.0 ? p.x - u.s * hit : far_x;\n"
    "  return pic.sample(sm, float2(xs, p.y) / u.pic, level(0.0)); }\n";

static id<MTLLibrary> g_lib;
static int setup(id<MTLDevice> dev) {
    if (d.ps_disp) return 1;
    NSError *e = nil;
    g_lib = [dev newLibraryWithSource:@(k_msl) options:[MTLCompileOptions new] error:&e];
    if (!g_lib) { DLOG("【红】shaders: %s", e.localizedDescription.UTF8String); return 0; }
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [g_lib newFunctionWithName:@"ds_vs"];
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatR16Float;
    pd.fragmentFunction = [g_lib newFunctionWithName:@"ds_disp"]; d.ps_disp = [dev newRenderPipelineStateWithDescriptor:pd error:&e];
    pd.fragmentFunction = [g_lib newFunctionWithName:@"ds_tile"]; d.ps_tile = [dev newRenderPipelineStateWithDescriptor:pd error:&e];
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new]; sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
    sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    d.smp = [dev newSamplerStateWithDescriptor:sd];
    if (!d.ps_disp || !d.ps_tile || !d.smp) { DLOG("【红】pipelines: %s", e ? e.localizedDescription.UTF8String : "?"); d.ps_disp = nil; return 0; }
    return 1;
}
static id<MTLRenderPipelineState> eye_pipeline(id<MTLDevice> dev, MTLPixelFormat f) {
    for (int i = 0; i < 4; i++) if (d.ps_eye[i] && d.eye_fmt[i] == f) return d.ps_eye[i];
    int i = 0; while (i < 3 && d.ps_eye[i]) i++;
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [g_lib newFunctionWithName:@"ds_vs"]; pd.fragmentFunction = [g_lib newFunctionWithName:@"ds_eye"];
    pd.colorAttachments[0].pixelFormat = f;
    NSError *e = nil; id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:pd error:&e];
    if (!ps) { DLOG("【红】eye pipeline for format %lu: %s", (unsigned long)f, e.localizedDescription.UTF8String); return nil; }
    d.ps_eye[i] = ps; d.eye_fmt[i] = f; return ps;
}
static id<MTLTexture> mk(id<MTLDevice> dev, MTLPixelFormat f, NSUInteger w, NSUInteger h, int mips) {
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f width:w height:h mipmapped:mips != 0];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; td.storageMode = MTLStorageModePrivate;
    return [dev newTextureWithDescriptor:td];
}
static void pass(id<MTLCommandBuffer> cb, id<MTLTexture> target, id<MTLRenderPipelineState> ps, void (^bind)(id<MTLRenderCommandEncoder>)) {
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = target; rp.colorAttachments[0].loadAction = MTLLoadActionDontCare; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
    [e setRenderPipelineState:ps]; bind(e);
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
}

int vr_ds_eyes(id<MTLCommandBuffer> cb, id<MTLTexture> pic, uint64_t seq, uint32_t dref, float tan_y, float aspect,

               float half, __strong id<MTLTexture> out[2]) {
    if (!vr_dstereo()) { atomic_fetch_add(&d.mono_off, 1); return 0; }
    if (!dref || !pic || !(half > 0) || !(tan_y > 0.1f) || !(aspect > 0.1f)) { atomic_fetch_add(&d.mono_none, 1); return 0; }
    const int ri = (int)(dref & 15); if (ri >= RING || atomic_load(&d.ring_pub[ri]) != dref) { atomic_fetch_add(&d.mono_stale, 1); return 0; }
    id<MTLTexture> dt = d.ring[ri]; const float near_plane = d.ring_near[ri];
    /* the depth covers the whole picture (OOT: the 1920x1080 canvas and its depth; the headset's MetalFX picture is twice
       as large): the same shape within 1 %, or no stereo for this picture */
    if (!dt || fabs((double)pic.width * dt.height / ((double)pic.height * dt.width) - 1) > 0.01) {
        atomic_fetch_add(&d.mono_size, 1);
        if (d.said < 3) { d.said++; DLOG("picture %lux%lu and depth %lux%lu differ in shape: one picture for both eyes", (unsigned long)pic.width, (unsigned long)pic.height, (unsigned long)(dt ? dt.width : 0), (unsigned long)(dt ? dt.height : 0)); }
        return 0;
    }
    if (d.out_seq == seq && d.out[0] && d.out[1]) { out[0] = d.out[0]; out[1] = d.out[1]; return 1; }
    id<MTLDevice> dev = cb.device;
    if (!setup(dev)) return 0;
    id<MTLRenderPipelineState> pe = eye_pipeline(dev, pic.pixelFormat);
    if (!pe) return 0;
    const NSUInteger tw = (dt.width + TILE - 1) / TILE;
    if (!d.disp || d.disp.width != dt.width || d.disp.height != dt.height) d.disp = mk(dev, MTLPixelFormatR16Float, dt.width, dt.height, 0);
    if (!d.tile || d.tile.width != tw || d.tile.height != dt.height) d.tile = mk(dev, MTLPixelFormatR16Float, tw, dt.height, 0);
    for (int k = 0; k < 2; k++) {
        id<MTLTexture> o = d.out[k];
        if (!o || o.width != pic.width || o.height != pic.height || o.pixelFormat != pic.pixelFormat) d.out[k] = mk(dev, pic.pixelFormat, pic.width, pic.height, 1);
    }
    if (!d.disp || !d.tile || !d.out[0] || !d.out[1]) return 0;
    /* f: the picture's focal length in pixels (its 3D fills the height: half the height over the half-angle tangent) */
    const float W = (float)pic.width, H = (float)pic.height, f = H * 0.5f / tan_y, w3 = H * aspect;
    const float maxpx = 64.0f * H / 1080.0f;
    struct { float pic[2], dsz[2], k, x0, x1, maxpx; } du = { { W, H }, { (float)dt.width, (float)dt.height }, f * half / near_plane, (W - w3) * 0.5f, (W + w3) * 0.5f, maxpx };
    id<MTLTexture> disp = d.disp, tile = d.tile;
    pass(cb, disp, d.ps_disp, ^(id<MTLRenderCommandEncoder> e) { [e setFragmentTexture:dt atIndex:0]; [e setFragmentBytes:&du length:sizeof du atIndex:0]; });
    pass(cb, tile, d.ps_tile, ^(id<MTLRenderCommandEncoder> e) { [e setFragmentTexture:disp atIndex:0]; });
    for (int k = 0; k < 2; k++) {
        struct { float pic[2], dsz[2], s, maxpx, x0, x1; } eu = { { W, H }, { (float)dt.width, (float)dt.height }, k == 0 ? 1.0f : -1.0f, maxpx, du.x0, du.x1 };
        id<MTLSamplerState> smp = d.smp;
        pass(cb, d.out[k], pe, ^(id<MTLRenderCommandEncoder> e) {
            [e setFragmentTexture:pic atIndex:0]; [e setFragmentTexture:disp atIndex:1]; [e setFragmentTexture:tile atIndex:2];
            [e setFragmentSamplerState:smp atIndex:0]; [e setFragmentBytes:&eu length:sizeof eu atIndex:0]; });
    }
    id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    [b generateMipmapsForTexture:d.out[0]]; [b generateMipmapsForTexture:d.out[1]];
    [b endEncoding];
    d.out_seq = seq; out[0] = d.out[0]; out[1] = d.out[1];
    atomic_fetch_add(&d.warped, 1);
    if (!d.said_on++)
        DLOG("on: each eye's picture from the scene depth (picture %lux%lu %lu, depth %lux%lu, near %g, focal %.1f px, half the eye distance %.4f, "
             "%.1f px at 1 m, at most %.0f px; the 3D view x %.0f..%.0f)", (unsigned long)pic.width, (unsigned long)pic.height, (unsigned long)pic.pixelFormat,
             (unsigned long)dt.width, (unsigned long)dt.height, near_plane, f, half, f * half, maxpx, du.x0, du.x1);
    return 1;
}

void vr_ds_stats(char *out, size_t n) {
    if (!n) return;
    snprintf(out, n, "depth stereo: %llu pictures warped, depth copies %llu; one picture for both eyes: off %llu, no depth %llu, depth overwritten %llu, shapes differ %llu",
             (unsigned long long)atomic_exchange(&d.warped, 0), (unsigned long long)atomic_exchange(&d.captures, 0),
             (unsigned long long)atomic_exchange(&d.mono_off, 0), (unsigned long long)atomic_exchange(&d.mono_none, 0),
             (unsigned long long)atomic_exchange(&d.mono_stale, 0), (unsigned long long)atomic_exchange(&d.mono_size, 0));
}
#endif
