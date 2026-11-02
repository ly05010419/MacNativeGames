// Touchscreen: the front end pushes the whole set of touch points (dm_touch_set, front.h), and the game reads them every frame through nn::hid::GetTouchScreenState(s) (hle_hid.c → touch_fill_n).
// How the game reads (main 0x1b65260): it clears 16 states, asks for 16, and looks only at state 0 -- the touch count and each touch point's finger_id / x / y; the panel coordinate system is 1280x720
// (it writes the constant 0x2d0_00000500 = 720 << 32 | 1280 in the same function). Therefore:
//   · front-end normalized coordinates [0,1) (origin at the top left of the output picture) → x = nx·1280, y = ny·720, clamped to [0,1279] / [0,719];
//   · short-press latch: the game only looks at the newest state, so a finger pressed and lifted between two reads would be lost entirely without a latch -- in this table every finger is first reported once as Start,
//     and is released only after being reported once more as End after lifting (pressed and lifted between two reads = Start in this report, End in the next).
// TouchState (0x28 bytes, = libnx HidTouchState): +0 delta_time (ns, i64), +8 attributes (bit0 Start / bit1 End), +0xc finger_id,
// +0x10 x, +0x14 y, +0x18 diameter x, +0x1c diameter y, +0x20 rotation angle, +0x24 reserved. TouchScreenState<16>: +0 sample number, +8 touch count, 16 TouchState from +0x10.
#include "dm.h"
#include "front.h"
#include <math.h>
#include <pthread.h>
enum { F_EMPTY, F_DOWN, F_LIFTED };
static struct { int st, reported, again; float x, y, ax, ay; } g_f[16];   // again/ax/ay: the same id was pressed again before End went out (the next read reports End first, then the new Start)
static struct { int down, pend; float x, y; } g_gf[16];   // the gesture recognizer's own view of the fingers; pend: pressed since its last sample (a press and lift between two samples still counts once)
static pthread_mutex_t g_tl = PTHREAD_MUTEX_INITIALIZER;
static int64_t g_tsamp; static uint64_t g_tlast_ns;

void dm_touch_set(const dm_touch *t, int n) {
    pthread_mutex_lock(&g_tl);
    int seen[16] = {0};
    for (int i = 0; i < n; i++) {
        uint32_t id = t[i].id; if (id >= 16) continue;
        seen[id] = 1;
        if (!g_gf[id].down) g_gf[id].pend = 1;
        g_gf[id].down = 1; g_gf[id].x = t[i].x; g_gf[id].y = t[i].y;
        if (g_f[id].st == F_LIFTED && g_f[id].reported) { g_f[id].again = 1; g_f[id].ax = t[i].x; g_f[id].ay = t[i].y; continue; }
        if (g_f[id].st == F_EMPTY) { g_f[id].st = F_DOWN; g_f[id].reported = 0; }
        else if (g_f[id].st == F_LIFTED) g_f[id].st = F_DOWN;              // Pressed again before the lift was reported: treat it as held down all along
        g_f[id].x = t[i].x; g_f[id].y = t[i].y;
    }
    for (int id = 0; id < 16; id++) {
        if (!seen[id]) g_gf[id].down = 0;
        if (!seen[id] && g_f[id].st == F_DOWN) g_f[id].st = F_LIFTED;
        if (!seen[id] && g_f[id].again) g_f[id].again = 0;                   // Pressed again and then lifted again (End not even reported yet): drop the re-press, still report End for the original lift
    }
    pthread_mutex_unlock(&g_tl);
}

