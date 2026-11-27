/* zeldaVR, Ocarina of Time: the VR side of the game's scenes and of the ocarina in the hands (vr.md rows 69-71, 75-79; the rules are
   zeldaVR/runtime/oot_vr_scene.h and oot_vr_ocarina.h, the game's side games/oot/runtime/oot_vr_scene.h and oot_vr_ocarina.h).
   Included by oot_vr_wrist_draw.h (so by totk_vr.m, GAME_OOT); the pad's thread calls oot_ocarina_pad, the VR thread
   oot_scene_update and oot_ocarina_draw. The ocarina's picture is drawn here with CoreGraphics: nothing of the game's art. */
#include "../runtime/oot_vr_scene.h"
#include "../runtime/oot_vr_ocarina.h"
extern void runtime_oot_vr_scene_options(const int v[7]) __attribute__((weak_import));
extern int runtime_oot_vr_ocarina(int *owned, int *waiting, int *kind) __attribute__((weak_import));
extern int runtime_oot_vr_scene_style(void) __attribute__((weak_import));
extern int runtime_oot_vr_item_hand(void) __attribute__((weak_import));   /* games/oot/runtime/oot_vr_hand_items.h: the hand holding a hand item, -1 none */

/* ---- the options (the VR page; camera-ui.md 4.4 for HookshotVR's, in brackets) ----
   Defaults (merge 2026-11-27, the user's rule: nothing that changes how the game plays now is on until it is verified in the game):
   the scenes as players have them now, the ocarina the game's buttons.
   cutscene_view: 0 every scene the game's picture on a screen, 1 story scenes and item gets on a screen and the rest from the
     player's eyes, 2 story scenes in 3D too [2]. Until it is set it follows "talks and doors" (events_eyes, before 2026-11-27):
     its 1 (the player's eyes, the default) is 1 here, its 0 (the game's camera) is 0: what each player has now.
   cutscene_3d: 0 the director's camera [default], 1 a theatre seat, 2 Link's spot, 3 following Link (his eyes). Only in 3D.
   cutscene_light: 1 every shot as the game lights it (default: nothing of ours) [0 auto], 2 the first shot's light held, 3 the area's.
   cutscene_cull: 1 the game culls from its own camera (default, as now) [0 from the player's view while a 3D scene plays].
   realign: 0 off (default, as now) [1 a quick turn], 2 at once, 3 following Link as he turns.
   item_get_view: 0 as the game shows it (default, as now) [1 the item before the eyes and the view not turned with Link].
   ocarina: 0 the game's buttons as before (default) [1 the ocarina in the hands]. song_fx: 1 a song's effects shown (default, as
   now), 0 hidden. song_menu: 1 the other hand's grip opens the game's song list while the ocarina is at the mouth (default). */
int vr_oot_events_eyes(void) { return vr_option("events_eyes", 1) != 0; }
void vr_oot_set_events_eyes(int on) { vr_option_set("events_eyes", on != 0); }
static int oot_opt_clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
int vr_oot_cutscene_view(void) { return oot_opt_clamp(vr_option("cutscene_view", vr_oot_events_eyes() ? OOT_CUT_SCREEN : OOT_CUT_FLAT), 0, 2); }
void vr_oot_set_cutscene_view(int v) { vr_option_set("cutscene_view", oot_opt_clamp(v, 0, 2)); VLOG("cutscenes: %s", v <= 0 ? "every scene on a screen" : v == 1 ? "story scenes on a screen" : "in 3D"); }
int vr_oot_cutscene_3d(void) { return oot_opt_clamp(vr_option("cutscene_3d", OOT_VANTAGE_DIRECTOR), 0, 3); }
void vr_oot_set_cutscene_3d(int v) { vr_option_set("cutscene_3d", oot_opt_clamp(v, 0, 3)); VLOG("3D cutscenes: from %s", v <= 0 ? "the director's camera" : v == 1 ? "a theatre seat" : v == 2 ? "Link's spot" : "Link's eyes"); }
int vr_oot_cutscene_light(void) { return oot_opt_clamp(vr_option("cutscene_light", OOT_LIGHT_EVERY), 0, 3); }
void vr_oot_set_cutscene_light(int v) { vr_option_set("cutscene_light", oot_opt_clamp(v, 0, 3)); VLOG("cutscene light: %d", vr_oot_cutscene_light()); }
int vr_oot_cutscene_cull(void) { return oot_opt_clamp(vr_option("cutscene_cull", 1), 0, 1); }
void vr_oot_set_cutscene_cull(int v) { vr_option_set("cutscene_cull", oot_opt_clamp(v, 0, 1)); VLOG("cutscene culling: %s", v ? "the game's camera" : "the player's view"); }
int vr_oot_realign(void) { return oot_opt_clamp(vr_option("realign", OOT_REALIGN_OFF), 0, 3); }
void vr_oot_set_realign(int v) { vr_option_set("realign", oot_opt_clamp(v, 0, 3)); VLOG("re-align after scenes: %d", vr_oot_realign()); }
int vr_oot_item_get_view(void) { return vr_option("item_get_view", 0) != 0; }
void vr_oot_set_item_get_view(int on) { vr_option_set("item_get_view", on != 0); VLOG("an item get: %s", on ? "before the eyes, the view kept" : "as the game shows it"); }
int vr_oot_ocarina(void) { const char *e = getenv("TOTK_VR_OCARINA"); return e ? !strcmp(e, "1") : vr_option("ocarina", 0) != 0; }
void vr_oot_set_ocarina(int on) { vr_option_set("ocarina", on != 0); VLOG("the ocarina: %s", on ? "in the hands" : "the game's buttons"); }
int vr_oot_song_fx(void) { return vr_option("song_fx", 1) != 0; }
void vr_oot_set_song_fx(int on) { vr_option_set("song_fx", on != 0); VLOG("a song's effects: %s", on ? "shown" : "hidden"); }
int vr_oot_song_menu(void) { return vr_option("song_menu", 1) != 0; }
void vr_oot_set_song_menu(int on) { vr_option_set("song_menu", on != 0); VLOG("the song list: %s", on ? "the other hand's grip" : "the game's ZL only"); }

