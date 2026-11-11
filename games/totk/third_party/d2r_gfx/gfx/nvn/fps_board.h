/* The leaderboard's frame-rate record (user 2026-11-11, the settings panel's 排行榜 page): every game frame presented
   after the world started, kept as a histogram of frame times, so the average, median, 1% low and highest of a whole
   session cost a few kilobytes. Intervals longer than FPS_BOARD_GAP_S (a pause, the window in the background, a load)
   are left out: the board compares play, not menus. Interpolated frames are a rate of their own, never game frames. */
#pragma once
#include <stdint.h>

#define FPS_BOARD_GAP_S 0.5

typedef struct {
    double seconds;     /* counted play time */
    uint64_t frames;    /* game frames in it */
    double avg;         /* frames / seconds */
    double median;      /* 1 / the median frame time */
    double low1;        /* 1 / the mean of the slowest 1% of frame times */
    double max;         /* the best whole second (at least the median and the average) */
    double interp_fps;  /* interpolated frames shown per counted second */
} fps_board_stats;

void fps_board_note(uint64_t ticket, double now_s);   /* a game frame was presented (ticket: the game's present count) */
void fps_board_note_gen(double now_s);                /* an interpolated frame was shown */
void fps_board_start(void);                           /* the world started: count from now on */
int fps_board_counting(void);
void fps_board_reset(void);                           /* forget the session (settings changed); counting stops until the next start */
int fps_board_snapshot(fps_board_stats *out);         /* 1 when frames were counted */
