// TOTK: MetalFX spatial upscaling (TOTK_MFX_UPSCALE; not in D2R).
// MFXUP_SCENE (iOS, handheld): with a scene render size below the canvas (TOTK_RENDER_RES=960x540 on the phone, canvas
// 1280x720) the game's compose shader (0ac9a549: tone mapping, bloom, into the canvas) samples the scene bilinearly. The
// replay (nvn_mtl.m, mfxup_draw) runs a MetalFX spatial upscale of the scene to the canvas size just before that draw and
// binds the result instead; the shader samples with normalised coordinates, so it reads the same picture at the canvas
// resolution. The scene there is linear HDR (tone mapping happens in that shader), hence the HDR colour processing mode.
// MFXUP_DOCKED (Mac, docked): tone mapping and FXAA run at 1600x900, then the game's own upscale a3a570eb (FSR 1-style,
// twelve texel reads) writes the 1080p canvas. The replay lets MetalFX write the canvas instead and skips that draw
// (perceptual mode: the canvas holds display values, copied to the swapchain unchanged).
// MFXUP_OUTPUT (Mac, the present; 2026-10-16 night, the user's "2K output with MetalFX"): when the window or the full
// screen shows the 1920x1080 picture bigger (2560x1440 full screen on the user's 3440x1440 display), the present upscales
// it with MetalFX (perceptual: display values) instead of stretching it bilinearly; the FPS / load boxes are drawn after.
// Each context belongs to one thread (the replay thread; the output context the present thread); the device-wide
// objects and the residency set are shared.
#import <MetalFX/MetalFX.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "totk_mfx_up.h"

#define ULOG(...) do { fprintf(stderr, "[dm] [nvn-mtl] [mfx-up] " __VA_ARGS__); fputc('\n', stderr); } while (0)

enum { MFXUP_OLD = 8, MFXUP_OLD_AFTER = 8, MFXUP_CACHE = 3, MFXUP_BAD = 4, MFXUP_RETRY_MS = 10000 };   // replaced objects kept for this many later encodes (the GPU is done with them by then)
/* One scaler configuration. Building a scaler takes about 70 ms (2026-10-17), so a new one is built on a background queue
   while the caller keeps its fallback (the game's own upscale, the bilinear present) for those frames, and the last few
   are kept: switching back (key 3, a window going in and out of full screen) is instant. */
typedef struct { NSUInteger in_w, in_h, out_w, out_h; MTLPixelFormat in_fmt, out_fmt; int mode, own; } cfg_t;   // out_fmt: dst's (own output: invalid)
typedef struct { cfg_t cfg; id<MTL4FXSpatialScaler> sc; id<MTLTexture> out; uint64_t used; } built_t;
typedef struct {
    built_t cache[MFXUP_CACHE]; int ncache, cur;   // cur: the cache entry in use, -1 none yet
    struct { cfg_t cfg; uint64_t at_ms; } bad[MFXUP_BAD]; int nbad;   // builds that failed: the caller's own path stays, retried after MFXUP_RETRY_MS
    _Atomic int building;                           // a background build in flight; its result waits in ready
    built_t ready; int has_ready, ready_ok;         // under g_build_mu
    id<MTLFence> fence;
    uint64_t done, refused, waited, tick;
    struct { id obj; id<MTLTexture> tex; uint64_t at; } old[MFXUP_OLD]; int nold;
} ctx_t;
static ctx_t g_c[MFXUP_NCTX] = {{.cur = -1}, {.cur = -1}, {.cur = -1}};
static const char *g_name[MFXUP_NCTX] = {"scene", "docked", "output"};
static id<MTLDevice> g_dev;
static id<MTL4Compiler> g_comp;
static id<MTLResidencySet> g_rs;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;         // g_dev / g_comp / g_rs
static pthread_mutex_t g_build_mu = PTHREAD_MUTEX_INITIALIZER;   // the contexts' ready slots
static dispatch_queue_t g_build_q;                               // background scaler builds, one at a time
static _Atomic int g_on = -1;
static _Atomic uint64_t g_completed[MFXUP_NCTX];
uint64_t mfxup_completed(int ctx){return ctx>=0&&ctx<MFXUP_NCTX?atomic_load(&g_completed[ctx]):0;}
static int g_unavailable;                                        // no MetalFX here (device, residency set, compiler): never retried

