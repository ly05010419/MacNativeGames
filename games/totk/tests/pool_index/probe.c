/* TOTK: differential test of the memory pool index (gfx/nvn/nvn_pool_index.h) against the old linear scan.
   Random inserts / removes / lookups over disjoint, nested and overlapping ranges; every lookup must agree with the
   scan on whether a containing pool exists, and the returned pool must contain the range. Two negative controls
   (lookup without the backwards walk, prefix maximum not maintained) must be caught. Ends with a timing comparison
   at the world-scene pool count (3,229). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "nvn_pool_index.h"

typedef struct { uintptr_t lo, hi; const void *key; } ref_ent;
static ref_ent g_ref[8192]; static int g_nref;
static uint64_t g_rng = 0x9e3779b97f4a7c15ull;
static uint64_t rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17; return g_rng; }

static int ref_find(uintptr_t addr, uint64_t len) {
    for (int i = 0; i < g_nref; i++) if (addr >= g_ref[i].lo && addr + len <= g_ref[i].hi) return i;
    return -1;
}
static int ref_contains(const void *key, uintptr_t addr, uint64_t len) {
    for (int i = 0; i < g_nref; i++)
        if (g_ref[i].key == key) return addr >= g_ref[i].lo && addr + len <= g_ref[i].hi;
    return 0;
}
typedef const pidx_ent *(*find_fn)(const pidx *, uintptr_t, uint64_t);
static const pidx_ent *find_no_walk(const pidx *x, uintptr_t addr, uint64_t len) {   // negative control 1
    int i = pidx_upper(x, addr) - 1;
    return i >= 0 && x->e[i].hi >= addr + len ? &x->e[i] : NULL;
}
static long g_checks;
static int check_one(const pidx *x, find_fn f, uintptr_t addr, uint64_t len) {
    const pidx_ent *p = f(x, addr, len); int r = ref_find(addr, len); g_checks++;
    if ((p != NULL) != (r >= 0)) return 0;
    if (p && !ref_contains(p->key, addr, len)) return 0;
    if (p && (p->lo > addr || p->hi < addr + len)) return 0;
    return 1;
}
static void add(pidx *x, uintptr_t lo, uintptr_t hi, uintptr_t key) {
    pidx_ent v = {.lo=lo, .hi=hi, .base=lo & ~(uintptr_t)0x3fff, .buf=(void *)key,
                  .gpu=0x1000000000ull + lo, .key=(const void *)key};
    if (!pidx_insert(x, &v)) { fprintf(stderr, "insert failed\n"); exit(2); }
    g_ref[g_nref++] = (ref_ent){lo, hi, (const void *)key};
}
static void del(pidx *x, int r) {
    pidx_ent out;
    if (!pidx_remove(x, g_ref[r].key, &out) || out.key != g_ref[r].key) { fprintf(stderr, "remove failed\n"); exit(2); }
    g_ref[r] = g_ref[--g_nref];
}
// One randomized session: returns the number of mismatching lookups for lookup function f.
static long session(find_fn f, int corrupt_maxhi, uint64_t seed) {
    pidx x = {0}; g_nref = 0; g_rng = seed; long bad = 0; uintptr_t key = 1;
    for (int step = 0; step < 20000; step++) {
        uint64_t op = rnd() % 100;
        if ((op < 45 && g_nref < 4000) || g_nref < 8) {
            uintptr_t lo = 0x70000000000ull + (rnd() % 0x4000000) * 0x100, size = 0x100 + (rnd() % 0x4000) * 0x10;
            if (rnd() % 4 == 0 && g_nref) {   // nested inside or overlapping an existing pool
                const ref_ent *o = &g_ref[rnd() % g_nref];
                lo = o->lo + (rnd() % (o->hi - o->lo)); size = 1 + rnd() % ((o->hi - lo) * 2 + 1);
            }
            add(&x, lo, lo + size, key++);
        } else if (op < 60 && g_nref) del(&x, (int)(rnd() % g_nref));
        else {
            uintptr_t addr; uint64_t len = rnd() % 3 == 0 ? 0 : 1 + rnd() % 0x2000;
            if (g_nref && rnd() % 2) {   // around an existing range: inside, at both ends, just outside
                const ref_ent *o = &g_ref[rnd() % g_nref]; uintptr_t pick[6] = {o->lo, o->hi - (len < o->hi - o->lo ? len : 0), o->lo - 1, o->hi, o->hi + 1,
                                                                                    o->lo + rnd() % (o->hi - o->lo + 1)};
                addr = pick[rnd() % 6];
            } else addr = 0x70000000000ull + (rnd() % 0x4000000) * 0x100 + rnd() % 0x100;
            if (corrupt_maxhi) for (int i = 0; i < x.n; i++) x.maxhi[i] = x.e[i].hi;   // negative control 2
            bad += !check_one(&x, f, addr, len);
            if (corrupt_maxhi) pidx_fix_maxhi(&x, 0);
        }
    }
    for (int i = 0; i + 1 < x.n; i++) if (x.e[i].lo > x.e[i + 1].lo) { fprintf(stderr, "index not sorted at %d\n", i); exit(2); }
    free(x.e); free(x.maxhi);
    return bad;
}
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
int main(void) {
    long bad = 0;
    for (uint64_t s = 1; s <= 8; s++) bad += session(pidx_find, 0, s * 0x9e3779b97f4a7c15ull);
    printf("pool index: %ld lookups checked against the linear scan, %ld mismatches\n", g_checks, bad);
    long checked = g_checks;
    if (bad) return 1;
    long neg1 = session(find_no_walk, 0, 0x1234567), neg2 = session(pidx_find, 1, 0x7654321);
    printf("negative controls: no backwards walk %ld mismatches, stale prefix maximum %ld mismatches\n", neg1, neg2);
    if (!neg1 || !neg2) { fprintf(stderr, "a negative control was not detected\n"); return 1; }
    // Timing at the world-scene pool count, disjoint ranges registered in random order.
    pidx x = {0}; g_nref = 0; enum { N = 3229, Q = 2000000 };
    uintptr_t *starts = malloc(N * sizeof *starts);
    for (int i = 0; i < N; i++) starts[i] = 0x70000000000ull + (uintptr_t)i * 0x100000;
    for (int i = N - 1; i > 0; i--) { int j = (int)(rnd() % (uint64_t)(i + 1)); uintptr_t t = starts[i]; starts[i] = starts[j]; starts[j] = t; }
    for (int i = 0; i < N; i++) add(&x, starts[i], starts[i] + 0x80000, (uintptr_t)i + 1);
    uintptr_t *q = malloc(Q * sizeof *q);
    for (int i = 0; i < Q; i++) q[i] = g_ref[rnd() % N].lo + rnd() % 0x7f000;
    volatile uintptr_t sink = 0; double t0 = now_s();
    for (int i = 0; i < Q; i++) { const pidx_ent *p = pidx_find(&x, q[i], 64); sink += p ? p->gpu : 0; }
    double t1 = now_s();
    for (int i = 0; i < Q / 20; i++) { int r = ref_find(q[i], 64); sink += r >= 0 ? g_ref[r].lo : 0; }
    double t2 = now_s();
    printf("timing with %d pools: index %.1f ns per lookup, linear scan %.1f ns per lookup\n", N, (t1 - t0) / Q * 1e9, (t2 - t1) / (Q / 20) * 1e9);
    printf("{\"status\":\"pass\",\"lookups_checked\":%ld,\"negative_controls\":2}\n", checked);
    return 0;
}
