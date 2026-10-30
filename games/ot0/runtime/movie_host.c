/* movie::Decoder for VP9 video on the host with libvpx.
   TOTK's pre-rendered cutscenes (the .webm files in Event/Movie, 28 of them: VP9 profile 0, 1920x1080, 30 fps, video only) play through
   the SDK movie library in subsdk0, which is Android's MediaCodec over NVIDIA's NvMM. Its video decoder opens Tegra's
   NVDEC through nvdrv (NvMMLiteOpenVideo -> NvRmChannelOpen -> NvOsDrvOpen), which this port does not have, so the SDK
   aborted at the first cutscene (the blood moon at midnight).
   main imports the constructor and calls the 17 virtual methods through the vtable for movie::Decoder, whose slots are
   relocated by symbol name: overriding the names replaces the slots. A VP9 buffer-mode decoder (type 3, mode 1) built by
   the constructor is a host object, +0 the library's vtable and +8 a host Movie where the library keeps its DecoderImpl;
   every other decoder goes to the original code. Behaviour follows DecoderImpl (analysis/movie-decoder-impl-named.c):
   - output format 4 is packed NV12 (Y plane, then interleaved UV; stride = width), written into the client's Buffer;
   - input and output buffers are indices handed out FIFO, as MediaCodec does; input flag 4 / output flag 4 = end of stream;
   - client events {input available, output available, format changed, error} (DecoderEvents, 4 EventType pointers) are
     signalled with nn::os::SignalEvent; the game waits on the first three (main+0x240d570).
   Decoding is synchronous in SendInputBufferForDecode. While the game holds every output slot, inputs wait and are decoded
   when it releases one (ReleaseOutputBufferIndex).
   Pacing: the game shows one decoded frame per game frame (main+0x240266c draws the movie texture every frame and puts a
   new frame into it whenever one is queued), which plays a 30 fps movie at 30 fps only at the original 30 fps. At our
   60-100 fps a cutscene ran 3x too fast. So decoded frames are handed to the game when their presentation time is due,
   counted from the first frame (movie_host_tick, called at every present on the game's thread); in between the movie
   texture keeps the last frame. At most one frame is handed over per game frame (the game takes one per frame, so more
   would pile up in its own queue), and when the game falls behind (a loading hitch: ~15 fps for a few seconds under the
   blood moon) a frame more than 1.5 frame periods late is dropped if the next one is due too: the picture stays in time
   with the cutscene's sound instead of lagging and then running fast to catch up. TOTK_MOVIE_PACE=0 hands every frame
   over as soon as it is decoded. */
#include "runtime.h"
#include "movie_host.h"
#include <arm_neon.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if !__has_include(<vpx/vpx_decoder.h>)
/* No libvpx for this target (the iPhone build has none yet): no override, so the movie library runs its own decoder,
   which still aborts at the first cutscene there. Build libvpx for iOS (or use VideoToolbox's VP9) to enable this file. */
void *runtime_movie_override(const char *name) {
    static int said;
    if (!said && strstr(name, "5movie7Decoder")) { said = 1; fprintf(stderr, "[movie] 本构建没有 libvpx：过场影片走原库（遇到过场会中止）\n"); }
    return NULL;
}
void movie_host_tick(void) {}
#else
#include <vpx/vp8dx.h>
#include <vpx/vpx_decoder.h>

enum { IN_BUFS = 4, IN_CAP = 2 << 20, OUT_BUFS = 8, MAX_MOVIES = 16 };
enum { TYPE_VP9 = 3, MODE_BUFFER = 1, FORMAT_NV12 = 4, FLAG_EOS = 4 };
enum { ST_OK = 0, ST_ERROR = 0x1000, ST_NULL = 0x100b, ST_NOT_READY = 0x100c, ST_NO_OUTPUT = 0x100e, ST_NO_INPUT = 0x1011,
       ST_UNSUPPORTED = 0x2001, ST_NOT_CONFIGURED = 0x2002 };
enum { EV_INPUT, EV_OUTPUT, EV_FORMAT, EV_ERROR };

