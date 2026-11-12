/* The host VP9 movie::Decoder (runtime/movie_host.c) when the game stops a movie while it still holds a decoded frame.
   Echoes of Wisdom's movie player fetches a frame it acquired (AcquireOutputBufferIndex) from one thread while another
   stops the decoder; Stop frees every frame. Before the fix a held index stayed valid, and GetOutputBuffer converted from
   a null frame: the intermittent crash at main+0x189714c when the intro movie stops. Here, deterministically: decode
   frames (VP9 from libvpx's own encoder, so no game file), acquire one, Stop, then every call for that index must answer
   ST_ERROR (0x1000) without touching freed memory (built with AddressSanitizer), and the decoder must work again after
   a new Configure and Start; the same after Reset. Stand-ins replace the game's movie::Buffer, movie::MediaData and
   nn::os events, as the probes of other ports do. */
#include "../../runtime/movie_host.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vpx/vp8cx.h>
#include <vpx/vpx_encoder.h>

_Noreturn void runtime_fail(const char *s) { fprintf(stderr, "movie stop test: runtime_fail: %s\n", s); exit(3); }
void *runtime_symbol(const char *name) { (void)name; return NULL; }
void dm_nvn_write_texels(const void *t, const void *r, const void *src) { (void)t; (void)r; (void)src; }
void *objc_autoreleasePoolPush(void) { return NULL; }
void objc_autoreleasePoolPop(void *pool) { (void)pool; }

static int checks;
static void need(int c, const char *fmt, ...) {
    checks++;
    if (c) return;
    va_list ap; va_start(ap, fmt); fprintf(stderr, "movie stop test: FAILED: "); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap); exit(1);
}

/* ---- stand-ins for the game ---- */
typedef struct { int offset, size; void *data; size_t cap; } Buf;
static void buf_set_range(void *b, int o, int s) { ((Buf *)b)->offset = o; ((Buf *)b)->size = s; }
static void buf_set_data(void *b, void *d, size_t c) { ((Buf *)b)->data = d; ((Buf *)b)->cap = c; }
static void *buf_base(const void *b) { return ((const Buf *)b)->data; }
static size_t buf_capacity(const void *b) { return ((const Buf *)b)->cap; }
typedef struct { const char *mime; int w, h; } Md;   /* only what Configure reads; the setters are not checked here */
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
static int events_signalled;
static void signal_event(void *ev) { (void)ev; events_signalled++; }
static char fake_vtable[0xa8];

typedef void (*ctor_t)(void *, int, int, int, int); typedef void (*obj_t)(void *);
typedef int (*md_t)(void *, void *); typedef int (*int_t)(void *); typedef int (*pp_t)(void *, int *, int *);
typedef int (*ib_t)(void *, int, void *); typedef int (*send_t)(void *, int, int, int, int64_t, unsigned);
typedef int (*acq_t)(void *, int *, int64_t *, unsigned *, int *); typedef int (*i_t)(void *, int);
typedef int (*ibm_t)(void *, int, void *, void *);
#define E(t, w) ((t)movie_host_entry(w))
enum { W = 64, H = 64, FRAMES = 4, ST_ERROR = 0x1000 };

/* FRAMES VP9 frames of a W x H picture, from libvpx's encoder. */
static int encode(uint8_t *out[FRAMES], size_t sizes[FRAMES]) {
    vpx_codec_enc_cfg_t cfg; vpx_codec_ctx_t enc;
    need(vpx_codec_enc_config_default(vpx_codec_vp9_cx(), &cfg, 0) == VPX_CODEC_OK, "encoder config");
    cfg.g_w = W; cfg.g_h = H; cfg.g_timebase.num = 1; cfg.g_timebase.den = 30; cfg.g_lag_in_frames = 0;
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
    return n;
}

