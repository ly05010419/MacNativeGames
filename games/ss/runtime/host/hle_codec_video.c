// nn::codec video decoders (the Switch 2's SDK): taken over on the host, VP9 decoded with libvpx as in hle_movie.c. The SDK sends every call to a decoder service (IPC) the host
// does not serve: nn::codec::video::Initialize crashed in it (Donkey Kong Bananza, sdk+0x2de99c, after its first punch, when the guide movie starts). The SDK's protocol, read from
// its implementation (sdk 20.5.19 Ounce):
//   - the client makes one image pool per output picture (MakeDecodeImagePreference → CalculateDecoderImagePoolBuffer → InitializeDecoderImagePool) and registers each with the
//     decoder under an index (RegisterVp9DecoderBuffer). Registering puts the index in the decoder's queue of acquirable buffers (8 deep), with a sync handle already signalled.
//   - Acquire takes the oldest index and its sync handle (description 3604 when the queue is empty); ReleaseVp9DecoderOutputBuffer gives the buffer to the decoder to fill and puts
//     it back in the queue at once, with a new sync handle that signals when the decoder has written a picture into it. The client polls nn::gs::TryWaitNativeSync on that handle,
//     then reads the picture's properties (description 3604 until it is filled) and the picture (ReadDecoderOutputImageRgba / Nv12, the CPU route; Bananza's player takes that one)
//   - SubmitVp9DecoderInputBuffer takes one packet with its time and an end-of-stream flag; description 3603 when the decoder cannot take more (the client retries after the
//     submission-ready event).
// Pictures go into the released buffers in release order. Packets decode on the submitting thread; pictures with no released buffer to go into wait in a short queue, and a full
// queue refuses packets. The SDK's opaque types hold a host pointer at +0 (pool: 0x20 bytes are the SDK's; decoder: 0x400), the decoder's event lives in its type at +0x100.
// VP8, VP9, H.264 and AV1 decoders share it all (cv_dec_*). Not served: encrypted input and alias pools.
// SDK 22 (Fire Emblem Fortune's Weave's movie module, subsdk2) reaches the same protocol through other names: ByTransferMemory / ByRemoteMemory initializers, a preference with an image tiling mode,
// a pool without its mode, and a submission event returned rather than written out. Its player can take pictures the GPU route: GetDecoderImagePoolNativeImageHandle's handle goes through
// nvnNativeImage to an RGBA texture (hle_nvn.c "Native images"), so a pool whose handle was asked for keeps each picture as RGBA in the pool's own memory, which the texture is made over
#include "hle.h"
#include <pthread.h>
#include <stdbool.h>
#include "vpx/vpx_decoder.h"
#include "vpx/vp8dx.h"
#include "hle_movie_yuv.h"
#include "hle_movie_avc.h"   // H.264 on VideoToolbox
#include "hle_movie_av1.h"   // AV1 on VideoToolbox

#define CV_MAGIC 0x56436d44u        // 'DmCV'
#define CV_SYNC 0x53436d44u         // 'DmCS': a sync handle the host made (dec id, index, generation)
#define CV_BUFS 32
#define CV_HOLD 4                   // decoded pictures waiting for a released buffer
#define CV_EVENT_OFF 0x100          // the submission-ready SystemEventType inside the decoder type
#define CV_R(desc) ((Result)(0x6f | ((desc) << 9)))   // nn::codec results (module 111)
#define CV_BUSY CV_R(3603)          // the decoder cannot take the packet now
#define CV_NONE CV_R(3604)          // no buffer to acquire / no picture in the buffer yet

typedef struct { uint16_t w, h; uint32_t fmt, mode, magic; uint32_t pad[2]; } cv_pref;   // VideoDecoderImagePreference as the host fills it (returned in x8)
typedef struct { uint32_t magic; uint16_t w, h; uint32_t fmt, count; vpx_image_t **img; uint8_t *mem; uint64_t size; int slot; _Atomic int native; } cv_pool;   // mem, size: the game's pool memory; native: a handle was asked for (RGBA kept in mem); slot: its place in g_cv_pools + 1
typedef struct { uint32_t magic, dec, idx, gen; } cv_sync;   // nn::gs::NativeSyncHandle (16 bytes)
enum { CV_VP8, CV_VP9, CV_H264, CV_AV1 };
static const char *const k_cv_name[] = {"VP8", "VP9", "H.264", "AV1"};
typedef struct {
    uint32_t magic, id; pthread_mutex_t mu; vpx_codec_ctx_t vpx; int vpx_ok; void *event; uint64_t submits, pictures;
    int codec; uint16_t w, h; mavc *avc; uint8_t *csd; size_t csd_n;   // H.264: VideoToolbox and the parameter sets Initialize gave
    mav1 *av1;   // AV1: VideoToolbox, or dav1d where the chip has no AV1 decoder (hle_movie_av1.c)
    struct { cv_pool *pool; uint32_t pidx; int reg, filled; uint32_t gen, filled_gen; uint16_t w, h; int64_t ts; uint8_t eos; } buf[CV_BUFS];
    uint32_t fifo[8], fhead, fcount;          // acquirable indices
    uint32_t pend[CV_BUFS], phead, pcount;    // released, waiting for a picture
    vpx_image_t *held[CV_HOLD]; int64_t held_ts[CV_HOLD]; uint8_t held_eos[CV_HOLD]; int nheld;
    int eos_pending;
} cv_dec;

static cv_dec *g_cv[16];
static pthread_mutex_t g_cv_mu = PTHREAD_MUTEX_INITIALIZER;   // lock order: g_cv_mu, then a decoder's mu, then g_spt_mu

static cv_dec *cv_of(void *t) { cv_dec *d = t ? *(cv_dec **)t : NULL; return d && d->magic == CV_MAGIC ? d : NULL; }
static cv_pool *pool_of(const void *t) { cv_pool *p = t ? *(cv_pool *const *)t : NULL; return p && p->magic == CV_MAGIC ? p : NULL; }

static vpx_image_t *img_copy(const vpx_image_t *im) {   // an owned I420 copy of a libvpx picture (its buffers are reused by the next decode)
    vpx_image_t *c = vpx_img_alloc(NULL, VPX_IMG_FMT_I420, im->d_w, im->d_h, 1);
    if (!c) return NULL;
    c->cs = im->cs; c->range = im->range;
    for (int p = 0; p < 3; p++) {
        unsigned w = p ? (im->d_w + 1) / 2 : im->d_w, h = p ? (im->d_h + 1) / 2 : im->d_h;
        for (unsigned y = 0; y < h; y++) memcpy(c->planes[p] + (size_t)y * c->stride[p], im->planes[p] + (size_t)y * im->stride[p], w);
    }
    return c;
}

