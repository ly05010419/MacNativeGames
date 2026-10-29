// 客体内存写保护脏页跟踪：预排布贴图（CPU 写、GPU 读）的池页上传后设只读，游戏一写即缺页 → 标脏、解保护；
// 使用前查覆盖页的写代号，比上传时新就重传。NVN 语义：贴图描述符指向池内存，GPU 每次读的是当前内容
// （实测游戏会把流式素材写进仍被采样的贴图存储，不重新登记）。
// 信号处理约束：缺页路径只用原子操作与 mprotect，不分配、不加锁。
// Write-protect dirty-page tracking for guest memory: pool pages of packaged textures (CPU writes, GPU reads) are set read-only after upload, and as soon as the game writes it faults -> mark dirty, unprotect;
// before use, check the write generation of the covered pages and re-upload if newer than at upload time. NVN semantics: a texture descriptor points to pool memory, and the GPU reads the current content every time
// (measured: the game writes streamed assets into texture storage that is still being sampled, without re-registering).
// Signal handler constraint: the page-fault path uses only atomic operations and mprotect, no allocation, no locks.
#include "nvn_dirty.h"
#include "../../host/dm.h"
#include <stdatomic.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#include <mach/mach_time.h>
#include <sched.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>

// 页状态与 mprotect 的次序（改保护的一方先把状态改成 BUSY，mprotect 落地后才改成终态；别的线程见 BUSY 就等）：
//   设只读：NONE → BUSY → mprotect(只读) → PROT；解保护：PROT → BUSY → mprotect(可写) → NONE。
// 不变式：状态是 NONE 的页一定可写、是 PROT 的页一定只读。原先设只读是「先记 PROT、攒一段再 mprotect」，这段时间里另一线程的写前解保护
// 会把状态改回 NONE，随后攒批的 mprotect 落地 ⇒ 页只读而状态 NONE：游戏线程写它时缺页处理只回「重执行」永远空转；
// 宿主用系统调用往里读文件时 prep_write 看状态 NONE 跳过，系统调用不走缺页处理，直接 EFAULT、数据读不全。
// BUSY 最长持续 = 一次 mprotect 的时间（设只读时再加上给这一段逐页改状态的几次原子写）。等待有上限 BUSY_WAIT_MS，超限报红：
// 缺页处理里超限按「不是我们的页」返回，交给崩溃处理如实报崩，不静默空转。等待只读原子量、不拿任何锁。
// Order of page state and mprotect (the side changing protection first sets the state to BUSY and sets the final state only after mprotect has landed; other threads wait when they see BUSY):
//   set read-only: NONE -> BUSY -> mprotect(read-only) -> PROT; unprotect: PROT -> BUSY -> mprotect(writable) -> NONE.
// Invariant: a page whose state is NONE is always writable, and one whose state is PROT is always read-only. Setting read-only used to be "record PROT first, batch up a run, then mprotect"; during that time another thread's unprotect-before-write
// could set the state back to NONE, and then the batched mprotect landed => the page read-only with state NONE: when the game thread wrote to it the page-fault handler only returned "re-execute" and spun forever;
// when the host read a file into it with a system call, prep_write saw state NONE and skipped, the system call did not go through the page-fault handler and failed with EFAULT, leaving the data incomplete.
// BUSY lasts at most one mprotect (plus, when setting read-only, the few atomic writes that change the state of the pages in the run). The wait has an upper limit BUSY_WAIT_MS and reports red beyond it:
// in the page-fault handler, exceeding it returns "not our page" and lets the crash handler report the crash truthfully instead of spinning silently. The wait only reads atomics and takes no lock.
enum { PG_NONE = 0, PG_PROT = 1, PG_BUSY = 2 };
enum { BUSY_WAIT_MS = 2000 };
// 跟踪区间 = 各内存池（MemoryPoolInitialize 登记）。池销毁后复用 live=0 的槽，信号处理里无锁扫描。
// Tracked ranges = the memory pools (registered by MemoryPoolInitialize). After a pool is destroyed its live=0 slot is reused; the signal handler scans without locks.
#define MAX_RANGE 16384 // 原为 256：内存池多于它时超出的池不跟踪，里面的贴图每次取用都当成脏的重传（暗黑实测峰值见内存读数行的「内存池」；塞尔达约 300 个，踩过）。扫描只到已用的槽数，容量大不增加开销 | was 256: with more memory pools than that the excess pools are untracked and their textures are treated as dirty and re-uploaded on every use (for Diablo's measured peak see "memory pools" in the memory readout line; Zelda has about 300 and hit this). The scan only covers slots in use, so a larger capacity costs nothing. TOTK (2026-10-17): 4096 was full in the user's own games and most long test games (about 3,900 live pools in the world, more when moving between regions); now 16384
typedef struct { uintptr_t base, end; _Atomic uint8_t *state; _Atomic uint64_t *gen; size_t cap; _Atomic int live; _Atomic uint64_t latest; _Atomic uint64_t *blk;
                 _Atomic uint64_t *pl_wf, *pl_wfb; _Atomic uint32_t *pl_wfr, *pl_pfr, *pl_psk; _Atomic uint8_t *pl_pc; size_t pl_cap; } range_t;   // pl_*: TOTK_PIPELINE per-page state (see "one frame deeper" below)   // latest (TOTK): highest page generation in the range; blk: the same per block of BLK_PAGES pages
/* TOTK_DIRTY_FAST (default on; =0 off), 2026-10-23. Two lookups the replay makes for every CPU-mirrored texture at its
   check in each submission (tex_mtl: about 2.5% of the translate thread in the Korok Forest profile):
   - nvn_dirty_since walked every page of the texture whenever anything else in its pool had been written since the stamp
     (the big material pools are written by streaming all the time; a 42 MB texture is 2,600 pages). blk[] keeps the
     highest page generation per block of BLK_PAGES pages, raised by unprotect_page after the page's own generation and
     before the range's latest, so a block whose maximum is not above the stamp holds no page that is: it is skipped, and
     the answer is the page walk's.
   - find() scanned the ~4,000 live ranges whenever the address was not in one of the thread's last 8 ranges. A per-thread
     table by page of the address answers first; an entry counts only if its range is live and holds the address, as for
     the recent list, and live ranges never share a page, so it is the range the scan would find. */
enum { BLK_SHIFT = 6, BLK_PAGES = 1 << BLK_SHIFT, FIND_HASH = 1024 };
static int g_dirty_fast = 1;   // set once, before the first range exists (nvn_dirty_add_range)
static _Atomic uint64_t g_blk_skips, g_find_hash_hits, g_find_scans;
static inline size_t blk_count(size_t np) { return (np + BLK_PAGES - 1) >> BLK_SHIFT; }
#if defined(GAME_BOTW)
/* BOTW: a pool that shares host pages with live ranges gets ranges for its other pages (the gaps), so ranges never share
   a page and the fault handler still finds at most one range per page. Before, such a pool got no range: its textures
   counted as untracked and were uploaded again at every submission (one 256x32 texture, 1 MB a frame). A texture across a
   range border is looked at range by range. g_rmem is the pool that owns each range: a pool is destroyed through its own
   address, which can lie in another pool's range (it starts on that pool's last page), so the address lookup dropped the
   other pool's range (after the map's Hero's Path playback about 350 uploads, 130 MB a frame, 33 ms of submit CPU).
   g_rdying: set before the removal unprotects the range, so a texture of a neighbour pool that shares a page with it does
   not protect that page meanwhile (a page left read-only in a dead range would fault with no range to find). */
static uintptr_t g_rmem[MAX_RANGE];
static _Atomic uint8_t g_rdying[MAX_RANGE];
static _Atomic uint64_t g_ov_inside, g_ov_split, g_ov_same, g_ov_many, g_rm_none;
#endif
static range_t g_r[MAX_RANGE];
static _Atomic int g_nr;
static uintptr_t g_pg, g_shift;
static _Atomic uint64_t g_now = 1;
static _Atomic uint64_t g_faults, g_preps;
static _Atomic uint64_t g_adds_total;
static _Atomic int g_live, g_live_peak;   // 在册的池数与峰值（内存读数行用） | number of registered pools and peak (for the memory readout line)
void nvn_dirty_stat(int *live, int *peak, int *cap) { *live = atomic_load(&g_live); *peak = atomic_load(&g_live_peak); *cap = MAX_RANGE; }
static _Atomic int g_overflow_logged;
static pthread_mutex_t g_slot_mu = PTHREAD_MUTEX_INITIALIZER;   // 建池 / 销池（客体线程，可能并发）互斥选槽；缺页处理路径不碰它 | Pool creation / destruction (guest threads, possibly concurrent) pick slots under a mutex; the page-fault handling path does not touch it
static _Atomic uint64_t g_mp_calls, g_mp_pages, g_mp_ticks;   // 本模块的 mprotect：调用次数、覆盖页数、耗时（mach 时基，缺页路径里也安全） | This module's mprotect: call count, pages covered, time spent (mach timebase, safe in the page-fault path too)
static void mp(uintptr_t a, size_t len, int prot) {
    uint64_t t = mach_absolute_time(); mprotect((void *)a, len, prot);
    atomic_fetch_add(&g_mp_ticks, mach_absolute_time() - t); atomic_fetch_add(&g_mp_calls, 1); atomic_fetch_add(&g_mp_pages, len >> g_shift);
}