typedef struct { int index; unsigned flags; int64_t time; } Ready;
typedef struct { int index, offset, size; int64_t time; unsigned flags; } Pending;
typedef struct {
    void *guest;                         /* the movie::Decoder object */
    int format, configured, started, w, h, eos_in, format_sent, last_error;
    void **events;                       /* DecoderEvents: EventType* per EV_* */
    vpx_codec_ctx_t vpx; int vpx_live;
    uint8_t *in[IN_BUFS]; int in_free[IN_BUFS], nin_free;
    Pending pending[IN_BUFS]; int npending;
    uint8_t *out[OUT_BUFS]; int out_bytes[OUT_BUFS], out_held[OUT_BUFS];
    Ready ready[OUT_BUFS]; int nready;
    Ready decoded[OUT_BUFS]; int ndecoded;   /* paced: decoded, not due yet */
    uint64_t t0; int64_t pts0;               /* paced: host time and presentation time of the first frame handed over */
    uint64_t frames, decode_ns, errors, dropped;
    pthread_mutex_t mu;
} Movie;

static MovieGuest G; static int g_ready;
static int g_negative = -1;   /* probe-movie negative controls only: 1 = U and V swapped, 2 = no format-changed event */
static int negative(void) { if (g_negative < 0) { const char *e = getenv("TOTK_PROBE_NEGATIVE"); g_negative = e ? atoi(e) : 0; } return g_negative; }
static uint64_t now_ns(void);
static uint64_t (*g_clock)(void) = now_ns;
void movie_host_set_clock(uint64_t (*clock)(void)) { g_clock = clock ? clock : now_ns; }
static int pace_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("TOTK_MOVIE_PACE"); v = !(e && !strcmp(e, "0")); } return v; }
static Movie *g_movies[MAX_MOVIES];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static const char *const NAMES[MOVIE_ENTRIES] = {
    "_ZN5movie7DecoderC1ENS_11DecoderTypeENS_19DecoderOutputFormatENS_11DecoderModeEi", "_ZN5movie7DecoderD2Ev",
    "_ZN5movie7DecoderD0Ev", "_ZN5movie7Decoder9ConfigureEPNS_9MediaDataE",
    "_ZN5movie7Decoder20RegisterClientEventsEPNS_13DecoderEventsE", "_ZN5movie7Decoder15GetOutputFormatEPNS_9MediaDataE",
    "_ZN5movie7Decoder5StartEv", "_ZN5movie7Decoder4StopEv", "_ZN5movie7Decoder5ResetEv", "_ZN5movie7Decoder5FlushEv",
    "_ZN5movie7Decoder23AcquireInputBufferIndexEPiS1_", "_ZN5movie7Decoder14GetInputBufferEiPNS_6BufferE",
    "_ZN5movie7Decoder24SendInputBufferForDecodeEiiilj", "_ZN5movie7Decoder24AcquireOutputBufferIndexEPiPlPjS1_",
    "_ZN5movie7Decoder15GetOutputBufferEiPNS_6BufferE", "_ZN5movie7Decoder19GetOutputBufferSizeEPiS1_",
    "_ZN5movie7Decoder24ReleaseOutputBufferIndexEi", "_ZN5movie7Decoder12GetLastErrorEv",
    "_ZN5movie7Decoder15GetOutputBufferEiPNS_6BufferEPNS_9MediaDataE", "_ZN5movie7Decoder21GetOutputBufferFormatEiPNS_9MediaDataE"};

#define LOG(...) do { fprintf(stderr, "[movie] " __VA_ARGS__); fputc('\n', stderr); } while (0)
static uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

static void need_symbol(void **slot, const char *name) {
    *slot = runtime_symbol(name);
    if (!*slot) { fprintf(stderr, "[movie] missing guest symbol %s\n", name); runtime_fail("movie decoder: guest symbol missing"); }
}
/* The guest functions are resolved the first time any entry runs (the modules are loaded and relocated by then). */
static void guest_init(void) {
    pthread_mutex_lock(&g_lock);
    if (!g_ready) {
        need_symbol((void **)&G.signal_event, "_ZN2nn2os11SignalEventEPNS0_9EventTypeE");
        need_symbol((void **)&G.buf_set_range, "_ZN5movie6Buffer8SetRangeEii");
        need_symbol((void **)&G.buf_set_data, "_ZN5movie6Buffer18SetDataAndCapacityEPvm");
        need_symbol((void **)&G.buf_base, "_ZNK5movie6Buffer4BaseEv");
        need_symbol((void **)&G.buf_capacity, "_ZNK5movie6Buffer8CapacityEv");
        need_symbol((void **)&G.md_find_int32, "_ZNK5movie9MediaData9FindInt32EPKcPi");
        need_symbol((void **)&G.md_find_string, "_ZNK5movie9MediaData10FindStringEPKcPS2_");
        need_symbol((void **)&G.md_set_int32, "_ZN5movie9MediaData8SetInt32EPKci");
        need_symbol((void **)&G.md_set_rect, "_ZN5movie9MediaData7SetRectEPKciiii");
        need_symbol((void **)&G.md_set_string, "_ZN5movie9MediaData9SetStringEPKcS2_");
        void *vt; need_symbol(&vt, "_ZTVN5movie7DecoderE"); G.vptr = (uint8_t *)vt + 0x10;
        for (int i = 0; i < MOVIE_ENTRIES; i++) need_symbol(&G.orig[i], NAMES[i]);
        g_ready = 1;
    }
    pthread_mutex_unlock(&g_lock);
}
void movie_host_set_guest(const MovieGuest *api) { pthread_mutex_lock(&g_lock); G = *api; g_ready = 1; pthread_mutex_unlock(&g_lock); }