int mfxup_enabled(void) {
    int v = atomic_load_explicit(&g_on, memory_order_relaxed);
    if (v < 0) {
        const char *e = getenv("TOTK_MFX_UPSCALE");
        v = e ? strcmp(e, "0") != 0 : 0;
        atomic_store(&g_on, v);
        ULOG("MetalFX spatial upscaling (TOTK_MFX_UPSCALE): %s", v ? "on" : "off");
    }
    return v;
}
void mfxup_set(int on) { atomic_store(&g_on, on != 0); ULOG("[flag] mfx_upscale = %d", on != 0); }
static _Atomic int g_out_on = -1;
int mfxup_output_enabled(void) {
    int v = atomic_load_explicit(&g_out_on, memory_order_relaxed);
    if (v < 0) { const char *e = getenv("TOTK_MFX_OUTPUT"); v = e ? strcmp(e, "0") != 0 : 0; atomic_store(&g_out_on, v); ULOG("MetalFX output upscaling (TOTK_MFX_OUTPUT, the window output stage): %s", v ? "on" : "off"); }
    return v;
}
void mfxup_output_set(int on) { atomic_store(&g_out_on, on != 0); ULOG("[flag] mfx_output = %d", on != 0); }

id<MTLResidencySet> mfxup_residency(void) { pthread_mutex_lock(&g_mu); id<MTLResidencySet> r = g_rs; pthread_mutex_unlock(&g_mu); return r; }

