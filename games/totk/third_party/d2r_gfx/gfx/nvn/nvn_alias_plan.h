/* The range of the region heap made for a render target that no region heap of its format holds (nvn_stor_alias.h).
   Pure arithmetic, no Metal: tests/alias_region_plan_probe.c runs it alone.

   A region heap holds the textures of one pixel format at heap offset = guest address - region base, so textures
   share bytes exactly where their guest ranges overlap. A texture that reaches out of the regions it touches replaces
   them by one heap over their union, and the textures on them move there (nvn_stor_migrate.h).

   Ocarina of Time lays its render targets out again every frame in one stretch of a pool, a few shapes per format at
   offsets that shift with what the frame needs (2026-11-18, the user's recorded route, scratch/runs/2026-11/oot-route:
   trace-1.alias.txt, aliassim/): the stretch a format uses only widens, at either end, as heavier frames come. With
   the exact union every widening, a median of 400 KiB, replaced the whole region and copied every texture on it:
   366 migrations and 192.9 GiB of copies in 55 s of walking, up to 4.2 GiB (2,697 depth textures, most of them layouts
   of earlier frames, over 59 MiB of heap) in one frame and 3-7 frames in a row; those frames took 90-100 ms on the GPU.
   A region cannot simply be made large: a placement heap's unused bytes are real memory on Apple GPUs (512 MiB heap,
   1 MiB written: footprint +553 MB; oot-route/heapprobe). And regions must not overlap: a first version that left big
   regions alone and started another heap at the texture made a heap per frame at the seam (2.9 GB of heaps).

   So a union that replaces regions holding many textures takes room on the side it grows on, a quarter of its size
   within 2-16 MiB, and reaches as far as the big regions of other formats over the same addresses already go (the
   formats share the stretch), never into a neighbouring region of its own format. On the recorded requests: 175.4 ->
   9.1 GiB copied, frames copying 500 MiB or more 83 -> 8, for 81 MiB more heap on average (aliassim/run3.txt). */
#pragma once
#include <stdint.h>

/* A current region: guest range, the textures alive on its heap, its heap's generation. */
typedef struct { uint64_t lo, size, live, generation; } alias_span;
/* One heap over [lo, hi) replaces the `merged` regions the texture touches (0: the texture is alone, its own range). */
typedef struct { int merged; uint64_t lo, hi; } alias_plan;

#define ALIAS_ROOM_TEXTURES 16                       /* regions with more textures in all take room when replaced */
#define ALIAS_ROOM_MIN (UINT64_C(2) << 20)
#define ALIAS_ROOM_MAX (UINT64_C(16) << 20)
#define ALIAS_ROOM_STEP (UINT64_C(64) << 10)         /* the base moves down in these steps: offsets keep their alignment */

/* The newest region that holds [lo, hi) at an offset `align` divides, or -1. */
static int alias_region_find(const alias_span *r, int n, uint64_t lo, uint64_t hi, uint64_t align) {
    int best = -1;
    for (int i = 0; i < n; i++)
        if (r[i].lo <= lo && hi <= r[i].lo + r[i].size && (lo - r[i].lo) % align == 0 && (best < 0 || r[i].generation > r[best].generation)) best = i;
    return best;
}

/* The heap to make for a texture [lo, hi) that alias_region_find placed nowhere. r: the n current regions of its
   format (they do not overlap one another); others: the m current regions of other formats. room 0: the exact union,
   as before 2026-11-18. */
static alias_plan alias_region_plan(const alias_span *r, int n, const alias_span *others, int m, uint64_t lo, uint64_t hi, int room) {
    alias_plan p = {0, lo, hi};
    uint64_t textures = 0, bottom = UINT64_MAX, top = 0;
    for (int i = 0; i < n; i++) {
        if (!(r[i].lo < hi && lo < r[i].lo + r[i].size)) continue;
        p.merged++; textures += r[i].live;
        if (r[i].lo < bottom) bottom = r[i].lo;
        if (r[i].lo + r[i].size > top) top = r[i].lo + r[i].size;
    }
    if (!p.merged) return p;
    if (bottom < p.lo) p.lo = bottom;
    if (top > p.hi) p.hi = top;
    if (!room || textures <= ALIAS_ROOM_TEXTURES) return p;
    const uint64_t base = p.lo;
    uint64_t step = (p.hi - p.lo) / 4, want_lo = p.lo, want_hi = p.hi;
    if (step < ALIAS_ROOM_MIN) step = ALIAS_ROOM_MIN;
    if (step > ALIAS_ROOM_MAX) step = ALIAS_ROOM_MAX;
    step -= step % ALIAS_ROOM_STEP;
    const int up = hi > top, down = lo < bottom;
    if (up) want_hi = p.hi + step;
    if (down) want_lo = p.lo > step ? p.lo - step : 0;
    for (int i = 0; i < m; i++) {   /* as far as another format's big region over these addresses goes */
        if (others[i].live <= ALIAS_ROOM_TEXTURES || !(others[i].lo < p.hi && p.lo < others[i].lo + others[i].size)) continue;
        if (up && others[i].lo + others[i].size > want_hi) want_hi = others[i].lo + others[i].size;
        if (down && others[i].lo < want_lo) want_lo = others[i].lo;
    }
    for (int i = 0; i < n; i++) {   /* not into a neighbour of this format */
        if (r[i].lo < hi && lo < r[i].lo + r[i].size) continue;
        if (r[i].lo >= p.hi && r[i].lo < want_hi) want_hi = r[i].lo;
        if (r[i].lo + r[i].size <= p.lo && r[i].lo + r[i].size > want_lo) want_lo = r[i].lo + r[i].size;
    }
    p.hi = want_hi;
    p.lo = base - (base - want_lo) / ALIAS_ROOM_STEP * ALIAS_ROOM_STEP;
    return p;
}
