/* zeldaVR, Ocarina of Time: the window view (vr.md row 3; the geometry in zeldaVR/runtime/oot_vr_window.h). The pictures with no head
   pose of their own (title, menus, loading, scenes shown on a screen, the big screen of the left-stick shift + Menu) go into a frame
   fixed in the room instead of on the curved screen (oot_arc_draw), when the VR page says so. Included by totk_vr.m (GAME_OOT, after
   oot_arc_draw); the VR thread only.

   Options (the VR page; HookshotVR's ranges and defaults for the frame itself, its window_view.inc / vr_options.inc, ideas only):
     screen_shape  0   0 the curved screen (what players have now), 1 the window. Default 0: nothing changes unless it is chosen.
     win_w         100 cm wide (20-300)
     win_169       1   its height follows the width at 16:9
     win_h         56  cm high without 16:9 (10-200)
     win_round     0   % roundness of its corners (0 a rectangle, 100 an ellipse)
     win_feather   0   mm of soft edge (0-100)
     win_tilt      0   degrees, more leans its top away (-45..45)
     win_dist      90  cm ahead of the head where it was placed (40-300)
     win_back      0   seen from behind: 1 a dark panel, 0 nothing
   The frame is placed where the head is (on the first headset frame, at a recentre, when the window is chosen, and by the VR page's
   "place again" button)
   and stays there in the room; changing an option reshapes it about the same place.

   A world picture (the scene the HUD was parted from, vr_ui_split, with the headset's field of view in the game, g.fov_on) is seen
   through the frame from each eye where it is now (OOT_WIN_THROUGH): the world stays behind the frame as the head moves. Anything
   else is fitted whole (OOT_WIN_FIT). An eye behind the frame sees its back panel or nothing; within 2 cm of its plane nothing, and
   the picture fades in over the 12 cm before that (HookshotVR's distances). */
#include "../runtime/oot_vr_window.h"

static _Atomic int g_win_replace;   /* placed again before the head at the next headset frame */
int vr_oot_screen_shape(void) { return vr_option("screen_shape", OOT_WIN_CURVED) == OOT_WIN_FRAME ? OOT_WIN_FRAME : OOT_WIN_CURVED; }
void vr_oot_set_screen_shape(int v) { vr_option_set("screen_shape", v == OOT_WIN_FRAME); if (v == OOT_WIN_FRAME) atomic_store(&g_win_replace, 1);
    VLOG("[vr-window] the screens: %s", v == OOT_WIN_FRAME ? "a window in the room" : "the curved screen"); }
static oot_win_opts oot_window_opts(void) {
    const oot_win_opts d = oot_win_default_opts();
    oot_win_opts o = { vr_option("win_w", d.width_cm), vr_option("win_169", d.keep169) != 0, vr_option("win_h", d.height_cm),
                       vr_option("win_round", d.round_pct), vr_option("win_feather", d.feather_mm), vr_option("win_tilt", d.tilt_deg),
                       vr_option("win_dist", d.dist_cm), vr_option("win_back", d.back) != 0 };
    o.width_cm = oot_win_clampi(o.width_cm, 20, 300); o.height_cm = oot_win_clampi(o.height_cm, 10, 200);
    o.round_pct = oot_win_clampi(o.round_pct, 0, 100); o.feather_mm = oot_win_clampi(o.feather_mm, 0, 100);
    o.tilt_deg = oot_win_clampi(o.tilt_deg, -45, 45); o.dist_cm = oot_win_clampi(o.dist_cm, 40, 300);
    return o;
}
static void oot_window_said(const char *what, int v) { VLOG("[vr-window] %s: %d", what, v); }
int vr_oot_win_w(void) { return oot_window_opts().width_cm; }
void vr_oot_set_win_w(int cm) { vr_option_set("win_w", oot_win_clampi(cm, 20, 300)); oot_window_said("width, cm", vr_oot_win_w()); }
int vr_oot_win_169(void) { return oot_window_opts().keep169; }
void vr_oot_set_win_169(int on) { vr_option_set("win_169", on != 0); oot_window_said("keep 16:9", on != 0); }
int vr_oot_win_h(void) { return oot_window_opts().height_cm; }
void vr_oot_set_win_h(int cm) { vr_option_set("win_h", oot_win_clampi(cm, 10, 200)); oot_window_said("height, cm", vr_oot_win_h()); }
int vr_oot_win_round(void) { return oot_window_opts().round_pct; }
void vr_oot_set_win_round(int pct) { vr_option_set("win_round", oot_win_clampi(pct, 0, 100)); oot_window_said("corner roundness, %", vr_oot_win_round()); }
int vr_oot_win_feather(void) { return oot_window_opts().feather_mm; }
void vr_oot_set_win_feather(int mm) { vr_option_set("win_feather", oot_win_clampi(mm, 0, 100)); oot_window_said("feathered edge, mm", vr_oot_win_feather()); }
int vr_oot_win_tilt(void) { return oot_window_opts().tilt_deg; }
void vr_oot_set_win_tilt(int deg) { vr_option_set("win_tilt", oot_win_clampi(deg, -45, 45)); oot_window_said("tilt, degrees", vr_oot_win_tilt()); }
int vr_oot_win_dist(void) { return oot_window_opts().dist_cm; }
void vr_oot_set_win_dist(int cm) { vr_option_set("win_dist", oot_win_clampi(cm, 40, 300)); oot_window_said("distance, cm", vr_oot_win_dist()); }
int vr_oot_win_back(void) { return oot_window_opts().back; }
void vr_oot_set_win_back(int on) { vr_option_set("win_back", on != 0); oot_window_said("back panel", on != 0); }
/* The window's parallax (vr.md row 3, oot_vr_winpar_draw.h): the game's camera moves with the head against the frame. Off by default
   (HookshotVR's window always moves it; ours keeps the window as committed in a21e531c unless this is chosen). */
