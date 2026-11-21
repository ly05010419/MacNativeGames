/**
 * test_offscreen.c - self-test for mtl_backend
 * All expected values are computed independently in this file, without reading backend internals:
 *   - the triangle vertices (NDC) are hand-copied into TRI below (same values as kTri in mtl_backend.m);
 *   - NDC→pixel: px = (x+1)/2*W, py = (1-y)/2*H (Metal's viewport origin is top-left, readback row 0 is the top);
 *   - the color at pixel center (i+0.5, j+0.5) = linear interpolation of the three vertex colors by barycentric coordinates (w=1, no perspective).
 * Each test is run once for each of the two formats BGRA8 and RGBA8.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mtl_backend.h"

#define W 256
#define H 256
#define TOL 2 /* Tolerance for unorm8 quantization and raster interpolation precision (per channel) */

static const float TRI[3][2] = {{0.0f, 0.5f}, {-0.5f, -0.5f}, {0.5f, -0.5f}};
static const float TRI_COL[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};

static int fails = 0;

typedef struct { int r, g, b, a; } px_t;

static px_t get_px(const uint8_t *buf, mtlb_fmt fmt, int x, int y) {
    const uint8_t *p = buf + ((size_t)y * W + x) * 4;
    px_t o;
    if (fmt == MTLB_FMT_BGRA8) { o.b = p[0]; o.g = p[1]; o.r = p[2]; o.a = p[3]; }
    else                       { o.r = p[0]; o.g = p[1]; o.b = p[2]; o.a = p[3]; }
    return o;
}

static int near(px_t a, px_t b, int tol) {
    return abs(a.r - b.r) <= tol && abs(a.g - b.g) <= tol && abs(a.b - b.b) <= tol && abs(a.a - b.a) <= tol;
}

static void report(const char *name, px_t want, px_t got, int ok) {
    printf("  %-34s expected (%3d,%3d,%3d,%3d) → got (%3d,%3d,%3d,%3d)  %s\n", name,
           want.r, want.g, want.b, want.a, got.r, got.g, got.b, got.a, ok ? "pass" : "[MISMATCH]");
    if (!ok) fails++;
}

/* Vertex pixel coordinates */
static void vert_px(int i, double *x, double *y) {
    *x = (TRI[i][0] + 1.0) * 0.5 * W;
    *y = (1.0 - TRI[i][1]) * 0.5 * H;
}

/* Signed distances from point (x,y) to the three edges (in pixels, positive inside) and barycentric coordinates */
static void bary(double x, double y, double l[3], double dist[3]) {
    double vx[3], vy[3];
    for (int i = 0; i < 3; i++) vert_px(i, &vx[i], &vy[i]);
    double area = (vx[1] - vx[0]) * (vy[2] - vy[0]) - (vy[1] - vy[0]) * (vx[2] - vx[0]);
    for (int i = 0; i < 3; i++) {
        int a = (i + 1) % 3, b = (i + 2) % 3; /* The edge opposite vertex i */
        double e = (vx[b] - vx[a]) * (y - vy[a]) - (vy[b] - vy[a]) * (x - vx[a]);
        l[i] = e / area;
        dist[i] = e / (area > 0 ? 1 : -1) / hypot(vx[b] - vx[a], vy[b] - vy[a]);
    }
}

static px_t tri_expect(int i, int j) {
    double l[3], d[3];
    bary(i + 0.5, j + 0.5, l, d);
    double c[3] = {0, 0, 0};
    for (int v = 0; v < 3; v++)
        for (int k = 0; k < 3; k++) c[k] += l[v] * TRI_COL[v][k];
    px_t o = {(int)lround(c[0] * 255), (int)lround(c[1] * 255), (int)lround(c[2] * 255), 255};
    return o;
}

static const char *fmt_name(mtlb_fmt f) { return f == MTLB_FMT_BGRA8 ? "BGRA8" : "RGBA8"; }

static void test_clear(mtlb_ctx *ctx, mtlb_fmt fmt, uint8_t *buf) {
    printf("[test 1: clear to pure red %s %dx%d]\n", fmt_name(fmt), W, H);
    mtlb_rt *rt = mtlb_target(ctx, W, H, fmt);
    memset(buf, 0x5a, W * H * 4); /* Fill with garbage first, to prevent "passes even though nothing was written" */
    if (!rt || mtlb_clear(ctx, rt, 1, 0, 0, 1) || mtlb_readback(ctx, rt, buf, W * H * 4)) {
        printf("  backend call failed\n"); fails++; mtlb_target_free(rt); return;
    }
    px_t want = {255, 0, 0, 255};
    int bad = 0; px_t first_bad = {0};
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            px_t g = get_px(buf, fmt, x, y);
            if (!near(g, want, 0)) { if (!bad) first_bad = g; bad++; }
        }
    printf("  all %d pixels compared one by one: expected all (255,0,0,255) → got %d mismatched%s\n", W * H, bad,
           bad ? " [MISMATCH]" : "  pass");
    if (bad) { printf("  first mismatched pixel (%d,%d,%d,%d)\n", first_bad.r, first_bad.g, first_bad.b, first_bad.a); fails++; }
    mtlb_target_free(rt);
}

