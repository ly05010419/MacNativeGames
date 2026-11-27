/* zeldaVR for Ocarina of Time: the game's scenes in VR (vr.md rows 75-79 and the view part of 71; the rules are
   zeldaVR/runtime/oot_vr_scene.h). Included by oot_vr.h after oot_vr_state_read; the camera hook (oot_vr_camera), Link's
   update (oot_vr_player) and Link's hit-sensor update (oot_link_calc) each call one function here.

   Game data used, every address read on our image (update 131072, main.patched.bin, 2026-11-27):
     - the gameplay view's own camera fields (the view matrix update main+0x4c48c0 writes them each frame from its camera before
       our hook at main+0x4c5068: main+0x4c4e58 `stp d0, d1, [x20, #0x60]` .. +0x88 the place and target as doubles,
       main+0x4c4e6c `stp s17, s18, [x20, #0x38]`, +0x40, +0x48 the place (+0x38) and target (+0x44) as floats, main+0x4c4ec8 /
       +0x4c4ed0 the up (+0x50) as floats): read for the director's camera, and written with the player's view while a 3D scene
       plays (HookshotVR's "world culling 2", camera-ui.md 3.9 and 4.8), put back the next frame when the game did not rewrite them;
     - the WorldMgr, *(*(main+0x4c7e898)) (its caller main+0x288f80c..0x288f82c: `ldr x8, [x8, #0x898]` / `ldr x0, [x8]` /
       `bl main+0x3453d50`), whose demo palette set main+0x3453d50 (pacibsp; x0 the WorldMgr, x1 a pointer to a pointer to the
       name: main+0x3451b2c `ldr x1, [x1]` copies that text) and reset main+0x3453fd0 (pacibsp; x0 the WorldMgr) are the game's
       own; the palette in use: slot = WorldMgr + 0xde8 + 0x11a0 * (u32 at +0x3128, when it is under 2), its name's text at
       slot + 0xd88 (main+0x3453d70..0x3453d8c);
     - Link's facing: his actor's rotation +0x328, the forward column's x at +0x330 and z at +0x348 (HookshotVR's notes, held-items.md
       0.2, read only; refused unless hypot(x, z) is between 0.5 and 1.01).
   Not confirmed: whether Link's hit-sensor update runs while a story scene plays (the palette calls are made from it, as the
   other game calls are; the log says once per scene whether they could be made). */
#pragma once
#include "../zeldaVR/runtime/oot_vr_scene.h"
int vr_first_person(void) __attribute__((weak));               /* zeldaVR/gfx/totk_vr.m */
static int oot_scene_fp(void) { return vr_first_person && vr_first_person(); }
uint64_t vr_oot_stick_moved_at(void) __attribute__((weak));   /* zeldaVR/gfx/oot_vr_scene_draw.h: the left stick last pushed past 0.2 */
int vr_oot_zl_held(void) __attribute__((weak));               /* the pad gives the game ZL (lock-on) */

static const uint64_t OOT_WORLD_SLOT = 0x4c7e898, OOT_PALETTE_SET = 0x3453d50, OOT_PALETTE_RESET = 0x3453fd0;

/* The VR page's options, set by the VR thread (zeldaVR/gfx/oot_vr_scene_draw.h). */
/* Until the VR thread first sends them: the scenes as players have them now (merge 2026-11-27; HookshotVR's in oot_scene_defaults) */
static _Atomic int oot_scene_opt[7] = { OOT_CUT_SCREEN, OOT_VANTAGE_DIRECTOR, OOT_LIGHT_EVERY, 1, OOT_REALIGN_OFF, 0, 0 };
void runtime_oot_vr_scene_options(const int v[7]) {
    static const char *const names[7] = { "cutscene view", "3D cutscenes", "cutscene light", "event culling by the game's camera", "re-align", "item get keeps the view", "ocarina in the hands" };
    for (int i = 0; i < 7; i++) {
        const int was = atomic_exchange(&oot_scene_opt[i], v[i]);
        if (was != v[i]) fprintf(stderr, "[vr-scene] option %s: %d\n", names[i], v[i]);
    }
}
static oot_scene_opts oot_scene_opts_now(void) {
    oot_scene_opts o = { atomic_load(&oot_scene_opt[0]), atomic_load(&oot_scene_opt[1]), atomic_load(&oot_scene_opt[2]), atomic_load(&oot_scene_opt[3]),
                         atomic_load(&oot_scene_opt[4]), atomic_load(&oot_scene_opt[5]), atomic_load(&oot_scene_opt[6]) };
    return o;
}