static int32_t clampi(float v, int hi) { int32_t r = (int32_t)(v * (float)(hi + 1)); return r < 0 ? 0 : r > hi ? hi : r; }
// Fill one TouchScreenState<max> (out is 0x10 + 0x28 · max bytes; max 1–16); now_ns is used for delta_time. Returns the number of touch points written.
// Fingers beyond max are left for a later read (their Start / End is not used up)
int touch_fill_n(void *out, int max, uint64_t now_ns) {
    if (max < 1) max = 1; else if (max > 16) max = 16;
    uint8_t *o = out; memset(o, 0, 0x10 + 0x28 * (size_t)max);
    pthread_mutex_lock(&g_tl);
    int64_t samp = ++g_tsamp; int64_t dt = g_tlast_ns ? (int64_t)(now_ns - g_tlast_ns) : 0; g_tlast_ns = now_ns;
    int k = 0;
    for (int id = 0; id < 16 && k < max; id++) {
        if (g_f[id].st == F_EMPTY) continue;
        uint32_t attr = 0;
        if (!g_f[id].reported) { attr |= 1; g_f[id].reported = 1; }          // Start (reported for the first time this time)
        else if (g_f[id].st == F_LIFTED) attr |= 2;                         // End (Start already reported, already lifted)
        uint8_t *s = o + 0x10 + 0x28 * k++;
        int32_t x = clampi(g_f[id].x, 1279), y = clampi(g_f[id].y, 719), d = 30, zero = 0, fid = id;
        memcpy(s + 0x00, &dt, 8); memcpy(s + 0x08, &attr, 4); memcpy(s + 0x0c, &fid, 4);
        memcpy(s + 0x10, &x, 4); memcpy(s + 0x14, &y, 4); memcpy(s + 0x18, &d, 4); memcpy(s + 0x1c, &d, 4); memcpy(s + 0x20, &zero, 4);
        if (attr & 2) {                                                      // End has been reported: release; if the same id was pressed again in the meantime, turn it into a new press
            if (g_f[id].again) { g_f[id].st = F_DOWN; g_f[id].reported = 0; g_f[id].again = 0; g_f[id].x = g_f[id].ax; g_f[id].y = g_f[id].ay; }
            else g_f[id].st = F_EMPTY;
        }
    }
    pthread_mutex_unlock(&g_tl);
    int32_t cnt = k; memcpy(o + 0, &samp, 8); memcpy(o + 8, &cnt, 4);
    // DM_TOUCH_LOG=1 (acceptance diagnostics): each read with non-zero touch points logs one line -- sample number, touch count, and for each point finger_id / panel x,y / attributes (1 Start, 2 End); capped at 500 lines
    static int lg = -1, nl; if (lg < 0) lg = getenv("DM_TOUCH_LOG") != NULL;
    if (lg && k && nl < 500) {
        char b[256]; int m = snprintf(b, sizeof b, "touch read sample %lld points %d", (long long)samp, k);
        for (int i = 0; i < k && m < (int)sizeof b - 40; i++) { int32_t v[4]; memcpy(v, o + 0x10 + 0x28 * i + 0x08, 16);
            m += snprintf(b + m, sizeof b - m, " | #%d (%d,%d) attribute %d", v[1], v[2], v[3], v[0]); }
        LOG("%s", b); if (++nl == 500) LOG("DM_TOUCH_LOG reached 500 lines, logging stopped");
    }
    return k;
}
int touch_fill(void *out, uint64_t now_ns) { return touch_fill_n(out, 16, now_ns); }   // TouchScreenState<16>, 0x290 bytes

