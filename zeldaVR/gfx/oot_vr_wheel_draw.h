/* zeldaVR, Ocarina of Time: the item wheel in the headset (zeldaVR/runtime/oot_vr_wheel.h has the list and the stick; the
   game side, games/oot/runtime/oot_vr_wheel_host.h, says what Link owns and puts the chosen item on the item button through
   the game's own call). The left stick pressed + A opens it where the player looks; the right stick pushed toward an item
   highlights it, let back to the middle chooses it; B or the same two buttons close it. Its slots stay as they were when it
   opened. Included by totk_vr.m (GAME_OOT, after the wrist panel). The pad's thread opens, highlights and chooses; the VR
   thread lists, draws and places. The picture is drawn here with CoreGraphics: each slot the game's own icon when the App has it
   (extracted/oot/vr-icons/<name>.png, made at packaging from the player's own RomFS by games/oot/scripts/make_vr_icons.py; a
   bottle the icon of what it holds, vr.md row 86), else its name. */
#import <ImageIO/ImageIO.h>
#include "../runtime/oot_vr_wheel.h"
#include "../runtime/oot_vr_wheel_icons.h"
#include "totk_ui_lang.h"
extern int runtime_oot_vr_wheel(uint8_t owned[OOT_WHEEL_ITEM_COUNT], int *child, uint64_t *age_ns) __attribute__((weak_import));
extern void runtime_oot_vr_wheel_refresh(void) __attribute__((weak_import));
extern void runtime_oot_vr_wheel_select(int item) __attribute__((weak_import));
extern int runtime_oot_vr_wheel_bottle(int item) __attribute__((weak_import));   /* -1 empty, 0..11, OOT_BOTTLE_UNKNOWN */
extern int runtime_oot_vr_hooks_on(void) __attribute__((weak_import));   /* the game side's small hooks are in: it can answer */

/* Option (the VR page): wheel 1 the item wheel on. */
int vr_oot_wheel(void) { return vr_option("wheel", 1) != 0; }
void vr_oot_set_wheel(int on) { vr_option_set("wheel", on != 0); VLOG("the item wheel: %s", on ? "on" : "off"); }
/* Where it opens (option wheel_hand: 0 before the eyes as before, 1 over the right hand, smaller, facing the eyes; HookshotVR offers
   both). */
int vr_oot_wheel_hand(void) { return vr_option("wheel_hand", 0) != 0; }
void vr_oot_set_wheel_hand(int on) { vr_option_set("wheel_hand", on != 0); VLOG("the item wheel opens %s", on ? "over the right hand" : "before the eyes"); }
static const float OOT_WHEEL_HAND_SIZE = 0.40f;

enum { OOT_WHEEL_PX = 1024 };
static const float OOT_WHEEL_SIZE = 0.62f, OOT_WHEEL_AHEAD = 0.72f;   /* metres across, and ahead of the eyes */
static struct {
    pthread_mutex_t mu;
    int open, gear, listed, placed, n, rows[OOT_WHEEL_SLOTS], highlight;
    uint64_t opened_at, asked_at, listed_at;
    oot_wheel_pick pick;
} g_wheel = { .mu = PTHREAD_MUTEX_INITIALIZER, .highlight = -1 };
/* the VR thread's own: */
static id<MTLTexture> g_wheel_tex;
static int g_wheel_show, g_wheel_drawn_n = -1, g_wheel_drawn_light = -2, g_wheel_drawn_rows[OOT_WHEEL_SLOTS], g_wheel_drawn_contents[OOT_WHEEL_SLOTS];
static float g_wheel_center[3], g_wheel_right[3], g_wheel_up[3];

