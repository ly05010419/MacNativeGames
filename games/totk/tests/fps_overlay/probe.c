/* Unit test for third_party/d2r_gfx/gfx/nvn/fps_overlay.c: glyph pixels of the FPS and screen-size text against hand-written patterns, box
   bounds for every value, screen size and scale, frame-rate measurement, and a negative control
   proving the pattern check detects a wrong pixel.
   Prints one JSON line; exit 1 on failure. */
#include "fps_overlay.h"
#include "totk_sysload.h"
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); } } while (0)

static const uint8_t TEXT[4] = {120, 255, 80, 255}, BOX[4] = {12, 12, 12, 255};
static uint8_t px[FPS_OVERLAY_TEX_W * FPS_OVERLAY_TEX_H * 4];

/* "FPS 60" at scale 1, written out pixel by pixel (glyph columns separated by one blank column). */
static const char *const FPS60[7] = {
    "11111 11110 01111 00000 00110 01110",
    "10000 10001 10000 00000 01000 10001",
    "10000 10001 10000 00000 10000 10011",
    "11110 11110 01110 00000 11110 10101",
    "10000 10000 00001 00000 10001 11001",
    "10000 10000 00001 00000 10001 10001",
    "10000 10000 11110 00000 01110 01110",
};

static _Atomic int g_spin_stop;
static void *spin_main(void *arg) { (void)arg; pthread_setname_np("probe-spin"); volatile unsigned long x = 0; while (!atomic_load(&g_spin_stop)) x++; return NULL; }

static int pixel_is(int x, int y, const uint8_t want[4]) { return !memcmp(px + 4 * ((size_t)y * FPS_OVERLAY_TEX_W + x), want, 4); }

/* "8x9" after "FPS 60  " (8 characters: x = 2 + 48 at scale 1) (the box ends right after it). */
static const char *const LINE2[7] = {
    "01110 00000 01110",
    "10001 00000 10001",
    "10001 10001 10001",
    "01110 01010 01111",
    "10001 00100 00001",
    "10001 01010 00010",
    "01110 10001 01100",
};

static int pattern_at(const char *const rows[7], int x0) {   /* text area starts at y = 2 at scale 1 */
    int bad = 0;
    for (int r = 0; r < 7; r++)
        for (int c = 0; rows[r][c]; c++) {
            int on = rows[r][c] == '1';
            if (on ? !pixel_is(x0 + c, 2 + r, TEXT) : !pixel_is(x0 + c, 2 + r, BOX)) bad++;
        }
    return bad;
}
static int pattern_mismatches(const char *const rows[7]) { return pattern_at(rows, 2); }

/* The system-font lines (CoreText, antialiased): a pixel counts as text when it is clearly green / yellow. */
static const uint8_t *at(int x, int y) { return px + 4 * ((size_t)y * FPS_OVERLAY_TEX_W + x); }
static int is_green(int x, int y) { const uint8_t *p = at(x, y); return p[1] > 170 && p[2] < 150; }                  /* B G R A: text {120,255,80} */
static int is_yellow(int x, int y) { const uint8_t *p = at(x, y); return p[2] > 180 && p[1] > 150 && p[0] < 110; }  /* highlight {40,230,255} */
static int outside_dirty(int w, int h) {   /* pixels outside the box that are not fully transparent */
    int n = 0;
    for (int y = 0; y < FPS_OVERLAY_TEX_H; y++)
        for (int x = 0; x < FPS_OVERLAY_TEX_W; x++)
            if ((x >= w || y >= h) && memcmp(at(x, y), (const uint8_t[4]){0, 0, 0, 0}, 4)) n++;
    return n;
}
static void colours(int w, int h, int *green, int *yellow) {
    *green = *yellow = 0;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { *green += is_green(x, y); *yellow += is_yellow(x, y); }
}