// Gestures: nn::hid::GetGestureStates hands the game the newest entries of a gesture recognizer's history (newest first, 16 kept), which Horizon's hid builds from the touch screen.
// Taiko Risshiden V DX reads its taps, pans and swipes only there (main+0x42f1ac acts on types 5–9; its GetTouchScreenState read only tracks the finger), so its menus took no touch while
// this answered 0 entries. The rules are yuzu's (hid_core gesture_handler.cpp): a new touch is Touch (IsNewTouch, unless the entry before is Cancel), one held still for 0.5 s
// becomes Press, a moving one Pan (delta from the last entry's position, velocity over the time since it), two or more fingers whose spread changes by more than 0.5 px Pinch
// (scale = new / old spread) or whose angle turns more than 0.015 rad Rotate (degrees), and a change in the finger count Cancel. On release Touch becomes Tap (IsDoubleTap within
// 0.35 s of the last tap; Press does not tap), Pan becomes Swipe above 400 px/s (direction by the larger delta) or Complete, the others Complete; an Idle entry follows a Complete
// or a Cancel. An entry is written only when the fingers moved, after a forced step (Tap → Complete → Idle) or when a Touch is due to become Press. Unlike yuzu, a gesture stays
// put until a finger has moved GESTURE_SLOP pixels from where it went down: a fingertip on a phone wobbles a pixel or two, and every tap would have come out a Pan.
// Sampled at each read (the games read once or twice a frame). GestureState (0x60 bytes, = libnx HidGestureState): +0 sampling number, +8 context number (gestures detected),
// +0x10 type, +0x14 direction, +0x18 x, +0x1c y (the fingers' mid point), +0x20 delta x, +0x24 delta y, +0x28 velocity x, +0x2c velocity y (px/s), +0x30 attributes (bit 4 IsNewTouch,
// bit 8 IsDoubleTap), +0x34 scale, +0x38 rotation angle, +0x3c point count, +0x40 four points (x, y); coordinates on the 1280x720 panel as the touch states
enum { GT_IDLE, GT_COMPLETE, GT_CANCEL, GT_TOUCH, GT_PRESS, GT_TAP, GT_PAN, GT_SWIPE, GT_PINCH, GT_ROTATE };
enum { GD_NONE, GD_LEFT, GD_UP, GD_RIGHT, GD_DOWN };
#define GESTURE_SLOP 8
typedef struct { int64_t samp, ctx; int32_t type, dir, x, y, dx, dy; float vx, vy; uint32_t attr; float scale, rot; int32_t npt, pt[4][2]; } gesture_state;
_Static_assert(sizeof(gesture_state) == 0x60, "GestureState is 0x60 bytes");
typedef struct { int n; int32_t pt[4][2], mx, my; float dist, angle; } gesture_props;
static gesture_state g_gr[16]; static uint64_t g_gw;   // history: g_gr[(g_gw - 1) % 16] is the newest of g_gw written
static gesture_props g_glast, g_gdown;                  // the fingers of the last entry; where the gesture's fingers went down
static int64_t g_gctx; static int g_gforce, g_gtap_ok, g_gslop;
static uint64_t g_gupd_ns, g_gtap_ns; static float g_gpan_dt;