static void rs_change(id<MTLTexture> add, id<MTLTexture> remove) {
    if (!add && !remove) return;
    pthread_mutex_lock(&g_mu);
    if (g_rs) { if (add) [g_rs addAllocation:add]; if (remove) [g_rs removeAllocation:remove]; [g_rs commit]; }   // committed before the GPU can use it (lesson 55)
    pthread_mutex_unlock(&g_mu);
}
static void old_push(ctx_t *c, id obj, id<MTLTexture> tex);
static void old_pop(ctx_t *c) {   // drop the oldest replaced scaler / output
    rs_change(nil, c->old[0].tex);
    c->old[0].obj = nil; c->old[0].tex = nil;
    for (int i = 1; i < c->nold; i++) c->old[i - 1] = c->old[i];
    c->old[c->nold - 1].obj = nil; c->old[c->nold - 1].tex = nil; c->nold--;
}
static void old_push(ctx_t *c, id obj, id<MTLTexture> tex) {   // encoded work may still use it: keep it a while
    if (!obj && !tex) return;
    if (c->nold == MFXUP_OLD) old_pop(c);
    c->old[c->nold].obj = obj; c->old[c->nold].tex = tex; c->old[c->nold].at = c->done + MFXUP_OLD_AFTER; c->nold++;
}
static void sweep(ctx_t *c) { while (c->nold && c->old[0].at <= c->done) old_pop(c); }
static int cfg_eq(const cfg_t *a, const cfg_t *b) {
    return a->in_w == b->in_w && a->in_h == b->in_h && a->out_w == b->out_w && a->out_h == b->out_h && a->in_fmt == b->in_fmt &&
           a->out_fmt == b->out_fmt && a->mode == b->mode && a->own == b->own;
}
static int shared_objects(id<MTLDevice> dev) {
    pthread_mutex_lock(&g_mu);
    int ok = 1; NSError *e = nil;
    if (!g_dev) g_dev = dev;
    if (g_dev != dev) { ULOG("textures of another device: game's own upscale stays"); ok = 0; }
    if (ok && !g_rs) { g_rs = [g_dev newResidencySetWithDescriptor:[MTLResidencySetDescriptor new] error:&e]; if (!g_rs) { ULOG("residency set: %s", e.localizedDescription.UTF8String); ok = 0; } }
    if (ok && ![MTLFXSpatialScalerDescriptor supportsMetal4FX:g_dev]) { ULOG("this GPU has no Metal 4 MetalFX spatial scaler: game's own upscale stays"); ok = 0; }
    if (ok && !g_comp) { g_comp = [g_dev newCompilerWithDescriptor:[MTL4CompilerDescriptor new] error:&e]; if (!g_comp) { ULOG("MTL4 compiler: %s", e.localizedDescription.UTF8String); ok = 0; } }
    if (ok && !g_build_q) g_build_q = dispatch_queue_create("totk.mfxup.build", DISPATCH_QUEUE_SERIAL);
    pthread_mutex_unlock(&g_mu);
    return ok;
}
static uint64_t now_ms(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000; }
/* A failed build (e.g. no memory for the output texture on iOS) is retried after a while; a full list drops its oldest entry. */
static void mark_bad(ctx_t *c, cfg_t cfg) {
    int slot = -1;
    for (int i = 0; i < c->nbad; i++) if (cfg_eq(&c->bad[i].cfg, &cfg)) slot = i;
    if (slot < 0 && c->nbad < MFXUP_BAD) slot = c->nbad++;
    if (slot < 0) { slot = 0; for (int i = 1; i < MFXUP_BAD; i++) if (c->bad[i].at_ms < c->bad[slot].at_ms) slot = i; }
    c->bad[slot].cfg = cfg; c->bad[slot].at_ms = now_ms();
}
static int is_bad(const ctx_t *c, const cfg_t *cfg) {
    for (int i = 0; i < c->nbad; i++) if (cfg_eq(&c->bad[i].cfg, cfg)) return now_ms() - c->bad[i].at_ms < MFXUP_RETRY_MS;
    return 0;
}
/* Background queue: build the scaler (and the own output texture) for one configuration, hand it to the owning thread. */
static void build(int ci, cfg_t cfg, NSUInteger src_usage, NSUInteger dst_usage, int dst_memoryless) {
    uint64_t t0 = now_ms(); built_t b = {.cfg = cfg}; const char *why = NULL;
    /* Own output: the source's format when MetalFX takes it, else RGBA16Float (the game only samples it). Into dst: its format. */
    MTLPixelFormat outs[2] = {cfg.own ? cfg.in_fmt : cfg.out_fmt, MTLPixelFormatRGBA16Float};
    for (int i = 0; i < (cfg.own ? 2 : 1) && !b.sc; i++) {
        MTLFXSpatialScalerDescriptor *d = [MTLFXSpatialScalerDescriptor new];
        d.colorTextureFormat = cfg.in_fmt; d.outputTextureFormat = outs[i];
        d.inputWidth = cfg.in_w; d.inputHeight = cfg.in_h; d.outputWidth = cfg.out_w; d.outputHeight = cfg.out_h;
        d.colorProcessingMode = (MTLFXSpatialScalerColorProcessingMode)cfg.mode;
        b.sc = [d newSpatialScalerWithDevice:g_dev compiler:g_comp];
    }
    if (!b.sc) why = "spatial scaler not created";
    else if ((src_usage & b.sc.colorTextureUsage) != b.sc.colorTextureUsage) why = "the source's usage lacks what MetalFX needs";
    else if (!cfg.own && ((dst_usage & b.sc.outputTextureUsage) != b.sc.outputTextureUsage || dst_memoryless)) why = "the target's usage lacks what MetalFX needs";
    else if (cfg.own) {
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:b.sc.outputTextureFormat width:cfg.out_w height:cfg.out_h mipmapped:NO];
        td.usage = b.sc.outputTextureUsage | MTLTextureUsageShaderRead; td.storageMode = MTLStorageModePrivate;
        b.out = [g_dev newTextureWithDescriptor:td];
        if (!b.out) why = "output texture not created"; else rs_change(b.out, nil);
    }
    static const char *modes[] = {"perceptual", "linear", "HDR"};
    if (why) ULOG("%s: %s for %lux%lu (format %lu) -> %lux%lu: the caller's own path stays", g_name[ci], why,
                  (unsigned long)cfg.in_w, (unsigned long)cfg.in_h, (unsigned long)cfg.in_fmt, (unsigned long)cfg.out_w, (unsigned long)cfg.out_h);
    else ULOG("%s: spatial scaler %lux%lu (format %lu) -> %lux%lu (format %lu), %s colour processing, %s; built in the background in %llu ms", g_name[ci],
              (unsigned long)cfg.in_w, (unsigned long)cfg.in_h, (unsigned long)cfg.in_fmt, (unsigned long)cfg.out_w, (unsigned long)cfg.out_h,
              (unsigned long)(cfg.own ? b.out.pixelFormat : cfg.out_fmt), cfg.mode >= 0 && cfg.mode < 3 ? modes[cfg.mode] : "?",
              cfg.own ? "own output" : "into the game's target", (unsigned long long)(now_ms() - t0));
    if (why) { b.sc = nil; b.out = nil; }
    pthread_mutex_lock(&g_build_mu); g_c[ci].ready = b; g_c[ci].ready_ok = !why; g_c[ci].has_ready = 1; pthread_mutex_unlock(&g_build_mu);
    atomic_store(&g_c[ci].building, 0);
}
/* Owning thread: take a finished background build into the cache (the least recently used entry other than the current
   one makes room; its objects wait in the old list until the GPU is done with them). */
