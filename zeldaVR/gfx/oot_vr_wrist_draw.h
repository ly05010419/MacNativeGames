/* zeldaVR, Ocarina of Time: the wrist panel (hearts, magic, rupees on the back of the wrist the sword is not in, shown
   while the player looks at it as at a watch: zeldaVR/runtime/oot_vr_wrist.h) and the comfort ring (a dark ring that
   narrows the view while it moves in a way the head did not: oot_vr_comfort.h), drawn over each eye's picture.
   Included by totk_vr.m (GAME_OOT, after the HUD panel); the VR thread only. The values are the game's own saved flags,
   read by the runtime (games/oot/runtime/oot_vr.h runtime_oot_vr_status), so the panel does not need the game's 2D HUD.
   The picture is drawn here with CoreGraphics; nothing of the game's art is used. */
#import <CoreGraphics/CoreGraphics.h>
#import <CoreText/CoreText.h>
#include "../runtime/oot_gamedata.h"
#include "../runtime/oot_vr_wrist.h"
#include "../runtime/oot_vr_comfort.h"
extern int runtime_oot_vr_status(oot_status *out) __attribute__((weak_import));
extern int runtime_oot_vr_feet(float out[3], uint64_t *age_ns) __attribute__((weak_import));
extern int runtime_oot_vr_event_kind(void) __attribute__((weak_import));
extern float runtime_oot_vr_game_pitch(void) __attribute__((weak_import));
extern void runtime_oot_vr_aim_want(void) __attribute__((weak_import));   /* games/oot/runtime/oot_vr_aim.h: the game's own answer */
extern int runtime_oot_vr_aim_point(float point[3], float start[3], uint64_t *age_ns) __attribute__((weak_import));

/* Options (the VR page): wrist 1 the panel on; vig_move / vig_turn / vig_air / vig_carry 0 off, 1 low, 2 medium, 3 high. */
int vr_oot_wrist(void) { const char *e = getenv("TOTK_VR_WRIST"); return e ? !strcmp(e, "1") : vr_option("wrist", 1) != 0; }
void vr_oot_set_wrist(int on) { vr_option_set("wrist", on != 0); VLOG("the wrist panel: %s", on ? "on" : "off"); }
/* Its size (option wrist_size: 0 small 10 cm, 1 medium 13 cm as before, 2 large 16 cm wide) and which wrist (option wrist_any: 0 the
   one the sword is not in, as before; 1 either, the one looked at): HookshotVR has three sizes and either wrist. */
int vr_oot_wrist_size(void) { const int v = vr_option("wrist_size", 1); return v < 0 ? 0 : v > 2 ? 2 : v; }
void vr_oot_set_wrist_size(int v) { vr_option_set("wrist_size", v < 0 ? 0 : v > 2 ? 2 : v); VLOG("the wrist panel's size: %d", vr_oot_wrist_size()); }
int vr_oot_wrist_any(void) { return vr_option("wrist_any", 0) != 0; }
void vr_oot_set_wrist_any(int on) { vr_option_set("wrist_any", on != 0); VLOG("the wrist panel: %s", on ? "either wrist" : "the wrist without the sword"); }
static const char *const OOT_VIG_OPTION[OOT_COMFORT_CAUSES] = { "vig_move", "vig_turn", "vig_air", "vig_carry" };
int vr_oot_vignette(int cause) {
    if (cause < 0 || cause >= OOT_COMFORT_CAUSES) return 0;
    const char *e = getenv("TOTK_VR_VIGNETTE");   /* a test run: one level for every cause */
    const int v = e ? atoi(e) : vr_option(OOT_VIG_OPTION[cause], 0);
    return v < 0 ? 0 : v > 3 ? 3 : v;
}
void vr_oot_set_vignette(int cause, int level) {
    if (cause < 0 || cause >= OOT_COMFORT_CAUSES) return;
    vr_option_set(OOT_VIG_OPTION[cause], level < 0 ? 0 : level > 3 ? 3 : level);
    VLOG("comfort ring, cause %d: level %d", cause, vr_oot_vignette(cause));
}

/* Scenes of the game (how each is shown, the lighting, the re-align) and the ocarina in the hands: oot_vr_scene_draw.h. The
   wrist panel's dungeon row: oot_vr_dungeon_draw.h. */
#include "oot_vr_scene_draw.h"
#include "oot_vr_dungeon_draw.h"

/* Stereo close to the eyes (option near_depth: 0 low, 1 medium, 2 full, the default): how far the depth stereo may shift what
   is near. Low keeps the gaps beside the player's own hands and sword small, with less depth on them. */