/* From Link's update: the scene (the raw word, and once it has played 0.3 s), its name, the palette's name. */
static pthread_mutex_t oot_scene_mu = PTHREAD_MUTEX_INITIALIZER;
static char oot_scene_name[64], oot_scene_palette_name[OOT_PALETTE_NAME];
static _Atomic int oot_scene_kind = OOT_EVENT_NONE, oot_scene_on, oot_scene_style_now = OOT_STYLE_EYES, oot_scene_item_may;
static _Atomic uint64_t oot_scene_item_shown_at;   /* the graphics side shows an item-get item before the eyes (oot_vr_itemget.inc) */
static oot_realign oot_scene_realign;               /* under oot_scene_mu */

int runtime_oot_vr_scene_style(void) { return atomic_load(&oot_scene_style_now); }
int runtime_oot_vr_scene_flat(void) { return atomic_load(&oot_scene_style_now) == OOT_STYLE_FLAT; }
/* An item get may play now (any scene but a story cinematic or the ocarina), for the graphics side's item-get item. */
int runtime_oot_vr_item_get_may(void) { return atomic_load(&oot_scene_item_may) && atomic_load(&oot_scene_opt[5]); }
void runtime_oot_vr_item_shown(void) { atomic_store(&oot_scene_item_shown_at, oot_vr_now()); }

/* The palette the WorldMgr shows, "" none; 0 when the WorldMgr is not readable. */
static int oot_scene_palette_read(uintptr_t *world, char out[OOT_PALETTE_NAME]) {
    uint64_t holder = 0, wm = 0, text = 0; uint32_t slot = 0;
    out[0] = 0; *world = 0;
    if (!oot_vr_safe_read((uintptr_t)oot_vr_base + OOT_WORLD_SLOT, &holder, 8) || !holder || !oot_vr_safe_read((uintptr_t)holder, &wm, 8) || !wm) return 0;
    if (!oot_vr_safe_read((uintptr_t)wm + 0x3128, &slot, 4)) return 0;
    const uintptr_t s = (uintptr_t)wm + 0xde8 + (slot < 2 ? (uintptr_t)slot * 0x11a0 : 0);
    if (!oot_vr_safe_read(s + 0xd88, &text, 8) || text < 0x100000000ull) return 0;
    for (int n = OOT_PALETTE_NAME - 1; n >= 7; n -= 8) { if (oot_vr_safe_read((uintptr_t)text, out, (size_t)n)) { out[n] = 0; break; } out[0] = 0; }
    for (char *c = out; *c; c++) if (*c < 32 || *c > 126) { *c = 0; break; }
    *world = (uintptr_t)wm;
    return 1;
}

/* Link's update, after the scene's name was read (oot_vr_state_read): what kind of scene, whether it has played 0.3 s, whether an
   item get may play; Link's facing for the re-align. */
static void oot_scene_link(uintptr_t actor, const float feet[3], int playing, const char *name, int kind) {
    char palette[OOT_PALETTE_NAME] = ""; uintptr_t world;
    if (playing) (void)oot_scene_palette_read(&world, palette);   /* only while a scene plays: each read is a checked system call */
    static char story_name[64]; static int story;   /* a talk lit as a story scene is one until the name changes */
    if (!playing) story = 0;
    else if (kind == OOT_EVENT_TALK && oot_palette_is_story(palette)) {
        if (!story || strcmp(story_name, name)) { story = 1; snprintf(story_name, sizeof story_name, "%s", name); fprintf(stderr, "[vr-scene] \"%s\" is lit as a story scene (%s): a story scene\n", name, palette); }
    } else if (story && strcmp(story_name, name)) story = 0;
    if (story && kind == OOT_EVENT_TALK) kind = OOT_EVENT_STORY;
    static uint64_t since;
    const uint64_t now = oot_vr_now();
    if (playing) { if (!since) since = now; } else since = 0;
    atomic_store(&oot_scene_kind, playing ? kind : OOT_EVENT_NONE);
    atomic_store(&oot_scene_on, playing && now - since > 300000000ull);
    { const int may = playing && oot_item_get_may_play(kind, name); if (atomic_exchange(&oot_scene_item_may, may) != may && may) fprintf(stderr, "[vr-scene] an item get may play (\"%s\")\n", name); }
    pthread_mutex_lock(&oot_scene_mu);
    snprintf(oot_scene_name, sizeof oot_scene_name, "%s", playing ? name : "");
    oot_palette_copy(oot_scene_palette_name, palette);
    float fx = 0, fz = 0; memcpy(&fx, (const void *)(actor + 0x330), 4); memcpy(&fz, (const void *)(actor + 0x348), 4);
    const float level = sqrtf(fx * fx + fz * fz);
    if (isfinite(level) && level > 0.5f && level < 1.01f) {
        const uint64_t moved = vr_oot_stick_moved_at ? vr_oot_stick_moved_at() : 0;
        const int stick = moved && now - moved < 100000000ull;
        const uint64_t shown = atomic_load(&oot_scene_item_shown_at);
        const int item = atomic_load(&oot_scene_opt[5]) && (kind == OOT_EVENT_ITEM_GET || (shown && now - shown < 250000000ull));
        const float was = oot_scene_realign.turn;
        oot_realign_note(&oot_scene_realign, now, atan2f(fx, fz), feet, atomic_load(&oot_scene_on), stick, item);
        static int lines;
        if (fabsf(oot_scene_realign.turn - was) > 0.0872665f && lines < 80) { lines++; fprintf(stderr, "[vr-scene] Link turned by the game: %.0f degrees in all so far (due %d)\n", (double)(oot_scene_realign.turn * 57.29578f), oot_scene_realign.due); }
    }
    pthread_mutex_unlock(&oot_scene_mu);
}

