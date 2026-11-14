/* The host VP9 movie::Decoder (runtime/movie_host.c) when the game stops or resets a movie while it still holds a decoded
   frame (Echoes of Wisdom's tests/movie_stop, carried over with its fix). A frame acquired (AcquireOutputBufferIndex) on
   one thread can be fetched on another after Stop freed every frame. Before the fix the held index stayed valid: buffer
   mode converted from a null frame, texture mode (Metroid Dread's mode 2) read the freed frame in nv12_to_rgba. Here,
   deterministically: decode frames (VP9 from libvpx's own encoder, so no game file), acquire one, Stop, then every call
   for that index must answer ST_ERROR (0x1000) without touching freed memory (built with AddressSanitizer), and the
   decoder must work again after a new Configure and Start; the same after Reset; then the same in texture mode, where
   GetOutputBuffer copies into the game's NVN texture. Stand-ins replace the game's movie::Buffer, movie::MediaData,
   nn::os events and the graphics layer's NVN entry points. */
#include "../../runtime/movie_host.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vpx/vp8cx.h>
#include <vpx/vpx_encoder.h>

_Noreturn void runtime_fail(const char *s) { fprintf(stderr, "movie stop test: runtime_fail: %s\n", s); exit(3); }
void *runtime_symbol(const char *name) { (void)name; return NULL; }
/* The graphics layer's NVN entry points texture mode calls (nvn_slot_fn), over a stand-in texture whose first word is the
   NVN texture magic, as the game's initialized NVNtexture. */
enum { TEX_MAGIC = 'T' | 'E' << 8 | 'X' << 16, NVN_RGBA8 = 0x25 };
static int texels_written, syncs_fenced;
static int tex_format(const void *t) { (void)t; return NVN_RGBA8; }
static int tex_width(const void *t) { (void)t; return 64; }
static int tex_height(const void *t) { (void)t; return 64; }
static void tex_write(const void *t, const void *v, const void *r, const void *src) { (void)t; (void)v; (void)r; (void)src; texels_written++; }
static void fence(void *q, void *sync, int cond, int flags) { (void)q; (void)sync; (void)cond; (void)flags; syncs_fenced++; }
void *nvn_slot_fn(const char *name) {
    if (!strcmp(name, "nvnTextureGetFormat")) return (void *)tex_format;
    if (!strcmp(name, "nvnTextureGetWidth")) return (void *)tex_width;
    if (!strcmp(name, "nvnTextureGetHeight")) return (void *)tex_height;
    if (!strcmp(name, "nvnTextureWriteTexels")) return (void *)tex_write;
    if (!strcmp(name, "nvnQueueFenceSync")) return (void *)fence;
    return NULL;
}

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
static void *buf_nvn_sync(const void *b) { (void)b; static char sync[64]; return sync; }
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

/* Texture mode: Configure, Start, feed every frame, then acquire `want` outputs (their indices in idx[]). */
static void start_texture(void *dec, uint8_t *frames[], size_t sizes[], int n, int idx[], int want) {
    Md fmt = {"video/x-vnd.on2.vp9", W, H};
    need(E(md_t, MOVIE_CONFIGURE)(dec, &fmt) == 0, "texture mode Configure");
    need(E(int_t, MOVIE_START)(dec) == 0, "texture mode Start");
    for (int f = 0; f < n; f++) {
        int i = -1, rem = 0; need(E(pp_t, MOVIE_ACQUIRE_INPUT)(dec, &i, &rem) == 0, "texture mode input %d", f);
        Buf ib = {0}; need(E(ib_t, MOVIE_GET_INPUT)(dec, i, &ib) == 0 && ib.cap >= sizes[f], "texture mode input buffer");
        memcpy(ib.data, frames[f], sizes[f]);
        need(E(send_t, MOVIE_SEND_INPUT)(dec, i, 0, (int)sizes[f], f * 33333, 0) == 0, "texture mode frame %d", f);
    }
    for (int k = 0; k < want; k++) {
        int rem = 0; int64_t t = 0; unsigned fl = 0; idx[k] = -1;
        need(E(acq_t, MOVIE_ACQUIRE_OUTPUT)(dec, &idx[k], &t, &fl, &rem) == 0 && idx[k] >= 0 && !(fl & 4),
             "texture mode output %d is ready", k);
    }
}

int main(void) {
    setenv("TOTK_MOVIE_PACE", "0", 1);   /* frames are handed over as soon as they are decoded */
    MovieGuest api = {signal_event, buf_set_range, buf_set_data, buf_base, buf_capacity, buf_nvn_sync, md_find_int32, md_find_string,
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

    /* Texture mode (type VP9, output format 3, mode 2): a held frame is copied into the game's texture, which releases
       the index; a frame held across Stop must answer ST_ERROR and never reach the copy. */
    void *tdec[2] = {0}; E(ctor_t, MOVIE_CTOR)(tdec, 3, 3, 2, 16);
    need(tdec[1] != NULL, "texture-mode VP9 decoder on the host");
    need(E(md_t, MOVIE_REGISTER_EVENTS)(tdec, events) == 0, "texture mode RegisterClientEvents");
    uint32_t texture[16] = {TEX_MAGIC}; Buf tb = {0}; buf_set_data(&tb, texture, 8);
    int held[2];
    start_texture(tdec, frames, sizes, n, held, 2);
    need(E(ib_t, MOVIE_GET_OUTPUT)(tdec, held[0], &tb) == 0 && texels_written == 1 && syncs_fenced == 1,
         "a held frame is copied into the texture and its sync fenced");
    need(E(i_t, MOVIE_RELEASE_OUTPUT)(tdec, held[0]) == ST_ERROR, "the copy released the index");
    need(E(int_t, MOVIE_STOP)(tdec) == 0, "texture mode Stop");
    need(E(ib_t, MOVIE_GET_OUTPUT)(tdec, held[1], &tb) == ST_ERROR && texels_written == 1,
         "a frame held before Stop: ST_ERROR, nothing copied");
    need(E(i_t, MOVIE_RELEASE_OUTPUT)(tdec, held[1]) == ST_ERROR, "texture mode ReleaseOutputBufferIndex after Stop");
    start_texture(tdec, frames, sizes, n, held, 1);   /* works again after a new Configure and Start */
    need(E(int_t, MOVIE_RESET)(tdec) == 0, "texture mode Reset");
    need(E(ib_t, MOVIE_GET_OUTPUT)(tdec, held[0], &tb) == ST_ERROR && texels_written == 1,
         "a frame held before Reset: ST_ERROR, nothing copied");
    E(obj_t, MOVIE_D2)(tdec);
    need(tdec[1] == NULL, "texture-mode decoder destroyed");
    for (int f = 0; f < n; f++) free(frames[f]);
    printf("{\"test\":\"movie_stop\",\"checks\":%d,\"ok\":true}\n", checks);
    return 0;
}
