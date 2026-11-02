// TOTK VR stage 1: a screen fixed in the room. See totk_vr.h.
// - Present thread (vr_source_encode): copies the cropped game picture into one of three textures of ours, in the
//   present's own Metal 4 command buffer; the feedback handler publishes it (vr_source_done).
// - VR thread: waits for each new head pose from the streamer (about 72 Hz), renders the newest published picture as a
//   quad at a fixed place in the room into the left and right halves of a shared IOSurface (one view per eye, the
//   headset's own field of view and IPD), waits for the GPU and sends FRAME_READY with that pose's tracking timestamp,
//   so the Quest can match it to the pose and reproject. The game's frame rate does not limit the headset's.
// The game itself is not changed: no camera, no stereo from the game yet (stages 2 and 3 of vr研究.md).
#import "totk_vr.h"
#include <sys/time.h>   // futimes: the VR activity file
#include <TargetConditionals.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define VLOG(...) do { fprintf(stderr, "[dm] [nvn-mtl] [vr] " __VA_ARGS__); fputc('\n', stderr); } while (0)

#include "../runtime/vr_camera.h"
vr_cam_state *g_vr_headlook;   // stage 2: set by the 1.4.3 camera hook's install (runtime/game_patches_v143.c)

#if !TARGET_OS_OSX
int vr_enabled(void) { return 0; }
int vr_source_encode(id<MTL4CommandBuffer> cb, id<MTLTexture> src, const int crop[4], uint64_t ticket) { (void)cb; (void)src; (void)crop; (void)ticket; return -1; }
void vr_present_queued(uint64_t ticket) { (void)ticket; }
void vr_source_done(int slot, int ok) { (void)slot; (void)ok; }
id<MTLResidencySet> vr_residency(void) { return nil; }
void vr_recenter(void) {}
int vr_wants_camera(void) { return 0; }
void vr_set_deadband(int tenths) { (void)tenths; }
int vr_first_person(void) { return 0; }
void vr_set_first_person(int on) { (void)on; }
void vr_set_eye_cm(int cm) { (void)cm; }
void vr_set_lean_cm(int cm) { (void)cm; }
void vr_set_eye_bone(int on) { (void)on; }
void vr_set_pitch_lock(int on) { (void)on; }
int vr_gamepad(int plain, uint64_t *b, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) { (void)plain; (void)b; (void)lx; (void)ly; (void)rx; (void)ry; return 0; }
int vr_mirror_mode(void) { return 0; }
void vr_set_mirror_mode(int on) { (void)on; }
void vr_mirror_crop(double out[4]) { out[0] = out[1] = 0; out[2] = out[3] = 1; }
double vr_mirror_aspect(void) { return 2; }
int vr_quest_view(void) { return 0; }
void vr_set_quest_view(int on) { (void)on; }
int vr_quest_zoom(void) { return 5; }
int vr_eye_spring_ms(void) { return 0; }
void vr_set_eye_spring_ms(int ms) { (void)ms; }
void vr_set_quest_zoom(int x) { (void)x; }
int vr_6dof(void) { return 0; }
void vr_set_6dof(int on) { (void)on; }
int vr_panel_in_headset(void) { return 0; }
int vr_motion(void) { return 0; }
void vr_set_motion(int on) { (void)on; }
void vr_panel_image(const void *bgra, int w, int h, int stride) { (void)bgra; (void)w; (void)h; (void)stride; }
int vr_snap_ms(void) { return 0; }
void vr_set_snap_ms(int ms) { (void)ms; }
int vr_offset_cm(int axis) { (void)axis; return 0; }
void vr_set_offset_cm(int axis, int cm) { (void)axis; (void)cm; }
int vr_streamer_managed(void) { return 0; }
void vr_resolution(int *w, int *h) { *w = *h = 0; }
int vr_set_resolution(int w, int h) { (void)w; (void)h; return 0; }
void vr_note_camera(const float *cam) { (void)cam; }
int vr_frame_pos(float p[3]) { (void)p; return 0; }
int vr_frame_ring(void) { return -1; }
void vr_frame_match_desc(char *out, size_t n) { if (n) out[0] = 0; }
void vr_stats(char *out, size_t n) { if (n) out[0] = 0; }
#else
#import <IOSurface/IOSurface.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include "totk_vr_link.h"
#include "../../third_party/d2r_gfx/gfx/nvn/fps_overlay.h"   /* the graphics layer (zeldaVR sits beside it in games/totk) */

enum { SRC_SLOTS = 3, IMAGES = 3, SWAPCHAIN_ID = 1 };

static struct {
    id<MTLDevice> dev; id<MTLResidencySet> rs; int failed;
    // picture copies: written by the present thread, read by the VR thread (g.mu)
    pthread_mutex_t mu;
    id<MTLTexture> src[SRC_SLOTS]; uint64_t seq[SRC_SLOTS], next_seq; int writing[SRC_SLOTS], readers[SRC_SLOTS];
    // VR thread
    id<MTLCommandQueue> q; id<MTLRenderPipelineState> ps; id<MTLSamplerState> smp;
    IOSurfaceRef surf[IMAGES]; id<MTLTexture> img[IMAGES]; uint64_t img_frame[IMAGES]; int eye_w, eye_h;
    _Atomic int recenter; float center[3], right[3], up[3];
    id<MTLTexture> hud[2]; int hud_i, hud_w, hud_h; uint64_t hud_at, hud_frames, hud_copies;   // VR thread
    // stage 2 (head look): the head pose of each rotation handed to the game, by its seq; each picture's seq
    vr_pose hl_pose[64]; uint32_t src_used[SRC_SLOTS]; float hl_ref[4]; int hl_have_ref;
    float hl_sent[4]; int hl_have_sent; _Atomic uint64_t hl_held;   // the dead band: the rotation the game has, frames it kept
    float fov_tanh, fov_aspect; int fov_on, hud_inside;   // the game's field of view set to the headset's (runtime_aspect_vr)
    _Atomic uint32_t frame_seq; _Atomic uint64_t seq_matched, seq_matched_rot, seq_missed;   // the replayed frame's head-rotation seq
    _Atomic uint32_t ticket_seq[64];   // ... carried by its present ticket to the present thread
    _Atomic uint64_t copies, frames, no_image, no_pose, gpu_errors, encoder_waits;
} g = { .mu = PTHREAD_MUTEX_INITIALIZER };

int vr_enabled(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("TOTK_VR_CONTROL"); on = e && *e; }
    return on;
}
id<MTLResidencySet> vr_residency(void) { return g.rs; }
void vr_recenter(void) { atomic_store(&g.recenter, 1); }

static uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }

// ---- math (column-major 4x4 as Metal's float4x4) ----
static void qrot(const float q[4], const float v[3], float o[3]) {   // v rotated by the unit quaternion q (xyzw)
    float x = q[0], y = q[1], z = q[2], w = q[3];
    float tx = 2 * (y * v[2] - z * v[1]), ty = 2 * (z * v[0] - x * v[2]), tz = 2 * (x * v[1] - y * v[0]);
    o[0] = v[0] + w * tx + (y * tz - z * ty); o[1] = v[1] + w * ty + (z * tx - x * tz); o[2] = v[2] + w * tz + (x * ty - y * tx);
}
/* clip = P * V * world for an eye at (q, p) with OpenXR field-of-view angles (left, right, up, down). Right-handed view
   space looking down -z; the screen quad is the only thing drawn, so depth is a constant half of w. */
static void eye_matrix(const float q[4], const float p[3], const float fov[4], float m[16]) {
    float r[3][3]; const float ax[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
    for (int i = 0; i < 3; i++) qrot(q, ax[i], r[i]);   // r[i] = world direction of the eye's axis i
    float L = tanf(fov[0]), R = tanf(fov[1]), U = tanf(fov[2]), D = tanf(fov[3]);
    float V[4][4] = {0};   // V[row][col]: view = R^T (world - p)
    for (int i = 0; i < 3; i++) { for (int j = 0; j < 3; j++) V[i][j] = r[i][j]; V[i][3] = -(r[i][0] * p[0] + r[i][1] * p[1] + r[i][2] * p[2]); }
    V[3][3] = 1;
    float P[4][4] = { { 2 / (R - L), 0, (R + L) / (R - L), 0 }, { 0, 2 / (U - D), (U + D) / (U - D), 0 }, { 0, 0, -0.5f, 0 }, { 0, 0, -1, 0 } };
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) {
        float s = 0; for (int k = 0; k < 4; k++) s += P[i][k] * V[k][j];
        m[j * 4 + i] = s;
    }
}

static void place_screen(const vr_pose *head) {
    static float dist = -1, width;
    if (dist < 0) { dist = 2.0f; width = 2.6f; const char *e = getenv("TOTK_VR_SCREEN");   // "<distance>,<width>" in metres
        if (e && sscanf(e, "%f,%f", &dist, &width) == 2 && dist > 0.3f && dist < 50 && width > 0.1f && width < 100) {} else { dist = 2.0f; width = 2.6f; } }
    float fwd[3]; const float nz[3] = { 0, 0, -1 }; qrot(head->q, nz, fwd);
    float l = sqrtf(fwd[0] * fwd[0] + fwd[2] * fwd[2]); if (l < 1e-3f) { fwd[0] = 0; fwd[2] = -1; l = 1; }
    fwd[0] /= l; fwd[2] /= l;   // level: only the head's yaw places the screen
    float height = width * 9.0f / 16.0f;
    g.center[0] = head->p[0] + fwd[0] * dist; g.center[1] = head->p[1]; g.center[2] = head->p[2] + fwd[2] * dist;
    g.right[0] = -fwd[2] * width; g.right[1] = 0; g.right[2] = fwd[0] * width;
    g.up[0] = 0; g.up[1] = height; g.up[2] = 0;
    VLOG("screen %.1f m wide, %.1f m ahead (TOTK_VR_SCREEN=<distance>,<width>)", width, dist);
}

