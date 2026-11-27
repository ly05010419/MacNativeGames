#ifndef NVN_DIAG_RANGE_H
#define NVN_DIAG_RANGE_H
#include <stdatomic.h>
#include <stdio.h>
#include <sched.h>

typedef struct { long lo, hi; } nvn_diag_range_value;
typedef struct { _Atomic int ready; nvn_diag_range_value value; } nvn_diag_range_cache;

static inline nvn_diag_range_value nvn_diag_range_parse(const char *text) {
    nvn_diag_range_value v = {-2, -2};
    if (!text || sscanf(text, "%ld-%ld", &v.lo, &v.hi) != 2) v.lo = v.hi = -2;
    return v;
}

/* The immutable environment range is published once to all recording threads.
   Legacy mode intentionally repeats the disabled-range lookup for in-process A/B.
   The old lo < 0 initialization test also matched its initialized -2 sentinel. */
static inline nvn_diag_range_value nvn_diag_range_get(nvn_diag_range_cache *c,
        int cache_disabled, const char *(*read_env)(void)) {
    if (atomic_load_explicit(&c->ready, memory_order_acquire) != 2) {
        int initial = 0;
        if (atomic_compare_exchange_strong_explicit(&c->ready, &initial, 1,
                memory_order_acq_rel, memory_order_acquire)) {
            c->value = nvn_diag_range_parse(read_env());
            atomic_store_explicit(&c->ready, 2, memory_order_release);
        } else {
            while (atomic_load_explicit(&c->ready, memory_order_acquire) != 2) sched_yield();
        }
    }
    return !cache_disabled && c->value.lo < 0 ? nvn_diag_range_parse(read_env()) : c->value;
}
#endif
