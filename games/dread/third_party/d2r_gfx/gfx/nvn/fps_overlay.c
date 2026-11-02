#include "fps_overlay.h"
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "include/totk_ui_lang.h"

enum { GLYPH_W = 5, GLYPH_H = 7, ADVANCE = 6, PAD = 2, BOX_ROWS = GLYPH_H + 2 * PAD, MAX_CHARS = 80 };
static const uint8_t TEXT_BGRA[4] = {120, 255, 80, 255}, BOX_BGRA[4] = {12, 12, 12, 255}, HIGHLIGHT_BGRA[4] = {40, 230, 255, 255};   // green / dark / yellow

typedef struct { char c; uint8_t rows[GLYPH_H]; } Glyph;   /* each row: 5 bits, bit 4 = leftmost pixel */
static const Glyph FONT[] = {
    {'0', {0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e}}, {'1', {0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e}},
    {'2', {0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f}}, {'3', {0x1e, 0x01, 0x01, 0x0e, 0x01, 0x01, 0x1e}},
    {'4', {0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02}}, {'5', {0x1f, 0x10, 0x1e, 0x01, 0x01, 0x11, 0x0e}},
    {'6', {0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e}}, {'7', {0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e}}, {'9', {0x0e, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x0c}},
    {'F', {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10}}, {'P', {0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10}},
    {'S', {0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e}}, {'-', {0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00}},
    {'x', {0x00, 0x00, 0x11, 0x0a, 0x04, 0x0a, 0x11}}, {'>', {0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08}},
    {'M', {0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11}}, {'X', {0x11, 0x11, 0x0a, 0x04, 0x0a, 0x11, 0x11}},
    {'H', {0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11}}, {'A', {0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11}},
    {'D', {0x1e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1e}}, {'E', {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f}},
    {'R', {0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11}}, {'+', {0x00, 0x04, 0x04, 0x1f, 0x04, 0x04, 0x00}},
    {'/', {0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10}}, {'O', {0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e}},
    {'G', {0x0e, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0f}}, {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e}},
    {'%', {0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03}}, {'(', {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02}},
    {')', {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08}}, {'T', {0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}},
    {' ', {0, 0, 0, 0, 0, 0, 0}},
};

static _Atomic int g_hints_visible = 1;
int fps_overlay_hints_enabled(void) { return atomic_load_explicit(&g_hints_visible, memory_order_relaxed); }
int fps_overlay_toggle_hints(void) { return !atomic_fetch_xor_explicit(&g_hints_visible, 1, memory_order_relaxed); }

int fps_overlay_enabled(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("TOTK_FPS_OVERLAY"); on = !(e && !strcmp(e, "0")); }
    return on;
}

int fps_overlay_scale(double view_h) {
    int s = (int)lround(view_h * 0.03 / BOX_ROWS);
    return s < 1 ? 1 : s > FPS_OVERLAY_MAX_SCALE ? FPS_OVERLAY_MAX_SCALE : s;
}

static const Glyph *glyph(char c) {
    for (size_t i = 0; i < sizeof FONT / sizeof FONT[0]; i++) if (FONT[i].c == c) return &FONT[i];
    return &FONT[sizeof FONT / sizeof FONT[0] - 1];
}
int fps_overlay_has_glyph(char c) { return c == ' ' || glyph(c) != &FONT[sizeof FONT / sizeof FONT[0] - 1]; }

static void fill(uint8_t *out, int stride, int x0, int y0, int w, int h, const uint8_t bgra[4]) {
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++) memcpy(out + 4 * ((size_t)y * stride + x), bgra, 4);
}

static void size_text(char *out, size_t n, int w, int h) {
    if (w > 0 && h > 0) snprintf(out, n, "%dx%d", w > 9999 ? 9999 : w, h > 9999 ? 9999 : h);
    else snprintf(out, n, "--");
}

static void draw_text(uint8_t *out, int stride, const char *text, int n, int scale, const uint8_t color[4]) {
    for (int i = 0; i < n; i++) {
        const Glyph *g = glyph(text[i]);
        for (int r = 0; r < GLYPH_H; r++)
            for (int c = 0; c < GLYPH_W; c++)
                if (g->rows[r] & (0x10 >> c))
                    fill(out, stride, (PAD + i * ADVANCE + c) * scale, (PAD + r) * scale, scale, scale, color);
    }
}

/* A line with characters the 5x7 font does not have (Chinese labels, user 2026-10-17: "MetalFX已开启", "新着色器 N / 总计 M")
   in the system font (CoreText), with the same box height, padding and colours as the pixel font at this scale; the
   scale drops until the line fits the texture. */
