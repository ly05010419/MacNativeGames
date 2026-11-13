/* TOTK: the shared compute encoder's ordering policy (plan P0, 2026-11-11).

   Today `disp_enc()` (nvn_mtl.m) inserts a full-stage barrier between *every* pair of consecutive
   compute dispatches that share the encoder, and the encoder uses Metal's default serial dispatch
   type. Fourteen low-occupancy kernels therefore run back to back: on the title screen their GPU
   time sums to ~23 ms even though only a few threadgroups are in flight at a time.

   This header holds the decision as a pure function so it can be exercised on the CPU, without a
   GPU, game files, or the shader seed (tests/test_dispatch_chain.c). nvn_mtl.m keeps today's
   behaviour until the decision below is wired in, and falls back to a barrier whenever a proof or
   an input is not available: the policy may only ever remove a barrier it can justify.

   The inputs are the ones the replay thread already knows at the dispatch boundary:
     - game_barrier: the game's own nvnCommandBufferBarrier sits between the two dispatches
       (nvn_mtl.m counts CommandBufferBarrier calls); ordering and visibility stay its job.
     - indirect / prev_indirect: either side is an indirect dispatch, so the grid is not a constant
       of the shader and no static proof can describe it.
     - concurrent_ok: the device/encoder supports MTLDispatchTypeConcurrent (macOS 15+ Metal 4
       compute encoder) and the port's encoder path is in its shared-encoder mode.
     - vpool_aliases: the port reports virtual-pool aliasing in this run (totk_vpool_aliases_used),
       so a storage-buffer write may be visible through another resource; the per-type range proof
       is not sound then.
     - alias: the byte-range check's answer for the previous dispatch's writes against this
       dispatch's reads and writes: NONE only when both sets were proved disjoint. */
#ifndef NVN_DISPATCH_CHAIN_H
#define NVN_DISPATCH_CHAIN_H

#include <stdint.h>

/* The byte-range check's answer (a conservative three-valued result). */
typedef enum {
    NVN_CHAIN_ALIAS_NONE = 0,     /* the two dispatches' byte sets are proved disjoint */
    NVN_CHAIN_ALIAS_MAYBE = 1,    /* at least one range may overlap */
    NVN_CHAIN_ALIAS_UNKNOWN = 2   /* no complete range information for one side */
} nvn_chain_alias;

typedef struct {
    int game_barrier;    /* the game's barrier sits between the dispatches */
    int indirect;        /* this dispatch reads its grid from memory */
    int prev_indirect;   /* the previous dispatch read its grid from memory */
    int concurrent_ok;   /* the encoder path can use MTLDispatchTypeConcurrent */
    int vpool_aliases;   /* the run reports virtual-pool aliasing */
    nvn_chain_alias alias;
} nvn_chain_input;

typedef enum {
    NVN_CHAIN_BARRIER = 0,     /* keep barrierAfterEncoderStages: today's behaviour */
    NVN_CHAIN_CONCURRENT = 1   /* no barrier; the two dispatches may overlap */
} nvn_chain_decision;

static inline nvn_chain_decision nvn_chain_decide(const nvn_chain_input *in) {
    if (!in) return NVN_CHAIN_BARRIER;
    if (in->game_barrier) return NVN_CHAIN_BARRIER;   /* the game asked for the ordering */
    if (!in->concurrent_ok) return NVN_CHAIN_BARRIER; /* no concurrent encoding here */
    if (in->indirect || in->prev_indirect) return NVN_CHAIN_BARRIER;   /* grid unknown */
    if (in->vpool_aliases) return NVN_CHAIN_BARRIER;  /* resources may alias across types */
    if (in->alias != NVN_CHAIN_ALIAS_NONE) return NVN_CHAIN_BARRIER;  /* no disjointness proof */
    return NVN_CHAIN_CONCURRENT;
}

#endif

/* The byte-range part of the same decision: the policy above only overlaps two dispatches when the
   previous one's writes and this one's reads/writes were proved disjoint. A terminator such as the
   pool bookkeeping kernels has no complete range set (it writes through addresses the translator
   could not map to a binding, see the raw-write notes), so `complete` is 0 for it and the answer is
   UNKNOWN, never NONE. Ranges are half-open [lo, hi) and need not be sorted. */
typedef struct {
    uint64_t lo, hi;
} nvn_chain_range;

typedef struct {
    const nvn_chain_range *prev_writes;
    const nvn_chain_range *prev_reads;
    const nvn_chain_range *cur_writes;
    const nvn_chain_range *cur_reads;
    unsigned n_prev_writes, n_prev_reads, n_cur_writes, n_cur_reads;
    int complete;   /* every one of the four sets is complete; 0 = UNKNOWN */
} nvn_chain_ranges;

/* 1 when any half-open range of the first set intersects any range of the second set. Invalid
   ranges (lo >= hi) are treated as covering nothing, so a caller that cannot validate a set must
   clear `complete` instead of relying on this. */
static inline int nvn_chain_ranges_intersect(const nvn_chain_range *as, unsigned na,
                                             const nvn_chain_range *bs, unsigned nb) {
    for (unsigned i = 0; i < na; i++) {
        if (as[i].lo >= as[i].hi) continue;
        for (unsigned j = 0; j < nb; j++) {
            if (bs[j].lo >= bs[j].hi) continue;
            if (as[i].lo < bs[j].hi && bs[j].lo < as[i].hi) return 1;
        }
    }
    return 0;
}

static inline nvn_chain_alias nvn_chain_alias_of(const nvn_chain_ranges *r) {
    if (!r || !r->complete) return NVN_CHAIN_ALIAS_UNKNOWN;
    /* Write-after-read, read-after-write and write-after-write are all hazards when the two
       dispatches are allowed to overlap. */
    if (nvn_chain_ranges_intersect(r->prev_writes, r->n_prev_writes, r->cur_reads, r->n_cur_reads) ||
        nvn_chain_ranges_intersect(r->prev_reads, r->n_prev_reads, r->cur_writes, r->n_cur_writes) ||
        nvn_chain_ranges_intersect(r->prev_writes, r->n_prev_writes, r->cur_writes, r->n_cur_writes))
        return NVN_CHAIN_ALIAS_MAYBE;
    return NVN_CHAIN_ALIAS_NONE;
}
