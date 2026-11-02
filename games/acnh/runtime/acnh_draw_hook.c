/* Animal Crossing: New Horizons only -- ACNH_DRAWHOOK=1: which threads are inside the per-object draw function
   main+0x1bf40d0 (x0 = object, w1 = ring slot), for which object and slot. Without ACNH_DRAWHOOK the game code is not touched.

   Why (2026-10-25, ACNH_STACKCB run): worker 0x1008 ended and finalized a command buffer that worker 0x1009 had begun in its
   own stack and was still recording -- two threads inside the same object's draw. This logs the first entries and exits,
   flags every time a thread enters an object another thread is inside (both threads, slots, callers, cores and their recent
   core events), and a running count. The hook entry is acnh_draw_hook.S. */
#include "runtime.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LOG(...) do { fprintf(stderr, "[acnh] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define DRAW_FN 0x1bf40d0ull
uint64_t acnh_draw_resume;   /* main + DRAW_FN + 16: where the displaced prologue continues (acnh_draw_hook.S) */
void acnh_draw_hook(void);
uint32_t runtime_thread_current_handle(void);
int runtime_current_core(void);
int runtime_module_offset(uint64_t pc, const char **name, uint64_t *off);
void acnh_thread_ring_dump(uint32_t h, const char *tag);   /* acnh_thread_diag.h */

#define DRAW_ACTIVE 128
static struct { uint64_t obj, caller, t0; uint32_t th, slot; int core, overlapped, used; } g_in[DRAW_ACTIVE];
static pthread_mutex_t g_in_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t g_calls, g_overlaps, g_logged;
static uint64_t now_ns_raw(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
static void where(uint64_t pc, char *out, size_t n) {
    const char *name; uint64_t off;
    if (runtime_module_offset(pc, &name, &off)) snprintf(out, n, "%s+%#llx", name, (unsigned long long)off); else snprintf(out, n, "%#llx", (unsigned long long)pc);
}

void acnh_draw_enter(uint64_t obj, uint32_t slot, uint64_t caller) {
    uint32_t me = runtime_thread_current_handle(); int core = runtime_current_core();
    uint64_t n = atomic_fetch_add(&g_calls, 1) + 1, t = now_ns_raw();
    char at[48]; where(caller - 4, at, sizeof at);
    pthread_mutex_lock(&g_in_mu);
    int other = -1, free_i = -1;
    for (int i = 0; i < DRAW_ACTIVE; i++) {
        if (!g_in[i].used) { if (free_i < 0) free_i = i; continue; }
        if (g_in[i].obj == obj && other < 0) other = i;
    }
    int overlap = other >= 0;
    if (overlap) {
        g_in[other].overlapped = 1; uint64_t k = atomic_fetch_add(&g_overlaps, 1);
        if (k < 50) {
            char at2[48]; where(g_in[other].caller - 4, at2, sizeof at2);
            LOG("【红】[draw] thread %#x (core %d) enters object %#llx slot %u from %s while thread %#x (core %d, slot %u, from %s) has been inside it %.3f ms%s; overlap #%llu of %llu calls",
                me, core, (unsigned long long)obj, slot, at, g_in[other].th, g_in[other].core, g_in[other].slot, at2, (double)(t - g_in[other].t0) / 1e6,
                g_in[other].th == me ? " (same thread: re-entered)" : "", (unsigned long long)k + 1, (unsigned long long)n);
            uint32_t them = g_in[other].th;
            pthread_mutex_unlock(&g_in_mu);
            acnh_thread_ring_dump(me, "entering"); if (them != me) acnh_thread_ring_dump(them, "inside");
            pthread_mutex_lock(&g_in_mu);
        }
    }
    if (free_i < 0) for (int i = 0; i < DRAW_ACTIVE; i++) if (!g_in[i].used) { free_i = i; break; }
    if (free_i >= 0) g_in[free_i] = (__typeof__(g_in[0])){obj, caller, t, me, slot, core, overlap, 1};
    pthread_mutex_unlock(&g_in_mu);
    if (atomic_fetch_add(&g_logged, 1) < 200)
        LOG("[draw] enter: thread %#x core %d object %#llx slot %u from %s", me, core, (unsigned long long)obj, slot, at);
    if (n % 100000 == 0) LOG("[draw] %llu calls, %llu entered an object another call was inside", (unsigned long long)n, (unsigned long long)atomic_load(&g_overlaps));
}

void acnh_draw_exit(uint64_t obj, uint32_t slot) {
    uint32_t me = runtime_thread_current_handle(); uint64_t t = now_ns_raw();
    pthread_mutex_lock(&g_in_mu);
    int found = -1;   /* this thread's latest entry for the object */
    for (int i = 0; i < DRAW_ACTIVE; i++) if (g_in[i].used && g_in[i].obj == obj && g_in[i].th == me && (found < 0 || g_in[i].t0 > g_in[found].t0)) found = i;
    int overlapped = 0; double ms = 0;
    if (found >= 0) { overlapped = g_in[found].overlapped; ms = (double)(t - g_in[found].t0) / 1e6; g_in[found].used = 0; }
    pthread_mutex_unlock(&g_in_mu);
    if (overlapped && atomic_load(&g_overlaps) <= 50)
        LOG("【红】[draw] thread %#x leaves object %#llx slot %u after %.3f ms (another thread was inside it at the same time)", me, (unsigned long long)obj, slot, ms);
    else if (atomic_load(&g_logged) <= 200)
        LOG("[draw] exit: thread %#x object %#llx slot %u after %.3f ms%s", me, (unsigned long long)obj, slot, ms, found < 0 ? " (no entry found)" : "");
}

/* Called from runtime_game_patches before main's text is sealed (the instruction cache is flushed by the seal). */
void acnh_draw_hook_install(Module *m) {
    if (!getenv("ACNH_DRAWHOOK")) return;
    static const uint32_t want[5] = {0xa9ba6ffc, 0xa90167fa, 0xa9025ff8, 0xa90357f6, 0xa9044ff4};   /* stp x28,x27,[sp,#-96]! ... stp x20,x19,[sp,#64] */
    uint32_t have[5]; memcpy(have, m->base + DRAW_FN, sizeof have);
    if (memcmp(have, want, sizeof want)) {
        fprintf(stderr, "[acnh] ACNH_DRAWHOOK: main+%#llx is %08x %08x %08x %08x, not the expected prologue; hook not installed\n",
                (unsigned long long)DRAW_FN, have[0], have[1], have[2], have[3]);
        return;
    }
    acnh_draw_resume = (uint64_t)(uintptr_t)(m->base + DRAW_FN + 16);
    uint64_t target = (uint64_t)(uintptr_t)&acnh_draw_hook;
    uint32_t words[4] = {0x58000050u /* ldr x16, #8 */, 0xd61f0200u /* br x16 */, (uint32_t)target, (uint32_t)(target >> 32)};
    memcpy(m->base + DRAW_FN, words, sizeof words);
    fprintf(stderr, "[acnh] ACNH_DRAWHOOK: main+%#llx (per-object draw) hooked for entry / exit diagnostics\n", (unsigned long long)DRAW_FN);
}