int main(void) {
    int w, h, green, yellow;
    setenv("TOTK_UI_LANG", "zh", 1);   /* the Chinese labels below; the English ones are checked after them */
    /* The 5x7 pixel font (lines of ASCII only: the bottom-right load line): glyph pixels against the written patterns. */
    fps_overlay_raster_line("FPS 60  8x9", 0, 1, px, &w, &h);
    CHECK(w == 11 * 6 - 1 + 4 && h == 11, "box size %dx%d (\"FPS 60  8x9\" is 11 characters, one line)", w, h);
    CHECK(pattern_mismatches(FPS60) == 0, "FPS 60 glyph pixels differ from the written pattern (%d)", pattern_mismatches(FPS60));
    CHECK(pattern_at(LINE2, 2 + 8 * 6) == 0, "resolution glyph pixels differ from the written pattern (%d)", pattern_at(LINE2, 2 + 8 * 6));
    char broken[7][40];   /* negative control: one flipped pixel must be detected */
    const char *rows[7];
    for (int r = 0; r < 7; r++) { strcpy(broken[r], FPS60[r]); rows[r] = broken[r]; }
    broken[3][26] = broken[3][26] == '1' ? '0' : '1';
    CHECK(pattern_mismatches(rows) == 1, "negative control: flipped pixel not detected exactly once");

    /* The FPS box: the system font (it has "MetalFX已开启", user 2026-10-17), one line on the dark box; the box grows with
       what is shown. */
    int base, grown;
    fps_overlay_raster(&(fps_overlay_info){97, 2560, 1440, 0, 0, 0, 0, 0}, 2, px, &base, &h);
    CHECK(base > 40 && h == 11 * 2, "\"FPS 97  2560x1440\" at scale 2: box %dx%d", base, h);
    fps_overlay_raster(&(fps_overlay_info){97, 2560, 1440, 1, 0, 0, 0, 0}, 2, px, &grown, &h);
    CHECK(grown > base + 60, "MetalFX on: \"  MetalFX已开启\" after the size (box %d, without it %d)", grown, base);
    fps_overlay_raster(&(fps_overlay_info){50, 2560, 1440, 0, 50, 0, 0, 0}, 2, px, &grown, &h);
    CHECK(grown > base, "frame interpolation on: \"+50\" after the fps (box %d, without it %d)", grown, base);
    fps_overlay_raster(&(fps_overlay_info){97, 2560, 1440, 0, 0, 1, 0, 0}, 2, px, &grown, &h);
    CHECK(grown > base, "true 2K: \"  2K\" at the end (box %d, without it %d)", grown, base);
    fps_overlay_raster(&(fps_overlay_info){999, 99999, 99999, 1, 5000, 1, 99999, 99999}, FPS_OVERLAY_MAX_SCALE, px, &w, &h);   /* longest line: fits, or drops a scale */
    CHECK(w > 0 && w <= FPS_OVERLAY_TEX_W && h <= FPS_OVERLAY_TEX_H, "longest FPS line %dx%d outside the texture", w, h);

    const int values[] = {-1, 0, 7, 30, 60, 120, 999, 5000};
    const int sizes[][2] = {{0, 0}, {1920, 1080}, {3440, 1440}, {99999, 99999}};
    for (size_t i = 0; i < sizeof values / sizeof values[0]; i++)
        for (size_t z = 0; z < sizeof sizes / sizeof sizes[0]; z++)
        for (int mfx = 0; mfx <= 1; mfx++)
        for (int s = 1; s <= FPS_OVERLAY_MAX_SCALE; s++) {
            fps_overlay_raster(&(fps_overlay_info){values[i], sizes[z][0], sizes[z][1], mfx, 0, 0, 0, 0}, s, px, &w, &h);
            CHECK(w > 0 && h > 0 && w <= FPS_OVERLAY_TEX_W && h <= FPS_OVERLAY_TEX_H, "value %d scale %d box %dx%d outside texture", values[i], s, w, h);
            colours(w, h, &green, &yellow);
            int dirty = outside_dirty(w, h);
            CHECK((s < 2 || green > 0) && yellow == 0 && dirty == 0, "value %d scale %d: %d text pixels, %d yellow, %d pixels outside the box", values[i], s, green, yellow, dirty);
        }

    CHECK(fps_overlay_scale(1440) == 4 && fps_overlay_scale(900) == 2 && fps_overlay_scale(4000) == FPS_OVERLAY_MAX_SCALE && fps_overlay_scale(10) == 1,
          "scale for 1440/900/4000/10 = %d/%d/%d/%d", fps_overlay_scale(1440), fps_overlay_scale(900), fps_overlay_scale(4000), fps_overlay_scale(10));

    /* The automatic shader collection box (bottom-left, user 2026-10-17: "新着色器 N / 总计 M", no pipeline count): its text,
       bounds of the longest line at every scale, and the yellow highlight. */
    char line[96];
    shader_overlay_text(line, sizeof line, 3, 7428, 12);
    CHECK(!strcmp(line, "新着色器 3 / 总计 7428"), "shader box text \"%s\"", line);
    shader_overlay_text(line, sizeof line, 0, -1, 0);
    CHECK(!strcmp(line, "新着色器 0 / 总计 --"), "shader box text before the cache is counted \"%s\"", line);
    shader_overlay_text(line, sizeof line, 123456, 9999999, 123456);
    CHECK(!strcmp(line, "新着色器 99999 / 总计 999999"), "shader box text clamped \"%s\"", line);
    pso_overlay_text(line, sizeof line, 12, 3128);
    CHECK(!strcmp(line, "新渲染管线 12 / 总计 3128"), "render pipeline line \"%s\"", line);
    pso_overlay_text(line, sizeof line, 0, -1);
    CHECK(!strcmp(line, "新渲染管线 0 / 总计 --"), "render pipeline line before counting \"%s\"", line);
    { char fl[96];   /* output resolution, then the internal (scene) one in brackets (2026-10-23) */
      fps_overlay_text(&(fps_overlay_info){60, 1920, 1080, 0, 0, 0, 1600, 900}, fl, sizeof fl);
      CHECK(!strcmp(fl, "FPS 60  1920x1080 (1600x900)"), "fps line with the internal resolution \"%s\"", fl);
      fps_overlay_text(&(fps_overlay_info){30, 1280, 720, 0, 0, 0, 0, 0}, fl, sizeof fl);
      CHECK(!strcmp(fl, "FPS 30  1280x720"), "fps line before the scene is known \"%s\"", fl); }
    /* English labels (TOTK_UI_LANG=en, or a Mac whose language is not Chinese). */
    setenv("TOTK_UI_LANG", "en", 1);
    shader_overlay_text(line, sizeof line, 3, 7428, 12);
    CHECK(!strcmp(line, "New shaders 3 / total 7428"), "English shader box text \"%s\"", line);
    pso_overlay_text(line, sizeof line, 12, 3128);
    CHECK(!strcmp(line, "New render pipelines 12 / total 3128"), "English render pipeline line \"%s\"", line);
    CHECK(!fps_overlay_line_pixel_font(line), "English shader line \"%s\" drawn with the pixel font, which lacks its lower-case letters", line);
    CHECK(fps_overlay_line_pixel_font("FPS 60  2560x1440"), "the FPS line left the pixel font");
    fps_overlay_raster(&(fps_overlay_info){97, 2560, 1440, 1, 0, 0, 0, 0}, 2, px, &grown, &h);
    CHECK(grown > base + 40, "English MetalFX on: \"  MetalFX on\" after the size (box %d, without it %d)", grown, base);
    setenv("TOTK_UI_LANG", "zh", 1);
    shader_overlay_text(line, sizeof line, 123456, 9999999, 123456);
    CHECK(!fps_overlay_has_glyph('Q'), "negative control: 'Q' has no glyph in the pixel font but was reported");
    for (int s = 1; s <= FPS_OVERLAY_MAX_SCALE; s++) {
        fps_overlay_raster_line(line, s & 1, s, px, &w, &h);
        CHECK(w > 0 && h > 0 && w <= FPS_OVERLAY_TEX_W && h <= FPS_OVERLAY_TEX_H, "longest shader line at scale %d: box %dx%d outside the texture", s, w, h);
    }
    shader_overlay_text(line, sizeof line, 3, 7428, 12);
    fps_overlay_raster_line(line, 1, 3, px, &w, &h);
    colours(w, h, &green, &yellow);
    CHECK(h == 11 * 3 && yellow > 0 && green == 0 && outside_dirty(w, h) == 0, "highlighted line: box %dx%d, %d yellow and %d green pixels", w, h, yellow, green);
    fps_overlay_raster_line(line, 0, 3, px, &w, &h);
    colours(w, h, &green, &yellow);
    CHECK(yellow == 0 && green > 0, "plain line: %d yellow and %d green pixels", yellow, green);

    CHECK(fps_overlay_value() == -1, "value before any present");
    uint64_t k = 0; double t = 100.0;
    for (int i = 0; i < 120; i++) { fps_overlay_note(++k, t); t += 1.0 / 60; }
    CHECK(fps_overlay_value() == 60, "60 presents per second measured as %d", fps_overlay_value());
    for (int i = 0; i < 60; i++) { fps_overlay_note(++k, t); t += 1.0 / 30; }
    CHECK(fps_overlay_value() == 30, "30 presents per second measured as %d", fps_overlay_value());
    for (int i = 0; i < 60; i++) { k += 2; fps_overlay_note(k, t); t += 1.0 / 30; }   /* display link merged every other game frame */
    CHECK(fps_overlay_value() == 60, "merged frames: 60 game presents per second measured as %d", fps_overlay_value());
    /* Interpolated frames shown: their own rate, independent of the game rate; a gap of more than a second (interpolation
       switched off and on) starts a new window instead of averaging the off time in. */
    CHECK(fps_overlay_gen_value() == -1, "generated rate before any interpolated frame");
    uint64_t gk = 0; double gt = 200.0;
    for (int i = 0; i < 100; i++) { fps_overlay_note_gen(++gk, gt); gt += 1.0 / 50; }
    CHECK(fps_overlay_gen_value() == 50, "50 interpolated frames per second measured as %d", fps_overlay_gen_value());
    CHECK(fps_overlay_value() == 60, "the game rate is unaffected by the generated one (%d)", fps_overlay_value());
    gt += 30.0;   /* off for 30 s, then on again */
    for (int i = 0; i < 60; i++) { fps_overlay_note_gen(++gk, gt); gt += 1.0 / 50; }
    CHECK(fps_overlay_gen_value() == 50, "after a gap: 50 per second, not an average over the gap (%d)", fps_overlay_gen_value());

    /* The bottom-right load line: text, its glyphs, the widest line within the wider texture at every scale, the busy-time
       union of in-order command buffers, and one real sample of this machine (cores split into P and E, GPU known). */
    sysload_info li = {4, 6, {55, 56, 55, 53}, {43, 29, 20, 12, 9, 7}, 99, 70, 47, "x"};
    sysload_text(line, sizeof line, &li);
    CHECK(!strcmp(line, "GPU 70%  P 55% 56% 55% 53%  E 43% 29% 20% 12%  9%  7%  T 47%"), "load line \"%s\"", line);   /* the game's share only */
    li.gpu_sys = -1; li.gpu_game = -1; li.thread_max = -1; sysload_text(line, sizeof line, &li);
    CHECK(!strcmp(line, "GPU --  P 55% 56% 55% 53%  E 43% 29% 20% 12%  9%  7%"), "load line without GPU numbers \"%s\"", line);
    for (const char *c = "PEGUT%()"; *c; c++) CHECK(fps_overlay_has_glyph(*c), "no glyph for '%c' (load line)", *c);
    static uint8_t lpx[LOAD_OVERLAY_TEX_W * FPS_OVERLAY_TEX_H * 4];
    sysload_info wide = {4, 6, {100, 100, 100, 100}, {100, 100, 100, 100, 100, 100}, 100, 100, 100, "x"};
    char wl[128]; sysload_text(wl, sizeof wl, &wide);
    int unit = (int)strlen(wl) * 6 - 1 + 4;
    for (int s = 1; s <= FPS_OVERLAY_MAX_SCALE; s++) {
        fps_overlay_raster_line_tex(wl, 0, s, lpx, LOAD_OVERLAY_TEX_W, FPS_OVERLAY_TEX_H, &w, &h);
        CHECK(w > 0 && h > 0 && w <= LOAD_OVERLAY_TEX_W && h <= FPS_OVERLAY_TEX_H && w % unit == 0,
              "widest load line at scale %d: box %dx%d (the whole line, within the texture)", s, w, h);
    }
    fps_overlay_raster_line_tex(wl, 0, 4, lpx, LOAD_OVERLAY_TEX_W, FPS_OVERLAY_TEX_H, &w, &h);
    CHECK(w == unit * 4, "the widest load line keeps scale 4 (1440p) in its texture: width %d", w);
    sysload_busy b = {0, 0};
    sysload_busy_add(&b, 1.0, 1.005); sysload_busy_add(&b, 1.005, 1.010); sysload_busy_add(&b, 1.008, 1.012); sysload_busy_add(&b, 1.020, 1.020);
    CHECK(b.busy > 0.01199 && b.busy < 0.01201, "busy union of 0-5, 5-10, 8-12 ms (and an empty one) is 12 ms, got %.6f s", b.busy);
    pthread_t spin; pthread_create(&spin, NULL, spin_main, NULL);   // a thread that keeps one core busy: the busiest thread
    sysload_start(); usleep(1200000);
    sysload_info real; int got = sysload_get(&real);
    CHECK(got && real.np >= 1 && real.np + real.ne >= 2 && real.gpu_sys >= 0 && real.gpu_sys <= 100 && real.gpu_game == 0 && real.thread_max >= 0 && real.thread_max <= 100 && real.thread_name[0],
          "a real sample: %d P and %d E cores, busiest thread %d%% (%s), GPU %d%%, game GPU %d%% (no command buffers in this probe)", real.np, real.ne, real.thread_max, real.thread_name, real.gpu_sys, real.gpu_game);
    for (int i = 0; got && i < real.np; i++) CHECK(real.p[i] >= 0 && real.p[i] <= 100, "P core %d load %d", i, real.p[i]);
    for (int i = 0; got && i < real.ne; i++) CHECK(real.e[i] >= 0 && real.e[i] <= 100, "E core %d load %d", i, real.e[i]);
    CHECK(got && real.thread_max >= 80 && !strcmp(real.thread_name, "probe-spin"), "the spinning thread is the busiest: %d%% (%s)", real.thread_max, real.thread_name);
    atomic_store(&g_spin_stop, 1); pthread_join(spin, NULL);
    if (got) { sysload_text(line, sizeof line, &real); fprintf(stderr, "this machine now: %s (busiest thread %s)\n", line, real.thread_name); }

    printf("{\"status\":\"%s\",\"probe\":\"probe-fps-overlay\",\"checked\":%d,\"failures\":%d,\"scope\":\"5x7 glyph pixels (ASCII lines), the system-font FPS box (MetalFX label, box growth, text colour, nothing outside the box), box bounds for values -1..5000 and screen sizes at scales 1..%d, scale choice, game-present and interpolated-frame rate measurement, FPS 50+50 form, bottom-right load line (text, glyphs, width, GPU busy union, busiest thread found, one real sample), shader collection box text / glyphs / bounds / highlight; negative controls flip one pixel and ask for a missing glyph\"}\n",
           failures ? "fail" : "pass", checks, failures, FPS_OVERLAY_MAX_SCALE);
    return failures ? 1 : 0;
}