static Movie *find(const void *guest) {
    if (!g_ready) guest_init();
    pthread_mutex_lock(&g_lock); Movie *m = NULL;
    for (int i = 0; i < MAX_MOVIES; i++) if (g_movies[i] && g_movies[i]->guest == guest) m = g_movies[i];
    pthread_mutex_unlock(&g_lock);
    return m;
}

/* Events are signalled after the decoder's lock is released: the woken game thread calls straight back in. */
typedef struct { void *ev[8]; int n; } Signals;
static void note(Movie *m, Signals *s, int which) { if (m->events && m->events[which] && s->n < 8) s->ev[s->n++] = m->events[which]; }
static void fire(const Signals *s) { for (int i = 0; i < s->n; i++) G.signal_event(s->ev[i]); }

static int frame_bytes(const Movie *m) { return m->w * m->h + (m->w / 2) * (m->h / 2) * 2; }

/* I420 (libvpx) -> packed NV12: Y rows, then rows of interleaved U, V. */
static void to_nv12(const vpx_image_t *img, uint8_t *dst, int w, int h) {
    for (int y = 0; y < h; y++) memcpy(dst + (size_t)y * w, img->planes[VPX_PLANE_Y] + (size_t)y * img->stride[VPX_PLANE_Y], (size_t)w);
    uint8_t *uv = dst + (size_t)w * h;
    for (int y = 0; y < h / 2; y++) {
        const uint8_t *u = img->planes[VPX_PLANE_U] + (size_t)y * img->stride[VPX_PLANE_U];
        const uint8_t *v = img->planes[VPX_PLANE_V] + (size_t)y * img->stride[VPX_PLANE_V];
        uint8_t *o = uv + (size_t)y * w; int x = 0;
        if (negative() == 1) { const uint8_t *t = u; u = v; v = t; }
        for (; x + 16 <= w / 2; x += 16) { uint8x16x2_t p = {{vld1q_u8(u + x), vld1q_u8(v + x)}}; vst2q_u8(o + 2 * x, p); }
        for (; x < w / 2; x++) { o[2 * x] = u[x]; o[2 * x + 1] = v[x]; }
    }
}
static int free_out(const Movie *m) { for (int i = 0; i < OUT_BUFS; i++) if (!m->out_held[i]) return i; return -1; }
static void push_ready(Movie *m, Signals *s, int slot, unsigned flags, int64_t time, int bytes) {
    m->out_held[slot] = 1; m->out_bytes[slot] = bytes;
    if (pace_on()) { m->decoded[m->ndecoded++] = (Ready){slot, flags, time}; return; }
    m->ready[m->nready++] = (Ready){slot, flags, time};
    note(m, s, EV_OUTPUT);
}
/* Host time a decoded frame is due (ns); end of stream is due as soon as it is first in line. */
static uint64_t due_at(Movie *m, const Ready *d) {
    int64_t ahead = d->time - m->pts0; if (ahead < 0) ahead = 0;
    return m->t0 + (uint64_t)ahead * 1000;
}
/* One game frame (a present): drop leading frames that are more than 1.5 frame periods late while the next one is due
   too, then hand over at most one due frame (end of stream right after the last frame). */