static void adopt(ctx_t *c) {
    built_t b; int has, ok;
    pthread_mutex_lock(&g_build_mu); has = c->has_ready; ok = c->ready_ok; b = c->ready; c->ready = (built_t){0}; c->has_ready = 0; pthread_mutex_unlock(&g_build_mu);
    if (!has) return;
    if (!ok) { mark_bad(c, b.cfg); return; }
    int slot = c->ncache;
    if (slot == MFXUP_CACHE) {
        slot = -1;
        for (int i = 0; i < MFXUP_CACHE; i++) if (i != c->cur && (slot < 0 || c->cache[i].used < c->cache[slot].used)) slot = i;
        old_push(c, c->cache[slot].sc, c->cache[slot].out);
    } else c->ncache++;
    c->cache[slot] = b; c->cache[slot].used = 0;
}
/* Takes a finished build, then makes the cached scaler for this configuration current if there is one. */
static int use_cached(int ci, ctx_t *c, const cfg_t *want) {
    adopt(c);
    for (int i = 0; i < c->ncache; i++) if (cfg_eq(&c->cache[i].cfg, want)) {
        if (c->cur >= 0) ULOG("%s: %s %lux%lu -> %lux%lu", g_name[ci], c->cache[i].used ? "back to the kept scaler" : "switched to the scaler just built",
                              (unsigned long)want->in_w, (unsigned long)want->in_h, (unsigned long)want->out_w, (unsigned long)want->out_h);
        c->cur = i; c->cache[i].used = ++c->tick; return 1;
    }
    return 0;
}
/* 1 when the scaler for this configuration is current; otherwise starts building it (in the background) and returns 0. */
static int ensure(int ci, id<MTLTexture> src, id<MTLTexture> dst, NSUInteger ow, NSUInteger oh, int mode) {
    ctx_t *c = &g_c[ci]; int own = dst == nil;
    cfg_t want = {src.width, src.height, ow, oh, src.pixelFormat, own ? MTLPixelFormatInvalid : dst.pixelFormat, mode, own};
    if (c->cur >= 0 && cfg_eq(&c->cache[c->cur].cfg, &want)) { c->cache[c->cur].used = ++c->tick; return 1; }
    if (use_cached(ci, c, &want)) return 1;
    if (g_unavailable || is_bad(c, &want)) return 0;
    if (!shared_objects(src.device)) { g_unavailable = 1; return 0; }
    if (!c->fence) c->fence = [g_dev newFence];
    if (!atomic_exchange(&c->building, 1)) {
        /* A build that finished after the adopt in use_cached left its result in ready: take it now, or the next build's
           result would replace it and its own output texture would stay resident for good. It may be the one wanted. */
        if (use_cached(ci, c, &want)) { atomic_store(&c->building, 0); return 1; }
        if (is_bad(c, &want)) { atomic_store(&c->building, 0); return 0; }
        NSUInteger su = src.usage, du = own ? 0 : dst.usage; int ml = !own && dst.storageMode == MTLStorageModeMemoryless;
        dispatch_async(g_build_q, ^{ build(ci, want, su, du, ml); });
    }
    c->waited++;
    return 0;
}
int mfxup_prepare(int ci, id<MTLTexture> src, id<MTLTexture> dst, NSUInteger ow, NSUInteger oh, int mode) {
    if (ci < 0 || ci >= MFXUP_NCTX || !src) return 0;
    if (dst) { ow = dst.width; oh = dst.height; }
    return ensure(ci, src, dst, ow, oh, mode);
}

