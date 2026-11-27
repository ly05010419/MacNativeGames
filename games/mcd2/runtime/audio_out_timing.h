/* AudioOut producer/callback timing, diagnostic only. All accesses under AudioOut.lock.
   Empty intervals describe callback queue starvation, not measured speaker underruns:
   Core Audio can retain device latency after returning the last client buffer. */
#ifndef MCD2_AUDIO_OUT_TIMING_H
#define MCD2_AUDIO_OUT_TIMING_H
#include <stdint.h>
typedef struct {
    uint64_t last_append, last_callback, empty_since;
    uint64_t append_gap_max, callback_gap_max, empty_ns, empty_max;
    uint64_t appends, callbacks, drains, refills;
} AudioOutTiming;
static inline void audio_out_timing_pause(AudioOutTiming *t) {
    t->last_append = t->last_callback = t->empty_since = 0;
}
static inline void audio_out_timing_callback(AudioOutTiming *t, uint64_t now, int started, unsigned remaining) {
    if (!started) return;
    if (t->last_callback && now >= t->last_callback && now - t->last_callback > t->callback_gap_max)
        t->callback_gap_max = now - t->last_callback;
    t->last_callback = now; t->callbacks++;
    if (!remaining && !t->empty_since) { t->empty_since = now; t->drains++; }
}
static inline void audio_out_timing_append(AudioOutTiming *t, uint64_t now, int started) {
    if (!started) return;
    if (t->last_append && now >= t->last_append && now - t->last_append > t->append_gap_max)
        t->append_gap_max = now - t->last_append;
    t->last_append = now; t->appends++;
    if (t->empty_since) {
        uint64_t gap = now >= t->empty_since ? now - t->empty_since : 0;
        t->empty_ns += gap;
        if (gap > t->empty_max) t->empty_max = gap;
        t->refills++; t->empty_since = 0;
    }
}
#endif