// Sync point events (nn::syncpt::sdk): the player's worker waits on one (in a MultiWait) for the sync handle of the picture it expects. Their library is set up by the graphics
// initialisation the host replaces, so CreateSyncPointEvent jumped through its null table (pc 0). The event type (0x80 bytes in Bananza's layout) holds a SystemEvent the host
// binds at +0 (0x30 bytes) and the awaited handle at +0x40; an armed one is signalled when its buffer is filled
typedef struct { uint32_t magic, armed; cv_sync want; } cv_spt;
#define SPT(e) ((cv_spt *)((uint8_t *)(e) + 0x40))
static void *g_spt[64];   // armed events (g_spt_mu)
static pthread_mutex_t g_spt_mu = PTHREAD_MUTEX_INITIALIZER;
static bool sync_reached(const cv_sync *s);
static void spt_fire(cv_dec *d, uint32_t i) {   // buffer i of d filled: signal the events waiting for it (decoder lock held)
    pthread_mutex_lock(&g_spt_mu);
    for (int k = 0; k < 64; k++) { void *e = g_spt[k]; cv_spt *t = e ? SPT(e) : NULL;
        if (t && t->want.dec == d->id && t->want.idx == i && d->buf[i].filled_gen >= t->want.gen) { t->armed = 0; g_spt[k] = NULL; hle_signal_system_event(e); } }
    pthread_mutex_unlock(&g_spt_mu);
}
static void spt_unarm(void *e) { pthread_mutex_lock(&g_spt_mu); for (int k = 0; k < 64; k++) if (g_spt[k] == e) g_spt[k] = NULL; pthread_mutex_unlock(&g_spt_mu); SPT(e)->armed = 0; }

// A native image: one picture's RGBA in the pool's memory, tight rows when 4 w is on the 64-byte stride NVN's linear textures take, and each picture on a 16 KB page, as a Metal buffer over it needs
#define CV_NATIVE_ALIGN 0x4000ull
static uint32_t native_stride(uint32_t w) { return (w * 4 + 63) & ~63u; }
static uint64_t native_size(uint32_t w, uint32_t h) { return ((uint64_t)native_stride(w) * h + CV_NATIVE_ALIGN - 1) & ~(CV_NATIVE_ALIGN - 1); }
static void pool_native_write(cv_pool *p, uint32_t i, const vpx_image_t *im) {
    if (!p->native || !p->mem || (i + 1) * native_size(p->w, p->h) > p->size) return;
    uint8_t *dst = p->mem + i * native_size(p->w, p->h);
    uint32_t stride = native_stride(p->w);
    if (stride == (uint32_t)p->w * 4) { md_rgba(im, dst, p->w, p->h); return; }
    uint8_t *tmp = malloc((size_t)p->w * p->h * 4);
    if (!tmp) return;
    md_rgba(im, tmp, p->w, p->h);
    for (uint32_t y = 0; y < p->h; y++) memcpy(dst + (size_t)y * stride, tmp + (size_t)y * p->w * 4, (size_t)p->w * 4);
    free(tmp);
}
// Pictures and end of stream into the released buffers, in order (decoder lock held)
static void drain(cv_dec *d) {
    while (d->pcount && (d->nheld || d->eos_pending)) {
        uint32_t i = d->pend[d->phead]; d->phead = (d->phead + 1) % CV_BUFS; d->pcount--;
        typeof(d->buf[0]) *b = &d->buf[i];
        if (d->nheld) {
            vpx_image_t *im = d->held[0]; int64_t ts = d->held_ts[0]; uint8_t eos = d->held_eos[0];
            memmove(d->held, d->held + 1, sizeof d->held[0] * (CV_HOLD - 1)); memmove(d->held_ts, d->held_ts + 1, sizeof d->held_ts[0] * (CV_HOLD - 1));
            memmove(d->held_eos, d->held_eos + 1, CV_HOLD - 1); d->nheld--;
            b->w = (uint16_t)im->d_w; b->h = (uint16_t)im->d_h; b->ts = ts; b->eos = eos;
            if (b->pool && b->pidx < b->pool->count) { pool_native_write(b->pool, b->pidx, im); if (b->pool->img[b->pidx]) vpx_img_free(b->pool->img[b->pidx]); b->pool->img[b->pidx] = im; } else vpx_img_free(im);
            if (!d->pictures++) LOG("[codec] %s decoder %u: first picture %ux%u into buffer %u", k_cv_name[d->codec], d->id, b->w, b->h, i);
        } else { b->w = b->h = 0; b->ts = 0; b->eos = 1; d->eos_pending = 0; }
        if (b->eos) LOG("[codec] %s decoder %u: end of stream after %llu pictures", k_cv_name[d->codec], d->id, (unsigned long long)d->pictures);
        b->filled = 1; b->filled_gen = b->gen;
        spt_fire(d, i);
    }
    if (d->event) { if (d->nheld < CV_HOLD) hle_signal_system_event(d->event); else hle_clear_system_event(d->event); }
}

HLE_FORCE("_ZN2nn5codec5video10InitializeEv", "the decoder service is the host's (hle_codec_video.c)", Result, h_cv_init, void) { return 0; }
HLE_FORCE("_ZN2nn5codec5video8FinalizeEv", "the decoder service is the host's", Result, h_cv_fin, void) { return 0; }

HLE_FORCE("_ZN2nn5codec25MakeDecodeImagePreferenceEttNS0_24VideoDecoderOutputFormatENS0_9CodecModeE", "the preference only carries the size and format to the host's pool",
          cv_pref, h_cv_pref, uint16_t w, uint16_t h, uint32_t fmt, uint32_t mode) {
    return (cv_pref){w, h, fmt, mode, CV_MAGIC, {0, 0}};
}
HLE_FORCE("_ZN2nn5codec25MakeDecodeImagePreferenceEttNS0_24VideoDecoderOutputFormatENS0_15ImageTilingModeE", "the preference only carries the size and format to the host's pool (SDK 22's name)",
          cv_pref, h_cv_pref22, uint64_t w, uint64_t h, uint32_t fmt, uint32_t tiling) {
    return (cv_pref){(uint16_t)w, (uint16_t)h, fmt, tiling, CV_MAGIC, {0, 0}};
}
HLE_FORCE("_ZN2nn5codec31CalculateDecoderImagePoolBufferEPmS1_RKNS0_27VideoDecoderImagePreferenceEj", "pool memory: room for RGBA pictures (a native image's texture is made over it; otherwise the host keeps its own copies)",
          Result, h_cv_poolsize, uint64_t *size, uint64_t *align, const cv_pref *p, uint32_t n) {
    *size = native_size(p->w, p->h) * (n ? n : 1); *align = CV_NATIVE_ALIGN;
    return 0;
}
static cv_pool *g_cv_pools[256];   // what a native image handle names (g_cv_mu): (slot + 1) << 8 | picture
static Result cv_pool_init(void *t, void *mem, uint64_t size, const cv_pref *p, uint32_t n) {
    cv_pool *o = calloc(1, sizeof *o); o->magic = CV_MAGIC; o->w = p->w; o->h = p->h; o->fmt = p->fmt; o->count = n ? n : 1; o->img = calloc(o->count, sizeof *o->img); o->mem = mem; o->size = size;
    *(cv_pool **)t = o;
    static int once; if (!once++) LOG("[codec] image pool %ux%u format %u, tiling or mode %u, %u picture(s): the host keeps the decoded pictures", p->w, p->h, p->fmt, p->mode, o->count);
    return 0;
}
HLE_FORCE("_ZN2nn5codec26InitializeDecoderImagePoolEPNS0_25VideoDecoderImagePoolTypeEPvmRKNS0_27VideoDecoderImagePreferenceEjNS0_9CodecModeE", "a host pool of decoded pictures",
          Result, h_cv_pool, void *t, void *mem, uint64_t size, const cv_pref *p, uint32_t n, uint32_t mode) { (void)mode; return cv_pool_init(t, mem, size, p, n); }
