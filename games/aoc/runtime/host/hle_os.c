// Host common miscellany: guest object side table, monotonic clock, C library imports that none of the modules define.
// (The former 136 HLE entries for nn::os/mem/lmem/C++ ABI have been deleted: the SDK brings its own nn::os, and at load time not one of these entries was resolved and used.)
#include <pthread.h>
#include "hle.h"
#include <mach/mach_time.h>
#include <stdatomic.h>

// ---------- Side table ----------
#define OM_SIZE (1u << 18)
static struct { const void *k; void *v; } g_om[OM_SIZE];
static pthread_mutex_t g_om_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned om_h(const void *k) { uint64_t x = (uint64_t)k; x ^= x >> 29; x *= 0xbf58476d1ce4e5b9ull; return (unsigned)(x >> 40) & (OM_SIZE - 1); }
void *objmap_get(const void *k) {
    pthread_mutex_lock(&g_om_lock);
    void *r = NULL;
    for (unsigned i = om_h(k);; i = (i + 1) & (OM_SIZE - 1)) {
        if (g_om[i].k == k) { r = g_om[i].v; break; }
        if (!g_om[i].k) break;
    }
    pthread_mutex_unlock(&g_om_lock);
    return r;
}
void objmap_put(const void *k, void *v) {
    pthread_mutex_lock(&g_om_lock);
    for (unsigned i = om_h(k);; i = (i + 1) & (OM_SIZE - 1))
        if (g_om[i].k == k || !g_om[i].k || g_om[i].k == (void *)1) { g_om[i].k = k; g_om[i].v = v; break; }
    pthread_mutex_unlock(&g_om_lock);
}
void *objmap_del(const void *k) {
    pthread_mutex_lock(&g_om_lock);
    void *r = NULL;
    for (unsigned i = om_h(k);; i = (i + 1) & (OM_SIZE - 1)) {
        if (g_om[i].k == k) { r = g_om[i].v; g_om[i].k = (void *)1; g_om[i].v = NULL; break; }
        if (!g_om[i].k) break;
    }
    pthread_mutex_unlock(&g_om_lock);
    return r;
}
uint64_t now_ns(void) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return mach_absolute_time() * tb.numer / tb.denom;
}
// Guest clock: the system counters the guest can read (mrs cntpct gate, svc GetSystemTick) all go through here. Default = now_ns().
// Freeze (only for the test driver DM_TICK_HOLD): while frozen the guest clock does not advance ⇒ the game's next tick never comes due; after unfreezing the frozen duration is subtracted, the guest clock is continuous with no jump
// DM_DETCLOCK=<step ns> (for replay / test runs; product runs do not set it = wall clock): from the first present on the guest clock becomes "advance a fixed step per present", not following the wall clock --
// the client's per-frame dt and the server tick catch-up snapshots are both determined by the present count, so frame N in a run is byte-for-byte comparable between two passes of the same replay (the foundation of byte-for-byte picture proof).
// Before the first frame the wall clock is still used (the boot period has waits that poll by time, freezing would hang them); at the switch it continues from the guest clock value at that time, no jump. It does not advance while frozen.
static _Atomic uint64_t g_gfrozen, g_glag, g_gdet, g_guest_frame; static _Atomic int g_gfz, g_gdet_on; static uint64_t g_gdet_step = 1;
uint64_t dm_guest_frame(void) { return atomic_load(&g_guest_frame); }
static int detclock_cfg(void) {
    static int c = -1; if (c < 0) { const char *e = getenv("DM_DETCLOCK"); g_gdet_step = e ? strtoull(e, 0, 0) : 0; c = g_gdet_step > 0; }
    return c;
}
static uint64_t wall_guest_ns(void) { return atomic_load(&g_gfz) ? atomic_load(&g_gfrozen) : now_ns() - atomic_load(&g_glag); }
uint64_t guest_ns(void) { return atomic_load(&g_gdet_on) ? atomic_load(&g_gdet) : wall_guest_ns(); }
int dm_detclock_enabled(void) { return detclock_cfg() && atomic_load(&g_gdet_on); }
void dm_guest_frame_tick(void) {
    crash_watch_frame();
    atomic_fetch_add(&g_guest_frame, 1);
    if (!detclock_cfg()) return;
    if (!atomic_load(&g_gdet_on)) {
        atomic_store(&g_gdet, wall_guest_ns()); atomic_store(&g_gdet_on, 1);
        LOG("DM_DETCLOCK: from the first present on, the guest clock advances by presents, step %llu ns", (unsigned long long)g_gdet_step); return;
    }
    if (!atomic_load(&g_gfz)) { atomic_fetch_add(&g_gdet, g_gdet_step); dm_guest_clock_advanced(); }
}
static void guest_clock_freeze_apply(int on) {
    if (on && !atomic_load(&g_gfz)) { atomic_store(&g_gfrozen, now_ns() - atomic_load(&g_glag)); atomic_store(&g_gfz, 1); }
    else if (!on && atomic_load(&g_gfz)) { atomic_store(&g_glag, now_ns() - atomic_load(&g_gfrozen)); atomic_store(&g_gfz, 0); }
}
// Freezing has several unrelated reasons (0 freeze tick for testing / loading screen, 1 App in background, 2 system keyboard up): freeze if any holds, release only when all are lifted --
// otherwise "switch to background and back while the keyboard is up" would be released early by the unfreeze on returning to foreground
static pthread_mutex_t g_gfz_mu = PTHREAD_MUTEX_INITIALIZER; static unsigned g_gfz_reasons;
// A suspended application does not run: Horizon suspends the whole process out of focus, iOS suspends a background app. The host freezes the guest clock in the background but its
// guest threads go on until they block, so a guest reading the clock twice there saw no time pass. Super Mario Galaxy 2's frame pacing (main+0x5926a8) took the interval between two
// frames as its period, got 0, and the next frame's loop that steps by it (main+0x9abd0) never ended. Guest reads of the system counter (the cntpct gate, svc GetSystemTick) wait while
// the app is in the background (reason 1); host work (dm_host_work: inside a submit or present, which the graphics suspend waits for) reads the frozen clock, as do the other reasons
static pthread_cond_t g_gfz_cv = PTHREAD_COND_INITIALIZER; static _Atomic int g_gfz_bg;
void guest_clock_read_wait(void) {
    if (!atomic_load(&g_gfz_bg) || atomic_load(&dm_host_work)) return;
    pthread_mutex_lock(&g_gfz_mu);
    while (g_gfz_reasons & 2u) pthread_cond_wait(&g_gfz_cv, &g_gfz_mu);
    pthread_mutex_unlock(&g_gfz_mu);
}
void guest_clock_freeze_for(int reason, int on) {
    pthread_mutex_lock(&g_gfz_mu);
    if (on) g_gfz_reasons |= 1u << reason; else g_gfz_reasons &= ~(1u << reason);
    guest_clock_freeze_apply(g_gfz_reasons != 0);
    atomic_store(&g_gfz_bg, (g_gfz_reasons & 2u) != 0);
    if (!(g_gfz_reasons & 2u)) pthread_cond_broadcast(&g_gfz_cv);   // after the clock is running again: a released read sees it continue
    pthread_mutex_unlock(&g_gfz_mu);
}
void guest_clock_freeze(int on) { guest_clock_freeze_for(0, on); }
// Entropy source: true random by default (arc4random); with DM_DETCLOCK on it becomes a fixed-seed splitmix64 stream (so that replay pictures are byte-for-byte comparable, the camp blue fire particles differed between two passes).
// The two outlets (libc getentropy, svc GetInfo RandomEntropy) share one stream; the call count is reported in the log (first call and every 1000 calls)
static _Atomic uint64_t g_detrng = 0x5357444554524e47ull; static _Atomic uint64_t g_detrng_n;
uint64_t dm_entropy64(void) {
    if (!detclock_cfg()) return ((uint64_t)arc4random() << 32) | arc4random();
    uint64_t z = atomic_fetch_add(&g_detrng, 0x9E3779B97F4A7C15ull) + 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull; z = (z ^ (z >> 27)) * 0x94D049BB133111EBull; z ^= z >> 31;
    uint64_t n = atomic_fetch_add(&g_detrng_n, 1) + 1; if (n == 1 || n % 1000 == 0) LOG("DM_DETCLOCK: deterministic entropy call %llu", (unsigned long long)n);
    return z;
}
HLE("getentropy", int, g_getentropy, void *p, size_t n) {
    if (!detclock_cfg()) { arc4random_buf(p, n); return 0; }
    for (size_t i = 0; i < n; i += 8) { uint64_t v = dm_entropy64(); memcpy((uint8_t *)p + i, &v, n - i < 8 ? n - i : 8); }
    return 0;
}
// The SDK's own getentropy (Unicorn Overlord, SDK 16.2.4: std::random_device at nnMain) fills through nn::crypto::GenerateCryptographicallyRandomBytes, which opens the csrng service;
// Pokémon Legends: Z-A (SDK 19) calls it first thing in nnMain (main+0x1734b4)
// (InitializeCsrng → sm::Initialize, the game aborted there). The call goes through the sdk's PLT, so the takeover gives it the same stream as getentropy above
HLE_FORCE("_ZN2nn6crypto36GenerateCryptographicallyRandomBytesEPvm", "csrng service; host entropy (the getentropy stream)", void, g_csrng_bytes, void *p, size_t n) {
    g_getentropy(p, n);
}

