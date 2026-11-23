// The working-ahead AV1 path (hle_movie_av1.c: mav1_send / mav1_receive / mav1_drain) against one frame at a time
// (mav1_decode), on a real stream: every picture comes out, in order, with its own timestamp, and with the same pixels.
// usage: av1_pipeline <frames.ivf> (an AV1 IVF, e.g. ffmpeg -i movie.webm -map 0:v -c copy -frames:v 600 -f ivf out.ivf)
// Runs on the dav1d path (DM_AV1_DAV1D=1 is set here, so it runs on any Mac); DM_AV1_FRAME_DELAY picks the depth.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include "../runtime/hle_movie_av1.h"

void runtime_fail(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); fputc('\n', stderr); exit(2); }

typedef struct { uint8_t *data; size_t n; } unit;
static uint64_t pic_hash(const mav1_picture *p) {   // FNV-1a over the visible luma and interleaved chroma
    uint64_t h = 1469598103934665603ull;
    for (int y = 0; y < p->h; y++) for (int x = 0; x < p->w; x++) { h ^= p->y[(size_t)y * p->ystride + x]; h *= 1099511628211ull; }
    for (int y = 0; y < (p->h + 1) / 2; y++) for (int x = 0; x < ((p->w + 1) / 2) * 2; x++) { h ^= p->uv[(size_t)y * p->uvstride + x]; h *= 1099511628211ull; }
    return h;
}
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: av1_pipeline <frames.ivf>\n"); return 2; }
    setenv("DM_AV1_DAV1D", "1", 1);
    FILE *f = fopen(argv[1], "rb"); if (!f) { perror(argv[1]); return 2; }
    uint8_t hdr[32]; if (fread(hdr, 1, 32, f) != 32 || memcmp(hdr, "DKIF", 4)) { fprintf(stderr, "not an IVF file\n"); return 2; }
    unit *u = NULL; size_t nu = 0, cap = 0; uint8_t fh[12];
    while (fread(fh, 1, 12, f) == 12) {
        size_t n = fh[0] | fh[1] << 8 | fh[2] << 16 | (size_t)fh[3] << 24;
        if (nu == cap) { cap = cap ? cap * 2 : 256; u = realloc(u, cap * sizeof *u); }
        u[nu].data = malloc(n); u[nu].n = n;
        if (fread(u[nu].data, 1, n, f) != n) break;
        nu++;
    }
    fclose(f);
    const int64_t step = 16666667;   // 60 fps, in ns

    // One frame at a time: the reference.
    setenv("DM_AV1_FRAME_DELAY", "1", 1);
    mav1 *a = mav1_new(); if (mav1_pipelined(a)) { fprintf(stderr, "FAIL: depth 1 is working ahead\n"); return 1; }
    uint64_t *ref = calloc(nu, sizeof *ref); size_t nref = 0; double t0 = now();
    for (size_t i = 0; i < nu; i++) { mav1_picture p; if (mav1_decode(a, u[i].data, u[i].n, &p) == 1) ref[nref++] = pic_hash(&p); }
    double t_ref = now() - t0; mav1_free(a);

    // Working ahead.
    const char *depth = getenv("AV1_TEST_DEPTH"); setenv("DM_AV1_FRAME_DELAY", depth ? depth : "3", 1);
    a = mav1_new(); if (!mav1_pipelined(a)) { fprintf(stderr, "FAIL: not working ahead\n"); return 1; }
    size_t got = 0, bad_ts = 0, bad_px = 0, max_burst = 0; int64_t last = -1; t0 = now();
    for (size_t i = 0; i <= nu; i++) {
        if (i < nu) { if (mav1_send(a, u[i].data, u[i].n, (int64_t)i * step)) { fprintf(stderr, "FAIL: send %zu refused\n", i); return 1; } }
        else mav1_drain(a);
        mav1_picture p; int64_t ts; size_t burst = 0;
        while (mav1_receive(a, &p, &ts) == 1) {
            burst++;
            if (ts <= last || ts % step || (size_t)(ts / step) >= nu) bad_ts++;
            last = ts;
            if (got >= nref || pic_hash(&p) != ref[got]) bad_px++;
            got++;
        }
        if (burst > max_burst) max_burst = burst;
    }
    double t_pipe = now() - t0;
    int ok_drained = mav1_drained(a);
    mav1_reset(a);   // a reset with nothing pending, then a second stream through the same decoder
    size_t got2 = 0; for (size_t i = 0; i <= 30 && i <= nu; i++) {
        if (i < 30 && i < nu) mav1_send(a, u[i].data, u[i].n, (int64_t)i * step); else mav1_drain(a);
        mav1_picture p; int64_t ts; while (mav1_receive(a, &p, &ts) == 1) got2++;
    }
    mav1_free(a);
    size_t want2 = nu < 30 ? nu : 30;
    printf("units %zu; one at a time: %zu pictures, %.0f fps; working ahead (depth %s): %zu pictures, %.0f fps; most from one call %zu\n",
           nu, nref, nref / t_ref, depth ? depth : "3", got, got / t_pipe, max_burst);
    printf("timestamps out of order or not their own: %zu; pictures differing from one at a time: %zu; drained: %d; after a reset: %zu of %zu\n",
           bad_ts, bad_px, ok_drained, got2, want2);
    int fail = got != nref || bad_ts || bad_px || !ok_drained || got2 != want2 || max_burst > (size_t)mav1_pipeline_depth() + 1;
    puts(fail ? "FAIL" : "PASS");
    return fail;
}
