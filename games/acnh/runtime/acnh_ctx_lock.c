/* Animal Crossing: New Horizons only -- one thread at a time per context in main+0x1be5b30 (on by default; ACNH_CTX_LOCK=0
   leaves the game code untouched).

   Why (2026-10-25, ACNH_DRAWHOOK run): main+0x1be5b30 (called through a vtable, x0 = context) tests the context's dirty bits
   for the current ring slot ([ctx+737] & 1 << ([ctx+741] & 3), main+0x1be5c88), clears them with a plain load / store and then
   draws every dirty object of the context's list (main+0x1be5cec -> main+0x1bf40d0), each into the object's per-slot command
   buffer. Several job threads call it for the same context; woken together they read the dirty bits before either clears
   them, both rebuild the list and record into the same command buffers at once -- every command buffer crash of that day
   (NULL / garbage segment, a recording ended by another thread, a freed recording submitted). With the lock the second
   thread waits, then finds the bits cleared and skips the rebuild, as when the threads do not meet.
   A waiter gives the virtual core up so the holder can run; it never proceeds until that holder releases the context.
   The hook entry is acnh_ctx_lock.S. */
#include "runtime.h"
#include <limits.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG(...) do { fprintf(stderr, "[acnh] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define CTX_FN 0x1be5b30ull
#define CTX_INITIAL_CAP 256
uint64_t acnh_ctx_resume;   /* main + CTX_FN + 16: where the displaced prologue continues (acnh_ctx_lock.S) */
void acnh_ctx_hook(void);
uint32_t runtime_thread_current_handle(void);
int runtime_core_away(void);
void runtime_core_back(int *away);

static struct ctx_owner { uint64_t ctx; pthread_t owner; uint32_t handle; unsigned depth; } *g_ctx;
static int g_ctx_n, g_ctx_cap;
static pthread_mutex_t g_ctx_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_ctx_cv = PTHREAD_COND_INITIALIZER;
static _Atomic uint64_t g_ctx_calls, g_ctx_waits;
static int ctx_find(uint64_t ctx) { for (int i = 0; i < g_ctx_n; i++) if (g_ctx[i].ctx == ctx) return i; return -1; }
static int ctx_other(int i) { return i >= 0 && !pthread_equal(g_ctx[i].owner, pthread_self()); }

static void ctx_reserve(void) {
    if (g_ctx_n < g_ctx_cap) return;
    if (g_ctx_cap > INT_MAX / 2) { LOG("[ctxlock] context table capacity overflow"); abort(); }
    int cap = g_ctx_cap ? g_ctx_cap * 2 : CTX_INITIAL_CAP;
    void *p = realloc(g_ctx, (size_t)cap * sizeof *g_ctx);
    if (!p) { LOG("[ctxlock] cannot grow context ownership table"); abort(); }
    g_ctx = p;
    g_ctx_cap = cap;
}

void acnh_ctx_lock(uint64_t ctx) {
    uint64_t calls = atomic_fetch_add(&g_ctx_calls, 1) + 1;
    pthread_mutex_lock(&g_ctx_mu);
    int i = ctx_find(ctx);
    if (ctx_other(i)) {   /* another thread is inside this context: wait for it with the virtual core given up */
        uint32_t them = g_ctx[i].handle; uint64_t t0 = mach_absolute_time();
        while (ctx_other(i)) {
            pthread_mutex_unlock(&g_ctx_mu);
            int away = runtime_core_away();
            pthread_mutex_lock(&g_ctx_mu);
            while (ctx_other(i = ctx_find(ctx))) pthread_cond_wait(&g_ctx_cv, &g_ctx_mu);
            pthread_mutex_unlock(&g_ctx_mu);
            runtime_core_back(&away);   /* no lock held while taking a core back */
            pthread_mutex_lock(&g_ctx_mu);
            i = ctx_find(ctx);
        }
        uint64_t n = atomic_fetch_add(&g_ctx_waits, 1);
        if (n < 20 || n % 1000 == 0) {
            mach_timebase_info_data_t tb; mach_timebase_info(&tb);
            LOG("[ctxlock] thread %#x waited %.3f ms for thread %#x to leave context %#llx (wait %llu, %llu calls)",
                runtime_thread_current_handle(), (double)(mach_absolute_time() - t0) * tb.numer / tb.denom / 1e6, them,
                (unsigned long long)ctx, (unsigned long long)n + 1, (unsigned long long)calls);
        }
    }
    if (i >= 0 && !ctx_other(i)) g_ctx[i].depth++;   /* this thread again (re-entered) */
    else if (i < 0) {
        ctx_reserve();
        g_ctx[g_ctx_n++] = (struct ctx_owner){ctx, pthread_self(), runtime_thread_current_handle(), 1};
    }
    pthread_mutex_unlock(&g_ctx_mu);
}

void acnh_ctx_unlock(uint64_t ctx) {
    pthread_mutex_lock(&g_ctx_mu);
    int i = ctx_find(ctx);
    if (i >= 0 && !ctx_other(i) && --g_ctx[i].depth == 0) { g_ctx[i] = g_ctx[--g_ctx_n]; pthread_cond_broadcast(&g_ctx_cv); }
    pthread_mutex_unlock(&g_ctx_mu);
}

/* Called from runtime_game_patches before main's text is sealed (the instruction cache is flushed by the seal). */
void acnh_ctx_lock_install(Module *m) {
    const char *e = getenv("ACNH_CTX_LOCK");
    if (e && !strcmp(e, "0")) { fprintf(stderr, "[acnh] ACNH_CTX_LOCK=0: main+%#llx left unlocked\n", (unsigned long long)CTX_FN); return; }
    static const uint32_t want[5] = {0xd10283ff, 0xf90023fb, 0xa90567fa, 0xa9065ff8, 0xa90757f6};   /* sub sp,sp,#160; str x27,[sp,#64]; stp x26,x25 / x24,x23 / x22,x21 */
    uint32_t have[5]; memcpy(have, m->base + CTX_FN, sizeof have);
    if (memcmp(have, want, sizeof want)) {
        fprintf(stderr, "[acnh] ACNH_CTX_LOCK: main+%#llx is %08x %08x %08x %08x, not the expected prologue; lock not installed\n",
                (unsigned long long)CTX_FN, have[0], have[1], have[2], have[3]);
        return;
    }
    acnh_ctx_resume = (uint64_t)(uintptr_t)(m->base + CTX_FN + 16);
    uint64_t target = (uint64_t)(uintptr_t)&acnh_ctx_hook;
    uint32_t words[4] = {0x58000050u /* ldr x16, #8 */, 0xd61f0200u /* br x16 */, (uint32_t)target, (uint32_t)(target >> 32)};
    memcpy(m->base + CTX_FN, words, sizeof words);
    fprintf(stderr, "[acnh] ACNH_CTX_LOCK: main+%#llx (rebuild of a context's dirty objects) runs one thread per context\n", (unsigned long long)CTX_FN);
}
