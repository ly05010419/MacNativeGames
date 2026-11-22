/* The host movie::Decoder's pacing (runtime/movie_host.c) when the game seeks or loops without Flush.
   Mario Kart 8 Deluxe's mode-menu preview is one H.264 file of clips. A on Grand Prix moves the reader from ~10 s to the
   engine-class clip at 144 s without a Flush, and the menu opens only when that clip's first frame comes out. The
   pacing counted 144 s from the first frame of the preview, so the menu waited 134 s on the Mac (and about a minute on
   the iPhone, where the player had sat longer in the menus) with every output slot held (2026-11-22).
   Here, with no game file: VP9 frames from libvpx's own encoder (the pacing is the same for both codecs), a fake clock
   that moves one 60 Hz game frame per tick, and the game's side as the stand-ins of games/eow/tests/movie_stop: each
   tick calls movie_host_tick (a present), acquires one output and releases it.
   - forward: pts 16.7, 33.3, 50 ms, then 144.0167, 144.0333, 144.05 s: every frame comes out, one per tick, the frame
     after the jump on the tick after the one before it (before the fix: ~8,000 ticks later);
   - backward (a loop): pts 5.0167 .. 5.05 s, then 16.7 .. 50 ms: every frame comes out, none dropped as "late" (before
     the fix the frames after the jump were all due at once and two of three were dropped).
   Prints one JSON line; exit 0 = pass. */
#include "../runtime/movie_host.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vpx/vp8cx.h>
#include <vpx/vpx_encoder.h>

_Noreturn void runtime_fail(const char *s) { fprintf(stderr, "movie pace probe: runtime_fail: %s\n", s); exit(3); }
void *runtime_symbol(const char *name) { (void)name; return NULL; }

static int checks;
static void need(int c, const char *fmt, ...) {
    checks++;
    if (c) return;
    va_list ap; va_start(ap, fmt); fprintf(stderr, "movie pace probe: FAILED: "); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap);
    printf("{\"test\":\"movie_pace\",\"status\":\"fail\",\"checks\":%d}\n", checks);
    exit(1);
}

/* ---- stand-ins for the game (movie::Buffer, movie::MediaData, nn::os events) ---- */
typedef struct { int offset, size; void *data; size_t cap; } Buf;
static void buf_set_range(void *b, int o, int s) { ((Buf *)b)->offset = o; ((Buf *)b)->size = s; }
static void buf_set_data(void *b, void *d, size_t c) { ((Buf *)b)->data = d; ((Buf *)b)->cap = c; }
static void *buf_base(const void *b) { return ((const Buf *)b)->data; }
static size_t buf_capacity(const void *b) { return ((const Buf *)b)->cap; }
typedef struct { const char *mime; int w, h; } Md;
static bool md_find_int32(const void *m, const char *k, int *v) {
    const Md *d = m; if (!strcmp(k, "width")) { *v = d->w; return true; } if (!strcmp(k, "height")) { *v = d->h; return true; }
    return false;
}
static bool md_find_string(const void *m, const char *k, const char **v) {
    if (strcmp(k, "mime")) return false; *v = ((const Md *)m)->mime; return true;
}
static void md_set_int32(void *m, const char *k, int v) { (void)m; (void)k; (void)v; }
static void md_set_rect(void *m, const char *k, int l, int t, int r, int b) { (void)m; (void)k; (void)l; (void)t; (void)r; (void)b; }
static void md_set_string(void *m, const char *k, const char *v) { (void)m; (void)k; (void)v; }
static void signal_event(void *ev) { (void)ev; }
static char fake_vtable[0xa8];

static uint64_t clock_ns;
static uint64_t fake_clock(void) { return clock_ns; }

typedef void (*ctor_t)(void *, int, int, int, int); typedef void (*obj_t)(void *);
typedef int (*md_t)(void *, void *); typedef int (*int_t)(void *); typedef int (*pp_t)(void *, int *, int *);
typedef int (*ib_t)(void *, int, void *); typedef int (*send_t)(void *, int, int, int, int64_t, unsigned);
typedef int (*acq_t)(void *, int *, int64_t *, unsigned *, int *); typedef int (*i_t)(void *, int);
#define E(t, w) ((t)movie_host_entry(w))
enum { W = 64, H = 64, FRAMES = 6, MAX_TICKS = 600 };
#define TICK_NS 16667000ULL   /* one 60 Hz game frame (rounded up to whole microseconds, as the pts are) */