static mach_timebase_info_data_t g_tb;   // 首次登记池时取好（缺页路径里只做乘除） | Obtained when a pool is first registered (the fault path only multiplies and divides)
static _Atomic int g_busy_red;
// 等这一页离开 BUSY。返回 1 = 已离开；0 = 等过了上限（报红一次）。信号处理里可用：只有原子读、mach_absolute_time、sched_yield
// Wait for this page to leave BUSY. Returns 1 = it has left; 0 = waited past the limit (reported red once). Usable in a signal handler: only atomic reads, mach_absolute_time and sched_yield
static int busy_wait(range_t *r, uintptr_t p) {
    uint64_t t0 = 0;
    for (unsigned n = 1; atomic_load(&r->state[p]) == PG_BUSY; n++) {
        if (n & 0x3ff) continue;
        uint64_t now = mach_absolute_time(); if (!t0) t0 = now;
        if ((now - t0) * g_tb.numer / g_tb.denom > (uint64_t)BUSY_WAIT_MS * 1000000ull) {
            if (!atomic_exchange(&g_busy_red, 1)) LOG("【红】脏页跟踪：页 %p 停在「忙」超过 %d 毫秒（改保护的线程没走完），不再等", (void *)(r->base + (p << g_shift)), BUSY_WAIT_MS);
            return 0; }
        sched_yield();
    }
    return 1;
}
/* Sorted index of the live ranges (by base), rebuilt under g_slot_mu whenever a range goes live or dies, published by
   pointer; a lookup that misses the per-thread caches is a binary search instead of a scan of the ~4,000 live ranges
   (2026-10-23: the deeper frame wait looked up many pages and untracked addresses: find was 34% of the translate thread).
   An old index is freed only when no lookup is inside one (g_rix_readers: a lookup counts itself before it loads the
   pointer, so a rebuild that sees no reader after the swap knows nobody holds an old index); until then it waits in a
   retire list (freed at a later rebuild). */
typedef struct { int n; uintptr_t *base; range_t **r; } rindex_t;
static rindex_t *_Atomic g_rix; static rindex_t *g_rix_retired[64]; static int g_rix_nret; static _Atomic int g_rix_readers; static _Atomic uint64_t g_rgen;
static void rix_free(rindex_t *x) { if (x) { free(x->base); free(x->r); free(x); } }
static int rix_cmp(const void *x, const void *y) { const range_t *a = *(range_t *const *)x, *b = *(range_t *const *)y; return a->base < b->base ? -1 : a->base > b->base; }
static void rindex_rebuild(void) {   // under g_slot_mu
    int n = atomic_load(&g_nr), k = 0;
    rindex_t *x = malloc(sizeof *x); range_t **r = malloc(sizeof *r * (size_t)(n ? n : 1)); uintptr_t *b = malloc(sizeof *b * (size_t)(n ? n : 1));
    if (!x || !r || !b) { free(x); free(r); free(b); atomic_store(&g_rix, NULL); atomic_fetch_add(&g_rgen, 1); return; }   // lookups scan
    for (int i = 0; i < n; i++) if (atomic_load_explicit(&g_r[i].live, memory_order_acquire)) r[k++] = &g_r[i];
    qsort(r, (size_t)k, sizeof *r, rix_cmp);
    for (int i = 0; i < k; i++) b[i] = r[i]->base;
    x->n = k; x->base = b; x->r = r;
    rindex_t *old = atomic_exchange(&g_rix, x);
    if (old) {
        if (g_rix_nret == 64) { while (atomic_load(&g_rix_readers)) sched_yield(); for (int i = 0; i < g_rix_nret; i++) rix_free(g_rix_retired[i]); g_rix_nret = 0; }   // full: wait for a quiet moment
        g_rix_retired[g_rix_nret++] = old;
    }
    if (!atomic_load(&g_rix_readers)) { for (int i = 0; i < g_rix_nret; i++) rix_free(g_rix_retired[i]); g_rix_nret = 0; }   // nobody can hold a retired index now
    atomic_fetch_add(&g_rgen, 1);
}
uint64_t nvn_dirty_range_gen(void) { return atomic_load(&g_rgen); }
static range_t *find(uintptr_t a) {
    /* TOTK: the replay asks about the same few pools over and over; the last hits per thread (8, round robin) are checked
       first (a slot reused for another pool fails the bounds or live test and falls through to the scan), so the scan over
       the ~4,000 live pools of the world mostly runs for the first touch of a pool. */
    static __thread range_t *recent[8]; static __thread unsigned next;
    static __thread range_t *hashed[FIND_HASH];   // TOTK_DIRTY_FAST: by page of the address
    for (int k = 0; k < 8; k++) {
        range_t *l = recent[k];
        if (l && atomic_load_explicit(&l->live, memory_order_acquire) && a >= l->base && a < l->end) return l;
    }
    unsigned h = 0;
    if (g_dirty_fast) {
        uintptr_t pg = a >> g_shift; h = (unsigned)(pg ^ (pg >> 10) ^ (pg >> 20)) & (FIND_HASH - 1);
        range_t *l = hashed[h];
        if (l && atomic_load_explicit(&l->live, memory_order_acquire) && a >= l->base && a < l->end) { recent[next++ & 7] = l; atomic_fetch_add_explicit(&g_find_hash_hits, 1, memory_order_relaxed); return l; }
        atomic_fetch_add_explicit(&g_find_scans, 1, memory_order_relaxed);
    }
    if (g_dirty_fast) {
        atomic_fetch_add(&g_rix_readers, 1);
        rindex_t *x = atomic_load_explicit(&g_rix, memory_order_acquire);
        if (x) {   // the last range starting at or below a
            int lo = 0, hi = x->n; range_t *found = NULL;
            while (lo < hi) { int m = (lo + hi) / 2; if (x->base[m] <= a) lo = m + 1; else hi = m; }
            for (int i = lo - 1; i >= 0 && i >= lo - 2 && !found; i--) {   // (a range rebuilt in place may sit one entry off)
                range_t *l = x->r[i];
                if (atomic_load_explicit(&l->live, memory_order_acquire) && a >= l->base && a < l->end) found = l;
            }
            int same = atomic_load(&g_rix) == x;
            atomic_fetch_sub(&g_rix_readers, 1);
            if (found) { recent[next++ & 7] = found; hashed[h] = found; return found; }
            if (same) return NULL;   // not in a live range (the index did not change meanwhile)
        } else atomic_fetch_sub(&g_rix_readers, 1);
    }
    int n = atomic_load(&g_nr);
    for (int i = 0; i < n; i++) if (atomic_load_explicit(&g_r[i].live, memory_order_acquire) && a >= g_r[i].base && a < g_r[i].end) { recent[next++ & 7] = &g_r[i]; if (g_dirty_fast) hashed[h] = &g_r[i]; return &g_r[i]; }
    return NULL;
}
static void dirty_fast_init(void) { const char *e = getenv("TOTK_DIRTY_FAST"); g_dirty_fast = !(e && !atoi(e)); if (!g_dirty_fast) LOG("TOTK_DIRTY_FAST=0: dirty checks walk every page, range lookups scan the list"); }
/* Pages [p, q] of r: one written after stamp? (TOTK_DIRTY_FAST: blocks whose maximum is not above the stamp are skipped.) */
static int pages_since(range_t *r, uintptr_t p, uintptr_t q, uint64_t stamp) {
    if (!g_dirty_fast) { for (uintptr_t i = p; i <= q; i++) if (atomic_load(&r->gen[i]) > stamp) return 1; return 0; }
    uint64_t skipped = 0;
    for (uintptr_t i = p; i <= q;) {
        uintptr_t last = ((i >> BLK_SHIFT) << BLK_SHIFT) + BLK_PAGES - 1; if (last > q) last = q;
        if (atomic_load(&r->blk[i >> BLK_SHIFT]) <= stamp) { i = last + 1; skipped++; continue; }
        for (; i <= last; i++) if (atomic_load(&r->gen[i]) > stamp) { if (skipped) atomic_fetch_add_explicit(&g_blk_skips, skipped, memory_order_relaxed); return 1; }
    }
    if (skipped) atomic_fetch_add_explicit(&g_blk_skips, skipped, memory_order_relaxed);
    return 0;
}
void nvn_dirty_fast_stats(int *on, uint64_t *blk_skips, uint64_t *hash_hits, uint64_t *scans) {
    *on = g_dirty_fast; *blk_skips = atomic_load(&g_blk_skips); *hash_hits = atomic_load(&g_find_hash_hits); *scans = atomic_load(&g_find_scans);
}
/* TOTK diagnostic (on with DM_NVN_TILELOG): a ring of the latest page unprotections with the writer, read by
   nvn_dirty_recent: game writes (fault: pc / lr of the faulting code) and host writes (dm_guest_write_prepare caller). */
