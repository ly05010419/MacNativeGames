/* zeldaVR, Ocarina of Time: the VR menu in the room and the big screen's drag (vr.md rows 90 and 4). The layout, the open button and
   the mask are zeldaVR/runtime/oot_vr_menu.h, the laser and the drag zeldaVR/runtime/oot_vr_laser.h (both unit-tested); the rows are
   the VR settings page's own, handed over by games/oot/runtime/settings_vr_menu.h (runtime_vr_menu_rows) and changed through it
   (runtime_vr_menu_change), so a change shows on the Mac's page too. HookshotVR's ideas only (GPL-3, no code).

   Threads: the pad's thread (oot_gamepad) watches the open button and B, and while the menu holds the controllers gives the game
   nothing (oot_vrmenu_pad); it also keeps a grip that is the big screen's from the game (oot_vrmenu_grips). The VR thread places
   the panel, points the lasers, presses what they point at, draws (oot_vrmenu_update / oot_vrmenu_draw) and drags the big screen.
   Included by totk_vr.m (GAME_OOT) after oot_vr_hud.h, before oot_arc_draw, which takes the big screen's place from here. The
   pictures are drawn with CoreGraphics: text and shapes only. Logs: one line when something happens, "[vr-menu]" / "[vr-screen]";
   TOTK_VR_BLADELOG=1 adds each laser hit as it changes. */
#include <CoreText/CoreText.h>
#include "../runtime/oot_vr_menu.h"
#include "../runtime/oot_vr_laser.h"
#include "totk_ui_lang.h"
extern int runtime_vr_menu_rows(oot_menu_row *out, int max, uint32_t *version) __attribute__((weak_import));
extern void runtime_vr_menu_change(int index, int how, int value) __attribute__((weak_import));
extern void runtime_vr_menu_refresh(void) __attribute__((weak_import));

/* Options (the VR page; settings_vr_menu.h has the rows). The open button by default: the left stick click held alone 1 s, which
   does nothing now (oot_vr_menu.h); the rest is off or as before until the player changes it. */
int vr_oot_menu_button(void) { const int v = vr_option("menu_button", OOT_MENU_BUTTON_L3); return v < 0 || v > 2 ? OOT_MENU_BUTTON_L3 : v; }
void vr_oot_set_menu_button(int v) { vr_option_set("menu_button", v < 0 || v > 2 ? OOT_MENU_BUTTON_L3 : v);
    VLOG("[vr-menu] opened by %s", v == 1 ? "the left stick click held alone 1 s" : v == 2 ? "the Menu button held 1.5 s" : "no button"); }
int vr_oot_menu_follow(void) { return vr_option("menu_follow", 0) != 0; }
void vr_oot_set_menu_follow(int on) { vr_option_set("menu_follow", on != 0); VLOG("[vr-menu] %s", on ? "follows the head" : "stays where it opened"); }
int vr_oot_laser_deg(void) { const int v = vr_option("laser_deg", 0); return v < -45 ? -45 : v > 45 ? 45 : v; }
void vr_oot_set_laser_deg(int v) { vr_option_set("laser_deg", v < -45 ? -45 : v > 45 ? 45 : v); VLOG("[vr-menu] the laser %d degrees from the controller's line", vr_oot_laser_deg()); }
int vr_oot_screen_drag(void) { return vr_option("screen_drag", 0) != 0; }
void vr_oot_set_screen_drag(int on) { vr_option_set("screen_drag", on != 0); VLOG("[vr-screen] dragging the big screen: %s", on ? "on (a grip with the laser on it)" : "off"); }
int vr_oot_screen_yaw(void) { const int v = vr_option("screen_yaw", 0); return v < -180 ? -180 : v > 180 ? 180 : v; }
void vr_oot_set_screen_yaw(int v) { vr_option_set("screen_yaw", v < -180 ? -180 : v > 180 ? 180 : v); }
int vr_oot_screen_up(void) { const int v = vr_option("screen_up", 0); return v < -150 ? -150 : v > 150 ? 150 : v; }
void vr_oot_set_screen_up(int v) { vr_option_set("screen_up", v < -150 ? -150 : v > 150 ? 150 : v); }
static int oot_vrmenu_log_on(void) { static int on = -1; if (on < 0) { const char *e = getenv("TOTK_VR_BLADELOG"); on = e && *e == '1'; } return on; }

static _Atomic int g_vrm_open;            /* the menu is open */
static _Atomic uint32_t g_vrm_opens;      /* how many times it opened: the VR thread places it anew */
static _Atomic int g_vrm_grips;           /* bit h: hand h's grip is the big screen's (its laser on it, or dragging it) */
static void oot_vrmenu_set_open(int open, const char *why) {
    if (atomic_exchange(&g_vrm_open, open) == open) return;
    if (open) { atomic_fetch_add(&g_vrm_opens, 1); if (oot_wheel_is_open()) oot_wheel_close("the VR menu opened"); }
    VLOG("[vr-menu] %s (%s)", open ? "opened" : "closed", why);
}