static void raster_line_ct(const char *text, int highlight, int scale, uint8_t *out, int tex_w, int tex_h, int *box_w, int *box_h) {
    memset(out, 0, (size_t)tex_w * tex_h * 4);
    *box_w = *box_h = 0;
    CFStringRef str = CFStringCreateWithCString(NULL, text, kCFStringEncodingUTF8);
    if (!str) return;
    const uint8_t *c = highlight ? HIGHLIGHT_BGRA : TEXT_BGRA;
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    const CGFloat comps[4] = {c[2] / 255.0, c[1] / 255.0, c[0] / 255.0, 1.0};
    CGColorRef color = CGColorCreate(cs, comps);
    for (scale = scale < 1 ? 1 : scale > FPS_OVERLAY_MAX_SCALE ? FPS_OVERLAY_MAX_SCALE : scale; scale >= 1; scale--) {
        CTFontRef font = CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, 8.0 * scale, CFSTR("zh-Hans"));
        const void *keys[2] = {kCTFontAttributeName, kCTForegroundColorAttributeName}, *vals[2] = {font, color};
        CFDictionaryRef attrs = CFDictionaryCreate(NULL, keys, vals, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFAttributedStringRef as = CFAttributedStringCreate(NULL, str, attrs);
        CTLineRef line = CTLineCreateWithAttributedString(as);
        CGFloat ascent = 0, descent = 0;
        double tw = CTLineGetTypographicBounds(line, &ascent, &descent, NULL);
        int w = (int)ceil(tw) + 2 * PAD * scale, h = BOX_ROWS * scale;
        int fits = w <= tex_w && h <= tex_h;
        if (fits || scale == 1) {
            if (w > tex_w) w = tex_w;
            if (h > tex_h) h = tex_h;
            fill(out, tex_w, 0, 0, w, h, BOX_BGRA);
            CGContextRef ctx = CGBitmapContextCreate(out, (size_t)tex_w, (size_t)tex_h, 8, (size_t)tex_w * 4, cs,
                                                     kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);   // B, G, R, A in memory
            if (ctx) {   // memory row 0 is the top of the texture; Core Graphics counts y up from the bottom row
                CGContextClipToRect(ctx, CGRectMake(0, tex_h - h, w, h));
                CGContextSetTextPosition(ctx, PAD * scale, tex_h - h / 2.0 - (ascent - descent) / 2.0);
                CTLineDraw(line, ctx);
                CGContextRelease(ctx);
            }
            *box_w = w; *box_h = h;
        }
        CFRelease(line); CFRelease(as); CFRelease(attrs); CFRelease(font);
        if (fits || scale == 1) break;
    }
    CGColorRelease(color); CGColorSpaceRelease(cs); CFRelease(str);
}

/* The 5x7 font has only the letters the original labels use: any other character (Chinese, or English words such as
   "New shaders ... / total ...") sends the whole line to the system font instead of drawing blanks. */
int fps_overlay_line_pixel_font(const char *s) {
    for (; *s; s++) if (!fps_overlay_has_glyph(*s)) return 0;
    return 1;
}

void fps_overlay_raster_line_tex(const char *text, int highlight, int scale, uint8_t *out, int tex_w, int tex_h, int *box_w, int *box_h) {
    if (!fps_overlay_line_pixel_font(text)) { raster_line_ct(text, highlight, scale, out, tex_w, tex_h, box_w, box_h); return; }
    int n = (int)strnlen(text, MAX_CHARS);
    if (scale < 1) scale = 1;
    if (scale > FPS_OVERLAY_MAX_SCALE) scale = FPS_OVERLAY_MAX_SCALE;
    while (scale > 1 && ((n * ADVANCE - 1 + 2 * PAD) * scale > tex_w || BOX_ROWS * scale > tex_h)) scale--;   // never past the texture
    while (n > 1 && (n * ADVANCE - 1 + 2 * PAD) > tex_w) n--;   // scale 1 and still too long: cut the line
    int w = (n * ADVANCE - 1 + 2 * PAD) * scale, h = BOX_ROWS * scale;
    memset(out, 0, (size_t)tex_w * tex_h * 4);
    fill(out, tex_w, 0, 0, w, h, BOX_BGRA);
    draw_text(out, tex_w, text, n, scale, highlight ? HIGHLIGHT_BGRA : TEXT_BGRA);
    *box_w = w; *box_h = h;
}
void fps_overlay_raster_line(const char *text, int highlight, int scale, uint8_t *out, int *box_w, int *box_h) {
    fps_overlay_raster_line_tex(text, highlight, scale, out, FPS_OVERLAY_TEX_W, FPS_OVERLAY_TEX_H, box_w, box_h);
}

