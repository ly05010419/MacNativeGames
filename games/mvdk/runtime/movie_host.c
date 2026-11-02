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
   over as soon as it is decoded.
   H.264 (Super Mario Odyssey: the 44 .mp4 play guide movies in MovieData, High profile with B-frames, 480x544 at 30 fps)
   goes through the same entries as decoder type 1: VideoToolbox decodes each input (Annex B or length-prefixed NAL units;
   SPS and PPS are taken wherever they appear and build the session), writes NV12 directly, and a reorder buffer of
   AVC_REORDER frames hands them over in presentation order, as MediaCodec does; end of stream flushes it.
   AAC (Mario vs. Donkey Kong: the cutscene .mp4 files carry an AAC track next to the H.264 video) is decoder type 4 to
   16-bit PCM: AudioToolbox's AudioConverter decodes each raw access unit, set up from the AudioSpecificConfig in csd-0.
   Its title-screen movies use H.264 in mode 2 with output format 3, as Luigi's Mansion 3's VP9 (games/ori): the library would
   render into an Android surface, so GetOutputBuffer is handed the game's NVNtexture and the host writes the frame into it
   as packed 8-bit RGBA (VideoToolbox converts to BGRA, swapped to RGBA here). */
#include "runtime.h"
#include "movie_host.h"
int dm_nvn_texture_write_rgba(const void *obj, int w, int h, const void *rgba);   /* hle_nvn.c */
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
#if __has_include(<vorbis/codec.h>)
#include <vorbis/codec.h>
#define HAVE_VORBIS 1
#else
#define HAVE_VORBIS 0
#endif
#include <VideoToolbox/VideoToolbox.h>
#include <AudioToolbox/AudioToolbox.h>

enum { IN_BUFS = 4, IN_CAP = 2 << 20, OUT_BUFS = 8, MAX_MOVIES = 16, IN_MAX = 16, OUT_MAX = 8 };
/* Vorbis (Super Smash Bros. Ultimate's movie sound): the buffers the library's audio decoder has, recorded 23.10. with
   TOTK_MOVIE_TRACE: 14 inputs of 32 KiB, 4 outputs; the game copies each output into its own 8 KiB buffer. */
enum { AUD_IN_BUFS = 14, AUD_IN_CAP = 32 << 10, AUD_OUT_BUFS = 4, AUD_OUT_CAP = 8 << 10 };
enum { TYPE_AVC = 1, TYPE_VP9 = 3, TYPE_AAC = 4, TYPE_VORBIS = 5, MODE_BUFFER = 1, MODE_TEXTURE = 2, FORMAT_PCM16 = 1, FORMAT_RGBA = 3, FORMAT_NV12 = 4, FLAG_EOS = 4 };
enum { AVC_REORDER = 4, AVC_HOLD = 16, AVC_PARAM = 256 };
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
    int n_in, n_out, in_cap, out_cap;        /* this decoder's buffers (video: IN_BUFS x IN_CAP, OUT_BUFS frames; Vorbis: AUD_*) */
    uint8_t *in[IN_MAX]; int in_free[IN_MAX], nin_free;
    Pending pending[IN_MAX]; int npending;
    uint8_t *out[OUT_MAX]; int out_bytes[OUT_MAX], out_held[OUT_MAX];
    Ready ready[OUT_MAX]; int nready;
    Ready decoded[OUT_MAX]; int ndecoded;   /* paced: decoded, not due yet */
    int vorbis, channels, rate, bitrate;     /* Vorbis (decoder type 5): 16-bit PCM out, not paced */
    int aac;                                 /* AAC (decoder type 4) through AudioToolbox: 16-bit PCM out, not paced */
    AudioConverterRef aac_conv; const uint8_t *aac_in; UInt32 aac_in_size; AudioStreamPacketDescription aac_desc; int aac_frames;
    int64_t anchor; uint64_t pcm_frames, packets;   /* time of the first output frame, frames handed out since */
#if HAVE_VORBIS
    vorbis_info vi; vorbis_comment vc; vorbis_dsp_state vd; vorbis_block vb; int vorbis_hdr, vorbis_live;
#endif
    uint64_t t0; int64_t pts0;               /* paced: host time and presentation time of the first frame handed over */
    uint64_t frames, decode_ns, errors, dropped;
    pthread_mutex_t mu;
    int avc;                                 /* H.264 through VideoToolbox (decoder type 1) instead of VP9 */
    VTDecompressionSessionRef vt; CMVideoFormatDescriptionRef vt_fmt;
    uint8_t sps[AVC_PARAM], pps[AVC_PARAM]; int nsps, npps;
    struct { uint8_t *nv12; int64_t time; } held[AVC_HOLD]; int nheld;   /* decoded, waiting for presentation order */
    uint8_t *scratch; size_t scratch_cap;    /* one access unit as 4-byte length-prefixed NAL units */
} Movie;

static MovieGuest G; static int g_ready;
static int g_negative = -1;   /* probe-movie negative controls only: 1 = U and V swapped, 2 = no format-changed event */
static int negative(void) { if (g_negative < 0) { const char *e = getenv("TOTK_PROBE_NEGATIVE"); g_negative = e ? atoi(e) : 0; } return g_negative; }
static uint64_t now_ns(void);
static uint64_t (*g_clock)(void) = now_ns;
void movie_host_set_clock(uint64_t (*clock)(void)) { g_clock = clock ? clock : now_ns; }
/* Breath of the Wild: off by default. The frame pacing holds that game at 30 fps while a movie plays (game_patches_botw.c
   runtime_game_fps_limit), so it takes every frame at the movie's rate as on the console; dropping late frames left the
   first tower's cutscene on a black screen after its last frame, the decoder never stopped (2026-10-20). */
/* Animal Crossing: New Horizons runs at 30 fps (like BOTW): off by default.
   Dropping late frames leaves cutscenes stuck on a black screen after the last frame. */
#define MOVIE_PACE_DEFAULT 0
static int pace_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("TOTK_MOVIE_PACE"); v = e ? strcmp(e, "0") != 0 : MOVIE_PACE_DEFAULT; } return v; }
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