static void release_due(Movie *m, Signals *s, uint64_t now) {
    while (m->ndecoded > 1 && !(m->decoded[0].flags & FLAG_EOS) && !(m->decoded[1].flags & FLAG_EOS) && m->t0) {
        uint64_t due = due_at(m, &m->decoded[0]), next = due_at(m, &m->decoded[1]);
        if (now < next || now - due <= (next - due) * 3 / 2) break;
        m->out_held[m->decoded[0].index] = 0; m->dropped++;
        memmove(m->decoded, m->decoded + 1, (size_t)--m->ndecoded * sizeof m->decoded[0]);
    }
    if (!m->ndecoded) return;
    Ready d = m->decoded[0];
    if (!(d.flags & FLAG_EOS)) {
        if (!m->t0) { m->t0 = now; m->pts0 = d.time; }
        if (now < due_at(m, &d)) return;
    }
    memmove(m->decoded, m->decoded + 1, (size_t)--m->ndecoded * sizeof m->decoded[0]);
    m->ready[m->nready++] = d; note(m, s, EV_OUTPUT);
}
void movie_host_tick(void) {   /* lock order g_lock -> Movie.mu, as in destroy; events fire after both are released */
    Signals s = {0};
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_MOVIES; i++) {
        Movie *m = g_movies[i]; if (!m) continue;
        pthread_mutex_lock(&m->mu); if (m->started) release_due(m, &s, g_clock()); pthread_mutex_unlock(&m->mu);
    }
    pthread_mutex_unlock(&g_lock);
    fire(&s);
}
/* Decode one input into output slots; returns 0 when it had to wait for a free output slot. */
static int decode_one(Movie *m, Signals *s, const Pending *p) {
    int need = p->size > 0 ? 1 : 0;
    if (p->flags & FLAG_EOS && !m->eos_in) need++;
    int freeslots = 0; for (int i = 0; i < OUT_BUFS; i++) freeslots += !m->out_held[i];
    if (freeslots < need) return 0;
    if (p->size > 0) {
        uint64_t t0 = now_ns();
        vpx_codec_err_t e = vpx_codec_decode(&m->vpx, m->in[p->index] + p->offset, (unsigned)p->size, NULL, 0);
        if (e != VPX_CODEC_OK) {
            m->errors++; m->last_error = ST_ERROR; note(m, s, EV_ERROR);
            if (m->errors <= 4) LOG("【红】VP9 decode error on a %d-byte frame at %lld us: %s", p->size, (long long)p->time, vpx_codec_error_detail(&m->vpx) ? vpx_codec_error_detail(&m->vpx) : vpx_codec_err_to_string(e));
        } else {
            vpx_codec_iter_t it = NULL; const vpx_image_t *img;
            while ((img = vpx_codec_get_frame(&m->vpx, &it)) != NULL) {
                if (img->fmt != VPX_IMG_FMT_I420 || (int)img->d_w != m->w || (int)img->d_h != m->h) {
                    LOG("【红】VP9 frame %ux%u format %d differs from the configured %dx%d I420", img->d_w, img->d_h, img->fmt, m->w, m->h);
                    runtime_fail("movie decoder: unexpected VP9 frame layout");
                }
                int slot = free_out(m); if (slot < 0) runtime_fail("movie decoder: output slots counted wrong");
                if (!m->format_sent) { m->format_sent = 1; if (negative() != 2) note(m, s, EV_FORMAT); }
                to_nv12(img, m->out[slot], m->w, m->h);
                push_ready(m, s, slot, 0, p->time, frame_bytes(m));
                m->frames++;
            }
        }
        m->decode_ns += now_ns() - t0;
        if (m->frames && m->frames % 300 == 0) LOG("%llu frames, decode %.2f ms per frame", (unsigned long long)m->frames, m->decode_ns / 1e6 / (double)m->frames);
    }
    if (p->flags & FLAG_EOS && !m->eos_in) {
        m->eos_in = 1; int slot = free_out(m); if (slot < 0) runtime_fail("movie decoder: no slot for end of stream");
        push_ready(m, s, slot, FLAG_EOS, p->time, 0);
        LOG("end of stream after %llu frames (%.2f ms per frame, %llu errors)", (unsigned long long)m->frames, m->frames ? m->decode_ns / 1e6 / (double)m->frames : 0.0, (unsigned long long)m->errors);
    }
    m->in_free[m->nin_free++] = p->index; note(m, s, EV_INPUT);
    return 1;
}
static void drain_pending(Movie *m, Signals *s) {
    while (m->npending && decode_one(m, s, &m->pending[0])) { memmove(m->pending, m->pending + 1, (size_t)--m->npending * sizeof m->pending[0]); }
}
static void release_codec(Movie *m) {
    if (m->vpx_live) { vpx_codec_destroy(&m->vpx); m->vpx_live = 0; }
    for (int i = 0; i < IN_BUFS; i++) { free(m->in[i]); m->in[i] = NULL; }
    for (int i = 0; i < OUT_BUFS; i++) { free(m->out[i]); m->out[i] = NULL; }
    m->configured = m->started = 0; m->nin_free = m->npending = m->nready = m->ndecoded = 0; m->t0 = 0;
}
static void reset_queues(Movie *m) {
    m->nin_free = 0; for (int i = 0; i < IN_BUFS; i++) m->in_free[m->nin_free++] = i;
    memset(m->out_held, 0, sizeof m->out_held); m->npending = m->nready = m->ndecoded = 0; m->eos_in = 0; m->t0 = 0;
}

