#pragma once
#include <stdint.h>

// Metal pixel formats, also used by the Foundation-only serialized-key tests.
enum { PSO_D16 = 250, PSO_D32 = 252, PSO_S8 = 253, PSO_D32S8 = 260, PSO_X32S8 = 261 };

// iOS rejects a combined stencil format with no matching depth format (#702).
// The pass attaches both planes of combined storage, even when lookahead uses
// only one. Match that in live, predicted and historical prewarm keys. Never
// replace a nonzero, conflicting format or invent storage for an empty pass.
static inline void pso_ds_pair(uint32_t *depth, uint32_t *stencil) {
    if (!*depth && *stencil == PSO_D32S8) *depth = PSO_D32S8;
    if (!*stencil && *depth == PSO_D32S8) *stencil = PSO_D32S8;
}

static inline int pso_ds_valid(uint32_t depth, uint32_t stencil) {
    if (depth && depth != PSO_D16 && depth != PSO_D32 && depth != PSO_D32S8) return 0;
    if (stencil && stencil != PSO_S8 && stencil != PSO_D32S8 && stencil != PSO_X32S8) return 0;
    if (depth == PSO_D32S8 || stencil == PSO_D32S8) return depth == stencil;
    return 1;
}