#include <sys/ucontext.h>
static nvn_dirty_rec g_drec[4096]; static _Atomic uint64_t g_drec_n; static int g_drec_on;
uint64_t dm_host_write_pc;   // set by dm_guest_write_prepare (runtime/gfx_bridge.c) before calling prep_write
static void drec_add(uintptr_t page, uint64_t pc, uint64_t lr, uint32_t src) {
    if (!g_drec_on) return;
    uint64_t i = atomic_fetch_add(&g_drec_n, 1) % 4096; g_drec[i] = (nvn_dirty_rec){ page, pc, lr, src };
}
int nvn_dirty_recent(uintptr_t lo, uintptr_t hi, nvn_dirty_rec *out, int max) {
    uint64_t n = atomic_load(&g_drec_n); int k = 0;
    for (uint64_t j = n > 4096 ? n - 4096 : 0; j < n && k < max; j++) { nvn_dirty_rec r = g_drec[j % 4096]; if (r.page >= lo && r.page < hi) out[k++] = r; }
    return k;
}
/* ---------- TOTK_PIPELINE: one frame deeper (nvn_mtl.m, totk_pipeline.h) ----------
   With TOTK_PIPELINE=1 the game's frame wait for submission k returns once k-1 has completed, so the GPU still runs k
   while the game records k+1 and writes the memory k+1 will use.
   - Per page of every tracked pool: pl_wfr, the frame (nvn_pl_set_frame) of the last CPU write seen (a write fault or a
     host write on a protected page); pl_pfr, the frame the page was last put on probation (write-protected again to see
     whether the CPU still writes it); pl_wf, the newest fence of GPU work writing the page (pl_wfb: its maximum per block
     of BLK_PAGES pages), so a snapshot is never taken over a GPU write in flight.
   - Per submission (nvn_pl_publish): the exact byte ranges its GPU work accesses in place (reads not snapshotted, and
     writes), sorted, with a running maximum of their ends. A CPU write to a protected page waits in the fault handler
     only when the bytes written (the 64-byte line at the faulting address; for an announced host write its span on
     that page) overlap a range of a submission that was released early and has not completed (a "hold": guest threads
     only; signal-safe: atomics, mach_absolute_time, nanosleep; at most PL_HOLD_MS, then it goes ahead as a timeout).
     A write elsewhere on such a page (the game keeps CPU data next to GPU data in a 16 KiB page) goes through and is
     counted ("passes"). Host threads are never held.
   - Pages unprotected by CPU writes are kept in a ring (signal-safe); at a release nvn_pl_reprotect protects again
     those of them inside the released submission's ranges, and every page of its ranges that are not static.
   TOTK_PIPELINE=2 (dry run): nothing waits; the fence of the previous frame wait plays "completed" for one frame and
   the writes that would have been held are counted. The writer's pc / lr of the latest holds are kept (nvn_pl_holds). */
static int g_pl_mode = -1;                    // 0 off, 1 on, 2 dry run (pl_init, before the first range)
static int g_pl_passkeep = 1;                 // TOTK_PIPELINE_PASSKEEP=0: pages whose other part the CPU keeps writing are protected again anyway
static uint64_t (*volatile g_pl_done)(void);  // completed fence (nvn_mtl.m), read signal-safely
static _Atomic uint64_t g_pl_released, g_pl_vdone;
static _Atomic uint32_t g_pl_frame = 1;
static _Atomic uint64_t g_pl_st[NVN_PLS_N];
enum { PL_HOLD_MS = 500, PL_RSETS = 8, PL_URING = 16384 };
static nvn_pl_hold_rec g_pl_hrec[64]; static _Atomic uint64_t g_pl_hrec_n;
typedef struct { _Atomic uint64_t fence; _Atomic int n; uintptr_t *_Atomic lo, *_Atomic hi, *_Atomic mx; int cap; } pl_rset;
static pl_rset g_pl_rs[PL_RSETS], g_pl_ws[PL_RSETS];   // per submission: in-place reads, GPU writes
static int g_pl_wwhold;   // TOTK_PIPELINE_WWHOLD=1: also hold CPU writes that only meet GPU writes (W-W)
static _Atomic uintptr_t g_pl_uring[PL_URING]; static _Atomic uint64_t g_pl_uring_n; static uint64_t g_pl_uring_cur;
static void pl_init(void) {
    if (g_pl_mode >= 0) return;
    /* Off unless asked for (TOTK_PIPELINE=1). Measured on the Mac with an earlier asynchronous preemption (full bench
       +10.6% / +13.8% average, 1% low equal); to be measured again with main's preemption (#488) and TOTK_SYNC_LAG (#680)
       before it is turned on by default. */
    const char *e = getenv("TOTK_PIPELINE"); int m = e ? atoi(e) : 0;
    e = getenv("TOTK_PIPELINE_PASSKEEP"); if (e) g_pl_passkeep = atoi(e) != 0;
    e = getenv("TOTK_PIPELINE_WWHOLD"); if (e) g_pl_wwhold = atoi(e) != 0;
    g_pl_mode = m == 1 || m == 2 ? m : 0;
}
int nvn_pl_mode(void) { pl_init(); return g_pl_mode; }
int nvn_pl_wwhold(void) { pl_init(); return g_pl_wwhold; }   // CPU writes that meet only GPU writes are held (else they pass: their pages need no protection)
void nvn_pl_set_done(uint64_t (*done)(void)) { g_pl_done = done; }
void nvn_pl_set_frame(uint32_t f) { atomic_store(&g_pl_frame, f ? f : 1); }
void nvn_pl_set_released(uint64_t released, uint64_t vdone) {
    if (g_pl_mode == 2) atomic_store(&g_pl_vdone, vdone);
    uint64_t r = atomic_load(&g_pl_released); while (r < released && !atomic_compare_exchange_weak(&g_pl_released, &r, released)) {}
}
static void pl_arrays(range_t *r, size_t np) {   // under g_slot_mu, before the range goes live
    pl_init(); if (!g_pl_mode) return;
    size_t nb = blk_count(np);
    if (r->pl_cap < np) {   // the old arrays are not freed (a signal handler may still read them, as for state / gen)
        _Atomic uint64_t *wf = calloc(np, 8), *wfb = calloc(nb, 8); _Atomic uint32_t *wfr = calloc(np, 4), *pfr = calloc(np, 4), *psk = calloc(np, 4); _Atomic uint8_t *pc = calloc(np, 1);
        if (!wf || !wfb || !wfr || !pfr || !psk || !pc) die("pipeline page tables (%zu pages)", np);
        r->pl_wfb = wfb; r->pl_wfr = wfr; r->pl_pfr = pfr; r->pl_psk = psk; r->pl_pc = pc; r->pl_cap = np; atomic_thread_fence(memory_order_release); r->pl_wf = wf;
    } else { memset((void *)r->pl_wf, 0, np * 8); memset((void *)r->pl_wfb, 0, nb * 8); memset((void *)r->pl_wfr, 0, np * 4); memset((void *)r->pl_pfr, 0, np * 4); memset((void *)r->pl_psk, 0, np * 4); memset((void *)r->pl_pc, 0, np); }
}
static uint64_t pl_done_now(void) {
    if (g_pl_mode == 2) return atomic_load(&g_pl_vdone);
    uint64_t (*f)(void) = g_pl_done; return f ? f() : UINT64_MAX;
}
#ifndef NVN_DIRTY_NO_HANDLE_DEFAULT   /* tests/pipeline_hold/probe.c brings its own */
/* runtime/threads.c: the guest thread's handle (0 = a host thread). This weak default (no guest threads: graphics tests)
   gives way to the runtime's definition. */
__attribute__((weak)) uint32_t runtime_thread_current_handle(void) { return 0; }
#else
uint32_t runtime_thread_current_handle(void);
#endif
static int pl_guest_thread(void) { return runtime_thread_current_handle() != 0; }
static void pl_nap(void) { struct timespec ts = {0, 50000}; nanosleep(&ts, NULL); }
/* signal-safe: lr, then the return addresses of the frame chain (8-byte aligned frame pointers above sp only) */
static void pl_bt(void *uc, uint64_t *pc, uint64_t bt[NVN_PL_BT]) {
    memset(bt, 0, sizeof(uint64_t) * NVN_PL_BT); *pc = 0;
    if (!uc) return;
    _STRUCT_MCONTEXT64 *mc = ((ucontext_t *)uc)->uc_mcontext; *pc = mc->__ss.__pc; bt[0] = mc->__ss.__lr;
    uint64_t fp = mc->__ss.__fp, sp = mc->__ss.__sp;
    for (int i = 1; i < NVN_PL_BT && fp && !(fp & 7) && fp >= sp && fp - sp < (8u << 20); i++) { const uint64_t *f = (const uint64_t *)(uintptr_t)fp; bt[i] = f[1]; if (f[0] <= fp) break; fp = f[0]; }
}
/* signal-safe: does [a, b) overlap a range of the set? (ranges sorted by start, mx = running maximum of the ends) */
static int pl_rset_hit(pl_rset *s, uint64_t f, uintptr_t a, uintptr_t b) {
    int n = atomic_load_explicit(&s->n, memory_order_acquire); uintptr_t *lo = atomic_load(&s->lo), *mx = atomic_load(&s->mx);
    if (!n || !lo || !mx) return 0;
    int l = 0, h = n;   // last range starting before b
    while (l < h) { int m = (l + h) / 2; if (lo[m] < b) l = m + 1; else h = m; }
    int hit = l > 0 && mx[l - 1] > a;
    return atomic_load_explicit(&s->fence, memory_order_acquire) == f ? hit : 1;   // the set was replaced meanwhile: conservative
}
/* The newest released, unfinished fence whose in-place ranges overlap [a, b), 0 = none. */
static uint64_t pl_inflight_hit2(uintptr_t a, uintptr_t b, uint64_t done, uint64_t rel, int *kinds) {   // kinds: 1 a read range, 2 a write range
    uint64_t until = 0; int k = 0;
    for (int i = 0; i < PL_RSETS; i++) {
        uint64_t f = atomic_load_explicit(&g_pl_rs[i].fence, memory_order_acquire);
        if (f > done && f <= rel && pl_rset_hit(&g_pl_rs[i], f, a, b)) { k |= 1; if (f > until) until = f; }
        f = atomic_load_explicit(&g_pl_ws[i].fence, memory_order_acquire);
        if (f > done && f <= rel && pl_rset_hit(&g_pl_ws[i], f, a, b)) { k |= 2; if (f > until) until = f; }
    }
    if (kinds) *kinds = k;
    return until;
}
static uint64_t pl_inflight_hit(uintptr_t a, uintptr_t b, uint64_t done, uint64_t rel) { return pl_inflight_hit2(a, b, done, rel, NULL); }
/* A CPU write of [a, b) (one page) is about to make page p of r writable (fault handler, or a host write announced
   through prep_write). */