/* ---- entries ---- */
typedef void (*ctor_fn)(void *, int, int, int, int);
static void h_ctor(void *obj, int type, int format, int mode, int priority) {
    if (!g_ready) guest_init();
    if (type != TYPE_VP9 || mode != MODE_BUFFER) {
        LOG("decoder type %d mode %d format %d: left to the library (only VP9 in buffer mode runs on the host)", type, mode, format);
        ((ctor_fn)G.orig[MOVIE_CTOR])(obj, type, format, mode, priority); return;
    }
    Movie *m = calloc(1, sizeof *m); if (!m) runtime_fail("movie decoder allocation");
    m->guest = obj; m->format = format; pthread_mutex_init(&m->mu, NULL);
    pthread_mutex_lock(&g_lock); int i = 0;
    while (i < MAX_MOVIES && g_movies[i] && g_movies[i]->guest != obj) i++;
    if (i == MAX_MOVIES) runtime_fail("movie decoder table full");
    if (g_movies[i]) { LOG("【红】decoder object %p reused without destruction", obj); release_codec(g_movies[i]); free(g_movies[i]); }
    g_movies[i] = m; pthread_mutex_unlock(&g_lock);
    ((void **)obj)[0] = G.vptr; ((void **)obj)[1] = m;
    LOG("VP9 decoder %p on the host (libvpx %s), output format %d", obj, vpx_codec_version_str(), format);
}
static void destroy(void *obj) {
    pthread_mutex_lock(&g_lock); Movie *m = NULL;
    for (int i = 0; i < MAX_MOVIES; i++) if (g_movies[i] && g_movies[i]->guest == obj) { m = g_movies[i]; g_movies[i] = NULL; }
    if (m) { pthread_mutex_lock(&m->mu); pthread_mutex_unlock(&m->mu); }   /* wait for an operation in progress (movie_host_tick) */
    pthread_mutex_unlock(&g_lock);
    if (!m) return;
    LOG("decoder %p destroyed after %llu frames", obj, (unsigned long long)m->frames);
    release_codec(m); pthread_mutex_destroy(&m->mu); free(m);
    ((void **)obj)[0] = G.vptr; ((void **)obj)[1] = NULL;   /* what the library's destructor leaves (no DecoderImpl) */
}
typedef void (*obj_fn)(void *);
static void h_d2(void *obj) { if (!find(obj)) { ((obj_fn)G.orig[MOVIE_D2])(obj); return; } destroy(obj); }
/* Deleting destructor: free the host state, then the library's own deleting destructor frees the object (impl now 0). */
static void h_d0(void *obj) { if (find(obj)) destroy(obj); ((obj_fn)G.orig[MOVIE_D0])(obj); }