/* For the game's side (games/oot/runtime/oot_vr_scene.h): the left stick last pushed past 0.2, and ZL (lock-on) going to the game. */
static _Atomic uint64_t g_oot_stick_at;
uint64_t vr_oot_stick_moved_at(void) { return atomic_load(&g_oot_stick_at); }
/* vr_oot_zl_held: zeldaVR/gfx/oot_vr_gear_options.h has it (the same ZL; merge 2026-11-27, one definition for both) */

/* ---- the ocarina: the pad's thread ---- */
static pthread_mutex_t g_oc_mu = PTHREAD_MUTEX_INITIALIZER;
static oot_oc_state g_oc;
static int g_oc_owned, g_oc_shown;   /* for the picture: Link has one; the VR thread's copy of where it is */
static _Atomic int g_oc_song_hidden;
/* nvn_mtl.m (oot_vr_song.inc): a song's effects are left out of the picture now. */
int vr_oot_song_fx_hidden(void) { return atomic_load(&g_oc_song_hidden); }
static void oot_ocarina_pad(uint64_t now, const vr_controls *c, int lefty, oot_pad_out *o) {
    if (!vr_oot_ocarina() || !runtime_oot_vr_ocarina) return;
    int owned = 0, waiting = 0, kind = 0; runtime_oot_vr_ocarina(&owned, &waiting, &kind);
    vr_hand h[2]; vr_link_hands(h); vr_pose hp;
    oot_oc_in in; memset(&in, 0, sizeof in);
    if (!vr_link_pose(&hp)) return;
    float f[3]; const float nz[3] = { 0, 0, -1 }; qrot(hp.q, nz, f);
    const float fl = sqrtf(f[0] * f[0] + f[2] * f[2]); if (fl < 1e-3f) return;
    const float fx = f[0] / fl, fz = f[2] / fl, rx = -fz, rz = fx, inv[4] = { -hp.q[0], -hp.q[1], -hp.q[2], hp.q[3] };
    for (int k = 0; k < 2; k++) {
        if (!h[k].have) continue;
        const float d[3] = { h[k].p[0] - hp.p[0], h[k].p[1] - hp.p[1], h[k].p[2] - hp.p[2] };
        in.hand[k].have = 1; qrot(inv, d, in.hand[k].p);
        in.hand[k].lvl[0] = d[0] * fx + d[2] * fz; in.hand[k].lvl[1] = (d[0] * rx + d[2] * rz) * (lefty ? -1 : 1); in.hand[k].lvl[2] = d[1];
        in.hand[k].grip = k ? c->rgrip : c->lgrip;
    }
    if (lefty) { const oot_oc_hand t = in.hand[0]; in.hand[0] = in.hand[1]; in.hand[1] = t; }   /* the belt on the side the sword is not */
    in.ltrig = c->ltrig; in.rtrig = c->rtrig; in.rx = c->rx; in.ry = c->ry;
    in.upper = (c->buttons & VR_PAD_Y) != 0; in.lower = (c->buttons & VR_PAD_X) != 0;
    in.owned = owned != 0 && runtime_oot_vr_fp_active && runtime_oot_vr_fp_active();   /* first person only: elsewhere it stays on the belt */
    in.waiting = waiting; in.wait_kind = kind;
    in.event = runtime_oot_vr_event_kind && runtime_oot_vr_event_kind() != OOT_EVENT_NONE;
    in.busy[1] = vr_oot_holding();   /* the sword hand (after the swap above) */
    {   /* merge 2026-11-27: nor a hand that holds the bow (oot_vr_bow_ctl.h) or a hand item (games/oot/runtime/oot_vr_hand_items.h);
           both are controller numbers, the swap above turned ours */
        const int item = runtime_oot_vr_item_hand ? runtime_oot_vr_item_hand() : -1, bow = vr_oot_bow_hand();
        for (int i = 0; i < 2; i++) { const int ctrl = lefty ? 1 - i : i; if (ctrl == item || ctrl == bow) in.busy[i] = 1; }
    }
    in.song_menu = vr_oot_song_menu();
    oot_oc_out out;
    pthread_mutex_lock(&g_oc_mu);
    oot_ocarina_step(&g_oc, now, &in, &out);
    g_oc_owned = in.owned;
    const int held = g_oc.held < 0 ? -1 : lefty ? 1 - g_oc.held : g_oc.held, menu = g_oc.menu;
    pthread_mutex_unlock(&g_oc_mu);
    o->bits = (o->bits & ~out.clear) | out.add;
    if (out.take_stick) { o->stick[2] = o->stick[3] = 0; o->yaw = 0; }
    for (int i = 0; i < 2; i++) if (out.buzz[i]) vr_link_send_haptic(lefty ? 1 - i : i, 30000000ull, 200, 0.4f);
    atomic_store(&g_oc_song_hidden, !vr_oot_song_fx() && in.event && runtime_oot_vr_event_kind() == OOT_EVENT_OCARINA && !waiting && !menu);
    static const char *const say[] = { "", "taken from the belt", "put back on the belt", "at the mouth", "at the mouth: the game's ocarina is asked for (D-pad Left)",
        "the game waits for notes: the triggers, the right stick and the left controller's buttons play", "the game did not start its ocarina within 1.5 s: given up until it leaves the mouth",
        "the game's field ocarina ended (B)", "away from the mouth", "the game's ocarina ended" };
    if (out.say > 0 && out.say < (int)(sizeof say / sizeof *say)) VLOG("[vr-ocarina] %s (%s hand; wait kind %d)", say[out.say], held == 0 ? "left" : held == 1 ? "right" : "no", kind);
}