// ---------- getenv: the game profile's game_env ----------
// Horizon gives a program no environment, and the SDK's C library starts with an empty environ. Runtimes that take their settings from the environment then run on defaults sized for the
// Switch's 64 GB alias region: Absolum's .NET GC (NativeAOT) reserves 5 × its heap limit as its region range, 3.75 GB, unless DOTNET_GCRegionRange says otherwise.
// The profile's game_env (KEY=VALUE, separated by spaces; DM_GAME_ENV overrides it, also from Documents/env.txt) answers getenv for those names; any other name goes to the SDK's own getenv
static const char **g_env_kv;   // "KEY=VALUE" strings, NULL-terminated
static void game_env_init(void) {
    const char *e = getenv("DM_GAME_ENV"); if (!e) e = DM_GAME_ENV;
    int n = 0; for (const char *p = e; *p; p++) n += *p == '=';
    if (!(g_env_kv = calloc((size_t)n + 1, sizeof *g_env_kv))) die("game_env allocation failed");
    char *s = strdup(e); int k = 0;
    for (char *t = strtok(s, " "); t && k < n; t = strtok(NULL, " ")) if (strchr(t, '=')) g_env_kv[k++] = t;
    if (k) LOG("game environment (%s): %s", getenv("DM_GAME_ENV") ? "DM_GAME_ENV" : "game profile game_env", e);
}
static const char *const *game_env(void) { static pthread_once_t once = PTHREAD_ONCE_INIT; pthread_once(&once, game_env_init); return g_env_kv; }
HLE_FORCE("getenv", "the game profile's game_env answers its names; the SDK's own getenv (an empty environ on Horizon) the rest", char *, g_getenv, const char *name) {
    static char *(*orig)(const char *);
    size_t n = name ? strlen(name) : 0;
    for (const char *const *p = game_env(); n && *p; p++) if (!strncmp(*p, name, n) && (*p)[n] == '=') return (char *)*p + n + 1;
    if (!orig && !(orig = guest_sym("getenv"))) return NULL;
    return orig(name);
}

