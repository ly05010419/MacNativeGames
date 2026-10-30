// The SDK's movie library (subsdk0 = NintendoSDK_movie, Engage): Extractor demuxes, Decoder uses NVIDIA's hardware decoder (NVOMX → NvRmChannelOpen opens the nvdrv device).
// The host has no nvdrv: the SDK aborts when the decoder thread opens the device. Extractor::SetDataSource returns 0x1000 (the same code the SDK returns when it has no implementation object); Engage's Unity demuxes WebM itself and does not use it.
// The host takes over Decoder (below; VP8 / VP9 are decoded with libvpx, H.264 with VideoToolbox in hle_movie_avc.c, Vorbis audio with stb_vorbis): Engage's movies are 103 VP9 WebM files under RomFS/Data/StreamingAssets/Movie (2.7 GB, no audio track), and the game does not go on until the opening movie S01F has played.
#include "hle.h"
#include <arm_neon.h>
#include <mach/mach.h>
#include <pthread.h>
#include <unistd.h>
#include "vpx/vpx_decoder.h"
#include "vpx/vp8dx.h"
#include "hle_movie_avc.h"   // H.264 on VideoToolbox
#include <AudioToolbox/AudioToolbox.h>   // AAC
// Vorbis (movie audio tracks, Link's Awakening): stb_vorbis, built into this file (dm/third_party/stb_vorbis); only its push-data API, floats out
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PULLDATA_API
#define STB_VORBIS_NO_INTEGER_CONVERSION
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#include "stb_vorbis.c"
#pragma clang diagnostic pop
#undef L
#undef C
#undef R

#if 0
HLE_FORCE("_ZN5movie9Extractor13SetDataSourceEPKc", "movie demuxer: the host does not play movies yet, opening fails (0x1000)", int, h_movie_SetSource, void *self, const char *path) {
    (void)self; static int red; if (!red++) LOG("[RED] movie: the host does not demux, opening reports failure (first %s)", path ? path : "(null)");
    return 0x1000;
}
#endif


// ---------- Decoder ----------
// The SDK's Decoder is a thin wrapper over Android's MediaCodec (DecoderImpl wraps android::MediaCodec):
//   · the client registers 4 nn::os events DecoderEvents {input available, output ready, format changed, error}, signalled with SignalEvent;
//   · input: AcquireInputBufferIndex takes a free input index (none: 0x1011) → GetInputBuffer hands its data area to the client's movie::Buffer → SendInputBufferForDecode(index, offset, size, time µs, flags);
//   · output: AcquireOutputBufferIndex(index, time, flags, remaining) (none: 0x100e) → GetOutputBufferFormat / GetOutputBuffer copy one NV12 frame into a buffer the client allocated → ReleaseOutputBufferIndex.
//   Unity (mode 1, output format 4 = NV12) allocates its output buffers by GetOutputBufferSize and reads width / height / crop / nv12-y-stride / nv12-y-offset / nv12-uv-offset / slice-height from the MediaData.
// The host implements the same protocol and decodes VP8 / VP9 with libvpx (dm/third_party/libvpx). Input packets queue in arrival order; whenever an output slot is free, the head packet is decoded and its input index returned, and a picture goes to the ready-output queue with its time and flags copied
// (the end-of-stream flag comes back with the last frame). While the client holds every output slot, packets stay queued and their input indices are not returned (as with MediaCodec: a decoder only consumes input when it has a free output); each output index the client returns lets one more packet be decoded.
// The size comes from Configure's MediaData, or else from the VP9 header of the first key frame. Unity allocates its output buffers itself as width × height × 1.5 (it does not call GetOutputBufferSize), so the stride is the width and the plane height the height, unaligned.
// Decoding runs on each decoder's own host thread, and the guest thread that sends a packet or returns an output index waits for it: guest threads run on guest stacks given by the SDK, and libvpx's stack frames are large. Events are still signalled by guest threads (SignalEvent is guest code).
// Buffer / MediaData / event operations call the SDK's own functions (guest_sym), so the host needs no knowledge of their layouts.
// Native mutex waits use dm_host_mutex_lock: a callback can wake a higher-priority guest thread that then waits for the decoder lock held by its signaller. That thread is blocked,
// not runnable guest work; counting it as runnable deadlocked Xenoblade Chronicles 3 at the first released output. Keep the locks across signalling for decoder/event lifetime safety.

#define MD_NIN 4                 // input buffers
#define MD_INCAP (4u << 20)      // 4 MB per input buffer (a 1080p VP9 packet is far smaller)
#define MD_NOUT 64               // output slots, at most (a Vorbis decoder's count)
#define MD_NOUT_AAC 32           // an AAC decoder's output slots
#define MD_NOUT_VIDEO 16         // a video decoder's output slots
#define MD_NO_INPUT 0x1011       // SDK: no free input
#define MD_NO_OUTPUT 0x100e      // SDK: no output ready
#define MD_STALE_NS 250000000ull // how long an input the client held across a flush stays withheld (h_movie_AcqIn)
#define MD_FMT_NV12 4
#define MD_FMT_ABGR 3            // 4-byte RGBA (memory order R G B A), only with the native texture mode
#define MD_MODE_TEX 2            // native texture mode: the decoder writes frames into the client's NVN texture
#define MD_TYPE_AVC 1
#define MD_TYPE_VP8 2
#define MD_TYPE_VP9 3
#define MD_VPX(d) ((d)->type == MD_TYPE_VP8 || (d)->type == MD_TYPE_VP9)
#define MD_TYPE_AAC 4            // audio: the SDK's Decoder accepts AAC (4) and Vorbis (5) in CPU mode, output format 1 only (interleaved PCM16)
#define MD_TYPE_VORBIS 5
#define MD_AUDIO(d) ((d)->type == MD_TYPE_VORBIS || (d)->type == MD_TYPE_AAC)
#define MD_FMT_PCM16 1
#define MD_PCM_CAP (8192 * 8 * 2)   // bytes per audio output: a Vorbis packet gives at most 4096 samples per channel (blocksize 8192), 8 channels at most

typedef struct {
    pthread_mutex_t mu;                          // guards the queues and slots below
    int type, fmt, mode, nout;                   // nout: output slots, MD_NOUT_VIDEO for video, MD_NOUT_AAC for AAC, MD_NOUT for Vorbis (see the constructor)
    void **evc;                                  // the client's DecoderEvents: {input, output, format, error} EventType*, read at signal time as the SDK does (md_event)
    void *ev[4];                                 // the entries that were events at registration: used when the client's entry no longer is one (by Metroid Dread's second movie its struct had been overwritten and Start crashed signalling)
    int w, h, fmt_sent, started, in_eos;          // in_eos: an end-of-stream packet was sent; no input until Flush / Stop (h_movie_AcqIn)
    uint8_t *in[MD_NIN]; int infree[MD_NIN], ninfree;                     // queue of free input indices
    int inown[MD_NIN]; uint64_t instale[MD_NIN];                          // inown: acquired by the client and not sent yet; instale: when a Flush / Stop found it held (the client may still send it), 0 if not
    struct { int used, pic; int64_t pts; uint32_t flags; uint8_t *frm; size_t cap, bytes; } out[MD_NOUT];   // pic: frm holds a decoded picture (audio: bytes of PCM); cap: bytes in frm
    int outq[MD_NOUT], noutq;                                             // queue of ready output indices
    struct { int in, off, size; int64_t pts; uint32_t flags; } pend[MD_NIN]; int npend;   // input packets waiting to be decoded (in arrival order)
    // Decoding: dmu lets only one guest thread take and decode packets at a time (packets enter libvpx in order); wmu / wcv hand one job to the host thread and wait for it
    pthread_mutex_t dmu, wmu; pthread_cond_t wcv; pthread_t wk;
    int job; const uint8_t *jp; int jn, jslot, jgot; int64_t jpts, jpts_out; int jflush;
    mavc *avc; int avc_drain;                    // H.264 (hle_movie_avc.c); avc_drain: the end-of-stream packet came with pictures still queued, they leave one per free slot
    vpx_codec_ctx_t vpx; int vpx_on, red_dec, red_img;
    // Vorbis: the identification and setup headers from Configure's MediaData ("csd-0", "csd-1", as with Android's MediaCodec), the decoder (opened at the first packet), channels / rate, Ogg page sequence
    uint8_t *vhdr[2]; int vhdr_n[2]; stb_vorbis *vb; int ch, rate; uint32_t ogg_seq; int64_t ogg_gran; int red_vb;
    // AAC (Monster Hunter Rise's movies): AudioToolbox's converter, opened at the first packet from the AudioSpecificConfig (Configure's csd-0, kept in vhdr[0]); the packet being fed
    AudioConverterRef aac; const uint8_t *aac_pkt; UInt32 aac_pkt_n; AudioStreamPacketDescription aac_desc;
    uint64_t dec_n, dec_ns, dec_max;            // decode timing (on the host thread, per packet)
    unsigned long ncall[24];
} mdec_t;

static struct {
    void (*signal)(void *ev);
    void (*set_data)(void *buf, void *data, size_t cap);
    void (*set_range)(void *buf, int off, int size);
    void *(*data)(const void *buf);
    size_t (*cap)(const void *buf);
    void (*set_i32)(void *md, const char *k, int v);
    void (*set_rect)(void *md, const char *k, int l, int t, int r, int b);
    int (*find_i32)(const void *md, const char *k, int *v);
    int (*find_buf)(const void *md, const char *k, void **buf);
    size_t (*size)(const void *buf);
    void *(*nvn_sync)(const void *buf);   // used only by the native texture mode; Engage's subsdk0 has it too, but it is not required
} G;