/* Link's hit-sensor update (the shared hook chain, the game's own thread): the palette calls. */
static oot_guest_call oot_scene_call;
static const char *oot_scene_call_name;
static char oot_scene_call_text[OOT_PALETTE_NAME];
static oot_guest_call *oot_scene_calc(void) {
    static oot_palette p; static int said_scene;
    char current[OOT_PALETTE_NAME]; uintptr_t world;
    const int playing = atomic_load(&oot_scene_kind) != OOT_EVENT_NONE, style = atomic_load(&oot_scene_style_now);
    if (!playing && !p.active) { said_scene = 0; memset(&p, 0, sizeof p); return NULL; }   /* nothing of ours to undo */
    if (!oot_scene_palette_read(&world, current)) return NULL;
    if (playing && !said_scene) { said_scene = 1; fprintf(stderr, "[vr-scene] Link's hit-sensor update runs while the scene plays: its lighting can be followed (palette \"%s\")\n", current); }
    if (!playing) said_scene = 0;
    const int mode = oot_light_mode(style, atomic_load(&oot_scene_opt[2]), !oot_scene_fp());
    const char *name = NULL;
    const int act = oot_palette_step(&p, mode, playing, current, &name);
    if (act == OOT_PAL_NONE) return NULL;
    if (act == OOT_PAL_SET) {
        oot_palette_copy(oot_scene_call_text, name); oot_scene_call_name = oot_scene_call_text;
        oot_scene_call = (oot_guest_call){ (uint64_t)((uintptr_t)oot_vr_base + OOT_PALETTE_SET), world, (uint64_t)(uintptr_t)&oot_scene_call_name, 0, 0, 0, NULL, 0 };
    } else oot_scene_call = (oot_guest_call){ (uint64_t)((uintptr_t)oot_vr_base + OOT_PALETTE_RESET), world, 0, 0, 0, 0, NULL, 0 };
    static int lines;
    if (lines < 120) { lines++; fprintf(stderr, "[vr-scene] lighting (%s): the game showed \"%s\", %s%s%s\n", mode == OOT_LIGHT_HELD ? "held" : mode == OOT_LIGHT_AREA ? "the area's" : "every shot",
                                       current, act == OOT_PAL_SET ? "set \"" : "reset to the area's", act == OOT_PAL_SET ? oot_scene_call_text : "", act == OOT_PAL_SET ? "\"" : ""); }
    return &oot_scene_call;
}

/* The camera hook (the game's camera thread). */
static struct {
    int far, have_spot, have_eye, wrote, last_style, cut_was;
    float spot[3], eye_outside[3], director_yaw;
    uint64_t eye_at, last_at;
    oot_seat seat;
    float wrote_f[9]; double wrote_d[6]; int wrote_doubles;
} oot_scene;
/* Before anything reads the view: what we wrote into its camera fields last frame goes back to the game's, where the game did not
   rewrite it (it does each frame: main+0x4c4e58..0x4c4ed0). */