static void pl_before_write(range_t *r, uintptr_t p, uintptr_t a, uintptr_t b, uint64_t pc, const uint64_t *bt, int host) {
    uint64_t rel = atomic_load(&g_pl_released), done = pl_done_now();
    if (rel <= done) return;
    int kinds = 0; uint64_t until = pl_inflight_hit2(a, b, done, rel, &kinds);
    if (until && kinds == 2 && !g_pl_wwhold) { atomic_fetch_add(&g_pl_st[NVN_PLS_WWPASS], 1); until = 0; }   // only GPU writes there (the game partitions such buffers: particle pools): goes through
    if (!until) {   // another part of the page; a page whose CPU part is written every frame is left writable for a while
        atomic_fetch_add(&g_pl_st[NVN_PLS_PASS], 1);
        if (r->pl_pc && p < r->pl_cap && atomic_fetch_add(&r->pl_pc[p], 1) >= 3) { atomic_store(&r->pl_pc[p], 0); atomic_store(&r->pl_psk[p], atomic_load(&g_pl_frame) + 256); atomic_fetch_add(&g_pl_st[NVN_PLS_PASSKEEP], 1); }
        return;
    }
    if (r->pl_pc && p < r->pl_cap) { atomic_store(&r->pl_pc[p], 0); atomic_store(&r->pl_psk[p], 0); }
    uint64_t i = atomic_fetch_add(&g_pl_hrec_n, 1);
    nvn_pl_hold_rec *hr = &g_pl_hrec[i % 64]; *hr = (nvn_pl_hold_rec){pc, a, atomic_load(&g_pl_frame), (uint32_t)host, until, rel, {0}, (uint32_t)kinds};
    if (bt) memcpy(hr->bt, bt, sizeof hr->bt);
    if (g_pl_mode == 2) { atomic_fetch_add(&g_pl_st[NVN_PLS_WOULD], 1); return; }
    if (!pl_guest_thread()) { atomic_fetch_add(&g_pl_st[NVN_PLS_HOSTSKIP], 1); return; }
    uint64_t t0 = mach_absolute_time(), (*f)(void) = g_pl_done;
    atomic_fetch_add(&g_pl_st[host ? NVN_PLS_HOLD_HOST : NVN_PLS_HOLD], 1);
    while (f && f() < until) {
        if ((mach_absolute_time() - t0) * g_tb.numer / g_tb.denom > (uint64_t)PL_HOLD_MS * 1000000ull) { atomic_fetch_add(&g_pl_st[NVN_PLS_TIMEOUT], 1); break; }
        pl_nap();
    }
    atomic_fetch_add(&g_pl_st[NVN_PLS_HOLD_NS], (mach_absolute_time() - t0) * g_tb.numer / g_tb.denom);
}
static inline void pl_wrote(range_t *r, uintptr_t p) {   // the page became writable for a CPU write (unprotect_page)
    if (!r->pl_wf || p >= r->pl_cap) return;
    atomic_store_explicit(&r->pl_wfr[p], atomic_load_explicit(&g_pl_frame, memory_order_relaxed), memory_order_relaxed);
    uint64_t k = atomic_fetch_add_explicit(&g_pl_uring_n, 1, memory_order_relaxed);
    atomic_store_explicit(&g_pl_uring[k % PL_URING], r->base + (p << g_shift), memory_order_release);
}
/* The GPU will read [a, a+len). -1: not all of it tracked. 1: a page was written by the CPU within keep frames, or is
   on probation: a snapshot should be read. 0: static. A page whose volatility expired (or, with first, a page never
   looked at) is put on probation here: write-protected and counted as volatile for probe frames; a CPU write meanwhile
   makes it volatile again. (GPU writes in flight are checked first by the caller: nvn_pl_gpu_written.) */
int nvn_pl_classify(const void *addr, size_t len, uint32_t keep, uint32_t probe, int first, void (*prot)(uintptr_t lo, uintptr_t hi)) {   // prot: protects the probation runs (NULL: here)
    if (g_pl_mode <= 0 || !len) return -1;
    uintptr_t a = (uintptr_t)addr, e = a + len; int vol = 0; uint32_t fr = atomic_load(&g_pl_frame);
    while (a < e) {
        range_t *r = find(a); if (!r || !r->pl_wf) { atomic_fetch_add(&g_pl_st[NVN_PLS_UNTRACKED], 1); return -1; }
        uintptr_t stop = e < r->end ? e : r->end, p0 = (a - r->base) >> g_shift, p1 = (stop - 1 - r->base) >> g_shift, run = 0; int open = 0;
        for (uintptr_t p = p0; p <= p1 + 1; p++) {
            int start = 0;
            if (p <= p1) {
                uint32_t w = atomic_load_explicit(&r->pl_wfr[p], memory_order_relaxed), pf = atomic_load_explicit(&r->pl_pfr[p], memory_order_relaxed);
                if (w && fr - w < keep) vol = 1;
                else if (pf && (!w || pf >= w) && fr - pf < probe) vol = 1;
                else if ((w && (!pf || pf < w)) || (!w && !pf && first)) { atomic_store(&r->pl_pfr[p], fr); start = 1; vol = 1; atomic_fetch_add(&g_pl_st[NVN_PLS_PROBATION], 1); }
            }
            if (start && !open) { run = p; open = 1; }
            if (!start && open) { if (prot) prot(r->base + (run << g_shift), r->base + (p << g_shift)); else nvn_dirty_protect((const void *)(r->base + (run << g_shift)), (p - run) << g_shift); open = 0; }
        }
        a = stop;
    }
    return vol;
}
/* [a, a+len) overlaps an in-place range of a submission in (done, released] (not signal context). */
int nvn_pl_inflight(const void *addr, size_t len, uint64_t done, uint64_t released) {
    if (g_pl_mode <= 0 || !len) return 0;
    return pl_inflight_hit((uintptr_t)addr, (uintptr_t)addr + len, done, released) != 0;
}
/* GPU work of submission fence writes [a, a+len). */
void nvn_pl_gpu_write(const void *addr, size_t len, uint64_t fence) {
    if (g_pl_mode <= 0 || !len) return;
    uintptr_t a = (uintptr_t)addr, e = a + len;
    while (a < e) {
        range_t *r = find(a); if (!r || !r->pl_wf) { a = (a + g_pg) & ~(g_pg - 1); continue; }
        uintptr_t stop = e < r->end ? e : r->end;
        for (uintptr_t p = (a - r->base) >> g_shift, q = (stop - 1 - r->base) >> g_shift; p <= q; p++) {
            if (atomic_load_explicit(&r->pl_wf[p], memory_order_relaxed) < fence) atomic_store_explicit(&r->pl_wf[p], fence, memory_order_relaxed);
            if (atomic_load_explicit(&r->pl_wfb[p >> BLK_SHIFT], memory_order_relaxed) < fence) atomic_store_explicit(&r->pl_wfb[p >> BLK_SHIFT], fence, memory_order_relaxed);
        }
        a = stop;
    }
}
/* Some page of [a, a+len) is written by GPU work above done (blocks whose maximum is not above it are skipped). */
int nvn_pl_gpu_written(const void *addr, size_t len, uint64_t done) {
    if (g_pl_mode <= 0 || !len) return 0;
    uintptr_t a = (uintptr_t)addr, e = a + len;
    while (a < e) {
        range_t *r = find(a); if (!r || !r->pl_wf) return 0;   // untracked from here on: no GPU write is known there
        uintptr_t stop = e < r->end ? e : r->end;
        for (uintptr_t p = (a - r->base) >> g_shift, q = (stop - 1 - r->base) >> g_shift; p <= q;) {
            uintptr_t last = ((p >> BLK_SHIFT) << BLK_SHIFT) + BLK_PAGES - 1; if (last > q) last = q;
            if (atomic_load_explicit(&r->pl_wfb[p >> BLK_SHIFT], memory_order_relaxed) <= done) { p = last + 1; continue; }
            for (; p <= last; p++) if (atomic_load_explicit(&r->pl_wf[p], memory_order_relaxed) > done) return 1;
        }
        a = stop;
    }
    return 0;
}
/* The in-place ranges of submission fence, sorted by start (they may overlap). Replaces the set of fence - PL_RSETS. */
void nvn_pl_publish(uint64_t fence, const uintptr_t *lo, const uintptr_t *hi, int n, int writes) {
    if (g_pl_mode <= 0) return;
    pl_rset *s = &(writes ? g_pl_ws : g_pl_rs)[fence % PL_RSETS];
    atomic_store_explicit(&s->fence, 0, memory_order_release); atomic_store(&s->n, 0);
    if (n > s->cap) {   // the old arrays are not freed (a signal handler may still read them)
        int cap = n + n / 2 + 256; uintptr_t *l = malloc(sizeof *l * (size_t)cap), *h = malloc(sizeof *h * (size_t)cap), *m = malloc(sizeof *m * (size_t)cap);
        if (!l || !h || !m) die("pipeline range sets (%d)", n);
        atomic_store(&s->lo, l); atomic_store(&s->hi, h); atomic_store(&s->mx, m); s->cap = cap;
    }
    uintptr_t *l = atomic_load(&s->lo), *h = atomic_load(&s->hi), *m = atomic_load(&s->mx), run = 0;
    for (int i = 0; i < n; i++) { l[i] = lo[i]; h[i] = hi[i]; if (hi[i] > run) run = hi[i]; m[i] = run; }
    atomic_store_explicit(&s->n, n, memory_order_release);
    atomic_store_explicit(&s->fence, fence, memory_order_release);
}
static void pl_protect_run(uintptr_t a, uintptr_t e, int *k) {   // protect the pages of [a, e) the tracker has writable
    while (a < e) {
        range_t *r = find(a); if (!r) return;   // untracked from here on: nothing to protect
        uintptr_t stop = e < r->end ? e : r->end, run = 0; int open = 0;
        for (uintptr_t p = (a - r->base) >> g_shift, q = (stop - 1 - r->base) >> g_shift; p <= q + 1; p++) {
            int none = p <= q && atomic_load(&r->state[p]) == PG_NONE && !(g_pl_passkeep && r->pl_psk && p < r->pl_cap && atomic_load(&r->pl_psk[p]) > atomic_load(&g_pl_frame));
            if (none && !open) { run = p; open = 1; }
            if (!none && open) { nvn_dirty_protect((const void *)(r->base + (run << g_shift)), (p - run) << g_shift); *k += (int)(p - run); open = 0; }
        }
        a = stop;
    }
}
/* A range GPU work accesses in place that is not static (translate thread, at its publication): its writable pages are
   write-protected now, so CPU writes until the release go through the ring. Ranges over 16 MiB are left (counted). */