HLE_FORCE("_ZN2nn5codec26InitializeDecoderImagePoolEPNS0_25VideoDecoderImagePoolTypeEPvmRKNS0_27VideoDecoderImagePreferenceEj", "a host pool of decoded pictures (SDK 22's name)",
          Result, h_cv_pool22, void *t, void *mem, uint64_t size, const cv_pref *p, uint32_t n) { return cv_pool_init(t, mem, size, p, n); }
HLE_FORCE("_ZN2nn5codec24FinalizeDecoderImagePoolEPNS0_25VideoDecoderImagePoolTypeE", "frees the host pool", void, h_cv_poolfin, void *t) {
    cv_pool *p = pool_of(t); if (!p) return;
    pthread_mutex_lock(&g_cv_mu);   // a decoder still holding a buffer of this pool keeps it, with no pool to write its picture into
    for (int k = 0; k < 16; k++) if (g_cv[k]) { pthread_mutex_lock(&g_cv[k]->mu); for (int i = 0; i < CV_BUFS; i++) if (g_cv[k]->buf[i].pool == p) g_cv[k]->buf[i].pool = NULL; pthread_mutex_unlock(&g_cv[k]->mu); }
    if (p->slot) g_cv_pools[p->slot - 1] = NULL;
    pthread_mutex_unlock(&g_cv_mu);
    for (uint32_t i = 0; i < p->count; i++) if (p->img[i]) vpx_img_free(p->img[i]);
    free(p->img); p->magic = 0; free(p); *(cv_pool **)t = NULL;
}
HLE_FORCE("_ZN2nn5codec36GetDecoderImagePoolNativeImageHandleEPKNS0_25VideoDecoderImagePoolTypeEj", "a handle the host's nvnNativeImage knows; the pool keeps its pictures as RGBA from now on",
          uint32_t, h_cv_native, const void *t, uint32_t i) {
    cv_pool *p = pool_of(t); if (!p || i >= p->count) return 0;
    pthread_mutex_lock(&g_cv_mu);
    if (!p->slot) for (int k = 0; k < 256; k++) if (!g_cv_pools[k]) { g_cv_pools[k] = p; p->slot = k + 1; break; }
    pthread_mutex_unlock(&g_cv_mu);
    if (!p->slot) { static int once; if (!once++) LOG("[RED] nn::codec: more than 256 pools with native images"); return 0; }
    p->native = 1;
    return (uint32_t)p->slot << 8 | i;
}
// A native image handle's picture for hle_nvn.c: 1 and its memory, size and layout; 0 for a handle no pool holds
int dm_native_image(uint32_t handle, uint8_t **mem, uint64_t *size, uint32_t *w, uint32_t *h, uint32_t *stride) {
    uint32_t slot = handle >> 8, i = handle & 0xff; int ok = 0;
    pthread_mutex_lock(&g_cv_mu);
    cv_pool *p = slot && slot <= 256 ? g_cv_pools[slot - 1] : NULL;
    if (p && p->mem && i < p->count && (i + 1) * native_size(p->w, p->h) <= p->size) {
        *mem = p->mem + i * native_size(p->w, p->h); *size = native_size(p->w, p->h); *w = p->w; *h = p->h; *stride = native_stride(p->w); ok = 1;
    }
    pthread_mutex_unlock(&g_cv_mu);
    return ok;
}
HLE_FORCE("_ZN2nn5codec29ReadDecoderOutputImageRgbaHdrEPKNS0_25VideoDecoderImagePoolTypeEjttttPvmm", "HDR pictures are not served: black",
          Result, h_cv_rgbahdr, const void *t, uint32_t i, uint64_t x, uint64_t y, uint64_t w, uint64_t h, void *dst, uint64_t stride, uint64_t size) {
    (void)t; (void)i; (void)x; (void)y; (void)w; (void)h; (void)stride;
    static int once; if (!once++) LOG("[RED] nn::codec::ReadDecoderOutputImageRgbaHdr: HDR pictures are not served, black");
    memset(dst, 0, size); return 0;
}
// The picture of pool image i (the region x, y, w, h; Bananza reads it whole) as RGBA or as NV12
HLE_FORCE("_ZN2nn5codec26ReadDecoderOutputImageRgbaEPKNS0_25VideoDecoderImagePoolTypeEjttttPvmm", "converts the host's picture (md_rgba)",
          Result, h_cv_rgba, const void *t, uint32_t i, uint16_t x, uint16_t y, uint16_t w, uint16_t h, void *dst, uint64_t stride, uint64_t size) {
    cv_pool *p = pool_of(t); const vpx_image_t *im = p && i < p->count ? p->img[i] : NULL;
    if (!im) { memset(dst, 0, size); return 0; }
    if (x || y || stride != (uint64_t)w * 4 || size < (uint64_t)w * h * 4) { static int once; if (!once++) LOG("[RED] nn::codec::ReadDecoderOutputImageRgba: region %u,%u stride %llu: only whole tight pictures are served", x, y, (unsigned long long)stride); return 0; }
    md_rgba(im, dst, w, h);
    return 0;
}
HLE_FORCE("_ZN2nn5codec26ReadDecoderOutputImageNv12EPKNS0_25VideoDecoderImagePoolTypeEjttttPvmmS4_mm", "converts the host's picture (md_nv12)",
          Result, h_cv_nv12, const void *t, uint32_t i, uint16_t x, uint16_t y, uint16_t w, uint16_t h, void *ydst, uint64_t ystride, uint64_t ysize, void *uvdst, uint64_t uvstride, uint64_t uvsize) {
    cv_pool *p = pool_of(t); const vpx_image_t *im = p && i < p->count ? p->img[i] : NULL;
    if (x || y || ystride != w || uvstride != w || ysize < (uint64_t)w * h || uvsize < (uint64_t)w * h / 2) { static int once; if (!once++) LOG("[RED] nn::codec::ReadDecoderOutputImageNv12: region %u,%u strides %llu / %llu: only whole tight pictures are served", x, y, (unsigned long long)ystride, (unsigned long long)uvstride); return 0; }
    if (!im) { memset(ydst, 16, ysize); memset(uvdst, 128, uvsize); return 0; }
    if ((uint8_t *)uvdst == (uint8_t *)ydst + (size_t)w * h) { md_nv12(im, ydst, w, h); return 0; }
    uint8_t *tmp = malloc((size_t)w * h * 3 / 2); md_nv12(im, tmp, w, h);
    memcpy(ydst, tmp, (size_t)w * h); memcpy(uvdst, tmp + (size_t)w * h, (size_t)w * h / 2); free(tmp);
    return 0;
}