static int oot_wheel_is_open(void) { pthread_mutex_lock(&g_wheel.mu); const int open = g_wheel.open; pthread_mutex_unlock(&g_wheel.mu); return open; }
static void oot_wheel_close(const char *why) {
    pthread_mutex_lock(&g_wheel.mu);
    const int was = g_wheel.open; g_wheel.open = 0;
    pthread_mutex_unlock(&g_wheel.mu);
    if (was) VLOG("item wheel: closed (%s)", why);
}
/* The pad's thread: the left stick pressed + A (gear 0, the items) or + B (gear 1, the equipment; vr.md row 87). */
static void oot_wheel_toggle(int world, int gear) {
    if (oot_wheel_is_open()) { oot_wheel_close("the same buttons"); return; }
    if (!world || !vr_oot_wheel() || !runtime_oot_vr_wheel || !runtime_oot_vr_wheel_refresh || !runtime_oot_vr_wheel_select) return;
    if (!runtime_oot_vr_hooks_on || !runtime_oot_vr_hooks_on()) { static int said; if (!said) { said = 1; VLOG("item wheel: not opened, the game side's hooks are off (TOTK_VR_SWORD_FOLLOW=0)"); } return; }
    pthread_mutex_lock(&g_wheel.mu);
    g_wheel.open = 1; g_wheel.gear = gear; g_wheel.listed = g_wheel.placed = g_wheel.n = 0; g_wheel.highlight = -1; g_wheel.pick = (oot_wheel_pick){ 0, -1 };
    g_wheel.opened_at = g_wheel.asked_at = now_ns();
    pthread_mutex_unlock(&g_wheel.mu);
    runtime_oot_vr_wheel_refresh();
    VLOG("%s wheel: opened", gear ? "equipment" : "item");
}
/* The pad's thread, while it is open: the right stick. */
static void oot_wheel_stick(float x, float y) {
    int item = -1, row = -1, lit = -1, moved = 0;
    pthread_mutex_lock(&g_wheel.mu);
    if (g_wheel.open && g_wheel.listed) {
        const int chosen = oot_wheel_step(&g_wheel.pick, x, y, g_wheel.n);
        moved = g_wheel.pick.slot != g_wheel.highlight && g_wheel.pick.slot >= 0;
        g_wheel.highlight = lit = g_wheel.pick.slot;
        if (chosen >= 0 && chosen < g_wheel.n) { row = g_wheel.rows[chosen]; item = OOT_WHEEL_ITEMS[row].item; g_wheel.open = 0; }
    }
    pthread_mutex_unlock(&g_wheel.mu);
    if (moved) { vr_link_send_haptic(1, 15000000ull, 200, 0.25f); VLOG("item wheel: slot %d highlighted", lit); }
    if (item >= 0) {
        runtime_oot_vr_wheel_select(item);
        vr_link_send_haptic(1, 40000000ull, 200, 0.5f);
        VLOG("item wheel: chose %s (the game's item %d); closed", OOT_WHEEL_ITEMS[row].en, item);
    }
}