static void test_triangle(mtlb_ctx *ctx, mtlb_fmt fmt, uint8_t *buf) {
    printf("[test 2: three-color triangle on black %s %dx%d, clear + triangle as two passes in one frame]\n", fmt_name(fmt), W, H);
    mtlb_rt *rt = mtlb_target(ctx, W, H, fmt);
    memset(buf, 0x5a, W * H * 4);
    if (!rt || mtlb_clear(ctx, rt, 0, 0, 0, 1) || mtlb_triangle(ctx, rt) ||
        mtlb_readback(ctx, rt, buf, W * H * 4)) {
        printf("  backend call failed\n"); fails++; mtlb_target_free(rt); return;
    }
    px_t clear = {0, 0, 0, 255};

    /* 1) The four corner points = clear color */
    const int corners[4][2] = {{0, 0}, {W - 1, 0}, {0, H - 1}, {W - 1, H - 1}};
    for (int k = 0; k < 4; k++) {
        char name[64]; snprintf(name, sizeof name, "corner (%d,%d) clear color", corners[k][0], corners[k][1]);
        px_t g = get_px(buf, fmt, corners[k][0], corners[k][1]);
        report(name, clear, g, near(g, clear, 0));
    }

    /* 2) Centroid: NDC (0,-1/6) → pixel (128, 149.33); take pixel (128,149), which is not the clear color and ≈ the interpolated value */
    {
        px_t want = tri_expect(128, 149), g = get_px(buf, fmt, 128, 149);
        report("centroid (128,149) not clear, ≈1/3 mix", want, g, !near(g, clear, 0) && near(g, want, TOL));
    }

    /* 3) Near the three vertices: vertex pixels v0(128,64) v1(64,192) v2(192,192), each sampled a few pixels inward toward the centroid */
    const struct { int x, y, ch; const char *name; } vs[3] = {
        {128, 72, 0, "near top vertex (128,72) reddish"},
        {72, 188, 1, "near bottom-left vertex (72,188) greenish"},
        {183, 188, 2, "near bottom-right vertex (183,188) bluish"},
    };
    for (int k = 0; k < 3; k++) {
        px_t want = tri_expect(vs[k].x, vs[k].y), g = get_px(buf, fmt, vs[k].x, vs[k].y);
        int ch[3] = {g.r, g.g, g.b};
        int dom = ch[vs[k].ch] >= 200 && ch[(vs[k].ch + 1) % 3] <= 30 && ch[(vs[k].ch + 2) % 3] <= 30;
        report(vs[k].name, want, g, dom && near(g, want, TOL));
    }

    /* 4) Whole image: interior pixels more than 1 pixel from an edge ≈ interpolated value (±TOL), exterior pixels = clear color; the ±1 pixel edge band is skipped */
    int n_in = 0, bad_in = 0, n_out = 0, bad_out = 0, n_edge = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            double l[3], d[3];
            bary(x + 0.5, y + 0.5, l, d);
            double dmin = fmin(d[0], fmin(d[1], d[2]));
            px_t g = get_px(buf, fmt, x, y);
            if (dmin > 1.0)       { n_in++;  if (!near(g, tri_expect(x, y), TOL)) bad_in++; }
            else if (dmin < -1.0) { n_out++; if (!near(g, clear, 0)) bad_out++; }
            else n_edge++;
        }
    printf("  whole image, inside %d pixels: expected ≈interpolated(±%d) → got %d mismatched%s\n", n_in, TOL, bad_in, bad_in ? " [MISMATCH]" : "  pass");
    printf("  whole image, outside %d pixels: expected =clear color       → got %d mismatched%s\n", n_out, bad_out, bad_out ? " [MISMATCH]" : "  pass");
    printf("  edge band skipped %d pixels (total %d)\n", n_edge, n_in + n_out + n_edge);
    if (bad_in || bad_out) fails++;
    mtlb_target_free(rt);
}

int main(void) {
    mtlb_ctx *ctx = mtlb_init();
    if (!ctx) { printf("mtlb_init failed\n"); return 1; }
    uint8_t *buf = malloc(W * H * 4);
    const mtlb_fmt fmts[2] = {MTLB_FMT_BGRA8, MTLB_FMT_RGBA8};
    for (int i = 0; i < 2; i++) test_clear(ctx, fmts[i], buf);
    for (int i = 0; i < 2; i++) test_triangle(ctx, fmts[i], buf);
    free(buf);
    mtlb_shutdown(ctx);
    printf("%s: mismatches %d\n", fails ? "[FAILED]" : "all passed", fails);
    return fails ? 1 : 0;
}
