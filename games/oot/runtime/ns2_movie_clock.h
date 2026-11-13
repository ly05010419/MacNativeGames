/* nn::codec picture timestamps are nanoseconds. Start at the first picture the
   client accepts, not while its movie is still being opened/prebuffered. */
#pragma once
#include <limits.h>
#include <stdint.h>

typedef struct {
    uint64_t start_ns;
    int64_t start_pts;
    int started;
} Ns2MovieClock;

static inline void ns2_movie_clock_start(Ns2MovieClock *clock, uint64_t now, int64_t pts) {
    if (!clock->started) *clock = (Ns2MovieClock){now, pts, 1};
}

static inline int64_t ns2_movie_clock_target(const Ns2MovieClock *clock, uint64_t now) {
    uint64_t elapsed = now >= clock->start_ns ? now - clock->start_ns : 0;
    if (elapsed > INT64_MAX || clock->start_pts > INT64_MAX - (int64_t)elapsed) return INT64_MAX;
    return clock->start_pts + (int64_t)elapsed;
}