static void oot_scene_cull_undo(uint8_t *view, const float game_f[9], const double game_d[6]) {
    if (!oot_scene.wrote) return;
    oot_scene.wrote = 0;
    float now_f[9]; double now_d[6]; memcpy(now_f, view + 0x38, sizeof now_f); memcpy(now_d, view + 0x60, sizeof now_d);
    int put = 0;
    for (int i = 0; i < 9; i++) if (!memcmp(&now_f[i], &oot_scene.wrote_f[i], 4)) { memcpy(view + 0x38 + 4 * i, &game_f[i], 4); put++; }
    for (int i = 0; oot_scene.wrote_doubles && i < 6; i++) if (!memcmp(&now_d[i], &oot_scene.wrote_d[i], 8)) { memcpy(view + 0x60 + 8 * i, &game_d[i], 8); put++; }
    static int said; if (put && said < 3) { said++; fprintf(stderr, "[vr-scene] %d of the camera's fields were still ours: put back\n", put); }
}
static float oot_scene_saved_f[9]; static double oot_scene_saved_d[6];
/* The style for this frame; first: the camera hook's start, with the camera fields as the game left them. */
static int oot_scene_camera_style(uint8_t *view, const vr_cam_state *control) {
    oot_scene_cull_undo(view, oot_scene_saved_f, oot_scene_saved_d);
    const int kind = atomic_load(&oot_scene_kind), on = atomic_load(&oot_scene_on), fp = oot_scene_fp();
    const oot_scene_opts o = oot_scene_opts_now();
    float feet[3]; uint64_t age = 0; const int have_feet = runtime_oot_vr_feet(feet, &age) && age < 500000000ull;
    const float eye_h = isfinite(control->fp_up[1]) && control->fp_up[1] > 0.1f ? control->fp_up[1] : 1.0f;
    const uint64_t now = oot_vr_now();
    if (have_feet && kind != OOT_EVENT_STORY) { oot_scene.eye_outside[0] = feet[0]; oot_scene.eye_outside[1] = feet[1] + eye_h; oot_scene.eye_outside[2] = feet[2]; oot_scene.eye_at = now; oot_scene.have_eye = 1; }
    int style = OOT_STYLE_EYES;
    if (on && kind != OOT_EVENT_NONE) {
        float pos[3]; memcpy(pos, view + 0x38, sizeof pos);
        if (have_feet) { const float d[3] = { pos[0] - feet[0], pos[1] - feet[1] - eye_h, pos[2] - feet[2] }; oot_scene_far(&oot_scene.far, sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2])); }
        if (!oot_scene.have_spot) {
            if (oot_scene.have_eye && now - oot_scene.eye_at < 500000000ull) { memcpy(oot_scene.spot, oot_scene.eye_outside, sizeof oot_scene.spot); oot_scene.have_spot = 1; }
            else if (have_feet) { oot_scene.spot[0] = feet[0]; oot_scene.spot[1] = feet[1] + eye_h; oot_scene.spot[2] = feet[2]; oot_scene.have_spot = 1; }
        }
        style = oot_scene_style(kind, &o, fp, oot_scene.far, oot_scene.have_spot);
    } else { oot_scene.far = 0; oot_scene.have_spot = 0; oot_scene.seat.have = 0; }
    if (!fp) { oot_scene.have_spot = 0; oot_scene.seat.have = 0; }
    if (style != OOT_STYLE_EYES && fp) oot_scene.cut_was = 1;   /* the first-person view comes back after this: came_back */
    const int was = atomic_exchange(&oot_scene_style_now, style);
    atomic_store(&oot_vr_in_event, style != OOT_STYLE_EYES);
    if (was != style) {
        char name[64]; pthread_mutex_lock(&oot_scene_mu); memcpy(name, oot_scene_name, sizeof name); pthread_mutex_unlock(&oot_scene_mu);
        static int lines; if (lines < 200) { lines++; fprintf(stderr, "[vr-scene] %.2f s: \"%s\" (kind %d) shown %s (%s person%s)\n", (double)now / 1e9, name, kind, oot_style_name(style), fp ? "first" : "third", oot_scene.far ? ", the game's camera far" : ""); }
    }
    memcpy(oot_scene_saved_f, view + 0x38, sizeof oot_scene_saved_f); memcpy(oot_scene_saved_d, view + 0x60, sizeof oot_scene_saved_d);
    return style;
}
/* A 3D scene's vantage: the view before the head's rotation, into m. 0: none (the game's own view stays). */
static int oot_scene_vantage(const uint8_t *view, const vr_cam_state *control, int style, float m[12]) {
    float pos[3], target[3], eye[3], yaw;
    memcpy(pos, view + 0x38, sizeof pos); memcpy(target, view + 0x44, sizeof target);
    for (int i = 0; i < 3; i++) if (!isfinite(pos[i]) || !isfinite(target[i])) return 0;
    if (style == OOT_STYLE_DIRECTOR) {
        if (oot_scene_heading(pos, target, &yaw)) oot_scene.director_yaw = yaw; else yaw = oot_scene.director_yaw;
        memcpy(eye, pos, sizeof eye);
    } else if (style == OOT_STYLE_THEATRE) {
        const int had = oot_scene.seat.have; const float was[3] = { oot_scene.seat.eye[0], oot_scene.seat.eye[1], oot_scene.seat.eye[2] };
        oot_seat_step(&oot_scene.seat, pos, target);
        if (!oot_scene.seat.have) return 0;
        if (!had || memcmp(was, oot_scene.seat.eye, sizeof was)) fprintf(stderr, "[vr-scene] a theatre seat at %.2f %.2f %.2f, looking %.0f degrees\n", (double)oot_scene.seat.eye[0], (double)oot_scene.seat.eye[1], (double)oot_scene.seat.eye[2], (double)(oot_scene.seat.yaw * 57.29578f));
        memcpy(eye, oot_scene.seat.eye, sizeof eye); yaw = oot_scene.seat.yaw;
    } else if (style == OOT_STYLE_SPOT && oot_scene.have_spot) {
        memcpy(eye, oot_scene.spot, sizeof eye); yaw = oot_vr_bits_float(atomic_load(&oot_vr_yaw_bits));
    } else return 0;
    const float off[3] = { control->off[0], control->off[1], control->off[2] };
    oot_scene_view(eye, yaw, off, m);
    return 1;
}
/* The re-align for this update of the first-person view: radians to add to the heading. came_back: the view is the player's own
   again after a flat picture or a 3D vantage. */