/* Configure, Start, feed every frame; returns the index of the first output acquired. */
static int start_and_acquire(void *dec, uint8_t *frames[], size_t sizes[], int n) {
    Md fmt = {"video/x-vnd.on2.vp9", W, H};
    need(E(md_t, MOVIE_CONFIGURE)(dec, &fmt) == 0, "Configure");
    need(E(int_t, MOVIE_START)(dec) == 0, "Start");
    for (int f = 0; f < n; f++) {
        int idx = -1, rem = 0; need(E(pp_t, MOVIE_ACQUIRE_INPUT)(dec, &idx, &rem) == 0, "input %d", f);
        Buf ib = {0}; need(E(ib_t, MOVIE_GET_INPUT)(dec, idx, &ib) == 0 && ib.cap >= sizes[f], "input buffer");
        memcpy(ib.data, frames[f], sizes[f]);
        need(E(send_t, MOVIE_SEND_INPUT)(dec, idx, 0, (int)sizes[f], f * 33333, 0) == 0, "send frame %d", f);
    }
    int idx = -1, rem = 0; int64_t t = 0; unsigned fl = 0;
    need(E(acq_t, MOVIE_ACQUIRE_OUTPUT)(dec, &idx, &t, &fl, &rem) == 0 && idx >= 0 && !(fl & 4), "an output is ready");
    uint8_t frame[W * H * 3 / 2]; Buf ob = {0}; buf_set_data(&ob, frame, sizeof frame);
    need(E(ib_t, MOVIE_GET_OUTPUT)(dec, idx, &ob) == 0 && ob.size == (int)sizeof frame, "a held frame can be fetched");
    return idx;
}

static void after_release(void *dec, int idx, const char *what) {
    uint8_t frame[W * H * 3 / 2]; Buf ob = {0}; buf_set_data(&ob, frame, sizeof frame);
    need(E(ib_t, MOVIE_GET_OUTPUT)(dec, idx, &ob) == ST_ERROR, "GetOutputBuffer of a frame held before %s: ST_ERROR", what);
    need(ob.size == 0, "no bytes handed over after %s", what);
    char md[8]; buf_set_data(&ob, frame, sizeof frame);
    need(E(ibm_t, MOVIE_GET_OUTPUT_MD)(dec, idx, &ob, md) == ST_ERROR, "GetOutputBuffer with MediaData after %s", what);
    need(E(ib_t, MOVIE_GET_OUTPUT_BUFFER_FORMAT)(dec, idx, md) == ST_ERROR, "GetOutputBufferFormat after %s", what);
    need(E(i_t, MOVIE_RELEASE_OUTPUT)(dec, idx) == ST_ERROR, "ReleaseOutputBufferIndex after %s", what);
}

int main(void) {
    setenv("TOTK_MOVIE_PACE", "0", 1);   /* frames are handed over as soon as they are decoded */
    MovieGuest api = {signal_event, buf_set_range, buf_set_data, buf_base, buf_capacity, md_find_int32, md_find_string,
                      md_set_int32, md_set_rect, md_set_string, fake_vtable + 0x10, {0}};
    movie_host_set_guest(&api);
    uint8_t *frames[FRAMES]; size_t sizes[FRAMES];
    int n = encode(frames, sizes);
    need(n == FRAMES, "%d VP9 frames encoded", n);

    void *dec[2] = {0}; E(ctor_t, MOVIE_CTOR)(dec, 3, 4, 1, 16);
    need(dec[1] != NULL, "VP9 decoder on the host");
    void *events[4] = {&events[0], &events[1], &events[2], &events[3]};
    need(E(md_t, MOVIE_REGISTER_EVENTS)(dec, events) == 0, "RegisterClientEvents");

    int idx = start_and_acquire(dec, frames, sizes, n);
    need(E(int_t, MOVIE_STOP)(dec) == 0, "Stop");
    after_release(dec, idx, "Stop");

    idx = start_and_acquire(dec, frames, sizes, n);   /* the decoder works again after a new Configure and Start */
    need(E(int_t, MOVIE_RESET)(dec) == 0, "Reset");
    after_release(dec, idx, "Reset");

    E(obj_t, MOVIE_D2)(dec);
    need(dec[1] == NULL, "destroyed");
    for (int f = 0; f < n; f++) free(frames[f]);
    printf("{\"test\":\"movie_stop\",\"checks\":%d,\"ok\":true}\n", checks);
    return 0;
}