void fps_overlay_text(const fps_overlay_info *info, char *text, size_t n) {
    char fps[8], ss[12];
    if (info->fps < 0) snprintf(fps, sizeof fps, "--");
    else snprintf(fps, sizeof fps, "%d", info->fps > 999 ? 999 : info->fps);
    size_text(ss, sizeof ss, info->screen_w, info->screen_h);
    char gen[8] = "";
    if (info->gen > 0 && info->fps >= 0) snprintf(gen, sizeof gen, "+%d", info->gen > 999 ? 999 : info->gen);   // interpolated frames a second (key 2)
    // MetalFX did the upscale in the last frames (key 3 selects the manual MetalFX mode); 2K: true 2K output.
    // The whole line in the system font, so switching MetalFX does not switch the font.
    static int render = -1; if (render < 0) { const char *e = getenv("DREAD_HUD_RENDER"); render = e && !strcmp(e, "1"); }
    char rs[16] = "";
    if (render && info->render_w > 0) { size_text(rs, sizeof rs - 3, info->render_w, info->render_h); strcat(rs, " > "); }
    snprintf(text, n, "FPS %s%s  %s%s%s%s", fps, gen, rs, ss, info->mfx ? TOTK_UI("  MetalFX已开启", "  MetalFX on") : "", info->big ? "  2K" : "");
}

void fps_overlay_raster(const fps_overlay_info *info, int scale, uint8_t *out, int *box_w, int *box_h) {
    char text[MAX_CHARS + 1];
    fps_overlay_text(info, text, sizeof text);
    raster_line_ct(text, 0, scale, out, FPS_OVERLAY_TEX_W, FPS_OVERLAY_TEX_H, box_w, box_h);
}

int hud_compact(void) {
    static int on = -1;
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
    const int def = 0;   // the iOS frontend has no corner panel
#else
    const int def = 1;
#endif
    if (on < 0) { const char *e = getenv("TOTK_HUD"); on = e ? strcmp(e, "corners") != 0 : def; }
    return on;
}

static pthread_mutex_t g_hud_mu = PTHREAD_MUTEX_INITIALIZER;
static char g_hud_text[HUD_LINES][128];
static int g_hud_hl[HUD_LINES], g_hud_set[HUD_LINES];
void hud_publish(int line, const char *text, int highlight) {
    if (line < 0 || line >= HUD_LINES) return;
    pthread_mutex_lock(&g_hud_mu);
    snprintf(g_hud_text[line], sizeof g_hud_text[line], "%s", text); g_hud_hl[line] = highlight; g_hud_set[line] = 1;
    pthread_mutex_unlock(&g_hud_mu);
}
int hud_snapshot(int line, char *out, size_t n, int *highlight) {
    if (line < 0 || line >= HUD_LINES) return 0;
    pthread_mutex_lock(&g_hud_mu);
    int set = g_hud_set[line];
    if (set) { snprintf(out, n, "%s", g_hud_text[line]); if (highlight) *highlight = g_hud_hl[line]; }
    pthread_mutex_unlock(&g_hud_mu);
    return set;
}

int shader_overlay_enabled(void) {
    static int on = -1;
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
    const int def = 0;   // the phone translates nothing (misses go to shader-requests/)
#else
    const int def = 1;
#endif
    if (on < 0) { const char *e = getenv("TOTK_SHADER_OVERLAY"); on = e ? strcmp(e, "0") != 0 : def; }
    return on;
}

void shader_overlay_text(char *out, size_t n, uint64_t fresh, int64_t total, uint64_t pipelines) {
    char t[16];
    (void)pipelines;   // user 2026-10-17: the pipeline count is not shown
    if (total < 0) snprintf(t, sizeof t, "--"); else snprintf(t, sizeof t, "%lld", (long long)(total > 999999 ? 999999 : total));
    snprintf(out, n, TOTK_UI("新着色器 %llu / 总计 %s", "New shaders %llu / total %s"), (unsigned long long)(fresh > 99999 ? 99999 : fresh), t);
}

/* A count's rate, re-measured every half second (game presents; interpolated frames shown). stale_s > 0: a gap longer
   than that starts a new window (interpolation switched off and on again must not average over the time it was off). */
typedef struct { pthread_mutex_t mu; double t0, stale_s; uint64_t k0; _Atomic int value; } rate_meter;
static rate_meter g_game = {PTHREAD_MUTEX_INITIALIZER, -1, 0, 0, -1}, g_gen = {PTHREAD_MUTEX_INITIALIZER, -1, 1.0, 0, -1};
static void rate_note(rate_meter *m, uint64_t k, double now_s) {
    pthread_mutex_lock(&m->mu);
    if (m->t0 < 0 || k < m->k0 || (m->stale_s > 0 && now_s - m->t0 > m->stale_s)) { m->t0 = now_s; m->k0 = k; }
    else if (now_s - m->t0 >= 0.5) {
        atomic_store(&m->value, (int)lround((double)(k - m->k0) / (now_s - m->t0)));
        m->t0 = now_s; m->k0 = k;
    }
    pthread_mutex_unlock(&m->mu);
}

void fps_overlay_note(uint64_t ticket, double now_s) { rate_note(&g_game, ticket, now_s); }
int fps_overlay_value(void) { return atomic_load(&g_game.value); }
void fps_overlay_note_gen(uint64_t n, double now_s) { rate_note(&g_gen, n, now_s); }
int fps_overlay_gen_value(void) { return atomic_load(&g_gen.value); }

