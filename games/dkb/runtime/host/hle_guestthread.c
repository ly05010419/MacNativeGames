// Host-initiated guest threads (host background work that calls the game's own allocator / locks, such as the movie player's, needs a complete SDK thread context).
// Approach: on a guest thread (e.g. before guest_main enters nnMain) call the SDK's nn::os::CreateThread / StartThread directly, with a host function as the entry --
// the thread is created by the SDK, so ThreadType, thread handle and TLS slots are all in place, and the game's malloc / nn::os locks work as usual (the lock tag word holds this thread's guest handle).
// Constraints: must be called on a guest thread (SDK thread creation goes through the SVC gate); not exposed to the game.
// Self-test DM_GUEST_SPAWN_TEST=1: before guest_main enters nnMain, start one thread that does "SDK malloc/free + lock an nn::os mutex and check that the owner is itself", and report the result after WaitThread finishes.
#include "hle.h"
#include <stdatomic.h>
#include <unistd.h>
typedef int (*create_fn)(void *tt, void (*fn)(void *), void *arg, void *stack, size_t size, int prio);
typedef void (*tt_fn)(void *tt);
static void *need(const char *n) { void *p = guest_sym(n); if (!p) die("host guest thread: missing symbol %s", n); return p; }
// an entry shim: first register the thread as host-created (hle_thread.c dm_yield_to_core_peers does not wait for them), then run fn
typedef struct { void (*fn)(void *); void *arg; } spawn_arg;
typedef struct { _Alignas(16) uint8_t tt[0x200]; void *stack_alloc; } host_thread;
void dm_thread_mark_host(void);
static void spawn_tramp(void *p) { spawn_arg a = *(spawn_arg *)p; free(p); dm_thread_mark_host(); a.fn(a.arg); }
void *dm_guest_thread_spawn(void (*fn)(void *), void *arg, int prio, size_t stack_size) {
    spawn_arg *sa = malloc(sizeof *sa); if (!sa) die("host guest thread: allocation failed"); sa->fn = fn; sa->arg = arg; fn = spawn_tramp; arg = sa;
    stack_size = (stack_size + 0xfff) & ~(size_t)0xfff;
    host_thread *t = calloc(1, sizeof *t);                       // ThreadType is first, 0x1c0 bytes with headroom
    if (!t) die("host guest thread: allocation failed");
    // CreateThread borrows this range through MapMemory. Host malloc has no
    // guest backing/state (the audio worker failed here in Tears of the Kingdom).
    // Use SDK malloc, retaining the raw pointer until DestroyThread copies back.
    t->stack_alloc = ((void *(*)(size_t))need("malloc"))(stack_size + 0xfff);
    if (!t->stack_alloc) die("host guest thread: stack allocation failed");
    void *stack = (void *)(((uintptr_t)t->stack_alloc + 0xfff) & ~(uintptr_t)0xfff);
    int rc = ((create_fn)need("_ZN2nn2os12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mi"))(t->tt, fn, arg, stack, stack_size, prio);
    if (rc) die("host guest thread: CreateThread failed %#x", rc);
    ((tt_fn)need("_ZN2nn2os11StartThreadEPNS0_10ThreadTypeE"))(t->tt);
    return t;
}
void dm_guest_thread_join(void *tt) {
    ((tt_fn)need("_ZN2nn2os10WaitThreadEPNS0_10ThreadTypeE"))(tt);
    ((tt_fn)need("_ZN2nn2os13DestroyThreadEPNS0_10ThreadTypeE"))(tt);
    host_thread *t = tt;
    ((void (*)(void *))need("free"))(t->stack_alloc);
    free(t);
}

static struct { int malloc_ok, lock_owned, handle; } g_st;
static void selftest_body(void *arg) {
    (void)arg;
    void *(*gmalloc)(size_t) = need("malloc"); void (*gfree)(void *) = need("free");
    void *p = gmalloc(4096); if (p) { memset(p, 0x5a, 4096); gfree(p); g_st.malloc_ok = 1; }
    uint8_t mtx[0x40] __attribute__((aligned(16))) = {0};           // nn::os::MutexType (0x20 bytes, with headroom)
    ((void (*)(void *, int, int))need("_ZN2nn2os15InitializeMutexEPNS0_9MutexTypeEbi"))(mtx, 0, 0);
    ((void (*)(void *))need("_ZN2nn2os9LockMutexEPNS0_9MutexTypeE"))(mtx);
    g_st.lock_owned = ((int (*)(const void *))need("_ZN2nn2os28IsMutexLockedByCurrentThreadEPKNS0_9MutexTypeE"))(mtx) & 1;
    ((void (*)(void *))need("_ZN2nn2os11UnlockMutexEPNS0_9MutexTypeE"))(mtx);
    uint32_t thr_cur_handle(void); g_st.handle = (int)thr_cur_handle();
}
static void park_body(void *arg) { (void)arg; for (;;) sleep(3600); }   // resident (=2): simulates the pre-decode thread staying alive for the whole run, occupying the handle
void dm_guest_spawn_selftest(void) {
    const char *e = getenv("DM_GUEST_SPAWN_TEST"); if (!e) return;
    if (atoi(e) == 2) { dm_guest_thread_spawn(park_body, NULL, 16, 16 << 10); LOG("host guest thread self-check: start a resident thread (later game thread handles shift up by one)"); }
    void *tt = dm_guest_thread_spawn(selftest_body, NULL, 16, 64 << 10);
    dm_guest_thread_join(tt);
    uint32_t thr_cur_handle(void); int mine = (int)thr_cur_handle();
    int ok = g_st.malloc_ok && g_st.lock_owned && g_st.handle && g_st.handle != mine;
    LOG("%shost guest thread self-check: SDK malloc/free %s; mutex owner is this thread %s; thread handle %#x (main thread %#x)", ok ? "" : "[RED] ",
        g_st.malloc_ok ? "ok" : "broken", g_st.lock_owned ? "yes" : "no", g_st.handle, mine);
}