typedef int (*md_fn)(void *, void *);
static int h_configure(void *obj, void *format) {
    Movie *m = find(obj); if (!m) return ((md_fn)G.orig[MOVIE_CONFIGURE])(obj, format);
    if (!format) return ST_NULL;
    if (m->format != FORMAT_NV12) { LOG("【红】output format %d not implemented on the host (NV12 = 4 only)", m->format); return ST_UNSUPPORTED; }
    const char *mime = NULL; int w = 0, h = 0;
    G.md_find_string(format, "mime", &mime); G.md_find_int32(format, "width", &w); G.md_find_int32(format, "height", &h);
    if (!mime || strcmp(mime, "video/x-vnd.on2.vp9") || w <= 0 || h <= 0 || w > 8192 || h > 8192 || (w | h) & 1) {
        LOG("【红】configure: mime %s %dx%d not supported", mime ? mime : "(none)", w, h); return ST_UNSUPPORTED;
    }
    pthread_mutex_lock(&m->mu);
    release_codec(m); m->w = w; m->h = h;
    vpx_codec_dec_cfg_t cfg = {.threads = 4, .w = (unsigned)w, .h = (unsigned)h};
    if (vpx_codec_dec_init(&m->vpx, vpx_codec_vp9_dx(), &cfg, 0) != VPX_CODEC_OK) runtime_fail("libvpx VP9 decoder initialisation");
    m->vpx_live = 1;
    for (int i = 0; i < IN_BUFS; i++) if (!(m->in[i] = malloc(IN_CAP))) runtime_fail("movie input buffers");
    for (int i = 0; i < OUT_BUFS; i++) if (!(m->out[i] = malloc((size_t)frame_bytes(m)))) runtime_fail("movie output buffers");
    reset_queues(m); m->format_sent = 0; m->configured = 1; m->last_error = 0;
    pthread_mutex_unlock(&m->mu);
    LOG("configured %dx%d VP9, %d input buffers of %d KiB, %d output frames of %d bytes", w, h, IN_BUFS, IN_CAP >> 10, OUT_BUFS, frame_bytes(m));
    return ST_OK;
}
static int h_register_events(void *obj, void *events) {
    Movie *m = find(obj); if (!m) return ((md_fn)G.orig[MOVIE_REGISTER_EVENTS])(obj, events);
    m->events = events; return ST_OK;
}
static void set_format(Movie *m, void *md) {
    G.md_set_string(md, "mime", "video/raw");
    G.md_set_int32(md, "width", m->w); G.md_set_int32(md, "height", m->h);
    G.md_set_int32(md, "stride", m->w); G.md_set_int32(md, "slice-height", m->h);
    G.md_set_int32(md, "color-format", 21);   /* COLOR_FormatYUV420SemiPlanar */
    G.md_set_rect(md, "crop", 0, 0, m->w - 1, m->h - 1);
    G.md_set_int32(md, "crop-width", m->w); G.md_set_int32(md, "crop-height", m->h);
    G.md_set_int32(md, "nv12-y-stride", m->w); G.md_set_int32(md, "nv12-uv-stride", m->w);
    G.md_set_int32(md, "nv12-y-offset", 0); G.md_set_int32(md, "nv12-uv-offset", m->w * m->h);
}
static int h_get_output_format(void *obj, void *md) {
    Movie *m = find(obj); if (!m) return ((md_fn)G.orig[MOVIE_GET_OUTPUT_FORMAT])(obj, md);
    if (!md) return ST_NULL;
    if (!m->configured) return ST_ERROR;
    set_format(m, md); return ST_OK;
}
typedef int (*int_fn)(void *);
static int h_start(void *obj) {
    Movie *m = find(obj); if (!m) return ((int_fn)G.orig[MOVIE_START])(obj);
    Signals s = {0};
    pthread_mutex_lock(&m->mu);
    if (!m->configured) { pthread_mutex_unlock(&m->mu); return ST_NOT_CONFIGURED; }
    m->started = 1; note(m, &s, EV_INPUT);
    pthread_mutex_unlock(&m->mu); fire(&s);
    return ST_OK;
}
static int h_stop(void *obj) {
    Movie *m = find(obj); if (!m) return ((int_fn)G.orig[MOVIE_STOP])(obj);
    pthread_mutex_lock(&m->mu); LOG("stop after %llu frames (%llu dropped to keep time)", (unsigned long long)m->frames, (unsigned long long)m->dropped); release_codec(m); pthread_mutex_unlock(&m->mu);
    return ST_OK;
}
static int h_reset(void *obj) {
    Movie *m = find(obj); if (!m) return ((int_fn)G.orig[MOVIE_RESET])(obj);
    pthread_mutex_lock(&m->mu); release_codec(m); m->last_error = 0; pthread_mutex_unlock(&m->mu);
    return ST_OK;
}
static int h_flush(void *obj) {
    Movie *m = find(obj); if (!m) return ((int_fn)G.orig[MOVIE_FLUSH])(obj);
    pthread_mutex_lock(&m->mu);
    if (m->configured) {   /* drop queued work; the next input starts a new decode (after a seek: a key frame) */
        vpx_codec_destroy(&m->vpx); m->vpx_live = 0;
        vpx_codec_dec_cfg_t cfg = {.threads = 4, .w = (unsigned)m->w, .h = (unsigned)m->h};
        if (vpx_codec_dec_init(&m->vpx, vpx_codec_vp9_dx(), &cfg, 0) != VPX_CODEC_OK) runtime_fail("libvpx VP9 decoder re-initialisation");
        m->vpx_live = 1; reset_queues(m);
    }
    pthread_mutex_unlock(&m->mu);
    return ST_OK;
}
typedef int (*pp_fn)(void *, int *, int *);
static int h_acquire_input(void *obj, int *index, int *remaining) {
    Movie *m = find(obj); if (!m) return ((pp_fn)G.orig[MOVIE_ACQUIRE_INPUT])(obj, index, remaining);
    if (!index || !remaining) return ST_NULL;
    pthread_mutex_lock(&m->mu);
    *index = -1; *remaining = 0; int st = ST_NO_INPUT;
    if (m->started && m->nin_free > 0) {
        *index = m->in_free[0]; memmove(m->in_free, m->in_free + 1, (size_t)--m->nin_free * sizeof m->in_free[0]);
        *remaining = m->nin_free; st = ST_OK;
    }
    pthread_mutex_unlock(&m->mu); return st;
}
typedef int (*ib_fn)(void *, int, void *);
static int h_get_input(void *obj, int index, void *buf) {
    Movie *m = find(obj); if (!m) return ((ib_fn)G.orig[MOVIE_GET_INPUT])(obj, index, buf);
    if (!buf) return ST_NULL;
    if (index < 0 || index >= IN_BUFS || !m->in[index]) { G.buf_set_range(buf, 0, 0); G.buf_set_data(buf, NULL, 0); return ST_ERROR; }
    G.buf_set_range(buf, 0, IN_CAP); G.buf_set_data(buf, m->in[index], IN_CAP);
    return ST_OK;
}
typedef int (*send_fn)(void *, int, int, int, int64_t, unsigned);
static int h_send_input(void *obj, int index, int offset, int size, int64_t time, unsigned flags) {
    Movie *m = find(obj); if (!m) return ((send_fn)G.orig[MOVIE_SEND_INPUT])(obj, index, offset, size, time, flags);
    if (index < 0 || index >= IN_BUFS || offset < 0 || size < 0 || (int64_t)offset + size > IN_CAP) return ST_ERROR;
    Signals s = {0};
    pthread_mutex_lock(&m->mu);
    if (!m->started) { pthread_mutex_unlock(&m->mu); return ST_ERROR; }
    Pending p = {index, offset, size, time, flags};
    if (m->eos_in) { m->in_free[m->nin_free++] = index; note(m, &s, EV_INPUT); }   /* after end of stream: hand the buffer back */
    else if (m->npending || !decode_one(m, &s, &p)) m->pending[m->npending++] = p;
    pthread_mutex_unlock(&m->mu); fire(&s);
    return ST_OK;
}
typedef int (*acq_fn)(void *, int *, int64_t *, unsigned *, int *);
static int h_acquire_output(void *obj, int *index, int64_t *time, unsigned *flags, int *remaining) {
    Movie *m = find(obj); if (!m) return ((acq_fn)G.orig[MOVIE_ACQUIRE_OUTPUT])(obj, index, time, flags, remaining);
    if (!index || !time || !flags || !remaining) return ST_NULL;
    pthread_mutex_lock(&m->mu);   /* paced frames become ready only at presents (movie_host_tick): one per game frame */
    *index = -1; *time = 0; *flags = 0; *remaining = 0; int st = ST_NO_OUTPUT;
    if (m->nready > 0) {
        Ready r = m->ready[0]; memmove(m->ready, m->ready + 1, (size_t)--m->nready * sizeof m->ready[0]);
        *index = r.index; *time = r.time; *flags = r.flags; *remaining = m->nready; st = ST_OK;
    }
    pthread_mutex_unlock(&m->mu);
    return st;
}
static int h_get_output(void *obj, int index, void *buf) {
    Movie *m = find(obj); if (!m) return ((ib_fn)G.orig[MOVIE_GET_OUTPUT])(obj, index, buf);
    if (!buf) return ST_NULL;
    G.buf_set_range(buf, 0, 0);
    pthread_mutex_lock(&m->mu);
    int st = ST_ERROR;
    if (index >= 0 && index < OUT_BUFS && m->out_held[index]) {
        int bytes = m->out_bytes[index]; uint8_t *dst = G.buf_base(buf);
        if (!bytes) st = ST_OK;
        else if (dst && G.buf_capacity(buf) >= (size_t)bytes) { memcpy(dst, m->out[index], (size_t)bytes); G.buf_set_range(buf, 0, bytes); st = ST_OK; }
        else LOG("【红】output buffer of %zu bytes for a %d-byte frame", G.buf_capacity(buf), bytes);
    }
    pthread_mutex_unlock(&m->mu); return st;
}
static int h_get_output_size(void *obj, int *size, int *count) {
    Movie *m = find(obj); if (!m) return ((pp_fn)G.orig[MOVIE_GET_OUTPUT_SIZE])(obj, size, count);
    if (!size || !count) return ST_NULL;
    *count = 1; *size = m->configured ? frame_bytes(m) : 0;
    return ST_OK;
}
typedef int (*i_fn)(void *, int);
static int h_release_output(void *obj, int index) {
    Movie *m = find(obj); if (!m) return ((i_fn)G.orig[MOVIE_RELEASE_OUTPUT])(obj, index);
    Signals s = {0};
    pthread_mutex_lock(&m->mu);
    int st = ST_ERROR;
    if (index >= 0 && index < OUT_BUFS && m->out_held[index]) {
        int queued = 0; for (int i = 0; i < m->nready; i++) queued |= m->ready[i].index == index;
        for (int i = 0; i < m->ndecoded; i++) queued |= m->decoded[i].index == index;
        if (!queued) { m->out_held[index] = 0; drain_pending(m, &s); st = ST_OK; }
    }
    pthread_mutex_unlock(&m->mu); fire(&s);
    return st;
}
static int h_get_last_error(void *obj) {
    Movie *m = find(obj); if (!m) return ((int_fn)G.orig[MOVIE_GET_LAST_ERROR])(obj);
    return m->last_error;
}
typedef int (*ibm_fn)(void *, int, void *, void *);
static int h_get_output_buffer_format(void *obj, int index, void *md) {
    Movie *m = find(obj); if (!m) return ((ib_fn)G.orig[MOVIE_GET_OUTPUT_BUFFER_FORMAT])(obj, index, md);
    if (!md) return ST_NULL;
    if (index < 0 || index >= OUT_BUFS || !m->out_held[index]) return ST_ERROR;
    set_format(m, md); return ST_OK;
}
static int h_get_output_md(void *obj, int index, void *buf, void *md) {
    Movie *m = find(obj); if (!m) return ((ibm_fn)G.orig[MOVIE_GET_OUTPUT_MD])(obj, index, buf, md);
    if (!buf && !md) return ST_NULL;
    int st = md ? h_get_output_buffer_format(obj, index, md) : ST_OK;
    return st == ST_OK && buf ? h_get_output(obj, index, buf) : st;
}