int vr_oot_win_parallax(void) { return vr_option("win_parallax", 0) != 0; }
void vr_oot_set_win_parallax(int on) { vr_option_set("win_parallax", on != 0); oot_window_said("parallax (the game's camera moves with the head)", on != 0); }

/* Where the frame was placed: the head then, and its heading. */
static float g_win_at[3], g_win_yaw; static int g_win_have;
void vr_oot_window_place(void) { atomic_store(&g_win_replace, 1); }   /* the VR page's button: before the head, at the next headset frame */
static void oot_window_place(const vr_pose *head) {
    const float nz[3] = { 0, 0, -1 }; float f[3]; qrot(head->q, nz, f);
    if (f[0] * f[0] + f[2] * f[2] < 1e-4f) return;   /* looking straight up or down: keep where it was */
    for (int i = 0; i < 3; i++) if (!isfinite(head->p[i])) return;
    memcpy(g_win_at, head->p, sizeof g_win_at); g_win_yaw = oot_hud_heading(f); g_win_have = 1;
    if (vr_oot_screen_shape() == OOT_WIN_FRAME)
        VLOG("[vr-window] placed before the head: heading %.0f degrees, %d cm ahead", (double)(g_win_yaw * 57.2957795f), vr_oot_win_dist());
}
/* VR thread, each headset frame: placed with the screen (first frame, recentre), or again when the VR page asks. */
static void oot_window_tick(const vr_pose *head, int placed_now) {
    if (placed_now || atomic_exchange(&g_win_replace, 0) || !g_win_have) oot_window_place(head);
}

typedef struct { float m[16]; float c[4], x[4], y[4], e[4], box[4], fit[4], k[4]; } oot_win_u;
static const char *k_win_msl =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "struct W { float4x4 m; float4 c, x, y, e, box, fit, k; };\n"
    "struct WO { float4 pos [[position]]; float2 p; };\n"
    /* the frame's four corners, top-left first (a triangle strip); p: the point in the frame's own axes, metres */
    "vertex WO win_vs(uint i [[vertex_id]], constant W &u [[buffer(0)]]) {\n"
    "  float2 p = float2(float(i & 1) * 2.0 - 1.0, 1.0 - float(i >> 1) * 2.0) * float2(u.x.w, u.y.w);\n"
    "  WO o; o.pos = u.m * float4(u.c.xyz + p.x * u.x.xyz + p.y * u.y.xyz, 1.0); o.p = p; return o; }\n"
    /* oot_vr_window.h oot_win_inside */
    "float win_inside(float2 p, float2 hs, float r) {\n"
    "  if (r <= 0.005) return min(hs.x - abs(p.x), hs.y - abs(p.y));\n"
    "  float n = 2.0 / min(r, 1.0); float2 a = abs(p) / hs; float m = max(a.x, a.y);\n"
    "  if (m <= 0.0) return min(hs.x, hs.y);\n"
    "  return (1.0 - m * pow(pow(a.x / m, n) + pow(a.y / m, n), 1.0 / n)) * min(hs.x, hs.y); }\n"
    /* k: x the way (0 fit, 1 through, 2 the back panel), y roundness, z feather (m), w 1 a linear picture; e: the eye in the frame's
       axes, w the fade; box: the picture's tangents left, right, bottom, top; fit: scale and offset of the picture in the frame */
    "fragment float4 win_fs(WO i [[stage_in]], constant W &u [[buffer(0)]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {\n"
    "  float2 hs = float2(u.x.w, u.y.w); float d = win_inside(i.p, hs, u.k.y);\n"
    "  float a = u.k.z > 0.0 ? clamp(d / u.k.z, 0.0, 1.0) : clamp(d / max(fwidth(d), 1e-6) + 0.5, 0.0, 1.0);\n"
    "  if (a <= 0.0) discard_fragment();\n"
    "  if (u.k.x > 1.5) return float4(0.035, 0.035, 0.04, 1.0) * a;\n"
    "  float2 uv;\n"
    "  if (u.k.x > 0.5) { float2 tg = (i.p - u.e.xy) / u.e.z; uv = float2((tg.x - u.box.x) / (u.box.y - u.box.x), (u.box.w - tg.y) / (u.box.w - u.box.z)); }\n"
    "  else uv = (float2(i.p.x / (2.0 * hs.x) + 0.5, 0.5 - i.p.y / (2.0 * hs.y)) - u.fit.zw) / u.fit.xy;\n"
    "  float edge = min(min(uv.x, 1.0 - uv.x), min(uv.y, 1.0 - uv.y));\n"
    "  float3 c = edge > 0.0 ? max(t.sample(s, clamp(uv, 0.0, 1.0)).rgb, float3(0)) : float3(0);\n"
    "  if (u.k.w > 0.5) c = select(1.055 * pow(c, float3(1.0 / 2.4)) - 0.055, 12.92 * c, c <= 0.0031308);\n"
    "  c *= clamp(edge / 0.01, 0.0, 1.0); a *= u.e.w;\n"
    "  return float4(c * a, a); }\n";