/* ---- the pad's thread ---- */
/* The controllers as the game is to see them: a grip that is the big screen's let go. */
static void oot_vrmenu_grips(vr_controls *c) {
    const int g2 = atomic_load(&g_vrm_grips);
    if (g2 & 1) c->lgrip = 0;
    if (g2 & 2) c->rgrip = 0;
}
/* 1 while the game is to get nothing from the controllers (the menu open, or after it closed until all is let go). */
static int oot_vrmenu_pad(uint64_t now, const vr_controls *c) {
    static oot_menu_open_state os; static oot_menu_mask mask; static uint32_t last;
    const int busy = fabsf(c->lx) > 0.3f || fabsf(c->ly) > 0.3f || fabsf(c->rx) > 0.3f || fabsf(c->ry) > 0.3f ||
                     c->ltrig > 0.5f || c->rtrig > 0.5f || c->lgrip > 0.5f || c->rgrip > 0.5f;
    const int option = vr_oot_menu_button(), open = atomic_load(&g_vrm_open);
    if (oot_menu_open_step(&os, now, c->buttons, busy, option)) {
        if (open) oot_vrmenu_set_open(0, "the open button");
        else if (!runtime_vr_menu_rows || !runtime_vr_menu_change) { static int said; if (!said++) VLOG("[vr-menu] not opened: the settings page's rows are not in this build"); }
        else oot_vrmenu_set_open(1, option == OOT_MENU_BUTTON_L3 ? "the left stick click held 1 s" : "the Menu button held 1.5 s");
    }
    if (atomic_load(&g_vrm_open) && (c->buttons & ~last & VR_PAD_B)) oot_vrmenu_set_open(0, "B");
    last = c->buttons;
    const int any = c->buttons != 0 || c->ltrig > 0.3f || c->rtrig > 0.3f;
    static int was;
    const int held = oot_menu_mask_step(&mask, now, atomic_load(&g_vrm_open), any);
    if (held != was) { was = held; VLOG("[vr-menu] the controllers %s", held ? "are the menu's: the game gets nothing" : "are the game's again"); }
    return held;
}

/* ---- the big screen's place (row 4): the VR thread ---- */
static struct { oot_screen_place place; oot_screen_drag drag; oot_laser_button grip[2]; uint64_t arc_at; float half, height; int hover[2]; } g_scr;
static oot_screen_place oot_scr_place_now(void) {
    if (g_scr.drag.mode != OOT_SCREEN_IDLE) return g_scr.place;
    const oot_screen_place p = { (float)vr_oot_screen_yaw(), (float)vr_oot_screen_up() / 100, (float)vr_oot_screen_deg() };
    return p;
}
/* oot_arc_draw: the screen's frame as it is to be drawn now (its half angle and height from oot_arc_size at this width). */
static float oot_scr_deg(void) { return oot_scr_place_now().deg; }
static int oot_scr_frame(float half, float height, oot_arc_frame *f) {
    const oot_screen_place p = oot_scr_place_now();
    if (!oot_arc_frame_make(g.menu_center, g.menu_right, 2.0f, p.yaw_deg, p.up_m, OOT_ARC_RADIUS_M, half, height, f)) return 0;
    g_scr.arc_at = now_ns(); g_scr.half = half; g_scr.height = height;
    return 1;
}

/* ---- the menu: the VR thread ---- */
static struct {
    uint32_t placed_for, version, drawn_version; int n, page, drawn_page, drawn_n, show, dirty;
    oot_menu_quad q; oot_hud_state follow; uint64_t at, asked_at;
    oot_menu_row rows[OOT_MENU_ROWS_MAX]; int kinds[OOT_MENU_ROWS_MAX];
    oot_laser_button trig[2]; int slide[2], slide_sent[2];   /* the slider row each hand's trigger holds (-1 none), the value last sent */
    oot_menu_hit hover[2], drawn_hover[2];
    int ray_on[2], dot_on[2]; float ray_o[2][3], ray_end[2][3];
} g_vrm = { .slide = { -1, -1 } };
static id<MTLTexture> g_vrm_tex, g_vrm_ray_tex, g_vrm_dot_tex;