static int frame_bytes(const Movie *m) { return m->format == FORMAT_RGBA ? m->w * m->h * 4 : m->w * m->h + (m->w / 2) * (m->h / 2) * 2; }

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
static int free_out(const Movie *m) { for (int i = 0; i < m->n_out; i++) if (!m->out_held[i]) return i; return -1; }
static void push_ready(Movie *m, Signals *s, int slot, unsigned flags, int64_t time, int bytes) {
    m->out_held[slot] = 1; m->out_bytes[slot] = bytes;
    if (pace_on() && !m->vorbis && !m->aac) {   /* sound is consumed as the game's mixer needs it */ m->decoded[m->ndecoded++] = (Ready){slot, flags, time}; return; }
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
/* ---- H.264 through VideoToolbox ---- */
typedef struct { const uint8_t *p; int n; } Nal;
/* The NAL units of one input: Annex B (start codes) or 4-byte length prefixes; -1 when the lengths do not add up. */
static int split_nals(const uint8_t *d, int n, Nal *out, int max) {
    int k = 0;
    if (n >= 4 && !d[0] && !d[1] && (d[2] == 1 || (!d[2] && d[3] == 1))) {
        int start = -1;
        for (int i = 0; i + 3 <= n;) {
            if (!d[i] && !d[i + 1] && d[i + 2] == 1) {
                if (start >= 0 && k < max) { int end = i; while (end > start && !d[end - 1]) end--; out[k++] = (Nal){d + start, end - start}; }
                i += 3; start = i; continue;
            }
            i++;
        }
        if (start >= 0 && start < n && k < max) out[k++] = (Nal){d + start, n - start};
        return k;
    }
    for (int i = 0; i + 4 <= n && k < max;) {
        uint32_t len = (uint32_t)d[i] << 24 | (uint32_t)d[i + 1] << 16 | (uint32_t)d[i + 2] << 8 | d[i + 3]; i += 4;
        if (!len || len > (uint32_t)(n - i)) return -1;
        out[k++] = (Nal){d + i, (int)len}; i += (int)len;
    }
    return k;
}
static void avc_close(Movie *m) {
    if (m->vt) { VTDecompressionSessionWaitForAsynchronousFrames(m->vt); VTDecompressionSessionInvalidate(m->vt); CFRelease(m->vt); m->vt = NULL; }
    if (m->vt_fmt) { CFRelease(m->vt_fmt); m->vt_fmt = NULL; }
    for (int i = 0; i < m->nheld; i++) free(m->held[i].nv12);
    m->nheld = 0;
}
/* VideoToolbox output (synchronous: inside VTDecompressionSessionDecodeFrame, under the decoder's lock): copy the NV12
   planes into a held frame. */
static void avc_output(void *ref, void *frame, OSStatus st, VTDecodeInfoFlags info, CVImageBufferRef img, CMTime pts, CMTime dur) {
    Movie *m = ref; (void)frame; (void)info; (void)dur;
    if (st != noErr || !img) { m->errors++; m->last_error = ST_ERROR; if (m->errors <= 4) LOG("【红】H.264 decode error %d", (int)st); return; }
    if (m->nheld == AVC_HOLD) runtime_fail("movie decoder: H.264 reorder buffer full");
    CVPixelBufferLockBaseAddress(img, kCVPixelBufferLock_ReadOnly);
    if (m->format == FORMAT_RGBA) {   /* texture mode: BGRA from VideoToolbox, swapped to packed RGBA */
        if ((int)CVPixelBufferGetWidth(img) < m->w || (int)CVPixelBufferGetHeight(img) < m->h || CVPixelBufferIsPlanar(img)) runtime_fail("movie decoder: unexpected H.264 BGRA frame layout");
        uint8_t *f = malloc((size_t)frame_bytes(m)); if (!f) runtime_fail("movie H.264 frame allocation");
        const uint8_t *src = CVPixelBufferGetBaseAddress(img); size_t rs = CVPixelBufferGetBytesPerRow(img);
        for (int r = 0; r < m->h; r++) {
            const uint8x16x4_t *in = (const void *)(src + r * rs); uint8_t *o = f + (size_t)r * m->w * 4; int x = 0;
            for (; x + 16 <= m->w; x += 16) { uint8x16x4_t v = vld4q_u8((const uint8_t *)in + x * 4); uint8x16_t b = v.val[0]; v.val[0] = v.val[2]; v.val[2] = b; vst4q_u8(o + x * 4, v); }
            for (; x < m->w; x++) { const uint8_t *q = src + r * rs + x * 4; o[4 * x] = q[2]; o[4 * x + 1] = q[1]; o[4 * x + 2] = q[0]; o[4 * x + 3] = q[3]; }
        }
        CVPixelBufferUnlockBaseAddress(img, kCVPixelBufferLock_ReadOnly);
        m->held[m->nheld].nv12 = f; m->held[m->nheld++].time = CMTimeConvertScale(pts, 1000000, kCMTimeRoundingMethod_Default).value;
        return;
    }
    if ((int)CVPixelBufferGetWidth(img) < m->w || (int)CVPixelBufferGetHeight(img) < m->h || CVPixelBufferGetPlaneCount(img) != 2) {
        LOG("【红】H.264 frame %zux%zu with %zu planes for the configured %dx%d NV12", CVPixelBufferGetWidth(img), CVPixelBufferGetHeight(img), CVPixelBufferGetPlaneCount(img), m->w, m->h);
        runtime_fail("movie decoder: unexpected H.264 frame layout");
    }
    uint8_t *f = malloc((size_t)frame_bytes(m)); if (!f) runtime_fail("movie H.264 frame allocation");
    const uint8_t *y = CVPixelBufferGetBaseAddressOfPlane(img, 0), *uv = CVPixelBufferGetBaseAddressOfPlane(img, 1);
    size_t ys = CVPixelBufferGetBytesPerRowOfPlane(img, 0), uvs = CVPixelBufferGetBytesPerRowOfPlane(img, 1);
    for (int r = 0; r < m->h; r++) memcpy(f + (size_t)r * m->w, y + r * ys, (size_t)m->w);
    for (int r = 0; r < m->h / 2; r++) memcpy(f + (size_t)m->w * m->h + (size_t)r * m->w, uv + r * uvs, (size_t)m->w);
    CVPixelBufferUnlockBaseAddress(img, kCVPixelBufferLock_ReadOnly);
    m->held[m->nheld].nv12 = f; m->held[m->nheld++].time = CMTimeConvertScale(pts, 1000000, kCMTimeRoundingMethod_Default).value;
}
static int avc_session(Movie *m) {
    if (m->vt) return 1;
    if (!m->nsps || !m->npps) return 0;
    const uint8_t *sets[2] = {m->sps, m->pps}; size_t sizes[2] = {(size_t)m->nsps, (size_t)m->npps};
    OSStatus st = CMVideoFormatDescriptionCreateFromH264ParameterSets(kCFAllocatorDefault, 2, sets, sizes, 4, &m->vt_fmt);
    if (st) { LOG("【红】H.264 parameter sets rejected: %d", (int)st); runtime_fail("movie decoder: H.264 parameter sets"); }
    CFMutableDictionaryRef attrs = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    int pf = m->format == FORMAT_RGBA ? kCVPixelFormatType_32BGRA : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange; CFNumberRef n = CFNumberCreate(NULL, kCFNumberSInt32Type, &pf);
    CFDictionarySetValue(attrs, kCVPixelBufferPixelFormatTypeKey, n); CFRelease(n);
    VTDecompressionOutputCallbackRecord cb = {avc_output, m};
    st = VTDecompressionSessionCreate(NULL, m->vt_fmt, NULL, attrs, &cb, &m->vt); CFRelease(attrs);
    if (st) { LOG("【红】VideoToolbox H.264 session: %d", (int)st); runtime_fail("movie decoder: VideoToolbox H.264 session"); }
    LOG("VideoToolbox H.264 session for %dx%d", m->w, m->h);
    return 1;
}
/* An SPS (7) or PPS (8) NAL unit replaces the kept one; returns 1 when it changed. */
static int avc_param(Movie *m, const uint8_t *p, int n) {
    int type = p[0] & 31; uint8_t *dst = type == 7 ? m->sps : m->pps; int *len = type == 7 ? &m->nsps : &m->npps;
    if (n > AVC_PARAM) runtime_fail("movie decoder: H.264 parameter set too large");
    if (*len == n && !memcmp(dst, p, (size_t)n)) return 0;
    memcpy(dst, p, (size_t)n); *len = n; return 1;
}
/* The format's codec-specific data: csd-0 / csd-1 as MediaCodec has them (Annex B parameter sets), or the MP4 avcC record
   (version 1, then SPS and PPS each with a 16-bit length) the movie library keeps under "avcc". Super Mario Odyssey's
   movies carry their SPS and PPS only there (avcc, 61 bytes; the first input is an IDR slice without them). */
static void avc_codec_data(Movie *m, const void *format) {
    static bool (*find_buffer)(const void *, const char *, void **); static void *(*buffer_data)(const void *); static size_t (*buffer_size)(const void *);
    if (!find_buffer) {
        need_symbol((void **)&buffer_data, "_ZNK5movie6Buffer4DataEv"); need_symbol((void **)&buffer_size, "_ZNK5movie6Buffer4SizeEv");
        need_symbol((void **)&find_buffer, "_ZNK5movie9MediaData10FindBufferEPKcPPNS_6BufferE");
    }
    static const char *const keys[] = {"csd-0", "csd-1", "csd-2", "avcc", "avcC"};   /* the movie library's own key names */
    for (int k = 0; k < 5; k++) {
        void *buf = NULL; if (!find_buffer(format, keys[k], &buf) || !buf) continue;
        const uint8_t *d = buffer_data(buf); int n = (int)buffer_size(buf); if (!d || n <= 0) continue;
        if (n > 7 && d[0] == 1) {   /* avcC */
            int at = 5, count = d[at++] & 31;
            for (int pass = 0; pass < 2; pass++) {
                for (int i = 0; i < count && at + 2 <= n; i++) { int len = d[at] << 8 | d[at + 1]; at += 2; if (len <= 0 || at + len > n) break; avc_param(m, d + at, len); at += len; }
                if (!pass && at < n) count = d[at++];
            }
        } else {
            Nal nals[8]; int c = split_nals(d, n, nals, 8);
            for (int i = 0; i < c; i++) if ((nals[i].p[0] & 31) == 7 || (nals[i].p[0] & 31) == 8) avc_param(m, nals[i].p, nals[i].n);
        }
    }
    LOG("H.264 parameter sets from the format: SPS %d bytes, PPS %d bytes", m->nsps, m->npps);
}
/* One input: parameter sets update the session, the other NAL units (access unit delimiters dropped) are one sample. */
static void avc_decode(Movie *m, const uint8_t *d, int n, int64_t time) {
    Nal nals[128]; int k = split_nals(d, n, nals, 128);
    if (k < 0) { m->errors++; m->last_error = ST_ERROR; if (m->errors <= 4) LOG("【红】H.264 input of %d bytes is neither Annex B nor length-prefixed", n); return; }
    size_t bytes = 0; int changed = 0;
    for (int i = 0; i < k; i++) {
        int type = nals[i].p[0] & 31;
        if (type == 7 || type == 8) changed |= avc_param(m, nals[i].p, nals[i].n);
        else if (type != 9) bytes += 4 + (size_t)nals[i].n;
    }
    if (changed && m->vt) { VTDecompressionSessionWaitForAsynchronousFrames(m->vt); VTDecompressionSessionInvalidate(m->vt); CFRelease(m->vt); m->vt = NULL; CFRelease(m->vt_fmt); m->vt_fmt = NULL; }
    if (!bytes) return;
    if (!avc_session(m)) { m->errors++; m->last_error = ST_ERROR; if (m->errors <= 4) LOG("【红】H.264 slice before its SPS and PPS"); return; }
    if (m->scratch_cap < bytes) { free(m->scratch); m->scratch = malloc(bytes); m->scratch_cap = bytes; if (!m->scratch) runtime_fail("movie H.264 sample buffer"); }
    size_t at = 0;
    for (int i = 0; i < k; i++) {
        int type = nals[i].p[0] & 31; if (type == 7 || type == 8 || type == 9) continue;
        uint32_t len = (uint32_t)nals[i].n; uint8_t *o = m->scratch + at;
        o[0] = (uint8_t)(len >> 24); o[1] = (uint8_t)(len >> 16); o[2] = (uint8_t)(len >> 8); o[3] = (uint8_t)len;
        memcpy(o + 4, nals[i].p, len); at += 4 + len;
    }
    CMBlockBufferRef block = NULL; CMSampleBufferRef sample = NULL;
    CMSampleTimingInfo timing = {.duration = kCMTimeInvalid, .presentationTimeStamp = CMTimeMake(time, 1000000), .decodeTimeStamp = kCMTimeInvalid};
    if (CMBlockBufferCreateWithMemoryBlock(NULL, m->scratch, bytes, kCFAllocatorNull, NULL, 0, bytes, 0, &block) ||
        CMSampleBufferCreateReady(NULL, block, m->vt_fmt, 1, 1, &timing, 1, &bytes, &sample)) runtime_fail("movie decoder: H.264 sample buffer");
    VTDecodeInfoFlags info = 0;
    OSStatus st = VTDecompressionSessionDecodeFrame(m->vt, sample, 0, NULL, &info);
    CFRelease(sample); CFRelease(block);
    if (st) { m->errors++; m->last_error = ST_ERROR; if (m->errors <= 4) LOG("【红】VideoToolbox DecodeFrame: %d at %lld us", (int)st, (long long)time); }
}
/* Hand held frames over in presentation order: all of them at end of stream, else those beyond the reorder depth. */
static void avc_emit(Movie *m, Signals *s, int all) {
    while (m->nheld > (all ? 0 : AVC_REORDER)) {
        int best = 0; for (int i = 1; i < m->nheld; i++) if (m->held[i].time < m->held[best].time) best = i;
        int slot = free_out(m); if (slot < 0) return;
        memcpy(m->out[slot], m->held[best].nv12, (size_t)frame_bytes(m)); free(m->held[best].nv12);
        int64_t time = m->held[best].time; m->held[best] = m->held[--m->nheld];
        if (!m->format_sent) { m->format_sent = 1; if (negative() != 2) note(m, s, EV_FORMAT); }
        push_ready(m, s, slot, 0, time, frame_bytes(m)); m->frames++;
    }
}
static int avc_decode_one(Movie *m, Signals *s, const Pending *p) {
    int eos = p->flags & FLAG_EOS && !m->eos_in, need = eos ? m->nheld + (p->size > 0) + 1 : p->size > 0;
    int freeslots = 0; for (int i = 0; i < m->n_out; i++) freeslots += !m->out_held[i];
    if (freeslots < need) return 0;
    if (p->size > 0) {
        uint64_t t0 = now_ns(); avc_decode(m, m->in[p->index] + p->offset, p->size, p->time); m->decode_ns += now_ns() - t0;
        if (m->frames && m->frames % 300 == 0) LOG("%llu frames, decode %.2f ms per frame", (unsigned long long)m->frames, m->decode_ns / 1e6 / (double)m->frames);
    }
    avc_emit(m, s, 0);
    if (eos) {
        if (m->vt) VTDecompressionSessionWaitForAsynchronousFrames(m->vt);
        avc_emit(m, s, 1);
        m->eos_in = 1; int slot = free_out(m); if (slot < 0) runtime_fail("movie decoder: no slot for end of stream");
        push_ready(m, s, slot, FLAG_EOS, p->time, 0);
        LOG("H.264 end of stream after %llu frames (%.2f ms per input, %llu errors)", (unsigned long long)m->frames, m->frames ? m->decode_ns / 1e6 / (double)m->frames : 0.0, (unsigned long long)m->errors);
    }
    m->in_free[m->nin_free++] = p->index; note(m, s, EV_INPUT);
    return 1;
}

static int md_buffer(void *md, const char *key, const uint8_t **data, size_t *size) {
    static int (*find_buffer)(void *, const char *, void **); static size_t (*buf_size)(void *), (*buf_offset)(void *);
    if (!find_buffer) { need_symbol((void **)&find_buffer, "_ZNK5movie9MediaData10FindBufferEPKcPPNS_6BufferE");
                        need_symbol((void **)&buf_size, "_ZNK5movie6Buffer4SizeEv"); need_symbol((void **)&buf_offset, "_ZNK5movie6Buffer6OffsetEv"); }
    void *b = NULL; if (!find_buffer(md, key, &b) || !b) return 0;
    const uint8_t *base = G.buf_base(b); if (!base) return 0;
    *data = base + buf_offset(b); *size = buf_size(b); return *size > 0;
}

/* ---- AAC (decoder type 4) with AudioToolbox ----
   csd-0 is the AudioSpecificConfig (object type, sampling-frequency index, channel configuration); every input is one raw
   AAC access unit with its time. Each unit's samples come out as interleaved 16-bit PCM in one output, timed from the first
   output frame on, as the Vorbis decoder above; the output format is audio/raw with the channel count and rate. */
static void aac_close(Movie *m) { if (m->aac_conv) { AudioConverterDispose(m->aac_conv); m->aac_conv = NULL; } }
static OSStatus aac_feed(AudioConverterRef conv, UInt32 *packets, AudioBufferList *data, AudioStreamPacketDescription **desc, void *ref) {
    (void)conv; Movie *m = ref;
    if (!m->aac_in_size) { *packets = 0; return 1; }   /* this unit is used up: the converter returns what it has */
    data->mNumberBuffers = 1; data->mBuffers[0].mData = (void *)m->aac_in; data->mBuffers[0].mDataByteSize = m->aac_in_size;
    data->mBuffers[0].mNumberChannels = (UInt32)m->channels;
    m->aac_desc = (AudioStreamPacketDescription){0, 0, m->aac_in_size};
    if (desc) *desc = &m->aac_desc;
    *packets = 1; m->aac_in_size = 0;
    return noErr;
}
static int aac_configure(Movie *m, void *md) {
    const uint8_t *asc = NULL; size_t nasc = 0;
    int have_asc = md_buffer(md, "csd-0", &asc, &nasc) && nasc >= 2;
    AudioStreamBasicDescription in = {.mSampleRate = m->rate, .mFormatID = kAudioFormatMPEG4AAC, .mChannelsPerFrame = (UInt32)m->channels,
                                      .mFramesPerPacket = 1024};
    if (have_asc) {
        unsigned object = asc[0] >> 3;
        if (object == 5 || object == 29) in.mFormatID = kAudioFormatMPEG4AAC_HE;   /* SBR (HE-AAC v1 / v2) */
    }
    /* One unit's frames: 1024 for AAC-LC, 2048 with SBR. The output buffers are exactly this long: the game copies a whole
       output buffer (GetOutputBufferSize) into the AudioOut, so a longer one played every unit followed by silence. */
    m->aac_frames = in.mFormatID == kAudioFormatMPEG4AAC_HE ? 2048 : 1024;
    AudioStreamBasicDescription out = {.mSampleRate = m->rate, .mFormatID = kAudioFormatLinearPCM,
                                       .mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked,
                                       .mBytesPerPacket = (UInt32)(2 * m->channels), .mFramesPerPacket = 1, .mBytesPerFrame = (UInt32)(2 * m->channels),
                                       .mChannelsPerFrame = (UInt32)m->channels, .mBitsPerChannel = 16};
    OSStatus st = AudioConverterNew(&in, &out, &m->aac_conv);
    if (st) { LOG("【红】AAC decoder for %d ch %d Hz not available (AudioConverterNew %d)", m->channels, m->rate, (int)st); m->aac_conv = NULL; return 0; }
    if (have_asc) {   /* a bare AudioSpecificConfig is accepted as the decompression cookie; without it LC still decodes */
        st = AudioConverterSetProperty(m->aac_conv, kAudioConverterDecompressionMagicCookie, (UInt32)nasc, asc);
        if (st) LOG("AAC: AudioSpecificConfig (%zu bytes) not taken as a cookie (%d); decoding from the track's rate and channels", nasc, (int)st);
    }
    m->packets = 0;
    return 1;
}
static int aac_decode_one(Movie *m, Signals *s, const Pending *p) {
    int eos = p->flags & FLAG_EOS && !m->eos_in, need = (p->size > 0) + eos;
    int freeslots = 0; for (int i = 0; i < m->n_out; i++) freeslots += !m->out_held[i];
    if (freeslots < need) return 0;
    if (p->size > 0) {
        uint64_t t0 = now_ns();
        int slot = free_out(m); if (slot < 0) runtime_fail("movie decoder: AAC output slots counted wrong");
        m->aac_in = m->in[p->index] + p->offset; m->aac_in_size = (UInt32)p->size;
        UInt32 frames = (UInt32)(m->out_cap / (2 * m->channels));
        AudioBufferList list = {.mNumberBuffers = 1, .mBuffers = {{(UInt32)m->channels, (UInt32)m->out_cap, m->out[slot]}}};
        OSStatus st = AudioConverterFillComplexBuffer(m->aac_conv, aac_feed, m, &frames, &list, NULL);
        m->aac_in_size = 0; m->packets++;
        if (st && st != 1) { m->errors++; if (m->errors <= 4) LOG("【红】AAC unit of %d bytes at %lld us rejected (%d)", p->size, (long long)p->time, (int)st); }
        if (frames > 0) {
            if (m->anchor < 0) m->anchor = p->time;
            if (!m->format_sent) { m->format_sent = 1; note(m, s, EV_FORMAT); }
            push_ready(m, s, slot, 0, m->anchor + (int64_t)(m->pcm_frames * 1000000ULL / (uint64_t)m->rate), (int)(frames * 2 * (UInt32)m->channels));
            m->pcm_frames += frames;
        }
        m->frames++; m->decode_ns += now_ns() - t0;
        if (m->frames % 3000 == 0) LOG("AAC: %llu units, %llu frames (%.1f s), %.3f ms per unit", (unsigned long long)m->frames, (unsigned long long)m->pcm_frames, m->pcm_frames / (double)m->rate, m->decode_ns / 1e6 / (double)m->frames);
    }
    if (eos) {
        m->eos_in = 1; int slot = free_out(m); if (slot < 0) runtime_fail("movie decoder: no slot for end of stream");
        push_ready(m, s, slot, FLAG_EOS, p->time, 0);
        LOG("AAC end of stream after %llu units, %.1f s of sound, %llu errors", (unsigned long long)m->frames, m->pcm_frames / (double)m->rate, (unsigned long long)m->errors);
    }
    m->in_free[m->nin_free++] = p->index; note(m, s, EV_INPUT);
    return 1;
}

/* ---- Vorbis (decoder type 5) with libvorbis ----
   Super Smash Bros. Ultimate's movies carry a Vorbis track. The library decodes it with Android's MediaCodec, which ran
   for ~19 s and then never answered the game's Flush (23.10.: the main thread waited in MediaCodec::flush; other runs
   ran its allocator dry in MatroskaSource::readBlock). Its behaviour, recorded with TOTK_MOVIE_TRACE, is rebuilt here:
   Configure gets the identification header as csd-0 and the setup header as csd-1 (the comment header is not passed);
   every input is one Vorbis packet with its time; every packet's samples come out as interleaved 16-bit PCM in one output
   (2 channels x 1024 frames = 4096 bytes for the long blocks), timed from the first output frame on (the first packet
   only primes the overlap and gives none); the output format is audio/raw with the channel count and rate. */
#if HAVE_VORBIS
static void vorbis_close(Movie *m) {
    if (m->vorbis_live) { vorbis_block_clear(&m->vb); vorbis_dsp_clear(&m->vd); m->vorbis_live = 0; }
    if (m->vorbis_hdr) { vorbis_comment_clear(&m->vc); vorbis_info_clear(&m->vi); m->vorbis_hdr = 0; }
}
static int vorbis_configure(Movie *m, void *md) {
    const uint8_t *id, *setup; size_t nid, nsetup;
    if (!md_buffer(md, "csd-0", &id, &nid) || !md_buffer(md, "csd-1", &setup, &nsetup)) { LOG("【红】Vorbis configure without csd-0 / csd-1"); return 0; }
    static const uint8_t comment[] = {3, 'v', 'o', 'r', 'b', 'i', 's', 0, 0, 0, 0, 0, 0, 0, 0, 1};   /* empty vendor, no comments, framing bit */
    vorbis_info_init(&m->vi); vorbis_comment_init(&m->vc); m->vorbis_hdr = 1;
    ogg_packet op = {.packet = (unsigned char *)id, .bytes = (long)nid, .b_o_s = 1, .packetno = 0};
    int r = vorbis_synthesis_headerin(&m->vi, &m->vc, &op);
    if (!r) { op = (ogg_packet){.packet = (unsigned char *)comment, .bytes = sizeof comment, .packetno = 1}; r = vorbis_synthesis_headerin(&m->vi, &m->vc, &op); }
    if (!r) { op = (ogg_packet){.packet = (unsigned char *)setup, .bytes = (long)nsetup, .packetno = 2}; r = vorbis_synthesis_headerin(&m->vi, &m->vc, &op); }
    if (r) { LOG("【红】Vorbis headers rejected (%d): identification %zu bytes, setup %zu bytes", r, nid, nsetup); return 0; }
    if (m->vi.channels != m->channels || m->vi.rate != m->rate || m->vi.channels < 1 || m->vi.channels > 8) {
        LOG("【红】Vorbis stream %d ch %ld Hz, the track says %d ch %d Hz", m->vi.channels, m->vi.rate, m->channels, m->rate); return 0;
    }
    if (vorbis_synthesis_init(&m->vd, &m->vi) || vorbis_block_init(&m->vd, &m->vb)) { LOG("【红】Vorbis decoder initialisation failed"); return 0; }
    m->vorbis_live = 1; m->packets = 0;
    return 1;
}
static int vorbis_decode_one(Movie *m, Signals *s, const Pending *p) {
    int per = m->out_cap / (2 * m->channels), most = (int)(vorbis_info_blocksize(&m->vi, 1) / 2);
    int eos = p->flags & FLAG_EOS && !m->eos_in, need = (p->size > 0 ? (most + per - 1) / per : 0) + eos;
    int freeslots = 0; for (int i = 0; i < m->n_out; i++) freeslots += !m->out_held[i];
    if (freeslots < need) return 0;
    if (p->size > 0) {
        uint64_t t0 = now_ns();
        ogg_packet op = {.packet = m->in[p->index] + p->offset, .bytes = p->size, .packetno = 3 + (int64_t)m->packets++, .granulepos = -1};
        int r = vorbis_synthesis(&m->vb, &op);
        if (!r) r = vorbis_synthesis_blockin(&m->vd, &m->vb);
        if (r) { m->errors++; if (m->errors <= 4) LOG("【红】Vorbis packet of %d bytes at %lld us rejected (%d)", p->size, (long long)p->time, r); }
        float **pcm; int n;
        while ((n = vorbis_synthesis_pcmout(&m->vd, &pcm)) > 0) {
            if (m->anchor < 0) m->anchor = p->time;
            for (int done = 0; done < n;) {
                int k = n - done < per ? n - done : per, slot = free_out(m);
                if (slot < 0) runtime_fail("movie decoder: Vorbis output slots counted wrong");
                int16_t *o = (int16_t *)m->out[slot];
                for (int f = 0; f < k; f++) for (int c = 0; c < m->channels; c++) {
                    float v = pcm[c][done + f] * 32768.0f;
                    o[f * m->channels + c] = (int16_t)(v >= 32767.0f ? 32767 : v <= -32768.0f ? -32768 : (int)lrintf(v));
                }
                if (!m->format_sent) { m->format_sent = 1; note(m, s, EV_FORMAT); }
                push_ready(m, s, slot, 0, m->anchor + (int64_t)(m->pcm_frames * 1000000ULL / (uint64_t)m->rate), k * m->channels * 2);
                m->pcm_frames += (uint64_t)k; done += k;
            }
            vorbis_synthesis_read(&m->vd, n);
        }
        m->frames++; m->decode_ns += now_ns() - t0;
        if (m->frames % 3000 == 0) LOG("Vorbis: %llu packets, %llu frames (%.1f s), %.3f ms per packet", (unsigned long long)m->frames, (unsigned long long)m->pcm_frames, m->pcm_frames / (double)m->rate, m->decode_ns / 1e6 / (double)m->frames);
    }
    if (eos) {
        m->eos_in = 1; int slot = free_out(m); if (slot < 0) runtime_fail("movie decoder: no slot for end of stream");
        push_ready(m, s, slot, FLAG_EOS, p->time, 0);
        LOG("Vorbis end of stream after %llu packets, %.1f s of sound, %llu errors", (unsigned long long)m->frames, m->pcm_frames / (double)m->rate, (unsigned long long)m->errors);
    }
    m->in_free[m->nin_free++] = p->index; note(m, s, EV_INPUT);
    return 1;
}
#else
static void vorbis_close(Movie *m) { (void)m; }
#endif

/* Decode one input into output slots; returns 0 when it had to wait for a free output slot. */
static int decode_one(Movie *m, Signals *s, const Pending *p) {
    if (m->avc) return avc_decode_one(m, s, p);
    if (m->aac) return aac_decode_one(m, s, p);
#if HAVE_VORBIS
    if (m->vorbis) return vorbis_decode_one(m, s, p);
#endif
    int need = p->size > 0 ? 1 : 0;
    if (p->flags & FLAG_EOS && !m->eos_in) need++;
    int freeslots = 0; for (int i = 0; i < m->n_out; i++) freeslots += !m->out_held[i];
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
    avc_close(m); m->nsps = m->npps = 0; free(m->scratch); m->scratch = NULL; m->scratch_cap = 0;
    vorbis_close(m); aac_close(m);
    for (int i = 0; i < IN_MAX; i++) { free(m->in[i]); m->in[i] = NULL; }
    for (int i = 0; i < OUT_MAX; i++) { free(m->out[i]); m->out[i] = NULL; }
    m->configured = m->started = 0; m->nin_free = m->npending = m->nready = m->ndecoded = 0; m->t0 = 0;
}
static void reset_queues(Movie *m) {
    m->nin_free = 0; for (int i = 0; i < m->n_in; i++) m->in_free[m->nin_free++] = i;
    m->anchor = -1; m->pcm_frames = 0;
    memset(m->out_held, 0, sizeof m->out_held); m->npending = m->nready = m->ndecoded = 0; m->eos_in = 0; m->t0 = 0;
}

/* ---- TOTK_MOVIE_TRACE=1 (diagnostics): the calls the game makes on decoders left to the library, with arguments, results,
   the keys of every MediaData passed in or out and the size and first bytes of every buffer, to rebuild a decoder type on
   the host from what the library really does (Super Smash Bros. Ultimate's Vorbis track, decoder type 5). ---- */
static void *g_traced[MAX_MOVIES]; static int g_traced_type[MAX_MOVIES];
static int trace_on(void) { static int v = -1; if (v < 0) v = getenv("TOTK_MOVIE_TRACE") != NULL; return v; }
static int traced(const void *obj) { if (!trace_on()) return 0; for (int i = 0; i < MAX_MOVIES; i++) if (g_traced[i] == obj) return g_traced_type[i]; return 0; }
static void trace_md(const char *what, void *md) {
    if (!md) { LOG("[trace] %s: (null)", what); return; }
    static int (*find_int64)(void *, const char *, int64_t *), (*find_buffer)(void *, const char *, void **);
    static size_t (*buf_size)(void *), (*buf_offset)(void *);
    if (!find_int64) { need_symbol((void **)&find_int64, "_ZNK5movie9MediaData9FindInt64EPKcPl"); need_symbol((void **)&find_buffer, "_ZNK5movie9MediaData10FindBufferEPKcPPNS_6BufferE");
                       need_symbol((void **)&buf_size, "_ZNK5movie6Buffer4SizeEv"); need_symbol((void **)&buf_offset, "_ZNK5movie6Buffer6OffsetEv"); }
    static const char *const keys[] = {"mime", "channel-count", "sample-rate", "durationUs", "max-input-size", "pcm-encoding", "bitrate", "bit-rate",
        "csd-0", "csd-1", "csd-2", "channel-mask", "encoder-delay", "encoder-padding", "is-adts", "aac-profile", "frame-size", "max-output-size",
        "output-buffer-size", "buffer-size", "num-buffers", "time-us", "timeUs", "is-sync-frame", "track-id", "language"};
    char line[2048]; size_t o = 0;
    for (size_t k = 0; k < sizeof keys / sizeof *keys && o < sizeof line - 128; k++) {
        const char *s = NULL; int i32 = 0; int64_t i64 = 0; void *b = NULL;
        if (G.md_find_string(md, keys[k], &s) && s) o += (size_t)snprintf(line + o, sizeof line - o, " %s=\"%s\"", keys[k], s);
        else if (G.md_find_int32(md, keys[k], &i32)) o += (size_t)snprintf(line + o, sizeof line - o, " %s=%d", keys[k], i32);
        else if (find_int64(md, keys[k], &i64)) o += (size_t)snprintf(line + o, sizeof line - o, " %s=%lld", keys[k], (long long)i64);
        else if (find_buffer(md, keys[k], &b) && b) {
            const uint8_t *p = G.buf_base(b); size_t n = buf_size(b), off = buf_offset(b);
            o += (size_t)snprintf(line + o, sizeof line - o, " %s=buf[%zu@%zu:", keys[k], n, off);
            for (size_t j = 0; p && j < n && j < 12 && o < sizeof line - 8; j++) o += (size_t)snprintf(line + o, sizeof line - o, "%02x", p[off + j]);
            o += (size_t)snprintf(line + o, sizeof line - o, "]");
        }
    }
    line[o] = 0; LOG("[trace] %s:%s", what, line);
}
static void trace_buf(const char *what, void *buf) {
    static size_t (*buf_size)(void *), (*buf_offset)(void *);
    if (!buf_size) { need_symbol((void **)&buf_size, "_ZNK5movie6Buffer4SizeEv"); need_symbol((void **)&buf_offset, "_ZNK5movie6Buffer6OffsetEv"); }
    if (!buf) { LOG("[trace] %s: buffer (null)", what); return; }
    const uint8_t *p = G.buf_base(buf); size_t n = buf_size(buf), off = buf_offset(buf), cap = G.buf_capacity(buf);
    char hex[80]; size_t o = 0; for (size_t j = 0; p && j < n && j < 16; j++) o += (size_t)snprintf(hex + o, sizeof hex - o, "%02x", p[off + j]); hex[o] = 0;
    LOG("[trace] %s: size %zu offset %zu capacity %zu base %p head %s", what, n, off, cap, (const void *)p, hex);
}

/* ---- entries ---- */
typedef void (*ctor_fn)(void *, int, int, int, int);
static void h_ctor(void *obj, int type, int format, int mode, int priority) {
    if (!g_ready) guest_init();
    static int vorbis_off = -1; if (vorbis_off < 0) { const char *e = getenv("TOTK_MOVIE_VORBIS"); vorbis_off = e && !strcmp(e, "0"); }   /* 0: the library's Vorbis (A/B) */
    int host_vorbis = HAVE_VORBIS && !vorbis_off && type == TYPE_VORBIS && format == FORMAT_PCM16;
    int host_aac = type == TYPE_AAC && format == FORMAT_PCM16;
    int host_texture = type == TYPE_AVC && mode == MODE_TEXTURE && format == FORMAT_RGBA;
    if ((type != TYPE_VP9 && type != TYPE_AVC && !host_vorbis && !host_aac) || (mode != MODE_BUFFER && !host_texture)) {
        LOG("decoder type %d mode %d format %d: left to the library (VP9, H.264, and AAC / Vorbis to 16-bit PCM in buffer mode run on the host)", type, mode, format);
        ((ctor_fn)G.orig[MOVIE_CTOR])(obj, type, format, mode, priority);
        if (trace_on()) { pthread_mutex_lock(&g_lock); for (int i = 0; i < MAX_MOVIES; i++) if (!g_traced[i] || g_traced[i] == obj) { g_traced[i] = obj; g_traced_type[i] = type; break; } pthread_mutex_unlock(&g_lock);
                          LOG("[trace] %p ctor type %d format %d mode %d priority %d", obj, type, format, mode, priority); }
        return;
    }
    Movie *m = calloc(1, sizeof *m); if (!m) runtime_fail("movie decoder allocation");
    m->guest = obj; m->format = format; m->avc = type == TYPE_AVC; m->vorbis = type == TYPE_VORBIS; m->aac = type == TYPE_AAC; m->anchor = -1; pthread_mutex_init(&m->mu, NULL);
    pthread_mutex_lock(&g_lock); int i = 0;
    while (i < MAX_MOVIES && g_movies[i] && g_movies[i]->guest != obj) i++;
    if (i == MAX_MOVIES) runtime_fail("movie decoder table full");
    if (g_movies[i]) { LOG("【红】decoder object %p reused without destruction", obj); release_codec(g_movies[i]); free(g_movies[i]); }
    g_movies[i] = m; pthread_mutex_unlock(&g_lock);
    ((void **)obj)[0] = G.vptr; ((void **)obj)[1] = m;
    if (m->avc) LOG("H.264 decoder %p on the host (VideoToolbox), mode %d, output format %d", obj, mode, format);
    else if (m->aac) LOG("AAC decoder %p on the host (AudioToolbox), 16-bit PCM", obj);
#if HAVE_VORBIS
    else if (m->vorbis) LOG("Vorbis decoder %p on the host (%s), 16-bit PCM", obj, vorbis_version_string());
#endif
    else LOG("VP9 decoder %p on the host (libvpx %s), output format %d", obj, vpx_codec_version_str(), format);
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
    Movie *m = find(obj);
    if (!m) { if (traced(obj)) trace_md("configure in", format); int r = ((md_fn)G.orig[MOVIE_CONFIGURE])(obj, format); if (traced(obj)) LOG("[trace] %p configure -> %#x", obj, r); return r; }
    if (!format) return ST_NULL;
    if (m->aac) {
        const char *mime = NULL; int ch = 0, rate = 0, br = 0;
        G.md_find_string(format, "mime", &mime); G.md_find_int32(format, "channel-count", &ch); G.md_find_int32(format, "sample-rate", &rate); G.md_find_int32(format, "bit-rate", &br);
        if (!mime || (strcmp(mime, "audio/mp4a-latm") && strcmp(mime, "audio/aac")) || ch < 1 || ch > 8 || rate < 8000 || rate > 96000) {
            LOG("【红】configure: mime %s %d ch %d Hz not supported", mime ? mime : "(none)", ch, rate); return ST_UNSUPPORTED;
        }
        pthread_mutex_lock(&m->mu);
        release_codec(m); m->channels = ch; m->rate = rate; m->bitrate = br;
        if (!aac_configure(m, format)) { release_codec(m); pthread_mutex_unlock(&m->mu); return ST_UNSUPPORTED; }
        m->n_in = AUD_IN_BUFS; m->in_cap = AUD_IN_CAP; m->n_out = AUD_OUT_BUFS; m->out_cap = m->aac_frames * 2 * ch;
        for (int i = 0; i < m->n_in; i++) if (!(m->in[i] = malloc((size_t)m->in_cap))) runtime_fail("movie input buffers");
        for (int i = 0; i < m->n_out; i++) if (!(m->out[i] = malloc((size_t)m->out_cap))) runtime_fail("movie output buffers");
        reset_queues(m); m->format_sent = 0; m->configured = 1; m->last_error = 0;
        pthread_mutex_unlock(&m->mu);
        LOG("configured AAC %d ch %d Hz, %d input buffers of %d KiB, %d outputs of %d bytes", ch, rate, m->n_in, m->in_cap >> 10, m->n_out, m->out_cap);
        return ST_OK;
    }
#if HAVE_VORBIS
    if (m->vorbis) {
        const char *mime = NULL; int ch = 0, rate = 0, br = 0;
        G.md_find_string(format, "mime", &mime); G.md_find_int32(format, "channel-count", &ch); G.md_find_int32(format, "sample-rate", &rate); G.md_find_int32(format, "bit-rate", &br);
        if (!mime || strcmp(mime, "audio/vorbis") || ch < 1 || ch > 8 || rate < 8000 || rate > 192000) {
            LOG("【红】configure: mime %s %d ch %d Hz not supported", mime ? mime : "(none)", ch, rate); return ST_UNSUPPORTED;
        }
        pthread_mutex_lock(&m->mu);
        release_codec(m); m->channels = ch; m->rate = rate; m->bitrate = br;
        if (!vorbis_configure(m, format)) { release_codec(m); pthread_mutex_unlock(&m->mu); return ST_UNSUPPORTED; }
        m->n_in = AUD_IN_BUFS; m->in_cap = AUD_IN_CAP; m->n_out = AUD_OUT_BUFS; m->out_cap = AUD_OUT_CAP;
        for (int i = 0; i < m->n_in; i++) if (!(m->in[i] = malloc((size_t)m->in_cap))) runtime_fail("movie input buffers");
        for (int i = 0; i < m->n_out; i++) if (!(m->out[i] = malloc((size_t)m->out_cap))) runtime_fail("movie output buffers");
        reset_queues(m); m->format_sent = 0; m->configured = 1; m->last_error = 0;
        pthread_mutex_unlock(&m->mu);
        LOG("configured Vorbis %d ch %d Hz (blocks %d / %d), %d input buffers of %d KiB, %d outputs of %d bytes", ch, rate,
            vorbis_info_blocksize(&m->vi, 0), vorbis_info_blocksize(&m->vi, 1), m->n_in, m->in_cap >> 10, m->n_out, m->out_cap);
        return ST_OK;
    }
#endif
    if (m->format != FORMAT_NV12 && !(m->avc && m->format == FORMAT_RGBA)) { LOG("【红】output format %d not implemented on the host (NV12 = 4; RGBA = 3 for H.264)", m->format); return ST_UNSUPPORTED; }
    const char *mime = NULL; int w = 0, h = 0;
    G.md_find_string(format, "mime", &mime); G.md_find_int32(format, "width", &w); G.md_find_int32(format, "height", &h);
    if (!mime || strcmp(mime, m->avc ? "video/avc" : "video/x-vnd.on2.vp9") || w <= 0 || h <= 0 || w > 8192 || h > 8192 || (w | h) & 1) {
        LOG("【红】configure: mime %s %dx%d not supported", mime ? mime : "(none)", w, h); return ST_UNSUPPORTED;
    }
    pthread_mutex_lock(&m->mu);
    release_codec(m); m->w = w; m->h = h;
    if (m->avc) avc_codec_data(m, format);   /* the session is made at the first slice (avc_session) */
    else {
        vpx_codec_dec_cfg_t cfg = {.threads = 4, .w = (unsigned)w, .h = (unsigned)h};
        if (vpx_codec_dec_init(&m->vpx, vpx_codec_vp9_dx(), &cfg, 0) != VPX_CODEC_OK) runtime_fail("libvpx VP9 decoder initialisation");
        m->vpx_live = 1;
    }
    m->n_in = IN_BUFS; m->in_cap = IN_CAP; m->n_out = OUT_BUFS; m->out_cap = frame_bytes(m);
    for (int i = 0; i < m->n_in; i++) if (!(m->in[i] = malloc((size_t)m->in_cap))) runtime_fail("movie input buffers");
    for (int i = 0; i < m->n_out; i++) if (!(m->out[i] = malloc((size_t)m->out_cap))) runtime_fail("movie output buffers");
    reset_queues(m); m->format_sent = 0; m->configured = 1; m->last_error = 0;
    pthread_mutex_unlock(&m->mu);
    LOG("configured %dx%d %s, %d input buffers of %d KiB, %d output frames of %d bytes", w, h, m->avc ? "H.264" : "VP9", IN_BUFS, IN_CAP >> 10, OUT_BUFS, frame_bytes(m));
    return ST_OK;
}
static int h_register_events(void *obj, void *events) {
    Movie *m = find(obj);
    if (!m) { if (traced(obj)) LOG("[trace] %p register events %p", obj, events); return ((md_fn)G.orig[MOVIE_REGISTER_EVENTS])(obj, events); }
    m->events = events; return ST_OK;
}
static void set_format(Movie *m, void *md) {
    if (m->vorbis || m->aac) {   /* what the library's Vorbis decoder reports (recorded): mime, channels, rate, the track's bit rate */
        G.md_set_string(md, "mime", "audio/raw"); G.md_set_int32(md, "channel-count", m->channels);
        G.md_set_int32(md, "sample-rate", m->rate); G.md_set_int32(md, "bit-rate", m->bitrate); return;
    }
    G.md_set_string(md, "mime", "video/raw");
    G.md_set_int32(md, "width", m->w); G.md_set_int32(md, "height", m->h);
    G.md_set_int32(md, "stride", m->format == FORMAT_RGBA ? m->w * 4 : m->w); G.md_set_int32(md, "slice-height", m->h);
    G.md_set_int32(md, "color-format", m->format == FORMAT_RGBA ? 0x7f00a000 : 21);   /* COLOR_Format32bitABGR8888 / COLOR_FormatYUV420SemiPlanar */
    G.md_set_rect(md, "crop", 0, 0, m->w - 1, m->h - 1);
    G.md_set_int32(md, "crop-width", m->w); G.md_set_int32(md, "crop-height", m->h);
    G.md_set_int32(md, "nv12-y-stride", m->w); G.md_set_int32(md, "nv12-uv-stride", m->w);
    G.md_set_int32(md, "nv12-y-offset", 0); G.md_set_int32(md, "nv12-uv-offset", m->w * m->h);
}
static int h_get_output_format(void *obj, void *md) {
    Movie *m = find(obj);
    if (!m) { int r = ((md_fn)G.orig[MOVIE_GET_OUTPUT_FORMAT])(obj, md); if (traced(obj)) { LOG("[trace] %p output format -> %#x", obj, r); trace_md("output format", md); } return r; }
    if (!md) return ST_NULL;
    if (!m->configured) return ST_ERROR;
    set_format(m, md); return ST_OK;
}
typedef int (*int_fn)(void *);
static int h_start(void *obj) {
    Movie *m = find(obj);
    if (!m) { int t = traced(obj); uint64_t t0 = now_ns(); if (t) LOG("[trace] %p start ...", obj); int r = ((int_fn)G.orig[MOVIE_START])(obj); if (t) LOG("[trace] %p start -> %#x after %.1f ms", obj, r, (now_ns() - t0) / 1e6); return r; }
    Signals s = {0};
    pthread_mutex_lock(&m->mu);
    if (!m->configured) { pthread_mutex_unlock(&m->mu); return ST_NOT_CONFIGURED; }
    m->started = 1; note(m, &s, EV_INPUT);
    pthread_mutex_unlock(&m->mu); fire(&s);
    return ST_OK;
}
static int h_stop(void *obj) {
    Movie *m = find(obj);
    if (!m) { int t = traced(obj); uint64_t t0 = now_ns(); if (t) LOG("[trace] %p stop ...", obj); int r = ((int_fn)G.orig[MOVIE_STOP])(obj); if (t) LOG("[trace] %p stop -> %#x after %.1f ms", obj, r, (now_ns() - t0) / 1e6); return r; }
    pthread_mutex_lock(&m->mu); LOG("stop after %llu frames (%llu dropped to keep time)", (unsigned long long)m->frames, (unsigned long long)m->dropped); release_codec(m); pthread_mutex_unlock(&m->mu);
    return ST_OK;
}
static int h_reset(void *obj) {
    Movie *m = find(obj);
    if (!m) { int t = traced(obj); uint64_t t0 = now_ns(); if (t) LOG("[trace] %p reset ...", obj); int r = ((int_fn)G.orig[MOVIE_RESET])(obj); if (t) LOG("[trace] %p reset -> %#x after %.1f ms", obj, r, (now_ns() - t0) / 1e6); return r; }
    pthread_mutex_lock(&m->mu); release_codec(m); m->last_error = 0; pthread_mutex_unlock(&m->mu);
    return ST_OK;
}
static int h_flush(void *obj) {
    Movie *m = find(obj);
    if (!m) { int t = traced(obj); uint64_t t0 = now_ns(); if (t) LOG("[trace] %p flush ...", obj); int r = ((int_fn)G.orig[MOVIE_FLUSH])(obj); if (t) LOG("[trace] %p flush -> %#x after %.1f ms", obj, r, (now_ns() - t0) / 1e6); return r; }
    pthread_mutex_lock(&m->mu);
    if (m->configured && m->avc) { avc_close(m); reset_queues(m); }   /* the parameter sets stay for the next key frame */
    else if (m->configured && m->aac) { AudioConverterReset(m->aac_conv); reset_queues(m); }
#if HAVE_VORBIS
    else if (m->configured && m->vorbis) { LOG("Vorbis flush after %llu packets, %.1f s of sound", (unsigned long long)m->packets, m->pcm_frames / (double)m->rate); vorbis_synthesis_restart(&m->vd); reset_queues(m); }
#endif
    else if (m->configured) {   /* drop queued work; the next input starts a new decode (after a seek: a key frame) */
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
    Movie *m = find(obj);
    if (!m) { int r = ((pp_fn)G.orig[MOVIE_ACQUIRE_INPUT])(obj, index, remaining); static unsigned n; if (traced(obj) && (++n <= 30 || n % 500 == 0)) LOG("[trace] %p acquire input #%u -> %#x index %d remaining %d", obj, n, r, index ? *index : -9, remaining ? *remaining : -9); return r; }
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
    Movie *m = find(obj);
    if (!m) { int r = ((ib_fn)G.orig[MOVIE_GET_INPUT])(obj, index, buf); static unsigned n; if (traced(obj) && ++n <= 8) { LOG("[trace] %p get input %d -> %#x", obj, index, r); trace_buf("input buffer", buf); } return r; }
    if (!buf) return ST_NULL;
    if (index < 0 || index >= m->n_in || !m->in[index]) { G.buf_set_range(buf, 0, 0); G.buf_set_data(buf, NULL, 0); return ST_ERROR; }
    G.buf_set_range(buf, 0, m->in_cap); G.buf_set_data(buf, m->in[index], (size_t)m->in_cap);
    return ST_OK;
}
typedef int (*send_fn)(void *, int, int, int, int64_t, unsigned);
static int h_send_input(void *obj, int index, int offset, int size, int64_t time, unsigned flags) {
    Movie *m = find(obj);
    if (!m) { int r = ((send_fn)G.orig[MOVIE_SEND_INPUT])(obj, index, offset, size, time, flags); static unsigned n; if (traced(obj) && (++n <= 40 || n % 500 == 0 || flags)) LOG("[trace] %p send input #%u index %d offset %d size %d time %lld flags %#x -> %#x", obj, n, index, offset, size, (long long)time, flags, r); return r; }
    if (index < 0 || index >= m->n_in || offset < 0 || size < 0 || (int64_t)offset + size > m->in_cap) return ST_ERROR;
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
    Movie *m = find(obj);
    if (!m) { int r = ((acq_fn)G.orig[MOVIE_ACQUIRE_OUTPUT])(obj, index, time, flags, remaining); static unsigned n; if (traced(obj) && r == 0 && (++n <= 40 || n % 500 == 0 || (flags && *flags))) LOG("[trace] %p acquire output #%u -> index %d time %lld flags %#x remaining %d", obj, n, *index, (long long)*time, *flags, *remaining); return r; }
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
    Movie *m = find(obj);
    if (!m) { int r = ((ib_fn)G.orig[MOVIE_GET_OUTPUT])(obj, index, buf); static unsigned n; if (traced(obj) && (++n <= 40 || n % 500 == 0)) { LOG("[trace] %p get output %d #%u -> %#x", obj, index, n, r); trace_buf("output buffer", buf); } return r; }
    if (!buf) return ST_NULL;
    G.buf_set_range(buf, 0, 0);
    pthread_mutex_lock(&m->mu);
    int st = ST_ERROR;
    if (index >= 0 && index < m->n_out && m->out_held[index]) {
        int bytes = m->out_bytes[index]; uint8_t *dst = G.buf_base(buf);
        if (!bytes) st = ST_OK;
        else if (dst && G.buf_capacity(buf) >= (size_t)bytes) { memcpy(dst, m->out[index], (size_t)bytes); G.buf_set_range(buf, 0, bytes); st = ST_OK; }
        else if (dst && m->format == FORMAT_RGBA && dm_nvn_texture_write_rgba(dst, m->w, m->h, m->out[index])) st = ST_OK;   /* texture mode */
        else { static int said; if (said++ < 4) LOG("【红】output buffer of %zu bytes for a %d-byte frame (not a texture this host can fill)", G.buf_capacity(buf), bytes); }
    }
    pthread_mutex_unlock(&m->mu); return st;
}
static int h_get_output_size(void *obj, int *size, int *count) {
    Movie *m = find(obj);
    if (!m) { int r = ((pp_fn)G.orig[MOVIE_GET_OUTPUT_SIZE])(obj, size, count); if (traced(obj)) LOG("[trace] %p output size -> %#x size %d count %d", obj, r, size ? *size : -9, count ? *count : -9); return r; }
    if (!size || !count) return ST_NULL;
    *count = 1; *size = m->configured ? m->out_cap : 0;
    return ST_OK;
}
typedef int (*i_fn)(void *, int);
static int h_release_output(void *obj, int index) {
    Movie *m = find(obj);
    if (!m) { int r = ((i_fn)G.orig[MOVIE_RELEASE_OUTPUT])(obj, index); static unsigned n; if (traced(obj) && (++n <= 30 || n % 500 == 0)) LOG("[trace] %p release output %d #%u -> %#x", obj, index, n, r); return r; }
    Signals s = {0};
    pthread_mutex_lock(&m->mu);
    int st = ST_ERROR;
    if (index >= 0 && index < m->n_out && m->out_held[index]) {
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
    Movie *m = find(obj);
    if (!m) { int r = ((ib_fn)G.orig[MOVIE_GET_OUTPUT_BUFFER_FORMAT])(obj, index, md); static unsigned n; if (traced(obj) && ++n <= 6) { LOG("[trace] %p output buffer format %d -> %#x", obj, index, r); trace_md("output buffer format", md); } return r; }
    if (!md) return ST_NULL;
    if (index < 0 || index >= m->n_out || !m->out_held[index]) return ST_ERROR;
    set_format(m, md); return ST_OK;
}
static int h_get_output_md(void *obj, int index, void *buf, void *md) {
    Movie *m = find(obj);
    if (!m) { int r = ((ibm_fn)G.orig[MOVIE_GET_OUTPUT_MD])(obj, index, buf, md); static unsigned n; if (traced(obj) && (++n <= 40 || n % 500 == 0)) { LOG("[trace] %p get output+format %d #%u -> %#x", obj, index, n, r); trace_buf("output buffer", buf); if (n <= 4) trace_md("output buffer format", md); } return r; }
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