/* ---- the VR thread: the options to the game's side once a frame; the ocarina's picture ---- */
static id<MTLTexture> g_oc_tex; static float g_oc_at[3], g_oc_axis[3];
static void oot_ocarina_render(void) {
    enum { W = 192, H = 128 }; static uint8_t px[W * H * 4];
    CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB();
    CGContextRef c = CGBitmapContextCreate(px, W, H, 8, W * 4, rgb, (CGBitmapInfo)kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    CGColorSpaceRelease(rgb);
    if (!c) return;
    CGContextClearRect(c, CGRectMake(0, 0, W, H));
    CGContextSetRGBFillColor(c, 0.16, 0.36, 0.78, 1); CGContextFillEllipseInRect(c, CGRectMake(18, 22, 150, 84));   /* the body */
    CGContextSetRGBFillColor(c, 0.16, 0.36, 0.78, 1); CGContextFillRect(c, CGRectMake(150, 56, 36, 18));              /* the mouthpiece */
    CGContextSetRGBStrokeColor(c, 0.85, 0.90, 1.0, 1); CGContextSetLineWidth(c, 5); CGContextStrokeEllipseInRect(c, CGRectMake(18, 22, 150, 84));
    CGContextSetRGBFillColor(c, 0.06, 0.10, 0.22, 1);
    for (int i = 0; i < 4; i++) CGContextFillEllipseInRect(c, CGRectMake(44 + i * 26, 64, 14, 14));
    CGContextFillEllipseInRect(c, CGRectMake(70, 36, 14, 14));
    CGContextRelease(c);
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:W height:H mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead; d.storageMode = MTLStorageModeShared; g_oc_tex = [g.dev newTextureWithDescriptor:d];
    if (g_oc_tex) [g_oc_tex replaceRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0 withBytes:px bytesPerRow:W * 4];
}
static void oot_scene_update(const vr_pose *head, int world) {
    if (runtime_oot_vr_scene_options) {
        const int v[7] = { vr_oot_cutscene_view(), vr_oot_cutscene_3d(), vr_oot_cutscene_light(), vr_oot_cutscene_cull(), vr_oot_realign(), vr_oot_item_get_view(), vr_oot_ocarina() };
        runtime_oot_vr_scene_options(v);
    }
    g_oc_shown = 0;
    const int fp = world && runtime_oot_vr_fp_active && runtime_oot_vr_fp_active() && (!runtime_oot_vr_scene_style || runtime_oot_vr_scene_style() == OOT_STYLE_EYES);
    if (!fp || !vr_oot_ocarina()) return;
    pthread_mutex_lock(&g_oc_mu); const int owned = g_oc_owned, held = g_oc.held; pthread_mutex_unlock(&g_oc_mu);
    if (!owned) return;
    const int lefty = vr_oot_left_handed();
    if (held >= 0) {   /* in the hand: a little ahead of the grip, along the controller */
        vr_hand h[2]; vr_link_hands(h); const int k = lefty ? 1 - held : held;
        if (!h[k].have) return;
        const float ahead0[3] = { 0, 0, -0.05f }, side0[3] = { 1, 0, 0 }; float a[3]; qrot(h[k].q, ahead0, a); qrot(h[k].q, side0, g_oc_axis);
        for (int i = 0; i < 3; i++) g_oc_at[i] = h[k].p[i] + a[i];
    } else {           /* at the belt (oot_vr_ocarina.h OOT_OC_BELT), in the head's level frame */
        float f[3]; const float nz[3] = { 0, 0, -1 }; qrot(head->q, nz, f);
        const float fl = sqrtf(f[0] * f[0] + f[2] * f[2]); if (fl < 1e-3f) return;
        const float fx = f[0] / fl, fz = f[2] / fl, r = OOT_OC_BELT[1] * (lefty ? -1 : 1);
        g_oc_at[0] = head->p[0] + fx * OOT_OC_BELT[0] - fz * r; g_oc_at[1] = head->p[1] + OOT_OC_BELT[2]; g_oc_at[2] = head->p[2] + fz * OOT_OC_BELT[0] + fx * r;
        g_oc_axis[0] = -fz; g_oc_axis[1] = 0; g_oc_axis[2] = fx;
    }
    if (!g_oc_tex) oot_ocarina_render();
    g_oc_shown = g_oc_tex != nil;
}
/* Over one eye: the ocarina, its long side along its axis and turned to face the eye. */
static void oot_ocarina_draw(id<MTLRenderCommandEncoder> e, const float m[16], const vr_pose *eye) {
    if (!g_oc_shown || !g.ps_pre) return;
    float to[3] = { eye->p[0] - g_oc_at[0], eye->p[1] - g_oc_at[1], eye->p[2] - g_oc_at[2] };
    const float dist = sqrtf(to[0] * to[0] + to[1] * to[1] + to[2] * to[2]);
    if (dist < 0.05f) return;
    for (int i = 0; i < 3; i++) to[i] /= dist;
    float r[3] = { g_oc_axis[0], g_oc_axis[1], g_oc_axis[2] };
    const float k = r[0] * to[0] + r[1] * to[1] + r[2] * to[2];
    for (int i = 0; i < 3; i++) r[i] -= to[i] * k;
    const float rl = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    if (rl < 1e-3f) return;
    for (int i = 0; i < 3; i++) r[i] /= rl;
    const float up[3] = { to[1] * r[2] - to[2] * r[1], to[2] * r[0] - to[0] * r[2], to[0] * r[1] - to[1] * r[0] };
    quad_u u; memcpy(u.m, m, sizeof u.m);
    for (int i = 0; i < 3; i++) { u.c[i] = g_oc_at[i]; u.r[i] = r[i] * 0.06f; u.u[i] = up[i] * 0.04f; }
    u.c[3] = u.r[3] = u.u[3] = 0; u.uvs[0] = u.uvs[1] = 1; u.uvs[2] = u.uvs[3] = 0;
    const float kk[4] = { 1, 0, 0, 0 };
    [e setRenderPipelineState:g.ps_pre]; [e setVertexBytes:&u length:sizeof u atIndex:0]; [e setFragmentBytes:kk length:sizeof kk atIndex:0];
    [e setFragmentTexture:g_oc_tex atIndex:0]; [e setFragmentSamplerState:g.smp atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    [e setRenderPipelineState:g.ps];
}