static void vrm_text(CGContextRef c, const char *utf8, CGFloat size, CGFloat x, CGFloat y_down, CGFloat width, int align, CGFloat rgb, CGFloat alpha, int bold) {
    if (!utf8 || !*utf8 || width <= 0) return;
    CTFontRef font = CTFontCreateUIFontForLanguage(bold ? kCTFontUIFontEmphasizedSystem : kCTFontUIFontSystem, size, NULL);
    if (!font) return;
    NSString *s = @(utf8); if (!s) { CFRelease(font); return; }
    NSDictionary *attrs = @{ (__bridge id)kCTFontAttributeName: (__bridge id)font, (__bridge id)kCTForegroundColorFromContextAttributeName: @YES };
    CTLineRef line = CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)[[NSAttributedString alloc] initWithString:s attributes:attrs]);
    if (line && CTLineGetTypographicBounds(line, NULL, NULL, NULL) > width) {   /* too long: cut with an ellipsis */
        CTLineRef dots = CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)[[NSAttributedString alloc] initWithString:@"…" attributes:attrs]);
        CTLineRef cut = dots ? CTLineCreateTruncatedLine(line, width, kCTLineTruncationEnd, dots) : NULL;
        if (dots) CFRelease(dots);
        if (cut) { CFRelease(line); line = cut; }
    }
    if (line) {
        CGFloat ascent = 0, descent = 0; const CGFloat w = (CGFloat)CTLineGetTypographicBounds(line, &ascent, &descent, NULL);
        const CGFloat at = align == 0 ? x : align == 1 ? x + (width - w) / 2 : x + width - w;
        CGContextSetRGBFillColor(c, rgb, rgb * 0.95, rgb * 0.85, alpha);
        CGContextSetTextPosition(c, at, OOT_MENU_H - y_down - (ascent - descent) / 2); CTLineDraw(line, c); CFRelease(line);
    }
    CFRelease(font);
}
static void vrm_box(CGContextRef c, CGFloat x0, CGFloat y0, CGFloat x1, CGFloat y1, CGFloat radius, int lit, CGFloat alpha) {
    const CGRect r = CGRectMake(x0, OOT_MENU_H - y1, x1 - x0, y1 - y0);
    CGPathRef p = CGPathCreateWithRoundedRect(r, radius, radius, NULL);
    CGContextAddPath(c, p);
    if (lit) CGContextSetRGBFillColor(c, 0.72, 0.47, 0.30, alpha); else CGContextSetRGBFillColor(c, 0.22, 0.15, 0.10, alpha);
    CGContextFillPath(c);
    CGContextAddPath(c, p); CGContextSetRGBStrokeColor(c, 0.72, 0.47, 0.30, alpha); CGContextSetLineWidth(c, 2); CGContextStrokePath(c);
    CGPathRelease(p);
}
static int vrm_lit(int kind, int row) {
    for (int h = 0; h < 2; h++) if (g_vrm.hover[h].kind == kind && (row < 0 || g_vrm.hover[h].row == row)) return 1;
    return 0;
}
/* The panel's picture for this page and what the pointers are over. */
static void vrm_render(void) {
    enum { W = OOT_MENU_W, H = OOT_MENU_H }; static uint8_t px[W * H * 4];
    CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB();
    CGContextRef c = CGBitmapContextCreate(px, W, H, 8, W * 4, rgb, (CGBitmapInfo)kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    CGColorSpaceRelease(rgb);
    if (!c) return;
    const int zh = totk_ui_zh(), pages = oot_menu_pages(g_vrm.n);
    CGContextClearRect(c, CGRectMake(0, 0, W, H));
    { CGPathRef p = CGPathCreateWithRoundedRect(CGRectMake(4, 4, W - 8, H - 8), 28, 28, NULL);
      CGContextAddPath(c, p); CGContextSetRGBFillColor(c, 0.09, 0.06, 0.04, 0.93); CGContextFillPath(c);
      CGContextAddPath(c, p); CGContextSetRGBStrokeColor(c, 0.72, 0.47, 0.30, 1); CGContextSetLineWidth(c, 6); CGContextStrokePath(c); CGPathRelease(p); }
    char head[96]; snprintf(head, sizeof head, zh ? "VR 设置   %d / %d" : "VR settings   %d / %d", g_vrm.page + 1, pages);
    vrm_text(c, head, 38, 40, 50, 1000, 0, 0.96, 1, 1);
    vrm_box(c, OOT_MENU_CLOSE_X0, OOT_MENU_CLOSE_Y0, OOT_MENU_CLOSE_X1, OOT_MENU_CLOSE_Y1, 14, vrm_lit(OOT_MENU_HIT_CLOSE, -1), 1);
    vrm_text(c, "✕", 36, OOT_MENU_CLOSE_X0, (OOT_MENU_CLOSE_Y0 + OOT_MENU_CLOSE_Y1) / 2.0, OOT_MENU_CLOSE_X1 - OOT_MENU_CLOSE_X0, 1, 0.96, 1, 1);
    if (g_vrm.n <= 0) vrm_text(c, zh ? "正在读取设置页……" : "Reading the settings page...", 34, 40, H / 2.0, W - 80, 1, 0.96, 1, 0);
    for (int k = 0; k < OOT_MENU_PER_PAGE; k++) {
        const int i = g_vrm.page * OOT_MENU_PER_PAGE + k; if (i >= g_vrm.n) break;
        const oot_menu_row *r = &g_vrm.rows[i];
        const CGFloat y0 = OOT_MENU_TOP + k * OOT_MENU_ROW_H, mid = y0 + OOT_MENU_ROW_H / 2.0, a = r->enabled ? 1 : 0.45;
        if (vrm_lit(OOT_MENU_HIT_ROW, i) || vrm_lit(OOT_MENU_HIT_LESS, i) || vrm_lit(OOT_MENU_HIT_MORE, i) || vrm_lit(OOT_MENU_HIT_TRACK, i) || g_vrm.slide[0] == i || g_vrm.slide[1] == i) {
            CGContextSetRGBFillColor(c, 0.72, 0.47, 0.30, 0.22); CGContextFillRect(c, CGRectMake(20, H - y0 - OOT_MENU_ROW_H + 4, W - 40, OOT_MENU_ROW_H - 8));
        }
        vrm_text(c, r->title, 28, 40, mid, OOT_MENU_LESS_X0 - 60, 0, 0.96, a, 0);
        if (r->kind == OOT_MENU_INFO) { vrm_text(c, r->value, 26, OOT_MENU_LESS_X0, mid, OOT_MENU_MORE_X1 - OOT_MENU_LESS_X0, 2, 0.8, a, 0); continue; }
        vrm_box(c, OOT_MENU_LESS_X0, y0 + 12, OOT_MENU_LESS_X1, y0 + OOT_MENU_ROW_H - 12, 12, vrm_lit(OOT_MENU_HIT_LESS, i), a);
        vrm_text(c, "‹", 40, OOT_MENU_LESS_X0, mid, OOT_MENU_LESS_X1 - OOT_MENU_LESS_X0, 1, 0.96, a, 1);
        vrm_box(c, OOT_MENU_MORE_X0, y0 + 12, OOT_MENU_MORE_X1, y0 + OOT_MENU_ROW_H - 12, 12, vrm_lit(OOT_MENU_HIT_MORE, i), a);
        vrm_text(c, "›", 40, OOT_MENU_MORE_X0, mid, OOT_MENU_MORE_X1 - OOT_MENU_MORE_X0, 1, 0.96, a, 1);
        if (r->kind == OOT_MENU_SLIDER && r->max > r->min) {
            const CGFloat f = fmin(1, fmax(0, (CGFloat)(r->now - r->min) / (r->max - r->min))), tx = OOT_MENU_MID_X0 + 20, tw = OOT_MENU_MID_X1 - OOT_MENU_MID_X0 - 40;
            CGContextSetRGBFillColor(c, 0.45, 0.30, 0.20, a); CGContextFillRect(c, CGRectMake(tx, H - mid - 22, tw, 6));
            CGContextSetRGBFillColor(c, 0.96, 0.84, 0.36, a); CGContextFillRect(c, CGRectMake(tx, H - mid - 22, tw * f, 6));
            CGContextFillEllipseInRect(c, CGRectMake(tx + tw * f - 12, H - mid - 31, 24, 24));
            vrm_text(c, r->value, 26, OOT_MENU_MID_X0, mid - 12, OOT_MENU_MID_X1 - OOT_MENU_MID_X0, 1, 0.96, a, 1);
        } else vrm_text(c, r->value, 28, OOT_MENU_MID_X0, mid, OOT_MENU_MID_X1 - OOT_MENU_MID_X0, 1, 0.96, a, 1);
    }
    if (g_vrm.page > 0) { vrm_box(c, OOT_MENU_PREV_X0, OOT_MENU_FOOT_Y0, OOT_MENU_PREV_X1, OOT_MENU_FOOT_Y1, 16, vrm_lit(OOT_MENU_HIT_PREV, -1), 1);
        vrm_text(c, zh ? "‹ 上一页" : "‹ Previous", 32, OOT_MENU_PREV_X0, (OOT_MENU_FOOT_Y0 + OOT_MENU_FOOT_Y1) / 2.0, OOT_MENU_PREV_X1 - OOT_MENU_PREV_X0, 1, 0.96, 1, 1); }
    if (g_vrm.page + 1 < pages) { vrm_box(c, OOT_MENU_NEXT_X0, OOT_MENU_FOOT_Y0, OOT_MENU_NEXT_X1, OOT_MENU_FOOT_Y1, 16, vrm_lit(OOT_MENU_HIT_NEXT, -1), 1);
        vrm_text(c, zh ? "下一页 ›" : "Next ›", 32, OOT_MENU_NEXT_X0, (OOT_MENU_FOOT_Y0 + OOT_MENU_FOOT_Y1) / 2.0, OOT_MENU_NEXT_X1 - OOT_MENU_NEXT_X0, 1, 0.96, 1, 1); }
    vrm_text(c, zh ? "激光指着，扳机选择 · B 关闭" : "Point the laser, trigger chooses · B closes", 26, OOT_MENU_PREV_X1 + 20,
             (OOT_MENU_FOOT_Y0 + OOT_MENU_FOOT_Y1) / 2.0, OOT_MENU_NEXT_X0 - OOT_MENU_PREV_X1 - 40, 1, 0.75, 1, 0);
    CGContextRelease(c);
    if (!g_vrm_tex) {
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:W height:H mipmapped:NO];
        d.usage = MTLTextureUsageShaderRead; d.storageMode = MTLStorageModeShared; g_vrm_tex = [g.dev newTextureWithDescriptor:d];
    }
    if (g_vrm_tex) [g_vrm_tex replaceRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0 withBytes:px bytesPerRow:W * 4];
}
/* The laser's ray (a warm line) and its dot (a glow): made once. */
static void vrm_laser_textures(void) {
    if (g_vrm_ray_tex && g_vrm_dot_tex) return;
    enum { N = 64 }; static uint8_t ray[4 * 4 * 4], dot[N * N * 4];
    for (int i = 0; i < 16; i++) { ray[i * 4 + 0] = 0x5c * 0.85; ray[i * 4 + 1] = 0xd6 * 0.85; ray[i * 4 + 2] = 0xf5 * 0.85; ray[i * 4 + 3] = 0xd9; }   /* BGRA, premultiplied */
    for (int y = 0; y < N; y++) for (int x = 0; x < N; x++) {
        const float d = hypotf(x - N / 2 + 0.5f, y - N / 2 + 0.5f) / (N / 2), a = fminf(1, fmaxf(0, (0.55f - d) * 10)) * 0.9f + fminf(1, fmaxf(0, 1 - d)) * 0.3f, k = fminf(1, a);
        uint8_t *p = dot + (y * N + x) * 4; p[0] = (uint8_t)(0x8c * k); p[1] = (uint8_t)(0xbc * k); p[2] = (uint8_t)(0xf2 * k); p[3] = (uint8_t)(255 * k);
    }
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:4 height:4 mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead; d.storageMode = MTLStorageModeShared;
    g_vrm_ray_tex = [g.dev newTextureWithDescriptor:d];
    if (g_vrm_ray_tex) [g_vrm_ray_tex replaceRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0 withBytes:ray bytesPerRow:16];
    d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:N height:N mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead; d.storageMode = MTLStorageModeShared;
    g_vrm_dot_tex = [g.dev newTextureWithDescriptor:d];
    if (g_vrm_dot_tex) [g_vrm_dot_tex replaceRegion:MTLRegionMake2D(0, 0, N, N) mipmapLevel:0 withBytes:dot bytesPerRow:N * 4];
}
static const char *vrm_hit_name(int kind) {
    static const char *names[] = { "nothing", "a row", "less", "more", "the track", "the previous page", "the next page", "close" };
    return kind >= 0 && kind <= OOT_MENU_HIT_CLOSE ? names[kind] : "?";
}
/* A trigger's press on what hand h points at. */
static void vrm_press(int h, oot_menu_hit hit) {
    const oot_menu_row *r = hit.row >= 0 && hit.row < g_vrm.n ? &g_vrm.rows[hit.row] : NULL;
    switch (hit.kind) {
    case OOT_MENU_HIT_CLOSE: oot_vrmenu_set_open(0, "its close button"); return;
    case OOT_MENU_HIT_PREV: g_vrm.page--; g_vrm.dirty = 1; VLOG("[vr-menu] page %d", g_vrm.page + 1); break;
    case OOT_MENU_HIT_NEXT: g_vrm.page++; g_vrm.dirty = 1; VLOG("[vr-menu] page %d", g_vrm.page + 1); break;
    case OOT_MENU_HIT_LESS: case OOT_MENU_HIT_MORE: case OOT_MENU_HIT_ROW:
        if (!r || !r->enabled || r->kind == OOT_MENU_INFO || (hit.kind == OOT_MENU_HIT_ROW && r->kind == OOT_MENU_SLIDER)) return;
        runtime_vr_menu_change(hit.row, OOT_MENU_STEP, hit.kind == OOT_MENU_HIT_LESS ? -1 : 1);
        VLOG("[vr-menu] row %d \"%s\": %s (was %s)", hit.row, r->title, hit.kind == OOT_MENU_HIT_LESS ? "less" : "more", r->value);
        break;
    case OOT_MENU_HIT_TRACK:
        if (!r || !r->enabled || r->kind != OOT_MENU_SLIDER) return;
        g_vrm.slide[h] = hit.row; g_vrm.slide_sent[h] = oot_menu_slider_value(r->min, r->max, hit.frac);
        runtime_vr_menu_change(hit.row, OOT_MENU_SET, oot_menu_slider_value(r->min, r->max, hit.frac));
        VLOG("[vr-menu] row %d \"%s\": slider taken at %d", hit.row, r->title, oot_menu_slider_value(r->min, r->max, hit.frac));
        break;
    default: return;
    }
    vr_link_send_haptic(h, 25000000ull, 200, 0.4f);
}
/* Once a headset frame, before the eyes are drawn. */
static void oot_vrmenu_update(const vr_pose *head) {
    const uint64_t t = now_ns();
    const float dt = g_vrm.at ? fminf((float)((t - g_vrm.at) / 1e9), 0.1f) : 0; g_vrm.at = t;
    vr_hand hands[2]; vr_oot_hands(hands); vr_controls pad; const int have_pad = vr_link_controls(&pad);   /* the calibrated hands, as every OOT reader (vr.md row 31) */
    const float trig[2] = { have_pad ? pad.ltrig : 0, have_pad ? pad.rtrig : 0 }, grip[2] = { have_pad ? pad.lgrip : 0, have_pad ? pad.rgrip : 0 };
    const float tilt = (float)vr_oot_laser_deg();
    float o[2][3], d[2][3]; int ray[2];
    for (int h = 0; h < 2; h++) ray[h] = hands[h].have && oot_laser_ray(hands[h].q, hands[h].p, tilt, o[h], d[h]);
    g_vrm.ray_on[0] = g_vrm.ray_on[1] = g_vrm.dot_on[0] = g_vrm.dot_on[1] = 0;
    g_vrm.show = 0;
    const int open = atomic_load(&g_vrm_open);
    if (open) {
        g_scr.drag.mode = OOT_SCREEN_IDLE; atomic_store(&g_vrm_grips, 0);
        const float nz[3] = { 0, 0, -1 }; float f[3]; qrot(head->q, nz, f);
        const int level = f[0] * f[0] + f[2] * f[2] > 1e-4f; const float heading = level ? oot_hud_heading(f) : g_vrm.follow.yaw;
        const uint32_t opens = atomic_load(&g_vrm_opens);
        if (g_vrm.placed_for != opens) {   /* placed once on opening, where the head looks */
            g_vrm.placed_for = opens; g_vrm.follow = (oot_hud_state){ 0 }; (void)oot_hud_follow(&g_vrm.follow, heading, 0);
            g_vrm.q = oot_menu_place(head->p, g_vrm.follow.yaw); g_vrm.slide[0] = g_vrm.slide[1] = -1; g_vrm.dirty = 1;
            g_vrm.trig[0].down = trig[0] > 0.4f; g_vrm.trig[1].down = trig[1] > 0.4f;   /* a trigger already down is no press */
            if (runtime_vr_menu_refresh) runtime_vr_menu_refresh();
            g_vrm.asked_at = t;
            VLOG("[vr-menu] placed %.2f m ahead at %.2f %.2f %.2f (the head at %.2f %.2f %.2f, heading %.0f degrees, %s)", (double)OOT_MENU_AHEAD_M,
                 (double)g_vrm.q.c[0], (double)g_vrm.q.c[1], (double)g_vrm.q.c[2], (double)head->p[0], (double)head->p[1], (double)head->p[2],
                 (double)(g_vrm.follow.yaw * 57.29578f), vr_oot_menu_follow() ? "following the head" : "fixed");
        } else if (vr_oot_menu_follow() && level) g_vrm.q = oot_menu_place(head->p, oot_hud_follow(&g_vrm.follow, heading, dt));
        if (runtime_vr_menu_refresh && t - g_vrm.asked_at > 250000000ull) { g_vrm.asked_at = t; runtime_vr_menu_refresh(); }
        uint32_t version = 0;
        if (runtime_vr_menu_rows) (void)runtime_vr_menu_rows(NULL, 0, &version);
        if (version != g_vrm.version && runtime_vr_menu_rows) {
            g_vrm.n = runtime_vr_menu_rows(g_vrm.rows, OOT_MENU_ROWS_MAX, &g_vrm.version);
            for (int i = 0; i < g_vrm.n; i++) g_vrm.kinds[i] = g_vrm.rows[i].kind;
            g_vrm.dirty = 1;
        }
        const int pages = oot_menu_pages(g_vrm.n);
        if (g_vrm.page >= pages) g_vrm.page = pages - 1;
        if (g_vrm.page < 0) g_vrm.page = 0;
        for (int h = 0; h < 2; h++) {
            const oot_menu_hit was = g_vrm.hover[h];
            const int edge = oot_laser_button_step(&g_vrm.trig[h], trig[h], OOT_LASER_TRIGGER_ON_PCT / 100.0f, OOT_LASER_TRIGGER_OFF_PCT / 100.0f);
            float x = 0, y = 0, dist = 0;
            const int hit = ray[h] && oot_laser_hit_rect(o[h], d[h], g_vrm.q.c, g_vrm.q.r, g_vrm.q.u, was.kind != OOT_MENU_HIT_NONE ? 0.02f : 0, 5, &x, &y, &dist);
            g_vrm.hover[h] = hit ? oot_menu_hit_at(x, y, g_vrm.page, g_vrm.n, g_vrm.kinds) : (oot_menu_hit){ OOT_MENU_HIT_NONE, -1, 0 };
            if (g_vrm.slide[h] >= 0) {   /* a slider held: it follows this pointer along its track until the trigger lets go */
                const int i = g_vrm.slide[h];
                if (edge < 0 || !g_vrm.trig[h].down) { VLOG("[vr-menu] row %d: slider let go at %s", i, i < g_vrm.n ? g_vrm.rows[i].value : "?"); g_vrm.slide[h] = -1; g_vrm.dirty = 1; }
                else if (hit && i < g_vrm.n) {
                    const int v = oot_menu_slider_value(g_vrm.rows[i].min, g_vrm.rows[i].max, oot_menu_track_frac(x));
                    if (v != g_vrm.slide_sent[h]) { g_vrm.slide_sent[h] = v; runtime_vr_menu_change(i, OOT_MENU_SET, v); }
                }
            } else if (edge > 0 && hit) vrm_press(h, g_vrm.hover[h]);
            if (g_vrm.hover[h].kind != was.kind || g_vrm.hover[h].row != was.row) {
                g_vrm.dirty = 1;
                if (g_vrm.hover[h].kind != OOT_MENU_HIT_NONE && g_vrm.hover[h].kind != OOT_MENU_HIT_ROW) vr_link_send_haptic(h, 10000000ull, 200, 0.2f);
                if (oot_vrmenu_log_on()) VLOG("[vr-menu] %s hand on %s (row %d) at %.3f %.3f", h ? "right" : "left", vrm_hit_name(g_vrm.hover[h].kind), g_vrm.hover[h].row, (double)x, (double)y);
            }
            if (ray[h]) {
                const float len = hit ? dist : 2.0f;
                g_vrm.ray_on[h] = 1; memcpy(g_vrm.ray_o[h], o[h], 12);
                for (int k = 0; k < 3; k++) g_vrm.ray_end[h][k] = o[h][k] + d[h][k] * (hit ? len - 0.002f : len);
                g_vrm.dot_on[h] = hit;
            }
        }
        if (g_vrm.dirty || !g_vrm_tex) { vrm_render(); g_vrm.dirty = 0; }
        g_vrm.show = g_vrm_tex != nil;
        vrm_laser_textures();
        return;
    }
    g_vrm.slide[0] = g_vrm.slide[1] = -1;
    g_vrm.hover[0] = g_vrm.hover[1] = (oot_menu_hit){ OOT_MENU_HIT_NONE, -1, 0 };
    /* closed: the big screen's drag (row 4), while it is drawn (within 150 ms) and the option is on */
    const int shown = vr_oot_screen_drag() && g_scr.arc_at && t - g_scr.arc_at < 150000000ull;
    int grips_mask = 0;
    if (!shown) {
        if (g_scr.drag.mode != OOT_SCREEN_IDLE) { g_scr.drag.mode = OOT_SCREEN_IDLE; VLOG("[vr-screen] drag ended: the big screen is not shown"); }
        g_scr.grip[0].down = grip[0] > 0.4f; g_scr.grip[1].down = grip[1] > 0.4f;   /* a grip already down when it shows is no press */
        atomic_store(&g_vrm_grips, 0);
        return;
    }
    if (g_scr.drag.mode == OOT_SCREEN_IDLE) g_scr.place = oot_scr_place_now();
    oot_arc_frame base, now_f;
    const oot_screen_place p = g_scr.place;
    if (!oot_arc_frame_make(g.menu_center, g.menu_right, 2.0f, 0, 0, OOT_ARC_RADIUS_M, g_scr.half, g_scr.height, &base) ||
        !oot_arc_frame_make(g.menu_center, g.menu_right, 2.0f, p.yaw_deg, p.up_m, OOT_ARC_RADIUS_M, g_scr.half, g_scr.height, &now_f)) { atomic_store(&g_vrm_grips, 0); return; }
    oot_screen_hands sh; memset(&sh, 0, sizeof sh);
    for (int h = 0; h < 2; h++) {
        sh.have[h] = ray[h];
        if (ray[h]) { memcpy(sh.o[h], o[h], 12); memcpy(sh.d[h], d[h], 12); memcpy(sh.pos[h], hands[h].p, 12); }
        sh.grip[h] = oot_laser_button_step(&g_scr.grip[h], grip[h], 0.55f, 0.35f);
        sh.held[h] = g_scr.grip[h].down;
        float x, y;
        const int on = ray[h] && oot_laser_hit_arc(o[h], d[h], &now_f, g_scr.hover[h] ? 0.02f : 0, &x, &y);
        if (on != g_scr.hover[h] && oot_vrmenu_log_on()) VLOG("[vr-screen] %s laser %s the big screen", h ? "right" : "left", on ? "on" : "off");
        g_scr.hover[h] = on;
        if (on) grips_mask |= 1 << h;
    }
    const int before = g_scr.drag.mode;
    const oot_screen_place at = g_scr.place;
    const int ended = oot_screen_drag_step(&g_scr.drag, &sh, &base, &now_f, &g_scr.place);
    if (before == OOT_SCREEN_IDLE && g_scr.drag.mode == OOT_SCREEN_DRAG)
        VLOG("[vr-screen] taken by the %s grip (turned %.0f degrees, %.2f m up, %.0f degrees wide)", g_scr.drag.hand ? "right" : "left", (double)at.yaw_deg, (double)at.up_m, (double)at.deg);
    if (before == OOT_SCREEN_DRAG && g_scr.drag.mode == OOT_SCREEN_SCALE) VLOG("[vr-screen] both grips: scaling from %.0f degrees", (double)g_scr.place.deg);
    if (g_scr.drag.mode != OOT_SCREEN_IDLE) grips_mask |= 3 & (1 << g_scr.drag.hand | (g_scr.drag.mode == OOT_SCREEN_SCALE ? 1 << (1 - g_scr.drag.hand) : 0));
    if (ended) {
        vr_oot_set_screen_yaw((int)lroundf(g_scr.place.yaw_deg)); vr_oot_set_screen_up((int)lroundf(g_scr.place.up_m * 100)); vr_oot_set_screen_deg((int)lroundf(g_scr.place.deg));
        VLOG("[vr-screen] let go: turned %d degrees, %d cm up, %d degrees wide (saved)", vr_oot_screen_yaw(), vr_oot_screen_up(), vr_oot_screen_deg());
    }
    atomic_store(&g_vrm_grips, grips_mask);
    for (int h = 0; h < 2; h++) if (ray[h] && (g_scr.hover[h] || (g_scr.drag.mode != OOT_SCREEN_IDLE && g_scr.drag.hand == h))) {
        float len = 3.0f;   /* the ray to where it meets the cylinder */
        { const float qx = o[h][0] - now_f.o[0], qz = o[h][2] - now_f.o[2], A = d[h][0] * d[h][0] + d[h][2] * d[h][2];
          const float B = 2 * (qx * d[h][0] + qz * d[h][2]), C = qx * qx + qz * qz - now_f.radius * now_f.radius, disc = B * B - 4 * A * C;
          if (A > 1e-6f && disc >= 0) len = (-B + sqrtf(disc)) / (2 * A) - 0.01f; }
        if (!(len > 0 && len < 9)) continue;
        g_vrm.ray_on[h] = 1; g_vrm.dot_on[h] = g_scr.hover[h]; memcpy(g_vrm.ray_o[h], o[h], 12);
        for (int k = 0; k < 3; k++) g_vrm.ray_end[h][k] = o[h][k] + d[h][k] * len;
        vrm_laser_textures();
    }
}
static void vrm_quad(id<MTLRenderCommandEncoder> e, const float m[16], const float c[3], const float r[3], const float u[3], id<MTLTexture> tex) {
    quad_u q; memcpy(q.m, m, sizeof q.m);
    memcpy(q.c, c, 12); memcpy(q.r, r, 12); memcpy(q.u, u, 12); q.c[3] = q.r[3] = q.u[3] = 0;
    q.uvs[0] = q.uvs[1] = 1; q.uvs[2] = q.uvs[3] = 0;
    const float k[4] = { 1, 0, 0, 0 };
    [e setVertexBytes:&q length:sizeof q atIndex:0]; [e setFragmentBytes:k length:sizeof k atIndex:0];
    [e setFragmentTexture:tex atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
}
/* Over one eye's picture: m takes the room to that eye's clip space; eye is that eye's pose. */
static void oot_vrmenu_draw(id<MTLRenderCommandEncoder> e, const float m[16], const vr_pose *eye) {
    if (!g.ps_pre) return;
    const int any_ray = g_vrm.ray_on[0] || g_vrm.ray_on[1];
    if (!g_vrm.show && !any_ray) return;
    [e setRenderPipelineState:g.ps_pre]; [e setFragmentSamplerState:g.smp atIndex:0];
    if (g_vrm.show && g_vrm_tex) vrm_quad(e, m, g_vrm.q.c, g_vrm.q.r, g_vrm.q.u, g_vrm_tex);
    for (int h = 0; h < 2; h++) {
        if (!g_vrm.ray_on[h] || !g_vrm_ray_tex || !g_vrm_dot_tex) continue;
        const float *a = g_vrm.ray_o[h], *b = g_vrm.ray_end[h];
        const float along[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, mid[3] = { (a[0] + b[0]) / 2, (a[1] + b[1]) / 2, (a[2] + b[2]) / 2 };
        const float to[3] = { eye->p[0] - mid[0], eye->p[1] - mid[1], eye->p[2] - mid[2] };
        float side[3] = { along[1] * to[2] - along[2] * to[1], along[2] * to[0] - along[0] * to[2], along[0] * to[1] - along[1] * to[0] };
        const float sl = sqrtf(side[0] * side[0] + side[1] * side[1] + side[2] * side[2]);
        if (sl > 1e-6f) { for (int k = 0; k < 3; k++) side[k] *= 0.004f / sl; vrm_quad(e, m, mid, along, side, g_vrm_ray_tex); }   /* 4 mm wide, turned to the eye */
        if (!g_vrm.dot_on[h]) continue;
        float f[3] = { b[0] - eye->p[0], b[1] - eye->p[1], b[2] - eye->p[2] }; const float dist = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
        if (dist < 0.05f) continue;
        for (int k = 0; k < 3; k++) f[k] /= dist;
        float right[3] = { -f[2], 0, f[0] }; const float rl = sqrtf(right[0] * right[0] + right[2] * right[2]); if (rl < 1e-3f) continue;
        right[0] /= rl; right[2] /= rl;
        float up[3] = { right[1] * f[2] - right[2] * f[1], right[2] * f[0] - right[0] * f[2], right[0] * f[1] - right[1] * f[0] };
        const float size = 0.012f * dist;   /* about 0.7 degrees across at any distance */
        for (int k = 0; k < 3; k++) { right[k] *= size; up[k] *= size; }
        vrm_quad(e, m, b, right, up, g_vrm_dot_tex);
    }
    [e setRenderPipelineState:g.ps];
}
