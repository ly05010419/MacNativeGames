/* Prediction work is paid for by a fixed allowance or actual synchronous pipeline builds.
   Scans and waits never earn more allowance. State belongs to the submit thread. */
#pragma once
#include <stdint.h>

typedef struct { uint64_t frame, used_ns, sync_ns; } nvn_scan_budget;
static inline void nvn_scan_budget_frame(nvn_scan_budget *b, uint64_t frame) {
    if (b->frame != frame) { b->frame = frame; b->used_ns = b->sync_ns = 0; }
}
static inline int nvn_scan_budget_left(nvn_scan_budget *b, uint64_t frame, uint64_t fixed_ns, int adaptive) {
    nvn_scan_budget_frame(b, frame);
    return b->used_ns < fixed_ns || (adaptive && b->used_ns < b->sync_ns);
}
static inline void nvn_scan_budget_build(nvn_scan_budget *b, uint64_t frame, uint64_t elapsed_ns) {
    nvn_scan_budget_frame(b, frame);
    b->sync_ns = UINT64_MAX - b->sync_ns < elapsed_ns ? UINT64_MAX : b->sync_ns + elapsed_ns;
}
