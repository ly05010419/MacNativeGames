// Present pacer (pure function, shared by Mac / iOS; unit test tests/t_pace.c).
// Semantics as vsync: present slot = previous present slot + period. Early → wait until the present slot; late by less than one period → present immediately and keep the original phase;
// behind by one period or more (stutter) → restart the cadence from the current time, no burst of presents to catch up. Period 0 = no wait (the present slot follows the current time).
#pragma once
#include <stdint.h>

typedef struct { uint64_t target; } nvn_pacer;

// now: monotonic clock (ns); period: this frame's period (ns, 0 = no wait). Returns the ns to wait (0 = present immediately) and updates the internal target time.
static inline uint64_t nvn_pace(nvn_pacer *p, uint64_t now, uint64_t period) {
    if (!period || !p->target) { p->target = now; return 0; }
    uint64_t next = p->target + period;
    if (now <= next) { p->target = next; return next - now; }
    p->target = now - next < period ? next : now;
    return 0;
}
