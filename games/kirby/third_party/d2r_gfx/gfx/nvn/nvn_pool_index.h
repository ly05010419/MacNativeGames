#pragma once
/* TOTK: memory pools sorted by guest address.
   The world scene keeps about 3,200 pools alive, and the replay resolves several dozen buffer addresses per draw
   (vertex / index / uniform / storage bindings). A linear scan of all pools for each of them was the largest part of
   the ~87k instructions per draw. Lookups are now a binary search on the range start plus a backwards walk that is
   bounded by a running maximum of the range ends, so a pool nested inside or overlapping another is still found.
   Not thread-safe: the caller holds the pool mutex for every call. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uintptr_t lo, hi, base; void *buf; uint64_t gpu; const void *key; int res; } pidx_ent;   // res: buffer is in the residency set (nvn_mtl.m pool_res)
typedef struct { pidx_ent *e; uintptr_t *maxhi; int n, cap; } pidx;   // maxhi[i] = largest hi among e[0..i]

static inline void pidx_fix_maxhi(pidx *x, int from) {
    for (int i = from > 0 ? from : 0; i < x->n; i++) {
        uintptr_t prev = i ? x->maxhi[i - 1] : 0;
        x->maxhi[i] = x->e[i].hi > prev ? x->e[i].hi : prev;
    }
}
// Number of entries whose range starts at or below addr (index of the first start above it).
static inline int pidx_upper(const pidx *x, uintptr_t addr) {
    int a = 0, b = x->n;
    while (a < b) { int m = (a + b) >> 1; if (x->e[m].lo <= addr) a = m + 1; else b = m; }
    return a;
}
// Returns 0 when memory runs out (the index is left unchanged).
static inline int pidx_insert(pidx *x, const pidx_ent *v) {
    if (x->n == x->cap) {
        int cap = x->cap ? x->cap * 2 : 64;
        pidx_ent *e = realloc(x->e, (size_t)cap * sizeof *e); if (!e) return 0; x->e = e;
        uintptr_t *mh = realloc(x->maxhi, (size_t)cap * sizeof *mh); if (!mh) return 0; x->maxhi = mh;
        x->cap = cap;
    }
    int at = pidx_upper(x, v->lo);
    memmove(x->e + at + 1, x->e + at, (size_t)(x->n - at) * sizeof *x->e);
    x->e[at] = *v; x->n++;
    pidx_fix_maxhi(x, at);
    return 1;
}
// Removes the entry registered for key; returns 0 when there is none.
static inline int pidx_remove(pidx *x, const void *key, pidx_ent *out) {
    for (int i = 0; i < x->n; i++) {
        if (x->e[i].key != key) continue;
        if (out) *out = x->e[i];
        memmove(x->e + i, x->e + i + 1, (size_t)(x->n - i - 1) * sizeof *x->e); x->n--;
        pidx_fix_maxhi(x, i);
        return 1;
    }
    return 0;
}
// A pool whose range [lo, hi] contains [addr, addr + len], or NULL (same containment test as the old linear scan).
static inline const pidx_ent *pidx_find(const pidx *x, uintptr_t addr, uint64_t len) {
    uintptr_t end = addr + (uintptr_t)len;
    if (end < addr) return NULL;
    for (int i = pidx_upper(x, addr) - 1; i >= 0 && x->maxhi[i] >= end; i--)
        if (x->e[i].hi >= end) return &x->e[i];
    return NULL;
}
// The same lookup for callers that update the entry they find (they hold the pool mutex as well).
static inline pidx_ent *pidx_find_mut(pidx *x, uintptr_t addr, uint64_t len) { return (pidx_ent *)pidx_find(x, addr, len); }
// 1 when [lo, hi) overlaps a registered range (diagnostic: real NVN pools do not normally overlap).
static inline int pidx_overlaps(const pidx *x, uintptr_t lo, uintptr_t hi) {
    int at = pidx_upper(x, lo);
    if (at > 0 && x->maxhi[at - 1] > lo) return 1;
    return at < x->n && x->e[at].lo < hi;
}

/* Exact-query memoization, protected by the SAME mutex as the index. The caller
   must invalidate after every successful insertion/removal, including while the
   experiment is disabled. It stores indices, not retained resource pointers.
   Overlapping ranges retain pidx_find's exact selection and zero-length semantics. */
#define PIDX_QUERY_SLOTS 256
typedef struct { uintptr_t address; uint64_t length, epoch; int index; } pidx_query;
typedef struct { pidx_query queries[PIDX_QUERY_SLOTS]; uint64_t epoch, hits, misses; } pidx_query_cache;
static inline void pidx_query_invalidate(pidx_query_cache *cache) {
    if (++cache->epoch == 0) { memset(cache->queries, 0, sizeof cache->queries); cache->epoch = 1; }
}
static inline pidx_ent *pidx_find_cached(pidx *x, pidx_query_cache *cache, uintptr_t address, uint64_t length) {
    if (!cache->epoch) cache->epoch = 1;
    uint64_t hash = (address >> 4) ^ (address >> 20) ^ (length * UINT64_C(0x9e3779b97f4a7c15));
    pidx_query *q = &cache->queries[hash & (PIDX_QUERY_SLOTS - 1)];
    if (q->epoch == cache->epoch && q->address == address && q->length == length && q->index < x->n) {
        cache->hits++;
        return q->index < 0 ? NULL : &x->e[q->index];
    }
    pidx_ent *found = pidx_find_mut(x, address, length);
    *q = (pidx_query){address, length, cache->epoch, found ? (int)(found - x->e) : -1};
    cache->misses++;
    return found;
}
