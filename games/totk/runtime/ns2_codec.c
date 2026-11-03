/* The Switch 2's nn::codec video decoders and sync point events, served on the host (GAME_NS2; docs/ns2-port.md). Ported from
   seedds/nxnative dm/host/hle_codec_video.c (5b2d4606 and later, MIT) with its SDK protocol notes; here VP8 and VP9 through
   libvpx (as movie_host.c), not H.264 (nxnative: VideoToolbox) or AV1 (nowhere), whose Initialize fails.
   The SDK sends every call to a decoder service the host does not serve: Donkey Kong Bananza starts one for its guide movies
   after its first punch. The protocol (sdk 20.5.19 Ounce):
   - the client makes one image pool per output picture and registers each with the decoder under an index. Registering puts
     the index in the decoder's queue of acquirable buffers (8 deep), with a sync handle already signalled.
   - Acquire takes the oldest index and its sync handle (description 3604 when the queue is empty); Release gives the buffer to
     the decoder to fill and puts it back in the queue at once, with a new sync handle that signals when a picture is in it.
     The client polls nn::gs::TryWaitNativeSync (or waits on a sync point event), reads the picture's properties (3604 until
     it is filled) and the picture (ReadDecoderOutputImageRgba / Nv12, the CPU route Bananza's player takes).
   - Submit takes one packet with its time and an end-of-stream flag; 3603 when the decoder cannot take more (the client
     retries after the submission-ready event).
   Pictures go into the released buffers in release order. Packets decode on the submitting thread; pictures with no released
   buffer wait in a short queue, and a full queue refuses packets. The SDK's opaque types hold a host pointer at +0; the
   decoder's event lives in its type at +0x100. Not served: encrypted input, native image handles (the GPU route). */
#include "runtime.h"
#include "thread_compat.h"
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vpx/vpx_decoder.h"
#include "vpx/vp8dx.h"

typedef uint32_t Result;
void hle_signal_system_event(void *event);   /* runtime/threads.c */
#undef LOG
#define LOG(...) do { fprintf(stderr, "[ns2-codec] " __VA_ARGS__); fputc('\n', stderr); } while (0)

#define CV_MAGIC 0x56436d44u        /* 'DmCV' */
#define CV_SYNC 0x53436d44u         /* 'DmCS': a sync handle the host made (decoder id, index, generation) */
#define CV_BUFS 32
#define CV_HOLD 4                   /* decoded pictures waiting for a released buffer */
#define CV_EVENT_OFF 0x100          /* the submission-ready SystemEventType inside the decoder type */
#define CV_R(desc) ((Result)(0x6f | ((desc) << 9)))   /* nn::codec results (module 111) */
#define CV_BUSY CV_R(3603)
#define CV_NONE CV_R(3604)

static void clear_event(void *e) {
    static void (*clear)(void *);
    if (!clear && !(clear = guest_sym("_ZN2nn2os16ClearSystemEventEPNS0_15SystemEventTypeE"))) runtime_fail("nn::os::ClearSystemEvent missing");
    clear(e);
}
static void destroy_event(void *e) {
    static void (*destroy)(void *);
    if (!destroy && !(destroy = guest_sym("_ZN2nn2os18DestroySystemEventEPNS0_15SystemEventTypeE"))) runtime_fail("nn::os::DestroySystemEvent missing");
    destroy(e);
}

