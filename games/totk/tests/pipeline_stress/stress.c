/* CPU only, for AddressSanitizer: the TOTK_PIPELINE helper-thread protection queue (totk_pl_protq.h) and the sorted range
   index of nvn_dirty.c (rebuilt and retired while other threads look ranges up), under concurrent use.
   - a producer pushes spans (the translate thread) and now and then syncs up to what it pushed (a release on another
     thread syncs too); every span pushed must be run exactly once, and a sync returns only when its spans have run;
   - a writer thread adds and removes thousands of ranges (index rebuilds) while two threads look up random addresses:
     every answer must be a live range holding the address or none.
   Build: make probe-pipeline-stress (clang -fsanitize=address,undefined). */
#define NVN_DIRTY_NO_HANDLE_DEFAULT 1
#include "../../third_party/d2r_gfx/gfx/nvn/nvn_dirty.c"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
uint32_t runtime_thread_current_handle(void) { return 0; }
void dm_fault_hook_register(int (*fn)(void *, int, void *)) { (void)fn; }
void dm_guest_write_prep_register(void (*fn)(void *, size_t)) { (void)fn; }
typedef struct { uintptr_t lo, hi; } pl_rng;
enum { NSPAN = 3000000 };
static _Atomic uint8_t g_ran[2 * NSPAN]; static _Atomic uint64_t g_dup;   // span i is [2i, 2i+1): spans do not touch, batches fill up
static void run_one(uintptr_t lo, uintptr_t hi) { for (uintptr_t i = lo; i < hi; i++) if (atomic_fetch_add(&g_ran[i], 1)) atomic_fetch_add(&g_dup, 1); }   // spans touching each other are joined by the producer's batch
#define PL_PROTQ_RUN(lo, hi) run_one((lo), (hi))
#define PL_PROTQ_DIE(m) do { fprintf(stderr, "%s\n", m); abort(); } while (0)
#include "../../third_party/d2r_gfx/gfx/nvn/totk_pl_protq.h"
static _Atomic int g_stop; static _Atomic uint64_t g_bad_sync, g_bad_find, g_finds;
static pthread_mutex_t g_cov_mu = PTHREAD_MUTEX_INITIALIZER; static uint64_t g_cov_seq, g_cov_upto;   // a commit: the sequence number and the spans it covers
static void *releaser(void *arg) {   // syncs up to the last commit, from another thread (a release)
    (void)arg;
    while (!atomic_load(&g_stop)) {
        pthread_mutex_lock(&g_cov_mu); uint64_t seq = g_cov_seq, upto = g_cov_upto; pthread_mutex_unlock(&g_cov_mu);
        pl_protq_sync(seq);
        for (uint64_t i = 0; i < upto; i++) if (!atomic_load(&g_ran[2 * i])) { atomic_fetch_add(&g_bad_sync, 1); break; }
        usleep(200);
    }
    return NULL;
}
static uint8_t *g_mem; static size_t g_pg; enum { NR = 2000 };
static void *ranges(void *arg) {   // add / remove ranges: index rebuilds
    (void)arg;
    for (int round = 0; !atomic_load(&g_stop); round++)
        for (int i = 0; i < NR; i++) {
            uint8_t *m = g_mem + (size_t)i * 3 * g_pg;
            if ((i + round) % 2) nvn_dirty_add_range(m, 2 * g_pg); else nvn_dirty_remove_range(m);
        }
    return NULL;
}
static void *finder(void *arg) {
    (void)arg;
    while (!atomic_load(&g_stop)) {
        size_t off = (size_t)arc4random_uniform((uint32_t)(3 * NR)) * g_pg + arc4random_uniform((uint32_t)g_pg);
        uintptr_t a = (uintptr_t)g_mem + off; range_t *r = find(a);
        if (r && (a < r->base || a >= r->end)) atomic_fetch_add(&g_bad_find, 1);
        if (r && off % (3 * g_pg) >= 2 * g_pg) atomic_fetch_add(&g_bad_find, 1);   // the gap page is never in a range
        atomic_fetch_add(&g_finds, 1);
    }
    return NULL;
}
int main(void) {
    g_pg = (size_t)getpagesize();
    g_mem = mmap(NULL, g_pg * 3 * NR, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    pthread_t rel, rg, f1, f2;
    pthread_create(&rel, NULL, releaser, NULL); pthread_create(&rg, NULL, ranges, NULL); pthread_create(&f1, NULL, finder, NULL); pthread_create(&f2, NULL, finder, NULL);
    for (uint64_t i = 0; i < NSPAN; i++) {   // the producer
        pl_protq_push((uintptr_t)(2 * i), (uintptr_t)(2 * i + 1));
        if (i % 3 == 0) pl_protq_push((uintptr_t)(2 * i), (uintptr_t)(2 * i + 1));   // the same span again (joined)
        if (i % 977 == 0) { uint64_t seq = pl_protq_seq(); pthread_mutex_lock(&g_cov_mu); g_cov_seq = seq; g_cov_upto = i + 1; pthread_mutex_unlock(&g_cov_mu);
            pl_protq_sync(seq); for (uint64_t k = 0; k <= i; k++) if (!atomic_load(&g_ran[2 * k])) { atomic_fetch_add(&g_bad_sync, 1); break; } }
    }
    pl_protq_sync(pl_protq_seq());
    atomic_store(&g_stop, 1);
    pthread_join(rel, NULL); pthread_join(rg, NULL); pthread_join(f1, NULL); pthread_join(f2, NULL);
    uint64_t missing = 0; for (int i = 0; i < NSPAN; i++) missing += !atomic_load(&g_ran[2 * i]);
    printf("spans %d: missing %llu, run twice %llu, syncs returned early %llu; lookups %llu, wrong %llu\n", NSPAN, (unsigned long long)missing,
           (unsigned long long)atomic_load(&g_dup), (unsigned long long)atomic_load(&g_bad_sync), (unsigned long long)atomic_load(&g_finds), (unsigned long long)atomic_load(&g_bad_find));
    return missing || atomic_load(&g_dup) || atomic_load(&g_bad_sync) || atomic_load(&g_bad_find);
}