// ---------- nn::mem::StandardAllocator::Allocate(size): Vanillaware's large blocks aligned to 64 KB (profile key alloc_big_align) ----------
// 13 Sentinels 1.10's own malloc (main 4b632573…, main+0x310550) is StandardAllocator::Allocate(size). It gets the storage of a GPU memory pool from malloc rounded only to 4 KB (main+0x3073b8),
//   then cuts three ring heaps out of the pool at 64 KB alignment relative to the GPU address (main+0x30c118), and the pool size is exactly their sum (main+0x307adc). On the Switch a pool's GPU address is aligned to a big page;
//   the host's GPU address is the CPU address, which is only 4 KB aligned, so up to 60 KB is lost at the start, the third ring heap (16 MB, used by the main thread's allocations while a command buffer is recorded) does not fit and keeps base 0,
//   and the first allocation while recording writes through a null pointer (main+0x2fa9a0, when the prologue enters its first battle).
// Unicorn Overlord 1.05 (main c841ffe2…), the same engine: operator new (main+0x5a55d4) takes the calling thread's ring heap (bump allocator main+0x5b8504: cursor +0x20 starts at the base) and one kept base 0.
//   Past the title screen the first allocation from it returned 0, which operator new's null check skipped, and the next returned 0x200; its constructor (main+0x5a3224 via main+0x4c754) wrote to 0x268.
//   With the profile key, allocations of 64 KB or more are aligned to 64 KB (Allocate(size, 0x10000)), which puts the pool storage on a 64 KB boundary. Other games forward unchanged
static void *(*g_sa_alloc)(void *, size_t), *(*g_sa_alloc_al)(void *, size_t, size_t);
static int g_sa_big_align;
static pthread_once_t g_sa_once = PTHREAD_ONCE_INIT;
static void sa_init(void) {
    module_t *ms = NULL; for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "sdk")) ms = &g_mod[i];
    g_sa_alloc = ms ? module_sym(ms, "_ZN2nn3mem17StandardAllocator8AllocateEm") : NULL;
    g_sa_alloc_al = ms ? module_sym(ms, "_ZN2nn3mem17StandardAllocator8AllocateEmm") : NULL;
    if (!g_sa_alloc || !g_sa_alloc_al) die("StandardAllocator::Allocate not found in the SDK");
    g_sa_big_align = DM_GAME_ALLOC_BIG_ALIGN;
    if (g_sa_big_align) LOG("StandardAllocator::Allocate: allocations ≥ 64 KB are 64 KB aligned (game profile alloc_big_align: memory pool storage, see hle_os.c)");
}
HLE_FORCE("_ZN2nn3mem17StandardAllocator8AllocateEm", "passed straight to the SDK; with the profile's alloc_big_align (13 Sentinels, Unicorn Overlord) allocations ≥ 64 KB are 64 KB aligned (memory pool start)", void *, h_mem_SAAllocate, void *self, size_t size) {
    pthread_once(&g_sa_once, sa_init);
    return g_sa_big_align && size >= 0x10000 ? g_sa_alloc_al(self, size, 0x10000) : g_sa_alloc(self, size);
}
