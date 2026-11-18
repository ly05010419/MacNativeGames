// Native-texture movie slots (GitHub issue #700 upstream), on the real decoder: hle_movie.c is included as it is; only what lies outside it is stubbed here (the guest's
// movie::Buffer and event functions, libvpx, the NVN texture write). Each fake picture carries its packet number in its pixels, so the test checks what was copied into the
// texture against the time the client was told, not a model of the slots. No game files. Built and run by games/ss/tests/test_movie_slots.py.
#include "../hle.h"
#undef HLE_FORCE
#define HLE_FORCE(mangled, why, ret, cname, ...) static ret cname(__VA_ARGS__)
#include "../hle_movie.c"
#include <assert.h>

uint64_t now_ns(void) { static _Atomic uint64_t t; return t += 1000000; }
void gate_obs_set(int id, void (*fn)(uint64_t *regs, int param)) { (void)id; (void)fn; }
void dm_host_helper_qos(qos_class_t q) { (void)q; }
void *dm_guest_thread_spawn(void (*fn)(void *), void *arg, int prio, size_t stack_size) { (void)fn; (void)arg; (void)prio; (void)stack_size; return NULL; }
// H.264 is not exercised: hle_movie_avc.c stays out
mavc *mavc_new(void) { return NULL; }
void mavc_set_rgba(mavc *a) { (void)a; }
int mavc_decode(mavc *a, const uint8_t *p, int n, int64_t pts) { (void)a; (void)p; (void)n; (void)pts; return 0; }
int mavc_take(mavc *a, int flush, uint8_t *dst, int w, int h, int64_t *pts) { (void)a; (void)flush; (void)dst; (void)w; (void)h; (void)pts; return 0; }
int mavc_queued(const mavc *a) { (void)a; return 0; }
void mavc_drop(mavc *a) { (void)a; }
void mavc_free(mavc *a) { (void)a; }
void mavc_param_sets(mavc *a, const uint8_t *p, size_t n) { (void)a; (void)p; (void)n; }
void mavc_avcc(mavc *a, const uint8_t *p, size_t n) { (void)a; (void)p; (void)n; }
// libvpx: a packet's first byte is its picture's number, which becomes the luma (full range, neutral chroma: md_rgba gives r = g = b = that number)
enum { W = 16, H = 16 };
static uint8_t luma[W * H], chroma[W * H / 4]; static vpx_image_t img; static int img_ready;
vpx_codec_err_t vpx_codec_dec_init_ver(vpx_codec_ctx_t *c, vpx_codec_iface_t *i, const vpx_codec_dec_cfg_t *cfg, vpx_codec_flags_t f, int v) { (void)c; (void)i; (void)cfg; (void)f; (void)v; return VPX_CODEC_OK; }
vpx_codec_err_t vpx_codec_control_(vpx_codec_ctx_t *c, int id, ...) { (void)c; (void)id; return VPX_CODEC_OK; }
vpx_codec_err_t vpx_codec_decode(vpx_codec_ctx_t *c, const uint8_t *p, unsigned n, void *u, long d) {
    (void)c; (void)u; (void)d; memset(luma, n ? p[0] : 0, sizeof luma); memset(chroma, 128, sizeof chroma);
    img = (vpx_image_t){ .fmt = VPX_IMG_FMT_I420, .d_w = W, .d_h = H, .range = VPX_CR_FULL_RANGE, .cs = VPX_CS_BT_709,
                         .planes = { luma, chroma, chroma }, .stride = { W, W / 2, W / 2 } };
    img_ready = 1; return VPX_CODEC_OK;
}
vpx_image_t *vpx_codec_get_frame(vpx_codec_ctx_t *c, vpx_codec_iter_t *it) { (void)c; if (*it || !img_ready) return NULL; *it = &img; img_ready = 0; return &img; }
vpx_codec_err_t vpx_codec_destroy(vpx_codec_ctx_t *c) { (void)c; return VPX_CODEC_OK; }
const char *vpx_codec_err_to_string(vpx_codec_err_t e) { (void)e; return "stub"; }
const char *vpx_codec_error(const vpx_codec_ctx_t *c) { (void)c; return "stub"; }
const char *vpx_codec_error_detail(const vpx_codec_ctx_t *c) { (void)c; return NULL; }
const char *vpx_codec_version_str(void) { return "stub"; }
vpx_codec_iface_t *vpx_codec_vp9_dx(void) { return NULL; }
vpx_codec_iface_t *vpx_codec_vp8_dx(void) { return NULL; }   // nxnative b1dee89's VP8 path
__thread _Atomic int dm_host_work;   // dm.h: host work outside a guest core (b1dee89)
void dm_host_lock(pthread_mutex_t *m) { pthread_mutex_lock(m); }   // hle_thread.c (b1dee89)
// the NVN texture: what GetOutputBuffer copies into it
static int shown = -1;
int nvn_host_write_texture(const void *t, const void *src, int w, int h, size_t row, int *tw, int *th) {
    (void)t; (void)w; (void)h; (void)row; if (tw) *tw = W; if (th) *th = H; if (src) shown = ((const uint8_t *)src)[0]; return 0x38; }