static float oot_scene_realign_take(int style, int came_back) {
    const uint64_t now = oot_vr_now();
    const float dt = oot_scene.last_at ? (float)((now - oot_scene.last_at) / 1e9) : 0; oot_scene.last_at = now;
    const int locked = vr_oot_zl_held ? vr_oot_zl_held() : 0;
    pthread_mutex_lock(&oot_scene_mu);
    const float turn = oot_realign_take(&oot_scene_realign, now, dt, atomic_load(&oot_scene_opt[4]), locked, style != OOT_STYLE_FLAT, came_back);
    pthread_mutex_unlock(&oot_scene_mu);
    if (turn != 0) { static float said; static uint64_t at; said += turn; if (now - at > 500000000ull) { at = now; fprintf(stderr, "[vr-scene] re-align: the heading turned %.0f degrees with Link\n", (double)(-said * 57.29578f)); said = 0; } }
    return -turn;   /* Link's facing turned by d: the heading by -d */
}
/* Keep the player's own heading when the first-person view comes back after a scene (the re-align turns it), unless re-align is off. */
static int oot_scene_keeps_heading(void) { return atomic_load(&oot_scene_opt[4]) != OOT_REALIGN_OFF; }
/* After the view is written (m: the final view, with the head): the game culls from it while a 3D scene plays (option). */
static void oot_scene_cull_write(uint8_t *view, const float m[12], int style) {
    const int kind = atomic_load(&oot_scene_kind);
    if (!atomic_load(&oot_scene_on) || !oot_scene_cull_headset(style, kind, oot_scene_fp(), atomic_load(&oot_scene_opt[3]))) return;
    float eye[3]; vr_view_eye(m, eye);
    const float back[3] = { m[8], m[9], m[10] }, up[3] = { m[4], m[5], m[6] };
    const float *gp = oot_scene_saved_f, *gt = oot_scene_saved_f + 3;
    float dist = sqrtf((gp[0] - gt[0]) * (gp[0] - gt[0]) + (gp[1] - gt[1]) * (gp[1] - gt[1]) + (gp[2] - gt[2]) * (gp[2] - gt[2]));
    if (!(dist > 1e-3f) || !isfinite(dist)) dist = 1;
    float f[9];
    for (int i = 0; i < 3; i++) { f[i] = eye[i]; f[3 + i] = eye[i] - back[i] * dist; f[6 + i] = up[i]; }
    for (int i = 0; i < 9; i++) if (!isfinite(f[i])) return;
    int doubles = 1;
    for (int i = 0; i < 6; i++) if (!(fabs(oot_scene_saved_d[i] - (double)oot_scene_saved_f[i]) < 1.0)) doubles = 0;
    memcpy(view + 0x38, f, sizeof f); memcpy(oot_scene.wrote_f, f, sizeof f);
    if (doubles) { for (int i = 0; i < 6; i++) oot_scene.wrote_d[i] = f[i]; memcpy(view + 0x60, oot_scene.wrote_d, sizeof oot_scene.wrote_d); }
    oot_scene.wrote_doubles = doubles; oot_scene.wrote = 1;
    static int said; if (!said) { said = 1; fprintf(stderr, "[vr-scene] the game culls from the player's view while this scene plays (its camera's fields%s)\n", doubles ? " and their doubles" : ""); }
}