static void gesture_read(gesture_props *g) {   // the fingers down now (or pressed since the last sample), in finger id order, at most 4
    memset(g, 0, sizeof *g);
    for (int id = 0; id < 16; id++) {
        if ((g_gf[id].down || g_gf[id].pend) && g->n < 4) { g->pt[g->n][0] = clampi(g_gf[id].x, 1279); g->pt[g->n][1] = clampi(g_gf[id].y, 719); g->n++; }
        g_gf[id].pend = 0;
    }
    for (int i = 0; i < g->n; i++) { g->mx += g->pt[i][0] / g->n; g->my += g->pt[i][1] / g->n; }
    for (int i = 0; i < g->n; i++) g->dist += hypotf((float)(g->mx - g->pt[i][0]), (float)(g->my - g->pt[i][1])) / (float)g->n;
    if (g->n) g->angle = atan2f((float)(g->my - g->pt[0][1]), (float)(g->mx - g->pt[0][0]));
}
static void gesture_sample(uint64_t now) {
    gesture_props g; gesture_read(&g);
    const gesture_state *le = g_gw ? &g_gr[(g_gw - 1) % 16] : NULL;
    int ltype = le ? le->type : GT_IDLE;
    float dt = g_gupd_ns ? (float)(now - g_gupd_ns) / 1e9f : 0;
    if (g.n && g.n == g_glast.n && !g_gslop) {   // within the slop the fingers stay where they went down
        int out = 0;
        for (int i = 0; i < g.n; i++) out |= abs(g.pt[i][0] - g_gdown.pt[i][0]) > GESTURE_SLOP || abs(g.pt[i][1] - g_gdown.pt[i][1]) > GESTURE_SLOP;
        if (out) g_gslop = 1; else g = g_glast;
    }
    int upd = g_gforce || g.n != g_glast.n || memcmp(g.pt, g_glast.pt, sizeof g.pt);
    if (!upd && ltype == GT_TOUCH && le->npt == 1 && dt > 0.5f) upd = g_gtap_ok;   // due to become Press
    if (!upd) return;
    g_gforce = 0;
    gesture_state s; memset(&s, 0, sizeof s); s.samp = le ? le->samp + 1 : 1;
    int type = GT_IDLE; uint32_t attr = 0;
    if (g.n) {
        if (!g_glast.n) {   // a new gesture
            g_gctx++; type = GT_TOUCH; g_gdown = g; g_gslop = 0;
            if (ltype != GT_CANCEL) { attr |= 1u << 4; g_gtap_ok = 1; }
        } else if (g.n != g_glast.n) {   // the finger count changed: cancel and start over at the next sample
            type = GT_CANCEL; g_gtap_ok = 0; memset(&g, 0, sizeof g);
        } else if (memcmp(g.pt, g_glast.pt, sizeof g.pt)) {
            type = GT_PAN; s.dx = g.mx - (le ? le->x : g.mx); s.dy = g.my - (le ? le->y : g.my);
            if (dt > 0) { s.vx = (float)s.dx / dt; s.vy = (float)s.dy / dt; }
            g_gpan_dt = dt;
            if (fabsf(g.dist - g_glast.dist) > 0.5f) { type = GT_PINCH; s.scale = g_glast.dist > 0 ? g.dist / g_glast.dist : 1; }
            float turn = atanf((g.angle - g_glast.angle) / (1 + g.angle * g_glast.angle));
            if (fabsf(turn) > 0.015f) { type = GT_ROTATE; s.scale = 0; s.rot = turn * 180.0f / (float)M_PI; }
        } else if (ltype == GT_TOUCH) type = GT_PRESS;
    } else if (g_glast.n) {   // released
        switch (ltype) {
        case GT_TOUCH:
            g_gforce = 1;
            if (!g_gtap_ok) { type = GT_CANCEL; break; }
            type = GT_TAP; g = g_glast;
            if (g_gtap_ns && now - g_gtap_ns < 350000000ull) attr |= 1u << 8;
            g_gtap_ns = now; break;
        case GT_PRESS: case GT_TAP: case GT_SWIPE: case GT_PINCH: case GT_ROTATE: type = GT_COMPLETE; g_gforce = 1; break;
        case GT_PAN: {
            float t = g_gpan_dt + dt, vx = t > 0 ? (float)le->dx / t : 0, vy = t > 0 ? (float)le->dy / t : 0;
            g_gforce = 1;
            if (hypotf(vx, vy) <= 400.0f) { type = GT_COMPLETE; break; }
            type = GT_SWIPE; g = g_glast; s.vx = vx; s.vy = vy; s.dx = le->dx; s.dy = le->dy;
            s.dir = abs(s.dx) > abs(s.dy) ? (s.dx > 0 ? GD_RIGHT : GD_LEFT) : (s.dy > 0 ? GD_DOWN : GD_UP);
            break;
        }
        default: break;
        }
    } else if (ltype == GT_COMPLETE || ltype == GT_CANCEL) g_gctx++;
    s.ctx = g_gctx; s.type = type; s.attr = attr; s.x = g.mx; s.y = g.my; s.npt = g.n; memcpy(s.pt, g.pt, sizeof s.pt);
    g_glast = g; g_gupd_ns = now; g_gr[g_gw++ % 16] = s;
    static int lg = -1, nl; if (lg < 0) lg = getenv("DM_TOUCH_LOG") != NULL;   // DM_TOUCH_LOG=1 also logs each gesture entry (up to 500 lines)
    if (lg && nl < 500) { LOG("gesture #%lld context %lld type %d (%d,%d) delta (%d,%d) points %d attributes %#x direction %d", (long long)s.samp, (long long)s.ctx, s.type, s.x, s.y, s.dx, s.dy, s.npt, s.attr, s.dir); nl++; }
}
// Fill up to n GestureStates, newest first; returns how many
int gesture_fill(void *out, int n, uint64_t now_ns) {
    if (n <= 0) return 0;
    pthread_mutex_lock(&g_tl);
    gesture_sample(now_ns);
    int k = g_gw < 16 ? (int)g_gw : 16; if (k > n) k = n;
    for (int i = 0; i < k; i++) memcpy((uint8_t *)out + 0x60 * (size_t)i, &g_gr[(g_gw - 1 - (uint64_t)i) % 16], 0x60);
    pthread_mutex_unlock(&g_tl);
    return k;
}
