/* Is every page of a guest range readable now? One byte per host page (and the last byte) is read under the fault guard
   (runtime_fault_guard, thread-local; the includer declares it).

   2026-11-19, Ocarina of Time on the user's route: the first upload of a texture read pages the system had paged out
   (mincore before the read: 127 of 128 pages of a 2 MB texture), 70-200 us a page, 9-25 ms a texture on the submit
   thread. Bringing a page back is the faulting thread's work, so a range of NVN_PROBE_PARALLEL_PAGES pages or more is
   read in chunks on several threads at once (each with its own guard); the submit thread waits for the slowest chunk.
   TOTK_PROBE_PARALLEL=0 reads it on the calling thread alone, as before. The answer is the same either way: 0 when
   any page faults. */
#ifndef NVN_GUEST_PROBE_H
#define NVN_GUEST_PROBE_H
#include <dispatch/dispatch.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern __thread sigjmp_buf *runtime_fault_guard;   /* the runtime's loader.c; its signal handler jumps to it */

#define NVN_PROBE_PARALLEL_PAGES 32   /* below this (512 KB of 16 KB pages) one thread: the dispatch costs more */
#define NVN_PROBE_CHUNK_PAGES 8

static int nvn_guest_probe_serial(const void *mem, uint64_t size) {
    if (!size) return 1;
    uintptr_t pg = (uintptr_t)getpagesize();
    const volatile uint8_t *m = (const volatile uint8_t *)mem; uintptr_t a = (uintptr_t)mem, e = a + size;
    sigjmp_buf jb, *prev = runtime_fault_guard;
    if (sigsetjmp(jb, 1)) { runtime_fault_guard = prev; return 0; }
    runtime_fault_guard = &jb;
    uint8_t sink = 0;
    for (uintptr_t at = a; at < e; at = (at & ~(pg - 1)) + pg) sink ^= m[at - a];
    sink ^= m[size - 1];
    runtime_fault_guard = prev;
    (void)sink;
    return 1;
}

static int nvn_probe_parallel_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("TOTK_PROBE_PARALLEL"); on = !(e && !strcmp(e, "0")); }
    return on;
}

/* The page chunk `i` of [a, e): it starts at a itself for the first chunk, at a page start otherwise. */
static void nvn_probe_chunk(uintptr_t a, uintptr_t e, uintptr_t pg, size_t i, uintptr_t *lo, uintptr_t *hi) {
    uintptr_t base = (a & ~(pg - 1)) + (uintptr_t)i * NVN_PROBE_CHUNK_PAGES * pg;
    *lo = base < a ? a : base;
    *hi = base + NVN_PROBE_CHUNK_PAGES * pg; if (*hi > e) *hi = e;
}

static int nvn_guest_probe_threads(const void *mem, uint64_t size, int parallel) {
    if (!size) return 1;
    uintptr_t pg = (uintptr_t)getpagesize(), a = (uintptr_t)mem, e = a + size;
    size_t pages = (size_t)((e - 1) / pg - a / pg + 1);
    if (!parallel || pages < NVN_PROBE_PARALLEL_PAGES) return nvn_guest_probe_serial(mem, size);
    size_t chunks = (pages + NVN_PROBE_CHUNK_PAGES - 1) / NVN_PROBE_CHUNK_PAGES;
    __block int bad = 0;
    dispatch_apply(chunks, dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^(size_t i) {
        uintptr_t lo, hi; nvn_probe_chunk(a, e, pg, i, &lo, &hi);
        if (lo < hi && !nvn_guest_probe_serial((const void *)lo, hi - lo)) __atomic_store_n(&bad, 1, __ATOMIC_RELAXED);
    });
    return !bad;
}

static int nvn_guest_probe(const void *mem, uint64_t size) { return nvn_guest_probe_threads(mem, size, nvn_probe_parallel_on()); }
#endif