/* ---------- YUV (libvpx's I420) to what the game reads; from nxnative hle_movie.c md_nv12 / md_rgba (MIT) ---------- */
static void yuv_nv12(const vpx_image_t *im, uint8_t *dst, int w, int h) {
    int cw = (int)im->d_w < w ? (int)im->d_w : w, ch = (int)im->d_h < h ? (int)im->d_h : h;
    if (cw < w || ch < h) { memset(dst, 16, (size_t)w * h); memset(dst + (size_t)w * h, 128, (size_t)w * h / 2); }
    for (int y = 0; y < ch; y++) memcpy(dst + (size_t)y * w, im->planes[0] + (size_t)y * im->stride[0], (size_t)cw);
    int cw2 = (cw + 1) / 2, ch2 = (ch + 1) / 2;
    for (int y = 0; y < ch2 && y < h / 2; y++) {
        const uint8_t *u = im->planes[1] + (size_t)y * im->stride[1], *v = im->planes[2] + (size_t)y * im->stride[2];
        uint8_t *o = dst + (size_t)w * h + (size_t)y * w;
        for (int x = 0; x < cw2 && 2 * x + 1 < w; x++) { o[2 * x] = u[x]; o[2 * x + 1] = v[x]; }
    }
}
/* By the colour space and range the picture declares (VP9 colour config; undeclared: BT.709 from 720 lines up, else BT.601). */
static void yuv_rgba(const vpx_image_t *im, uint8_t *dst, int w, int h) {
    int full = im->range == VPX_CR_FULL_RANGE, b709 = im->cs == VPX_CS_BT_709 || (im->cs == VPX_CS_UNKNOWN && h >= 720);
    double ky = full ? 1.0 : 255.0 / 219.0, kc = full ? 1.0 : 255.0 / 224.0;
    double rv = b709 ? 1.5748 : 1.402, gu = b709 ? 0.1873 : 0.3441, gv = b709 ? 0.4681 : 0.7141, bu = b709 ? 1.8556 : 1.772;
    int Y = (int)(ky * 65536), RV = (int)(rv * kc * 65536), GU = (int)(gu * kc * 65536), GV = (int)(gv * kc * 65536), BU = (int)(bu * kc * 65536), y0 = full ? 0 : 16;
    int cw = (int)im->d_w < w ? (int)im->d_w : w, ch = (int)im->d_h < h ? (int)im->d_h : h;
    if (cw < w || ch < h) for (size_t i = 0; i < (size_t)w * h; i++) ((uint32_t *)(void *)dst)[i] = 0xff000000u;
#define CL(v) ((v) < 0 ? 0 : (v) > 255 ? 255 : (v))
    for (int y = 0; y < ch; y++) {
        const uint8_t *py = im->planes[0] + (size_t)y * im->stride[0], *pu = im->planes[1] + (size_t)(y >> 1) * im->stride[1], *pv = im->planes[2] + (size_t)(y >> 1) * im->stride[2];
        uint8_t *o = dst + (size_t)y * w * 4;
        for (int x = 0; x < cw; x++) {
            int l = (py[x] - y0) * Y, u = pu[x >> 1] - 128, v = pv[x >> 1] - 128;
            int r = (l + RV * v + 32768) >> 16, g = (l - GU * u - GV * v + 32768) >> 16, b = (l + BU * u + 32768) >> 16;
            o[4 * x] = (uint8_t)CL(r); o[4 * x + 1] = (uint8_t)CL(g); o[4 * x + 2] = (uint8_t)CL(b); o[4 * x + 3] = 255;
        }
    }
#undef CL
}

typedef struct { uint16_t w, h; uint32_t fmt, mode, magic; uint32_t pad[2]; } cv_pref;   /* VideoDecoderImagePreference as the host fills it (returned in x8) */
typedef struct { uint32_t magic; uint16_t w, h; uint32_t fmt, count; vpx_image_t **img; } cv_pool;
typedef struct { uint32_t magic, dec, idx, gen; } cv_sync;   /* nn::gs::NativeSyncHandle (16 bytes) */
enum { CV_VP8, CV_VP9 };
typedef struct cv_buf { cv_pool *pool; uint32_t pidx; int reg, filled; uint32_t gen, filled_gen; uint16_t w, h; int64_t ts; uint8_t eos; } cv_buf;
typedef struct {
    uint32_t magic, id; pthread_mutex_t mu; vpx_codec_ctx_t vpx; int vpx_ok; void *event; uint64_t submits, pictures;
    int codec; uint16_t w, h;
    cv_buf buf[CV_BUFS];
    uint32_t fifo[8], fhead, fcount;          /* acquirable indices */
    uint32_t pend[CV_BUFS], phead, pcount;    /* released, waiting for a picture */
    vpx_image_t *held[CV_HOLD]; int64_t held_ts[CV_HOLD]; uint8_t held_eos[CV_HOLD]; int nheld;
    int eos_pending;
} cv_dec;
static const char *const k_name[] = {"VP8", "VP9"};

static cv_dec *g_cv[16];
static pthread_mutex_t g_cv_mu = PTHREAD_MUTEX_INITIALIZER;   /* lock order: g_cv_mu, then a decoder's mu, then g_spt_mu */
static cv_dec *cv_of(void *t) { cv_dec *d = t ? *(cv_dec **)t : NULL; return d && d->magic == CV_MAGIC ? d : NULL; }
static cv_pool *pool_of(const void *t) { cv_pool *p = t ? *(cv_pool *const *)t : NULL; return p && p->magic == CV_MAGIC ? p : NULL; }