// ---- stage 2: head look ----
static void qmul(const float a[4], const float b[4], float o[4]) {   // a * b (xyzw)
    o[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    o[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    o[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    o[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}
/* Hands the head's rotation to the game's camera (vr_camera.h): relative to the yaw the player faced when VR started
   (or at a recenter), so looking straight ahead leaves the game's camera as the stick put it. h = R^T, rows. */
/* First person (user 2026-10-26: "我要 支持第一人称"): the hook puts the camera at the player's eyes (vr_camera.h). The
   player's position field is looked up every second (the actor changes when an area loads). TOTK_VR_FIRST_PERSON=0 /
   flag vr_fp 0 / the settings panel: third person. Eyes TOTK_VR_EYE metres above his feet (1.55; flag vr_eye <cm>),
   TOTK_VR_EYE_FWD metres ahead (0.15), so his head is behind the near plane (0.21 m). */
extern uintptr_t runtime_player_pos_address(void) __attribute__((weak_import));
static _Atomic int g_fp = -1, g_eye_cm = -1;
int vr_first_person(void) { if (atomic_load(&g_fp) < 0) { const char *e = getenv("TOTK_VR_FIRST_PERSON"); atomic_store(&g_fp, !(e && !strcmp(e, "0"))); } return atomic_load(&g_fp); }
void vr_set_first_person(int on) { atomic_store(&g_fp, on != 0); VLOG("view: %s", on ? "first person" : "third person"); }
void vr_set_eye_cm(int cm) { atomic_store(&g_eye_cm, cm < 50 ? 50 : cm > 250 ? 250 : cm); VLOG("first person: eyes %d cm above the feet", atomic_load(&g_eye_cm)); }
/* Looking down moves the eyes forward (user 2026-10-29 on the Quest: "头显往下看的时候 自动 往前移动30厘米 这样可以看到 肚子"):
   along where the view points, flattened, up to g_lean_cm when it points 60 degrees or more below the horizon, eased in
   from level. flag vr_lean <cm> / TOTK_VR_LEAN (metres); 0 off. */
static _Atomic int g_lean_cm = -1;
static _Atomic int g_eye_bone = -1;
void vr_set_eye_bone(int on) { atomic_store(&g_eye_bone, on != 0); VLOG("first person: the eyes %s", on ? "may follow the head bone" : "at the fixed height"); }
void vr_set_lean_cm(int cm) { atomic_store(&g_lean_cm, cm < 0 ? 0 : cm > 100 ? 100 : cm); VLOG("first person: looking down moves the eyes up to %d cm forward", atomic_load(&g_lean_cm)); }
/* The VR page's switches, one file (TOTK_VR_OPTIONS; the launcher: <data>/vr-options.txt), a "name=value" line each:
   mirror (the Mac window), 6dof (the head's position), snap (snap turning). Read once, written whole on a change. */
static pthread_mutex_t g_opt_mu = PTHREAD_MUTEX_INITIALIZER;
static struct { char name[16]; int value; } g_opt[16]; static int g_opt_n = -1;
static void opt_load_locked(void) {
    if (g_opt_n >= 0) return;
    g_opt_n = 0;
    const char *p = getenv("TOTK_VR_OPTIONS"); FILE *f = p ? fopen(p, "r") : NULL; if (!f) return;
    char line[64];
    while (g_opt_n < 16 && fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '='); if (!eq || eq == line || eq - line >= 16) continue;
        memcpy(g_opt[g_opt_n].name, line, (size_t)(eq - line)); g_opt[g_opt_n].name[eq - line] = 0; g_opt[g_opt_n].value = atoi(eq + 1); g_opt_n++;
    }
    fclose(f);
}
static int vr_option(const char *name, int def) {
    pthread_mutex_lock(&g_opt_mu); opt_load_locked(); int v = def;
    for (int i = 0; i < g_opt_n; i++) if (!strcmp(g_opt[i].name, name)) { v = g_opt[i].value; break; }
    pthread_mutex_unlock(&g_opt_mu); return v;
}
static void vr_option_set(const char *name, int value) {
    pthread_mutex_lock(&g_opt_mu); opt_load_locked(); int i = 0;
    while (i < g_opt_n && strcmp(g_opt[i].name, name)) i++;
    if (i == g_opt_n && g_opt_n < 16) { snprintf(g_opt[i].name, sizeof g_opt[i].name, "%s", name); g_opt_n++; }
    if (i < 16) g_opt[i].value = value;
    const char *p = getenv("TOTK_VR_OPTIONS"); FILE *f = p ? fopen(p, "w") : NULL;
    if (f) { for (int j = 0; j < g_opt_n; j++) fprintf(f, "%s=%d\n", g_opt[j].name, g_opt[j].value); fclose(f); }
    pthread_mutex_unlock(&g_opt_mu);
}
/* The head's position (6DoF; "按建议 都做", 2026-10-30, after BotW BetterVR's first person: player + height + the headset's
   own movement): where the head is relative to where it was at the last recenter, in that straight-ahead frame (x right,
   y up, -z forward), turned to the game camera's yaw by offset_update and added to the camera in both views, so leaning
   and crouching move it. The VR page's "头部位置" (option 6dof, off by default since 2026-10-30); flag vr_6dof. */
static float g_hl_pos[3], g_hl_pos_ref[3]; static int g_hl_pos_have;
/* The settings panel in the headset: off by default (user 2026-10-30: "先 关闭 设置在 vr里的效果"); option panel=1 shows it. */
int vr_panel_in_headset(void) { return vr_option("panel", 0) != 0; }
int vr_6dof(void) { return vr_option("6dof", 0); }   // off by default (user 2026-10-30: "默认 头部位置 关")
void vr_set_6dof(int on) { vr_option_set("6dof", on != 0); VLOG("the head's position: %s", on ? "moves the camera (6DoF)" : "not used"); }
/* Snap turning (comfort, as BetterVR / UEVR; "按建议 都做", 2026-10-30): the game's camera has no "turn by 30 degrees", so a
   right-stick flick past 70% sends the stick fully that way for snap ms (the VR page; option "snap", 0 = smooth turning
   as before) while the headset stays dark (g_blank_until), then nothing until the stick is back near the centre. */
static _Atomic uint64_t g_blank_until;
int vr_snap_ms(void) { int v = vr_option("snap", 0); return v < 0 ? 0 : v > 600 ? 600 : v; }
void vr_set_snap_ms(int ms) { vr_option_set("snap", ms < 0 ? 0 : ms > 600 ? 600 : ms); VLOG("turning: %s (%d ms a step)", ms > 0 ? "snap" : "smooth", ms); }
/* The game's vibration on the Quest controllers ("按建议 都做", 2026-10-30): runtime/hid_feedback.c hands each value it
   gets (side 0 left, 1 right; the stronger of the low / high band) to runtime_vr_haptic. Sent for 80 ms, again when it
   changes or every 50 ms while it lasts, once with 0 when it stops. TOTK_VR_HAPTIC=0: not sent. */
static _Atomic int g_link_up;   // (defined with the VR thread below)
static void vr_haptic(int side, float amp, float freq) {
    static int on = -1; if (on < 0) { const char *e = getenv("TOTK_VR_HAPTIC"); on = !(e && !strcmp(e, "0")); }
    if (!on || side < 0 || side > 1 || !atomic_load(&g_link_up)) return;
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER; static float last[2]; static uint64_t sent[2];
    amp = amp == amp ? fmaxf(0, fminf(1, amp)) : 0; if (!(freq > 0 && freq < 2000)) freq = 160;
    const uint64_t now = now_ns();
    pthread_mutex_lock(&mu);
    int send = amp > 0.02f ? (fabsf(amp - last[side]) > 0.05f || now - sent[side] > 50000000ull) : last[side] > 0.02f;
    if (send) { last[side] = amp; sent[side] = now; }
    pthread_mutex_unlock(&mu);
    if (send) vr_link_send_haptic(side, amp > 0.02f ? 80000000ull : 0, freq, amp > 0.02f ? amp : 0);
}
/* The Quest 3 controllers as the game's controller (user 2026-10-29: "支持 quest3 手柄"; the scheme after BotW BetterVR's
   Touch bindings, "按建议 都做", 2026-10-30): their inputs come over the link (vr_link_controls), runtime/gamepad.m merges
   them like the touch screen and the mouse (runtime_vr_pad). Short / long presses give the Touch's 11 buttons the
   Switch's 16. In the world (the camera hook ran in the last 0.2 s):
     right A    tap: A (talk, pick up)      hold: B (run)
     right B    X (jump)
     left X     tap: B (cancel, let go)     hold 0.6 s: the settings panel (runtime_settings_toggle)
     left Y     L (abilities; held: the ability wheel)
     right trigger Y (attack)   left trigger ZL (shield, lock on)   right grip ZR (bow)   left grip R (throw)
     left stick click   tap: L3 (crouch)    hold: R3 (scope)
     right stick click  tap: + (menu)       hold: - (map)
     both stick clicks  recentre (straight ahead and the 6DoF origin)
     left menu  held with the right stick: the d-pad (the stick's direction; the camera does not turn meanwhile);
                tapped alone: d-pad down (whistle)
   In menus and the map, by name: A B X Y, triggers ZL / ZR, grips L / R, stick clicks L3 / R3, left menu +.
   Switch button bits: A 0, B 1, X 2, Y 3, L3 4, R3 5, L 6, R 7, ZL 8, ZR 9, + 10, - 11, left 12, up 13, right 14, down 15.
   TOTK_VR_PAD=0: not used. */
enum { SW_A = 0, SW_B, SW_X, SW_Y, SW_L3, SW_R3, SW_L, SW_R, SW_ZL, SW_ZR, SW_PLUS, SW_MINUS, SW_LEFT, SW_UP, SW_RIGHT, SW_DOWN };
typedef struct { uint64_t down_at; int held_long; uint64_t pulse_until; uint64_t pulse_bits; } press_state;
/* One button's short / long press: on a release before long_ns, short_bits for 120 ms; held past long_ns, long_bits while
   held (or for 120 ms once when long_pulse). Returns the bits to set now. */
static uint64_t press(press_state *p, int down, uint64_t now, uint64_t long_ns, uint64_t short_bits, uint64_t long_bits, int long_pulse) {
    uint64_t out = 0;
    if (down) {
        if (!p->down_at) { p->down_at = now; p->held_long = 0; }
        if (now - p->down_at >= long_ns) {
            if (!p->held_long && long_pulse) { p->pulse_bits = long_bits; p->pulse_until = now + 120000000ull; }
            p->held_long = 1;
            if (!long_pulse) out |= long_bits;
        }
    } else if (p->down_at) {
        if (!p->held_long && short_bits) { p->pulse_bits = short_bits; p->pulse_until = now + 120000000ull; }
        p->down_at = 0;
    }
    if (now < p->pulse_until) out |= p->pulse_bits;
    return out;
}
static _Atomic int g_link_up;   // (defined with the VR thread below)
static int world_camera_running(uint64_t now) {   // the camera hook counts its calls (vr_cam_state.calls)
    vr_cam_state *s = g_vr_headlook; if (!s) return 0;
    static _Atomic uint32_t last; static _Atomic uint64_t at;
    uint32_t c = __atomic_load_n(&s->calls, __ATOMIC_RELAXED);
    if (c != atomic_load(&last)) { atomic_store(&last, c); atomic_store(&at, now); }
    return now - atomic_load(&at) < 200000000ull;
}
/* Motion controls (user 2026-10-30: "做体感操作"): gestures become the game's buttons; Link's arms do not follow the hands
   (that is BotW BetterVR's thousands of hours of skeleton work). Positions are taken relative to the head, in its level
   frame (forward, right, up; metres), from the controllers' poses over the link (vr_link_hands, the head's space):
     swing      the right hand faster than 2.2 m/s (not while drawing the bow): attack (Y), at most every 0.3 s
     shield     the left hand raised in front of the face: guard (ZL)
     bow        the left hand reaching forward and the right hand at the face: the right trigger draws (ZR) instead of attacking
     whistle    both hands at the mouth with both grips: d-pad down
     shoulders  a hand over its shoulder (behind the head) with its grip: d-pad right (weapons) / left (shields), a short
                buzz when the hand gets there
   The VR page's "体感操作" (option motion, on by default). */
typedef struct { int bow, shield, swing, slot_l, slot_r, whistle; } gesture_state;
int vr_motion(void) { return vr_option("motion", 1); }
void vr_set_motion(int on) { vr_option_set("motion", on != 0); VLOG("motion controls: %s", on ? "on" : "off"); }
static void gestures(uint64_t now, const vr_controls *c, gesture_state *gs) {
    memset(gs, 0, sizeof *gs);
    static uint64_t swing_at, shield_since; static int was_slot[2];
    if (!vr_motion()) return;
    vr_hand h[2]; vr_link_hands(h); vr_pose hp;
    if (!vr_link_pose(&hp) || !h[0].have || !h[1].have) return;
    float f[3]; const float nz[3] = { 0, 0, -1 }; qrot(hp.q, nz, f);
    float fl = sqrtf(f[0] * f[0] + f[2] * f[2]); if (fl < 1e-3f) return;
    const float fx = f[0] / fl, fz = f[2] / fl, rx = -fz, rz = fx;   // level forward and right
    float lf[2], lr[2], lu[2], mouth[2], head[2];
    for (int i = 0; i < 2; i++) {
        const float d[3] = { h[i].p[0] - hp.p[0], h[i].p[1] - hp.p[1], h[i].p[2] - hp.p[2] };
        lf[i] = d[0] * fx + d[2] * fz; lr[i] = d[0] * rx + d[2] * rz; lu[i] = d[1];
        const float m[3] = { lf[i] - 0.08f, lr[i], lu[i] + 0.08f };   // the mouth: 8 cm in front of and below the head's centre
        mouth[i] = sqrtf(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
        head[i] = sqrtf(lf[i] * lf[i] + lr[i] * lr[i] + lu[i] * lu[i]);
    }
    gs->bow = lf[0] > 0.35f && head[1] < 0.30f;
    gs->whistle = mouth[0] < 0.22f && mouth[1] < 0.22f;
    /* raised to the face, not where hands rest while playing with the sticks (chest and waist, 0.4-0.6 m below the head) */
    const int shield = !gs->bow && lf[0] > 0.25f && lu[0] > -0.30f && lu[0] < 0.15f && fabsf(lr[0]) < 0.40f;
    if (shield) { if (!shield_since) shield_since = now; gs->shield = now - shield_since > 100000000ull; } else shield_since = 0;
    const float speed = sqrtf(h[1].v[0] * h[1].v[0] + h[1].v[1] * h[1].v[1] + h[1].v[2] * h[1].v[2]);
    if (!gs->bow && speed > 2.2f && now - swing_at > 300000000ull && !(c->rgrip > 0.5f)) { swing_at = now; }
    gs->swing = now - swing_at < 120000000ull;
    gs->slot_r = lf[1] < 0.0f && lu[1] > -0.30f && lr[1] > 0.0f;
    gs->slot_l = lf[0] < 0.0f && lu[0] > -0.30f && lr[0] < 0.0f;
    const int slot[2] = { gs->slot_l, gs->slot_r };
    for (int i = 0; i < 2; i++) { if (slot[i] && !was_slot[i]) vr_link_send_haptic(i, 30000000ull, 200, 0.4f); was_slot[i] = slot[i]; }   // a buzz on reaching a shoulder
}
int vr_gamepad(int plain, uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {   // plain: the settings panel (by name)
    static int on = -1; if (on < 0) { const char *e = getenv("TOTK_VR_PAD"); on = !(e && !strcmp(e, "0")); }
    vr_controls c;
    if (!on || !atomic_load(&g_link_up) || !vr_link_controls(&c)) return 0;
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    static press_state pa, px, pls, prs, pmenu; static int said, menu_stick, settings_sent;
    const uint64_t now = now_ns(), B = 1;
    const float dz = 0.12f;   // the Touch sticks rest a little off centre
    float ax[4] = { c.lx, c.ly, c.rx, c.ry };
    for (int k = 0; k < 4; k++) ax[k] = fabsf(ax[k]) < dz ? 0 : (ax[k] - copysignf(dz, ax[k])) / (1 - dz);
    /* TOTK_VR_ACTIVITY=<file>: touched (at most every 5 s) while a Quest button is down or a stick is off centre, so the
       idle watchdog that closes a game nobody plays sees play in the headset too (2026-10-31: it saw only the Mac's
       keyboard, mouse and controllers and closed a game the user was flying in). */
    { static uint64_t touched; static const char *path; static int looked;
      if (!looked) { looked = 1; path = getenv("TOTK_VR_ACTIVITY"); if (path && !*path) path = NULL; }
      if (path && (c.buttons || ax[0] || ax[1] || ax[2] || ax[3]) && now - touched > 5000000000ull) {
          touched = now; int fd = open(path, O_WRONLY | O_CREAT, 0644); if (fd >= 0) { futimes(fd, NULL); close(fd); } } }
    uint64_t b = 0;
    pthread_mutex_lock(&mu);
    if (!said) { said = 1; VLOG("Quest controllers: on (in the world: A tap A / hold run, B jump, X tap cancel / hold the settings, Y abilities, triggers attack / shield, grips bow / throw, menu + right stick the d-pad)"); }
    if (!plain && world_camera_running(now)) {
        b |= press(&pa, c.buttons & VR_PAD_A, now, 400000000ull, B << SW_A, B << SW_B, 0);
        if (c.buttons & VR_PAD_B) b |= B << SW_X;
        b |= press(&px, c.buttons & VR_PAD_X, now, 600000000ull, B << SW_B, 0, 0);
        if (px.down_at && px.held_long && !settings_sent) { settings_sent = 1; extern void runtime_settings_toggle(void); runtime_settings_toggle(); }
        if (!px.down_at) settings_sent = 0;
        if (c.buttons & VR_PAD_Y) b |= B << SW_L;
        gesture_state gs; gestures(now, &c, &gs);
        if (c.rtrig > 0.5f) b |= B << (gs.bow ? SW_ZR : SW_Y);   // drawing the bow: the trigger holds ZR, letting go shoots
        if (gs.swing) b |= B << SW_Y;
        if (c.ltrig > 0.5f || gs.shield) b |= B << SW_ZL;
        if (gs.whistle) { if (c.lgrip > 0.5f && c.rgrip > 0.5f) b |= B << SW_DOWN; }
        else {
            if (c.rgrip > 0.5f) b |= B << (gs.slot_r ? SW_RIGHT : SW_ZR);
            if (c.lgrip > 0.5f) b |= B << (gs.slot_l ? SW_LEFT : SW_R);
        }
        /* Both stick clicks together: straight ahead is where the head looks now, the 6DoF origin too (user 2026-10-30:
           "同时 按下 l3 r3 就自动 “对准正前方”"); neither click goes to the game until both are up again. */
        static int both;
        if ((c.buttons & VR_PAD_LSTICK) && (c.buttons & VR_PAD_RSTICK)) {
            if (!both) { both = 1; vr_recenter(); VLOG("both stick clicks: recentred (straight ahead and the head's position)"); }
            pls.down_at = prs.down_at = 0; pls.pulse_until = prs.pulse_until = 0;
        } else if (both && !(c.buttons & (VR_PAD_LSTICK | VR_PAD_RSTICK))) both = 0;
        if (!both) {
            b |= press(&pls, c.buttons & VR_PAD_LSTICK, now, 400000000ull, B << SW_L3, B << SW_R3, 1);
            b |= press(&prs, c.buttons & VR_PAD_RSTICK, now, 400000000ull, B << SW_PLUS, B << SW_MINUS, 1);
        }
        {   // snap turning: the right stick's left / right as steps (not while the menu button makes it the d-pad)
            static int armed = 1; static uint64_t turn_until; static float dir;
            const int snap = vr_snap_ms();
            if (snap > 0 && !(c.buttons & VR_PAD_MENU)) {
                if (now < turn_until) ax[2] = dir;
                else {
                    if (armed && fabsf(ax[2]) > 0.7f) { armed = 0; dir = ax[2] > 0 ? 1 : -1; turn_until = now + (uint64_t)snap * 1000000ull;
                                                       atomic_store(&g_blank_until, turn_until + 60000000ull); ax[2] = dir; }
                    else { if (fabsf(ax[2]) < 0.3f) armed = 1; ax[2] = 0; }
                }
            }
        }
        if (c.buttons & VR_PAD_MENU) {   // the d-pad from the right stick while the menu button is held
            if (!pmenu.down_at) { pmenu.down_at = now; menu_stick = 0; }
            if (fabsf(ax[2]) > 0.5f || fabsf(ax[3]) > 0.5f) {
                menu_stick = 1;
                if (fabsf(ax[2]) > fabsf(ax[3])) b |= B << (ax[2] > 0 ? SW_RIGHT : SW_LEFT); else b |= B << (ax[3] > 0 ? SW_UP : SW_DOWN);
            }
            ax[2] = ax[3] = 0;
        } else if (pmenu.down_at) {
            if (!menu_stick && now - pmenu.down_at < 600000000ull) { pmenu.pulse_bits = B << SW_DOWN; pmenu.pulse_until = now + 120000000ull; }   // a tap: whistle
            pmenu.down_at = 0;
        }
        if (now < pmenu.pulse_until) b |= pmenu.pulse_bits;
    } else {   // menus, the map, cut scenes: by name
        pa.down_at = px.down_at = pls.down_at = prs.down_at = pmenu.down_at = 0; settings_sent = 0;
        if (c.buttons & VR_PAD_A) b |= B << SW_A;
        if (c.buttons & VR_PAD_B) b |= B << SW_B;
        if (c.buttons & VR_PAD_X) b |= B << SW_X;
        if (c.buttons & VR_PAD_Y) b |= B << SW_Y;
        if (c.buttons & VR_PAD_LSTICK) b |= B << SW_L3;
        if (c.buttons & VR_PAD_RSTICK) b |= B << SW_R3;
        if (c.buttons & VR_PAD_MENU) b |= B << SW_PLUS;
        if (c.ltrig > 0.5f) b |= B << SW_ZL;
        if (c.rtrig > 0.5f) b |= B << SW_ZR;
        if (c.lgrip > 0.5f) b |= B << SW_L;
        if (c.rgrip > 0.5f) b |= B << SW_R;
    }
    pthread_mutex_unlock(&mu);
    *buttons = b; *lx = (int32_t)(ax[0] * 32767); *ly = (int32_t)(ax[1] * 32767); *rx = (int32_t)(ax[2] * 32767); *ry = (int32_t)(ax[3] * 32767);
    return 1;
}
/* The camera's offset (user 2026-10-29: "设置里面 添加新功能 可以改变 摄像头 上下前后左右 位置 ，方便我 实时调整"; the VR page's
   sliders, -100..100 cm): up, forward and right of where the game's camera faces (its yaw, not the head's, so turning the
   head does not swing the camera), in both views. Kept in TOTK_VR_OFFSET (the launcher: <data>/vr-offset.txt;
   "up forward right" in cm). */
static _Atomic int g_off_cm[3] = { -1000, -1000, -1000 };   // up, forward, right; -1000 = not read yet
static void offset_load(void) {
    if (atomic_load(&g_off_cm[0]) != -1000) return;
    int v[3] = { 0, 0, 0 }; const char *p = getenv("TOTK_VR_OFFSET"); FILE *f = p ? fopen(p, "r") : NULL;
    if (f) { if (fscanf(f, "%d %d %d", &v[0], &v[1], &v[2]) != 3) v[0] = v[1] = v[2] = 0; fclose(f); }
    for (int i = 0; i < 3; i++) atomic_store(&g_off_cm[i], v[i] < -100 ? -100 : v[i] > 100 ? 100 : v[i]);
    if (v[0] || v[1] || v[2]) VLOG("camera offset: up %d, forward %d, right %d cm (%s)", v[0], v[1], v[2], p);
}
int vr_offset_cm(int axis) { offset_load(); return axis >= 0 && axis < 3 ? atomic_load(&g_off_cm[axis]) : 0; }
void vr_set_offset_cm(int axis, int cm) {
    if (axis < 0 || axis > 2) return;
    offset_load(); atomic_store(&g_off_cm[axis], cm < -100 ? -100 : cm > 100 ? 100 : cm);
    const char *p = getenv("TOTK_VR_OFFSET"); FILE *f = p ? fopen(p, "w") : NULL;
    if (f) { fprintf(f, "%d %d %d\n", atomic_load(&g_off_cm[0]), atomic_load(&g_off_cm[1]), atomic_load(&g_off_cm[2])); fclose(f); }
    static const char *const names[3] = { "up", "forward", "right" }; VLOG("camera offset: %s %d cm", names[axis], atomic_load(&g_off_cm[axis]));
}
static void offset_update(vr_cam_state *s, float d[3]) {   // world axes from the game camera's right axis (the hook's game_right)
    offset_load();
    float r[4]; memcpy(r, s->game_right, sizeof r);
    const float h = sqrtf(r[0] * r[0] + r[2] * r[2]), up = atomic_load(&g_off_cm[0]) / 100.0f, fw = atomic_load(&g_off_cm[1]) / 100.0f, rt = atomic_load(&g_off_cm[2]) / 100.0f;
    d[0] = d[1] = d[2] = 0;
    float rt6 = rt, up6 = up, fw6 = fw;
    if (g_hl_pos_have && vr_6dof()) {   // the head's movement (-z is forward), at most a metre each way
        float q[3]; for (int i = 0; i < 3; i++) q[i] = g_hl_pos[i] == g_hl_pos[i] ? fmaxf(-1, fminf(1, g_hl_pos[i])) : 0;
        rt6 += q[0]; up6 += q[1]; fw6 -= q[2];
    }
    if (h > 0.125f) { const float rx = r[0] / h, rz = r[2] / h; d[0] = rt6 * rx + fw6 * rz; d[1] = up6; d[2] = rt6 * rz - fw6 * rx; }   // forward = (r.z, 0, -r.x)
    const float o[4] = { d[0], d[1], d[2], 0 }; memcpy(s->off, o, sizeof o);
}
int vr_eye_spring_ms(void); extern _Atomic int g_spring_dev_mm;   // (below, with the VR page's options)
static void first_person_update(void) {   // VR thread, each frame (the lookup once a second)
    vr_cam_state *s = g_vr_headlook;
    if (!s) return;
    float d[3]; offset_update(s, d);
    extern _Atomic int g_vr_hidehead_fp;   // nvn_mtl.m: hide the player's head while the camera is in it
    extern _Atomic int g_vr_eye_track; extern int vr_head_eye(float off[3], uint64_t max_age_ns);   // nvn_mtl.m: the eyes on the head bone
    /* The switch eased over 0.6 s (user 2026-11-01: "p键 切换 第一 第三人称的时候 要 平滑 过度 0.6秒吧"): b runs from 0 (third
       person) to 1 (the eyes) or back at 1/0.6 per second; the hook moves the camera smoothstep(b) of the way from the
       third-person camera to the eyes (vr_camera.h fp_blend). His head stays hidden while the camera is nearer the eyes
       (b >= 0.5), so it is never seen from inside. At start the view is taken as it is, without a move. */
    const int target = vr_first_person() && runtime_player_pos_address;
    static float b = -1; static uint64_t b_at;
    { const uint64_t t = now_ns(); if (b < 0) b = target ? 1 : 0;
      else { const float step = (float)((t - b_at) / 1e9) / 0.6f; b = target ? fminf(1, b + step) : fmaxf(0, b - step); }
      b_at = t; }
    if (!target && b <= 0) { __atomic_store_n(&s->fp_pos, 0, __ATOMIC_RELEASE); __atomic_store_n(&s->fp_lag, 0, __ATOMIC_RELEASE); atomic_store(&g_vr_hidehead_fp, 0); atomic_store(&g_vr_eye_track, 0); return; }
    s->fp_blend = b * b * (3 - 2 * b);
    if (atomic_load(&g_eye_cm) < 0) { const char *e = getenv("TOTK_VR_EYE"); atomic_store(&g_eye_cm, e ? (int)(atof(e) * 100 + 0.5f) : 170); }   // 170: the user on the Quest, 2026-10-29 (155 before)
    static float fwd = -1; if (fwd < 0) { const char *e = getenv("TOTK_VR_EYE_FWD"); fwd = e ? (float)atof(e) : 0.15f; if (fwd < 0 || fwd > 1) fwd = 0.15f; }
    /* The eyes where the head is (user 2026-10-29 "眼睛跟着头骨"): the head mesh's bone gives the offset from the player's
       position, each frame it is drawn (vr_head_eye_note); without one for 0.25 s (head not drawn: a cut scene, another
       outfit) the fixed height as before. TOTK_VR_EYE_BONE=0: always the fixed height. */
    /* On by default (user 2026-10-30: "眼睛跟头骨 不变", "改成眼睛"): read where the eyes' mesh is drawn. TOTK_VR_EYE_BONE=0 /
       "flag vr_eyebone 0": the fixed height. */
    if (atomic_load(&g_eye_bone) < 0) { const char *e = getenv("TOTK_VR_EYE_BONE"); atomic_store(&g_eye_bone, !(e && !strcmp(e, "0"))); }
    const int bone_eye = atomic_load(&g_eye_bone);
    atomic_store(&g_vr_eye_track, 1);   // the replay reads the body every frame: where it is drawn (below) and, with the bone, the head
    /* The eyes' mesh is not drawn every frame in first person (user 2026-10-30: "为什么 屏幕 闪烁": with a 0.25 s limit the
       camera switched 1,218 times between the eyes and the fixed height): the last place is kept up to 3 s, and the camera
       moves a third of the way to a new one each frame (72 Hz), so two sources a few cm apart do not shake it. */
    float off[3]; static int was = -1; static float sm[3]; static int sm_have;
    const int on = bone_eye && vr_head_eye(off, 3000000000ull);
    if (on) {
        /* Bound to the head (user 2026-11-01: "忘记所有以前的要求！ 我要求 vr镜头 和头绑定！！！"; running, his head leans 0.3 m
           ahead of his feet and a camera on the feet showed his body ahead of it): the eyes in all three directions, no dead
           band (it was 40 cm, user 2026-10-30 on crouching) and no smoothing (it trailed the head). TOTK_VR_EYE_BAND=<metres>
           and TOTK_VR_EYE_SMOOTH=<0..1, the share taken each frame> bring them back. */
        static float band = -1, take = -1;
        if (band < 0) { const char *e = getenv("TOTK_VR_EYE_BAND"); band = e ? (float)atof(e) : 0; if (!(band >= 0 && band < 3)) band = 0; }
        if (take < 0) { const char *e = getenv("TOTK_VR_EYE_SMOOTH"); take = e ? (float)atof(e) : 1; if (!(take > 0 && take <= 1)) take = 1; }
        const float hl = sqrtf(off[0] * off[0] + off[2] * off[2]);
        const float kept = hl > band ? (hl - band) / hl : 0;
        off[0] *= kept; off[2] *= kept;
        /* The spring (user 2026-11-02, "用 弹性 解决" the shaking; it replaces the rule of 2026-11-01 "no smoothing"): only
           the eyes' offset from his position goes through it, a critically damped spring of time constant vr_eye_spring_ms
           (4 ms steps); his position itself, carried with the drawn body (carry, fp_lag), stays exact, so running and
           flying the camera keeps up with him and is off his head by at most the bob it takes out (logged). A jump over
           0.5 m (another outfit, a cut scene's end) is taken at once. TOTK_VR_EYE_SMOOTH (the old share a frame) wins. */
        static float sv[3]; static uint64_t sp_at; const uint64_t tn = now_ns();
        float dt = sp_at ? (float)((tn - sp_at) / 1e9) : 0; sp_at = tn; if (dt > 0.1f) dt = 0.1f;
        const float jump = sqrtf((off[0] - sm[0]) * (off[0] - sm[0]) + (off[1] - sm[1]) * (off[1] - sm[1]) + (off[2] - sm[2]) * (off[2] - sm[2]));
        if (!sm_have || jump > 0.5f) { memcpy(sm, off, sizeof sm); memset(sv, 0, sizeof sv); sm_have = 1; }
        const int tau = vr_eye_spring_ms();
        if (take < 1 || !tau) { for (int i = 0; i < 3; i++) { sm[i] += (off[i] - sm[i]) * take; sv[i] = 0; } }
        else {
            const float w = 1000.0f / tau;
            for (float left = dt; left > 1e-6f; left -= 0.004f) { const float h = fminf(left, 0.004f);
                for (int i = 0; i < 3; i++) { const float a = w * w * (off[i] - sm[i]) - 2 * w * sv[i]; sv[i] += a * h; sm[i] += sv[i] * h; } }
            const int mm = jump > 0.5f ? 0 : (int)lroundf(jump * 1000); for (int v = atomic_load(&g_spring_dev_mm); mm > v && !atomic_compare_exchange_weak(&g_spring_dev_mm, &v, mm); ) ;   // a jump taken at once is no offset kept
        }
        for (int i = 0; i < 3; i++) off[i] = sm[i];
    } else sm_have = 0;
    /* Bound to the body (user 2026-10-31: "vr摄像头 没有和 眼睛的位置绑定": in a Zonai device his body flew off while the
       camera stayed at the position read): where the replay draws the body against the position that frame's camera was
       put at (nvn_mtl.m vr_body_carry), in full; walking it is about 0. Half the way each 72 Hz frame; without one for
       0.5 s (a cut scene, no body), none. */
    /* With the lag (user 2026-11-01: "任何时候 不能出现 头 身体 分离"): the hook takes the position of the update the body is
       drawn at (fp_lag, vr_body_lag) and the small rest is added as it is, so the camera is on the drawn body when he speeds
       up too (before, a gap measured frames ago: 0.5-0.7 m off for the first 0.3 s of a run). TOTK_VR_LAG=0: the gap only. */
    extern int vr_body_carry(float off[3], uint64_t max_age_ns);
    extern int vr_body_lag(int *lag, float lr[3], uint64_t max_age_ns);
    float carry[3] = {0, 0, 0}; int lag = 0;
    static int bind = -1; if (bind < 0) { const char *e = getenv("TOTK_VR_BIND"); bind = !(e && !strcmp(e, "0")); }   // TOTK_VR_BIND=0: as before (A/B)
    static int use_lag = -1; if (use_lag < 0) { const char *e = getenv("TOTK_VR_LAG"); use_lag = !(e && !strcmp(e, "0")); }
    if (bind && !(use_lag && vr_body_lag(&lag, carry, 500000000ull))) { lag = 0; if (!vr_body_carry(carry, 500000000ull)) memset(carry, 0, sizeof carry); }
    if (!bind) lag = 0;
    __atomic_store_n(&s->fp_lag, (uint32_t)lag, __ATOMIC_RELEASE);
    if (on != was) { extern uint64_t vr_head_eye_age_ms(void); VLOG("first person: the eyes %s (eye data %llu ms old)", on ? "follow the head bone" : "at a fixed height above the feet", (unsigned long long)vr_head_eye_age_ms()); was = on; }
    /* the height setting is the fixed height without the head (cut scenes, another outfit); following the head, the eyes'
       own height (user 2026-11-01: bound to the head; it added 170 - 155 cm on top before) */
    const float eye = atomic_load(&g_eye_cm) / 100.0f;
    float up[4] = { carry[0] + (on ? off[0] : 0) + d[0], carry[1] + (on ? off[1] : eye) + d[1], carry[2] + (on ? off[2] : 0) + d[2], 0 };
    if (atomic_load(&g_lean_cm) < 0) { const char *e = getenv("TOTK_VR_LEAN"); atomic_store(&g_lean_cm, e ? (int)(atof(e) * 100 + 0.5f) : 15); }   // 15: the user on the Quest, 2026-10-29 (tried 30)
    if (!on) {   // looking down, forward by the lean (fixed height only: on the head, the head's own lean counts); the view the camera last took (its ring entry: rows of the final matrix; row 2 is the camera's back, the view looks down -z)
        const float *m = s->ring[__atomic_load_n(&s->calls, __ATOMIC_ACQUIRE) & 15].m;   // the entry of the latest camera update
        const float fx = -m[8], fy = -m[9], fz = -m[10], h = sqrtf(fx * fx + fz * fz);
        if (h > 1e-3f && fy < 0) {
            float t = fminf(-fy / 0.866f, 1); t = t * t * (3 - 2 * t);
            const float d = atomic_load(&g_lean_cm) / 100.0f * t;
            up[0] += d * fx / h; up[2] += d * fz / h;
        }
    }
    memcpy(s->fp_up, up, sizeof up);   // one 16-byte store: the hook reads the four at once (ldr q9)
    s->fp_fwd = fwd;
    static uint64_t at; static uintptr_t addr; uint64_t now = now_ns();
    if (!addr || now - at > 1000000000ull) {
        at = now; uintptr_t a = runtime_player_pos_address();
        if (a != addr) VLOG("first person: %s", a ? "the player's position found" : "no player in the scene (third person until he is)");
        addr = a;
    }
    __atomic_store_n(&s->fp_pos, (uint64_t)addr, __ATOMIC_RELEASE);
    atomic_store(&g_vr_hidehead_fp, addr != 0 && b >= 0.5f);
}
static _Atomic int g_deadband_tenths = -1;
/* Up / down only from the headset (user 2026-10-29: "手柄 只能左右改变摄像头 不能上下， 上下看 只能 头显 自己"): the camera hook
   takes the game's pitch out of its camera before the head's rotation (vr_cam_state.level), and the right stick's up /
   down does nothing while the world camera runs (input.c). flag vr_pitch 1 / 0, TOTK_VR_PITCH=0: the game's pitch as before. */
static _Atomic int g_pitch_lock = -1;
void vr_set_pitch_lock(int on) { atomic_store(&g_pitch_lock, on != 0); VLOG("looking up / down: %s", on ? "the headset only (the stick turns left / right)" : "the headset and the game's camera"); }
void vr_set_deadband(int tenths) { atomic_store(&g_deadband_tenths, tenths < 0 ? 0 : tenths > 100 ? 100 : tenths); VLOG("head-look dead band %.1f degrees", atomic_load(&g_deadband_tenths) / 10.0); }
static void headlook_update(const vr_pose *head, int recenter) {
    if (atomic_load(&g_deadband_tenths) < 0) { const char *e = getenv("TOTK_VR_DEADBAND"); atomic_store(&g_deadband_tenths, e ? (int)(atof(e) * 10 + 0.5f) : 10); }
    vr_cam_state *s = g_vr_headlook;
    if (!s) return;
    if (atomic_load(&g_pitch_lock) < 0) { const char *e = getenv("TOTK_VR_PITCH"); atomic_store(&g_pitch_lock, !(e && !strcmp(e, "0"))); }
    __atomic_store_n(&s->level, (uint32_t)atomic_load(&g_pitch_lock), __ATOMIC_RELAXED);
    if (!g.hl_have_ref || recenter) {
        float f[3]; const float nz[3] = { 0, 0, -1 }; qrot(head->q, nz, f);
        float yaw = atan2f(-f[0], -f[2]);
        g.hl_ref[0] = 0; g.hl_ref[1] = sinf(yaw / 2); g.hl_ref[2] = 0; g.hl_ref[3] = cosf(yaw / 2); g.hl_have_ref = 1;
    }
    const float inv[4] = { -g.hl_ref[0], -g.hl_ref[1], -g.hl_ref[2], g.hl_ref[3] };
    float r[4]; qmul(inv, head->q, r);
    if (!g_hl_pos_have || recenter) { memcpy(g_hl_pos_ref, head->p, sizeof g_hl_pos_ref); g_hl_pos_have = 1; }   // the head's position from here
    { const float d[3] = { head->p[0] - g_hl_pos_ref[0], head->p[1] - g_hl_pos_ref[1], head->p[2] - g_hl_pos_ref[2] }; qrot(inv, d, g_hl_pos); }
    /* Dead band (user 2026-10-26: shadows flicker in VR, "原来平面游戏 看着不明显"): the game fits its shadow cascades to
       the camera every frame, and a head never holds still, so every frame moved them a little. Rotations within the
       band leave the game's camera where it was; the frames keep the pose they were rendered with, so the headset's
       reprojection still shows the small head motion and the world stays put. Beyond the band the camera follows at
       once. flag vr_deadband <tenths of a degree> (TOTK_VR_DEADBAND, degrees; 1 by default; 0 off). */
    float band = atomic_load(&g_deadband_tenths) / 10.0f * (float)M_PI / 180;
    if (g.hl_have_sent && !recenter && band > 0) {
        float d = fabsf(r[0] * g.hl_sent[0] + r[1] * g.hl_sent[1] + r[2] * g.hl_sent[2] + r[3] * g.hl_sent[3]);
        if (2 * acosf(fminf(d, 1)) < band) { atomic_fetch_add(&g.hl_held, 1); return; }
    }
    memcpy(g.hl_sent, r, sizeof r); g.hl_have_sent = 1;
    float col[3][3]; const float ax[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
    for (int i = 0; i < 3; i++) qrot(r, ax[i], col[i]);   // col[i] = R e_i, the i-th column of R
    uint32_t seq = s->seq + 1;
    g.hl_pose[seq % 64] = *head;
    float h[12] = { col[0][0], col[0][1], col[0][2], 0, col[1][0], col[1][1], col[1][2], 0, col[2][0], col[2][1], col[2][2], 0 };   // rows of R^T
    memcpy(s->h, h, sizeof h);
    __atomic_store_n(&s->seq, seq, __ATOMIC_RELEASE);
    __atomic_store_n(&s->on, 1, __ATOMIC_RELEASE);
}
/* The game renders the headset's field of view (user, first head-look try: "画面依然是前面正方形 不是360度 围绕": the
   game's own 50 degrees left a rectangle ahead in black): the union of both eyes' angles, made symmetric (the game's
   projection is), 5% more for the turn between the picture and the current pose; the ratio and the Vert+ factor on the
   aspect patch's data page (runtime/aspect_patch.h runtime_aspect_vr). The view is fixed, a negative factor (user
   2026-10-31: flying a Zonai device the picture stretched: the game widens its camera at speed, narrows it to aim, and
   with a factor on the game's own angle the picture then held more, or less, than the headset's angle it was placed at).
   TOTK_VR_GAME_FOV: the game's vertical field of view when the Vert+ stubs are not there (50 degrees). */
extern int runtime_aspect_vr(float a, float k) __attribute__((weak_import));
static float game_vfov(void) {
    static float vfov = 0; if (!vfov) { const char *e = getenv("TOTK_VR_GAME_FOV"); vfov = e ? (float)atof(e) : 50; if (vfov < 10 || vfov > 150) vfov = 50; }
    return vfov;
}
static void headlook_fov(const vr_views *v) {
    if (g.fov_on || !g_vr_headlook || !runtime_aspect_vr) return;
    float w = 0, h = 0;
    for (int e = 0; e < 2; e++) {
        w = fmaxf(w, fmaxf(fabsf(tanf(v->fov[e][0])), fabsf(tanf(v->fov[e][1]))));
        h = fmaxf(h, fmaxf(fabsf(tanf(v->fov[e][2])), fabsf(tanf(v->fov[e][3]))));
    }
    w *= 1.05f; h *= 1.05f;
    if (!(w > 0.3f && h > 0.3f && w < 6 && h < 6)) return;
    float a = w / h, k = -h;   // the half-angle tangent itself: the game's own zoom and speed widening do not reach the headset
    if (!runtime_aspect_vr(a, k)) { VLOG("the game's field of view stays its own (no Vert+ stubs)"); g.fov_on = -1; return; }
    g.fov_tanh = h; g.fov_aspect = a; g.fov_on = 1;
    VLOG("the game renders the headset's view: %.0f x %.0f degrees", 2 * atanf(w) * 180 / M_PI, 2 * atanf(h) * 180 / M_PI);
}

/* The picture of a frame the game rendered with head pose p: a plane far ahead along p's view (TOTK_VR_GAME_FOV, the
   game's vertical field of view in degrees, 50 by default; the width from the picture's shape). The headset then
   reprojects it to the current pose, so the world stays put while the head turns. */
static void headlook_place(const vr_pose *p, double aspect) {
    const float D = 10, hh = 2 * D * (g.fov_on > 0 ? g.fov_tanh : tanf(game_vfov() * (float)M_PI / 360)),
                ww = hh * (g.fov_on > 0 ? g.fov_aspect : (float)aspect);
    const float nz[3] = { 0, 0, -D }, ex[3] = { ww, 0, 0 }, ey[3] = { 0, hh, 0 };
    float f[3]; qrot(p->q, nz, f); qrot(p->q, ex, g.right); qrot(p->q, ey, g.up);
    for (int i = 0; i < 3; i++) g.center[i] = p->p[i] + f[i];
    g.hud_inside = 1;
}

/* Replay thread, at the frame's G-buffer pass: its camera block's view matrix is one of the turned matrices the hook
   left in its ring; that entry's seq is the head pose this frame was rendered with. The present of the same frame
   comes after its replay, so vr_source_encode tags its picture with it. Exact equality: the block is a copy. */
int vr_wants_camera(void) { return g_vr_headlook && __atomic_load_n(&g_vr_headlook->on, __ATOMIC_RELAXED); }
/* First person: the player's position the hook put this frame's camera at (its ring entry), for the replay thread to set
   the body it draws against (nvn_mtl.m vr_head_eye_note): both of the same frame, so the gap between them is where the
   body really is, not how far it moved since (user 2026-10-31: in a Zonai device the body drifted off the camera). */
static float g_frame_pos[3]; static int g_frame_pos_have, g_frame_idx;   // the replay thread's (g_frame_idx: its ring entry)
static void frame_pos_take(int i) { memcpy(g_frame_pos, g_vr_headlook->ring[i].pos, sizeof g_frame_pos); g_frame_idx = i; g_frame_pos_have = g_frame_pos[0] || g_frame_pos[1] || g_frame_pos[2]; }
int vr_frame_pos(float p[3]) { if (!g_frame_pos_have) return 0; memcpy(p, g_frame_pos, sizeof g_frame_pos); return 1; }
int vr_frame_ring(void) { return g_frame_pos_have ? g_frame_idx : -1; }   /* the replay thread: this frame's ring entry (-1 without one) */
/* The vertical field of view each G-buffer camera had (camera block float 70, radians), lowest and highest since the last
   statistics line: with the view fixed for VR both stay at the headset's (a camera the Vert+ stubs miss would show). */
static _Atomic int g_cam_fov_lo = INT_MAX, g_cam_fov_hi = INT_MIN;   // tenths of a degree
/* Diagnostic (TOTK_VR_FRAMELOG): how this frame's camera block matched the hook's ring (0 none taken, 1 exact, 2 by
   rotation, 3 missed) and the seq; reset at each frame line (user 2026-11-01: the screen shakes while the Zonai device
   thumbnails load, frames with no hook call among them). */
static int g_note_kind; static uint32_t g_note_seq;
static void note_kind(int k, uint32_t seq) { g_note_kind = k; g_note_seq = seq; }
void vr_frame_match_desc(char *out, size_t n) {
    vr_cam_state *s = g_vr_headlook; static const char *kn[] = { "no camera taken", "exact", "by rotation", "MISSED" };
    if (!s || !n) { if (n) out[0] = 0; return; }
    const uint32_t c = __atomic_load_n(&s->calls, __ATOMIC_RELAXED); const int i = (int)((c - 1) & 15); const float *m = s->ring[i].m; float e[3];
    for (int j = 0; j < 3; j++) e[j] = -(m[j] * m[3] + m[4 + j] * m[7] + m[8 + j] * m[11]);
    int k = snprintf(out, n, "ring match %s seq %u; hook's newest seq %u camera %.2f %.2f %.2f fwd %.3f %.3f %.3f",
             kn[g_note_kind & 3], g_note_seq, s->ring[i].seq, e[0], e[1], e[2], m[8], m[9], m[10]);
    /* the main camera's binding (game_patches_v143.c vr_view0_hook): cam and the main view set */
    if (k > 0 && (size_t)k < n)
        snprintf(out + k, n - k, "; cam %#llx, main view set %#llx (changed %u times)", (unsigned long long)__atomic_load_n(&s->cam, __ATOMIC_RELAXED),
                 (unsigned long long)__atomic_load_n(&s->main_vs, __ATOMIC_RELAXED), __atomic_load_n(&s->vs_changes, __ATOMIC_RELAXED));
    g_note_kind = 0; g_note_seq = 0;
}
void vr_note_camera(const float *cam) {
    vr_cam_state *s = g_vr_headlook;
    if (!s) return;
    { int d = (int)lroundf(cam[70] * 1800 / (float)M_PI), v;
      for (v = atomic_load(&g_cam_fov_lo); d < v && !atomic_compare_exchange_weak(&g_cam_fov_lo, &v, d); ) ;
      for (v = atomic_load(&g_cam_fov_hi); d > v && !atomic_compare_exchange_weak(&g_cam_fov_hi, &v, d); ) ; }
    /* The main view set: the one whose view 0 was set to this block's matrix (the veneer's v0ring). A matrix several view
       sets were given (a copy of the same view) keeps the one already found. */
    { uint64_t vs = 0; int keep = 0; const uint64_t cur = __atomic_load_n(&s->main_vs, __ATOMIC_RELAXED);
      for (int i = 0; i < 32; i++) {
          const uint64_t v = __atomic_load_n(&s->v0ring[i].vs, __ATOMIC_RELAXED);
          if (!v || memcmp(s->v0ring[i].m, cam, 48)) continue;
          if (v == cur) { keep = 1; break; }
          if (!vs) vs = v;
      }
      if (!keep && vs) { __atomic_store_n(&s->main_vs, vs, __ATOMIC_RELAXED); __atomic_fetch_add(&s->vs_changes, 1, __ATOMIC_RELAXED);
          static int said; if (said++ < 8) VLOG("the main picture is drawn from view set %#llx (was %#llx)", (unsigned long long)vs, (unsigned long long)cur); } }
    for (int i = 0; i < 16; i++)
        if (!memcmp(s->ring[i].m, cam, 48)) { note_kind(1, s->ring[i].seq); atomic_store(&g.frame_seq, __atomic_load_n(&s->ring[i].seq, __ATOMIC_RELAXED)); atomic_fetch_add(&g.seq_matched, 1); frame_pos_take(i); return; }
    /* By the view's rotation alone (user 2026-10-30, the picture jumping while turning or walking: "1"): the camera
       block's position can differ from the hook's matrix (the ring keeps one matrix per seq, which a later camera update
       of the same seq overwrites, and the game can move the camera after the hook). A miss placed the picture by the
       last matched pose while it was rendered turned: a jump for the frames it was shown. Rotations of different seqs
       differ by at least the dead band (1 degree by default, about 0.017 in a matrix entry; with it off, a near seq is as good). */
    static const int rot[9] = { 0, 1, 2, 4, 5, 6, 8, 9, 10 };
    /* Among the rotations that match, the nearest translation (2026-10-31: a head held still turns every entry the same,
       and first person needs the right frame's position, vr_frame_pos; the camera block may sit a little off the hook's) */
    int best = -1; float best_t = 1e30f;
    for (int i = 0; i < 16; i++) {
        float d = 0; for (int k = 0; k < 9; k++) d = fmaxf(d, fabsf(s->ring[i].m[rot[k]] - cam[rot[k]]));
        if (d >= 2e-4f) continue;
        float t = 0; for (int k = 3; k < 12; k += 4) t += fabsf(s->ring[i].m[k] - cam[k]);
        if (t < best_t) { best_t = t; best = i; }
    }
    { static const char *dbg; static int looked; if (!looked) { looked = 1; dbg = getenv("TOTK_VR_RINGLOG"); }   // diagnostic: the block's camera against every entry's
      static int lines; if (dbg && best >= 0 && lines < 400 && s->ring[best].pos[0]) { lines++;
        float cb[3]; for (int j = 0; j < 3; j++) cb[j] = -(cam[j] * cam[3] + cam[4 + j] * cam[7] + cam[8 + j] * cam[11]);
        char line[1200]; int k = snprintf(line, sizeof line, "[vr] ringlog block cam %.3f %.3f %.3f picked seq %u |", cb[0], cb[1], cb[2], s->ring[best].seq);
        for (int i = 0; i < 16 && k < (int)sizeof line - 80; i++) { const float *m = s->ring[i].m; float e[3];
            for (int j = 0; j < 3; j++) e[j] = -(m[j] * m[3] + m[4 + j] * m[7] + m[8 + j] * m[11]);
            k += snprintf(line + k, sizeof line - k, " %u:%+.2f,%+.2f,%+.2f", s->ring[i].seq, e[0] - cb[0], e[1] - cb[1], e[2] - cb[2]); }
        VLOG("%s", line); } }
    if (best >= 0) { note_kind(2, s->ring[best].seq); atomic_store(&g.frame_seq, __atomic_load_n(&s->ring[best].seq, __ATOMIC_RELAXED)); atomic_fetch_add(&g.seq_matched_rot, 1); frame_pos_take(best); return; }
    note_kind(3, 0); atomic_fetch_add(&g.seq_missed, 1); g_frame_pos_have = 0;
}

void vr_present_queued(uint64_t ticket) {   // the replay thread, at the game's present (after that frame's replay)
    if (g_vr_headlook) atomic_store(&g.ticket_seq[ticket % 64], atomic_exchange(&g.frame_seq, 0));
}

// ---- picture copies (present thread) ----
static id<MTLTexture> src_tex(NSUInteger w, NSUInteger h, MTLPixelFormat f) {
    /* Mipmapped: the 1920-wide picture covers about 900 headset pixels, and sampling it without mip levels blurred and
       shimmered (user, first Quest 3 try: "有点糊"). */
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f width:w height:h mipmapped:YES];
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView; d.storageMode = MTLStorageModePrivate;
    return [g.dev newTextureWithDescriptor:d];
}
static int latest_slot(void) {   // under g.mu
    int b = -1; for (int i = 0; i < SRC_SLOTS; i++) if (g.seq[i] && !g.writing[i] && (b < 0 || g.seq[i] > g.seq[b])) b = i;
    return b;
}
static void start_thread(id<MTLDevice> dev);

int vr_source_encode(id<MTL4CommandBuffer> cb, id<MTLTexture> src, const int crop[4], uint64_t ticket) {
    if (!vr_enabled() || g.failed || !src) return -1;
    if (!g.dev) start_thread(src.device);
    if (!g.dev) return -1;
    NSUInteger x = 0, y = 0, w = src.width, h = src.height;
    if (crop && crop[2] > 0 && crop[3] > 0 && (NSUInteger)crop[0] + (NSUInteger)crop[2] <= src.width && (NSUInteger)crop[1] + (NSUInteger)crop[3] <= src.height) {
        x = (NSUInteger)crop[0]; y = (NSUInteger)crop[1]; w = (NSUInteger)crop[2]; h = (NSUInteger)crop[3]; }
    pthread_mutex_lock(&g.mu);
    int latest = latest_slot(), s = -1;
    for (int i = 0; i < SRC_SLOTS && s < 0; i++) if (i != latest && !g.writing[i] && !g.readers[i]) s = i;
    if (s >= 0) { g.writing[s] = 1; g.seq[s] = 0; g.src_used[s] = g_vr_headlook ? atomic_load(&g.ticket_seq[ticket % 64]) : 0; }   // 0: not matched, placed by the newest pose
    pthread_mutex_unlock(&g.mu);
    if (s < 0) return -1;
    id<MTLTexture> t = g.src[s];
    if (!t || t.width != w || t.height != h || t.pixelFormat != src.pixelFormat) {
        if (t) [g.rs removeAllocation:t];
        t = src_tex(w, h, src.pixelFormat);
        if (!t) { pthread_mutex_lock(&g.mu); g.writing[s] = 0; pthread_mutex_unlock(&g.mu); return -1; }
        [g.rs addAllocation:t]; [g.rs commit];
        g.src[s] = t;   // the old one is not read: readers[s] was 0
    }
    id<MTL4ComputeCommandEncoder> e = [cb computeCommandEncoder];
    [e barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
    [e copyFromTexture:src sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(x, y, 0) sourceSize:MTLSizeMake(w, h, 1)
             toTexture:t destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
    [e barrierAfterEncoderStages:MTLStageBlit beforeEncoderStages:MTLStageBlit visibilityOptions:MTL4VisibilityOptionDevice];
    [e generateMipmapsForTexture:t];
    [e endEncoding];
    return s;
}

void vr_source_done(int s, int ok) {
    if (s < 0 || s >= SRC_SLOTS) return;
    pthread_mutex_lock(&g.mu); g.writing[s] = 0; if (ok) g.seq[s] = ++g.next_seq; pthread_mutex_unlock(&g.mu);
    if (ok) atomic_fetch_add(&g.copies, 1);
}

// ---- VR thread ----
static IOSurfaceRef make_surface(int w, int h) {
    size_t bpr = IOSurfaceAlignProperty(kIOSurfaceBytesPerRow, (size_t)w * 4);
    /* Global so the streamer (another process) finds it with IOSurfaceLookup, as BasaltVR's own runtime does; the key is
       spelled out because kIOSurfaceIsGlobal is deprecated. */
    NSDictionary *p = @{ (__bridge NSString *)kIOSurfaceWidth: @(w), (__bridge NSString *)kIOSurfaceHeight: @(h),
                         (__bridge NSString *)kIOSurfaceBytesPerElement: @4, (__bridge NSString *)kIOSurfaceBytesPerRow: @(bpr),
                         (__bridge NSString *)kIOSurfacePixelFormat: @((uint32_t)'BGRA'), @"IOSurfaceIsGlobal": @YES };
    return IOSurfaceCreate((__bridge CFDictionaryRef)p);
}

static int setup_gpu(void) {
    NSError *e = nil;
    static const char *msl =
        "#include <metal_stdlib>\nusing namespace metal;\n"
        "struct U { float4x4 m; float4 c, r, u, uvs; };\n"
        "struct O { float4 pos [[position]]; float2 uv; };\n"
        "vertex O vr_vs(uint i [[vertex_id]], constant U &u [[buffer(0)]]) {\n"
        "  float2 t = float2(i & 1, i >> 1);\n"   // 0 (0,0) 1 (1,0) 2 (0,1) 3 (1,1): triangle strip, top-left first
        "  float3 w = u.c.xyz + (t.x - 0.5) * u.r.xyz + (0.5 - t.y) * u.u.xyz;\n"
        "  O o; o.pos = u.m * float4(w, 1.0); o.uv = t * u.uvs.xy; return o; }\n"
        "fragment float4 vr_fs(O i [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {\n"
        "  return float4(t.sample(s, i.uv).rgb, 1.0); }\n";
    id<MTLLibrary> lib = [g.dev newLibraryWithSource:@(msl) options:[MTLCompileOptions new] error:&e];
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [lib newFunctionWithName:@"vr_vs"]; pd.fragmentFunction = [lib newFunctionWithName:@"vr_fs"];
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    g.ps = lib ? [g.dev newRenderPipelineStateWithDescriptor:pd error:&e] : nil;
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new]; sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
    sd.mipFilter = MTLSamplerMipFilterLinear; sd.maxAnisotropy = 8;
    sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    g.smp = [g.dev newSamplerStateWithDescriptor:sd];
    g.q = [g.dev newCommandQueue];
    if (!g.ps || !g.smp || !g.q) { VLOG("【红】not available: %s", e ? e.localizedDescription.UTF8String : "Metal objects"); return 0; }
    return 1;
}

static int setup_images(void) {
    uint32_t ids[IMAGES];
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:(NSUInteger)(2 * g.eye_w) height:(NSUInteger)g.eye_h mipmapped:NO];
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; d.storageMode = MTLStorageModeShared;
    for (int i = 0; i < IMAGES; i++) {
        g.surf[i] = make_surface(2 * g.eye_w, g.eye_h);
        g.img[i] = g.surf[i] ? [g.dev newTextureWithDescriptor:d iosurface:g.surf[i] plane:0] : nil;
        if (!g.img[i]) { VLOG("【红】headset image %d (%dx%d IOSurface) could not be made", i, 2 * g.eye_w, g.eye_h); return 0; }
        ids[i] = IOSurfaceGetID(g.surf[i]); g.img_frame[i] = 0;
    }
    if (vr_link_create_swapchain(SWAPCHAIN_ID, (uint32_t)(2 * g.eye_w), (uint32_t)g.eye_h, VR_LINK_FORMAT_BGRA8, IMAGES, ids)) {
        VLOG("【红】the streamer did not take our images (swapchain not acknowledged)"); return 0; }
    return 1;
}

static id<MTLTexture> sample_view(id<MTLTexture> t) {   // sRGB pictures are sampled as stored: the headset image is not sRGB either
    MTLPixelFormat f = t.pixelFormat;
    if (f == MTLPixelFormatRGBA8Unorm_sRGB) return [t newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm];
    if (f == MTLPixelFormatBGRA8Unorm_sRGB) return [t newTextureViewWithPixelFormat:MTLPixelFormatBGRA8Unorm];
    return t;
}

/* The settings panel in the headset (runtime/settings_panel.h settings_vr_capture): its latest bitmap, uploaded by the
   VR thread when it changed, drawn in the middle of the view over the picture. */
static pthread_mutex_t g_panel_mu = PTHREAD_MUTEX_INITIALIZER;
static uint8_t *g_panel_px; static size_t g_panel_cap; static int g_panel_w, g_panel_h, g_panel_dirty, g_panel_on;
static id<MTLTexture> g_panel_tex;
void vr_panel_image(const void *bgra, int w, int h, int stride) {
    pthread_mutex_lock(&g_panel_mu);
    if (!bgra || w <= 0 || h <= 0 || stride < w * 4) g_panel_on = 0;
    else {
        size_t need = (size_t)w * h * 4;
        if (need > g_panel_cap) { uint8_t *nb = realloc(g_panel_px, need); if (nb) { g_panel_px = nb; g_panel_cap = need; } }
        if (need <= g_panel_cap) {
            for (int y = 0; y < h; y++) memcpy(g_panel_px + (size_t)y * w * 4, (const uint8_t *)bgra + (size_t)y * stride, (size_t)w * 4);
            g_panel_w = w; g_panel_h = h; g_panel_dirty = 1; g_panel_on = 1;
        }
    }
    pthread_mutex_unlock(&g_panel_mu);
}
static id<MTLTexture> panel_update(int *w, int *h) {   // VR thread, between frames
    pthread_mutex_lock(&g_panel_mu);
    if (!g_panel_on) { pthread_mutex_unlock(&g_panel_mu); return nil; }
    if (g_panel_dirty) {
        if (!g_panel_tex || (int)g_panel_tex.width != g_panel_w || (int)g_panel_tex.height != g_panel_h) {
            MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:(NSUInteger)g_panel_w height:(NSUInteger)g_panel_h mipmapped:NO];
            d.usage = MTLTextureUsageShaderRead; d.storageMode = MTLStorageModeShared; g_panel_tex = [g.dev newTextureWithDescriptor:d];
        }
        if (g_panel_tex) [g_panel_tex replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)g_panel_w, (NSUInteger)g_panel_h) mipmapLevel:0 withBytes:g_panel_px bytesPerRow:(NSUInteger)g_panel_w * 4];
        g_panel_dirty = 0;
    }
    *w = g_panel_w; *h = g_panel_h;
    pthread_mutex_unlock(&g_panel_mu);
    return g_panel_tex;
}
typedef struct { float m[16]; float c[4], r[4], u[4], uvs[4]; } quad_u;

/* The FPS readout under the screen (user, first Quest 3 try: "看不到fps"): the window's corner box lines are drawn by
   the Mac window, not into the picture, so they are rasterised here with the same 5x7 font, plus our own line. */
enum { HUD_TEX_W = FPS_OVERLAY_TEX_W, HUD_TEX_H = 2 * FPS_OVERLAY_TEX_H, HUD_SCALE = 3 };
static id<MTLTexture> hud_update(void) {   // VR thread, between frames (the GPU is done with the texture)
    if (!fps_overlay_hints_enabled()) return nil;
    uint64_t now = now_ns();
    if (g.hud[0] && now - g.hud_at < 500000000ull) return g.hud[0];
    uint64_t frames = atomic_load(&g.frames), copies = atomic_load(&g.copies);
    double dt = g.hud_at ? (now - g.hud_at) / 1e9 : 0;
    char l0[96] = "", l1[96]; int hl = 0;
    if (!hud_snapshot(HUD_FPS, l0, sizeof l0, &hl) || !fps_overlay_line_pixel_font(l0)) {
        int f = fps_overlay_value(); if (f >= 0) snprintf(l0, sizeof l0, "FPS %d", f); else snprintf(l0, sizeof l0, "FPS --");
    }
    /* What the Quest reports it showed (user 2026-10-30: "quest里 vr 还是显示72帧 为什么？": this line was the frames the
       Mac handed to the streamer, not what the headset showed); the frames sent after it. */
    const int sent = dt > 0 ? (int)((frames - g.hud_frames) / dt + 0.5) : 0; float qs, ql;
    if (vr_link_headset(&qs, &ql)) {
        if (qs > 0) snprintf(l1, sizeof l1, "QUEST %d FPS %dMS  SENT %d  %dx%d", (int)(qs + 0.5f), (int)(ql + 0.5f), sent, g.eye_w, g.eye_h);
        else snprintf(l1, sizeof l1, "QUEST NO PICTURE  SENT %d", sent);
    } else snprintf(l1, sizeof l1, "VR SENT %d FPS  %dx%d", sent, g.eye_w, g.eye_h);
    (void)copies;
    g.hud_at = now; g.hud_frames = frames;
    if (!g.hud[0]) {
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:HUD_TEX_W height:HUD_TEX_H mipmapped:NO];
        d.usage = MTLTextureUsageShaderRead; d.storageMode = MTLStorageModeShared;
        g.hud[0] = [g.dev newTextureWithDescriptor:d];
        if (!g.hud[0]) return nil;
    }
    static uint8_t px[HUD_TEX_W * HUD_TEX_H * 4];
    memset(px, 0, sizeof px);
    int w0 = 0, h0 = 0, w1 = 0, h1 = 0;
    fps_overlay_raster_line_tex(l0, hl, HUD_SCALE, px, HUD_TEX_W, FPS_OVERLAY_TEX_H, &w0, &h0);
    fps_overlay_raster_line_tex(l1, 0, HUD_SCALE, px + (size_t)h0 * HUD_TEX_W * 4, HUD_TEX_W, HUD_TEX_H - h0, &w1, &h1);
    g.hud_w = w0 > w1 ? w0 : w1; g.hud_h = h0 + h1;
    [g.hud[0] replaceRegion:MTLRegionMake2D(0, 0, HUD_TEX_W, HUD_TEX_H) mipmapLevel:0 withBytes:px bytesPerRow:HUD_TEX_W * 4];
    return g.hud[0];
}
/* The readout's quad: centred under the picture, in the picture's own plane (stage 1: an upright screen; stage 2: a plane
   turned with the head), its texels scaled with the picture's height (about 4 cm text on the 1.5 m screen at 2 m). */
static int hud_quad(quad_u *u) {
    if (g.hud_w <= 0 || g.hud_h <= 0) return 0;
    float rl = sqrtf(g.right[0] * g.right[0] + g.right[1] * g.right[1] + g.right[2] * g.right[2]);
    float ul = sqrtf(g.up[0] * g.up[0] + g.up[1] * g.up[1] + g.up[2] * g.up[2]);
    if (rl < 1e-4f || ul < 1e-4f) return 0;
    /* Stage 2 (the picture fills the view): inside the picture, 30% of its height below the centre, about 1.2 degrees
       tall; below the picture it was out of sight (user, first 2K try: "看不到fps了"). */
    const int in = g.hud_inside;
    const float k = (in ? 0.00045f : 0.0014f) * ul, hw = g.hud_w * k, hh = g.hud_h * k, gap = 0.035f * ul;
    for (int i = 0; i < 3; i++) {
        float ru = g.right[i] / rl, uu = g.up[i] / ul;
        u->r[i] = ru * hw; u->u[i] = uu * hh;
        u->c[i] = in ? g.center[i] - g.up[i] * 0.30f : g.center[i] - g.up[i] * 0.5f - uu * (gap + hh * 0.5f);
    }
    u->uvs[0] = (float)g.hud_w / HUD_TEX_W; u->uvs[1] = (float)g.hud_h / HUD_TEX_H;
    return 1;
}

/* The Mac window shows what the headset gets (user 2026-10-29: "屏幕 显示 类似vr 那种 两个屏幕"): the newest image handed to
   the streamer, both eyes side by side; runtime/frontend.m puts it on a layer over the game's picture (vr_mirror_copy).
   The VR page's row "Mac 屏幕" switches it, kept in TOTK_VR_MIRROR (<data>/vr-mirror.txt: 1 / 0); on by default. */
static pthread_mutex_t g_mirror_mu = PTHREAD_MUTEX_INITIALIZER;
static IOSurfaceRef g_mirror_surf;
static void mirror_note(IOSurfaceRef s) { pthread_mutex_lock(&g_mirror_mu); g_mirror_surf = s; pthread_mutex_unlock(&g_mirror_mu); }
/* 0: the game's picture; 1: both eyes side by side; 2: the left eye's middle at 16:9 (for recording, as BetterVR's
   "recording mode"; runtime/frontend.m crops it with the layer's contentsRect). */
int vr_mirror_mode(void) { int m = vr_option("mirror", 1); return m < 0 || m > 2 ? 1 : m; }
void vr_set_mirror_mode(int m) {
    vr_option_set("mirror", m < 0 || m > 2 ? 1 : m);
    VLOG("the Mac window shows %s", m == 1 ? "both eyes as the headset gets them" : m == 2 ? "one eye at 16:9" : "the game's picture");
}
double vr_mirror_aspect(void) { return g.eye_w > 0 && g.eye_h > 0 ? 2.0 * g.eye_w / g.eye_h : 2; }   // the whole image: both eyes side by side
/* "显示 Quest 画面" (user 2026-10-30: on, the Mac shows what the lenses show, each eye in its ellipse; off, "就是 不遮挡状态":
   both eyes uncovered). Option quest_view, on by default. */
int vr_quest_view(void) { return vr_option("quest_view", 1) != 0; }
/* How much larger the Mac window shows each eye's middle with "显示 Quest 画面" (user 2026-10-30: "设置里可以调 2倍 到 10 倍"):
   option quest_zoom, 2..10, 5 by default (user 2026-10-30: "默认 改成 5倍"). */
int vr_quest_zoom(void) { int x = vr_option("quest_zoom", 5); return x < 2 ? 2 : x > 10 ? 10 : x; }
void vr_set_quest_zoom(int x) { x = x < 2 ? 2 : x > 10 ? 10 : x; vr_option_set("quest_zoom", x); VLOG("the Mac window: each eye's middle %dx larger", x); }
/* The camera's spring on the head (user 2026-11-02: "vr cam 和 头部 绑定的 太死 晃动的厉害 如何 用 弹性 解决？"): the time
   constant of a critically damped spring on the eyes' offset from his position (the head bone's few cm of bob running
   and walking), in ms, 0 = bound rigidly; option eye_spring, 300 by default (the user on the Quest, 2026-11-02: "设置 300"); the VR page's slider, "flag vr_eye_spring". */
int vr_eye_spring_ms(void) { int x = vr_option("eye_spring", 300); return x < 0 ? 0 : x > 400 ? 400 : x; }
void vr_set_eye_spring_ms(int ms) { ms = ms < 0 ? 0 : ms > 400 ? 400 : ms; vr_option_set("eye_spring", ms); VLOG("first person: the camera's spring on the head %d ms%s", ms, ms ? "" : " (bound rigidly)"); }
_Atomic int g_spring_dev_mm;   // diagnostic: the most the spring kept the camera off the head bone's eyes since the last statistics line
void vr_set_quest_view(int on) { vr_option_set("quest_view", on != 0); if (vr_mirror_mode() != 1) vr_set_mirror_mode(1); VLOG("the Mac window: both eyes %s", on ? "as the lenses show them" : "uncovered"); }
void vr_mirror_crop(double out[4]) {   // the shown part of the image (unit coordinates): all, or the left eye's middle at 16:9
    out[0] = out[1] = 0; out[2] = out[3] = 1;
    if (vr_mirror_mode() != 2 || g.eye_w <= 0 || g.eye_h <= 0) return;
    double ch = fmin((double)g.eye_h, g.eye_w * 9.0 / 16);
    out[1] = (g.eye_h - ch) / 2 / g.eye_h; out[2] = 0.5; out[3] = ch / g.eye_h;
}
IOSurfaceRef vr_mirror_copy(void) {   // +1 (CFRelease it), or NULL: mirror off or no frame yet
    if (!vr_mirror_mode()) return NULL;
    pthread_mutex_lock(&g_mirror_mu); IOSurfaceRef s = g_mirror_surf; if (s) CFRetain(s); pthread_mutex_unlock(&g_mirror_mu);
    return s;
}
/* "显示 Quest 画面" on the Mac (user 2026-10-30: "如何 把这个 远处 变清楚 不要 一直晃动", "开始 2根本改法"): an image of its
   own, both eyes side by side at half the headset's eye size, each eye's middle vr_quest_zoom() times larger, drawn from
   the game's picture at the head pose that picture was rendered with. Before, the window enlarged the middle of the
   headset image, which is placed at the current head pose 72 times a second: the tracking's small noise shook it, and
   the game's picture was resampled twice (into the headset image, then by the window). Three images in turn; the window
   shows the newest. */
enum { MAC_IMAGES = 3 };
static IOSurfaceRef g_mac_surf[MAC_IMAGES]; static id<MTLTexture> g_mac_img[MAC_IMAGES]; static int g_mac_i, g_mac_w, g_mac_h;
static int mac_view_wanted(void) { return vr_mirror_mode() == 1 && vr_quest_view(); }
static int mac_image(void) {   // the ring's next index (made at half the headset's eye size), or -1
    const int w = g.eye_w / 2, h = g.eye_h / 2;
    if (w <= 0 || h <= 0) return -1;
    if (w != g_mac_w || h != g_mac_h) {   // the headset's size changed: the window lets go of the old images first
        mirror_note(NULL);
        for (int i = 0; i < MAC_IMAGES; i++) { g_mac_img[i] = nil; if (g_mac_surf[i]) CFRelease(g_mac_surf[i]); g_mac_surf[i] = NULL; }
        g_mac_w = w; g_mac_h = h;
    }
    const int i = g_mac_i; g_mac_i = (g_mac_i + 1) % MAC_IMAGES;
    if (!g_mac_img[i]) {
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:(NSUInteger)(2 * w) height:(NSUInteger)h mipmapped:NO];
        d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; d.storageMode = MTLStorageModeShared;
        g_mac_surf[i] = make_surface(2 * w, h);
        g_mac_img[i] = g_mac_surf[i] ? [g.dev newTextureWithDescriptor:d iosurface:g_mac_surf[i] plane:0] : nil;
        if (!g_mac_img[i]) { VLOG("【红】the Mac view's image (%dx%d IOSurface) could not be made", 2 * w, h); return -1; }
    }
    return i;
}
/* An eye's field of view (OpenXR angles: left, right, up, down) narrowed z times around the middle of its image. */
static void zoom_fov(const float fov[4], float z, float out[4]) {
    const float L = tanf(fov[0]), R = tanf(fov[1]), U = tanf(fov[2]), D = tanf(fov[3]), cx = (L + R) / 2, cy = (U + D) / 2;
    out[0] = atanf(cx + (L - cx) / z); out[1] = atanf(cx + (R - cx) / z); out[2] = atanf(cy + (U - cy) / z); out[3] = atanf(cy + (D - cy) / z);
}
static void mac_view_encode(id<MTLCommandBuffer> cb, id<MTLTexture> target, const vr_pose *pose, const vr_views *v, id<MTLTexture> pic, id<MTLTexture> hud) {
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = target; rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1); rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
    [e setRenderPipelineState:g.ps]; [e setFragmentSamplerState:g.smp atIndex:0];
    const float z = (float)vr_quest_zoom();
    for (int eye = 0; eye < 2; eye++) {
        const float off[3] = { (eye ? 0.5f : -0.5f) * v->ipd, 0, 0 }; float o[3], p[3], f[4]; qrot(pose->q, off, o);
        for (int k = 0; k < 3; k++) p[k] = pose->p[k] + o[k];
        zoom_fov(v->fov[eye], z, f);
        quad_u u; eye_matrix(pose->q, p, f, u.m);
        memcpy(u.c, g.center, 12); memcpy(u.r, g.right, 12); memcpy(u.u, g.up, 12); u.c[3] = u.r[3] = u.u[3] = 0;
        u.uvs[0] = u.uvs[1] = 1; u.uvs[2] = u.uvs[3] = 0;
        [e setViewport:(MTLViewport){ eye * g_mac_w, 0, g_mac_w, g_mac_h, 0, 1 }];
        [e setVertexBytes:&u length:sizeof u atIndex:0];
        [e setFragmentTexture:pic atIndex:0];
        [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        if (hud && hud_quad(&u)) {
            [e setVertexBytes:&u length:sizeof u atIndex:0];
            [e setFragmentTexture:hud atIndex:0];
            [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        }
    }
    [e endEncoding];
}
/* Paced by the encoder (user 2026-10-30: "让 VR 线程按编码器跟得上的帧率出画面"): the streamer gives a frame's image back
   (FRAME_CONSUMED) only once VideoToolbox has encoded it (BasaltVR streamer App.swift, a local change), and at most
   ENCODER_SLOTS frames are out at once, as many as its encoder holds (Encoder.swift maximumFramesInFlight). With them out
   the VR thread waits for one to come back and renders at once with the newest pose. Before, it rendered on the 72 Hz
   grid regardless and the streamer threw away what its busy encoder could not take, unevenly: hundreds of frames a
   minute with H.264 while the game filled the GPU, felt as an occasional jerk ("带上 会 偶尔抖动"). */
enum { ENCODER_SLOTS = 2 };
static int frames_out(void) {   // takes the frames given back, counts those still out
    uint64_t done[32]; int nd = vr_link_take_consumed(done, 32);
    for (int k = 0; k < nd; k++) for (int i = 0; i < IMAGES; i++) if (g.img_frame[i] == done[k]) g.img_frame[i] = 0;
    int n = 0; for (int i = 0; i < IMAGES; i++) n += g.img_frame[i] != 0;
    return n;
}
static int render_frame(const vr_pose *head, const vr_views *v, uint64_t frame) {
    (void)frames_out();
    int im = -1; for (int i = 0; i < IMAGES && im < 0; i++) if (!g.img_frame[i]) im = i;
    if (im < 0) { atomic_fetch_add(&g.no_image, 1); return 0; }

    pthread_mutex_lock(&g.mu); int s = latest_slot(); if (s >= 0) g.readers[s]++; id<MTLTexture> pic = s >= 0 ? g.src[s] : nil;
    uint32_t used = s >= 0 ? g.src_used[s] : 0; pthread_mutex_unlock(&g.mu);
    if (pic && used && g_vr_headlook) headlook_place(&g.hl_pose[used % 64], (double)pic.width / pic.height);   // stage 2
    if (pic) pic = sample_view(pic);

    id<MTLTexture> hud = hud_update();
    int pw = 0, ph = 0; id<MTLTexture> panel = panel_update(&pw, &ph);
    vr_view_submit vs[2];
    id<MTLCommandBuffer> cb = [g.q commandBuffer];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = g.img[im]; rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0.02, 0.02, 0.025, 1); rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
    for (int eye = 0; eye < 2; eye++) {
        const float off[3] = { (eye ? 0.5f : -0.5f) * v->ipd, 0, 0 }; float o[3]; qrot(head->q, off, o);
        vr_view_submit *x = &vs[eye];
        x->swapchain = SWAPCHAIN_ID; x->image = (uint32_t)im;
        x->rect[0] = eye * g.eye_w; x->rect[1] = 0; x->rect[2] = g.eye_w; x->rect[3] = g.eye_h;
        x->pose = *head; for (int k = 0; k < 3; k++) x->pose.p[k] = head->p[k] + o[k];
        memcpy(x->fov, v->fov[eye], sizeof x->fov);
        if (!pic || now_ns() < atomic_load(&g_blank_until)) continue;   // a snap turn: the headset dark while the camera turns
        quad_u u;
        eye_matrix(x->pose.q, x->pose.p, x->fov, u.m);
        memcpy(u.c, g.center, 12); memcpy(u.r, g.right, 12); memcpy(u.u, g.up, 12); u.c[3] = u.r[3] = u.u[3] = 0;
        u.uvs[0] = u.uvs[1] = 1; u.uvs[2] = u.uvs[3] = 0;
        [e setViewport:(MTLViewport){ eye * g.eye_w, 0, g.eye_w, g.eye_h, 0, 1 }];
        [e setRenderPipelineState:g.ps];
        [e setVertexBytes:&u length:sizeof u atIndex:0];
        [e setFragmentTexture:pic atIndex:0]; [e setFragmentSamplerState:g.smp atIndex:0];
        [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        if (hud && hud_quad(&u)) {
            [e setVertexBytes:&u length:sizeof u atIndex:0];
            [e setFragmentTexture:hud atIndex:0];
            [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        }
        if (panel && pw > 0 && ph > 0) {   // the settings panel: in the middle, 55% of the picture's height
            const float ul = sqrtf(g.up[0] * g.up[0] + g.up[1] * g.up[1] + g.up[2] * g.up[2]), rl = sqrtf(g.right[0] * g.right[0] + g.right[1] * g.right[1] + g.right[2] * g.right[2]);
            if (ul > 1e-4f && rl > 1e-4f) {
                const float hh = 0.55f * ul, hw = hh * pw / ph;
                for (int i = 0; i < 3; i++) { u.c[i] = g.center[i]; u.u[i] = g.up[i] / ul * hh; u.r[i] = g.right[i] / rl * hw; }
                u.uvs[0] = u.uvs[1] = 1;
                [e setVertexBytes:&u length:sizeof u atIndex:0];
                [e setFragmentTexture:panel atIndex:0];
                [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
            }
        }
    }
    [e endEncoding];
    const int mi = pic && now_ns() >= atomic_load(&g_blank_until) && mac_view_wanted() ? mac_image() : -1;
    if (mi >= 0) mac_view_encode(cb, g_mac_img[mi], used && g_vr_headlook ? &g.hl_pose[used % 64] : head, v, pic, hud);
    [cb commit]; [cb waitUntilCompleted];
    pthread_mutex_lock(&g.mu); if (s >= 0) g.readers[s]--; pthread_mutex_unlock(&g.mu);
    if (cb.error) { atomic_fetch_add(&g.gpu_errors, 1); VLOG("【红】headset frame GPU error: %s", cb.error.localizedDescription.UTF8String); return 0; }
    g.img_frame[im] = frame;
    if (vr_link_frame_ready(frame, 0, head->ts, 2, vs)) { g.img_frame[im] = 0; return 0; }
    atomic_fetch_add(&g.frames, 1);
    mirror_note(mi >= 0 ? g_mac_surf[mi] : g.surf[im]);
    return 1;
}

/* VR settings (user 2026-10-26: "放在一个新设置 叫 vr设置里 可以选这几种分辨率"): with TOTK_VR_STREAMER=<basalt-streamer>
   the game runs the streamer itself (--wired --runtime TOTK_VR_CONTROL), at the per-eye size picked in the settings
   panel (runtime/settings_panel.h), kept in TOTK_VR_SETTINGS; a new size restarts the streamer and the VR thread
   reconnects. TOTK_VR_MBPS: its bitrate (150). Sizes, measured on this Mac and a Quest 3: 1344x1408 and 1680x1760 hold
   72 Hz (about 81 ms); 1920x2048 about 60 Hz and 120 ms (H.264 encode about 20 ms, Quest decode about 41 ms);
   2048x2208 encodes in about 41 ms and cannot keep up, so it is not offered. */
static pthread_mutex_t g_st_mu = PTHREAD_MUTEX_INITIALIZER;
static pid_t g_streamer; static int g_res_w = 1680, g_res_h = 1760, g_res_loaded;
int vr_streamer_managed(void) { const char *e = getenv("TOTK_VR_STREAMER"); return vr_enabled() && e && *e; }
static void res_load(void) {   // under g_st_mu
    if (g_res_loaded) return;
    g_res_loaded = 1;
    const char *p = getenv("TOTK_VR_SETTINGS"); FILE *f = p ? fopen(p, "r") : NULL; int w, h;
    if (f) { if (fscanf(f, "%dx%d", &w, &h) == 2 && w >= 256 && h >= 256 && w <= 2048 && h <= 2304 && !((w | h) & 1)) { g_res_w = w; g_res_h = h; } fclose(f); }
}
void vr_resolution(int *w, int *h) {
    pthread_mutex_lock(&g_st_mu); res_load();
    if (vr_streamer_managed()) { *w = g_res_w; *h = g_res_h; } else vr_link_view_size(w, h);
    pthread_mutex_unlock(&g_st_mu);
}
static void streamer_stop_locked(void) {
    if (g_streamer <= 0) return;
    kill(g_streamer, SIGTERM);   // the watchdog passes it on; the streamer removes its adb forwards on a clean stop
    int st = 0; for (int i = 0; i < 50 && waitpid(g_streamer, &st, WNOHANG) == 0; i++) usleep(100000);
    if (waitpid(g_streamer, &st, WNOHANG) == 0) { kill(g_streamer, SIGKILL); waitpid(g_streamer, &st, 0); }
    g_streamer = 0;
}
static void streamer_stop(void) { pthread_mutex_lock(&g_st_mu); streamer_stop_locked(); pthread_mutex_unlock(&g_st_mu); }
static void streamer_start_locked(void) {
    extern char **environ;
    const char *bin = getenv("TOTK_VR_STREAMER"), *ctl = getenv("TOTK_VR_CONTROL"), *mb = getenv("TOTK_VR_MBPS");
    char res[32], mbps[16], log[1100]; res_load();
    snprintf(res, sizeof res, "%dx%d", g_res_w, g_res_h); snprintf(mbps, sizeof mbps, "%d", mb && atoi(mb) > 0 && atoi(mb) <= 500 ? atoi(mb) : 150);
    snprintf(log, sizeof log, "%s.streamer.log", ctl);
    unlink(ctl);   // the new streamer writes its own port and token
    posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, log, O_WRONLY | O_CREAT | O_APPEND, 0644);
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
    /* Under a small watchdog shell: it stops the streamer (SIGTERM, which it handles like SIGINT and removes its adb forwards; a background job of sh ignores SIGINT) when the game is gone,
       however the game ended (its exit does not run atexit handlers; a test left the streamer holding the ports), and
       passes our own stop on. */
    static const char *watch =
        "S=$1; G=$2; shift 2; \"$S\" \"$@\" & C=$!; "
        "trap 'kill -TERM $C 2>/dev/null; wait $C; exit 0' INT TERM; "
        "while kill -0 \"$G\" 2>/dev/null && kill -0 $C 2>/dev/null; do sleep 1; done; "
        "kill -TERM $C 2>/dev/null; wait $C";
    char game[16]; snprintf(game, sizeof game, "%d", (int)getpid());
    char *argv[] = { "/bin/sh", "-c", (char *)watch, "vr-streamer", (char *)bin, game, "--wired", "--runtime", (char *)ctl, "--resolution", res, "--mbps", mbps, NULL };
    int r = posix_spawn(&g_streamer, "/bin/sh", &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (r) { g_streamer = 0; VLOG("【红】could not start the streamer %s: %s", bin, strerror(r)); return; }
    VLOG("streamer started: %s per eye, %s Mbps (log %s)", res, mbps, log);
}
/* The streamer exits when the Quest shows no picture (its QuestPictureWatch: a lost USB session does not come back by
   itself; user 2026-10-30: "我发现你永远都不检测 quest 有没有画面"); started again here, as a resolution change does. */
static void streamer_restart_if_gone(void) {   // the VR thread, while not connected
    if (!vr_streamer_managed()) return;
    pthread_mutex_lock(&g_st_mu);
    int st = 0;
    if (g_streamer > 0 && waitpid(g_streamer, &st, WNOHANG) == g_streamer) {
        g_streamer = 0;
        VLOG("the streamer exited (status %d; why: its log); starting it again", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
        streamer_start_locked();
    }
    pthread_mutex_unlock(&g_st_mu);
}
int vr_set_resolution(int w, int h) {   // the settings panel (main thread)
    if (!vr_streamer_managed() || w < 256 || h < 256 || w > 2048 || h > 2304 || (w | h) & 1) return 0;
    pthread_mutex_lock(&g_st_mu); res_load();
    if (w != g_res_w || h != g_res_h) {
        g_res_w = w; g_res_h = h;
        const char *p = getenv("TOTK_VR_SETTINGS"); FILE *f = p ? fopen(p, "w") : NULL;
        if (f) { fprintf(f, "%dx%d\n", w, h); fclose(f); }
        VLOG("VR resolution %dx%d per eye: restarting the streamer", w, h);
        streamer_stop_locked(); streamer_start_locked();   // the VR thread sees the teardown and reconnects
    }
    pthread_mutex_unlock(&g_st_mu);
    return 1;
}

/* Audio (user 2026-10-26: "没有声音 加上声音"): the game's own mix goes to the headset over the same link (message 100,
   a local streamer change in third_party_patches), no loopback device or rerouted Mac output. runtime/audio_mixer.c's
   output callback hands each block to vr_audio_tap on the CoreAudio thread: converted to PCM16 into a lock-free ring
   (one writer, one reader); a sender thread ships 20 ms blocks (3840 bytes, under the link's 4096-byte message). While the headset is connected the Mac's speakers are
   silenced, else the two play about 100 ms apart (TOTK_VR_AUDIO_MAC=1 keeps both); TOTK_VR_AUDIO=0: no headset audio.
   48 kHz only (the game's renderer runs at it; another rate is reported and left on the Mac). */
enum { AUD_RING = 48000, AUD_BLOCK = 960 };   // 20 ms: the streamer keeps one block in flight and drops one that comes while it is busy (10 ms blocks lost about 6%)
static int16_t g_aud[AUD_RING * 2];
static _Atomic uint64_t g_aud_wr, g_aud_rd, g_aud_sent, g_aud_drops;
static _Atomic int g_link_up, g_aud_bad_rate;
static int g_aud_keep_mac;
extern void (*runtime_audio_tap)(float *out, unsigned frames, int rate) __attribute__((weak_import));
static void vr_audio_tap(float *out, unsigned frames, int rate) {   // CoreAudio thread: no locks, no logging
    if (!atomic_load_explicit(&g_link_up, memory_order_acquire)) return;
    if (rate != 48000) { atomic_store(&g_aud_bad_rate, rate); return; }
    uint64_t wr = atomic_load_explicit(&g_aud_wr, memory_order_relaxed), rd = atomic_load_explicit(&g_aud_rd, memory_order_acquire);
    if (wr - rd + frames > AUD_RING) atomic_fetch_add(&g_aud_drops, 1);
    else {
        for (unsigned i = 0; i < frames; i++) {
            uint64_t k = (wr + i) % AUD_RING;
            for (int c = 0; c < 2; c++) { float v = out[2 * i + c] * 32767.0f; g_aud[2 * k + c] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
        }
        atomic_store_explicit(&g_aud_wr, wr + frames, memory_order_release);
    }
    if (!g_aud_keep_mac) memset(out, 0, (size_t)frames * 2 * sizeof(float));
}
static void *audio_main(void *arg) {
    (void)arg; int16_t block[AUD_BLOCK * 2]; int said_rate = 0;
    for (;;) {
        if (!said_rate && atomic_load(&g_aud_bad_rate)) { said_rate = 1; VLOG("【红】game audio at %d Hz, not 48000: it stays on the Mac", atomic_load(&g_aud_bad_rate)); }
        uint64_t rd = atomic_load_explicit(&g_aud_rd, memory_order_relaxed), wr = atomic_load_explicit(&g_aud_wr, memory_order_acquire);
        if (!atomic_load(&g_link_up)) { atomic_store(&g_aud_rd, wr); usleep(20000); continue; }
        if (wr - rd < AUD_BLOCK) { usleep(3000); continue; }
        for (int i = 0; i < AUD_BLOCK; i++) { uint64_t k = (rd + i) % AUD_RING; block[2 * i] = g_aud[2 * k]; block[2 * i + 1] = g_aud[2 * k + 1]; }
        atomic_store_explicit(&g_aud_rd, rd + AUD_BLOCK, memory_order_release);
        if (!vr_link_send_audio(block, AUD_BLOCK)) atomic_fetch_add(&g_aud_sent, 1);
    }
    return NULL;
}
static void audio_start(void) {
    const char *e = getenv("TOTK_VR_AUDIO"), *mac = getenv("TOTK_VR_AUDIO_MAC");
    if ((e && !strcmp(e, "0")) || !&runtime_audio_tap) { VLOG("headset audio off (%s)", e ? "TOTK_VR_AUDIO=0" : "no mixer hook"); return; }
    g_aud_keep_mac = mac && !strcmp(mac, "1");
    pthread_t t; if (pthread_create(&t, NULL, audio_main, NULL)) { VLOG("【红】no audio thread: headset audio off"); return; }
    pthread_detach(t);
    runtime_audio_tap = vr_audio_tap;
    VLOG("headset audio on: the game's mix at 48 kHz over the link%s", g_aud_keep_mac ? ", the Mac plays it too (TOTK_VR_AUDIO_MAC=1)" : "; the Mac's speakers are silent while the headset is connected (TOTK_VR_AUDIO_MAC=1 keeps them)");
}

static void release_images(void) {
    mirror_note(NULL);
    for (int i = 0; i < IMAGES; i++) { g.img[i] = nil; if (g.surf[i]) { CFRelease(g.surf[i]); g.surf[i] = NULL; } g.img_frame[i] = 0; }
}

static void *vr_main(void *arg) {
    (void)arg;
    const char *path = getenv("TOTK_VR_CONTROL");
    if (vr_streamer_managed()) { pthread_mutex_lock(&g_st_mu); streamer_start_locked(); pthread_mutex_unlock(&g_st_mu); atexit(streamer_stop); }
    audio_start();
    for (int session = 1;; session++) {   // a streamer restart (a new size) or a lost headset: connect again
        char err[256]; int said = 0;
        for (;;) {   // the streamer may start after the game: try every 2 s, say why once
            if (!vr_link_open(path, "MacNativeGames (Tears of the Kingdom)", err, sizeof err)) break;
            if (!said) { VLOG("not connected yet: %s; retrying every 2 s", err); said = 1; }
            streamer_restart_if_gone();
            sleep(2);
        }
        vr_link_view_size(&g.eye_w, &g.eye_h);
        if (!setup_images()) { vr_link_close(); release_images(); sleep(2); continue; }
        VLOG("connected to the BasaltVR streamer: %dx%d per eye, %d images (session %d)", g.eye_w, g.eye_h, IMAGES, session);
        atomic_store(&g_link_up, 1);
        vr_pose head; uint64_t last = 0, frame = 0, t_stats = now_ns(); static int placed;
        vr_views v = { 0.063f, { { -0.82f, 0.82f, 0.82f, -0.82f }, { -0.82f, 0.82f, 0.82f, -0.82f } } };
        /* One frame per headset refresh: the streamer sends head poses far more often (about 205 a second on a Quest 3)
           and its encoder drops what comes faster than the refresh, so rendering every pose doubled the GPU work for
           nothing. TOTK_VR_HZ: the refresh the streamer negotiated (its --fps, 72 by default). The newest pose is taken
           just before rendering. */
        static double hz = 0; if (!hz) { const char *e = getenv("TOTK_VR_HZ"); hz = e ? atof(e) : 72; if (hz < 30 || hz > 144) hz = 72; }
        const uint64_t period = (uint64_t)(1e9 / hz);
        uint64_t t_next = 0;   // a fixed grid of refresh periods, so the rate holds without drifting
        VLOG("headset frames at %.0f Hz (TOTK_VR_HZ)", hz);
        while (vr_link_alive()) {
            @autoreleasepool {
                if (!vr_link_wait_pose(&head, last, 50)) { atomic_fetch_add(&g.no_pose, 1); continue; }   // no new pose in 50 ms: the headset is asleep or the stream stalled
                uint64_t now = now_ns();
                if (t_next && now < t_next) {
                    struct timespec ts = { 0, (long)(t_next - now) }; nanosleep(&ts, NULL);
                    vr_link_wait_pose(&head, last, 0);   // the newest pose after the wait
                    now = now_ns();
                }
                t_next = t_next && now < t_next + period ? t_next + period : now + period;   // behind by a whole period: restart the grid
                if (frames_out() >= ENCODER_SLOTS) {   // the encoder is full: render when it gives a frame back (see frames_out)
                    atomic_fetch_add(&g.encoder_waits, 1);
                    if (!vr_link_wait_consumed(50)) continue;
                    vr_link_wait_pose(&head, last, 0);   // the newest pose after the wait
                }
                last = head.ts;
                if (vr_link_views(&v)) headlook_fov(&v);
                int rc = atomic_exchange(&g.recenter, 0);
                if (!placed || rc) { place_screen(&head); placed = 1; }
                headlook_update(&head, rc);
            first_person_update();
                /* The streamer acknowledges frames strictly in order (a missing number stops every later FRAME_CONSUMED,
                   and with all images held the headset gets nothing more): only a frame actually handed over takes a number. */
                if (render_frame(&head, &v, frame + 1)) frame++;
                if (now_ns() - t_stats > 10000000000ull) { char b[2048]; vr_stats(b, sizeof b); VLOG("%s", b); t_stats = now_ns(); }
            }
        }
        atomic_store(&g_link_up, 0);
        VLOG("the streamer disconnected (session %d); connecting again", session);
        release_images();
    }
    return NULL;
}

static void start_thread(id<MTLDevice> dev) {   // present thread, once
    NSError *e = nil;
    MTLResidencySetDescriptor *rd = [MTLResidencySetDescriptor new]; rd.label = @"TOTK VR";
    g.rs = [dev newResidencySetWithDescriptor:rd error:&e];
    g.dev = dev;
    pthread_t th;
    if (!g.rs || !setup_gpu() || pthread_create(&th, NULL, vr_main, NULL)) {
        VLOG("【红】VR off: %s", e ? e.localizedDescription.UTF8String : "Metal objects or the VR thread could not be made");
        g.failed = 1; g.dev = nil; return;
    }
    pthread_detach(th);
    { extern int (*runtime_vr_pad)(int, uint64_t *, int32_t *, int32_t *, int32_t *, int32_t *); __atomic_store_n(&runtime_vr_pad, vr_gamepad, __ATOMIC_RELEASE); }   // the Quest controllers into runtime_gamepad_read
    { extern void (*runtime_vr_haptic)(int, float, float); __atomic_store_n(&runtime_vr_haptic, vr_haptic, __ATOMIC_RELEASE); }   // and the game's vibration out to them
    { extern void (*runtime_vr_recenter)(void); __atomic_store_n(&runtime_vr_recenter, vr_recenter, __ATOMIC_RELEASE); }   // a gamepad's L3 + R3 tap recentres
    VLOG("on: the game's picture goes to the headset (TOTK_VR_CONTROL=%s)", getenv("TOTK_VR_CONTROL"));
}

void vr_stats(char *out, size_t n) {
    char l[160]; vr_link_stats(l, sizeof l);
    float qs, ql; char q[1200];   // what the Quest reports it showed (vr_link_headset), and the game camera's view
    if (!vr_link_headset(&qs, &ql)) snprintf(q, sizeof q, "the Quest reports nothing (older streamer, or not connected)");
    else if (qs > 0) snprintf(q, sizeof q, "the Quest shows %.0f fps, %.0f ms", qs, ql);
    else snprintf(q, sizeof q, "【红】the Quest shows no picture");
    { extern int vr_body_carry(float off[3], uint64_t max_age_ns); float c[3];
      if (vr_body_carry(c, 500000000ull)) { size_t k = strlen(q); snprintf(q + k, sizeof q - k, "; the camera bound to the body %.2f %.2f %.2f m from the position read", c[0], c[1], c[2]); }
      extern int vr_body_lag(int *lag, float lr[3], uint64_t max_age_ns); int lg; float r[3];
      if (vr_body_lag(&lg, r, 500000000ull)) { size_t k = strlen(q); snprintf(q + k, sizeof q - k, " (drawn %d camera updates back, %.2f %.2f %.2f m off it)", lg, r[0], r[1], r[2]); } }
    { extern _Atomic int g_vr_gap_read_cm, g_vr_gap_cam_cm; int gr = atomic_exchange(&g_vr_gap_read_cm, 0), gc = atomic_exchange(&g_vr_gap_cam_cm, 0);
      if (gr || gc) { size_t k = strlen(q); snprintf(q + k, sizeof q - k, "; the drawn body at most %.2f m across from the position read, %.2f m from the camera", gr / 100.0, gc / 100.0); } }
    { extern _Atomic int g_vr_head_hid, g_vr_head_missed, g_vr_head_shown, g_vr_head_by_bone; int hh = atomic_exchange(&g_vr_head_hid, 0), hm = atomic_exchange(&g_vr_head_missed, 0), hs = atomic_exchange(&g_vr_head_shown, 0), hb = atomic_exchange(&g_vr_head_by_bone, 0);   // nvn_mtl.m: the head left out
      if (hh || hm) { size_t k = strlen(q); snprintf(q + k, sizeof q - k, "; head meshes left out %d (%d by his head bone), not taken as the player's %d (shadows and other characters' too; %d in colour passes)", hh, hb, hm, hs); }
      extern _Atomic int g_vr_head_by_buffer; const int hbuf = atomic_exchange(&g_vr_head_by_buffer, 0);
      if (hbuf) { size_t k = strlen(q); snprintf(q + k, sizeof q - k, "; %d of them by a buffer found his before", hbuf); } }
    {   // the camera hook's calls against the game's pictures since the last line: one each normally (a second camera on the
        // main camera object would show here: the Zonai device menu, user 2026-11-01 "人物消失" only with the hook)
        static uint32_t c0; static unsigned long long p0; const uint32_t c = g_vr_headlook ? __atomic_load_n(&g_vr_headlook->calls, __ATOMIC_RELAXED) : 0;
        const unsigned long long p = atomic_load(&g.copies);
        if (c0 || p0) { size_t k = strlen(q); snprintf(q + k, sizeof q - k, "; camera hook calls %u for %llu game pictures", c - c0, p - p0); }
        c0 = c; p0 = p; }
    { const int sd = atomic_exchange(&g_spring_dev_mm, 0);
      if (sd) { size_t k = strlen(q); snprintf(q + k, sizeof q - k, "; the head's spring (%d ms) kept the camera at most %.1f cm off the head bone's eyes", vr_eye_spring_ms(), sd / 10.0); } }
    { static uint32_t v0; const uint32_t v = g_vr_headlook ? __atomic_load_n(&g_vr_headlook->vs_changes, __ATOMIC_RELAXED) : 0;   // the main view set (game_patches_v143.c vr_view0_hook)
      if (v != v0) { size_t k = strlen(q); snprintf(q + k, sizeof q - k, "; the main picture's view set changed %u times", v - v0); } v0 = v; }
    { int lo = atomic_exchange(&g_cam_fov_lo, INT_MAX), hi = atomic_exchange(&g_cam_fov_hi, INT_MIN);
      if (lo <= hi) { size_t k = strlen(q); snprintf(q + k, sizeof q - k, "; the game camera's vertical view %.1f-%.1f degrees (the headset's %.1f)",
                                                     lo / 10.0, hi / 10.0, g.fov_on > 0 ? 2 * atan(g.fov_tanh) * 180 / M_PI : 0.0); } }
    snprintf(out, n, "%s; headset frames %llu, game pictures copied %llu, skipped (all images in the streamer) %llu, waits for the encoder %llu, waits for a pose %llu, GPU errors %llu; head pose of frames matched %llu (by the rotation alone %llu) / missed %llu, held by the dead band %llu; audio blocks sent %llu, dropped %llu; %s", q,
             (unsigned long long)atomic_load(&g.frames), (unsigned long long)atomic_load(&g.copies), (unsigned long long)atomic_load(&g.no_image),
             (unsigned long long)atomic_load(&g.encoder_waits), (unsigned long long)atomic_load(&g.no_pose), (unsigned long long)atomic_load(&g.gpu_errors),
             (unsigned long long)atomic_load(&g.seq_matched), (unsigned long long)atomic_load(&g.seq_matched_rot), (unsigned long long)atomic_load(&g.seq_missed), (unsigned long long)atomic_load(&g.hl_held),
             (unsigned long long)atomic_load(&g_aud_sent), (unsigned long long)atomic_load(&g_aud_drops), l);
}
#endif