id<MTLTexture> mfxup_encode_ex(int ci, id<MTL4CommandBuffer> cb, id<MTLTexture> src, NSUInteger in_w, NSUInteger in_h,
                               id<MTLTexture> dst, NSUInteger ow, NSUInteger oh, int mode) {
    if (ci < 0 || ci >= MFXUP_NCTX) return nil;
    ctx_t *c = &g_c[ci];
    if (dst) { ow = dst.width; oh = dst.height; }
    if (!cb || !src || src.textureType != MTLTextureType2D || (dst && dst.textureType != MTLTextureType2D) || !in_w || !in_h ||
        in_w > src.width || in_h > src.height || !ow || !oh || !ensure(ci, src, dst, ow, oh, mode)) { c->refused++; return nil; }
    built_t *b = &c->cache[c->cur];
    id<MTLTexture> out = dst ? dst : b->out;
    /* MetalFX waits on the fence before reading: signal it after everything before (the producer of src, earlier users of
       the target). The next replay encoder starts with a queue barrier on all stages, which orders it after MetalFX. */
    id<MTL4ComputeCommandEncoder> e = [cb computeCommandEncoder];
    [e barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageDispatch visibilityOptions:MTL4VisibilityOptionDevice];
    [e updateFence:c->fence afterEncoderStages:MTLStageDispatch];
    [e endEncoding];
    b->sc.colorTexture = src; b->sc.outputTexture = out; b->sc.inputContentWidth = in_w; b->sc.inputContentHeight = in_h; b->sc.fence = c->fence;
    [b->sc encodeToCommandBuffer:cb];
    c->done++;atomic_fetch_add(&g_completed[ci],1);
    sweep(c);
    return out;
}

id<MTLTexture> mfxup_encode(id<MTL4CommandBuffer> cb, id<MTLTexture> src, NSUInteger out_w, NSUInteger out_h) {
    return src ? mfxup_encode_ex(MFXUP_SCENE, cb, src, src.width, src.height, nil, out_w, out_h, MTLFXSpatialScalerColorProcessingModeHDR) : nil;
}

void mfxup_stats(int ci, char *out, size_t n) {
    if (ci < 0 || ci >= MFXUP_NCTX) { snprintf(out, n, "?"); return; }
    const ctx_t *c = &g_c[ci];
    cfg_t z = {0}; const cfg_t *k = c->cur >= 0 ? &c->cache[c->cur].cfg : &z;
    snprintf(out, n, "MetalFX 空间放大（%s）：%llu 次，未放大 %llu 次（其中等后台建放大器 %llu 次）（%lux%lu → %lux%lu）", g_name[ci], (unsigned long long)c->done,
             (unsigned long long)c->refused, (unsigned long long)c->waited, (unsigned long)k->in_w, (unsigned long)k->in_h, (unsigned long)k->out_w, (unsigned long)k->out_h);
}