void nvn_pl_protect_span(const void *addr, size_t len) {
    if (g_pl_mode <= 0 || !len) return;
    if (len > (16u << 20)) { atomic_fetch_add(&g_pl_st[NVN_PLS_BIGSPAN], 1); return; }
    int k = 0; pl_protect_run((uintptr_t)addr, (uintptr_t)addr + len, &k);
    if (k) atomic_fetch_add(&g_pl_st[NVN_PLS_SPANPROT], (uint64_t)k);
}
/* Release (submissions in (done, released] now treated as complete): pages CPU writes unprotected since the last release
   that lie in those submissions' in-place ranges, and every page of their ranges [nlo, nhi) that are not static, are
   write-protected again. Returns the pages protected. The frame-wait thread only. */
int nvn_pl_reprotect(uint64_t done, uint64_t released, const uintptr_t *nlo, const uintptr_t *nhi, int nn) {
    if (g_pl_mode <= 0) return 0;
    int k = 0;
    uint64_t end = atomic_load(&g_pl_uring_n), from = g_pl_uring_cur;
    if (end - from > PL_URING) {   // the ring overflowed: every range of those submissions
        for (int i = 0; i < (g_pl_wwhold ? 2 : 1) * PL_RSETS; i++) {   // the write ranges only when W-W writes are held
            pl_rset *s = i < PL_RSETS ? &g_pl_rs[i] : &g_pl_ws[i - PL_RSETS]; uint64_t f = atomic_load(&s->fence);
            if (f <= done || f > released) continue;
            int n = atomic_load(&s->n); uintptr_t *lo = atomic_load(&s->lo), *hi = atomic_load(&s->hi);
            for (int j = 0; j < n; j++) pl_protect_run(lo[j], hi[j], &k);
        }
        atomic_fetch_add(&g_pl_st[NVN_PLS_URING_OVER], 1);
    } else
        for (uint64_t j = from; j < end; j++) {
            uintptr_t pg = atomic_load_explicit(&g_pl_uring[j % PL_URING], memory_order_acquire);
            int kinds = 0;   // a page only GPU writes of those submissions reach: a CPU write there passes anyway (unless TOTK_PIPELINE_WWHOLD)
            if (pg && pl_inflight_hit2(pg, pg + g_pg, done, released, &kinds) && ((kinds & 1) || g_pl_wwhold)) pl_protect_run(pg, pg + g_pg, &k);
        }
    g_pl_uring_cur = end;
    for (int i = 0; i < nn; i++) pl_protect_run(nlo[i], nhi[i], &k);
    if (k) atomic_fetch_add(&g_pl_st[NVN_PLS_REPROT], (uint64_t)k);
    return k;
}
/* ---------- TOTK_PIPELINE_RWATCH (dry run diagnostic) ---------- */
enum { PLW_MAX = 8192 };
static uintptr_t g_plw[PLW_MAX]; static _Atomic uint8_t g_plw_on[PLW_MAX]; static _Atomic int g_plw_n;
static int pl_track_prot(uintptr_t pg) { range_t *r = find(pg); return r && atomic_load(&r->state[(pg - r->base) >> g_shift]) == PG_PROT ? PROT_READ : PROT_READ | PROT_WRITE; }
static int pl_watch_find(uintptr_t pg) {
    int n = atomic_load_explicit(&g_plw_n, memory_order_acquire), lo = 0, hi = n;
    while (lo < hi) { int m = (lo + hi) / 2; if (g_plw[m] < pg) lo = m + 1; else hi = m; }
    return lo < n && g_plw[lo] == pg ? lo : -1;
}
/* signal handler: 1 = a watched page, made accessible again (run again). A page is made accessible before its flag is
   cleared (here and in nvn_pl_watch), so a cleared flag means accessible: 0, the tracker decides. */