static void wheel_label(CGContextRef c, NSString *text, CGFloat size, CGFloat cx, CGFloat cy, int box, int lit) {
    CTFontRef font = CTFontCreateUIFontForLanguage(kCTFontUIFontEmphasizedSystem, size, NULL);
    if (!font) return;
    NSDictionary *attrs = @{ (__bridge id)kCTFontAttributeName: (__bridge id)font, (__bridge id)kCTForegroundColorFromContextAttributeName: @YES };
    NSAttributedString *as = [[NSAttributedString alloc] initWithString:text attributes:attrs];
    CTLineRef line = CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)as);
    if (line) {
        CGFloat ascent = 0, descent = 0; const CGFloat w = (CGFloat)CTLineGetTypographicBounds(line, &ascent, &descent, NULL);
        if (box) {
            const CGRect r = CGRectMake(cx - w / 2 - 22, cy - size * 0.85, w + 44, size * 1.7);
            CGPathRef pill = CGPathCreateWithRoundedRect(r, r.size.height / 2, r.size.height / 2, NULL);
            CGContextAddPath(c, pill);
            if (lit) CGContextSetRGBFillColor(c, 0.96, 0.84, 0.36, 1); else CGContextSetRGBFillColor(c, 0.22, 0.15, 0.10, 0.96);
            CGContextFillPath(c);
            CGContextAddPath(c, pill); CGContextSetRGBStrokeColor(c, 0.72, 0.47, 0.30, 1); CGContextSetLineWidth(c, 3); CGContextStrokePath(c);
            CGPathRelease(pill);
        }
        if (lit) CGContextSetRGBFillColor(c, 0.12, 0.08, 0.05, 1); else CGContextSetRGBFillColor(c, 0.96, 0.92, 0.82, 1);
        CGContextSetTextPosition(c, cx - w / 2, cy - (ascent - descent) / 2); CTLineDraw(line, c); CFRelease(line);
    }
    CFRelease(font);
}
/* The game's icon of this name (extracted/oot/vr-icons/<name>.png), loaded once; NULL when the App has none (logged once). */
static CGImageRef wheel_icon(const char *name) {
    enum { CACHE = 64 };
    static struct { const char *name; CGImageRef image; } cache[CACHE];
    static int cached, said_missing, said_found;
    if (!name) return NULL;
    for (int i = 0; i < cached; i++) if (!strcmp(cache[i].name, name)) return cache[i].image;
    if (cached >= CACHE) return NULL;
    char path[160]; snprintf(path, sizeof path, "extracted/oot/vr-icons/%s.png", name);
    CGImageRef image = NULL;
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), false);
    if (url) {
        CGImageSourceRef source = CGImageSourceCreateWithURL(url, NULL);
        CFRelease(url);
        if (source) { if (CGImageSourceGetCount(source) > 0) image = CGImageSourceCreateImageAtIndex(source, 0, NULL); CFRelease(source); }
    }
    cache[cached].name = name; cache[cached].image = image; cached++;
    if (!image && !said_missing) { said_missing = 1; VLOG("[vr-wheel] no icon %s: such slots show their names (said once)", path); }
    if (image && !said_found) { said_found = 1; VLOG("[vr-wheel] the game's icons from extracted/oot/vr-icons (%zux%zu)", CGImageGetWidth(image), CGImageGetHeight(image)); }
    return image;
}
/* One slot as the game's icon on a round plate (lit: the gold one). */
static void wheel_icon_slot(CGContextRef c, CGImageRef icon, CGFloat cx, CGFloat cy, CGFloat radius, int lit) {
    const CGRect plate = CGRectMake(cx - radius, cy - radius, 2 * radius, 2 * radius);
    if (lit) CGContextSetRGBFillColor(c, 0.96, 0.84, 0.36, 1); else CGContextSetRGBFillColor(c, 0.22, 0.15, 0.10, 0.96);
    CGContextFillEllipseInRect(c, plate);
    CGContextSetRGBStrokeColor(c, 0.72, 0.47, 0.30, 1); CGContextSetLineWidth(c, 3); CGContextStrokeEllipseInRect(c, plate);
    const CGFloat side = radius * 1.6f;
    CGContextSetInterpolationQuality(c, kCGInterpolationHigh);
    CGContextDrawImage(c, CGRectMake(cx - side / 2, cy - side / 2, side, side), icon);
}
/* The wheel's picture for these slots (rows of OOT_WHEEL_ITEMS, contents[k]: a bottle's content), and this highlighted one (-1
   none), into the texture. */
