/* Exercise the real nn::codec output queues without game files or a GPU. */
#define clock_gettime_nsec_np test_clock_ns
#include "../runtime/ns2_codec.c"
#include <assert.h>

static uint64_t virtual_ns;
uint64_t test_clock_ns(clockid_t which) { (void)which; return virtual_ns; }
_Noreturn void runtime_fail(const char *why) { fprintf(stderr, "%s\n", why); abort(); }
static void event_noop(void *event) { (void)event; }
void *runtime_symbol(const char *name) { (void)name; return (void *)event_noop; }
void hle_signal_system_event(void *event) { (void)event; }
void hle_bind_system_event(void *event, int autoclear) { (void)event; (void)autoclear; }
/* AV1 packet decoding is independently exercised by the actual game. These
   stubs are never reached: the queue receives synthetic owned I420 pictures. */
mav1 *mav1_new(void) { return (mav1 *)calloc(1, 1); }
void mav1_free(mav1 *a) { free(a); }
void mav1_reset(mav1 *a) { (void)a; }
const char *mav1_backend(const mav1 *a) { (void)a; return "test"; }
int mav1_decode(mav1 *a, const uint8_t *bytes, size_t n, mav1_picture *out) {
    (void)a; (void)bytes; (void)n; (void)out; abort();
}

static unsigned checks;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } checks++; } while (0)

static void queues_valid(const cv_dec *d) {
    unsigned owned[CV_BUFS] = {0}, pending[CV_BUFS] = {0};
    CHECK(d->fcount <= 8 && d->pcount <= CV_BUFS && d->nheld <= CV_HOLD);
    for (unsigned k = 0; k < d->fcount; k++) CHECK(!owned[d->fifo[(d->fhead + k) % 8]]++);
    for (unsigned k = 0; k < d->pcount; k++) {
        unsigned i = d->pend[(d->phead + k) % CV_BUFS];
        CHECK(!pending[i]++ && !d->buf[i].filled && owned[i]);
    }
}

static void fixture(cv_dec *d, cv_pool *pool, unsigned count) {
    *d = (cv_dec){.magic = CV_MAGIC, .id = 1, .codec = CV_AV1};
    pthread_mutex_init(&d->mu, NULL);
    *pool = (cv_pool){.magic = CV_MAGIC, .count = count, .w = 2, .h = 2};
    pool->img = calloc(count, sizeof *pool->img);
    for (unsigned i = 0; i < count; i++) {
        d->buf[i] = (cv_buf){.pool = pool, .pidx = i, .reg = 1, .gen = 1};
        d->fifo[d->fcount++] = i;
        d->pend[d->pcount++] = i;
    }
}

static void cleanup(cv_dec *d, cv_pool *pool) {
    for (unsigned i = 0; i < pool->count; i++) if (pool->img[i]) vpx_img_free(pool->img[i]);
    for (int i = 0; i < d->nheld; i++) vpx_img_free(d->held[i]);
    if (d->last_skipped) vpx_img_free(d->last_skipped);
    free(pool->img);
    if (d->av1) mav1_free(d->av1);
    pthread_mutex_destroy(&d->mu);
}

static int64_t consume(cv_dec *d, int *eos) {
    cv_dec *handle = d;
    uint32_t i; cv_sync sync;
    CHECK(cv_dec_acq(&i, &sync, &handle) == 0);
    uint16_t w, h, cw, ch; int64_t ts; bool end;
    CHECK(cv_dec_props(&w, &h, &cw, &ch, &ts, &end, &handle, i) == 0);
    CHECK(sync.gen == d->buf[i].gen && sync.idx == i);
    *eos = end;
    cv_dec_rel(&handle, i, sync);
    queues_valid(d);
    return ts;
}

