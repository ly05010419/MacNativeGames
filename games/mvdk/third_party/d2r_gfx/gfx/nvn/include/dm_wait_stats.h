/* Launch-time diagnostic switches, shared by runtime and graphics without a link dependency.
   Disabled paths add no clocks, counters or scheduler locks. */
#pragma once
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

static inline int dm_diag_value(const char *value) { return value && *value && strcmp(value, "0"); }
static inline int dm_core_diag_enabled(void) {
    static _Atomic int enabled = -1;
    int value = atomic_load_explicit(&enabled, memory_order_relaxed);
    if (value < 0) {
        value = dm_diag_value(getenv("TOTK_CORE_DIAG"));
        atomic_store_explicit(&enabled, value, memory_order_relaxed);
    }
    return value;
}
static inline int dm_wait_stats_enabled(void) {
    static _Atomic int enabled = -1;
    int value = atomic_load_explicit(&enabled, memory_order_relaxed);
    if (value < 0) {
        const char *tsv = getenv("DM_NVN_TSV");
        value = (tsv && *tsv) || dm_core_diag_enabled();
        atomic_store_explicit(&enabled, value, memory_order_relaxed);
    }
    return value;
}