static vpx_image_t *img_copy(const vpx_image_t *im) {   /* an owned I420 copy (libvpx reuses its buffers at the next decode) */
    vpx_image_t *c = vpx_img_alloc(NULL, VPX_IMG_FMT_I420, im->d_w, im->d_h, 1);
    if (!c) return NULL;
    c->cs = im->cs; c->range = im->range;
    for (int p = 0; p < 3; p++) {
        unsigned w = p ? (im->d_w + 1) / 2 : im->d_w, h = p ? (im->d_h + 1) / 2 : im->d_h;
        for (unsigned y = 0; y < h; y++) memcpy(c->planes[p] + (size_t)y * c->stride[p], im->planes[p] + (size_t)y * im->stride[p], w);
    }
    return c;
}

/* Sync point events (nn::syncpt::sdk): the player's worker waits on one (in a MultiWait) for the sync handle of the picture it
   expects. Their library is set up by the graphics initialisation the host replaces, so CreateSyncPointEvent jumped through
   its null table. The event type holds a SystemEvent the host binds at +0 (0x30 bytes) and the awaited handle at +0x40. */
typedef struct { uint32_t magic, armed; cv_sync want; } cv_spt;
#define SPT(e) ((cv_spt *)(void *)((uint8_t *)(e) + 0x40))
static void *g_spt[64];
static pthread_mutex_t g_spt_mu = PTHREAD_MUTEX_INITIALIZER;
static bool sync_reached(const cv_sync *s);
static void spt_fire(cv_dec *d, uint32_t i) {   /* buffer i of d filled: signal the events waiting for it (decoder lock held) */
    pthread_mutex_lock(&g_spt_mu);
    for (int k = 0; k < 64; k++) { void *e = g_spt[k]; cv_spt *t = e ? SPT(e) : NULL;
        if (t && t->want.dec == d->id && t->want.idx == i && d->buf[i].filled_gen >= t->want.gen) { t->armed = 0; g_spt[k] = NULL; hle_signal_system_event(e); } }
    pthread_mutex_unlock(&g_spt_mu);
}
static void spt_unarm(void *e) { pthread_mutex_lock(&g_spt_mu); for (int k = 0; k < 64; k++) if (g_spt[k] == e) g_spt[k] = NULL; pthread_mutex_unlock(&g_spt_mu); SPT(e)->armed = 0; }

/* Pictures and end of stream into the released buffers, in order (decoder lock held). */
static void drain(cv_dec *d) {
    while (d->pcount && (d->nheld || d->eos_pending)) {
        uint32_t i = d->pend[d->phead]; d->phead = (d->phead + 1) % CV_BUFS; d->pcount--;
        cv_buf *b = &d->buf[i];
        if (d->nheld) {
            vpx_image_t *im = d->held[0]; int64_t ts = d->held_ts[0]; uint8_t eos = d->held_eos[0];
            memmove(d->held, d->held + 1, sizeof d->held[0] * (CV_HOLD - 1)); memmove(d->held_ts, d->held_ts + 1, sizeof d->held_ts[0] * (CV_HOLD - 1));
            memmove(d->held_eos, d->held_eos + 1, CV_HOLD - 1); d->nheld--;
            b->w = (uint16_t)im->d_w; b->h = (uint16_t)im->d_h; b->ts = ts; b->eos = eos;
            if (b->pool && b->pidx < b->pool->count) { if (b->pool->img[b->pidx]) vpx_img_free(b->pool->img[b->pidx]); b->pool->img[b->pidx] = im; } else vpx_img_free(im);
            if (!d->pictures++) LOG("%s decoder %u: first picture %ux%u into buffer %u", k_name[d->codec], d->id, b->w, b->h, i);
        } else { b->w = b->h = 0; b->ts = 0; b->eos = 1; d->eos_pending = 0; }
        if (b->eos) LOG("%s decoder %u: end of stream after %llu pictures", k_name[d->codec], d->id, (unsigned long long)d->pictures);
        b->filled = 1; b->filled_gen = b->gen;
        spt_fire(d, i);
    }
    if (d->event) { if (d->nheld < CV_HOLD) hle_signal_system_event(d->event); else clear_event(d->event); }
}

