/* Test the frame-rate recorder (games/totk/third_party/d2r_gfx/gfx/nvn/frame_record.h) with synthetic presents. */
#include "../games/totk/third_party/d2r_gfx/gfx/nvn/frame_record.h"
#include <math.h>
#include <stdio.h>

static int failures;
static void expect(int ok, const char *what) { if (!ok) { fprintf(stderr, "FAIL: %s\n", what); failures++; } }
static int near(double a, double b, double tol) { return fabs(a - b) <= tol; }

/* Feeds n presents dt apart starting at *t (ticket += step each), collecting closed rows. */
static int feed(frame_record *r, uint64_t *ticket, double *t, int n, double dt, int step, frame_record_row *rows, int max) {
    int got = 0;
    for (int i = 0; i < n; i++) {
        *ticket += (uint64_t)step; *t += dt;
        frame_record_row row;
        if (frame_record_note(r, *ticket, *t, &row) && got < max) rows[got++] = row;
    }
    return got;
}

static void steady_60(void) {
    frame_record r; frame_record_init(&r);
    uint64_t k = 0; double t = 100.0; frame_record_row rows[8];
    frame_record_row row;
    expect(!frame_record_note(&r, k, t, &row), "the first present starts the recording");
    int n = feed(&r, &k, &t, 180, 1.0 / 60, 1, rows, 8);
    expect(n == 3, "three seconds of 60 fps close three rows");
    expect(near(rows[0].t, 0, 1e-6) && near(rows[1].t, 1, 0.02) && near(rows[2].t, 2, 0.04), "rows start at 0, 1, 2 s");
    expect(near(rows[0].fps, 60, 0.6), "60 fps");
    expect(near(rows[0].avg_ms, 16.67, 0.2), "16.7 ms average");
    expect(near(rows[0].max_ms, 16.67, 0.2) && near(rows[0].low1_fps, 60, 0.6), "no spikes: max and 1% low match");
}

static void one_spike(void) {
    frame_record r; frame_record_init(&r);
    uint64_t k = 0; double t = 0; frame_record_row rows[4], row;
    frame_record_note(&r, k, t, &row);
    int n = feed(&r, &k, &t, 30, 1.0 / 60, 1, rows, 4);
    n += feed(&r, &k, &t, 1, 0.100, 1, rows + n, 4 - n);    /* one 100 ms hitch */
    n += feed(&r, &k, &t, 40, 1.0 / 60, 1, rows + n, 4 - n);
    expect(n == 1, "one row for the first second");
    expect(near(rows[0].max_ms, 100, 0.5), "the hitch is the worst frame");
    expect(rows[0].low1_fps < 15, "the 1% low shows the hitch");
    expect(rows[0].fps > 50 && rows[0].fps < 60, "the hitch lowers the second's average");
}

static void merged_presents_count_frames(void) {
    frame_record r; frame_record_init(&r);
    uint64_t k = 0; double t = 0; frame_record_row rows[2], row;
    frame_record_note(&r, k, t, &row);
    int n = feed(&r, &k, &t, 40, 1.0 / 30, 2, rows, 2);   /* the display link shows every other game frame */
    expect(n == 1 && near(rows[0].fps, 60, 1.0), "ticket steps of 2 count two frames");
}

static void long_gap_restarts_window(void) {
    frame_record r; frame_record_init(&r);
    uint64_t k = 0; double t = 0; frame_record_row rows[4], row;
    frame_record_note(&r, k, t, &row);
    int n = feed(&r, &k, &t, 30, 1.0 / 60, 1, rows, 4);   /* half a second */
    expect(n == 0, "no row before a second");
    n = feed(&r, &k, &t, 1, 10.0, 1, rows, 4);              /* the game was hidden or paused for 10 s */
    expect(n == 1 && rows[0].t == 0 && near(rows[0].fps, 60, 1.0) && near(rows[0].max_ms, 16.67, 0.3),
           "a gap closes the window before it, without the gap");
    n = feed(&r, &k, &t, 61, 1.0 / 60, 1, rows, 4);
    expect(n == 1 && near(rows[0].t, 10.5, 0.05), "the next window starts after the gap");
}

static void format_row(void) {
    frame_record_row row = {.t = 12.0, .fps = 59.94, .avg_ms = 16.683, .low1_fps = 41.2, .max_ms = 24.27};
    char line[96];
    frame_record_format(&row, line, sizeof line);
    expect(!strcmp(line, "12.0,59.9,16.68,41.2,24.27\n"), "CSV row");
    expect(!strcmp(FRAME_RECORD_HEADER, "time_s,fps,avg_ms,low1_fps,max_ms\n"), "CSV header");
}

int main(void) {
    steady_60();
    one_spike();
    merged_presents_count_frames();
    long_gap_restarts_window();
    format_row();
    if (failures) return 1;
    puts("PASS");
    return 0;
}