static void wheel_render(const int rows[], const int contents[], int n, int lit) {
    enum { N = OOT_WHEEL_PX }; static uint8_t px[N * N * 4];
    CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB();
    CGContextRef c = CGBitmapContextCreate(px, N, N, 8, N * 4, rgb, (CGBitmapInfo)kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    CGColorSpaceRelease(rgb);
    if (!c) return;
    const int zh = totk_ui_zh();
    CGContextClearRect(c, CGRectMake(0, 0, N, N));
    CGContextSetRGBFillColor(c, 0.10, 0.07, 0.05, 0.78); CGContextFillEllipseInRect(c, CGRectMake(8, 8, N - 16, N - 16));
    CGContextSetRGBStrokeColor(c, 0.72, 0.47, 0.30, 1); CGContextSetLineWidth(c, 6); CGContextStrokeEllipseInRect(c, CGRectMake(8, 8, N - 16, N - 16));
    char name[128];
    for (int k = 0; k < n; k++) {   /* more than eight: every other one on an inner ring, so that neighbours do not cover each other */
        float at[2]; oot_wheel_place(k, n, n > 8 && (k & 1) ? 250.0f : n > 8 ? 345.0f : 300.0f, at);
        const oot_wheel_item *it = &OOT_WHEEL_ITEMS[rows[k]];
        CGImageRef icon = wheel_icon(oot_wheel_icon(it, contents[k]));
        if (icon) wheel_icon_slot(c, icon, N / 2 + at[0], N / 2 + at[1], n > 8 ? 60 : 78, k == lit);
        else { oot_wheel_label(it, contents[k], zh, name, sizeof name); wheel_label(c, @(name), zh ? 34 : 27, N / 2 + at[0], N / 2 + at[1], 1, k == lit); }
    }
    if (n <= 0) wheel_label(c, zh ? @"这里没有可以选的东西" : @"Nothing to choose here yet", 34, N / 2, N / 2, 0, 0);
    else if (lit >= 0 && lit < n) { oot_wheel_label(&OOT_WHEEL_ITEMS[rows[lit]], contents[lit], zh, name, sizeof name); wheel_label(c, @(name), n > 8 ? 40 : 50, N / 2, N / 2, 0, 0); }
    else {
        wheel_label(c, zh ? @"右摇杆指向道具" : @"Right stick: point at an item", 30, N / 2, N / 2 + 26, 0, 0);
        wheel_label(c, zh ? @"松开选择 · B 关闭" : @"let go to choose · B closes", 30, N / 2, N / 2 - 26, 0, 0);
    }
    CGContextRelease(c);
    if (!g_wheel_tex) {
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:N height:N mipmapped:NO];
        d.usage = MTLTextureUsageShaderRead; d.storageMode = MTLStorageModeShared; g_wheel_tex = [g.dev newTextureWithDescriptor:d];
    }
    if (g_wheel_tex) [g_wheel_tex replaceRegion:MTLRegionMake2D(0, 0, N, N) mipmapLevel:0 withBytes:px bytesPerRow:N * 4];
}