static Result cv_init(void) { return 0; }
static cv_pref cv_preference(uint16_t w, uint16_t h, uint32_t fmt, uint32_t mode) { return (cv_pref){w, h, fmt, mode, CV_MAGIC, {0, 0}}; }
static Result cv_pool_size(uint64_t *size, uint64_t *align, const cv_pref *p, uint32_t n) {
    *size = (((uint64_t)p->w * p->h * 4 * (n ? n : 1)) + 0xfff) & ~0xfffull; *align = 0x1000;   /* room for RGBA pictures; the host keeps its own copies */
    return 0;
}
static Result cv_pool_init(void *t, void *mem, uint64_t size, const cv_pref *p, uint32_t n, uint32_t mode) {
    (void)mem; (void)size; (void)mode;
    cv_pool *o = calloc(1, sizeof *o); if (!o) runtime_fail("nn::codec image pool memory");
    o->magic = CV_MAGIC; o->w = p->w; o->h = p->h; o->fmt = p->fmt; o->count = n ? n : 1; o->img = calloc(o->count, sizeof *o->img);
    if (!o->img) runtime_fail("nn::codec image pool memory");
    *(cv_pool **)t = o;
    static int once; if (!once++) LOG("image pool %ux%u format %u, %u picture(s): the host keeps the decoded pictures", p->w, p->h, p->fmt, o->count);
    return 0;
}
static void cv_pool_fin(void *t) {
    cv_pool *p = pool_of(t); if (!p) return;
    pthread_mutex_lock(&g_cv_mu);   /* a decoder still holding a buffer of this pool keeps it, with no pool to write into */
    for (int k = 0; k < 16; k++) if (g_cv[k]) { pthread_mutex_lock(&g_cv[k]->mu); for (int i = 0; i < CV_BUFS; i++) if (g_cv[k]->buf[i].pool == p) g_cv[k]->buf[i].pool = NULL; pthread_mutex_unlock(&g_cv[k]->mu); }
    pthread_mutex_unlock(&g_cv_mu);
    for (uint32_t i = 0; i < p->count; i++) if (p->img[i]) vpx_img_free(p->img[i]);
    free(p->img); p->magic = 0; free(p); *(cv_pool **)t = NULL;
}
static uint32_t cv_native(const void *t, uint32_t i) {
    (void)t; (void)i; static int once; if (!once++) LOG("[RED] nn::codec::GetDecoderImagePoolNativeImageHandle: the GPU route of decoded pictures is not served (0)");
    return 0;
}
static Result cv_read_rgba(const void *t, uint32_t i, uint16_t x, uint16_t y, uint16_t w, uint16_t h, void *dst, uint64_t stride, uint64_t size) {
    cv_pool *p = pool_of(t); const vpx_image_t *im = p && i < p->count ? p->img[i] : NULL;
    if (!im) { memset(dst, 0, size); return 0; }
    if (x || y || stride != (uint64_t)w * 4 || size < (uint64_t)w * h * 4) { static int once; if (!once++) LOG("[RED] ReadDecoderOutputImageRgba: region %u,%u stride %llu: only whole tight pictures are served", x, y, (unsigned long long)stride); return 0; }
    yuv_rgba(im, dst, w, h);
    return 0;
}
static Result cv_read_nv12(const void *t, uint32_t i, uint16_t x, uint16_t y, uint16_t w, uint16_t h, void *ydst, uint64_t ystride, uint64_t ysize, void *uvdst, uint64_t uvstride, uint64_t uvsize) {
    cv_pool *p = pool_of(t); const vpx_image_t *im = p && i < p->count ? p->img[i] : NULL;
    if (x || y || ystride != w || uvstride != w || ysize < (uint64_t)w * h || uvsize < (uint64_t)w * h / 2) { static int once; if (!once++) LOG("[RED] ReadDecoderOutputImageNv12: region %u,%u strides %llu / %llu: only whole tight pictures are served", x, y, (unsigned long long)ystride, (unsigned long long)uvstride); return 0; }
    if (!im) { memset(ydst, 16, ysize); memset(uvdst, 128, uvsize); return 0; }
    if ((uint8_t *)uvdst == (uint8_t *)ydst + (size_t)w * h) { yuv_nv12(im, ydst, w, h); return 0; }
    uint8_t *tmp = malloc((size_t)w * h * 3 / 2); if (!tmp) runtime_fail("nn::codec readback memory");
    yuv_nv12(im, tmp, w, h);
    memcpy(ydst, tmp, (size_t)w * h); memcpy(uvdst, tmp + (size_t)w * h, (size_t)w * h / 2); free(tmp);
    return 0;
}

