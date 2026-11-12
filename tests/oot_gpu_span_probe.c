/* Fixture for tests/test_oot_gpu_span.py: the per-frame GPU accounting of games/oot is spliced in from nvn_mtl.m and
   totk_sysload.c as written, and driven with synthetic command-buffer completions. No Metal, no device, no game data.
   The markers below are replaced by production source; everything else is the harness. */
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ---- the production pieces ---- */
typedef struct { double last_end, busy; } sysload_busy;   /* totk_sysload.h */
/* SYSLOAD_ADD */
/* RING */
/* COUNTERS */
static void gpu_watchdog_start(void) {}   /* the watchdog thread itself is not under test */
static void sysload_note_gpu(double start_s, double end_s);
/* TAG_BEGIN */
/* TAG_DONE */
/* READERS */

/* ---- the harness ---- */
static sysload_busy g_busy;
static int g_note_calls;
static double g_note_added;
/* The game's own callback forwards to sysload_busy_add under a mutex (totk_sysload.c); here it is single-threaded. */
static void sysload_note_gpu(double start_s, double end_s) { g_note_calls++; g_note_added += sysload_busy_add(&g_busy, start_s, end_s); }

static int failures;
static void expect(int ok, const char *what) { if (!ok) { fprintf(stderr, "FAIL: %s\n", what); failures++; } }
static void expect_eq(double got, double want, const char *what) {
    double d = got - want; if (d < 0) d = -d;
    if (!(d <= 1e-6)) { fprintf(stderr, "FAIL: %s (got %.6f, want %.6f)\n", what, got, want); failures++; }
}

/* One command buffer of frame f, committed and then completed with this GPU pair. */
static void buffer(uint64_t f, double g0, double g1) { gpu_tag_begin(f); gpu_tag_done(f, g0, g1); }

/* A valid pair is counted: 1 ms of GPU time, and the span is the pair itself. */
static void valid_interval_counted(void) {
    buffer(1, 105.000, 105.001);
    uint64_t t0 = 0, t1 = 0;
    expect_eq(mtl_gpu_ms(1), 1.0, "a valid pair gives 1 ms");
    expect(mtl_gpu_span(1, &t0, &t1) == 1, "a valid pair reports a span");
    expect_eq((double)t0, 105.000 * 1e9, "the span starts at GPUStartTime");
    expect_eq((double)t1, 105.001 * 1e9, "the span ends at GPUEndTime");
}

/* A zero start with a positive end is dropped, and the frame still finishes: pending reaches zero, so the reader
   answers instead of staying at -1, and the watchdog still sees a completion. This is the pair captured on 2026-11-12
   (g0 = 0.000000000, g1 = 588279.960720750); the old rule charged the frame the whole clock. */
static void zero_start_dropped(void) {
    uint64_t before = atomic_load(&g_cbw_completed);
    buffer(2, 0.0, 588279.960720750);
    uint64_t t0 = 0, t1 = 0;
    expect_eq(mtl_gpu_ms(2), 0.0, "a zero start adds no GPU time");
    expect(mtl_gpu_ms(2) >= 0, "the frame still finishes (pending reached zero)");
    expect(mtl_gpu_span(2, &t0, &t1) == 0, "a zero start leaves no span");
    expect(atomic_load(&g_cbw_completed) == before + 1, "the watchdog still counts the completion");
    expect(atomic_load(&g_cbw_last_done_ns) != 0, "the watchdog still marks the time of the completion");
}

/* The realistic frame: several command buffers, one of them with no start time. The good ones are counted whole. */
static void zero_start_beside_good_ones(void) {
    gpu_tag_begin(3); gpu_tag_begin(3); gpu_tag_begin(3);
    gpu_tag_done(3, 200.000, 200.002);
    gpu_tag_done(3, 0.0, 588300.0);
    gpu_tag_done(3, 200.004, 200.005);
    uint64_t t0 = 0, t1 = 0;
    expect_eq(mtl_gpu_ms(3), 3.0, "only the two usable buffers are counted");
    expect(mtl_gpu_span(3, &t0, &t1) == 1, "the frame reports a span");
    expect_eq((double)t0, 200.000 * 1e9, "the span starts at the earliest usable start");
    expect_eq((double)t1, 200.005 * 1e9, "the span ends at the latest usable end");
}

/* Unchanged: an empty interval was already dropped, and the frame still finishes. */
static void empty_interval_dropped(void) {
    uint64_t before = atomic_load(&g_cbw_completed);
    buffer(4, 300.0, 300.0);
    expect_eq(mtl_gpu_ms(4), 0.0, "an empty interval adds no GPU time");
    expect(atomic_load(&g_cbw_completed) == before + 1, "the watchdog counts the empty interval too");
}

/* The load line. sysload_busy_add clamps the start to the end of the last interval, so a zero start is only harmless
   when the real work began at or before that end. After a gap it is not: with last_end = 100 s, the real interval
   [105, 106] adds 1 s and the pair (0, 106) would add 6. The guard keeps the bad pair away from the accumulator. */
static void load_line_after_a_gap(void) {
    g_busy.last_end = 100.0; g_busy.busy = 0.0;
    g_note_calls = 0; g_note_added = 0.0;
    buffer(5, 0.0, 106.0);
    expect(g_note_calls == 0, "a zero start never reaches the load line");
    expect_eq(g_busy.busy, 0.0, "a zero start adds no busy time after a gap");
    buffer(6, 105.0, 106.0);
    expect(g_note_calls == 1, "a valid pair reaches the load line");
    expect_eq(g_busy.busy, 1.0, "a valid pair adds its own second, not the gap");
}

/* Unchanged: when the slot already belongs to a later frame, nothing is charged and this frame's count is not touched. */
static void slot_already_reused(void) {
    gpu_tag_begin(7);
    int pending_before = atomic_load(&g_gpu[7 % GRING].pending);
    uint64_t completed_before = atomic_load(&g_cbw_completed);
    gpu_tag_begin(7 + GRING);                 /* the same slot, a later frame: the slot is re-armed */
    gpu_tag_done(7, 400.0, 400.001);          /* the late completion of the frame that left */
    expect(atomic_load(&g_gpu[7 % GRING].pending) == 1, "a late completion does not touch the new frame's count");
    expect(atomic_load(&g_cbw_completed) == completed_before + 1, "the watchdog counts the late completion");
    expect(pending_before == 1, "the frame had one buffer outstanding");
    expect_eq(mtl_gpu_ms(7), -1.0, "the frame that left reports nothing");
}

int main(void) {
    valid_interval_counted();
    zero_start_dropped();
    zero_start_beside_good_ones();
    empty_interval_dropped();
    load_line_after_a_gap();
    slot_already_reused();
    if (failures) { fprintf(stderr, "%d checks failed\n", failures); return 1; }
    printf("GPU span accounting: 6 cases passed\n");
    return 0;
}