static void *msym(const char *n) { void *p = guest_sym(n); if (!p) die("movie: missing SDK symbol %s", n); return p; }
static void md_syms(void) {
    if (G.signal) return;
    G.set_data = msym("_ZN5movie6Buffer18SetDataAndCapacityEPvm");
    G.set_range = msym("_ZN5movie6Buffer8SetRangeEii");
    G.data = msym("_ZNK5movie6Buffer4DataEv");
    G.cap = msym("_ZNK5movie6Buffer8CapacityEv");
    G.set_i32 = msym("_ZN5movie9MediaData8SetInt32EPKci");
    G.set_rect = msym("_ZN5movie9MediaData7SetRectEPKciiii");
    G.find_i32 = msym("_ZNK5movie9MediaData9FindInt32EPKcPi");
    G.find_buf = msym("_ZNK5movie9MediaData10FindBufferEPKcPPNS_6BufferE");
    G.size = msym("_ZNK5movie6Buffer4SizeEv");
    G.signal = msym("_ZN2nn2os11SignalEventEPNS0_9EventTypeE");
}
static mdec_t *md_of(void **self) { return self ? (mdec_t *)self[1] : NULL; }
// An EventType pointer that can be signalled: 8-byte aligned and readable (vm_read_overwrite fails instead of faulting)
static int ev_ok(const void *p) {
    uint64_t w; vm_size_t got = 0; uintptr_t a = (uintptr_t)p;
    return a >= 0x100000000ull && !(a & 7) && vm_read_overwrite(mach_task_self(), (vm_address_t)a, sizeof w, (vm_address_t)&w, &got) == KERN_SUCCESS && got == sizeof w;
}
// The SDK keeps only the pointer to the client's struct (DecoderImpl::RegisterClientEvents: str x1, [x0, #0xd0]) and reads the entry each time it signals, skipping a null one
// (SendOutputBufferEventAvailableToClient and the others, subsdk0+0x1feb0 in DRAGON QUEST XI S). That game's movie player registers its struct before it fills it (main+0x39edab0, right after
// the Decoder constructor: leftover values such as 0xe00000010, sometimes 0 or a stray pointer), so the entry is read at signal time; when it is not an event (any longer), the event it held
// at registration is used, if any
static void md_event(mdec_t *d, int i) {   // 0 input 1 output 2 format 3 error
    void *p = NULL; vm_size_t got = 0;
    if (d->evc && (vm_read_overwrite(mach_task_self(), (vm_address_t)&d->evc[i], sizeof p, (vm_address_t)&p, &got) != KERN_SUCCESS || got != sizeof p)) p = NULL;
    void *live = p;
    if (!p || !ev_ok(p)) p = d->ev[i];
    if (p) G.signal(p);
    else if (live) { static _Atomic int red; if (red++ < 8) LOG("[RED] movie: decoder %p event %d not delivered: the client's entry %p is not an event", (void *)d, i, live); }
}
// Deferred events: "input available" after Start / Flush. The SDK's decoder signals it asynchronously from its own codec thread; Metroid Dread calls InitializeMultiWaitHolder before RegisterClientEvents
// and only later links the holders into its MultiWait. Signalled synchronously inside Start, the holders were not linked yet and SignalEvent crashed in MultiWaitImpl::NotifyAndWakeupThread on a null MultiWait (0x20)
// (it showed only when a decoder was built a second time in the same memory). A guest thread now signals it about 2 ms later (SignalEvent is guest code and must be called on a guest thread); the destructor drops that decoder's pending signals under the same lock.
// The movie player's dispatcher and worker must also finish their requests before the posting thread continues; the shared SVC wake handoff supplies that ordering (hle_thread.c).
// g_dfmu and each decoder's dmu are held while guest code signals events, so guest threads take them with dm_host_lock: a thread waiting for one counts as blocked, and the
// signaller's handoff does not wait for it (Kirby and the Forgotten Land froze at its first guide movie: the handoff waited for the input thread, which waited for dmu)
void *dm_guest_thread_spawn(void (*fn)(void *), void *arg, int prio, size_t stack_size);
void dm_host_lock(pthread_mutex_t *m);   // hle_thread.c
static pthread_mutex_t g_dfmu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_dfcv = PTHREAD_COND_INITIALIZER;
static struct { void *d; int ev; uint64_t due; } g_dfq[64]; static int g_ndfq, g_dfthr;
static void md_defer_body(void *arg);
static void md_defer_at(void *d, int ev, uint64_t due) {
    dm_host_lock(&g_dfmu);
    if (!g_dfthr) { g_dfthr = 1; dm_guest_thread_spawn(md_defer_body, NULL, 16, 64 << 10); }
    for (int i = 0; i < g_ndfq; i++) if (g_dfq[i].d == d && g_dfq[i].ev == ev && g_dfq[i].due <= due) { pthread_mutex_unlock(&g_dfmu); return; }   // already due by then
    if (g_ndfq < 64) g_dfq[g_ndfq++] = (__typeof__(g_dfq[0])){d, ev, due};
    else LOG("[RED] movie: deferred event queue full, dropping one");
    pthread_cond_signal(&g_dfcv); pthread_mutex_unlock(&g_dfmu);
}
static void md_defer(void *d, int ev) { md_defer_at(d, ev, now_ns() + 2000000); }
static void md_defer_drop(void *d) {
    dm_host_lock(&g_dfmu);
    int n = 0; for (int i = 0; i < g_ndfq; i++) if (g_dfq[i].d != d) g_dfq[n++] = g_dfq[i];
    g_ndfq = n; pthread_mutex_unlock(&g_dfmu);
}
// signal under the lock: the destructor first drops pending entries under it, so a signal never interleaves with destruction
static void md_defer_body(void *arg) {
    (void)arg;
    dm_host_lock(&g_dfmu);
    for (;;) {
        uint64_t now = now_ns(), next = 0; int k = -1;
        for (int i = 0; i < g_ndfq && k < 0; i++) { if (g_dfq[i].due <= now) k = i; else if (!next || g_dfq[i].due < next) next = g_dfq[i].due; }
        if (k >= 0) {
            mdec_t *d = g_dfq[k].d; int e = g_dfq[k].ev;
            memmove(g_dfq + k, g_dfq + k + 1, (size_t)(--g_ndfq - k) * sizeof *g_dfq);
            if (d->started) md_event(d, e);   // not after Stop: the game takes its events apart right after Stop (a Stop within 2 ms of a Flush let the deferred signal hit events being torn down)
            continue;
        }
        if (!next) pthread_cond_wait(&g_dfcv, &g_dfmu);
        else { uint64_t w = next - now; struct timespec ts = { (time_t)(w / 1000000000u), (long)(w % 1000000000u) }; pthread_cond_timedwait_relative_np(&g_dfcv, &g_dfmu, &ts); }
    }
}
// log the first 12 calls one by one, then every 2^k-th (DM_MOVIE_TRACE=1: every call, with the calling thread and the decoder: Unity drives a movie's audio and video decoders from one thread)
static int md_trace_all(void) { static int on = -1; if (on < 0) on = getenv("DM_MOVIE_TRACE") != NULL; return on; }
#define MD_TRACE(d, slot, fmt, ...) do { unsigned long n_ = ++(d)->ncall[slot]; if (md_trace_all()) LOG("[movie] [%p] [%p] " fmt, (void *)pthread_self(), (void *)(d), ##__VA_ARGS__); \
                                         else if (n_ <= 12 || !(n_ & (n_ - 1))) LOG("[movie] " fmt, ##__VA_ARGS__); } while (0)

// VP9 key frame uncompressed header → width and height (VP9 spec 6.2: frame_marker, profile, show_existing_frame, frame_type, show_frame, error_resilient_mode, sync code, color config, frame_size)
static int vp9_size(const uint8_t *p, int n, int *w, int *h) {
    int bit = 0;
    #define RB(k) ({ unsigned v_ = 0; for (int i_ = 0; i_ < (k); i_++, bit++) { if (bit >> 3 >= n) return 0; v_ = v_ << 1 | (p[bit >> 3] >> (7 - (bit & 7)) & 1); } v_; })
    if (RB(2) != 2) return 0;
    unsigned lo = RB(1), hi = RB(1), prof = hi << 1 | lo;
    if (prof == 3) RB(1);
    if (RB(1)) return 0;                       // show_existing_frame
    if (RB(1) != 0) return 0;                  // not a key frame
    RB(2);                                     // show_frame, error_resilient_mode
    if (RB(24) != 0x498342) return 0;
    if (prof >= 2) RB(1);
    if (RB(3) != 7) { RB(1); if (prof == 1 || prof == 3) RB(3); }
    else if (prof == 1 || prof == 3) RB(1);
    *w = (int)RB(16) + 1; *h = (int)RB(16) + 1;
    #undef RB
    return 1;
}
// VP8 key frame: frame tag, sync code, then 14-bit width and height (RFC 6386 section 9.1).
static int vp8_size(const uint8_t *p, int n, int *w, int *h) {
    if (n < 10 || (p[0] & 1) || memcmp(p + 3, "\x9d\x01\x2a", 3)) return 0;
    int x = (p[6] | p[7] << 8) & 0x3fff, y = (p[8] | p[9] << 8) & 0x3fff;
    if (!x || !y) return 0;
    *w = x; *h = y; return 1;
}
static void md_dims(const mdec_t *d, int *w, int *h, int *stride, int *slice) {
    *w = d->w > 0 ? d->w : 1920; *h = d->h > 0 ? d->h : 1080;
    *stride = *w; *slice = *h;
}

// ---------- Host decoding thread ----------
enum { MDJ_NONE, MDJ_DECODE, MDJ_CLOSE, MDJ_QUIT, MDJ_TAKE };
static int md_threads(void) { long n = sysconf(_SC_NPROCESSORS_ONLN); return n >= 8 ? 4 : 2; }
// I420 → NV12 (stride = width, plane height = height); a smaller picture is padded with black, a larger one cropped
static void md_nv12(const vpx_image_t *im, uint8_t *dst, int w, int h) {
    int cw = (int)im->d_w < w ? (int)im->d_w : w, ch = (int)im->d_h < h ? (int)im->d_h : h;
    if (cw < w || ch < h) { memset(dst, 16, (size_t)w * h); memset(dst + (size_t)w * h, 128, (size_t)w * h / 2); }
    for (int y = 0; y < ch; y++) memcpy(dst + (size_t)y * w, im->planes[0] + (size_t)y * im->stride[0], cw);
    int cw2 = (cw + 1) / 2, ch2 = (ch + 1) / 2;
    for (int y = 0; y < ch2 && y < h / 2; y++) {
        const uint8_t *u = im->planes[1] + (size_t)y * im->stride[1], *v = im->planes[2] + (size_t)y * im->stride[2];
        uint8_t *o = dst + (size_t)w * h + (size_t)y * w;
        int x = 0;
        for (; x + 16 <= cw2; x += 16) { uint8x16x2_t t = { { vld1q_u8(u + x), vld1q_u8(v + x) } }; vst2q_u8(o + 2 * x, t); }
        for (; x < cw2 && 2 * x + 1 < w; x++) { o[2 * x] = u[x]; o[2 * x + 1] = v[x]; }
    }
}
// I420 → RGBA (native texture mode): by the colour space and range the picture declares (VP9 colour config; when undeclared BT.709 from 720 lines up, else BT.601), 16-bit fixed point; a smaller picture is padded with black, a larger one cropped
static void md_rgba(const vpx_image_t *im, uint8_t *dst, int w, int h) {
    int full = im->range == VPX_CR_FULL_RANGE, b709 = im->cs == VPX_CS_BT_709 || (im->cs == VPX_CS_UNKNOWN && h >= 720);
    double ky = full ? 1.0 : 255.0 / 219.0, kc = full ? 1.0 : 255.0 / 224.0;
    double rv = b709 ? 1.5748 : 1.402, gu = b709 ? 0.1873 : 0.3441, gv = b709 ? 0.4681 : 0.7141, bu = b709 ? 1.8556 : 1.772;
    int Y = (int)(ky * 65536), RV = (int)(rv * kc * 65536), GU = (int)(gu * kc * 65536), GV = (int)(gv * kc * 65536), BU = (int)(bu * kc * 65536), y0 = full ? 0 : 16;
    int cw = (int)im->d_w < w ? (int)im->d_w : w, ch = (int)im->d_h < h ? (int)im->d_h : h;
    if (cw < w || ch < h) for (size_t i = 0; i < (size_t)w * h; i++) ((uint32_t *)dst)[i] = 0xff000000u;
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
// decode one packet into output slot slot: 1 = a picture came out, 0 = none (hidden frame or error)
static int md_vpx(mdec_t *d, const uint8_t *p, int n, int slot) {
    const char *codec = d->type == MD_TYPE_VP8 ? "VP8" : "VP9";
    if (!d->vpx_on) {
        vpx_codec_dec_cfg_t cfg = { .threads = (unsigned)md_threads(), .w = (unsigned)d->w, .h = (unsigned)d->h };
        vpx_codec_iface_t *iface = d->type == MD_TYPE_VP8 ? vpx_codec_vp8_dx() : vpx_codec_vp9_dx();
        vpx_codec_err_t e = vpx_codec_dec_init(&d->vpx, iface, &cfg, 0);
        if (e) { LOG("[RED] movie: libvpx failed to create the %s decoder: %s", codec, vpx_codec_err_to_string(e)); return 0; }
        if (d->type == MD_TYPE_VP9) vpx_codec_control(&d->vpx, VP9D_SET_ROW_MT, 1);
        d->vpx_on = 1;
        LOG("[movie] libvpx %s: %s decoder, %d threads", vpx_codec_version_str(), codec, cfg.threads);
    }
    if (vpx_codec_decode(&d->vpx, p, (unsigned)n, NULL, 0)) {
        if (!d->red_dec++) LOG("[RED] movie: %s decode error (first case, %d bytes): %s %s", codec, n, vpx_codec_error(&d->vpx), vpx_codec_error_detail(&d->vpx) ? vpx_codec_error_detail(&d->vpx) : "");
        return 0;
    }
    vpx_codec_iter_t it = NULL; vpx_image_t *im, *last = NULL;
    while ((im = vpx_codec_get_frame(&d->vpx, &it))) last = im;
    if (!last) return 0;
    if (last->fmt != VPX_IMG_FMT_I420) { if (!d->red_img++) LOG("[RED] movie: %s picture format %#x is not I420 (4:2:0 8-bit), outputting a black frame", codec, last->fmt); return 0; }
    if ((int)last->d_w != d->w || (int)last->d_h != d->h) { if (!d->red_img++) LOG("[RED] movie: %s picture %u×%u does not match the output %d×%d (first case), cropping / padding with black", codec, last->d_w, last->d_h, d->w, d->h); }
    int rgba = d->fmt == MD_FMT_ABGR;
    size_t need = (size_t)d->w * d->h * (rgba ? 4 : 3) / (rgba ? 1 : 2);
    if (d->out[slot].cap < need) { free(d->out[slot].frm); d->out[slot].frm = malloc(need); d->out[slot].cap = need; }   // a Configure after Reset may change the size
    if (rgba) md_rgba(last, d->out[slot].frm, d->w, d->h); else md_nv12(last, d->out[slot].frm, d->w, d->h);
    return 1;
}
// ---------- Vorbis ----------
// The demuxer hands over raw Vorbis packets (WebM blocks); stb_vorbis reads Ogg pages, so each packet goes in as one page of its own: the three headers first
// (identification and setup from Configure, a comment header made up here: stb_vorbis only checks its type), then one page per audio packet. The page CRC is filled in:
// stb_vorbis checks it while resynchronising after an error
static uint32_t ogg_crc(const uint8_t *p, size_t n) {
    static uint32_t t[256]; if (!t[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t r = i << 24; for (int k = 0; k < 8; k++) r = r & 0x80000000u ? r << 1 ^ 0x04c11db7u : r << 1; t[i] = r; }
    uint32_t c = 0; for (size_t i = 0; i < n; i++) c = c << 8 ^ t[(c >> 24 ^ p[i]) & 0xff]; return c;
}
static size_t ogg_page(mdec_t *d, uint8_t *out, const uint8_t *pkt, int n, int bos) {   // out needs n + 27 + 255 bytes; n < 65025
    int seg = n / 255 + 1; uint8_t *h = out;
    memcpy(h, "OggS", 4); h[4] = 0; h[5] = bos ? 2 : 0;
    int64_t g = d->ogg_gran; memcpy(h + 6, &g, 8); uint32_t sn = 0x444d5642u; memcpy(h + 14, &sn, 4);   // granule, serial "BVMD"
    uint32_t q = d->ogg_seq++; memcpy(h + 18, &q, 4); memset(h + 22, 0, 4); h[26] = (uint8_t)seg;
    for (int i = 0; i < seg - 1; i++) h[27 + i] = 255;
    h[27 + seg - 1] = (uint8_t)(n % 255);
    memcpy(h + 27 + seg, pkt, (size_t)n);
    size_t len = 27 + (size_t)seg + (size_t)n; uint32_t c = ogg_crc(out, len); memcpy(h + 22, &c, 4);
    return len;
}
static int md_vorbis_open(mdec_t *d) {
    if (!d->vhdr[0] || !d->vhdr[1]) { if (!d->red_vb++) LOG("[RED] movie: the Vorbis decoder got no headers (csd-0 / csd-1), outputting silence"); return 0; }
    static const uint8_t comment[] = {3, 'v', 'o', 'r', 'b', 'i', 's', 0, 0, 0, 0, 0, 0, 0, 0, 1};
    size_t cap = (size_t)d->vhdr_n[0] + d->vhdr_n[1] + sizeof comment + 3 * (27 + 255); uint8_t *buf = malloc(cap); if (!buf) return 0;
    d->ogg_seq = 0; d->ogg_gran = 0;
    size_t n = ogg_page(d, buf, d->vhdr[0], d->vhdr_n[0], 1); n += ogg_page(d, buf + n, comment, sizeof comment, 0); n += ogg_page(d, buf + n, d->vhdr[1], d->vhdr_n[1], 0);
    int used = 0, err = 0; d->vb = stb_vorbis_open_pushdata(buf, (int)n, &used, &err, NULL); free(buf);
    if (!d->vb) { if (!d->red_vb++) LOG("[RED] movie: Vorbis headers cannot be decoded (stb_vorbis error %d), outputting silence", err); return 0; }
    stb_vorbis_info in = stb_vorbis_get_info(d->vb); d->ch = in.channels; d->rate = (int)in.sample_rate;
    LOG("[movie] Vorbis: %d channels %d Hz", d->ch, d->rate);
    return 1;
}
// one packet → PCM16 interleaved in the slot; returns 1 when there are samples (the first packet of a stream only primes the overlap and gives none)
static int md_vorbis(mdec_t *d, const uint8_t *p, int n, int slot) {
    if (!d->vb && !md_vorbis_open(d)) return 0;
    if (n >= 65025) { if (!d->red_vb++) LOG("[RED] movie: Vorbis packet of %d bytes, larger than a page", n); return 0; }
    uint8_t *page = malloc((size_t)n + 27 + 255); if (!page) return 0;
    size_t len = ogg_page(d, page, p, n, 0);
    int ch = 0, ns = 0; float **pcm = NULL;
    stb_vorbis_decode_frame_pushdata(d->vb, page, (int)len, &ch, &pcm, &ns); free(page);
    if (ns <= 0 || ch <= 0 || !pcm) return 0;
    d->ogg_gran += ns;
    size_t need = (size_t)ns * ch * 2;
    if (d->out[slot].cap < need) { free(d->out[slot].frm); d->out[slot].frm = malloc(need); d->out[slot].cap = need; }
    int16_t *o = (int16_t *)d->out[slot].frm;
    for (int i = 0; i < ns; i++) for (int c = 0; c < ch; c++) { float v = pcm[c][i] * 32767.0f; o[i * ch + c] = (int16_t)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : v); }
    d->out[slot].bytes = need;
    return 1;
}
// ---------- AAC ----------
// The demuxer hands over raw AAC frames (1024 samples each) and the AudioSpecificConfig in csd-0. AudioToolbox takes the configuration as an MPEG-4 ES descriptor (the magic cookie built
// here as FFmpeg's AudioToolbox decoder builds it); without csd-0 an AAC-LC configuration is made from the sample rate and channel count
static uint8_t *esds_descr(uint8_t *p, int tag, unsigned n) { *p++ = (uint8_t)tag; for (int i = 3; i > 0; i--) *p++ = (uint8_t)(n >> (7 * i) | 0x80); *p++ = (uint8_t)(n & 0x7f); return p; }
static OSStatus aac_feed(AudioConverterRef c, UInt32 *np, AudioBufferList *bl, AudioStreamPacketDescription **pd, void *ud) {
    (void)c; mdec_t *d = ud;
    if (!d->aac_pkt) { *np = 0; return 1; }   // this packet is used up: a non-zero status ends the fill with what was decoded
    bl->mNumberBuffers = 1; bl->mBuffers[0].mData = (void *)d->aac_pkt; bl->mBuffers[0].mDataByteSize = d->aac_pkt_n; bl->mBuffers[0].mNumberChannels = (UInt32)d->ch;
    d->aac_desc = (AudioStreamPacketDescription){0, 0, d->aac_pkt_n}; if (pd) *pd = &d->aac_desc;
    *np = 1; d->aac_pkt = NULL; return 0;
}
static int md_aac_open(mdec_t *d) {
    static const int rates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
    uint8_t asc[64]; int an = d->vhdr_n[0] > 0 && d->vhdr_n[0] <= (int)sizeof asc ? d->vhdr_n[0] : 0;
    if (an) memcpy(asc, d->vhdr[0], (size_t)an);
    if (an >= 2) {   // sampling frequency index and channel configuration from the configuration itself
        int fi = (asc[0] & 7) << 1 | asc[1] >> 7, cc = asc[1] >> 3 & 15;
        if (fi < 13 && d->rate <= 0) d->rate = rates[fi]; if (cc && cc < 8 && d->ch <= 0) d->ch = cc == 7 ? 8 : cc;
    }
    if (d->rate <= 0) d->rate = 48000; if (d->ch <= 0) d->ch = 2;
    if (!an) { int fi = 3; for (int i = 0; i < 13; i++) if (rates[i] == d->rate) fi = i; asc[0] = (uint8_t)(2 << 3 | fi >> 1); asc[1] = (uint8_t)((fi & 1) << 7 | (d->ch & 15) << 3); an = 2; }
    AudioStreamBasicDescription in = {.mSampleRate = d->rate, .mFormatID = kAudioFormatMPEG4AAC, .mFramesPerPacket = 1024, .mChannelsPerFrame = (UInt32)d->ch};
    AudioStreamBasicDescription out = {.mSampleRate = d->rate, .mFormatID = kAudioFormatLinearPCM, .mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked,
                                       .mBytesPerPacket = 2u * d->ch, .mFramesPerPacket = 1, .mBytesPerFrame = 2u * d->ch, .mChannelsPerFrame = (UInt32)d->ch, .mBitsPerChannel = 16};
    OSStatus st = AudioConverterNew(&in, &out, &d->aac);
    if (st) { if (!d->red_vb++) LOG("[RED] movie: AudioToolbox cannot create an AAC decoder (%d; %d channels %d Hz), outputting silence", (int)st, d->ch, d->rate); d->aac = NULL; return 0; }
    uint8_t ck[128], *p = ck;
    p = esds_descr(p, 3, 3 + 5 + 13 + 5 + (unsigned)an); *p++ = 0; *p++ = 0; *p++ = 0;   // ES descriptor: ES ID 0, no flags
    p = esds_descr(p, 4, 13 + 5 + (unsigned)an); *p++ = 0x40; *p++ = 0x15; memset(p, 0, 11); p += 11;   // decoder configuration: MPEG-4 audio, audio stream; buffer size and bit rates 0
    p = esds_descr(p, 5, (unsigned)an); memcpy(p, asc, (size_t)an); p += an;
    st = AudioConverterSetProperty(d->aac, kAudioConverterDecompressionMagicCookie, (UInt32)(p - ck), ck);
    if (st && !d->red_vb++) LOG("[RED] movie: AAC configuration not accepted by AudioToolbox (%d)", (int)st);
    LOG("[movie] AAC: %d channels %d Hz, AudioSpecificConfig %d bytes (object type %d)", d->ch, d->rate, an, asc[0] >> 3);
    return 1;
}
// one packet → PCM16 interleaved in the slot; 1 when there are samples (the decoder's start-up delay may give none for the first packet)
static int md_aac(mdec_t *d, const uint8_t *p, int n, int slot) {
    if (!d->aac && !md_aac_open(d)) return 0;
    if (n <= 0) return 0;
    if (d->out[slot].cap < MD_PCM_CAP) { free(d->out[slot].frm); d->out[slot].frm = malloc(MD_PCM_CAP); d->out[slot].cap = MD_PCM_CAP; }
    d->aac_pkt = p; d->aac_pkt_n = (UInt32)n;
    UInt32 frames = (UInt32)(MD_PCM_CAP / (2 * d->ch));
    AudioBufferList ob = {1, {{(UInt32)d->ch, frames * 2u * d->ch, d->out[slot].frm}}};
    OSStatus st = AudioConverterFillComplexBuffer(d->aac, aac_feed, d, &frames, &ob, NULL);
    d->aac_pkt = NULL;
    if (st && st != 1 && !d->red_dec++) LOG("[RED] movie: AAC decode error (AudioToolbox %d, first case)", (int)st);
    d->out[slot].bytes = (size_t)frames * 2 * d->ch;
    return frames > 0;
}
// The H.264 decoder, made by Configure (when the stream's parameter sets come with it) or by the first packet; RGBA pictures in the native texture mode either way.
// Absolum's movies bring csd-0 / csd-1 to Configure, and a decoder made there used to stay NV12: the frames' bytes went into the RGBA texture as they were
static void md_avc_open(mdec_t *d) { if (!d->avc) { d->avc = mavc_new(); if (d->fmt == MD_FMT_ABGR) mavc_set_rgba(d->avc); } }
// H.264: decode the packet (if any) and hand out the next picture in presentation order (with jflush every queued one in turn); 1 = a picture is in the slot, its time in jpts_out
static int md_avc(mdec_t *d, const uint8_t *p, int n, int slot, int take_only) {
    md_avc_open(d);
    if (!take_only && n > 0) mavc_decode(d->avc, p, n, d->jpts);
    if (d->w <= 0 || d->h <= 0) return 0;
    size_t need = (size_t)d->w * d->h * (d->fmt == MD_FMT_ABGR ? 4 : 3) / (d->fmt == MD_FMT_ABGR ? 1 : 2);
    if (d->out[slot].cap < need) { free(d->out[slot].frm); d->out[slot].frm = malloc(need); d->out[slot].cap = need; }
    return mavc_take(d->avc, d->jflush, d->out[slot].frm, d->w, d->h, &d->jpts_out);
}
static void md_close(mdec_t *d) { if (d->vpx_on) { vpx_codec_destroy(&d->vpx); d->vpx_on = 0; } if (d->vb) { stb_vorbis_close(d->vb); d->vb = NULL; } if (d->aac) { AudioConverterDispose(d->aac); d->aac = NULL; } if (d->avc) { mavc_free(d->avc); d->avc = NULL; } }
void dm_host_helper_qos(qos_class_t q);   // hle_thread.c
static void *md_worker(void *arg) {
    mdec_t *d = arg;
    pthread_setname_np("dm movie decoder");
    dm_host_helper_qos(QOS_CLASS_USER_INITIATED);   // under DM_QOS_MAP: decodes ahead of playback, not on the frame's critical path
    dm_host_mutex_lock(&d->wmu);
    for (;;) {
        while (d->job == MDJ_NONE) pthread_cond_wait(&d->wcv, &d->wmu);
        int job = d->job;
        pthread_mutex_unlock(&d->wmu);
        if (job == MDJ_DECODE || job == MDJ_TAKE) {
            uint64_t t0 = now_ns();
            d->jgot = d->type == MD_TYPE_AVC ? md_avc(d, d->jp, d->jn, d->jslot, job == MDJ_TAKE) : d->type == MD_TYPE_VORBIS ? md_vorbis(d, d->jp, d->jn, d->jslot) :
                      d->type == MD_TYPE_AAC ? md_aac(d, d->jp, d->jn, d->jslot) : md_vpx(d, d->jp, d->jn, d->jslot);
            uint64_t dt = now_ns() - t0; d->dec_n++; d->dec_ns += dt; if (dt > d->dec_max) d->dec_max = dt;
        }
        else md_close(d);
        dm_host_mutex_lock(&d->wmu);
        d->job = MDJ_NONE; pthread_cond_broadcast(&d->wcv);
        if (job == MDJ_QUIT) break;
    }
    pthread_mutex_unlock(&d->wmu);
    return NULL;
}
// hand one job to the host thread and wait for it (the caller holds dmu)
static void md_job(mdec_t *d, int job) {
    dm_host_mutex_lock(&d->wmu);
    d->job = job; pthread_cond_broadcast(&d->wcv);
    while (d->job != MDJ_NONE) pthread_cond_wait(&d->wcv, &d->wmu);
    pthread_mutex_unlock(&d->wmu);
}
// while packets are queued and an output slot is free, decode the head packet, until the queue is empty or every slot is taken; signal the input / output events by the result
static void md_pump(mdec_t *d) {
    dm_host_lock(&d->dmu);
    for (;;) {
        dm_host_mutex_lock(&d->mu);
        if (d->avc_drain) {   // H.264 end of stream: the pictures still queued leave one per free slot; the last carries the end-of-stream flag
            int s = -1; for (int k = 0; k < d->nout; k++) if (!d->out[k].used) { s = k; break; }
            if (s < 0) { pthread_mutex_unlock(&d->mu); break; }
            d->out[s].used = 1; pthread_mutex_unlock(&d->mu);
            d->jslot = s; d->jflush = 1; md_job(d, MDJ_TAKE); int got = d->jgot, left = d->avc ? mavc_queued(d->avc) : 0;
            dm_host_mutex_lock(&d->mu);
            d->out[s].pic = got; d->out[s].pts = got ? d->jpts_out : 0; d->out[s].flags = left ? 0 : 4; d->outq[d->noutq++] = s;
            if (!left) d->avc_drain = 0;
            pthread_mutex_unlock(&d->mu);
            md_event(d, 1);
            continue;
        }
        int slot = -1;
        if (d->npend) for (int k = 0; k < d->nout; k++) if (!d->out[k].used) { slot = k; break; }
        if (slot < 0) { pthread_mutex_unlock(&d->mu); break; }
        __typeof__(d->pend[0]) pk = d->pend[0];
        memmove(d->pend, d->pend + 1, --d->npend * sizeof *d->pend);
        d->out[slot].used = 1;
        pthread_mutex_unlock(&d->mu);
        int got = 0, avc = d->type == MD_TYPE_AVC;
        int eos = (pk.flags & 4) != 0;   // MediaCodec BUFFER_FLAG_END_OF_STREAM
        if (eos && (pk.pts < 0 || (uint32_t)pk.size >= MD_INCAP)) pk.size = 0;   // Monster Hunter Rise ends a stream with time -1 and the whole input buffer (4 MB) as its length: no data (decoding it gave VideoToolbox -12909 and AudioToolbox -50); an end-of-stream packet as long as the whole buffer is the same, with a real time (from PR #36)
        // H.264 end of stream: this packet's output takes the first queued picture (jflush). Taking none put a black picture without the flag ahead of the last
        // `depth` pictures, a black flash before the movie's last frames
        if ((pk.size > 0 && (MD_VPX(d) || MD_AUDIO(d) || avc)) || (avc && eos)) {
            d->jp = d->in[pk.in] + pk.off; d->jn = pk.size; d->jslot = slot; d->jpts = pk.pts; d->jflush = avc && eos; md_job(d, MDJ_DECODE); got = d->jgot; }
        int more = avc && eos && d->avc && mavc_queued(d->avc) > 0;   // H.264 pictures still queued: they follow this output, the last of them carries the end-of-stream flag
        dm_host_mutex_lock(&d->mu);
        d->infree[d->ninfree++] = pk.in;
        int ready = got || eos;   // an end-of-stream packet without a picture still yields an output to carry the flag back
        if (ready) { d->out[slot].pic = got; d->out[slot].pts = got && avc ? d->jpts_out : pk.pts; d->out[slot].flags = more ? pk.flags & ~4u : pk.flags; d->outq[d->noutq++] = slot; }
        else d->out[slot].used = 0;
        if (more) d->avc_drain = 1;
        int ineos = d->in_eos;
        pthread_mutex_unlock(&d->mu);
        if (!ineos) md_event(d, 0);   // no input is announced after an end-of-stream packet (h_movie_AcqIn)
        if (ready) md_event(d, 1);
    }
    pthread_mutex_unlock(&d->dmu);
}

HLE_FORCE("_ZN5movie7DecoderC1ENS_11DecoderTypeENS_19DecoderOutputFormatENS_11DecoderModeEi", "movie decoder: host implementation (no NVIDIA hardware decoder; VP8 / VP9 with libvpx, AVC with VideoToolbox)",
          void, h_movie_Decoder, void **self, int type, int fmt, int mode, int prio) {
    static void *vt; if (!vt) { uint8_t *t = guest_sym("_ZTVN5movie7DecoderE"); if (!t) die("movie: missing the vtable of movie::Decoder"); vt = t + 0x10; }
    md_syms();
    self[0] = vt; self[1] = NULL;
    // Only video (AVC / VP8 / VP9 = 1..3): NV12 output (Engage's Unity, CPU buffers), or RGBA output into the client's NVN texture (mode 2: Metroid Dread's VP9, Monster Hunter Rise's AVC);
    // anything else (audio) keeps a null impl and every method returns the SDK's failure code. VP8 / VP9 use libvpx, AVC uses VideoToolbox (hle_movie_avc.c)
    // Audio: Vorbis to PCM16 (Link's Awakening's movies carry a Vorbis track; without a decoder the SDK's player plays them silent), AAC to PCM16 with AudioToolbox (Monster Hunter Rise)
    int audio = (type == MD_TYPE_VORBIS || type == MD_TYPE_AAC) && fmt == MD_FMT_PCM16;
    if (!audio && (type < 1 || type > 3 || (fmt != MD_FMT_NV12 && !(fmt == MD_FMT_ABGR && mode == MD_MODE_TEX)))) { LOG("[RED] movie: decoder type %d output %d mode %d not supported by the host, every method returns failure", type, fmt, mode); return; }
    mdec_t *d = calloc(1, sizeof *d);
    pthread_mutex_init(&d->mu, NULL); pthread_mutex_init(&d->dmu, NULL); pthread_mutex_init(&d->wmu, NULL); pthread_cond_init(&d->wcv, NULL);
    d->type = type; d->fmt = fmt; d->mode = mode;
    // A Vorbis decoder keeps 64 outputs, an AAC decoder 32 (about 0.7 s), a video decoder 16. The host decodes far faster than the Switch, and a player that feeds its decoders in file order
    // stops at the first track with no free input. DRAGON QUEST XI S's player fills every track before it plays (IsPreDecodingCompleted: one output queued on each, polled once a frame),
    // while its render thread's first fetch takes every decoded picture and shows the newest. With 8 audio outputs, both audio tracks were full before 240 ms of video was decoded: the
    // extractor waited for an audio input, no picture followed the ones that fetch took, and the opening movie never started (in about half the boots on the Mac: whichever of the poll and the fetch came first).
    // Unity 2022.3's movie player (Monster Train 2's intro, an MP4 of H.264 and AAC interleaved packet by packet) takes every output it is offered and starts playing once it holds about
    // half a second of audio: measured with 64 audio and 8 video slots, it starts holding 27 audio and 8 video outputs and peaks at 30 audio, close to 32 (8 audio slots hang it on a black
    // screen whatever the video count: 32 video slots did not help). An audio slot is a PCM buffer allocated when first used (MD_PCM_CAP, 128 KB), so 64 cost at most 8 MB.
    // AAC no more than 32: Mario vs. Donkey Kong's movie player (1.0.1) keeps the outputs it holds in a ring of 32 entries per track (main+0xe93d4c: 0x200 bytes) and pushes with no
    // bounds check (main+0xe94140), so with 64 the 33rd AAC output it took wrote its index and time over the next heap block, and the opening movie crashed in the heap's free a second in.
    // The console's AAC decoder cannot offer more than 32, or the game would overflow there too
    // Video got 16 slots for Animal Crossing: New Horizons' in-flight movie (Movie/OpeningMovie.webm, VP9 1080p and Vorbis through movie::Extractor): its player also holds every output it is
    // offered (16 of 16 video, 59 of 64 audio), so how far its extractor gets ahead of the picture on screen is the video slots plus the 4 queued packets. With 8 that was 0.4 s, and the
    // audio behind the next video packet, whose clock the player follows, ran dry: 86 pictures in 25 s, a black screen with a loading icon. With 16 (or 32) all 1,960 play at 30 fps.
    // A video slot is allocated when first used, so the 8 more cost a greedy player 24 MB of NV12 at 1080p (64 MB as RGBA)
    d->nout = !audio ? MD_NOUT_VIDEO : type == MD_TYPE_AAC ? MD_NOUT_AAC : MD_NOUT;
    for (int i = 0; i < MD_NIN; i++) { d->in[i] = malloc(MD_INCAP); d->infree[i] = i; }
    d->ninfree = MD_NIN;
    pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 1 << 20);
    if (pthread_create(&d->wk, &a, md_worker, d)) die("movie: failed to create the decoder thread");
    pthread_attr_destroy(&a);
    self[1] = d;
    LOG("[movie] decoder %p: type %d output %d mode %d priority %d", (void *)self, type, fmt, mode, prio);
}
// SDK 4's constructor has no priority argument (Dark Souls Remastered, NintendoSDK_movie 4.5.0). Without this take-over the SDK built its own decoder and the host's Configure read it as a host one (a call through a null pointer)
HLE_FORCE("_ZN5movie7DecoderC1ENS_11DecoderTypeENS_19DecoderOutputFormatENS_11DecoderModeE", "movie decoder (SDK 4, no priority): host implementation", void, h_movie_Decoder4,
          void **self, int type, int fmt, int mode) { h_movie_Decoder(self, type, fmt, mode, 0); }
static void md_free(void **self) {
    mdec_t *d = md_of(self); if (!d) return;
    md_defer_drop(d);
    self[1] = NULL;
    LOG("[movie] decoder %p destroyed: %lu packets in, %lu frames out; decoded %llu packets, average %.2f ms per packet, longest %.2f ms", (void *)self, d->ncall[7], d->ncall[8],
        (unsigned long long)d->dec_n, d->dec_n ? d->dec_ns / 1e6 / d->dec_n : 0.0, d->dec_max / 1e6);
    dm_host_lock(&d->dmu); md_job(d, MDJ_QUIT); pthread_mutex_unlock(&d->dmu);
    pthread_join(d->wk, NULL);
    for (int i = 0; i < MD_NIN; i++) free(d->in[i]);
    for (int k = 0; k < MD_NOUT; k++) free(d->out[k].frm);
    free(d->vhdr[0]); free(d->vhdr[1]);
    pthread_mutex_destroy(&d->mu); pthread_mutex_destroy(&d->dmu); pthread_mutex_destroy(&d->wmu); pthread_cond_destroy(&d->wcv); free(d);
}
HLE_FORCE("_ZN5movie7DecoderD2Ev", "movie decoder destructor: frees the host object", void, h_movie_DecD2, void **self) { md_free(self); }
// complete-object destructor (D1): Metroid Dread calls it directly (main+0x225a08); without the take-over the SDK destroys the host object as a DecoderImpl and crashes
HLE_FORCE("_ZN5movie7DecoderD1Ev", "movie decoder destructor (complete object): frees the host object", void, h_movie_DecD1, void **self) { md_free(self); }
// deleting destructor: Unity allocates the object with its own allocator, calls D2 and frees it itself. Monster Hunter Rise deletes its decoders (new / delete), so after the host object the
// object itself goes back through the game's operator delete (guest code, called on the calling guest thread)
HLE_FORCE("_ZN5movie7DecoderD0Ev", "movie decoder deleting destructor: frees the host object, then the object with operator delete", void, h_movie_DecD0, void **self) {
    md_free(self);
    static void (*del)(void *); if (!del) del = (void (*)(void *))guest_sym("_ZdlPv");
    if (del) del(self); else LOG("[RED] movie: no operator delete in the game; the decoder object is not freed");
}

HLE_FORCE("_ZN5movie7Decoder9ConfigureEPNS_9MediaDataE", "movie decoder configure", int, h_movie_Configure, void **self, void *md) {
    mdec_t *d = md_of(self); if (!d) return 0x1000;
    if (MD_AUDIO(d)) {   // codec-specific data as Android's MediaCodec takes it: Vorbis csd-0 = identification header, csd-1 = setup header; AAC csd-0 = AudioSpecificConfig
        for (int k = 0; k < 2 && md; k++) { void *b = NULL; char key[8]; snprintf(key, sizeof key, "csd-%d", k);
            if (G.find_buf(md, key, &b) && b && G.data(b) && G.size(b)) { size_t n = G.size(b); free(d->vhdr[k]); d->vhdr[k] = malloc(n); memcpy(d->vhdr[k], G.data(b), n); d->vhdr_n[k] = (int)n; } }
        int ch = 0, rate = 0; if (md) { G.find_i32(md, "channel-count", &ch); G.find_i32(md, "sample-rate", &rate); }
        if (ch > 0) d->ch = ch; if (rate > 0) d->rate = rate;
        MD_TRACE(d, 0, "Configure (%s): %d channels %d Hz, headers %d + %d bytes", d->type == MD_TYPE_AAC ? "AAC" : "Vorbis", ch, rate, d->vhdr_n[0], d->vhdr_n[1]);
        return 0;
    }
    if (d->type == MD_TYPE_AVC && md) {   // SPS / PPS: csd-0 / csd-1 (Annex B, as Android's MediaCodec takes them) or, from this SDK's MP4 demuxer (Super Mario Odyssey), "avcc" = the avcC box
        md_avc_open(d);
        void *b = NULL;
        for (int k = 0; k < 2; k++) { char key[8]; snprintf(key, sizeof key, "csd-%d", k); if (G.find_buf(md, key, &b) && b && G.data(b) && G.size(b)) mavc_param_sets(d->avc, G.data(b), G.size(b)); }
        if (G.find_buf(md, "avcc", &b) && b && G.data(b) && G.size(b)) mavc_avcc(d->avc, G.data(b), G.size(b));
    }
    int w = 0, h = 0; if (md) { G.find_i32(md, "width", &w); G.find_i32(md, "height", &h); }
    dm_host_mutex_lock(&d->mu); if (w > 0 && h > 0 && !d->fmt_sent) { d->w = w; d->h = h; } pthread_mutex_unlock(&d->mu);
    MD_TRACE(d, 0, "Configure: MediaData width %d height %d", w, h);
    return 0;
}
HLE_FORCE("_ZN5movie7Decoder20RegisterClientEventsEPNS_13DecoderEventsE", "movie decoder: register client events", int, h_movie_RegEv, void **self, void **ev) {
    mdec_t *d = md_of(self); if (!d) return 0x1000;
    int late = 0;
    for (int i = 0; i < 4; i++) { d->ev[i] = ev && ev_ok(ev[i]) ? ev[i] : NULL; late += !d->ev[i]; }
    d->evc = ev;
    MD_TRACE(d, 1, "RegisterClientEvents: %p %p %p %p%s", ev ? ev[0] : 0, ev ? ev[1] : 0, ev ? ev[2] : 0, ev ? ev[3] : 0, late ? " (not all events yet: each entry is read when it is signalled)" : "");
    return 0;
}
HLE_FORCE("_ZN5movie7Decoder5StartEv", "movie decoder start: all inputs empty, notify the client", int, h_movie_Start, void **self) {
    mdec_t *d = md_of(self); if (!d) return 0x2002;
    dm_host_mutex_lock(&d->mu); d->started = 1; pthread_mutex_unlock(&d->mu);
    MD_TRACE(d, 2, "Start");
    md_defer(d, 0);
    return 0;
}
// clear the queues and slots; with close, the libvpx decoder is closed too (rebuilt at the next packet)
static void md_reset(mdec_t *d, int close) {
    dm_host_lock(&d->dmu);   // wait for the packet being decoded
    if (close) md_job(d, MDJ_CLOSE);
    if (d->avc) mavc_drop(d->avc);   // H.264 pictures waiting for their turn belong to the old position
    dm_host_mutex_lock(&d->mu);
    // Every input goes back to the free list, as with MediaCodec, but one the client holds may still be sent: Big Brain Academy: Brain vs. Brain's event thread acquires inputs while its
    // control thread flushes to loop an activity preview, and sends them after the flush (SendInputBufferForDecode claims them back). Those are withheld for a while (h_movie_AcqIn)
    uint64_t now = now_ns();
    for (int i = 0; i < MD_NIN; i++) { d->infree[i] = i; d->instale[i] = d->inown[i] ? now : 0; d->inown[i] = 0; }
    d->ninfree = MD_NIN; d->noutq = 0; d->npend = 0; d->avc_drain = 0; d->in_eos = 0;
    // a flush keeps each slot's last picture: the same game reads an output it acquired before the flush right after it (a black frame in the loop otherwise)
    for (int i = 0; i < MD_NOUT; i++) { d->out[i].used = 0; if (close) d->out[i].pic = 0; }
    pthread_mutex_unlock(&d->mu);
    pthread_mutex_unlock(&d->dmu);
}
HLE_FORCE("_ZN5movie7Decoder4StopEv", "movie decoder stop", int, h_movie_Stop, void **self) {
    mdec_t *d = md_of(self); if (!d) return 0x2003;
    MD_TRACE(d, 3, "Stop");
    dm_host_lock(&g_dfmu); pthread_mutex_lock(&d->mu); d->started = 0; pthread_mutex_unlock(&d->mu); pthread_mutex_unlock(&g_dfmu);   // the deferred thread signals holding g_dfmu: taking it guarantees no signal afterwards
    md_defer_drop(d);
    md_reset(d, 1); return 0;
}
HLE_FORCE("_ZN5movie7Decoder5ResetEv", "movie decoder reset", int, h_movie_Reset, void **self) {
    mdec_t *d = md_of(self); if (!d) return 0x1000;
    MD_TRACE(d, 4, "Reset"); md_reset(d, 1); d->fmt_sent = 0; return 0;
}
// flush: inputs and outputs all go back; libvpx is kept (the next packet after a seek is a key frame). No "input available": as with MediaCodec, nothing is announced after a flush until the next Start.
// Metroid Dread ends a movie with Flush, Flush, Stop, destroy; a signal raised at the flush let its "input available" handler post the wrapper object to the message queue, the object was destroyed right after, and the worker thread read the freed object; when looping it calls Start after the Flush
HLE_FORCE("_ZN5movie7Decoder5FlushEv", "movie decoder flush: all inputs and outputs returned", int, h_movie_Flush, void **self) {
    mdec_t *d = md_of(self); if (!d) return 0x1000;
    MD_TRACE(d, 5, "Flush"); md_defer_drop(d); md_reset(d, 0);
    if (d->vb) { dm_host_lock(&d->dmu); stb_vorbis_flush_pushdata(d->vb); pthread_mutex_unlock(&d->dmu); }   // a seek: start the overlap afresh
    return 0;
}
HLE_FORCE("_ZN5movie7Decoder23AcquireInputBufferIndexEPiS1_", "movie decoder: acquire a free input index", int, h_movie_AcqIn, void **self, int *idx, int *left) {
    mdec_t *d = md_of(self); if (!d) return 0x1000;
    if (!idx || !left) return 0x100b;
    // No input indices after Stop (as with MediaCodec): Metroid Dread's "input available" handler (main+0x222f14) posts the decoder wrapper object to the player manager's message queue once it gets an index;
    // after Stop returned every input index, a late input event could still get one, the wrapper was destroyed afterwards, and the worker thread (main+0x223bd4) read the freed object and crashed
    dm_host_mutex_lock(&d->mu);
    int r = MD_NO_INPUT; *idx = -1; *left = 0;
    // An input the client held across a flush (md_reset) is not handed out again for MD_STALE_NS unless the client sends it first: handing it out while the client still meant to send
    // it gave the client one buffer twice (a packet dropped as sent twice, VideoToolbox -12909). A client that abandons it gets it back after that time instead of losing a buffer
    // No input after an end-of-stream packet until Flush / Stop either, as with MediaCodec, which movie::Decoder wraps (ACodec keeps the input buffers once the input port saw EOS).
    // DRAGON QUEST XI S's movie player sends an end-of-stream packet (length 0, time 0) for every input it gets once its extractor is done, until the game stops the movie: when the
    // prologue movie was skipped, each became an end-of-stream output, the player made each one an audio sample, and 2.25 GB were mapped within seconds (the allocator then broke)
    uint64_t now = now_ns(), wake = 0; int k = -1;
    for (int j = 0; j < d->ninfree && k < 0; j++) { int x = d->infree[j]; if (!d->instale[x] || now - d->instale[x] >= MD_STALE_NS) k = j; }
    if (d->in_eos) k = -1;
    if (k >= 0 && d->started) {
        *idx = d->infree[k]; memmove(d->infree + k, d->infree + k + 1, (size_t)(--d->ninfree - k) * sizeof *d->infree); *left = d->ninfree; r = 0;
        d->inown[*idx] = 1; d->instale[*idx] = 0;
    } else if (d->ninfree && d->started && !d->in_eos) {   // only withheld inputs are free: announce input when the first of them comes back
        wake = UINT64_MAX; for (int j = 0; j < d->ninfree; j++) { uint64_t t = d->instale[d->infree[j]] + MD_STALE_NS; if (t < wake) wake = t; }
    }
    pthread_mutex_unlock(&d->mu);
    if (wake) md_defer_at(d, 0, wake);   // after mu: Stop takes the deferred queue's lock before mu
    MD_TRACE(d, 6, "AcquireInputBufferIndex → %#x index %d left %d", r, *idx, *left);
    return r;
}
HLE_FORCE("_ZN5movie7Decoder14GetInputBufferEiPNS_6BufferE", "movie decoder: hand out an input buffer", int, h_movie_GetIn, void **self, int i, void *buf) {
    mdec_t *d = md_of(self); if (!d) return 0x1000;
    if (!buf || i < 0 || i >= MD_NIN) return 0x100b;
    G.set_data(buf, d->in[i], MD_INCAP); G.set_range(buf, 0, (int)MD_INCAP);
    return 0;
}
HLE_FORCE("_ZN5movie7Decoder24SendInputBufferForDecodeEiiilj", "movie decoder: send a packet for decoding (VP8 / VP9 with libvpx, AVC with VideoToolbox)", int, h_movie_SendIn,
          void **self, int i, int off, int size, int64_t pts, uint32_t flags) {
    mdec_t *d = md_of(self); if (!d) return 0x1000;
    if (i < 0 || i >= MD_NIN || off < 0 || size < 0 || (uint32_t)off + (uint32_t)size > MD_INCAP) return 0x100b;
    dm_host_mutex_lock(&d->mu);
    // An index the client acquired before a Flush and sends after it (Big Brain Academy: Brain vs. Brain loops its activity previews this way): the flush put it back in the free list,
    // so it is claimed from there now. Before, it could be handed out a second time, two packets shared one buffer (VideoToolbox -12909 on the overwritten one) and the fifth pending
    // packet wrote past pend[] into npend and dmu (the process died destroying dmu, 2 runs in 5)
    for (int k = 0; k < d->ninfree; k++) if (d->infree[k] == i) {
        memmove(d->infree + k, d->infree + k + 1, (size_t)(--d->ninfree - k) * sizeof *d->infree);
        static int once; if (!once++) LOG("[movie] input index %d sent after a flush returned it: claimed from the free list (first case)", i);
        break;
    }
    d->inown[i] = d->instale[i] = 0;
    if (flags & 4) d->in_eos = 1;   // the last input until Flush / Stop (h_movie_AcqIn)
    for (int k = 0; k < d->npend; k++) if (d->pend[k].in == i) { pthread_mutex_unlock(&d->mu); if (!d->red_dec++) LOG("[RED] movie: input index %d sent twice, the second packet is dropped (first case)", i); return 0x100b; }
    if (d->npend == MD_NIN) { pthread_mutex_unlock(&d->mu); if (!d->red_dec++) LOG("[RED] movie: more packets pending than input buffers, one dropped (first case)"); return 0x100b; }
    int fmt_now = 0;
    if (d->type == MD_TYPE_VP9 && !d->w && size > 0 && vp9_size(d->in[i] + off, size, &d->w, &d->h)) LOG("[movie] VP9 key frame: %d×%d", d->w, d->h);
    if (d->type == MD_TYPE_VP8 && !d->w && size > 0 && vp8_size(d->in[i] + off, size, &d->w, &d->h)) LOG("[movie] VP8 key frame: %d×%d", d->w, d->h);
    if (!d->fmt_sent) { d->fmt_sent = 1; fmt_now = 1; if (!d->w && !MD_AUDIO(d)) { d->w = 1920; d->h = 1080; } }   // the size is fixed from here on (slots are allocated by it)
    d->pend[d->npend].in = i; d->pend[d->npend].off = off; d->pend[d->npend].size = size; d->pend[d->npend].pts = pts; d->pend[d->npend].flags = flags; d->npend++;
    int queued = d->npend;
    pthread_mutex_unlock(&d->mu);
    MD_TRACE(d, 7, "SendInputBufferForDecode index %d offset %d length %d time %lld µs flags %#x (%d packets queued)", i, off, size, (long long)pts, flags, queued);
    if (fmt_now) md_event(d, 2);
    md_pump(d);
    return 0;
}

HLE_FORCE("_ZN5movie7Decoder24AcquireOutputBufferIndexEPiPlPjS1_", "movie decoder: acquire a ready output index", int, h_movie_AcqOut,
          void **self, int *idx, int64_t *pts, uint32_t *flags, int *left) {
    mdec_t *d = md_of(self); if (!d) return 0x1000;
    if (!idx || !pts || !flags || !left) return 0x100b;
    dm_host_mutex_lock(&d->mu);
    int r = MD_NO_OUTPUT; *idx = -1; *left = 0;
    if (d->noutq && d->started) {
        int s = d->outq[0]; memmove(d->outq, d->outq + 1, --d->noutq * sizeof *d->outq);
        *idx = s; *pts = d->out[s].pts; *flags = d->out[s].flags; *left = d->noutq; r = 0;
    }
    pthread_mutex_unlock(&d->mu);
    if (!r) MD_TRACE(d, 8, "AcquireOutputBufferIndex → index %d time %lld flags %#x left %d", *idx, (long long)*pts, *flags, *left);
    return r;
}
static void md_format(mdec_t *d, void *md) {
    if (MD_AUDIO(d)) { G.set_i32(md, "channel-count", d->ch > 0 ? d->ch : 2); G.set_i32(md, "sample-rate", d->rate > 0 ? d->rate : 48000); return; }
    int w, h, stride, slice; md_dims(d, &w, &h, &stride, &slice);
    G.set_i32(md, "width", w); G.set_i32(md, "height", h);
    G.set_rect(md, "crop", 0, 0, w - 1, h - 1);
    G.set_i32(md, "stride", stride); G.set_i32(md, "slice-height", slice);
    G.set_i32(md, "nv12-y-stride", stride); G.set_i32(md, "nv12-y-offset", 0); G.set_i32(md, "nv12-uv-offset", stride * slice);
    // The interleaved UV plane has the luma plane's byte stride. CRI Sofdec2's player (Catherine: Full Body, main+0x34e6bc) reads it with the other three and checks none of the results:
    // without it the chroma stride was stack garbage and its NV12 → RGB conversion read far outside guest memory on the first picture (main+0x344f74); in DRAGON QUEST VII Reimagined
    // every chroma row was read from the first one (a grey movie with vertical bands). The picture size again, as the SDK's DecoderImpl::GetOutputBufferFormatFromDecoder
    // (SDK 19 subsdk0+0x121f50) also sets it
    G.set_i32(md, "nv12-uv-stride", stride); G.set_i32(md, "nv12-width", w); G.set_i32(md, "nv12-height", h);
}
HLE_FORCE("_ZN5movie7Decoder15GetOutputFormatEPNS_9MediaDataE", "movie decoder: output format (NV12)", int, h_movie_OutFmt, void **self, void *md) {
    mdec_t *d = md_of(self); if (!d) return 0x1000;
    if (!md) return 0x100b;
    md_format(d, md); MD_TRACE(d, 9, "GetOutputFormat");
    return 0;
}
HLE_FORCE("_ZN5movie7Decoder21GetOutputBufferFormatEiPNS_9MediaDataE", "movie decoder: output frame format (NV12)", int, h_movie_BufFmt, void **self, int i, void *md) {
    mdec_t *d = md_of(self); if (!d) return 0x1000;
    if (!md) return 0x100b;
    md_format(d, md); MD_TRACE(d, 10, "GetOutputBufferFormat index %d", i);
    return 0;
}
HLE_FORCE("_ZN5movie7Decoder19GetOutputBufferSizeEPiS1_", "movie decoder: output buffer size (one NV12 frame)", int, h_movie_OutSize, void **self, int *size, int *mode) {
    mdec_t *d = md_of(self); if (!d) return 0x1000;
    if (!size || !mode) return 0x100b;
    // AAC: one frame, 1024 samples per channel (2048 with SBR, object type 5 or 29 in the AudioSpecificConfig), which is what each output holds. Mario vs. Donkey Kong's player
    // (1.0.1) makes its AudioOut buffers this size, decodes into them and appends each whole, advancing its audio clock by size / 4 samples (main+0xe92a74): with the 128 KB
    // of a Vorbis bound every 4 KB AAC frame was played as 0.68 s, the clock crawled, the player stopped taking audio and the opening movie froze on its first second.
    // Vorbis packets vary in length (a long block gives far more than a short one), so its outputs stay at the bound
    if (MD_AUDIO(d)) {
        int sbr = d->vhdr_n[0] > 0 && ((d->vhdr[0][0] >> 3) == 5 || (d->vhdr[0][0] >> 3) == 29);
        *size = d->type == MD_TYPE_AAC && d->ch > 0 ? 1024 * (sbr ? 2 : 1) * d->ch * 2 : MD_PCM_CAP; *mode = d->mode;
        MD_TRACE(d, 11, "GetOutputBufferSize → %d (PCM)", *size); return 0;
    }
    int w, h, stride, slice; md_dims(d, &w, &h, &stride, &slice);
    *size = stride * slice * 3 / 2; *mode = d->mode;   // the SDK fills the second output with the decoder mode
    MD_TRACE(d, 11, "GetOutputBufferSize → %d (%d×%d)", *size, w, h);
    return 0;
}
// Write one frame into the client buffer: the decoded picture, or a black frame (video range Y 16, UV 128) when there is none (an end-of-stream packet); a client texture keeps its last frame then
// Native texture mode: the client Buffer's data pointer is the NVNtexture object itself (capacity given as 8; Metroid Dread main+0x226c98: the texture object embedded at +0x18 of a texture pool entry), plus the NVN device and a sync object (SetNvnDev / SetNvnSync).
// In the SDK, NnCodecConsumer draws the decoded frame into that texture on the GPU and signals the sync when done; the host writes the texture storage on the CPU (as WriteTexels; the replay layer re-uploads dirty pages), and the sync counts as done once written
int nvn_host_write_texture(const void *nvntex, const void *src, int w, int h, size_t row, int *tw, int *th);
int nvn_host_signal_sync(void *nvnsync);
static int md_out_tex(mdec_t *d, int i, void *buf) {
    // SDK 4's Buffer has no sync object (Dark Souls Remastered's subsdk0 exports no GetNvnSync): the game reads the texture once GetOutputBuffer returns, which the CPU write already satisfies
    static int sync_looked; if (!sync_looked) { G.nvn_sync = guest_sym("_ZNK5movie6Buffer10GetNvnSyncEv"); sync_looked = 1; }
    uint8_t *p = G.data(buf); size_t cap = G.cap(buf);
    if (!p || cap < 8) { LOG("[RED] movie: client buffer in native-texture mode is %zu bytes, needs 8 (a texture object)", cap); return 0x100b; }
    void *tex = p; int tw = 0, th = 0, f = nvn_host_write_texture(tex, NULL, 0, 0, 0, &tw, &th);
    if (f < 0) { LOG("[RED] movie: buffer data %p in native-texture mode is not a texture object", tex); return 0x100b; }
    if (f != 0x25 && f != 0x38 && f != 0x6d && f != 0x37 && !d->red_img++) LOG("[RED] movie: client texture format %#x is not RGBA8 / RGBX8 (sRGB included), writing in RGBA byte order", f);   // Metroid Dread: RGBX8_SRGB
    int w = d->w < tw ? d->w : tw, h = d->h < th ? d->h : th;
    // No picture (an end-of-stream output): nothing is drawn and the texture keeps the last frame, as the SDK's consumer has no frame to draw. A black frame here flashed at the
    // end of every loop of Big Brain Academy: Brain vs. Brain's activity previews, which read the end-of-stream output's buffer
    if (d->out[i].pic && d->out[i].frm) nvn_host_write_texture(tex, d->out[i].frm, w, h, (size_t)d->w * 4, NULL, NULL);
    void *sy = G.nvn_sync ? G.nvn_sync(buf) : NULL; if (sy && nvn_host_signal_sync(sy) && !d->red_dec++) LOG("[RED] movie: the buffer's sync object %p is not an NVNsync", sy);
    MD_TRACE(d, 12, "GetOutputBuffer index %d → texture %p (%d×%d format %#x)%s", i, tex, tw, th, f, d->out[i].pic ? "" : " (no picture: the texture keeps its last frame)");
    return 0;
}
HLE_FORCE("_ZN5movie7Decoder15GetOutputBufferEiPNS_6BufferE", "movie decoder: get a frame", int, h_movie_GetOut, void **self, int i, void *buf) {
    mdec_t *d = md_of(self); if (!d) return 0x1000;
    if (!buf || i < 0 || i >= d->nout) return 0x100b;
    if (d->mode == MD_MODE_TEX) return md_out_tex(d, i, buf);
    // The decoder writes from the start of the client's buffer. movie::Buffer::Data() is Base() + Offset() and Capacity() the whole buffer (STEINS;GATE RE:BOOT's subsdk0), so a buffer whose
    // read offset the client advanced was written past its end, over the guest heap (from PR #36): the range is reset first. Not in native-texture mode (md_out_tex),
    // where Data() is the texture object the client points it at and nothing is written through it
    G.set_range(buf, 0, 0);
    if (MD_AUDIO(d)) {   // PCM16 interleaved; an output without samples (end of stream) is empty
        size_t n = d->out[i].pic ? d->out[i].bytes : 0, cap = G.cap(buf); uint8_t *p = G.data(buf);
        if (!p || cap < n) { LOG("[RED] movie: client audio output buffer is %zu bytes, this packet needs %zu", cap, n); G.set_range(buf, 0, 0); return 0x100b; }
        if (n) memcpy(p, d->out[i].frm, n);
        G.set_range(buf, 0, (int)n);
        MD_TRACE(d, 12, "GetOutputBuffer index %d → PCM %zu bytes", i, n);
        return 0;
    }
    int w, h, stride, slice; md_dims(d, &w, &h, &stride, &slice);
    size_t need = (size_t)stride * slice * 3 / 2, cap = G.cap(buf);
    uint8_t *p = G.data(buf);
    if (!p || cap < need) { LOG("[RED] movie: client output buffer is %zu bytes, a frame needs %zu", cap, need); G.set_range(buf, 0, 0); return 0x100b; }
    if (d->out[i].pic && d->out[i].frm) memcpy(p, d->out[i].frm, need);
    else { memset(p, 16, (size_t)stride * slice); memset(p + (size_t)stride * slice, 128, need - (size_t)stride * slice); }
    G.set_range(buf, 0, (int)need);
    MD_TRACE(d, 12, "GetOutputBuffer index %d → %zu bytes%s", i, need, d->out[i].pic ? "" : " (black frame)");
    return 0;
}
HLE_FORCE("_ZN5movie7Decoder15GetOutputBufferEiPNS_6BufferEPNS_9MediaDataE", "movie decoder: get a frame with its format", int, h_movie_GetOutFmt, void **self, int i, void *buf, void *md) {
    mdec_t *d = md_of(self); if (!d) return 0x1000;
    if (md) md_format(d, md);
    return h_movie_GetOut(self, i, buf);
}
HLE_FORCE("_ZN5movie7Decoder24ReleaseOutputBufferIndexEi", "movie decoder: release an output index (decodes queued packets again)", int, h_movie_RelOut, void **self, int i) {
    mdec_t *d = md_of(self); if (!d) return 0x1000;
    if (i < 0 || i >= d->nout) return 0x100b;
    dm_host_mutex_lock(&d->mu);
    d->out[i].used = 0; d->out[i].pic = 0;
    int queued = d->npend, drain = d->avc_drain;
    pthread_mutex_unlock(&d->mu);
    MD_TRACE(d, 13, "ReleaseOutputBufferIndex index %d (%d packets queued%s)", i, queued, drain ? ", H.264 pictures draining" : "");
    // An H.264 drain waits for a free slot too: AI: THE SOMNIUM FILES (Unity 2017.4) held all 16 video slots when its logo movie's end-of-stream packet came, the drain stopped
    // with no packet queued, the last pictures and the end-of-stream flag never came out, and the game waited for the movie's end for good
    if (queued || drain) md_pump(d);
    return 0;
}
HLE_FORCE("_ZN5movie7Decoder12GetLastErrorEv", "movie decoder: last error (none)", int, h_movie_LastErr, void **self) { (void)self; return 0; }