int nvn_host_signal_sync(void *s) { (void)s; return 0; }
// the guest's movie::Buffer: data() is the texture object; events are counted, not signalled
static uint64_t texobj[8];
static void b_set_data(void *b, void *d, size_t c) { (void)b; (void)d; (void)c; }
static void b_set_range(void *b, int o, int s) { (void)b; (void)o; (void)s; }
static void *b_data(const void *b) { (void)b; return texobj; }
static size_t b_cap(const void *b) { (void)b; return sizeof texobj; }
static void signal_ev(void *e) { (void)e; }
static uint8_t vtable[0x40];
void *guest_sym(const char *n) {
    if (!strcmp(n, "_ZTVN5movie7DecoderE")) return vtable;
    if (!strcmp(n, "_ZN5movie6Buffer18SetDataAndCapacityEPvm")) return (void *)b_set_data;
    if (!strcmp(n, "_ZN5movie6Buffer8SetRangeEii")) return (void *)b_set_range;
    if (!strcmp(n, "_ZNK5movie6Buffer4DataEv")) return (void *)b_data;
    if (!strcmp(n, "_ZNK5movie6Buffer8CapacityEv")) return (void *)b_cap;
    if (!strcmp(n, "_ZN2nn2os11SignalEventEPNS0_9EventTypeE")) return (void *)signal_ev;
    if (!strncmp(n, "_ZN5movie9MediaData", 19) || !strncmp(n, "_ZNK5movie9MediaData", 20) || !strcmp(n, "_ZNK5movie6Buffer4SizeEv")) return (void *)signal_ev;
    return NULL;   // no GetNvnSync
}

static void *self[2]; static int next_pkt = 1, expect = 1;   // expect: the next picture the player must get, every picture once and in order
static void open_decoder(void) {
    h_movie_Decoder(self, MD_TYPE_VP9, MD_FMT_ABGR, MD_MODE_TEX, 12);
    mdec_t *d = md_of(self); d->w = W; d->h = H; assert(!h_movie_Start(self));
}
static int send_one(void) {   // a free input index must exist: the decoder hands them back as it decodes
    int i, left; if (h_movie_AcqIn(self, &i, &left)) return 0;
    uint8_t *p = md_of(self)->in[i]; p[0] = (uint8_t)next_pkt;
    assert(!h_movie_SendIn(self, i, 0, 1, (int64_t)next_pkt * 1000, 0)); next_pkt++; return 1;
}
static int acquire(int64_t *pts) { int i, left; uint32_t f; return h_movie_AcqOut(self, &i, pts, &f, &left) ? -1 : i; }
// one picture as the player does it: acquire, copy into the texture, then (optionally) release. The texture must hold the announced picture, and the pictures come
// once each and in order: a picture freed by a late release is lost, and its slot announced twice gives a picture ahead and then one behind
static int play_one(int release) {
    int64_t pts; int i = acquire(&pts); if (i < 0) return 0;
    shown = -1; assert(!h_movie_GetOut(self, i, texobj));
    if (shown != pts / 1000 || shown != expect) { fprintf(stderr, "picture %d expected, announced %lld, texture got %d (slot %d)\n", expect, (long long)pts / 1000, shown, i); assert(0); }
    expect++;
    if (release) h_movie_RelOut(self, i);
    return 1;
}