static int pl_watch_fault(uintptr_t a, int is_write, uint64_t pc, const uint64_t *bt) {
    if (!atomic_load_explicit(&g_plw_n, memory_order_relaxed)) return 0;
    uintptr_t pg = a & ~(g_pg - 1); int i = pl_watch_find(pg); if (i < 0 || !atomic_load(&g_plw_on[i])) return 0;
    mp(pg, g_pg, pl_track_prot(pg));
    if (atomic_exchange(&g_plw_on[i], 0)) {
        atomic_fetch_add(&g_pl_st[is_write ? NVN_PLS_WATCH_WRITE : NVN_PLS_WATCH_READ], 1);
        uint64_t k = atomic_fetch_add(&g_pl_hrec_n, 1);
        nvn_pl_hold_rec *hr = &g_pl_hrec[k % 64]; *hr = (nvn_pl_hold_rec){pc, a, atomic_load(&g_pl_frame), is_write ? 3u : 2u, 0, atomic_load(&g_pl_released), {0}, 0};
        if (bt) memcpy(hr->bt, bt, sizeof hr->bt);
    }
    return 1;
}
static int pl_upcmp(const void *x, const void *y) { uintptr_t a = *(const uintptr_t *)x, b = *(const uintptr_t *)y; return a < b ? -1 : a > b; }
int nvn_pl_watch(const uintptr_t *pages, int n) {   // the frame-wait thread only; at most 1024 pages, mprotect over runs
    int old = atomic_load(&g_plw_n);
    for (int i = 0; i < old;) {   // runs of consecutive pages still watched that want the same protection
        if (!atomic_load(&g_plw_on[i])) { i++; continue; }
        int prot = pl_track_prot(g_plw[i]), j = i + 1;
        while (j < old && atomic_load(&g_plw_on[j]) && g_plw[j] == g_plw[j - 1] + g_pg && pl_track_prot(g_plw[j]) == prot) j++;
        mp(g_plw[i], (size_t)(j - i) * g_pg, prot);
        for (int k = i; k < j; k++) atomic_store(&g_plw_on[k], 0);
        i = j;
    }
    atomic_store_explicit(&g_plw_n, 0, memory_order_release);
    if (g_pl_mode != 2 || !n) return 0;
    int k = 0;
    for (int i = 0; i < n && k < 1024; i++) g_plw[k++] = pages[i] & ~(g_pg - 1);
    qsort(g_plw, (size_t)k, sizeof *g_plw, pl_upcmp);
    int m = 0; for (int i = 0; i < k; i++) if (!m || g_plw[m - 1] != g_plw[i]) g_plw[m++] = g_plw[i];
    for (int i = 0; i < m; i++) atomic_store(&g_plw_on[i], 1);
    atomic_store_explicit(&g_plw_n, m, memory_order_release);   // published before any page becomes inaccessible
    for (int i = 0; i < m;) { int j = i + 1; while (j < m && g_plw[j] == g_plw[j - 1] + g_pg) j++; mp(g_plw[i], (size_t)(j - i) * g_pg, PROT_NONE); i = j; }
    return m;
}
void nvn_pl_stats(uint64_t v[NVN_PLS_N]) { for (int i = 0; i < NVN_PLS_N; i++) v[i] = atomic_load(&g_pl_st[i]); }
int nvn_pl_holds(nvn_pl_hold_rec *out, int max) {   // the latest held (or, dry run, would-be held) writes, newest first
    uint64_t n = atomic_load(&g_pl_hrec_n); int k = 0;
    for (uint64_t j = n; j > 0 && j + 64 > n && k < max; j--) out[k++] = g_pl_hrec[(j - 1) % 64];
    return k;
}
static void unprotect_page(range_t *r, uintptr_t p) {   // 调用方已把状态从 PROT 改成 BUSY | The caller has already changed the state from PROT to BUSY
    mp(r->base + (p << g_shift), g_pg, PROT_READ | PROT_WRITE);
    uint64_t g = atomic_fetch_add(&g_now, 1) + 1, l = atomic_load(&r->latest);
    atomic_store(&r->gen[p], g);
    { _Atomic uint64_t *b = &r->blk[p >> BLK_SHIFT]; uint64_t bv = atomic_load(b);   // TOTK_DIRTY_FAST: the block's maximum before the range's (readers look at latest, then the block, then the page)
      while (bv < g && !atomic_compare_exchange_weak(b, &bv, g)) {} }
    while (l < g && !atomic_compare_exchange_weak(&r->latest, &l, g)) {}
    /* TOTK (#90): "now" moves on once more after the page's generation is stored. A checker that read "now" between the
       first step and the stores found the page clean and kept that "now" as its last look (tex_mtl's now == seen); the page
       is writable from here on, so without a newer "now" every later write went unseen until something else faulted. */
    atomic_fetch_add(&g_now, 1);
    pl_wrote(r, p);   // TOTK_PIPELINE: a CPU write
    atomic_store(&r->state[p], PG_NONE);
}
static int on_fault(void *addr, int is_write, void *uc) {
    if (g_pl_mode == 2 && atomic_load_explicit(&g_plw_n, memory_order_relaxed)) {   // TOTK_PIPELINE_RWATCH
        uint64_t pc, bt[NVN_PL_BT]; pl_bt(uc, &pc, bt);
        if (pl_watch_fault((uintptr_t)addr, is_write, pc, bt)) return 1;
    }
    (void)uc;
    range_t *r = is_write ? find((uintptr_t)addr) : NULL;
    if (!r) return 0;
    uintptr_t p = ((uintptr_t)addr - r->base) >> g_shift;
    if (g_pl_mode > 0 && atomic_load(&r->state[p]) == PG_PROT) {   // TOTK_PIPELINE: the GPU may still access these bytes in place
        uint64_t pc, bt[NVN_PL_BT]; pl_bt(uc, &pc, bt);
        uintptr_t line = (uintptr_t)addr & ~(uintptr_t)63;
        pl_before_write(r, p, line, line + 64, pc, bt, 0);
    }
    uint8_t st = PG_PROT;
    if (atomic_compare_exchange_strong(&r->state[p], &st, PG_BUSY)) { unprotect_page(r, p); atomic_fetch_add(&g_faults, 1);
        if (g_drec_on && uc) { _STRUCT_MCONTEXT64 *mc = ((ucontext_t *)uc)->uc_mcontext; drec_add(r->base + (p << g_shift), mc->__ss.__pc, mc->__ss.__lr, 0); }
        return 1; }
    if (st == PG_BUSY) return busy_wait(r, p);   // 别的线程正在改这页的保护：等它改完重执行（改成了只读就再缺页一次、走上面那一支） | Another thread is changing this page's protection: wait for it to finish and re-execute (if it became read-only, it faults once more and takes the branch above)
    return atomic_load(&r->gen[p]) != 0;   // 刚被别的线程解掉（本次缺页发生在解之前）：重执行；从未保护过的页不是我们的 | Just unprotected by another thread (this fault happened before the unprotect): re-execute; pages that were never protected are not ours
}
static void prep_write(void *addr, size_t len) {
    uintptr_t a = (uintptr_t)addr, e = a + len;
    if (g_pl_mode == 2 && len && atomic_load_explicit(&g_plw_n, memory_order_relaxed))   // TOTK_PIPELINE_RWATCH: a system call must not meet an inaccessible page
        for (uintptr_t pg = a & ~(g_pg - 1); pg < e; pg += g_pg) pl_watch_fault(pg, 1, dm_host_write_pc, NULL);
    while (len && a < e) {
        range_t *r = find(a);
        if (!r) { a = (a + g_pg) & ~(g_pg - 1); continue; }
        uintptr_t stop = e < r->end ? e : r->end;
        for (uintptr_t p = (a - r->base) >> g_shift, q = (stop - 1 - r->base) >> g_shift; p <= q; p++) {
            if (g_pl_mode > 0 && atomic_load(&r->state[p]) == PG_PROT) {   // TOTK_PIPELINE: the span of this write on the page
                uintptr_t pa = r->base + (p << g_shift), wa = a > pa ? a : pa, we = stop < pa + g_pg ? stop : pa + g_pg;
                pl_before_write(r, p, wa, we, dm_host_write_pc, NULL, 1); }
            for (;;) { uint8_t st = PG_PROT;   // 见 BUSY 等完要重看：对方可能是在设只读，等完这页是 PROT，照样得解 | After waiting out BUSY, look again: the other side may have been setting read-only, in which case the page is PROT after the wait and still has to be unprotected
                if (atomic_compare_exchange_strong(&r->state[p], &st, PG_BUSY)) { unprotect_page(r, p); atomic_fetch_add(&g_preps, 1); drec_add(r->base + (p << g_shift), dm_host_write_pc, 0, 1); break; }
                if (st != PG_BUSY || !busy_wait(r, p)) break; }
        }
        a = stop;
    }
}

/* Writes on behalf of the GPU (a rendering written back into CPU-visible memory): the bytes change without counting as
   a CPU write. Protected pages are taken to BUSY (a guest write meanwhile waits in the fault handler, then faults again
   and is recorded as usual), made writable, and protected again by nvn_dirty_gpu_write_end with their generation kept.
   Pages already unprotected are left as they are. Returns a token for nvn_dirty_gpu_write_end (NULL: nothing tracked). */
#if !defined(GAME_BOTW)
typedef struct { range_t *r; uintptr_t p0, p1; uint8_t *was; } gpu_write_tok;
void *nvn_dirty_gpu_write_begin(void *addr, size_t len) {
    if (!len) return NULL;
    range_t *r = find((uintptr_t)addr); if (!r || (uintptr_t)addr + len > r->end) return NULL;
    uintptr_t a = (uintptr_t)addr, p0 = (a - r->base) >> g_shift, p1 = (a + len - 1 - r->base) >> g_shift;
    gpu_write_tok *t = malloc(sizeof *t); uint8_t *was = calloc(p1 - p0 + 1, 1); if (!t || !was) { free(t); free(was); return NULL; }
    *t = (gpu_write_tok){ r, p0, p1, was };
    for (uintptr_t p = p0; p <= p1; p++) {
        for (;;) { uint8_t st = PG_PROT;
            if (atomic_compare_exchange_strong(&r->state[p], &st, PG_BUSY)) { mp(r->base + (p << g_shift), g_pg, PROT_READ | PROT_WRITE); was[p - p0] = 1; break; }
            if (st != PG_BUSY || !busy_wait(r, p)) break; }
    }
    return t;
}
void nvn_dirty_gpu_write_end(void *tok) {
    gpu_write_tok *t = tok; if (!t) return;
    for (uintptr_t p = t->p0; p <= t->p1; p++) if (t->was[p - t->p0]) { mp(t->r->base + (p << g_shift), g_pg, PROT_READ); atomic_store(&t->r->state[p], PG_PROT); }
    free(t->was); free(t);
}
#endif