// One host decoder for every codec the SDK has a decoder type for (Vp8 / Vp9 / H264 / Av1DecoderType: the same layout, its event at +8 in the SDK, all calls taken over here):
// VP8 and VP9 through libvpx, H.264 through VideoToolbox and AV1 through VideoToolbox or dav1d (hle_movie_avc.c, hle_movie_av1.c, whose NV12 pictures are copied into the pool as I420)
static void cv_open(cv_dec *d) {   // (re)creates the codec state: a new stream starts at the next key frame
    if (d->codec == CV_AV1) {
        if (!d->av1) d->av1 = mav1_new(); else mav1_reset(d->av1);
        d->vpx_ok = 1;
        return;
    }
    if (d->codec == CV_H264) { if (!d->avc) d->avc = mavc_new(); else mavc_drop(d->avc); if (d->csd_n) mavc_param_sets(d->avc, d->csd, d->csd_n); d->vpx_ok = 1; return; }
    if (d->vpx_ok) vpx_codec_destroy(&d->vpx);
    vpx_codec_dec_cfg_t cfg = {.threads = 2, .w = d->w, .h = d->h};
    d->vpx_ok = vpx_codec_dec_init(&d->vpx, d->codec == CV_VP8 ? vpx_codec_vp8_dx() : vpx_codec_vp9_dx(), &cfg, 0) == VPX_CODEC_OK;
    if (!d->vpx_ok) LOG("[RED] nn::codec %s decoder: libvpx init failed", k_cv_name[d->codec]);
}
static Result cv_dec_init(void *t, int codec, uint16_t w, uint16_t h, uint32_t in, uint32_t out, const void *csd, uint64_t csd_n) {
    cv_dec *d = calloc(1, sizeof *d);
    d->magic = CV_MAGIC; d->codec = codec; d->w = w; d->h = h; pthread_mutex_init(&d->mu, NULL);
    if (csd && csd_n && csd_n < (1u << 20)) { d->csd = malloc((size_t)csd_n); memcpy(d->csd, csd, (size_t)csd_n); d->csd_n = (size_t)csd_n; }
    cv_open(d);
    pthread_mutex_lock(&g_cv_mu);
    for (int i = 0; i < 16; i++) if (!g_cv[i]) { g_cv[i] = d; d->id = (uint32_t)i + 1; break; }
    pthread_mutex_unlock(&g_cv_mu);
    if (!d->id) die("nn::codec: more than 16 video decoders");
    d->event = (uint8_t *)t + CV_EVENT_OFF; hle_bind_system_event(d->event, 0); hle_signal_system_event(d->event);
    *(cv_dec **)t = d;
    LOG("[codec] %s decoder %u: %ux%u, input format %u, output format %u%s (%s on the host)", k_cv_name[codec], d->id, w, h, in, out,
        d->csd_n ? ", with parameter sets" : "", codec == CV_AV1 ? mav1_backend(d->av1) : codec == CV_H264 ? "VideoToolbox" : "libvpx");
    return 0;
}
static Result cv_dec_fin(void *t) {
    cv_dec *d = cv_of(t); if (!d) return 0;
    pthread_mutex_lock(&g_cv_mu); g_cv[d->id - 1] = NULL; pthread_mutex_lock(&d->mu); pthread_mutex_unlock(&d->mu); pthread_mutex_unlock(&g_cv_mu);   // no sync handle lookup is still inside
    if (d->codec != CV_H264 && d->codec != CV_AV1 && d->vpx_ok) vpx_codec_destroy(&d->vpx);
    if (d->avc) mavc_free(d->avc);
    if (d->av1) mav1_free(d->av1);
    for (int i = 0; i < d->nheld; i++) vpx_img_free(d->held[i]);
    free(d->csd); d->magic = 0;
    // The submission-ready event goes with the decoder, as in the SDK. Kept, it stayed in the host's bound-event table, so the next decoder initialised in the same memory found its
    // event "bound" and was not given one: the game had cleared that memory, and nn::os aborted on the uninitialised event (Donkey Kong Bananza, its second movie)
    static void (*destroy)(void *); if (!destroy && !(destroy = guest_sym("_ZN2nn2os18DestroySystemEventEPNS0_15SystemEventTypeE"))) die("nn::os::DestroySystemEvent missing");
    destroy(d->event);
    pthread_mutex_destroy(&d->mu); free(d); *(cv_dec **)t = NULL;
    return 0;
}
static Result cv_dec_reg(void *t, uint32_t i, void *pool, uint32_t pidx) {
    cv_dec *d = cv_of(t); if (!d || i >= CV_BUFS) { LOG("[RED] nn::codec::Register…DecoderBuffer index %u", i); return CV_R(2004); }
    pthread_mutex_lock(&d->mu);
    d->buf[i].pool = pool_of(pool); d->buf[i].pidx = pidx; d->buf[i].reg = 1; d->buf[i].filled = 0; d->buf[i].gen = 0; d->buf[i].filled_gen = 0;
    if (d->fcount < 8) { d->fifo[(d->fhead + d->fcount) % 8] = i; d->fcount++; } else LOG("[RED] nn::codec: acquirable queue full at register %u", i);
    pthread_mutex_unlock(&d->mu);
    return 0;
}
static void cv_drop_index(uint32_t *q, uint32_t *head, uint32_t *count, uint32_t cap, uint32_t i) {   // removes i from a ring of indices, keeping the others' order
    uint32_t n = 0, tmp[CV_BUFS];
    for (uint32_t k = 0; k < *count; k++) { uint32_t v = q[(*head + k) % cap]; if (v != i) tmp[n++] = v; }
    for (uint32_t k = 0; k < n; k++) q[k] = tmp[k];
    *head = 0; *count = n;
}
static Result cv_dec_unreg(void *t, uint32_t i) {   // the buffer leaves the decoder: no longer acquirable, filled or waited for
    cv_dec *d = cv_of(t); if (!d || i >= CV_BUFS) return CV_R(2004);
    pthread_mutex_lock(&d->mu);
    d->buf[i].reg = 0; d->buf[i].filled = 0; d->buf[i].pool = NULL; d->buf[i].gen++;
    cv_drop_index(d->fifo, &d->fhead, &d->fcount, 8, i); cv_drop_index(d->pend, &d->phead, &d->pcount, CV_BUFS, i);
    pthread_mutex_unlock(&d->mu);
    return 0;
}
static Result cv_dec_reset(void *t) {   // Stop / Flush: pictures not yet given to a buffer and a pending end of stream are dropped, the decoder starts over at the next key frame
    cv_dec *d = cv_of(t); if (!d) return 0;
    pthread_mutex_lock(&d->mu);
    for (int i = 0; i < d->nheld; i++) vpx_img_free(d->held[i]);
    d->nheld = 0; d->eos_pending = 0;
    cv_open(d);
    if (d->event) hle_signal_system_event(d->event);
    pthread_mutex_unlock(&d->mu);
    return 0;
}
static Result cv_dec_event(void **out, void *t) { cv_dec *d = cv_of(t); *out = d ? d->event : NULL; return d ? 0 : CV_R(2004); }
static int cv_hold(cv_dec *d, vpx_image_t *c, int64_t ts) {   // a decoded picture waits for a released buffer (decoder lock held); 0 when the host holds no more
    if (d->nheld >= CV_HOLD) { static int n; if (n++ < 4) LOG("[RED] nn::codec %s: a packet gave more pictures than the host holds: one dropped", k_cv_name[d->codec]); vpx_img_free(c); return 0; }
    d->held[d->nheld] = c; d->held_ts[d->nheld] = ts; d->held_eos[d->nheld] = 0; d->nheld++; return 1;
}
static vpx_image_t *nv12_to_i420(const uint8_t *nv12, unsigned w, unsigned h) {   // VideoToolbox's NV12 as the I420 the pool keeps
    vpx_image_t *c = vpx_img_alloc(NULL, VPX_IMG_FMT_I420, w, h, 1); if (!c) return NULL;
    for (unsigned y = 0; y < h; y++) memcpy(c->planes[0] + (size_t)y * c->stride[0], nv12 + (size_t)y * w, w);
    const uint8_t *uv = nv12 + (size_t)w * h;
    for (unsigned y = 0; y < (h + 1) / 2; y++) for (unsigned x = 0; x < (w + 1) / 2; x++) {
        c->planes[1][(size_t)y * c->stride[1] + x] = uv[(size_t)y * w + 2 * x];
        c->planes[2][(size_t)y * c->stride[2] + x] = uv[(size_t)y * w + 2 * x + 1]; }
    return c;
}
static vpx_image_t *av1_to_i420(const mav1_picture *p) {   // the decoder's NV12, rows at its strides, as the I420 the pool keeps
    vpx_image_t *c = vpx_img_alloc(NULL, VPX_IMG_FMT_I420, (unsigned)p->w, (unsigned)p->h, 1); if (!c) return NULL;
    c->cs = p->matrix == 2 ? VPX_CS_BT_709 : p->matrix == 5 ? VPX_CS_BT_2020 : VPX_CS_BT_601; c->range = p->full_range ? VPX_CR_FULL_RANGE : VPX_CR_STUDIO_RANGE;
    for (int y = 0; y < p->h; y++) memcpy(c->planes[0] + (size_t)y * c->stride[0], p->y + (size_t)y * p->ystride, (size_t)p->w);
    for (int y = 0; y < (p->h + 1) / 2; y++) {
        const uint8_t *uv = p->uv + (size_t)y * p->uvstride; uint8_t *u = c->planes[1] + (size_t)y * c->stride[1], *v = c->planes[2] + (size_t)y * c->stride[2];
        for (int x = 0; x < (p->w + 1) / 2; x++) { u[x] = uv[2 * x]; v[x] = uv[2 * x + 1]; }
    }
    return c;
}
// One packet: decoded at once; its pictures wait for released buffers (end of stream marks the last one, or comes as a buffer of its own)
static Result cv_dec_submit(uint64_t *out, void *t, const void *data, uint64_t size, int64_t ts, bool eos) {
    cv_dec *d = cv_of(t); if (!d) return CV_R(2004);
    pthread_mutex_lock(&d->mu);
    if (d->nheld >= CV_HOLD) { if (d->event) hle_clear_system_event(d->event); pthread_mutex_unlock(&d->mu); return CV_BUSY; }
    int got = 0;
    if (d->codec == CV_H264 && d->avc) {   // VideoToolbox reorders: pictures leave in presentation order, all of them at the end of the stream
        if (size && mavc_decode(d->avc, data, (int)size, ts / 1000) < 0) { static int n; if (n++ < 4) LOG("[RED] nn::codec H.264: VideoToolbox could not decode a packet"); }
        uint8_t *nv12 = malloc((size_t)d->w * d->h * 3 / 2); int64_t pts = 0;
        while (nv12 && d->nheld < CV_HOLD && mavc_take(d->avc, eos, nv12, d->w, d->h, &pts) == 1) { vpx_image_t *c = nv12_to_i420(nv12, d->w, d->h); if (c) got += cv_hold(d, c, pts * 1000); }
        free(nv12);
    } else if (d->codec == CV_AV1) {
        if (size) {
            mav1_picture pic;
            if (mav1_decode(d->av1, data, (size_t)size, &pic) == 1) { vpx_image_t *c = av1_to_i420(&pic); if (c) got += cv_hold(d, c, ts); }
        }
    } else if (size && d->vpx_ok) {
        if (vpx_codec_decode(&d->vpx, data, (unsigned)size, NULL, 0) != VPX_CODEC_OK) { static int n; if (n++ < 4) LOG("[RED] nn::codec %s: %s (%s)", k_cv_name[d->codec], vpx_codec_error(&d->vpx), vpx_codec_error_detail(&d->vpx) ? vpx_codec_error_detail(&d->vpx) : ""); }
        vpx_codec_iter_t it = NULL; const vpx_image_t *im;
        while ((im = vpx_codec_get_frame(&d->vpx, &it))) { vpx_image_t *c = img_copy(im); if (c) got += cv_hold(d, c, ts); }
    }
    if (eos) { if (got) d->held_eos[d->nheld - 1] = 1; else d->eos_pending = 1; }
    *out = ++d->submits;
    drain(d);
    pthread_mutex_unlock(&d->mu);
    return 0;
}
static Result cv_dec_acq(uint32_t *idx, cv_sync *sync, void *t) {
    cv_dec *d = cv_of(t); if (!d) return CV_R(2004);
    pthread_mutex_lock(&d->mu);
    if (!d->fcount) { pthread_mutex_unlock(&d->mu); return CV_NONE; }
    uint32_t i = d->fifo[d->fhead]; d->fhead = (d->fhead + 1) % 8; d->fcount--;
    *idx = i; *sync = (cv_sync){CV_SYNC, d->id, i, d->buf[i].gen};
    pthread_mutex_unlock(&d->mu);
    return 0;
}
static void cv_dec_rel(void *t, uint32_t i, cv_sync sync) {
    (void)sync; cv_dec *d = cv_of(t); if (!d || i >= CV_BUFS || !d->buf[i].reg) { LOG("[RED] nn::codec::Release…DecoderOutputBuffer index %u", i); return; }
    pthread_mutex_lock(&d->mu);
    d->buf[i].gen++; d->buf[i].filled = 0;
    if (d->pcount < CV_BUFS) { d->pend[(d->phead + d->pcount) % CV_BUFS] = i; d->pcount++; }
    if (d->fcount < 8) { d->fifo[(d->fhead + d->fcount) % 8] = i; d->fcount++; } else LOG("[RED] nn::codec: acquirable queue full at release %u", i);
    drain(d);
    pthread_mutex_unlock(&d->mu);
}
static Result cv_dec_props(uint16_t *w, uint16_t *h, uint16_t *cw, uint16_t *ch, int64_t *ts, bool *eos, void *t, uint32_t i) {
    cv_dec *d = cv_of(t); if (!d || i >= CV_BUFS) return CV_R(2004);
    pthread_mutex_lock(&d->mu);
    int ok = d->buf[i].filled;
    if (ok) { *w = *cw = d->buf[i].w; *h = *ch = d->buf[i].h; *ts = d->buf[i].ts; *eos = d->buf[i].eos != 0; }
    pthread_mutex_unlock(&d->mu);
    return ok ? 0 : CV_NONE;
}
static Result cv_dec_encrypted(void) { static int once; if (!once++) LOG("[RED] nn::codec: an encrypted input buffer: protected streams are not served"); return CV_R(2004); }
// Each decoder type's entry points, by their names in the SDK (the work buffer, the two Initialize overloads without the H.264 parameter sets, …)
#define CV_FAMILY(tag, codec, s_work, s_init, s_init2, s_fin, s_reg, s_unreg, s_start, s_stop, s_flush, s_ev, s_sub, s_enc, s_acq, s_rel, s_props) \
    HLE_FORCE(s_work, "the host decoder needs no work memory", Result, h_cv_##tag##work, uint64_t *size, uint64_t *align, uint64_t bs, uint16_t w, uint16_t h) { (void)bs; (void)w; (void)h; *size = 0x1000; *align = 0x1000; return 0; } \
    HLE_FORCE(s_init, "decoded on the host", Result, h_cv_##tag##init, void *t, void *work, uint64_t wsize, uint64_t bs, uint16_t w, uint16_t h, uint32_t refs, uint32_t in, uint32_t out) { (void)work; (void)wsize; (void)bs; (void)refs; return cv_dec_init(t, codec, w, h, in, out, NULL, 0); } \
    HLE_FORCE(s_init2, "decoded on the host", Result, h_cv_##tag##init2, void *t, uint64_t bs, uint16_t w, uint16_t h, uint32_t refs, uint32_t in, uint32_t out) { (void)bs; (void)refs; return cv_dec_init(t, codec, w, h, in, out, NULL, 0); } \
    HLE_FORCE(s_fin, "frees the host decoder", Result, h_cv_##tag##fin, void *t) { return cv_dec_fin(t); } \
    HLE_FORCE(s_reg, "a buffer of the host decoder, acquirable at once", Result, h_cv_##tag##reg, void *t, uint32_t i, void *pool, uint32_t pidx) { return cv_dec_reg(t, i, pool, pidx); } \
    HLE_FORCE(s_unreg, "the buffer leaves the host decoder", Result, h_cv_##tag##unreg, void *t, uint32_t i) { return cv_dec_unreg(t, i); } \
    HLE_FORCE(s_start, "the host decoder decodes on submission", Result, h_cv_##tag##start, void *t) { (void)t; return 0; } \
    HLE_FORCE(s_stop, "drops undelivered pictures", Result, h_cv_##tag##stop, void *t) { return cv_dec_reset(t); } \
    HLE_FORCE(s_flush, "drops undelivered pictures", Result, h_cv_##tag##flush, void *t) { return cv_dec_reset(t); } \
    HLE_FORCE(s_ev, "signalled while the host decoder takes packets", Result, h_cv_##tag##event, void **out, void *t) { return cv_dec_event(out, t); } \
    HLE_FORCE(s_sub, "decodes the packet on the submitting thread", Result, h_cv_##tag##submit, uint64_t *out, void *t, const void *data, uint64_t size, int64_t ts, bool eos) { return cv_dec_submit(out, t, data, size, ts, eos); } \
    HLE_FORCE(s_enc, "protected streams are not served", Result, h_cv_##tag##enc, void) { return cv_dec_encrypted(); } \
    HLE_FORCE(s_acq, "the oldest acquirable buffer and its sync handle", Result, h_cv_##tag##acq, uint32_t *idx, cv_sync *sync, void *t) { return cv_dec_acq(idx, sync, t); } \
    HLE_FORCE(s_rel, "the buffer waits for the next picture and is acquirable again", void, h_cv_##tag##rel, void *t, uint32_t i, cv_sync sync) { cv_dec_rel(t, i, sync); } \
    HLE_FORCE(s_props, "the filled buffer's picture size, time and end of stream", Result, h_cv_##tag##props, uint16_t *w, uint16_t *h, uint16_t *cw, uint16_t *ch, int64_t *ts, bool *eos, void *t, uint32_t i) { return cv_dec_props(w, h, cw, ch, ts, eos, t, i); }
CV_FAMILY(vp9, CV_VP9, "_ZN2nn5codec29CalculateVp9DecoderWorkBufferEPmS1_mtt",
    "_ZN2nn5codec20InitializeVp9DecoderEPNS0_14Vp9DecoderTypeEPvmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatE",
    "_ZN2nn5codec20InitializeVp9DecoderEPNS0_14Vp9DecoderTypeEmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatE",
    "_ZN2nn5codec18FinalizeVp9DecoderEPNS0_14Vp9DecoderTypeE", "_ZN2nn5codec24RegisterVp9DecoderBufferEPNS0_14Vp9DecoderTypeEjPNS0_25VideoDecoderImagePoolTypeEj",
    "_ZN2nn5codec26UnregisterVp9DecoderBufferEPNS0_14Vp9DecoderTypeEj", "_ZN2nn5codec15StartVp9DecoderEPNS0_14Vp9DecoderTypeE", "_ZN2nn5codec14StopVp9DecoderEPNS0_14Vp9DecoderTypeE",
    "_ZN2nn5codec15FlushVp9DecoderEPNS0_14Vp9DecoderTypeE", "_ZN2nn5codec33GetVp9DecoderSubmissionReadyEventEPPNS_2os15SystemEventTypeEPNS0_14Vp9DecoderTypeE",
    "_ZN2nn5codec27SubmitVp9DecoderInputBufferEPmPNS0_14Vp9DecoderTypeEPKvmNS_8TimeSpanEb",
    "_ZN2nn5codec36SubmitEncryptedVp9DecoderInputBufferEPmPNS0_14Vp9DecoderTypeEPKNS0_18EncryptionPropertyEPKvmNS_8TimeSpanEb",
    "_ZN2nn5codec29AcquireVp9DecoderOutputBufferEPjPNS_2gs16NativeSyncHandleEPNS0_14Vp9DecoderTypeE", "_ZN2nn5codec29ReleaseVp9DecoderOutputBufferEPNS0_14Vp9DecoderTypeEjNS_2gs16NativeSyncHandleE",
    "_ZN2nn5codec35GetVp9DecoderOutputBufferPropertiesEPtS1_S1_S1_PNS_8TimeSpanEPbPNS0_14Vp9DecoderTypeEj")
CV_FAMILY(vp8, CV_VP8, "_ZN2nn5codec29CalculateVp8DecoderWorkBufferEPmS1_mtt",
    "_ZN2nn5codec20InitializeVp8DecoderEPNS0_14Vp8DecoderTypeEPvmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatE",
    "_ZN2nn5codec20InitializeVp8DecoderEPNS0_14Vp8DecoderTypeEmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatE",
    "_ZN2nn5codec18FinalizeVp8DecoderEPNS0_14Vp8DecoderTypeE", "_ZN2nn5codec24RegisterVp8DecoderBufferEPNS0_14Vp8DecoderTypeEjPNS0_25VideoDecoderImagePoolTypeEj",
    "_ZN2nn5codec26UnregisterVp8DecoderBufferEPNS0_14Vp8DecoderTypeEj", "_ZN2nn5codec15StartVp8DecoderEPNS0_14Vp8DecoderTypeE", "_ZN2nn5codec14StopVp8DecoderEPNS0_14Vp8DecoderTypeE",
    "_ZN2nn5codec15FlushVp8DecoderEPNS0_14Vp8DecoderTypeE", "_ZN2nn5codec33GetVp8DecoderSubmissionReadyEventEPPNS_2os15SystemEventTypeEPNS0_14Vp8DecoderTypeE",
    "_ZN2nn5codec27SubmitVp8DecoderInputBufferEPmPNS0_14Vp8DecoderTypeEPKvmNS_8TimeSpanEb",
    "_ZN2nn5codec36SubmitEncryptedVp8DecoderInputBufferEPmPNS0_14Vp8DecoderTypeEPKNS0_18EncryptionPropertyEPKvmNS_8TimeSpanEb",
    "_ZN2nn5codec29AcquireVp8DecoderOutputBufferEPjPNS_2gs16NativeSyncHandleEPNS0_14Vp8DecoderTypeE", "_ZN2nn5codec29ReleaseVp8DecoderOutputBufferEPNS0_14Vp8DecoderTypeEjNS_2gs16NativeSyncHandleE",
    "_ZN2nn5codec35GetVp8DecoderOutputBufferPropertiesEPtS1_S1_S1_PNS_8TimeSpanEPbPNS0_14Vp8DecoderTypeEj")
// H.264: its Initialize overloads also take the stream's parameter sets (SPS / PPS, a pointer and a size), one of them an output mode too
CV_FAMILY(h264, CV_H264, "_ZN2nn5codec30CalculateH264DecoderWorkBufferEPmS1_mtt",
    "_ZN2nn5codec21InitializeH264DecoderEPNS0_15H264DecoderTypeEPvmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatEPKvm",
    "_ZN2nn5codec21InitializeH264DecoderEPNS0_15H264DecoderTypeEmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatEPKvm",
    "_ZN2nn5codec19FinalizeH264DecoderEPNS0_15H264DecoderTypeE", "_ZN2nn5codec25RegisterH264DecoderBufferEPNS0_15H264DecoderTypeEjPNS0_25VideoDecoderImagePoolTypeEj",
    "_ZN2nn5codec27UnregisterH264DecoderBufferEPNS0_15H264DecoderTypeEj", "_ZN2nn5codec16StartH264DecoderEPNS0_15H264DecoderTypeE", "_ZN2nn5codec15StopH264DecoderEPNS0_15H264DecoderTypeE",
    "_ZN2nn5codec16FlushH264DecoderEPNS0_15H264DecoderTypeE", "_ZN2nn5codec34GetH264DecoderSubmissionReadyEventEPPNS_2os15SystemEventTypeEPNS0_15H264DecoderTypeE",
    "_ZN2nn5codec28SubmitH264DecoderInputBufferEPmPNS0_15H264DecoderTypeEPKvmNS_8TimeSpanEb",
    "_ZN2nn5codec37SubmitEncryptedH264DecoderInputBufferEPmPNS0_15H264DecoderTypeEPKNS0_18EncryptionPropertyEPKvmNS_8TimeSpanEb",
    "_ZN2nn5codec30AcquireH264DecoderOutputBufferEPjPNS_2gs16NativeSyncHandleEPNS0_15H264DecoderTypeE", "_ZN2nn5codec30ReleaseH264DecoderOutputBufferEPNS0_15H264DecoderTypeEjNS_2gs16NativeSyncHandleE",
    "_ZN2nn5codec36GetH264DecoderOutputBufferPropertiesEPtS1_S1_S1_PNS_8TimeSpanEPbPNS0_15H264DecoderTypeEj")
CV_FAMILY(av1, CV_AV1, "_ZN2nn5codec29CalculateAv1DecoderWorkBufferEPmS1_mtt",
    "_ZN2nn5codec20InitializeAv1DecoderEPNS0_14Av1DecoderTypeEPvmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatE",
    "_ZN2nn5codec20InitializeAv1DecoderEPNS0_14Av1DecoderTypeEmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatE",
    "_ZN2nn5codec18FinalizeAv1DecoderEPNS0_14Av1DecoderTypeE", "_ZN2nn5codec24RegisterAv1DecoderBufferEPNS0_14Av1DecoderTypeEjPNS0_25VideoDecoderImagePoolTypeEj",
    "_ZN2nn5codec26UnregisterAv1DecoderBufferEPNS0_14Av1DecoderTypeEj", "_ZN2nn5codec15StartAv1DecoderEPNS0_14Av1DecoderTypeE", "_ZN2nn5codec14StopAv1DecoderEPNS0_14Av1DecoderTypeE",
    "_ZN2nn5codec15FlushAv1DecoderEPNS0_14Av1DecoderTypeE", "_ZN2nn5codec33GetAv1DecoderSubmissionReadyEventEPPNS_2os15SystemEventTypeEPNS0_14Av1DecoderTypeE",
    "_ZN2nn5codec27SubmitAv1DecoderInputBufferEPmPNS0_14Av1DecoderTypeEPKvmNS_8TimeSpanEb",
    "_ZN2nn5codec36SubmitEncryptedAv1DecoderInputBufferEPmPNS0_14Av1DecoderTypeEPKNS0_18EncryptionPropertyEPKvmNS_8TimeSpanEb",
    "_ZN2nn5codec29AcquireAv1DecoderOutputBufferEPjPNS_2gs16NativeSyncHandleEPNS0_14Av1DecoderTypeE", "_ZN2nn5codec29ReleaseAv1DecoderOutputBufferEPNS0_14Av1DecoderTypeEjNS_2gs16NativeSyncHandleE",
    "_ZN2nn5codec35GetAv1DecoderOutputBufferPropertiesEPtS1_S1_S1_PNS_8TimeSpanEPbPNS0_14Av1DecoderTypeEj")
#undef CV_FAMILY
HLE_FORCE("_ZN2nn5codec21InitializeH264DecoderEPNS0_15H264DecoderTypeEPvmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeEPKvm",
          "decoded with VideoToolbox on the host", Result, h_cv_h264init3, void *t, void *work, uint64_t wsize, uint64_t bs, uint64_t w, uint64_t h, uint32_t refs, uint32_t in, uint64_t out,
          uint64_t mode, const void *csd, uint64_t csd_n) {   // past the registers: 8-byte slots, as the SDK 22 forms below
    (void)work; (void)wsize; (void)bs; (void)refs; (void)mode; return cv_dec_init(t, CV_H264, (uint16_t)w, (uint16_t)h, in, (uint32_t)out, csd, csd_n);
}
// SDK 22's forms of the entry points (the movie module's), for every codec. ByTransferMemory has ten arguments and ByRemoteMemory eight (H.264 two more: the parameter sets): past the eight registers
// the guest's AAPCS64 gives each its own 8-byte slot where Apple's arm64 packs them, so those are declared 64 bits wide; the 16-bit size arguments are masked, as the guest does not extend them
#define CV_SDK22(tag, codec, s_tm, s_rm, s_ev) \
    HLE_FORCE(s_tm, "decoded on the host", Result, h_cv_##tag##tm, void *t, void *work, uint64_t wsize, uint64_t bs, uint64_t w, uint64_t h, uint32_t refs, uint32_t in, uint64_t out, uint64_t mode) { (void)work; (void)wsize; (void)bs; (void)refs; (void)mode; return cv_dec_init(t, codec, (uint16_t)w, (uint16_t)h, in, (uint32_t)out, NULL, 0); } \
    HLE_FORCE(s_rm, "decoded on the host", Result, h_cv_##tag##rm, void *t, uint64_t bs, uint64_t w, uint64_t h, uint32_t refs, uint32_t in, uint32_t out, uint32_t mode) { (void)bs; (void)refs; (void)mode; return cv_dec_init(t, codec, (uint16_t)w, (uint16_t)h, in, out, NULL, 0); } \
    HLE_FORCE(s_ev, "signalled while the host decoder takes packets", void *, h_cv_##tag##ev22, void *t) { cv_dec *d = cv_of(t); return d ? d->event : NULL; }
CV_SDK22(vp9, CV_VP9, "_ZN2nn5codec36InitializeVp9DecoderByTransferMemoryEPNS0_14Vp9DecoderTypeEPvmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeE",
    "_ZN2nn5codec34InitializeVp9DecoderByRemoteMemoryEPNS0_14Vp9DecoderTypeEmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeE",
    "_ZN2nn5codec33GetVp9DecoderSubmissionReadyEventEPNS0_14Vp9DecoderTypeE")
CV_SDK22(vp8, CV_VP8, "_ZN2nn5codec36InitializeVp8DecoderByTransferMemoryEPNS0_14Vp8DecoderTypeEPvmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeE",
    "_ZN2nn5codec34InitializeVp8DecoderByRemoteMemoryEPNS0_14Vp8DecoderTypeEmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeE",
    "_ZN2nn5codec33GetVp8DecoderSubmissionReadyEventEPNS0_14Vp8DecoderTypeE")
CV_SDK22(av1, CV_AV1, "_ZN2nn5codec36InitializeAv1DecoderByTransferMemoryEPNS0_14Av1DecoderTypeEPvmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeE",
    "_ZN2nn5codec34InitializeAv1DecoderByRemoteMemoryEPNS0_14Av1DecoderTypeEmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeE",
    "_ZN2nn5codec33GetAv1DecoderSubmissionReadyEventEPNS0_14Av1DecoderTypeE")
#undef CV_SDK22
HLE_FORCE("_ZN2nn5codec37InitializeH264DecoderByTransferMemoryEPNS0_15H264DecoderTypeEPvmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeEPKvm",
          "decoded with VideoToolbox on the host", Result, h_cv_h264tm, void *t, void *work, uint64_t wsize, uint64_t bs, uint64_t w, uint64_t h, uint32_t refs, uint32_t in, uint64_t out, uint64_t mode, const void *csd, uint64_t csd_n) {
    (void)work; (void)wsize; (void)bs; (void)refs; (void)mode; return cv_dec_init(t, CV_H264, (uint16_t)w, (uint16_t)h, in, (uint32_t)out, csd, csd_n);
}
HLE_FORCE("_ZN2nn5codec35InitializeH264DecoderByRemoteMemoryEPNS0_15H264DecoderTypeEmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeEPKvm",
          "decoded with VideoToolbox on the host", Result, h_cv_h264rm, void *t, uint64_t bs, uint64_t w, uint64_t h, uint32_t refs, uint32_t in, uint32_t out, uint32_t mode, const void *csd, uint64_t csd_n) {
    (void)bs; (void)refs; (void)mode; return cv_dec_init(t, CV_H264, (uint16_t)w, (uint16_t)h, in, out, csd, csd_n);
}
HLE_FORCE("_ZN2nn5codec34GetH264DecoderSubmissionReadyEventEPNS0_15H264DecoderTypeE", "signalled while the host decoder takes packets", void *, h_cv_h264ev22, void *t) { cv_dec *d = cv_of(t); return d ? d->event : NULL; }
// The host's sync handles signal when their buffer holds the picture of that release; a handle the host did not make (another nn::gs user) reads as signalled
static bool sync_reached(const cv_sync *s) {
    if (s->magic != CV_SYNC) { static int once; if (!once++ && (s->magic || s->dec)) LOG("[RED] nn::codec: a sync handle the host did not make (%#x): signalled", s->magic); return true; }
    if (!s->gen) return true;
    pthread_mutex_lock(&g_cv_mu);   // held until the decoder's lock is: FinalizeVp9Decoder cannot free it in between
    cv_dec *d = s->dec && s->dec <= 16 ? g_cv[s->dec - 1] : NULL;
    bool r = true;
    if (d && s->idx < CV_BUFS) { pthread_mutex_lock(&d->mu); r = d->buf[s->idx].filled && d->buf[s->idx].filled_gen >= s->gen; pthread_mutex_unlock(&d->mu); }
    pthread_mutex_unlock(&g_cv_mu);
    return r;
}
HLE_FORCE("_ZN2nn2gs17TryWaitNativeSyncERKNS0_16NativeSyncHandleE", "decoder sync handles are the host's (hle_codec_video.c)", bool, h_gs_trywait, const cv_sync *s) {
    return sync_reached(s);
}
HLE_FORCE("_ZN2nn6syncpt3sdk20CreateSyncPointEventEPNS1_18SyncPointEventTypeE", "a host-bound SystemEvent inside the event type", Result, h_spt_create, void *e) {
    memset(e, 0, 0x58); hle_bind_system_event(e, 0); SPT(e)->magic = CV_SYNC;
    return 0;
}
HLE_FORCE("_ZN2nn6syncpt3sdk25InitializeMultiWaitHolderEPNS_2os19MultiWaitHolderTypeEPNS1_18SyncPointEventTypeE", "links the event's SystemEvent", void, h_spt_holder, void *holder, void *e) {
    static void (*init)(void *, void *); if (!init && !(init = guest_sym("_ZN2nn2os25InitializeMultiWaitHolderEPNS0_19MultiWaitHolderTypeEPNS0_15SystemEventTypeE"))) die("nn::os::InitializeMultiWaitHolder(SystemEventType) missing");
    init(holder, e);
}
HLE_FORCE("_ZN2nn6syncpt3sdk21RequestSyncPointEventEPNS1_18SyncPointEventTypeERKNS1_15SyncPointHandleE", "signalled when the decoder fills the awaited buffer",
          Result, h_spt_request, void *e, const cv_sync *s) {
    spt_unarm(e); hle_clear_system_event(e);
    SPT(e)->want = *s; SPT(e)->armed = 1;
    pthread_mutex_lock(&g_spt_mu); int k = 0; while (k < 64 && g_spt[k]) k++; if (k < 64) g_spt[k] = e; pthread_mutex_unlock(&g_spt_mu);
    if (k == 64) { LOG("[RED] nn::syncpt: more than 64 armed events: signalled at once"); hle_signal_system_event(e); return 0; }
    if (sync_reached(s)) { spt_unarm(e); hle_signal_system_event(e); }
    return 0;
}
HLE_FORCE("_ZN2nn6syncpt3sdk20CancelSyncPointEventEPNS1_18SyncPointEventTypeE", "disarms the event", void, h_spt_cancel, void *e) { spt_unarm(e); hle_clear_system_event(e); }
HLE_FORCE("_ZN2nn6syncpt3sdk21DestroySyncPointEventEPNS1_18SyncPointEventTypeE", "destroys the event's SystemEvent", void, h_spt_destroy, void *e) {
    spt_unarm(e);
    static void (*destroy)(void *); if (!destroy && !(destroy = guest_sym("_ZN2nn2os18DestroySystemEventEPNS0_15SystemEventTypeE"))) die("nn::os::DestroySystemEvent missing");
    destroy(e); SPT(e)->magic = 0;
}
