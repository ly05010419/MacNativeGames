/* CPU only: ordering race between "set read-only" (nvn_dirty_protect) and "unprotect before a host write" (prep_write) in
   the dirty-page tracker, adapted from D2R Mobile dm/gfx/tests/t_dirty_race.c (upstream commit ef9b1ff).
   Thread A protects two pages; the mprotect that sets them read-only is held back until thread B has run prep_write on
   the first page (the unprotect the host does before reading a file into texture storage). With the old order (state set
   to PROT first, mprotect later) B set the state back to NONE and A's late mprotect left the page read-only with state
   NONE: a host read(2) into it failed with EFAULT and a guest write spun in the fault handler forever.
   Expected values do not depend on the tracker: after B returns, a read(2) of 16 bytes into the page returns 16, a direct
   write reads back, and the write counts as dirty against a stamp taken before the protect. The second page, which nobody
   unprotected, is written through the fault handler (control). TOTK_PROBE_NEGATIVE=1 skips B's prep_write: the page then
   stays read-only and the read(2) must fail. */
#define mprotect t_mprotect
#include "../../third_party/d2r_gfx/gfx/nvn/nvn_dirty.c"
#undef mprotect
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int mprotect(void *, size_t, int);

static int (*g_fault)(void *, int, void *);
static void (*g_prep)(void *, size_t);
void dm_fault_hook_register(int (*fn)(void *, int, void *)) { g_fault = fn; }
void dm_guest_write_prep_register(void (*fn)(void *, size_t)) { g_prep = fn; }
static void on_signal(int sig, siginfo_t *si, void *uc) {   // what the runtime's fault handler does for the tracker: re-execute or crash
    if (g_fault && g_fault(si->si_addr, 1, uc)) return;
    signal(sig, SIG_DFL);
}

static int g_fail, g_checks;
#define EXPECT(name, want, got) do { long long w_ = (long long)(want), g_ = (long long)(got); g_checks++; \
    printf("%-62s expected %-4lld got %-4lld %s\n", name, w_, g_, w_ == g_ ? "ok" : "FAIL"); if (w_ != g_) g_fail++; } while (0)

static uint8_t *g_page; static _Atomic int g_armed, g_go, g_b_done; static int g_negative;
static void *thread_b(void *arg) {
    (void)arg; while (!atomic_load(&g_go)) usleep(200);
    if (!g_negative) g_prep(g_page, 16);
    atomic_store(&g_b_done, 1); return NULL;
}
int t_mprotect(void *a, size_t n, int prot) {   // A's read-only mprotect lands only after B had 100 ms to run
    if (prot == PROT_READ && atomic_exchange(&g_armed, 0)) { atomic_store(&g_go, 1); usleep(100000); }
    return mprotect(a, n, prot);
}

/* #90 (glyph cache letters missing): nvn_dirty_protect meets a page another thread is unprotecting, with the mprotect to
   writable done and the new generation stored but the state still BUSY. Skipping that page left it writable with a
   generation the caller's stamp covers, so later writes did not fault and were never reported. Thread C plays the last
   step of that unprotect 50 ms late. "No write yet" must still be clean (control). */