// The issue: acquire → copy (the host returns the slot) → the slot is reused for a new picture → the client's own release of the old index comes. It must not free the new picture.
static void reused_slot(void) {
    open_decoder(); next_pkt = expect = 1;
    while (send_one()) {}   // the decoder ahead of the player, as in the game: every slot decoded, packets queued behind them
    for (int n = 0; n < 120; n++) { play_one(1); while (send_one()) {} }
    puts("reused slot: 120 pictures, each texture holds the picture announced for it");
}
// A client that releases explicitly: a second release changes nothing; a picture dropped without GetOutputBuffer is freed by its release
static void explicit_release(void) {
    open_decoder(); next_pkt = expect = 1;
    while (send_one()) {}   // the decoder ahead of the player, as in the game: every slot decoded, packets queued behind them
    int64_t pts; int i = acquire(&pts); assert(i >= 0 && pts / 1000 == expect); expect++;
    assert(!h_movie_RelOut(self, i) && !h_movie_RelOut(self, i));   // dropped, released twice
    for (int n = 0; n < 40; n++) { play_one(1); while (send_one()) {} }
    puts("explicit release: a dropped picture and a double release leave the stream intact");
}
// A client that never releases (Super Mario Party Jamboree's player upstream) must not stop the decoder once every slot was used once
static void never_released(void) {
    open_decoder(); next_pkt = expect = 1;
    while (send_one()) {}   // the decoder ahead of the player, as in the game: every slot decoded, packets queued behind them
    int n = 0; for (; n < 3 * MD_NOUT_VIDEO + 8; n++) { assert(play_one(0)); while (send_one()) {} }
    printf("never released: %d pictures, more than three slot cycles\n", n);
}
// Flush and Stop give every slot back and the decoder goes on
static void flush_and_stop(void) {
    open_decoder(); next_pkt = expect = 1;
    while (send_one()) {}   // the decoder ahead of the player, as in the game: every slot decoded, packets queued behind them
    int64_t pts; int i = acquire(&pts); assert(i >= 0 && !h_movie_GetOut(self, i, texobj));   // held, not released
    assert(!h_movie_Flush(self)); expect = next_pkt;   // the queued pictures are dropped: a seek
    mdec_t *d = md_of(self); for (int k = 0; k < d->nout; k++) assert(!d->out[k].used);
    assert(!h_movie_Start(self));
    while (send_one()) {}   // the decoder ahead of the player, as in the game: every slot decoded, packets queued behind them
    for (int n = 0; n < 20; n++) { play_one(1); while (send_one()) {} }
    i = acquire(&pts); assert(i >= 0 && !h_movie_GetOut(self, i, texobj));
    assert(!h_movie_Stop(self)); expect = next_pkt; for (int k = 0; k < d->nout; k++) assert(!d->out[k].used);
    assert(!h_movie_Start(self)); for (int k = 0; k < 4; k++) assert(send_one());
    for (int n = 0; n < 20; n++) { play_one(1); while (send_one()) {} }
    puts("flush and stop: every slot free afterwards, decoding goes on");
}

int main(int argc, char **argv) {
    const char *only = argc > 1 ? argv[1] : NULL;
    if (!only || !strcmp(only, "reused")) reused_slot();
    if (!only || !strcmp(only, "explicit")) explicit_release();
    if (!only || !strcmp(only, "never")) never_released();
    if (!only || !strcmp(only, "flush")) flush_and_stop();
    return 0;
}
