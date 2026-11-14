/* Frame-rate recording (TOTK_FPS_RECORD=<file.csv>, from the environment: a front end that starts the app sets it): one CSV row
   per second of play with the game frames shown that second, their average time, the 1% low (the frame rate of the
   slowest 1 % of frame times) and the slowest frame. Fed from fps_overlay_note (game presents, ticket = frames so far:
   a step of 2 is two frames the display link merged). A gap longer than FRAME_RECORD_GAP_S (the window hidden, the game
   paused or loading on one frame) ends the second before it without counting the gap; the next window starts after it.
   Pure C: fps_overlay.c writes the rows, tests/test_frame_record.c covers it. */
#ifndef TOTK_FRAME_RECORD_H
#define TOTK_FRAME_RECORD_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { FRAME_RECORD_MAX_FRAMES = 4096 };
#define FRAME_RECORD_GAP_S 2.0
#define FRAME_RECORD_HEADER "time_s,fps,avg_ms,low1_fps,max_ms\n"

typedef struct { double t, fps, avg_ms, low1_fps, max_ms; } frame_record_row;
typedef struct {
    int started;
    double t0, win_start, last_t;
    uint64_t last_ticket, frames;
    int n;
    float interval[FRAME_RECORD_MAX_FRAMES];   /* seconds per frame in this window */
} frame_record;

static inline void frame_record_init(frame_record *r) { memset(r, 0, sizeof *r); }

static inline int frame_record_cmp_desc(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;
    return (x < y) - (x > y);
}

/* Closes the window [win_start, end) into *row; 0 if it holds no frame. */
static inline int frame_record_close(frame_record *r, double end, frame_record_row *row) {
    double span = end - r->win_start;
    int ok = r->frames > 0 && span > 0 && r->n > 0;
    if (ok) {
        qsort(r->interval, (size_t)r->n, sizeof r->interval[0], frame_record_cmp_desc);
        double p99 = r->interval[(int)(r->n * 0.01)];
        *row = (frame_record_row){.t = r->win_start - r->t0, .fps = (double)r->frames / span,
                                  .avg_ms = 1000.0 * span / (double)r->frames,
                                  .low1_fps = p99 > 0 ? 1.0 / p99 : 0, .max_ms = 1000.0 * r->interval[0]};
    }
    r->frames = 0; r->n = 0;
    return ok;
}

/* One game present. Returns 1 when a second closed, with its row in *row. */
static inline int frame_record_note(frame_record *r, uint64_t ticket, double now_s, frame_record_row *row) {
    if (!r->started) {
        r->started = 1; r->t0 = r->win_start = r->last_t = now_s; r->last_ticket = ticket;
        return 0;
    }
    double dt = now_s - r->last_t;
    uint64_t step = ticket > r->last_ticket ? ticket - r->last_ticket : 0;
    int closed = 0;
    if (dt > FRAME_RECORD_GAP_S) {
        closed = frame_record_close(r, r->last_t, row);
        r->win_start = now_s;
    } else if (step) {
        r->frames += step;
        if (r->n < FRAME_RECORD_MAX_FRAMES) r->interval[r->n++] = (float)(dt / (double)step);
        if (now_s - r->win_start >= 1.0 - 1e-9) {
            closed = frame_record_close(r, now_s, row);
            r->win_start = now_s;
        }
    }
    r->last_t = now_s; r->last_ticket = ticket;
    return closed;
}

static inline int frame_record_format(const frame_record_row *row, char *out, size_t n) {
    return snprintf(out, n, "%.1f,%.1f,%.2f,%.1f,%.2f\n", row->t, row->fps, row->avg_ms, row->low1_fps, row->max_ms);
}
#endif
