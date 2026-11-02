// Gesture recognizer unit test: dm_touch_set -> gesture_fill (hle_touch.c), the GestureState entries (0x60 bytes, newest first) checked field by field.
// Expected values are hand-written from the rules in hle_touch.c (yuzu's, plus the 8 px slop), not computed by calling the implementation under test.
// Coordinates are exact binary fractions of the 1280x720 panel, so the pixel values are exact.
#include <stdio.h>
#include <string.h>
#include <stdint.h>
typedef struct { uint32_t id; float x, y; } dm_touch;
void dm_touch_set(const dm_touch *t, int n);
int gesture_fill(void *out, int n, uint64_t now_ns);
typedef struct { int64_t samp, ctx; int32_t type, dir, x, y, dx, dy; float vx, vy; uint32_t attr; float scale, rot; int32_t npt, pt[4][2]; } gesture_state;
enum { IDLE, COMPLETE, CANCEL, TOUCH, PRESS, TAP, PAN, SWIPE, PINCH, ROTATE };
enum { NEW = 1u << 4, DOUBLE = 1u << 8 };
static int fails, cases;
static gesture_state g[16]; static int gn;
static const uint64_t S = 1000000000ull, MS = 1000000ull;
static void rd(uint64_t t) { memset(g, 0xcc, sizeof g); gn = gesture_fill(g, 16, t); }
static void check(const char *what, int ok) { cases++; if (!ok) fails++; printf("%s %s\n", ok ? "✓" : "✗", what); }
// The newest entry: sampling number, context number, type, position, delta, attributes, direction, point count
static void newest(const char *what, int64_t samp, int64_t ctx, int type, int x, int y, int dx, int dy, uint32_t attr, int dir, int npt) {
    const gesture_state *e = &g[0];
    int ok = gn > 0 && e->samp == samp && e->ctx == ctx && e->type == type && e->x == x && e->y == y && e->dx == dx && e->dy == dy && e->attr == attr && e->dir == dir && e->npt == npt;
    if (!ok && gn > 0) printf("  got #%lld context %lld type %d (%d,%d) delta (%d,%d) attributes %#x direction %d points %d (%d entries)\n", (long long)e->samp, (long long)e->ctx, e->type,
                              e->x, e->y, e->dx, e->dy, e->attr, e->dir, e->npt, gn);
    check(what, ok);
}
static void one(float x, float y) { dm_touch t = {0, x, y}; dm_touch_set(&t, 1); }
static void up(void) { dm_touch_set(NULL, 0); }
int main(void) {
    rd(1 * S); check("no touch yet: no entries", gn == 0);
    // 1 A tap: Touch (IsNewTouch), unchanged reads add nothing, release → Tap at the same point, then Complete and Idle (the next context)
    one(0.5f, 0.5f); rd(1 * S);
    newest("tap: Touch (640,360), IsNewTouch", 1, 1, TOUCH, 640, 360, 0, 0, NEW, 0, 1);
    rd(1 * S + 16 * MS); check("tap: a read with nothing changed adds no entry", gn == 1 && g[0].samp == 1);
    up(); rd(1 * S + 33 * MS); newest("tap: Tap (640,360)", 2, 1, TAP, 640, 360, 0, 0, 0, 0, 1);
    check("tap: entries newest first (Tap, then Touch)", gn == 2 && g[1].samp == 1 && g[1].type == TOUCH);
    rd(1 * S + 50 * MS); newest("tap: Complete", 3, 1, COMPLETE, 0, 0, 0, 0, 0, 0, 0);
    rd(1 * S + 66 * MS); newest("tap: Idle, next context", 4, 2, IDLE, 0, 0, 0, 0, 0, 0, 0);
    rd(1 * S + 83 * MS); check("tap: nothing more after Idle", gn == 4 && g[0].samp == 4);
    // 2 A second tap 83 ms after the first: IsDoubleTap
    one(0.5f, 0.5f); rd(1 * S + 100 * MS); newest("double tap: Touch", 5, 3, TOUCH, 640, 360, 0, 0, NEW, 0, 1);
    up(); rd(1 * S + 116 * MS); newest("double tap: Tap with IsDoubleTap", 6, 3, TAP, 640, 360, 0, 0, DOUBLE, 0, 1);
    rd(1 * S + 133 * MS); rd(1 * S + 150 * MS); newest("double tap: Complete, then Idle", 8, 4, IDLE, 0, 0, 0, 0, 0, 0, 0);
    // 3 A finger that wobbles 5 px (under the 8 px slop) still taps, at the point where it went down
    one(0.25f, 0.25f); rd(2 * S); newest("slop: Touch (320,180)", 9, 5, TOUCH, 320, 180, 0, 0, NEW, 0, 1);
    one(0.25390625f, 0.25f); rd(2 * S + 16 * MS); check("slop: a 5 px move adds no entry", gn > 0 && g[0].samp == 9);
    up(); rd(2 * S + 33 * MS); newest("slop: Tap at the down point (320,180), not a double tap (0.9 s later)", 10, 5, TAP, 320, 180, 0, 0, 0, 0, 1);
    rd(2 * S + 50 * MS); rd(2 * S + 66 * MS); newest("slop: Complete, then Idle", 12, 6, IDLE, 0, 0, 0, 0, 0, 0, 0);
    // 4 A slow drag: Pan (delta from the last entry, velocity over the time since it), released at 100 px/s → Complete
    one(0.5f, 0.5f); rd(3 * S); newest("slow pan: Touch", 13, 7, TOUCH, 640, 360, 0, 0, NEW, 0, 1);
    one(0.515625f, 0.5f); rd(3 * S + 100 * MS); newest("slow pan: Pan (660,360) delta (20,0)", 14, 7, PAN, 660, 360, 20, 0, 0, 0, 1);
    check("slow pan: velocity 200 px/s", g[0].vx > 199.9f && g[0].vx < 200.1f && g[0].vy == 0);
    up(); rd(3 * S + 200 * MS); newest("slow pan: released at 100 px/s → Complete", 15, 7, COMPLETE, 0, 0, 0, 0, 0, 0, 0);
    rd(3 * S + 216 * MS); newest("slow pan: Idle", 16, 8, IDLE, 0, 0, 0, 0, 0, 0, 0);
    // 5 A flick: 80 px left in 16 ms, released → Swipe left at the last point, with the last delta
    one(0.5f, 0.5f); rd(4 * S); newest("swipe: Touch", 17, 9, TOUCH, 640, 360, 0, 0, NEW, 0, 1);
    one(0.4375f, 0.5f); rd(4 * S + 16 * MS); newest("swipe: Pan (560,360) delta (-80,0)", 18, 9, PAN, 560, 360, -80, 0, 0, 0, 1);
    up(); rd(4 * S + 32 * MS); newest("swipe: Swipe left (560,360) delta (-80,0)", 19, 9, SWIPE, 560, 360, -80, 0, 0, 1, 1);
    check("swipe: velocity -2500 px/s", g[0].vx < -2499.0f && g[0].vx > -2501.0f);
    rd(4 * S + 48 * MS); rd(4 * S + 64 * MS); newest("swipe: Complete, then Idle", 21, 10, IDLE, 0, 0, 0, 0, 0, 0, 0);
    check("16 entries kept, newest first", gn == 16 && g[15].samp == 6);
    // 6 Held still for 0.6 s: Press (no tap), released → Complete
    one(0.5f, 0.5f); rd(5 * S); newest("press: Touch", 22, 11, TOUCH, 640, 360, 0, 0, NEW, 0, 1);
    rd(5 * S + 400 * MS); check("press: not yet at 0.4 s", g[0].samp == 22);
    rd(5 * S + 600 * MS); newest("press: Press after 0.5 s", 23, 11, PRESS, 640, 360, 0, 0, 0, 0, 1);
    up(); rd(5 * S + 700 * MS); newest("press: released → Complete, no Tap", 24, 11, COMPLETE, 0, 0, 0, 0, 0, 0, 0);
    rd(5 * S + 716 * MS); newest("press: Idle", 25, 12, IDLE, 0, 0, 0, 0, 0, 0, 0);
    // 7 A second finger joins: Cancel, then a new Touch of two fingers without IsNewTouch, and lifting them cancels instead of tapping
    one(0.25f, 0.5f); rd(6 * S); newest("count change: Touch of one finger (320,360)", 26, 13, TOUCH, 320, 360, 0, 0, NEW, 0, 1);
    dm_touch two[2] = {{0, 0.25f, 0.5f}, {1, 0.75f, 0.5f}}; dm_touch_set(two, 2);
    rd(6 * S + 16 * MS); newest("count change: Cancel", 27, 13, CANCEL, 0, 0, 0, 0, 0, 0, 0);
    rd(6 * S + 33 * MS); newest("count change: Touch of two fingers, mid point (640,360), no IsNewTouch after Cancel", 28, 14, TOUCH, 640, 360, 0, 0, 0, 0, 2);
    check("count change: points in finger id order", g[0].pt[0][0] == 320 && g[0].pt[1][0] == 960);
    up(); rd(6 * S + 50 * MS); newest("count change: released → Cancel, no Tap", 29, 14, CANCEL, 0, 0, 0, 0, 0, 0, 0);
    rd(6 * S + 66 * MS); newest("count change: Idle", 30, 15, IDLE, 0, 0, 0, 0, 0, 0, 0);
    // 8 Two fingers spread from 320 px apart to 640: Pinch, scale 2
    dm_touch near[2] = {{0, 0.375f, 0.5f}, {1, 0.625f, 0.5f}}; dm_touch_set(near, 2);
    rd(7 * S); newest("pinch: Touch of two fingers", 31, 16, TOUCH, 640, 360, 0, 0, NEW, 0, 2);
    dm_touch_set(two, 2); rd(7 * S + 16 * MS); newest("pinch: Pinch (640,360)", 32, 16, PINCH, 640, 360, 0, 0, 0, 0, 2);
    check("pinch: scale 2", g[0].scale > 1.999f && g[0].scale < 2.001f);
    up(); rd(7 * S + 33 * MS); newest("pinch: released → Complete", 33, 16, COMPLETE, 0, 0, 0, 0, 0, 0, 0);
    // 9 A press and lift between two reads still makes a Touch, then a Tap
    rd(7 * S + 50 * MS); one(0.5f, 0.5f); up();
    rd(8 * S); newest("between reads: Touch", 35, 18, TOUCH, 640, 360, 0, 0, NEW, 0, 1);
    rd(8 * S + 16 * MS); newest("between reads: Tap", 36, 18, TAP, 640, 360, 0, 0, 0, 0, 1);
    printf("== %d checks, %d failed ==\n", cases, fails);
    return fails != 0;
}