int vr_oot_near_depth(void) { const int v = vr_option("near_depth", 2); return v < 0 ? 0 : v > 2 ? 2 : v; }
void vr_oot_set_near_depth(int v) { vr_option_set("near_depth", v < 0 ? 0 : v > 2 ? 2 : v); VLOG("stereo close to the eyes: %s", v <= 0 ? "low" : v == 1 ? "medium" : "full"); }

/* The aim's mark (option reticle, on by default) while the game aims the slingshot or the bow: in first person 10 m along the line
   from the hand that pulls to the hand that holds it, which is where the shot now goes (oot_vr_bow.h oot_shot_steer); in third
   person 8 m along the game's own camera, whose up and down the levelled VR view does not show and where the game's flat mark
   on the HUD panel is not. */
int vr_oot_reticle(void) { return vr_option("reticle", 1) != 0; }
void vr_oot_set_reticle(int on) { vr_option_set("reticle", on != 0); VLOG("the aim's mark: %s", on ? "on" : "off"); }
/* Its size, % of the 2.7 degrees it had (option reticle_size, 100; HookshotVR's is sized on its VR page too). */
int vr_oot_reticle_size(void) { const int v = vr_option("reticle_size", 100); return v < 40 ? 40 : v > 300 ? 300 : v; }
void vr_oot_set_reticle_size(int v) { vr_option_set("reticle_size", v < 40 ? 40 : v > 300 ? 300 : v); VLOG("the aim's mark: %d%%", vr_oot_reticle_size()); }
static id<MTLTexture> g_reticle_tex; static int g_reticle_on; static float g_reticle_at[3];
static void reticle_render(void) {
    enum { N = 128 }; static uint8_t px[N * N * 4];
    CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB();
    CGContextRef c = CGBitmapContextCreate(px, N, N, 8, N * 4, rgb, (CGBitmapInfo)kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    CGColorSpaceRelease(rgb);
    if (!c) return;
    CGContextClearRect(c, CGRectMake(0, 0, N, N));
    CGContextSetRGBStrokeColor(c, 0.10, 0.07, 0.05, 0.9); CGContextSetLineWidth(c, 14); CGContextStrokeEllipseInRect(c, CGRectMake(20, 20, N - 40, N - 40));
    CGContextSetRGBStrokeColor(c, 0.96, 0.84, 0.36, 1); CGContextSetLineWidth(c, 7); CGContextStrokeEllipseInRect(c, CGRectMake(20, 20, N - 40, N - 40));
    CGContextSetRGBFillColor(c, 0.96, 0.84, 0.36, 1); CGContextFillEllipseInRect(c, CGRectMake(N / 2 - 6, N / 2 - 6, 12, 12));
    CGContextRelease(c);
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:N height:N mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead; d.storageMode = MTLStorageModeShared; g_reticle_tex = [g.dev newTextureWithDescriptor:d];
    if (g_reticle_tex) [g_reticle_tex replaceRegion:MTLRegionMake2D(0, 0, N, N) mipmapLevel:0 withBytes:px bytesPerRow:N * 4];
}
static void oot_reticle_say(int mode, const vr_pose *head, float pitch) {   /* 0 none, 1 along the hands, 2 along the game's camera, 3 aiming but no line, 4 where the game's shot lands, 5 the boomerang's line */
    static int was = -1;
    if (mode == was) return;
    was = mode;
    VLOG("the aim's mark: %s, at %.2f %.2f %.2f (the head at %.2f %.2f %.2f; the game's camera looks %.1f degrees up)", mode == 0 ? "off" : mode == 1 ? "along the two hands" :
         mode == 2 ? "along the game's camera" : mode == 4 ? "where the game's own shot would land" : mode == 5 ? "along the boomerang's line" : "aiming, no line to show", (double)g_reticle_at[0], (double)g_reticle_at[1], (double)g_reticle_at[2],
         (double)head->p[0], (double)head->p[1], (double)head->p[2], (double)(pitch * 57.29578f));
}
/* The game's own flat reticle is hidden (vr.md row 61; games/oot/third_party/d2r_gfx/gfx/nvn/oot_vr_reticle_tex.inc) in first person
   while the bow or the slingshot is out or aimed, and in third person while our own reticle shows, these last 250 ms (HookshotVR's
   rule, aim-items.md 1.8). Read by the replay thread. */
static _Atomic uint64_t g_flat_reticle_hide_at;
int vr_oot_flat_reticle_hidden(void) { const uint64_t at = atomic_load(&g_flat_reticle_hide_at); return at && now_ns() - at < 250000000ull; }
extern int runtime_oot_vr_boomerang_aim(int *hand) __attribute__((weak_import));   /* games/oot/runtime/oot_vr_hand_items.h */
/* The boomerang aimed in first person (vr.md row 64): the mark 10 m along the line it will fly through, the hand's pointing line
   (the hand's -z: the controller's grip -z turned by the wrist calibration and the hand tilt, vr_oot_hands, as the game side's) or
   the headset's forward. */
static int oot_reticle_boomerang(const vr_pose *head) {
    int hand = -1;
    if (!runtime_oot_vr_boomerang_aim || !runtime_oot_vr_boomerang_aim(&hand)) return 0;
    const float along[3] = { 0, 0, -1 }, ahead[3] = { 0, 0, -1 };   /* (the tilt is in the hand's orientation: vr_oot_hands) */
    float d[3]; const float *from = head->p;
    if (hand == 0 || hand == 1) {
        vr_hand h[2]; vr_oot_hands(h);
        if (!h[hand].have) return 0;
        qrot(h[hand].q, along, d); from = h[hand].p;
        for (int i = 0; i < 3; i++) g_reticle_at[i] = from[i] + d[i] * 10.05f;
    } else { qrot(head->q, ahead, d); for (int i = 0; i < 3; i++) g_reticle_at[i] = from[i] + d[i] * 10.0f; }
    return 1;
}
static void oot_reticle_update(const vr_pose *head, int world) {
    g_reticle_on = 0;
    const int first_person = runtime_oot_vr_fp_active && runtime_oot_vr_fp_active();
    if (world && first_person && (vr_oot_aiming() || (runtime_oot_vr_bow_out && runtime_oot_vr_bow_out() > 0))) atomic_store(&g_flat_reticle_hide_at, now_ns());
    if (world && vr_oot_reticle() && runtime_oot_vr_fp_active && runtime_oot_vr_fp_active() && oot_reticle_boomerang(head)) {
        if (!g_reticle_tex) reticle_render();
        g_reticle_on = g_reticle_tex != nil;
        oot_reticle_say(5, head, 0);
        return;
    }
    if (!world || !vr_oot_aiming() || !vr_oot_reticle()) { oot_reticle_say(0, head, 0); return; }
    int mode = 3; float pitch = 0;
    if (first_person) {
        vr_hand h[2]; vr_oot_hands(h);
        const int drawn_in = oot_bow_hand_drawn();   /* the hand the bow is drawn in (oot_vr_bow_ctl.h), else the left */
        const int bow = drawn_in == 0 || drawn_in == 1 ? drawn_in : vr_oot_left_handed() ? 1 : 0, pull = 1 - bow;
        if (h[0].have && h[1].have) {
            const float d[3] = { h[bow].p[0] - h[pull].p[0], h[bow].p[1] - h[pull].p[1], h[bow].p[2] - h[pull].p[2] }, len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (len >= 0.14f && len <= 0.95f) { for (int i = 0; i < 3; i++) g_reticle_at[i] = h[bow].p[i] + d[i] / len * 10.0f; mode = 1; }
        }
    } else if (runtime_oot_vr_game_pitch && g.hl_have_ref) {
        pitch = runtime_oot_vr_game_pitch();
        const float along[3] = { 0, sinf(pitch), -cosf(pitch) }; float dir[3];
        qrot(g.hl_ref, along, dir);
        for (int i = 0; i < 3; i++) g_reticle_at[i] = head->p[i] + dir[i] * 8.0f;
        mode = 2;
        /* third person: where the game says its shot would go (oot_vr_aim.h), through the game's own camera to the room, if the answer
           is fresh, ahead of the camera and within its reach (HookshotVR: 70 m) */
        if (runtime_oot_vr_aim_want) runtime_oot_vr_aim_want();
        float w[3], s[3], bv[12]; uint64_t age = 0;
        if (runtime_oot_vr_aim_point && runtime_oot_vr_aim_point(w, s, &age) && age < 150000000ull && runtime_vr_base_view(bv)) {
            float cam[3], room[3];
            for (int i = 0; i < 3; i++) cam[i] = bv[4 * i] * w[0] + bv[4 * i + 1] * w[1] + bv[4 * i + 2] * w[2] + bv[4 * i + 3];
            const float d = sqrtf(cam[0] * cam[0] + cam[1] * cam[1] + cam[2] * cam[2]);
            if (cam[2] < 0 && d > 0.5f && d <= 70.0f) { qrot(g.hl_ref, cam, room); for (int i = 0; i < 3; i++) g_reticle_at[i] = head->p[i] + room[i]; mode = 4; }
        }
    }
    if (mode != 3 && !g_reticle_tex) reticle_render();
    g_reticle_on = mode != 3 && g_reticle_tex != nil;
    if (g_reticle_on && !first_person) atomic_store(&g_flat_reticle_hide_at, now_ns());   /* third person: ours stands in for the game's */
    oot_reticle_say(mode, head, pitch);
}

/* What the pad's thread saw since the VR thread last asked: the left stick's push and how far it turned the view. */
static _Atomic uint32_t g_oot_push_bits, g_oot_turned_bits;
static void oot_comfort_note(float push, float turned) {
    uint32_t b; memcpy(&b, &push, 4); atomic_store(&g_oot_push_bits, b);
    if (push > 0.2f) atomic_store(&g_oot_stick_at, now_ns());   /* the re-align leaves out the turns the player made (oot_vr_scene_draw.h) */
    if (turned != 0) { uint32_t have = atomic_load(&g_oot_turned_bits), want; float f;
        do { memcpy(&f, &have, 4); f += fabsf(turned); memcpy(&want, &f, 4); } while (!atomic_compare_exchange_weak(&g_oot_turned_bits, &have, want)); }
}

enum { OOT_WRIST_W = 512, OOT_WRIST_H = 352 };   /* the board's width; its height is what its rows need, this at most */
static id<MTLTexture> g_wrist_tex; static int g_wrist_h = OOT_WRIST_H;
/* The board's height for these values: one or two rows of hearts, the magic bar when there is magic, the rupees. */
static int wrist_height(const oot_status *s) {
    const int rows = oot_heart_count(s->max_life) > 10 ? 2 : 1, h = 30 + rows * 46 + (s->max_magic > 0 ? 50 : 20) + 64 + (g_wrist_dungeon.dungeon >= 0 ? OOT_WRIST_DUNGEON_H : 0);
    return h > OOT_WRIST_H ? OOT_WRIST_H : h;
}
static oot_status g_wrist_status, g_wrist_drawn;
static oot_wrist_state g_wrist_state; static oot_wrist_quad g_wrist_quad; static float g_wrist_shown;
static oot_comfort g_comfort; static oot_vignette g_vignette;
static uint64_t g_wrist_at, g_wrist_read_at, g_wrist_hurt_at; static int g_wrist_hand = -1;

static void wrist_heart_path(CGContextRef c, CGFloat x, CGFloat y, CGFloat w) {   /* in a w x w box whose lower left is (x, y) */
    CGContextBeginPath(c);
    CGContextAddEllipseInRect(c, CGRectMake(x + w * 0.02, y + w * 0.40, w * 0.54, w * 0.54));
    CGContextAddEllipseInRect(c, CGRectMake(x + w * 0.44, y + w * 0.40, w * 0.54, w * 0.54));
    CGContextMoveToPoint(c, x + w * 0.055, y + w * 0.56); CGContextAddLineToPoint(c, x + w * 0.945, y + w * 0.56);
    CGContextAddLineToPoint(c, x + w * 0.5, y + w * 0.06); CGContextClosePath(c);
}
static void wrist_text(CGContextRef c, NSString *text, CGFloat size, CGFloat x, CGFloat y) {
    CTFontRef font = CTFontCreateUIFontForLanguage(kCTFontUIFontEmphasizedSystem, size, NULL);
    if (!font) return;
    NSDictionary *attrs = @{ (__bridge id)kCTFontAttributeName: (__bridge id)font, (__bridge id)kCTForegroundColorFromContextAttributeName: @YES };
    NSAttributedString *as = [[NSAttributedString alloc] initWithString:text attributes:attrs];
    CTLineRef line = CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)as);
    if (line) { CGContextSetTextPosition(c, x, y); CTLineDraw(line, c); CFRelease(line); }
    CFRelease(font);
}
/* The panel's picture for these values, into the texture (premultiplied BGRA). */
static void wrist_render(const oot_status *s) {
    static uint8_t px[OOT_WRIST_W * OOT_WRIST_H * 4];
    CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB();
    CGContextRef c = CGBitmapContextCreate(px, OOT_WRIST_W, OOT_WRIST_H, 8, OOT_WRIST_W * 4, rgb, (CGBitmapInfo)kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    CGColorSpaceRelease(rgb);
    if (!c) return;
    const CGFloat W = OOT_WRIST_W, H = wrist_height(s);   /* drawn in the upper part of the bitmap, which is the texture */
    CGContextClearRect(c, CGRectMake(0, 0, W, OOT_WRIST_H));
    CGContextTranslateCTM(c, 0, OOT_WRIST_H - H);
    CGPathRef board = CGPathCreateWithRoundedRect(CGRectMake(4, 4, W - 8, H - 8), 34, 34, NULL);
    CGContextAddPath(c, board); CGContextSetRGBFillColor(c, 0.10, 0.07, 0.05, 0.90); CGContextFillPath(c);
    CGContextAddPath(c, board); CGContextSetRGBStrokeColor(c, 0.72, 0.47, 0.30, 1); CGContextSetLineWidth(c, 6); CGContextStrokePath(c);
    CGPathRelease(board);
    const int hearts = oot_heart_count(s->max_life), rows = hearts > 10 ? 2 : 1;
    const CGFloat hw = 42, gap = 4, left = (W - 10 * hw - 9 * gap) / 2;
    for (int i = 0; i < hearts && i < 20; i++) {
        const CGFloat x = left + (i % 10) * (hw + gap), y = H - 30 - hw - (i / 10) * (hw + 4);
        wrist_heart_path(c, x, y, hw); CGContextSetRGBFillColor(c, 0.20, 0.11, 0.07, 1); CGContextFillPath(c);
        const int q = oot_heart_quarters(s->life, i);
        if (q) {
            CGContextSaveGState(c); wrist_heart_path(c, x, y, hw); CGContextClip(c);
            CGContextSetRGBFillColor(c, 0.86, 0.22, 0.20, 1); CGContextFillRect(c, CGRectMake(x, y, hw * q / 4, hw));
            CGContextRestoreGState(c);
        }
    }
    CGFloat y = H - 30 - rows * (hw + 4) - 30;
    if (s->max_magic > 0) {   /* the magic bar */
        const CGRect bar = CGRectMake(left, y, 10 * hw + 9 * gap, 20);
        CGContextSetRGBFillColor(c, 0.07, 0.14, 0.09, 1); CGContextFillRect(c, bar);
        CGContextSetRGBFillColor(c, 0.25, 0.80, 0.38, 1); CGContextFillRect(c, CGRectMake(bar.origin.x, bar.origin.y, bar.size.width * s->magic / s->max_magic, bar.size.height));
        y -= 30;
    }
    {   /* rupees: a green gem and the count */
        const CGFloat gx = left + 16, gy = y - 34;
        CGContextBeginPath(c); CGContextMoveToPoint(c, gx, gy + 46); CGContextAddLineToPoint(c, gx + 14, gy + 34); CGContextAddLineToPoint(c, gx + 14, gy + 12);
        CGContextAddLineToPoint(c, gx, gy); CGContextAddLineToPoint(c, gx - 14, gy + 12); CGContextAddLineToPoint(c, gx - 14, gy + 34); CGContextClosePath(c);
        CGContextSetRGBFillColor(c, 0.20, 0.78, 0.32, 1); CGContextFillPath(c);
        CGContextSetRGBFillColor(c, 0.96, 0.92, 0.82, 1);
        wrist_text(c, [NSString stringWithFormat:@"%d", s->rupees], 44, gx + 30, gy + 6);
        wrist_dungeon_draw(c, &g_wrist_dungeon, left, gy - OOT_WRIST_DUNGEON_H + 4);   /* in a dungeon: keys, boss key, map, compass */
    }
    CGContextRelease(c);
    const int tall = (int)H;
    if (!g_wrist_tex || (int)g_wrist_tex.height != tall) {
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:OOT_WRIST_W height:(NSUInteger)tall mipmapped:NO];
        d.usage = MTLTextureUsageShaderRead; d.storageMode = MTLStorageModeShared; g_wrist_tex = [g.dev newTextureWithDescriptor:d];
    }
    g_wrist_h = tall;
    if (g_wrist_tex) [g_wrist_tex replaceRegion:MTLRegionMake2D(0, 0, OOT_WRIST_W, (NSUInteger)tall) mipmapLevel:0 withBytes:px bytesPerRow:OOT_WRIST_W * 4];
}