static void simulate(int fps, int hitch, int enabled, unsigned buffers) {
    cv_dec d; cv_pool pool; fixture(&d, &pool, buffers);
    const int total = 180;
    int produced = 0, ended = 0, saw_eos = 0, displayed = 0;
    int64_t last_pts = -1, max_lag = 0;
    uint64_t next_render = 0;
    for (virtual_ns = 0; virtual_ns <= 8000000000ull; virtual_ns += 1000000) {
        while (produced < total && (uint64_t)produced * 1000000000ull / 60 <= virtual_ns && d.nheld < CV_HOLD) {
            vpx_image_t *im = vpx_img_alloc(NULL, VPX_IMG_FMT_I420, 2, 2, 1);
            CHECK(im != NULL);
            cv_hold(&d, im, (int64_t)produced++ * 1000000000ll / 60);
            drain(&d);
        }
        if (produced == total && !ended) { d.eos_pending = 1; drain(&d); ended = 1; }
        if (virtual_ns < next_render || (hitch && virtual_ns >= 1000000000ull && virtual_ns < 1750000000ull)) continue;
        next_render = virtual_ns + 1000000000ull / (unsigned)fps;
        if (!d.fcount || !d.buf[d.fifo[d.fhead]].filled) continue;
        int eos = 0; int64_t pts = consume(&d, &eos);
        if (eos) { saw_eos = 1; break; }
        CHECK(pts > last_pts); last_pts = pts; displayed++;
        int64_t lag = (int64_t)virtual_ns - pts;
        if (lag > max_lag) max_lag = lag;
        // The first render after a forced hitch may need one producer refill.
        if (enabled && (!hitch || virtual_ns < 1000000000ull || virtual_ns > 1850000000ull))
            CHECK(lag >= -1000000 && lag <= 70000000);
    }
    CHECK(saw_eos && produced == total && displayed > 0);
    CHECK(last_pts == (int64_t)(total - 1) * 1000000000ll / 60);
    if (enabled && fps < 60) CHECK(d.dropped > 0);
    if (!enabled && fps == 30) CHECK(max_lag > 1000000000 && !d.dropped);
    printf("buffers=%u fps=%d hitch=%d sync=%d displayed=%d dropped=%llu max_lag_ms=%.3f\n",
           buffers, fps, hitch, enabled, displayed, (unsigned long long)d.dropped, max_lag / 1e6);
    cleanup(&d, &pool);
}

static void ownership_and_reset(void) {
    cv_dec d; cv_pool pool; fixture(&d, &pool, 8);
    cv_dec *handle = &d;
    for (int i = 0; i < 8; i++) {
        cv_hold(&d, vpx_img_alloc(NULL, VPX_IMG_FMT_I420, 2, 2, 1), i * 16666667ll);
        drain(&d);
    }
    virtual_ns = 1000000000ull;
    uint32_t held; cv_sync sync; CHECK(!cv_dec_acq(&held, &sync, &handle));
    ns2_movie_clock_start(&d.clock, virtual_ns, 0);
    virtual_ns += 1000000000;
    uint32_t other; cv_sync other_sync; CHECK(!cv_dec_acq(&other, &other_sync, &handle));
    CHECK(held != other && d.buf[held].filled && d.buf[held].gen == sync.gen);
    CHECK(d.dropped == 6);
    cv_dec_rel(&handle, held, sync); cv_dec_rel(&handle, other, other_sync);
    queues_valid(&d);
    CHECK(!cv_dec_reset(&handle)); CHECK(!d.clock.started && !d.dropped);
    cleanup(&d, &pool);
    Ns2MovieClock c = {0}; ns2_movie_clock_start(&c, 100, -100);
    CHECK(ns2_movie_clock_target(&c, 90) == -100);
    CHECK(ns2_movie_clock_target(&c, 200) == 0);
    c = (Ns2MovieClock){0, INT64_MAX - 2, 1}; CHECK(ns2_movie_clock_target(&c, 10) == INT64_MAX);
    c = (Ns2MovieClock){0, 0, 1}; CHECK(ns2_movie_clock_target(&c, UINT64_MAX) == INT64_MAX);
}

int main(int argc, char **argv) {
    int enabled = argc < 2 || strcmp(argv[1], "off");
    setenv("OOT_MOVIE_SYNC", enabled ? "1" : "0", 1);
    for (unsigned buffers = 1; buffers <= 8; buffers += 7) {
        simulate(30, 0, enabled, buffers);
        if (enabled) {
            simulate(24, 0, 1, buffers); simulate(60, 0, 1, buffers);
            simulate(120, 0, 1, buffers); simulate(30, 1, 1, buffers);
        }
    }
    if (enabled) ownership_and_reset();
    printf("{\"status\":\"pass\",\"checks\":%u,\"sync\":%s}\n", checks, enabled ? "true" : "false");
    return 0;
}