static void *const ENTRIES[MOVIE_ENTRIES] = {
    h_ctor, h_d2, h_d0, h_configure, h_register_events, h_get_output_format, h_start, h_stop, h_reset, h_flush,
    h_acquire_input, h_get_input, h_send_input, h_acquire_output, h_get_output, h_get_output_size, h_release_output,
    h_get_last_error, h_get_output_md, h_get_output_buffer_format};
void *movie_host_entry(int which) { return which >= 0 && which < MOVIE_ENTRIES ? ENTRIES[which] : NULL; }

void *runtime_movie_override(const char *name) {
    static int off = -1;
    if (off < 0) { const char *e = getenv("TOTK_MOVIE_HOST"); off = e && !strcmp(e, "0"); }
    if (off || strncmp(name, "_ZN5movie7Decoder", 17)) return NULL;
    for (int i = 0; i < MOVIE_ENTRIES; i++) if (!strcmp(name, NAMES[i])) return ENTRIES[i];
    /* aliases: the base-object constructor and the complete-object destructor share code with the entries above */
    if (!strcmp(name, "_ZN5movie7DecoderC2ENS_11DecoderTypeENS_19DecoderOutputFormatENS_11DecoderModeEi")) return h_ctor;
    if (!strcmp(name, "_ZN5movie7DecoderD1Ev")) return h_d2;
    return NULL;
}
#endif   /* libvpx */