/* Once a headset frame: is the wrist looked at, what the ring is to show. world: the game's world camera is running. */
static void oot_wrist_update(const vr_pose *head, int world) {
    const uint64_t t = now_ns(); const float dt = g_wrist_at ? (float)((t - g_wrist_at) / 1e9) : 0; g_wrist_at = t;
    {   /* the comfort ring: first person only */
        float feet[3]; uint64_t age = 0; uint32_t b = atomic_load(&g_oot_push_bits); float push, turned;
        memcpy(&push, &b, 4); b = atomic_exchange(&g_oot_turned_bits, 0); memcpy(&turned, &b, 4);
        const int fp = world && runtime_oot_vr_fp_active && runtime_oot_vr_fp_active();
        const int have = fp && runtime_oot_vr_feet && runtime_oot_vr_feet(feet, &age) && age < 150000000ull;
        const int causes = oot_comfort_step(&g_comfort, t - (have ? age : 0), have ? feet : NULL, push, turned);
        int levels[OOT_COMFORT_CAUSES]; for (int i = 0; i < OOT_COMFORT_CAUSES; i++) levels[i] = vr_oot_vignette(i);
        const int level = fp ? oot_comfort_level(causes, levels) : 0;
        static int said = -1; if (level != said) { said = level; VLOG("comfort ring: level %d (causes %x)", level, causes); }
        oot_vignette_step(&g_vignette, level, dt);
    }
    oot_scene_update(head, world);
    oot_reticle_update(head, world);
    { static const int px[3] = { 24, 40, 64 }; vr_ds_set_max_shift(px[vr_oot_near_depth()]); }
    g_wrist_shown = 0;
    if (!vr_oot_wrist() || !runtime_oot_vr_status) return;
    if (t - g_wrist_read_at > 200000000ull) {
        g_wrist_read_at = t; if (!runtime_oot_vr_status(&g_wrist_status)) g_wrist_status.have = 0;
        if (!runtime_oot_vr_dungeon || !runtime_oot_vr_dungeon(&g_wrist_dungeon)) g_wrist_dungeon.dungeon = -1;
    }
    vr_hand h[2]; vr_oot_hands(h);
    /* hurt: life went down since the last read; no panel for 3 s after (HookshotVR's "in a fight") */
    { static int life = -1; if (g_wrist_status.have) { if (life >= 0 && g_wrist_status.life < life) g_wrist_hurt_at = t; life = g_wrist_status.life; } }
    const float widths[3] = { 0.10f, 0.13f, 0.16f }, width = widths[vr_oot_wrist_size()];
    const int sword = vr_oot_left_handed() ? 0 : 1, any = vr_oot_wrist_any();
    oot_wrist_in in = { 9, 180, 180, 9, 1 };
    oot_wrist_quad quad = g_wrist_quad; int chosen = -1;
    for (int k = 0; k < 2; k++) {   /* the arm the sword is not on; with either wrist the other too, the one looked at better */
        if (k == sword && !any) continue;
        if (!(world && g_wrist_status.have && h[k].have)) continue;
        if (any && g_wrist_state.on && g_wrist_hand >= 0 && k != g_wrist_hand) continue;   /* shown: it stays on its wrist */
        const float back0[3] = { k ? 1.f : -1.f, 0, 0 }, fingers0[3] = { 0, -1, 0 }, right0[3] = { 1, 0, 0 }, ahead0[3] = { 0, 0, -1 };
        float back[3], fingers[3], right[3], ahead[3];
        qrot(h[k].q, back0, back); qrot(h[k].q, fingers0, fingers); qrot(head->q, right0, right); qrot(head->q, ahead0, ahead);
        const oot_wrist_quad q = oot_wrist_place(h[k].p, back, fingers, right, width, (float)OOT_WRIST_W / (float)wrist_height(&g_wrist_status));
        const float e[3] = { head->p[0] - q.center[0], head->p[1] - q.center[1], head->p[2] - q.center[2] };
        const float d = sqrtf(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
        if (d <= 1e-3f) continue;
        const float cf = (back[0] * e[0] + back[1] * e[1] + back[2] * e[2]) / d, cg = -(ahead[0] * e[0] + ahead[1] * e[1] + ahead[2] * e[2]) / d;
        float speed = 0;
        for (int i = 0; i < 2; i++) if (h[i].have) speed = fmaxf(speed, sqrtf(h[i].v[0] * h[i].v[0] + h[i].v[1] * h[i].v[1] + h[i].v[2] * h[i].v[2]));
        const oot_wrist_in c = { d, acosf(fmaxf(-1, fminf(1, cf))) * 57.29578f, acosf(fmaxf(-1, fminf(1, cg))) * 57.29578f, speed, 0 };
        if (chosen < 0 || c.facing + c.gaze < in.facing + in.gaze) { in = c; quad = q; chosen = k; }
    }
    if (chosen >= 0) {
        g_wrist_quad = quad;
        in.blocked = vr_oot_holding() || vr_oot_aiming() || (runtime_oot_vr_event_kind && runtime_oot_vr_event_kind() != OOT_EVENT_NONE) ||
                     atomic_load(&g_oot_locked_on) || oot_wheel_is_open() || (g_wrist_hurt_at && t - g_wrist_hurt_at < 3000000000ull);
    }
    const int was = g_wrist_state.on;
    g_wrist_shown = oot_wrist_step(&g_wrist_state, &in, dt);
    if (g_wrist_state.on && !was) g_wrist_hand = chosen;
    if (!g_wrist_state.on) g_wrist_hand = -1;
    if (g_wrist_state.on != was) VLOG("wrist panel: %s on the %s wrist, %.0f cm (eyes %.2f m, facing %.0f deg, gaze %.0f deg; life %d of %d, magic %d of %d, rupees %d)", g_wrist_state.on ? "shown" : "hidden", chosen == 0 ? "left" : chosen == 1 ? "right" : "no", (double)(width * 100),
                                      (double)in.eye_distance, (double)in.facing, (double)in.gaze, g_wrist_status.life, g_wrist_status.max_life, g_wrist_status.magic, g_wrist_status.max_magic, g_wrist_status.rupees);
    if (g_wrist_shown > 0 && (!g_wrist_tex || memcmp(&g_wrist_drawn, &g_wrist_status, sizeof g_wrist_drawn) || memcmp(&g_wrist_dungeon_drawn, &g_wrist_dungeon, sizeof g_wrist_dungeon))) {
        wrist_render(&g_wrist_status); g_wrist_drawn = g_wrist_status; g_wrist_dungeon_drawn = g_wrist_dungeon;
    }
}
/* A diagnostic (option hand_marks, off; HookshotVR's "debug cubes"): a small square at each controller's tracked place, facing the
   eye, red for the left and blue for the right, to see where the headset says the hands are against the arms the game draws. */
int vr_oot_hand_marks(void) { const char *e = getenv("TOTK_VR_HAND_MARKS"); return e ? !strcmp(e, "1") : vr_option("hand_marks", 0) != 0; }
void vr_oot_set_hand_marks(int on) { vr_option_set("hand_marks", on != 0); VLOG("the controllers' marks: %s", on ? "on" : "off"); }
static id<MTLTexture> g_mark_tex[2];
static void oot_hand_marks_draw(id<MTLRenderCommandEncoder> e, const float m[16], const vr_pose *eye) {
    if (!vr_oot_hand_marks() || !g.ps_pre) return;
    vr_hand h[2]; vr_link_hands(h);
    for (int k = 0; k < 2; k++) {
        if (!h[k].have) continue;
        if (!g_mark_tex[k]) {
            enum { N = 8 }; uint8_t px[N * N * 4];
            for (int i = 0; i < N * N; i++) { px[4 * i] = k ? 230 : 40; px[4 * i + 1] = 40; px[4 * i + 2] = k ? 40 : 230; px[4 * i + 3] = 255; }   /* BGRA: left red, right blue */
            MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:N height:N mipmapped:NO];
            d.usage = MTLTextureUsageShaderRead; d.storageMode = MTLStorageModeShared; g_mark_tex[k] = [g.dev newTextureWithDescriptor:d];
            if (!g_mark_tex[k]) continue;
            [g_mark_tex[k] replaceRegion:MTLRegionMake2D(0, 0, N, N) mipmapLevel:0 withBytes:px bytesPerRow:N * 4];
        }
        float to[3] = { h[k].p[0] - eye->p[0], h[k].p[1] - eye->p[1], h[k].p[2] - eye->p[2] };
        const float dist = sqrtf(to[0] * to[0] + to[1] * to[1] + to[2] * to[2]);
        if (dist < 0.05f) continue;
        for (int i = 0; i < 3; i++) to[i] /= dist;
        float right[3] = { -to[2], 0, to[0] }; const float rl = sqrtf(right[0] * right[0] + right[2] * right[2]);
        if (rl < 1e-3f) continue;
        right[0] /= rl; right[2] /= rl;
        const float up[3] = { right[1] * to[2] - right[2] * to[1], right[2] * to[0] - right[0] * to[2], right[0] * to[1] - right[1] * to[0] }, size = 0.03f;
        quad_u u; memcpy(u.m, m, sizeof u.m);
        for (int i = 0; i < 3; i++) { u.c[i] = h[k].p[i]; u.r[i] = right[i] * size; u.u[i] = up[i] * size; }
        u.c[3] = u.r[3] = u.u[3] = 0; u.uvs[0] = u.uvs[1] = 1; u.uvs[2] = u.uvs[3] = 0;
        const float kk[4] = { 1, 0, 0, 0 };
        [e setRenderPipelineState:g.ps_pre]; [e setVertexBytes:&u length:sizeof u atIndex:0]; [e setFragmentBytes:kk length:sizeof kk atIndex:0];
        [e setFragmentTexture:g_mark_tex[k] atIndex:0]; [e setFragmentSamplerState:g.smp atIndex:0];
        [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        [e setRenderPipelineState:g.ps];
    }
}
/* Over one eye's picture: m takes the room to that eye's clip space; pose and fov are the eye's. */
static void oot_wrist_draw(id<MTLRenderCommandEncoder> e, const float m[16], const vr_pose *eye, const float fov[4]) {
    oot_hand_marks_draw(e, m, eye);
    oot_ocarina_draw(e, m, eye);
    if (g_wrist_shown > 0 && g_wrist_tex && g.ps_pre) {
        quad_u u; memcpy(u.m, m, sizeof u.m);
        memcpy(u.c, g_wrist_quad.center, 12); memcpy(u.r, g_wrist_quad.right, 12); memcpy(u.u, g_wrist_quad.up, 12); u.c[3] = u.r[3] = u.u[3] = 0;
        u.uvs[0] = u.uvs[1] = 1; u.uvs[2] = u.uvs[3] = 0;
        const float k[4] = { g_wrist_shown, 0, 0, 0 };
        [e setRenderPipelineState:g.ps_pre]; [e setVertexBytes:&u length:sizeof u atIndex:0]; [e setFragmentBytes:k length:sizeof k atIndex:0];
        [e setFragmentTexture:g_wrist_tex atIndex:0]; [e setFragmentSamplerState:g.smp atIndex:0];
        [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        [e setRenderPipelineState:g.ps];
    }
    if (g_reticle_on && g.ps_pre) {   /* the aim's mark, facing this eye */
        float to[3] = { g_reticle_at[0] - eye->p[0], g_reticle_at[1] - eye->p[1], g_reticle_at[2] - eye->p[2] };
        const float dist = sqrtf(to[0] * to[0] + to[1] * to[1] + to[2] * to[2]);
        if (dist > 0.5f) {
            for (int i = 0; i < 3; i++) to[i] /= dist;
            float right[3] = { -to[2], 0, to[0] }; const float rl = sqrtf(right[0] * right[0] + right[2] * right[2]);
            if (rl > 1e-3f) {
                right[0] /= rl; right[2] /= rl;
                const float up[3] = { right[1] * to[2] - right[2] * to[1], right[2] * to[0] - right[0] * to[2], right[0] * to[1] - right[1] * to[0] }, size = dist * 0.07f * (float)vr_oot_reticle_size() / 100;   /* the ring is two thirds of the square: about 2.7 degrees across (0.035 was hard to find, run new5) */
                quad_u u; memcpy(u.m, m, sizeof u.m);
                for (int i = 0; i < 3; i++) { u.c[i] = g_reticle_at[i]; u.r[i] = right[i] * size; u.u[i] = up[i] * size; }
                u.c[3] = u.r[3] = u.u[3] = 0; u.uvs[0] = u.uvs[1] = 1; u.uvs[2] = u.uvs[3] = 0;
                const float k[4] = { 1, 0, 0, 0 };
                [e setRenderPipelineState:g.ps_pre]; [e setVertexBytes:&u length:sizeof u atIndex:0]; [e setFragmentBytes:k length:sizeof k atIndex:0];
                [e setFragmentTexture:g_reticle_tex atIndex:0]; [e setFragmentSamplerState:g.smp atIndex:0];
                [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
                [e setRenderPipelineState:g.ps];
            }
        }
    }
    if (g_vignette.amount > 0 && g_vignette.inner > 0 && g.ps_ring) {   /* a quad a metre ahead of the eye, wider than its view */
        const float ahead0[3] = { 0, 0, -1 }, right0[3] = { 1, 0, 0 }, up0[3] = { 0, 1, 0 }; float ahead[3], right[3], up[3], reach = 0;
        qrot(eye->q, ahead0, ahead); qrot(eye->q, right0, right); qrot(eye->q, up0, up);
        for (int i = 0; i < 4; i++) reach = fmaxf(reach, fabsf(tanf(fov[i])));
        const float W = 2.1f * fmaxf(reach, 2 * g_vignette.inner);
        quad_u u; memcpy(u.m, m, sizeof u.m);
        for (int i = 0; i < 3; i++) { u.c[i] = eye->p[i] + ahead[i]; u.r[i] = right[i] * W; u.u[i] = up[i] * W; }
        u.c[3] = u.r[3] = u.u[3] = 0; u.uvs[0] = u.uvs[1] = W; u.uvs[2] = u.uvs[3] = -W / 2;   /* uv: tangents from straight ahead */
        const float k[4] = { g_vignette.inner, g_vignette.amount, 0, 0 };
        [e setRenderPipelineState:g.ps_ring]; [e setVertexBytes:&u length:sizeof u atIndex:0]; [e setFragmentBytes:k length:sizeof k atIndex:0];
        [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        [e setRenderPipelineState:g.ps];
    }
}
