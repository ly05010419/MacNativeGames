// The present pacer (shared/third_party/d2r_gfx/gfx/nvn/nvn_pace.h) as hle_nvn.c drives it: the acquire waits for the slot
// time of the texture (avail = slot + period) unless the display link paced the frame, and present either steps the beat
// (nvn_pace) or restarts it at the present (nvn_pace_follow). Built and run by tests/test_pace.py.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "nvn_pace.h"

#define MS 1000000ull
static const uint64_t PERIOD = 16666667ull;   // 60 fps

typedef struct { nvn_pacer pc; uint64_t avail[3]; unsigned next; } window;

/* One present at `now`; follow: the display link paced the frame. Returns the slot. */
static uint64_t present(window *w, unsigned idx, uint64_t now, int follow) {
    uint64_t slot = now;
    if (follow) nvn_pace_follow(&w->pc, now); else slot += nvn_pace(&w->pc, now, PERIOD);
    w->avail[idx] = slot + PERIOD;
    return slot;
}

/* The acquire's fallback wait for texture idx at `now` (what sleep_until_ns would sleep). */
static uint64_t fallback_wait(const window *w, unsigned idx, uint64_t now) { return w->avail[idx] > now ? w->avail[idx] - now : 0; }

/* 18,000 frames the display link paced 12.5 ms apart (an 80-120 Hz variable-refresh link faster than the 60 fps period),
   then the link goes silent: the first fallback wait. */
static uint64_t silent_link_after_fast_link(int follow) {
    window w = {0}; uint64_t now = 1000 * MS;
    for (unsigned i = 0; i < 18000; i++) { present(&w, i % 3, now, follow); now += 12500000ull; }
    return fallback_wait(&w, 18000 % 3, now);
}

int main(void) {
    /* A caller that waits for its slot keeps the vsync semantics: 60.00 fps with three textures, 1 ms of work a frame. */
    window w = {0}; uint64_t now = 1000 * MS, first = 0, last = 0;
    for (unsigned i = 0; i < 600; i++) {
        unsigned idx = i % 3; uint64_t wait = fallback_wait(&w, idx, now); now += wait;
        uint64_t slot = present(&w, idx, now, 0); if (!i) first = slot; last = slot;
        now += 1 * MS;
    }
    double fps = 599.0 / ((last - first) / 1e9);
    printf("waiting caller: %.2f fps\n", fps);
    assert(fps > 59.9 && fps < 60.1);

    uint64_t stepped = silent_link_after_fast_link(0), followed = silent_link_after_fast_link(1);
    printf("silent link after 18,000 link-paced frames: stepping waits %.1f ms, following waits %.1f ms\n", stepped / 1e6, followed / 1e6);
    assert(stepped > 60000 * MS);            // the old stepping: a lead of over a minute, slept in one go
    assert(followed <= PERIOD);              // following: at most one period from the last present

    /* After the link is silent, the frames the slot time paces settle at 60 fps from the first one. */
    window v = {0}; now = 1000 * MS;
    for (unsigned i = 0; i < 100; i++) { present(&v, i % 3, now, 1); now += 12500000ull; }
    uint64_t worst = 0, prev = 0;
    for (unsigned i = 100; i < 400; i++) {
        unsigned idx = i % 3; uint64_t wait = fallback_wait(&v, idx, now); if (wait > worst) worst = wait; now += wait;
        uint64_t slot = present(&v, idx, now, 0); if (prev) assert(slot - prev <= PERIOD + 1 * MS); prev = slot; now += 1 * MS;
    }
    printf("silent link: worst wait %.1f ms\n", worst / 1e6);
    assert(worst <= PERIOD);
    puts("pacer passed");
    return 0;
}