/* Once a headset frame. world: the game's world camera is running. */
static void oot_wheel_update(const vr_pose *head, int world) {
    const uint64_t t = now_ns();
    int rows[OOT_WHEEL_SLOTS], contents[OOT_WHEEL_SLOTS], n = 0, lit = -1, ask = 0, listed = 0; const char *close = NULL;
    g_wheel_show = 0;
    pthread_mutex_lock(&g_wheel.mu);
    if (g_wheel.open) {
        if (!world) close = "the world's view ended";
        else if (!vr_oot_wheel()) close = "switched off";
        else if (g_wheel.listed && !g_wheel.n && t - g_wheel.listed_at > 2000000000ull) close = "nothing to choose";
        else if (!g_wheel.listed && t - g_wheel.opened_at > 3000000000ull) close = "the game did not answer in 3 s";
    }
    if (g_wheel.open && !close) {
        if (!g_wheel.placed) {   /* where the player looks now, upright, a hand under the eyes */
            const float ahead0[3] = { 0, 0, -1 }; float f[3]; qrot(head->q, ahead0, f);
            float len = sqrtf(f[0] * f[0] + f[2] * f[2]);
            if (len < 0.2f) {   /* looking straight up or down: the way the top of the head or the chin points */
                const float up0[3] = { 0, 1, 0 }; const float sign = f[1] > 0 ? -1.0f : 1.0f; qrot(head->q, up0, f);
                f[0] *= sign; f[2] *= sign; len = sqrtf(f[0] * f[0] + f[2] * f[2]);
            }
            if (len < 1e-3f) { f[0] = 0; f[2] = -1; len = 1; }
            f[0] /= len; f[2] /= len;
            float size = OOT_WHEEL_SIZE;
            g_wheel_center[0] = head->p[0] + f[0] * OOT_WHEEL_AHEAD; g_wheel_center[1] = head->p[1] - 0.10f; g_wheel_center[2] = head->p[2] + f[2] * OOT_WHEEL_AHEAD;
            vr_hand hands[2]; vr_oot_hands(hands);
            if (vr_oot_wheel_hand() && hands[1].have) {   /* over the right hand, upright, turned to the eyes */
                const float to[3] = { hands[1].p[0] - head->p[0], 0, hands[1].p[2] - head->p[2] }; const float l = sqrtf(to[0] * to[0] + to[2] * to[2]);
                if (l > 0.05f) { f[0] = to[0] / l; f[2] = to[2] / l; }
                size = OOT_WHEEL_HAND_SIZE;
                g_wheel_center[0] = hands[1].p[0]; g_wheel_center[1] = hands[1].p[1] + 0.06f + size / 2; g_wheel_center[2] = hands[1].p[2];
            }
            g_wheel_right[0] = -f[2] * size; g_wheel_right[1] = 0; g_wheel_right[2] = f[0] * size;
            g_wheel_up[0] = 0; g_wheel_up[1] = size; g_wheel_up[2] = 0;
            g_wheel.placed = 1;
        }
        if (!g_wheel.listed) {   /* the game's answer, newer than the opening */
            uint8_t owned[OOT_WHEEL_ITEM_COUNT]; int child = 1; uint64_t age = 0;
            if (runtime_oot_vr_wheel(owned, &child, &age) && age < t - g_wheel.opened_at) {
                g_wheel.n = oot_wheel_list(owned, child, g_wheel.gear, g_wheel.rows); g_wheel.listed = listed = 1; g_wheel.listed_at = t;
            } else if (t - g_wheel.asked_at > 250000000ull) { g_wheel.asked_at = t; ask = 1; }
        }
        if (g_wheel.listed) { n = g_wheel.n; memcpy(rows, g_wheel.rows, sizeof rows); lit = g_wheel.highlight; g_wheel_show = 1; }
    }
    pthread_mutex_unlock(&g_wheel.mu);
    if (close) { oot_wheel_close(close); return; }
    if (ask) runtime_oot_vr_wheel_refresh();
    for (int k = 0; k < n; k++)   /* a bottle's content, from the same answer as the list */
        contents[k] = oot_wheel_is_bottle(OOT_WHEEL_ITEMS[rows[k]].item) && runtime_oot_vr_wheel_bottle ? runtime_oot_vr_wheel_bottle(OOT_WHEEL_ITEMS[rows[k]].item) : OOT_BOTTLE_UNKNOWN;
    if (listed) {
        char text[640] = "", name[96]; int at = 0;
        for (int k = 0; k < n && at < 560; k++) {
            oot_wheel_label(&OOT_WHEEL_ITEMS[rows[k]], contents[k], 0, name, sizeof name);
            at += snprintf(text + at, sizeof text - (size_t)at, "%s%s", k ? ", " : "", name);
        }
        VLOG("item wheel: %d slots: %s (at %.2f %.2f %.2f, the head at %.2f %.2f %.2f)", n, n ? text : "none", (double)g_wheel_center[0], (double)g_wheel_center[1],
             (double)g_wheel_center[2], (double)head->p[0], (double)head->p[1], (double)head->p[2]);
    }
    if (g_wheel_show && (!g_wheel_tex || n != g_wheel_drawn_n || lit != g_wheel_drawn_light || memcmp(rows, g_wheel_drawn_rows, sizeof(int) * (size_t)n)
                         || memcmp(contents, g_wheel_drawn_contents, sizeof(int) * (size_t)n))) {
        wheel_render(rows, contents, n, lit);
        g_wheel_drawn_n = n; g_wheel_drawn_light = lit; memcpy(g_wheel_drawn_rows, rows, sizeof(int) * (size_t)n); memcpy(g_wheel_drawn_contents, contents, sizeof(int) * (size_t)n);
    }
}
/* Over one eye's picture: m takes the room to that eye's clip space. */
static void oot_wheel_draw(id<MTLRenderCommandEncoder> e, const float m[16]) {
    if (!g_wheel_show || !g_wheel_tex || !g.ps_pre) return;
    quad_u u; memcpy(u.m, m, sizeof u.m);
    memcpy(u.c, g_wheel_center, 12); memcpy(u.r, g_wheel_right, 12); memcpy(u.u, g_wheel_up, 12); u.c[3] = u.r[3] = u.u[3] = 0;
    u.uvs[0] = u.uvs[1] = 1; u.uvs[2] = u.uvs[3] = 0;
    const float k[4] = { 1, 0, 0, 0 };
    [e setRenderPipelineState:g.ps_pre]; [e setVertexBytes:&u length:sizeof u atIndex:0]; [e setFragmentBytes:k length:sizeof k atIndex:0];
    [e setFragmentTexture:g_wheel_tex atIndex:0]; [e setFragmentSamplerState:g.smp atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    [e setRenderPipelineState:g.ps];
}