/* FRAMES VP9 frames of a W x H picture, from libvpx's encoder. */
static void encode(uint8_t *out[FRAMES], size_t sizes[FRAMES]) {
    vpx_codec_enc_cfg_t cfg; vpx_codec_ctx_t enc;
    need(vpx_codec_enc_config_default(vpx_codec_vp9_cx(), &cfg, 0) == VPX_CODEC_OK, "encoder config");
    cfg.g_w = W; cfg.g_h = H; cfg.g_timebase.num = 1; cfg.g_timebase.den = 60; cfg.g_lag_in_frames = 0;
    need(vpx_codec_enc_init(&enc, vpx_codec_vp9_cx(), &cfg, 0) == VPX_CODEC_OK, "encoder init");
    vpx_image_t img; need(vpx_img_alloc(&img, VPX_IMG_FMT_I420, W, H, 1) != NULL, "image");
    int n = 0;
    for (int f = 0; f < FRAMES; f++) {
        for (int y = 0; y < H; y++) memset(img.planes[0] + y * img.stride[0], (y * 4 + f * 16) & 0xff, W);
        for (int p = 1; p < 3; p++) for (int y = 0; y < H / 2; y++) memset(img.planes[p] + y * img.stride[p], 128, W / 2);
        need(vpx_codec_encode(&enc, &img, f, 1, f == 0 ? VPX_EFLAG_FORCE_KF : 0, VPX_DL_GOOD_QUALITY) == VPX_CODEC_OK, "encode");
        vpx_codec_iter_t it = NULL; const vpx_codec_cx_pkt_t *pkt;
        while ((pkt = vpx_codec_get_cx_data(&enc, &it)) != NULL) {
            if (pkt->kind != VPX_CODEC_CX_FRAME_PKT || n == FRAMES) continue;
            out[n] = malloc(pkt->data.frame.sz); memcpy(out[n], pkt->data.frame.buf, pkt->data.frame.sz);
            sizes[n++] = pkt->data.frame.sz;
        }
    }
    vpx_img_free(&img); vpx_codec_destroy(&enc);
    need(n == FRAMES, "%d VP9 frames encoded", n);
}

/* Configure, Start and send the frames with the given pts (no Flush anywhere, as the game), then tick the clock one
   game frame at a time, taking at most one output per tick as the game does. got[i] = pts of the i-th frame out,
   at[i] = the tick it came out on. Returns how many came out. */
static int play(void *dec, uint8_t *frames[], size_t sizes[], const int64_t pts[FRAMES], int64_t got[FRAMES], int at[FRAMES]) {
    Md fmt = {"video/x-vnd.on2.vp9", W, H};
    need(E(md_t, MOVIE_CONFIGURE)(dec, &fmt) == 0, "Configure");
    need(E(int_t, MOVIE_START)(dec) == 0, "Start");
    for (int f = 0; f < FRAMES; f++) {
        int idx = -1, rem = 0; need(E(pp_t, MOVIE_ACQUIRE_INPUT)(dec, &idx, &rem) == 0, "input %d", f);
        Buf ib = {0}; need(E(ib_t, MOVIE_GET_INPUT)(dec, idx, &ib) == 0 && ib.cap >= sizes[f], "input buffer");
        memcpy(ib.data, frames[f], sizes[f]);
        need(E(send_t, MOVIE_SEND_INPUT)(dec, idx, 0, (int)sizes[f], pts[f], 0) == 0, "send frame %d", f);
    }
    int n = 0;
    for (int tick = 1; tick <= MAX_TICKS && n < FRAMES; tick++) {
        clock_ns += TICK_NS; movie_host_tick();
        int idx = -1, rem = 0; int64_t t = 0; unsigned fl = 0;
        if (E(acq_t, MOVIE_ACQUIRE_OUTPUT)(dec, &idx, &t, &fl, &rem) != 0) continue;
        need(idx >= 0 && !(fl & 4), "a frame, not end of stream");
        got[n] = t; at[n++] = tick;
        need(E(i_t, MOVIE_RELEASE_OUTPUT)(dec, idx) == 0, "release output %d", idx);
    }
    need(E(int_t, MOVIE_STOP)(dec) == 0, "Stop");
    return n;
}

static void check_run(const char *what, const int64_t pts[FRAMES], int n, const int64_t got[FRAMES], const int at[FRAMES]) {
    need(n == FRAMES, "%s: %d of %d frames came out within %d game frames (the rest held or dropped)", what, n, FRAMES, MAX_TICKS);
    for (int i = 0; i < FRAMES; i++) need(got[i] == pts[i], "%s: frame %d has pts %lld, expected %lld (dropped or out of order)", what, i, (long long)got[i], (long long)pts[i]);
    for (int i = 1; i < FRAMES; i++) need(at[i] == at[i - 1] + 1, "%s: frame %d came out %d game frames after frame %d", what, i, at[i] - at[i - 1], i - 1);
}

int main(void) {
    MovieGuest api = {signal_event, buf_set_range, buf_set_data, buf_base, buf_capacity, md_find_int32, md_find_string,
                      md_set_int32, md_set_rect, md_set_string, NULL, NULL, NULL, fake_vtable + 0x10, {0}};
    movie_host_set_guest(&api);
    movie_host_set_clock(fake_clock);
    clock_ns = 1000000000ULL;
    uint8_t *frames[FRAMES]; size_t sizes[FRAMES];
    encode(frames, sizes);

    void *dec[2] = {0}; E(ctor_t, MOVIE_CTOR)(dec, 3, 4, 1, 16);
    need(dec[1] != NULL, "VP9 decoder on the host");
    void *events[4] = {&events[0], &events[1], &events[2], &events[3]};
    need(E(md_t, MOVIE_REGISTER_EVENTS)(dec, events) == 0, "RegisterClientEvents");

    int64_t got[FRAMES]; int at[FRAMES];
    static const int64_t forward[FRAMES] = {16666, 33333, 50000, 144016666, 144033333, 144050000};
    check_run("forward seek", forward, play(dec, frames, sizes, forward, got, at), got, at);
    static const int64_t backward[FRAMES] = {5016666, 5033333, 5050000, 16666, 33333, 50000};
    check_run("loop back", backward, play(dec, frames, sizes, backward, got, at), got, at);

    E(obj_t, MOVIE_D2)(dec);
    for (int f = 0; f < FRAMES; f++) free(frames[f]);
    printf("{\"test\":\"movie_pace\",\"status\":\"pass\",\"checks\":%d}\n", checks);
    return 0;
}