#if !defined(GAME_BOTW)
void nvn_dirty_add_range(const void *mem, size_t size) {
    static int inited;
    if (!inited) { inited = 1; g_pg = (uintptr_t)getpagesize(); g_shift = __builtin_ctzl(g_pg); mach_timebase_info(&g_tb); dirty_fast_init(); dm_fault_hook_register(on_fault); dm_guest_write_prep_register(prep_write); g_drec_on = getenv("DM_NVN_TILELOG") != NULL; }
    atomic_fetch_add(&g_adds_total, 1);
    pthread_mutex_lock(&g_slot_mu);
    int n = atomic_load(&g_nr);
    int slot = -1;
    {   /* TOTK: a pool created over memory that another live pool already covers must not get a second tracking range: find() returns
           the first live range for an address, so a fault on a page protected under the older range would be looked up in the newer one
           (state NONE, generation 0) and reported as a crash. Inside an existing range the pages are tracked already; any other overlap
           leaves the new pool untracked (its textures are treated as always dirty, which is safe). */
        uintptr_t nb = (uintptr_t)mem & ~(g_pg - 1), ne = ((uintptr_t)mem + size + g_pg - 1) & ~(g_pg - 1);
        for (int i = 0; i < n; i++) {
            if (!atomic_load_explicit(&g_r[i].live, memory_order_acquire) || ne <= g_r[i].base || nb >= g_r[i].end) continue;
            static _Atomic int said; int expected = 0;
            if (atomic_compare_exchange_strong(&said, &expected, 1))
                LOG("脏页跟踪：池 %p+%#zx 与已跟踪区间 %#lx-%#lx 重叠，%s", mem, size, (unsigned long)g_r[i].base, (unsigned long)g_r[i].end,
                    nb >= g_r[i].base && ne <= g_r[i].end ? "已由该区间跟踪" : "不另行跟踪");
            pthread_mutex_unlock(&g_slot_mu); return;
        }
    }
    for (int i = 0; i < n; i++) if (!atomic_load_explicit(&g_r[i].live, memory_order_acquire)) { slot = i; break; }
    if (slot < 0) {
        if (n >= MAX_RANGE) {
            int expected = 0;
            if (atomic_compare_exchange_strong(&g_overflow_logged, &expected, 1)) LOG("【红】脏页跟踪区间已满（%d），池 %p 不跟踪", MAX_RANGE, mem);
            pthread_mutex_unlock(&g_slot_mu); return;
        }
        slot = n;
    }
    { int lv = atomic_fetch_add(&g_live, 1) + 1, pk = atomic_load(&g_live_peak); while (lv > pk && !atomic_compare_exchange_weak(&g_live_peak, &pk, lv)) {} }
    range_t *r = &g_r[slot];
    uintptr_t base = (uintptr_t)mem & ~(g_pg - 1), end = ((uintptr_t)mem + size + g_pg - 1) & ~(g_pg - 1);
    size_t np = (end - base) >> g_shift;
    _Atomic uint8_t *state = r->state;
    _Atomic uint64_t *gen = r->gen, *blk = r->blk;
    if (r->cap < np) {
        state = calloc(np, 1); gen = calloc(np, sizeof *gen); blk = calloc(blk_count(np), sizeof *blk);
        if (!state || !gen || !blk) die("脏页表分配失败（%zu 页）", np);
        // 旧数组不 free：信号处理里可能仍有极短暂的陈旧读者，泄漏量 = 每页 9 字节。
        // The old array is not freed: the signal handler may still have a very brief stale reader; leak = 9 bytes per page.
        r->cap = np;
    } else {
        memset((void *)state, 0, np); memset((void *)gen, 0, np * sizeof *gen); memset((void *)blk, 0, blk_count(np) * sizeof *blk);
    }
    r->base = base; r->end = end; r->state = state; r->gen = gen; r->blk = blk; atomic_store(&r->latest, 0);
    pl_arrays(r, np);
    if (slot == n) atomic_store(&g_nr, n + 1);
    atomic_store_explicit(&r->live, 1, memory_order_release);
    rindex_rebuild();
    pthread_mutex_unlock(&g_slot_mu);
}
// 池销毁：先把仍受保护的页还原成可写（池内存归还游戏后可能另作他用），再标失效
// Pool destruction: first restore still-protected pages to writable (the pool memory may be reused for something else once returned to the game), then mark invalid
void nvn_dirty_remove_range(const void *mem) {
    pthread_mutex_lock(&g_slot_mu);
    range_t *r = find((uintptr_t)mem);
    if (r) { prep_write((void *)r->base, r->end - r->base); atomic_store_explicit(&r->live, 0, memory_order_release); atomic_fetch_sub(&g_live, 1); rindex_rebuild(); }
    pthread_mutex_unlock(&g_slot_mu);
}
int nvn_dirty_tracked(const void *addr, size_t len) {
    range_t *r = find((uintptr_t)addr); return r && (uintptr_t)addr + len <= r->end;
}
// 设只读：先把页标成 BUSY，mprotect 落地后才标 PROT（次序与不变式见文件头的状态说明）
// Set read-only: mark the page BUSY first, and mark it PROT only after mprotect has landed (for the order and the invariant see the state notes at the top of the file)
void nvn_dirty_protect(const void *addr, size_t len) {
    if (!len || !nvn_dirty_tracked(addr, len)) return;
    range_t *r = find((uintptr_t)addr);
    uintptr_t a = (uintptr_t)addr, p = (a - r->base) >> g_shift, q = (a + len - 1 - r->base) >> g_shift;
    // 连续置成 PROT 的页合成一次 mprotect（逐页调用在首传大贴图时一帧上千次；选角页启动帧 5：4888 次）。
    // 已是 PROT / BUSY 的页断开连续段：不重复保护，也不碰别的线程正在改的页
    // Consecutive pages set to PROT are merged into one mprotect (per-page calls ran to over a thousand per frame when large textures were first uploaded; character select, startup frame 5: 4888 calls).
    // Pages already PROT / BUSY break a run: they are not protected again, and pages another thread is changing are left alone
    // TOTK (#90): a BUSY page is waited out (the run so far is protected first, so nothing of ours is BUSY meanwhile) and
    // looked at again. An unprotect in progress leaves the page writable with a generation the caller's stamp may already
    // cover: skipped, later writes to it never faulted and the texture stayed stale (letters missing in the glyph cache).
    uintptr_t run = 0; int open = 0;
    for (uintptr_t i = p; i <= q + 1; i++) {
        uint8_t st = PG_NONE;
        int took = i <= q && atomic_compare_exchange_strong(&r->state[i], &st, PG_BUSY);
        if (took && !open) { run = i; open = 1; }
        if (!took && open) { mp(r->base + (run << g_shift), (i - run) << g_shift, PROT_READ); for (uintptr_t k = run; k < i; k++) atomic_store(&r->state[k], PG_PROT); open = 0; }
        if (!took && st == PG_BUSY && busy_wait(r, i)) i--;   // this page again
    }
}
#endif
uint64_t nvn_dirty_now(void) { return atomic_load(&g_now); }
#if !defined(GAME_BOTW)
int nvn_dirty_since(const void *addr, size_t len, uint64_t stamp) {
    if (!len) return 0;
    if (!nvn_dirty_tracked(addr, len)) return 1;   // 未跟踪时保守视为脏，宁可多传也不显示旧内容 | When untracked, conservatively treat as dirty: better to upload more than to show stale content
    range_t *r = find((uintptr_t)addr);
    if (atomic_load(&r->latest) <= stamp) return 0;   // TOTK: nothing in the whole pool was written since the stamp (no per-page scan)
    uintptr_t a = (uintptr_t)addr, p = (a - r->base) >> g_shift, q = (a + len - 1 - r->base) >> g_shift;
    return pages_since(r, p, q, stamp);   // 上传前最后一次写的代号 ≤ 戳；之后的写必 > 戳 | The generation of the last write before upload is <= the stamp; any later write must be > the stamp
}
#endif
#if defined(GAME_BOTW)
/* BOTW versions of the functions above: a pool's pages can lie in several ranges (its own gaps and the neighbours' shared
   pages, see g_rmem), so the lookups walk range by range; a page in no live range makes the span untracked. */
