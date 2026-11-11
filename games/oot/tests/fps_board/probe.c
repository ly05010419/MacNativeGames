/* Unit test for third_party/d2r_gfx/gfx/nvn/fps_board.c, the leaderboard's frame-rate record (user 2026-11-11: average,
   median, 1% low, highest, interpolated frames). Feeds made-up present times and checks each number.
   Prints one JSON line; exit 1 on failure. */
#include "fps_board.h"
#include <math.h>
#include <stdio.h>

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); } } while (0)
#define NEAR(a, b, tol) (fabs((a) - (b)) <= (tol))

/* n presents, one every dt seconds, from t; returns the time after the last. */
static double feed(uint64_t *ticket, double t, int n, double dt) {
    for (int i = 0; i < n; i++) { t += dt; fps_board_note(++*ticket, t); }
    return t;
}

static void steady_60(void) {
    fps_board_reset(); fps_board_start();
    uint64_t k = 0; double t = 100.0; fps_board_note(++k, t);
    feed(&k, t, 600, 1.0 / 60);
    fps_board_stats s; int got = fps_board_snapshot(&s);
    CHECK(got, "steady 60: a snapshot");
    CHECK(s.frames == 600, "steady 60: 600 frames, got %llu", (unsigned long long)s.frames);
    CHECK(NEAR(s.seconds, 10.0, 0.01), "steady 60: 10 s, got %.3f", s.seconds);
    CHECK(NEAR(s.avg, 60, 0.1), "steady 60: average 60, got %.2f", s.avg);
    CHECK(NEAR(s.median, 60, 0.5), "steady 60: median 60, got %.2f", s.median);
    CHECK(NEAR(s.low1, 60, 0.5), "steady 60: 1%% low 60, got %.2f", s.low1);
    CHECK(NEAR(s.max, 60, 0.5), "steady 60: highest 60, got %.2f", s.max);
    CHECK(s.interp_fps == 0, "steady 60: no interpolated frames, got %.2f", s.interp_fps);
}

/* 99 frames of 10 ms, then one of 100 ms, ten times: the slowest 1% are the 100 ms frames. */
static void one_slow_in_a_hundred(void) {
    fps_board_reset(); fps_board_start();
    uint64_t k = 0; double t = 5.0; fps_board_note(++k, t);
    for (int r = 0; r < 10; r++) { t = feed(&k, t, 99, 0.010); t = feed(&k, t, 1, 0.100); }
    fps_board_stats s; fps_board_snapshot(&s);
    CHECK(s.frames == 1000, "1-in-100: 1000 frames, got %llu", (unsigned long long)s.frames);
    CHECK(NEAR(s.seconds, 10.9, 0.01), "1-in-100: 10.9 s, got %.3f", s.seconds);
    CHECK(NEAR(s.avg, 1000 / 10.9, 0.1), "1-in-100: average %.2f, got %.2f", 1000 / 10.9, s.avg);
    CHECK(NEAR(s.median, 100, 1), "1-in-100: median 100, got %.2f", s.median);
    CHECK(NEAR(s.low1, 10, 0.1), "1-in-100: 1%% low 10, got %.2f", s.low1);
    CHECK(s.low1 <= s.avg && s.low1 <= s.median && s.median <= s.max && s.avg <= s.max,
          "1-in-100: low1 %.2f <= avg %.2f, median %.2f <= max %.2f", s.low1, s.avg, s.median, s.max);
}

/* Frames before the world (fps_board_start) and pauses longer than half a second are not counted. */
static void before_world_and_pauses(void) {
    fps_board_reset();
    uint64_t k = 0; double t = 1.0; fps_board_note(++k, t);
    t = feed(&k, t, 300, 1.0 / 30);
    fps_board_stats s;
    CHECK(!fps_board_snapshot(&s), "before the world: nothing counted");
    fps_board_start();
    t = feed(&k, t, 120, 1.0 / 60);   // 2 s (the first interval after start crosses no gap: counted)
    t += 5.0; fps_board_note(++k, t); // a 5 s pause: not counted
    t = feed(&k, t, 120, 1.0 / 60);   // 2 s more
    fps_board_snapshot(&s);
    CHECK(s.frames == 240, "pauses: 240 frames, got %llu", (unsigned long long)s.frames);
    CHECK(NEAR(s.seconds, 4.0, 0.01), "pauses: 4 s, got %.3f", s.seconds);
    CHECK(NEAR(s.avg, 60, 0.1), "pauses: average 60, got %.2f", s.avg);
}