static id<MTLRenderPipelineState> g_win_ps; static int g_win_ps_failed;
static int oot_window_pipeline(void) {
    if (g_win_ps) return 1;
    if (g_win_ps_failed) return 0;
    NSError *e = nil; id<MTLLibrary> lib = [g.dev newLibraryWithSource:@(k_win_msl) options:[MTLCompileOptions new] error:&e];
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [lib newFunctionWithName:@"win_vs"]; pd.fragmentFunction = [lib newFunctionWithName:@"win_fs"];
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    pd.colorAttachments[0].blendingEnabled = YES;   /* premultiplied over the dark around it */
    pd.colorAttachments[0].sourceRGBBlendFactor = pd.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
    pd.colorAttachments[0].destinationRGBBlendFactor = pd.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    g_win_ps = lib ? [g.dev newRenderPipelineStateWithDescriptor:pd error:&e] : nil;
    if (!g_win_ps) { g_win_ps_failed = 1; VLOG("[vr-window] 【红】no pipeline (%s): the curved screen stays", e ? e.localizedDescription.UTF8String : "?"); return 0; }
    return 1;
}
/* One eye's picture in the window. 0: not the window's (the curved screen draws it); 1: done (drawn, or nothing to see from there).
   world: a world picture whose field of view is the headset's (seen through the frame). eye: this eye in the room. from: NULL, or
   where the picture was drawn from in the frame's axes (the window's parallax, oot_vr_winpar_draw.h): seen through the frame from
   there; whether it shows and how much it fades still go by the eye now. */
static int oot_window_draw_from(id<MTLRenderCommandEncoder> e, const float m[16], id<MTLTexture> pic, const float eye[3], int world, const float *from) {
    if (vr_oot_screen_shape() != OOT_WIN_FRAME || !pic || !g_win_have || !oot_window_pipeline()) return 0;
    const oot_win_opts o = oot_window_opts();
    const oot_win_shape s = oot_win_shape_of(&o);
    const oot_win_frame f = oot_win_place(g_win_at, g_win_yaw, &s);
    float el[3]; oot_win_local(&f, eye, el);
    float box[4] = { 0, 0, 0, 0 };
    const int through = world && g.fov_on > 0 && isfinite(g.fov_tanh) && isfinite(g.fov_aspect) && g.fov_tanh > 0.1f && g.fov_aspect > 0.1f;
    if (through) oot_win_symmetric_box(g.fov_tanh, g.fov_aspect, box);
    float fade; const int side = oot_win_side(el, through, s.back, &fade);
    { static int said = -1; if (side != said) { said = side;
        static const char *const names[4] = { "the whole picture fitted in it", "the world through it", "its back panel", "nothing (too near or behind it)" };
        VLOG("[vr-window] the frame (%.0f x %.0f cm, %.0f cm ahead) shows %s", (double)(s.w * 100), (double)(s.h * 100), (double)(s.dist * 100), names[side]); } }
    if (side == OOT_WIN_NONE) return 1;
    oot_win_u u; memcpy(u.m, m, sizeof u.m);
    memcpy(u.c, f.c, 12); memcpy(u.x, f.x, 12); memcpy(u.y, f.y, 12); memcpy(u.box, box, sizeof box);
    memcpy(u.e, from && from[2] > OOT_WIN_NEAR_M ? from : el, 12);
    u.c[3] = 0; u.x[3] = f.hw; u.y[3] = f.hh; u.e[3] = fade;
    oot_win_fit(pic.height ? (float)pic.width / (float)pic.height : 0, s.w / s.h, u.fit);
    u.k[0] = (float)(side == OOT_WIN_BACK ? 2 : side == OOT_WIN_THROUGH ? 1 : 0); u.k[1] = s.round; u.k[2] = s.feather;
    u.k[3] = pic.pixelFormat == MTLPixelFormatRG11B10Float ? 1 : 0;
    [e setRenderPipelineState:g_win_ps]; [e setVertexBytes:&u length:sizeof u atIndex:0]; [e setFragmentBytes:&u length:sizeof u atIndex:0];
    [e setFragmentTexture:pic atIndex:0]; [e setFragmentSamplerState:g.smp atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    [e setRenderPipelineState:g.ps];
    return 1;
}
static int oot_window_draw(id<MTLRenderCommandEncoder> e, const float m[16], id<MTLTexture> pic, const float eye[3], int world) {
    return oot_window_draw_from(e, m, pic, eye, world, NULL);
}