static void range_put(int slot, int n, uintptr_t base, uintptr_t end, const void *mem) {   // under g_slot_mu
    { int lv = atomic_fetch_add(&g_live, 1) + 1, pk = atomic_load(&g_live_peak); while (lv > pk && !atomic_compare_exchange_weak(&g_live_peak, &pk, lv)) {} }
    range_t *r = &g_r[slot];
    size_t np = (end - base) >> g_shift;
    _Atomic uint8_t *state = r->state;
    _Atomic uint64_t *gen = r->gen, *blk = r->blk;
    if (r->cap < np) {
        state = calloc(np, 1); gen = calloc(np, sizeof *gen); blk = calloc(blk_count(np), sizeof *blk);
        if (!state || !gen || !blk) die("脏页表分配失败（%zu 页）", np);
        r->cap = np;   // the old arrays are not freed (see nvn_dirty_add_range above)
    } else {
        memset((void *)state, 0, np); memset((void *)gen, 0, np * sizeof *gen); memset((void *)blk, 0, blk_count(np) * sizeof *blk);
    }
    r->base = base; r->end = end; r->state = state; r->gen = gen; r->blk = blk; atomic_store(&r->latest, 0);
    pl_arrays(r, np);
    g_rmem[slot] = (uintptr_t)mem; atomic_store(&g_rdying[slot], 0);
    if (slot == n) atomic_store(&g_nr, n + 1);
    atomic_store_explicit(&r->live, 1, memory_order_release);
    rindex_rebuild();
}
static void ov_log(const char *what, _Atomic uint64_t *c, const void *mem, size_t size) {
    uint64_t k = atomic_fetch_add(c, 1) + 1;
    if (k <= 3 || !(k & (k - 1))) LOG("脏页跟踪：池 %p+%#zx 与已跟踪区间重叠，%s（累计 %llu 次）", mem, size, what, (unsigned long long)k);
}
void nvn_dirty_add_range(const void *mem, size_t size) {
    static int inited;
    if (!inited) { inited = 1; g_pg = (uintptr_t)getpagesize(); g_shift = __builtin_ctzl(g_pg); mach_timebase_info(&g_tb); dirty_fast_init(); dm_fault_hook_register(on_fault); dm_guest_write_prep_register(prep_write); g_drec_on = getenv("DM_NVN_TILELOG") != NULL; }
    atomic_fetch_add(&g_adds_total, 1);
    pthread_mutex_lock(&g_slot_mu);
    int n = atomic_load(&g_nr);
    uintptr_t nb = (uintptr_t)mem & ~(g_pg - 1), ne = ((uintptr_t)mem + size + g_pg - 1) & ~(g_pg - 1);
    /* the live ranges this pool overlaps, by base; the pool gets one range for each gap between them */
    enum { MAX_OV = 16 }; uintptr_t ob[MAX_OV], oe[MAX_OV]; int no = 0, same = 0;
    for (int i = 0; i < n; i++) {
        range_t *r = &g_r[i];
        if (!atomic_load_explicit(&r->live, memory_order_acquire) || ne <= r->base || nb >= r->end) continue;
        same |= g_rmem[i] == (uintptr_t)mem;
        if (no == MAX_OV) { ov_log("重叠区间太多，不另行跟踪", &g_ov_many, mem, size); pthread_mutex_unlock(&g_slot_mu); return; }
        int k = no++; while (k && ob[k - 1] > r->base) { ob[k] = ob[k - 1]; oe[k] = oe[k - 1]; k--; }
        ob[k] = r->base; oe[k] = r->end;
    }
    if (same) ov_log("起点与另一个池相同", &g_ov_same, mem, size);
    int made = 0;
    for (int k = 0; k <= no; k++) {
        uintptr_t gb = k ? oe[k - 1] : nb, ge = k < no ? ob[k] : ne;
        if (gb < nb) gb = nb;
        if (ge > ne) ge = ne;
        if (gb >= ge) continue;
        int slot = -1;
        for (int i = 0; i < n; i++) if (!atomic_load_explicit(&g_r[i].live, memory_order_acquire)) { slot = i; break; }
        if (slot < 0) {
            if (n >= MAX_RANGE) {
                int expected = 0;
                if (atomic_compare_exchange_strong(&g_overflow_logged, &expected, 1)) LOG("【红】脏页跟踪区间已满（%d），池 %p 不跟踪", MAX_RANGE, mem);
                break;
            }
            slot = n;
        }
        range_put(slot, n, gb, ge, mem); made++;
        if (slot == n) n++;
    }
    if (no) ov_log(made ? "只跟踪它自己的页" : "整个在已跟踪区间内，由该区间跟踪", made ? &g_ov_split : &g_ov_inside, mem, size);
    pthread_mutex_unlock(&g_slot_mu);
}
void nvn_dirty_remove_range(const void *mem) {
    pthread_mutex_lock(&g_slot_mu);
    int n = atomic_load(&g_nr), found = 0;
    for (int i = 0; i < n; i++) {
        range_t *r = &g_r[i];
        if (!atomic_load_explicit(&r->live, memory_order_acquire) || g_rmem[i] != (uintptr_t)mem) continue;
        atomic_store(&g_rdying[i], 1);   // before the unprotect: see protect_seg
        prep_write((void *)r->base, r->end - r->base); atomic_store_explicit(&r->live, 0, memory_order_release); atomic_fetch_sub(&g_live, 1); found++; rindex_rebuild();
    }
    /* "now" moves on: a texture of a neighbour pool that crossed into the removed range is untracked from here on, and the
       replay's shortcut (nothing written since the last look = clean, nvn_mtl.m) must look again */
    if (found) atomic_fetch_add(&g_now, 1);
    if (!found) { uint64_t k = atomic_fetch_add(&g_rm_none, 1) + 1;
        if (k <= 3 || !(k & (k - 1))) LOG("脏页跟踪：销毁的池 %p 没有自己的跟踪区间，别的池的区间不动（累计 %llu 次）", mem, (unsigned long long)k); }
    pthread_mutex_unlock(&g_slot_mu);
}
int nvn_dirty_tracked(const void *addr, size_t len) {
    uintptr_t a = (uintptr_t)addr, e = a + len;
    do { range_t *r = find(a); if (!r) return 0; a = r->end; } while (a < e);
    return 1;
}
/* Set read-only the pages of [a, e) in range r (as nvn_dirty_protect above). A run is set read-only only if the range is not
   being removed: the remover sets g_rdying before it unprotects, so either this sees the flag or the remover meets the
   run's pages BUSY, waits, and unprotects them. */
static void protect_seg(range_t *r, uintptr_t a, uintptr_t e) {
    uintptr_t p = (a - r->base) >> g_shift, q = (e - 1 - r->base) >> g_shift, run = 0; int open = 0;
    _Atomic uint8_t *dying = &g_rdying[r - g_r];
    for (uintptr_t i = p; i <= q + 1; i++) {
        uint8_t st = PG_NONE;
        int took = i <= q && atomic_compare_exchange_strong(&r->state[i], &st, PG_BUSY);
        if (took && !open) { run = i; open = 1; }
        if (!took && open) {
            int keep = !atomic_load(dying);
            if (keep) mp(r->base + (run << g_shift), (i - run) << g_shift, PROT_READ);
            for (uintptr_t k = run; k < i; k++) atomic_store(&r->state[k], keep ? PG_PROT : PG_NONE);
            open = 0; }
        if (!took && st == PG_BUSY && busy_wait(r, i)) i--;   // this page again
    }
}
void nvn_dirty_protect(const void *addr, size_t len) {
    if (!len || !nvn_dirty_tracked(addr, len)) return;
    uintptr_t a = (uintptr_t)addr, e = a + len;
    while (a < e) { range_t *r = find(a); if (!r) return; uintptr_t stop = e < r->end ? e : r->end; protect_seg(r, a, stop); a = stop; }
}
int nvn_dirty_since(const void *addr, size_t len, uint64_t stamp) {
    if (!len) return 0;
    uintptr_t a = (uintptr_t)addr, e = a + len;
    while (a < e) {
        range_t *r = find(a); if (!r) return 1;   // untracked: dirty
        uintptr_t stop = e < r->end ? e : r->end;
        if (atomic_load(&r->latest) > stamp && pages_since(r, (a - r->base) >> g_shift, (stop - 1 - r->base) >> g_shift, stamp)) return 1;
        a = stop;
    }
    return 0;
}
/* GPU writes (as above), over up to 4 ranges */
typedef struct { int n; range_t *r[4]; uintptr_t p0[4], p1[4]; uint8_t *was[4]; } gpu_write_tok;
void nvn_dirty_gpu_write_end(void *tok);
void *nvn_dirty_gpu_write_begin(void *addr, size_t len) {
    if (!len || !nvn_dirty_tracked(addr, len)) return NULL;
    gpu_write_tok *t = calloc(1, sizeof *t); if (!t) return NULL;
    uintptr_t a = (uintptr_t)addr, e = a + len;
    while (a < e) {
        range_t *r = find(a); uintptr_t stop = r ? (e < r->end ? e : r->end) : e;
        uintptr_t p0 = r ? (a - r->base) >> g_shift : 0, p1 = r ? (stop - 1 - r->base) >> g_shift : 0;
        uint8_t *was = r && t->n < 4 ? calloc(p1 - p0 + 1, 1) : NULL;
        if (!was) { nvn_dirty_gpu_write_end(t); return NULL; }   // a range went away meanwhile, or more than 4: writes fault as usual
        int k = t->n++; t->r[k] = r; t->p0[k] = p0; t->p1[k] = p1; t->was[k] = was;
        for (uintptr_t p = p0; p <= p1; p++) {
            for (;;) { uint8_t st = PG_PROT;
                if (atomic_compare_exchange_strong(&r->state[p], &st, PG_BUSY)) { mp(r->base + (p << g_shift), g_pg, PROT_READ | PROT_WRITE); was[p - p0] = 1; break; }
                if (st != PG_BUSY || !busy_wait(r, p)) break; }
        }
        a = stop;
    }
    return t;
}
void nvn_dirty_gpu_write_end(void *tok) {
    gpu_write_tok *t = tok; if (!t) return;
    for (int k = 0; k < t->n; k++) {
        range_t *r = t->r[k]; int keep = !atomic_load(&g_rdying[r - g_r]);   // a range being removed gets its pages back writable
        for (uintptr_t p = t->p0[k]; p <= t->p1[k]; p++) if (t->was[k][p - t->p0[k]]) { if (keep) mp(r->base + (p << g_shift), g_pg, PROT_READ); atomic_store(&r->state[p], keep ? PG_PROT : PG_NONE); }
        free(t->was[k]);
    }
    free(t);
}
#endif
void nvn_dirty_slot_stats(int *live, int *used_slots, uint64_t *adds_total) {
    int n = atomic_load(&g_nr), active = 0;
    for (int i = 0; i < n; i++) active += atomic_load_explicit(&g_r[i].live, memory_order_acquire) != 0;
    *live = active; *used_slots = n; *adds_total = atomic_load(&g_adds_total);
}
void nvn_dirty_stats(uint64_t *faults, uint64_t *preps) { *faults = atomic_load(&g_faults); *preps = atomic_load(&g_preps); }
void nvn_dirty_mp_stats(uint64_t *calls, uint64_t *pages, uint64_t *ns) {
    static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb);
    *calls = atomic_load(&g_mp_calls); *pages = atomic_load(&g_mp_pages); *ns = atomic_load(&g_mp_ticks) * tb.numer / tb.denom;
}