static void cv_open(cv_dec *d) {   /* (re)creates the codec state: a new stream starts at the next key frame */
    if (d->vpx_ok) vpx_codec_destroy(&d->vpx);
    vpx_codec_dec_cfg_t cfg = {.threads = 2, .w = d->w, .h = d->h};
    d->vpx_ok = vpx_codec_dec_init(&d->vpx, d->codec == CV_VP8 ? vpx_codec_vp8_dx() : vpx_codec_vp9_dx(), &cfg, 0) == VPX_CODEC_OK;
    if (!d->vpx_ok) LOG("[RED] %s decoder: libvpx init failed", k_name[d->codec]);
}
static Result cv_dec_init(void *t, int codec, uint16_t w, uint16_t h, uint32_t in, uint32_t out) {
    cv_dec *d = calloc(1, sizeof *d); if (!d) runtime_fail("nn::codec decoder memory");
    d->magic = CV_MAGIC; d->codec = codec; d->w = w; d->h = h; pthread_mutex_init(&d->mu, NULL);
    cv_open(d);
    pthread_mutex_lock(&g_cv_mu);
    for (int i = 0; i < 16; i++) if (!g_cv[i]) { g_cv[i] = d; d->id = (uint32_t)i + 1; break; }
    pthread_mutex_unlock(&g_cv_mu);
    if (!d->id) runtime_fail("nn::codec: more than 16 video decoders");
    d->event = (uint8_t *)t + CV_EVENT_OFF; hle_bind_system_event(d->event, 0); hle_signal_system_event(d->event);
    *(cv_dec **)t = d;
    LOG("%s decoder %u: %ux%u, input format %u, output format %u (libvpx on the host)", k_name[codec], d->id, w, h, in, out);
    return 0;
}
static Result cv_dec_fin(void *t) {
    cv_dec *d = cv_of(t); if (!d) return 0;
    pthread_mutex_lock(&g_cv_mu); g_cv[d->id - 1] = NULL; pthread_mutex_lock(&d->mu); pthread_mutex_unlock(&d->mu); pthread_mutex_unlock(&g_cv_mu);
    if (d->vpx_ok) vpx_codec_destroy(&d->vpx);
    for (int i = 0; i < d->nheld; i++) vpx_img_free(d->held[i]);
    d->magic = 0;
    /* The submission-ready event goes with the decoder, as in the SDK: kept, it stayed bound, and the next decoder made in the
       same (cleared) memory found it "bound" and got none; nn::os aborted on it (nxnative, Bananza's second movie). */
    destroy_event(d->event);
    pthread_mutex_destroy(&d->mu); free(d); *(cv_dec **)t = NULL;
    return 0;
}
static Result cv_dec_reg(void *t, uint32_t i, void *pool, uint32_t pidx) {
    cv_dec *d = cv_of(t); if (!d || i >= CV_BUFS) { LOG("[RED] Register...DecoderBuffer index %u", i); return CV_R(2004); }
    pthread_mutex_lock(&d->mu);
    d->buf[i] = (cv_buf){.pool = pool_of(pool), .pidx = pidx, .reg = 1};
    if (d->fcount < 8) { d->fifo[(d->fhead + d->fcount) % 8] = i; d->fcount++; } else LOG("[RED] acquirable queue full at register %u", i);
    pthread_mutex_unlock(&d->mu);
    return 0;
}
static void drop_index(uint32_t *q, uint32_t *head, uint32_t *count, uint32_t cap, uint32_t i) {   /* removes i from a ring, keeping the others' order */
    uint32_t n = 0, tmp[CV_BUFS];
    for (uint32_t k = 0; k < *count; k++) { uint32_t v = q[(*head + k) % cap]; if (v != i) tmp[n++] = v; }
    for (uint32_t k = 0; k < n; k++) q[k] = tmp[k];
    *head = 0; *count = n;
}
static Result cv_dec_unreg(void *t, uint32_t i) {
    cv_dec *d = cv_of(t); if (!d || i >= CV_BUFS) return CV_R(2004);
    pthread_mutex_lock(&d->mu);
    d->buf[i].reg = 0; d->buf[i].filled = 0; d->buf[i].pool = NULL; d->buf[i].gen++;
    drop_index(d->fifo, &d->fhead, &d->fcount, 8, i); drop_index(d->pend, &d->phead, &d->pcount, CV_BUFS, i);
    pthread_mutex_unlock(&d->mu);
    return 0;
}
static Result cv_dec_reset(void *t) {   /* Stop / Flush: undelivered pictures and a pending end of stream are dropped; next key frame starts over */
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
static int cv_hold(cv_dec *d, vpx_image_t *c, int64_t ts) {
    if (d->nheld >= CV_HOLD) { static int n; if (n++ < 4) LOG("[RED] %s: a packet gave more pictures than the host holds: one dropped", k_name[d->codec]); vpx_img_free(c); return 0; }
    d->held[d->nheld] = c; d->held_ts[d->nheld] = ts; d->held_eos[d->nheld] = 0; d->nheld++; return 1;
}
static Result cv_dec_submit(uint64_t *out, void *t, const void *data, uint64_t size, int64_t ts, bool eos) {
    cv_dec *d = cv_of(t); if (!d) return CV_R(2004);
    pthread_mutex_lock(&d->mu);
    if (d->nheld >= CV_HOLD) { if (d->event) clear_event(d->event); pthread_mutex_unlock(&d->mu); return CV_BUSY; }
    int got = 0;
    if (size && d->vpx_ok) {
        if (vpx_codec_decode(&d->vpx, data, (unsigned)size, NULL, 0) != VPX_CODEC_OK) { static int n; if (n++ < 4) LOG("[RED] %s: %s", k_name[d->codec], vpx_codec_error(&d->vpx)); }
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
    (void)sync; cv_dec *d = cv_of(t); if (!d || i >= CV_BUFS || !d->buf[i].reg) { LOG("[RED] Release...DecoderOutputBuffer index %u", i); return; }
    pthread_mutex_lock(&d->mu);
    d->buf[i].gen++; d->buf[i].filled = 0;
    if (d->pcount < CV_BUFS) { d->pend[(d->phead + d->pcount) % CV_BUFS] = i; d->pcount++; }
    if (d->fcount < 8) { d->fifo[(d->fhead + d->fcount) % 8] = i; d->fcount++; } else LOG("[RED] acquirable queue full at release %u", i);
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
static Result cv_dec_encrypted(void) { static int once; if (!once++) LOG("[RED] an encrypted input buffer: protected streams are not served"); return CV_R(2004); }

/* One family of entry points per codec (the SDK's Vp8 / Vp9DecoderType share the layout). */
#define CV_FAMILY(tag, codec) \
    static Result tag##_work(uint64_t *size, uint64_t *align, uint64_t bs, uint16_t w, uint16_t h) { (void)bs; (void)w; (void)h; *size = 0x1000; *align = 0x1000; return 0; } \
    static Result tag##_init(void *t, void *work, uint64_t wsize, uint64_t bs, uint16_t w, uint16_t h, uint32_t refs, uint32_t in, uint32_t out) { (void)work; (void)wsize; (void)bs; (void)refs; return cv_dec_init(t, codec, w, h, in, out); } \
    static Result tag##_init2(void *t, uint64_t bs, uint16_t w, uint16_t h, uint32_t refs, uint32_t in, uint32_t out) { (void)bs; (void)refs; return cv_dec_init(t, codec, w, h, in, out); } \
    static Result tag##_fin(void *t) { return cv_dec_fin(t); } \
    static Result tag##_reg(void *t, uint32_t i, void *pool, uint32_t pidx) { return cv_dec_reg(t, i, pool, pidx); } \
    static Result tag##_unreg(void *t, uint32_t i) { return cv_dec_unreg(t, i); } \
    static Result tag##_start(void *t) { (void)t; return 0; } \
    static Result tag##_reset(void *t) { return cv_dec_reset(t); } \
    static Result tag##_event(void **out, void *t) { return cv_dec_event(out, t); } \
    static Result tag##_submit(uint64_t *out, void *t, const void *data, uint64_t size, int64_t ts, bool eos) { return cv_dec_submit(out, t, data, size, ts, eos); } \
    static Result tag##_acq(uint32_t *idx, cv_sync *sync, void *t) { return cv_dec_acq(idx, sync, t); } \
    static void tag##_rel(void *t, uint32_t i, cv_sync sync) { cv_dec_rel(t, i, sync); } \
    static Result tag##_props(uint16_t *w, uint16_t *h, uint16_t *cw, uint16_t *ch, int64_t *ts, bool *eos, void *t, uint32_t i) { return cv_dec_props(w, h, cw, ch, ts, eos, t, i); }
CV_FAMILY(vp9, CV_VP9)
CV_FAMILY(vp8, CV_VP8)
#undef CV_FAMILY

/* AV1: no decoder on the host; Initialize fails (the client's error path) instead of reaching the decoder service. Open. */
static Result av1_fail(void) { static int once; if (!once++) LOG("[RED] AV1 decoder: no AV1 decoder on the host, Initialize fails"); return CV_R(2004); }
static Result av1_work(uint64_t *size, uint64_t *align) { *size = 0x1000; *align = 0x1000; return 0; }
static Result av1_init(void *t) { *(void **)t = NULL; return av1_fail(); }
static Result av1_fin(void *t) { (void)t; return 0; }

/* The host's sync handles signal when their buffer holds the picture of that release; a handle the host did not make reads as
   signalled. */
static bool sync_reached(const cv_sync *s) {
    if (s->magic != CV_SYNC) { static int once; if (!once++ && (s->magic || s->dec)) LOG("[RED] a sync handle the host did not make (%#x): signalled", s->magic); return true; }
    if (!s->gen) return true;
    pthread_mutex_lock(&g_cv_mu);
    cv_dec *d = s->dec && s->dec <= 16 ? g_cv[s->dec - 1] : NULL;
    bool r = true;
    if (d && s->idx < CV_BUFS) { pthread_mutex_lock(&d->mu); r = d->buf[s->idx].filled && d->buf[s->idx].filled_gen >= s->gen; pthread_mutex_unlock(&d->mu); }
    pthread_mutex_unlock(&g_cv_mu);
    return r;
}
static bool gs_try_wait(const cv_sync *s) { return sync_reached(s); }
static Result spt_create(void *e) { memset(e, 0, 0x58); hle_bind_system_event(e, 0); SPT(e)->magic = CV_SYNC; return 0; }
static void spt_holder(void *holder, void *e) {
    static void (*init)(void *, void *);
    if (!init && !(init = guest_sym("_ZN2nn2os25InitializeMultiWaitHolderEPNS0_19MultiWaitHolderTypeEPNS0_15SystemEventTypeE"))) runtime_fail("nn::os::InitializeMultiWaitHolder missing");
    init(holder, e);
}
static Result spt_request(void *e, const cv_sync *s) {
    spt_unarm(e); clear_event(e);
    SPT(e)->want = *s; SPT(e)->armed = 1;
    pthread_mutex_lock(&g_spt_mu); int k = 0; while (k < 64 && g_spt[k]) k++; if (k < 64) g_spt[k] = e; pthread_mutex_unlock(&g_spt_mu);
    if (k == 64) { LOG("[RED] nn::syncpt: more than 64 armed events: signalled at once"); hle_signal_system_event(e); return 0; }
    if (sync_reached(s)) { spt_unarm(e); hle_signal_system_event(e); }
    return 0;
}
static void spt_cancel(void *e) { spt_unarm(e); clear_event(e); }
static void spt_destroy(void *e) { spt_unarm(e); destroy_event(e); SPT(e)->magic = 0; }

#define VP(codec, tag) \
    {"_ZN2nn5codec29Calculate" codec "DecoderWorkBufferEPmS1_mtt", tag##_work}, \
    {"_ZN2nn5codec20Initialize" codec "DecoderEPNS0_14" codec "DecoderTypeEPvmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatE", tag##_init}, \
    {"_ZN2nn5codec20Initialize" codec "DecoderEPNS0_14" codec "DecoderTypeEmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatE", tag##_init2}, \
    {"_ZN2nn5codec18Finalize" codec "DecoderEPNS0_14" codec "DecoderTypeE", tag##_fin}, \
    {"_ZN2nn5codec24Register" codec "DecoderBufferEPNS0_14" codec "DecoderTypeEjPNS0_25VideoDecoderImagePoolTypeEj", tag##_reg}, \
    {"_ZN2nn5codec26Unregister" codec "DecoderBufferEPNS0_14" codec "DecoderTypeEj", tag##_unreg}, \
    {"_ZN2nn5codec15Start" codec "DecoderEPNS0_14" codec "DecoderTypeE", tag##_start}, \
    {"_ZN2nn5codec14Stop" codec "DecoderEPNS0_14" codec "DecoderTypeE", tag##_reset}, \
    {"_ZN2nn5codec15Flush" codec "DecoderEPNS0_14" codec "DecoderTypeE", tag##_reset}, \
    {"_ZN2nn5codec33Get" codec "DecoderSubmissionReadyEventEPPNS_2os15SystemEventTypeEPNS0_14" codec "DecoderTypeE", tag##_event}, \
    {"_ZN2nn5codec27Submit" codec "DecoderInputBufferEPmPNS0_14" codec "DecoderTypeEPKvmNS_8TimeSpanEb", tag##_submit}, \
    {"_ZN2nn5codec36SubmitEncrypted" codec "DecoderInputBufferEPmPNS0_14" codec "DecoderTypeEPKNS0_18EncryptionPropertyEPKvmNS_8TimeSpanEb", cv_dec_encrypted}, \
    {"_ZN2nn5codec29Acquire" codec "DecoderOutputBufferEPjPNS_2gs16NativeSyncHandleEPNS0_14" codec "DecoderTypeE", tag##_acq}, \
    {"_ZN2nn5codec29Release" codec "DecoderOutputBufferEPNS0_14" codec "DecoderTypeEjNS_2gs16NativeSyncHandleE", tag##_rel}, \
    {"_ZN2nn5codec35Get" codec "DecoderOutputBufferPropertiesEPtS1_S1_S1_PNS_8TimeSpanEPbPNS0_14" codec "DecoderTypeEj", tag##_props}

void *runtime_ns2_codec_override(const char *name) {
    static const struct { const char *name; void *fn; } map[] = {
        {"_ZN2nn5codec5video10InitializeEv", cv_init}, {"_ZN2nn5codec5video8FinalizeEv", cv_init},
        {"_ZN2nn5codec25MakeDecodeImagePreferenceEttNS0_24VideoDecoderOutputFormatENS0_9CodecModeE", cv_preference},
        {"_ZN2nn5codec31CalculateDecoderImagePoolBufferEPmS1_RKNS0_27VideoDecoderImagePreferenceEj", cv_pool_size},
        {"_ZN2nn5codec26InitializeDecoderImagePoolEPNS0_25VideoDecoderImagePoolTypeEPvmRKNS0_27VideoDecoderImagePreferenceEjNS0_9CodecModeE", cv_pool_init},
        {"_ZN2nn5codec24FinalizeDecoderImagePoolEPNS0_25VideoDecoderImagePoolTypeE", cv_pool_fin},
        {"_ZN2nn5codec36GetDecoderImagePoolNativeImageHandleEPKNS0_25VideoDecoderImagePoolTypeEj", cv_native},
        {"_ZN2nn5codec26ReadDecoderOutputImageRgbaEPKNS0_25VideoDecoderImagePoolTypeEjttttPvmm", cv_read_rgba},
        {"_ZN2nn5codec26ReadDecoderOutputImageNv12EPKNS0_25VideoDecoderImagePoolTypeEjttttPvmmS4_mm", cv_read_nv12},
        VP("Vp9", vp9), VP("Vp8", vp8),
        {"_ZN2nn5codec29CalculateAv1DecoderWorkBufferEPmS1_mtt", av1_work},
        {"_ZN2nn5codec20InitializeAv1DecoderEPNS0_14Av1DecoderTypeEPvmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatE", av1_init},
        {"_ZN2nn5codec20InitializeAv1DecoderEPNS0_14Av1DecoderTypeEmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatE", av1_init},
        {"_ZN2nn5codec18FinalizeAv1DecoderEPNS0_14Av1DecoderTypeE", av1_fin},
        {"_ZN2nn2gs17TryWaitNativeSyncERKNS0_16NativeSyncHandleE", gs_try_wait},
        {"_ZN2nn6syncpt3sdk20CreateSyncPointEventEPNS1_18SyncPointEventTypeE", spt_create},
        {"_ZN2nn6syncpt3sdk25InitializeMultiWaitHolderEPNS_2os19MultiWaitHolderTypeEPNS1_18SyncPointEventTypeE", spt_holder},
        {"_ZN2nn6syncpt3sdk21RequestSyncPointEventEPNS1_18SyncPointEventTypeERKNS1_15SyncPointHandleE", spt_request},
        {"_ZN2nn6syncpt3sdk20CancelSyncPointEventEPNS1_18SyncPointEventTypeE", spt_cancel},
        {"_ZN2nn6syncpt3sdk21DestroySyncPointEventEPNS1_18SyncPointEventTypeE", spt_destroy},
    };
    for (size_t i = 0; i < sizeof map / sizeof *map; i++) if (!strcmp(name, map[i].name)) return map[i].fn;
    return NULL;
}
#undef VP