static _Atomic uint8_t *g_c_state;
static void *thread_c(void *arg) { (void)arg; usleep(50000); atomic_store(g_c_state, PG_NONE); return NULL; }
static int busy_protect(size_t pg) {
    uint8_t *page = mmap(NULL, pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (page == MAP_FAILED) { perror("mmap"); return 1; }
    nvn_dirty_add_range(page, pg);
    range_t *r = find((uintptr_t)page);
    nvn_dirty_protect(page, pg);
    uint8_t st = PG_PROT;
    if (!r || !atomic_compare_exchange_strong(&r->state[0], &st, PG_BUSY)) { printf("dirty race probe: page not protected\n"); return 1; }
    mprotect(page, pg, PROT_READ | PROT_WRITE);   /* what unprotect_page does before it stores the generation */
    uint64_t g = atomic_fetch_add(&g_now, 1) + 1; atomic_store(&r->gen[0], g); atomic_store(&r->blk[0], g); atomic_store(&r->latest, g);
    g_c_state = &r->state[0];
    pthread_t tc; pthread_create(&tc, NULL, thread_c, NULL);
    uint64_t stamp = nvn_dirty_now();   /* the uploader's stamp, then its protect */
    nvn_dirty_protect(page, pg);
    pthread_join(tc, NULL);
    EXPECT("#90: protect waited out a page being unprotected (state PROT)", PG_PROT, atomic_load(&r->state[0]));
    EXPECT("#90: no write after the stamp yet: clean (control)", 0, nvn_dirty_since(page, 16, stamp));
    page[0] = 7;   /* a write after the upload copied the page */
    EXPECT("#90: a later write to that page is seen", 1, nvn_dirty_since(page, 16, stamp));
    return 0;
}
/* #90: why pkg_upload_ / wb_note take the stamp before nvn_dirty_protect, and tex_mtl's "nothing written since" shortcut
   starts from that stamp. A write that faults while the pages are being protected or copied gets a generation above a
   stamp taken before, and the page is writable from then on; a stamp taken after covers it, so the copy can miss that
   write and every later one (second check: the old order, expected to miss). */
static void stamp_order(size_t pg) {
    uint8_t *page = mmap(NULL, pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (page == MAP_FAILED) { perror("mmap"); g_fail++; return; }
    nvn_dirty_add_range(page, pg);
    uint64_t before = nvn_dirty_now();
    nvn_dirty_protect(page, pg);
    page[0] = 1;   /* faults during the upload */
    uint64_t after = nvn_dirty_now();
    page[0] = 2;   /* after the copy: no fault, the page is writable */
    EXPECT("#90: stamp before protect: the writes are seen", 1, nvn_dirty_since(page, 16, before));
    EXPECT("#90: stamp after protect (old order): the writes are missed", 0, nvn_dirty_since(page, 16, after));
}

/* TOTK_DIRTY_FAST: a 300-page pool with writes on three pages (two in one 64-page block, one alone in another); every
   window answers the same with the per-block maxima as with the page walk, and a range lookup through the per-thread
   page table finds the range the list scan finds, also after the range's slot was reused for another pool. */
static void block_summary(size_t pg) {
    enum { NP = 300 };
    uint8_t *m = mmap(NULL, NP * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (m == MAP_FAILED) { perror("mmap"); g_fail++; return; }
    nvn_dirty_add_range(m, NP * pg);
    uint64_t s = nvn_dirty_now(); nvn_dirty_protect(m, NP * pg);
    m[5 * pg] = 1; m[70 * pg + 3] = 1; m[100 * pg] = 1; uint64_t mid = nvn_dirty_now(); m[250 * pg] = 1;
    static const int win[][2] = {{0, 5}, {0, 6}, {6, 70}, {6, 71}, {71, 100}, {64, 128}, {101, 250}, {101, 251}, {128, 192}, {251, 300}, {0, 300}, {192, 250}};
    int same = 0, n = 0, fast_dirty = 0;
    for (size_t k = 0; k < sizeof win / sizeof win[0]; k++) for (int st = 0; st < 2; st++) {
        uint64_t stamp = st ? mid : s; const void *a = m + (size_t)win[k][0] * pg; size_t len = (size_t)(win[k][1] - win[k][0]) * pg;
        g_dirty_fast = 1; int f = nvn_dirty_since(a, len, stamp); g_dirty_fast = 0; int w = nvn_dirty_since(a, len, stamp); g_dirty_fast = 1;
        same += f == w; n++; fast_dirty += f;
    }
    EXPECT("TOTK_DIRTY_FAST: block maxima answer as the page walk (24 windows)", n, same);
    EXPECT("TOTK_DIRTY_FAST: ... of which dirty", 7, fast_dirty);
    uint64_t skips0 = atomic_load(&g_blk_skips); (void)nvn_dirty_since(m + 128 * pg, 64 * pg, s);
    EXPECT("TOTK_DIRTY_FAST: an untouched block is skipped", 1, atomic_load(&g_blk_skips) - skips0);
    range_t *r1 = find((uintptr_t)m + 7 * pg);
    nvn_dirty_remove_range(m); nvn_dirty_add_range(m + 150 * pg, 10 * pg);   // the slot goes to a pool over other pages
    g_dirty_fast = 0; range_t *scan_a = find((uintptr_t)m + 7 * pg), *scan_b = find((uintptr_t)m + 155 * pg); g_dirty_fast = 1;
    EXPECT("TOTK_DIRTY_FAST: page table after the slot was reused: no stale range", (long long)(uintptr_t)scan_a, (long long)(uintptr_t)find((uintptr_t)m + 7 * pg));
    EXPECT("TOTK_DIRTY_FAST: page table finds the new pool's range", (long long)(uintptr_t)scan_b, (long long)(uintptr_t)find((uintptr_t)m + 155 * pg));
    EXPECT("TOTK_DIRTY_FAST: the first lookup found the pool", 1, r1 != NULL);
    nvn_dirty_remove_range(m + 150 * pg);
}

#if defined(GAME_BOTW)
/* BOTW: pool B starts on pool A's last page. B gets a range for its own pages only; a texture of B across the border is
   tracked through both ranges; destroying B keeps A's range (the Hero's Path bug), destroying A first leaves B's texture
   untracked (dirty) and A's protected page writable; a pool inside A has no range and its destruction leaves A alone. */
static void overlap(size_t pg) {
    uint8_t *R = mmap(NULL, 6 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (R == MAP_FAILED) { perror("mmap"); exit(2); }
    uint8_t *A = R, *B = R + 2 * pg + pg / 2, *C = R + pg / 2; size_t as = 2 * pg + pg / 2, bs = 2 * pg, ts = pg + pg / 2;
    nvn_dirty_add_range(A, as); nvn_dirty_add_range(B, bs);
    EXPECT("BOTW: B's texture across A's last page is tracked", 1, nvn_dirty_tracked(B, ts));
    uint64_t s = nvn_dirty_now(); nvn_dirty_protect(B, ts);
    EXPECT("BOTW: no write yet: clean", 0, nvn_dirty_since(B, ts, s));
    R[3 * pg + 8] = 1;   // B's own page, through the fault handler
    EXPECT("BOTW: a write to B's own page is seen", 1, nvn_dirty_since(B, ts, s));
    s = nvn_dirty_now(); nvn_dirty_protect(B, ts);
    R[2 * pg + 8] = 1;   // the shared page, in A's range
    EXPECT("BOTW: a write to the shared page is seen", 1, nvn_dirty_since(B, ts, s));
    EXPECT("BOTW: ... and A's own texture there too", 1, nvn_dirty_since(A + 2 * pg, 16, s));
    nvn_dirty_add_range(C, pg / 4); nvn_dirty_remove_range(C);
    EXPECT("BOTW: a pool inside A gone: A still tracked", 1, nvn_dirty_tracked(A, as));
    nvn_dirty_remove_range(B);
    EXPECT("BOTW: B gone: A still tracked", 1, nvn_dirty_tracked(A, as));
    EXPECT("BOTW: B gone: its own pages untracked", 0, nvn_dirty_tracked(R + 3 * pg, 16));
    nvn_dirty_add_range(B, bs); s = nvn_dirty_now(); nvn_dirty_protect(B, ts);
    nvn_dirty_remove_range(A);
    R[2 * pg + 8] = 2;   // writable again (a read-only page with no range would crash here)
    EXPECT("BOTW: A gone first: the shared page is writable", 2, R[2 * pg + 8]);
    EXPECT("BOTW: A gone first: B's texture across it is untracked", 0, nvn_dirty_tracked(B, ts));
    EXPECT("BOTW: ... and counts as dirty", 1, nvn_dirty_since(B, ts, s));
    EXPECT("BOTW: B's own pages still tracked", 1, nvn_dirty_tracked(R + 3 * pg, pg));
    nvn_dirty_remove_range(B);
}
#endif
int main(void) {
    g_negative = getenv("TOTK_PROBE_NEGATIVE") != NULL;
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = on_signal; sa.sa_flags = SA_SIGINFO; sigemptyset(&sa.sa_mask);
    sigaction(SIGBUS, &sa, NULL); sigaction(SIGSEGV, &sa, NULL);
    size_t pg = (size_t)getpagesize();
    g_page = mmap(NULL, 2 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (g_page == MAP_FAILED) { perror("mmap"); return 2; }
    nvn_dirty_add_range(g_page, 2 * pg);
    if (!g_fault || !g_prep) { printf("dirty race probe: the tracker did not register its hooks\n"); return 2; }
    uint64_t stamp = nvn_dirty_now();
    pthread_t tb; pthread_create(&tb, NULL, thread_b, NULL);
    atomic_store(&g_armed, 1);
    nvn_dirty_protect(g_page, 2 * pg);   // A: state -> (B runs) -> mprotect lands
    for (int i = 0; i < 2000 && !atomic_load(&g_b_done); i++) usleep(1000);
    EXPECT("B's unprotect returns within 2 s", 1, atomic_load(&g_b_done));
    if (!atomic_load(&g_b_done)) { printf("dirty race probe: B did not return\n"); return 1; }
    pthread_join(tb, NULL);
    g_page[pg] = 0x11;   // control: the second page is written through the fault handler
    EXPECT("write to the other page goes through the fault handler", 0x11, g_page[pg]);
    int fd = open("/dev/zero", O_RDONLY); ssize_t r = read(fd, g_page, 16); int err = errno; close(fd);
    if (g_negative) {
        EXPECT("negative control: host read into the still read-only page fails", -1, r);
        printf("dirty race probe (negative control): errno %d, %s\n", err, g_fail ? "FAILED" : "detected as expected");
        return g_fail ? 1 : 3;
    }
    EXPECT("host read(2) into the page after B: bytes read", 16, r);
    if (r == 16) { ((volatile uint8_t *)g_page)[1] = 0x5a; EXPECT("direct write to the page reads back", 0x5a, g_page[1]); }
    else { printf("  read failed, errno %d: the page is read-only with state NONE (a direct write would spin)\n", err); g_fail++; }
    EXPECT("the write counts as dirty against the earlier stamp", 1, nvn_dirty_since(g_page, 16, stamp));
    /* Capacity (2026-10-17: 4096 tracked pools were full in long TOTK games, the rest went untracked): 6,000 more pools are
       all tracked, a write into the last one is seen, and a write into the first is still seen after the lookups in between. */
    enum { MANY = 6000 };
    uint8_t *many = mmap(NULL, (size_t)MANY * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (many == MAP_FAILED) { perror("mmap"); return 2; }
    for (int i = 0; i < MANY; i++) nvn_dirty_add_range(many + (size_t)i * pg, pg);
    int tracked = 0; for (int i = 0; i < MANY; i++) tracked += nvn_dirty_tracked(many + (size_t)i * pg, 16) != 0;
    EXPECT("6,000 more pools: all tracked", MANY, tracked);
    uint64_t s2 = nvn_dirty_now(); for (int i = 0; i < MANY; i++) nvn_dirty_protect(many + (size_t)i * pg, pg);   /* one pool at a time, as the runtime does */
    many[(size_t)(MANY - 1) * pg] = 1; many[0] = 1;
    EXPECT("a write into the last of them is seen", 1, nvn_dirty_since(many + (size_t)(MANY - 1) * pg, 16, s2));
    EXPECT("a write into the first of them is seen", 1, nvn_dirty_since(many, 16, s2));
    EXPECT("an untouched one in between is clean", 0, nvn_dirty_since(many + (size_t)(MANY / 2) * pg, 16, s2));
    if (busy_protect(pg)) return 2;
    stamp_order(pg);
    block_summary(pg);
#if defined(GAME_BOTW)
    overlap(pg);
#endif
    printf("{\"status\":\"%s\",\"checks\":%d,\"failed\":%d}\n", g_fail ? "fail" : "pass", g_checks, g_fail);
    return g_fail != 0;
}