/* A ticket that moves by two counts two frames over the interval. */
static void ticket_jump(void) {
    fps_board_reset(); fps_board_start();
    uint64_t k = 0; double t = 0; fps_board_note(++k, t);
    for (int i = 0; i < 300; i++) { t += 2.0 / 60; k += 2; fps_board_note(k, t); }
    fps_board_stats s; fps_board_snapshot(&s);
    CHECK(s.frames == 600, "ticket jump: 600 frames, got %llu", (unsigned long long)s.frames);
    CHECK(NEAR(s.avg, 60, 0.1) && NEAR(s.median, 60, 0.5), "ticket jump: 60 fps, got avg %.2f median %.2f", s.avg, s.median);
}

/* Alternating 10 ms / 30 ms: the median frame time can sit on either side; the numbers still keep their order. */
static void uneven_pacing(void) {
    fps_board_reset(); fps_board_start();
    uint64_t k = 0; double t = 0; fps_board_note(++k, t);
    for (int i = 0; i < 300; i++) { t = feed(&k, t, 1, 0.010); t = feed(&k, t, 1, 0.030); }
    fps_board_stats s; fps_board_snapshot(&s);
    CHECK(NEAR(s.avg, 50, 0.1), "uneven: average 50, got %.2f", s.avg);
    CHECK(NEAR(s.low1, 1 / 0.030, 0.3), "uneven: 1%% low 33.3, got %.2f", s.low1);
    CHECK(s.low1 <= s.avg && s.low1 <= s.median && s.median <= s.max && s.avg <= s.max,
          "uneven: low1 %.2f <= avg %.2f, median %.2f <= max %.2f", s.low1, s.avg, s.median, s.max);
}

/* The highest is the best whole second: 2 s at 30, then 2 s at 90. */
static void highest_second(void) {
    fps_board_reset(); fps_board_start();
    uint64_t k = 0; double t = 0; fps_board_note(++k, t);
    t = feed(&k, t, 60, 1.0 / 30);
    t = feed(&k, t, 180, 1.0 / 90);
    fps_board_stats s; fps_board_snapshot(&s);
    CHECK(NEAR(s.max, 90, 1), "highest: 90, got %.2f", s.max);
    CHECK(NEAR(s.avg, 60, 0.1), "highest: average 60, got %.2f", s.avg);
}

/* Interpolated frames are a rate of their own over the counted time, never in the frame count. */
static void interpolated(void) {
    fps_board_reset(); fps_board_start();
    uint64_t k = 0; double t = 0; fps_board_note(++k, t);
    for (int i = 0; i < 300; i++) { t += 1.0 / 30; fps_board_note(++k, t); fps_board_note_gen(t + 1.0 / 60); }
    fps_board_stats s; fps_board_snapshot(&s);
    CHECK(s.frames == 300, "interpolated: 300 game frames, got %llu", (unsigned long long)s.frames);
    CHECK(NEAR(s.avg, 30, 0.1), "interpolated: average 30, got %.2f", s.avg);
    CHECK(NEAR(s.interp_fps, 30, 0.2), "interpolated: 30 interpolated a second, got %.2f", s.interp_fps);
}

/* Negative control: a reset forgets everything. */
static void reset_forgets(void) {
    fps_board_reset(); fps_board_start();
    uint64_t k = 0; double t = 0; fps_board_note(++k, t); feed(&k, t, 60, 1.0 / 60);
    fps_board_reset();
    fps_board_stats s;
    CHECK(!fps_board_snapshot(&s), "reset: nothing left");
    CHECK(!fps_board_counting(), "reset: not counting until the next start");
}

int main(void) {
    steady_60(); one_slow_in_a_hundred(); before_world_and_pauses(); ticket_jump(); uneven_pacing(); highest_second();
    interpolated(); reset_forgets();
    printf("{\"probe\":\"fps_board\",\"checks\":%d,\"failures\":%d,\"pass\":%s}\n", checks, failures, failures ? "false" : "true");
    return failures ? 1 : 0;
}
