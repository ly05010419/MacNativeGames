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
#ifndef BUSY_WAIT_MS
#define BUSY_WAIT_MS 2000
#endif
enum { BUSY_SITE_FAULT = 0, BUSY_SITE_PREP, BUSY_SITE_GPU, BUSY_SITE_PROTECT, BUSY_SITE_PROTECT_SEG, BUSY_SITE_NOREAD, BUSY_SITE_N };
static const char *const g_busy_site_name[BUSY_SITE_N] = {
    "on_fault", "prep_write", "gpu_write_begin", "nvn_dirty_protect", "protect_seg", "noread_probe",
};
// 跟踪区间 = 各内存池（MemoryPoolInitialize 登记）。池销毁后复用 live=0 的槽，信号处理里无锁扫描。
// Tracked ranges = the memory pools (registered by MemoryPoolInitialize). After a pool is destroyed its live=0 slot is reused; the signal handler scans without locks.
#define MAX_RANGE 16384 // 原为 256：内存池多于它时超出的池不跟踪，里面的贴图每次取用都当成脏的重传（暗黑实测峰值见内存读数行的「内存池」；塞尔达约 300 个，踩过）。扫描只到已用的槽数，容量大不增加开销 | was 256: with more memory pools than that the excess pools are untracked and their textures are treated as dirty and re-uploaded on every use (for Diablo's measured peak see "memory pools" in the memory readout line; Zelda has about 300 and hit this). The scan only covers slots in use, so a larger capacity costs nothing. TOTK (2026-10-17): 4096 was full in the user's own games and most long test games (about 3,900 live pools in the world, more when moving between regions); now 16384
typedef struct { uintptr_t base, end; _Atomic uint8_t *state; _Atomic uint64_t *gen; size_t cap; _Atomic int live; _Atomic uint64_t latest; _Atomic uint64_t *blk; _Atomic uint64_t *rd; uint16_t *rm; uint8_t *rk; _Atomic uint8_t *nr; } range_t;   // rd (OOT): per page, the fence of the latest GPU submission that reads it (nvn_dirty_read_mark)   // latest (TOTK): highest page generation in the range; blk: the same per block of BLK_PAGES pages
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
static pthread_mutex_t g_slot_mu = PTHREAD_MUTEX_INITIALIZER;   // 建池 / 销池（客体线程，可能并发）互斥选槽；缺页处理路径不碰它 | Pool creation / destruction (guest threads, possibly concurrent) pick slots under a mutex, and TOTK's protect holds it from its lookup to its last mprotect; the page-fault handling path does not touch it
static _Atomic uint64_t g_mp_calls, g_mp_pages, g_mp_ticks;   // 本模块的 mprotect：调用次数、覆盖页数、耗时（mach 时基，缺页路径里也安全） | This module's mprotect: call count, pages covered, time spent (mach timebase, safe in the page-fault path too)
static void mp(uintptr_t a, size_t len, int prot) {
    uint64_t t = mach_absolute_time(); mprotect((void *)a, len, prot);
    atomic_fetch_add(&g_mp_ticks, mach_absolute_time() - t); atomic_fetch_add(&g_mp_calls, 1); atomic_fetch_add(&g_mp_pages, len >> g_shift);
}

static mach_timebase_info_data_t g_tb;   // 首次登记池时取好（缺页路径里只做乘除） | Obtained when a pool is first registered (the fault path only multiplies and divides)
static _Atomic uint64_t g_busy_to[BUSY_SITE_N], g_busy_to_n;   // 各调用点与总计的 BUSY 超时次数 | per-site and total BUSY timeout counts
// 等这一页离开 BUSY。返回 1 = 已离开；0 = 等过了上限（进程总计前 8 次各打一行，含调用点自身计数；之后只计数不再打）。信号处理里可用：只有原子操作、mach_absolute_time、sched_yield、LOG
// Wait for this page to leave BUSY. Returns 1 = it has left; 0 = waited past the limit (log only the first 8 process-wide totals, with per-site count; further timeouts keep counting silently). Usable in a signal handler: only atomics, mach_absolute_time, sched_yield and LOG
static int busy_wait(range_t *r, uintptr_t p, int site) {
    uint64_t t0 = 0;
    for (unsigned n = 1; atomic_load(&r->state[p]) == PG_BUSY; n++) {
        if (n & 0x3ff) continue;
        uint64_t now = mach_absolute_time(); if (!t0) t0 = now;
        if ((now - t0) * g_tb.numer / g_tb.denom > (uint64_t)BUSY_WAIT_MS * 1000000ull) {
            uint64_t waited_ms = (now - t0) * g_tb.numer / g_tb.denom / 1000000ull;
            uint64_t sk = atomic_fetch_add(&g_busy_to[site], 1) + 1;
            uint64_t k = atomic_fetch_add(&g_busy_to_n, 1) + 1;
            if (k <= 8) {
                const char *sn = g_busy_site_name[site];
                LOG("【红】脏页跟踪：页 %p 停在「忙」超过 %llu 毫秒，不再等 | dirty page %p stayed BUSY over %llu ms (%s #%llu, total #%llu)", (void *)(r->base + (p << g_shift)), (unsigned long long)waited_ms, (void *)(r->base + (p << g_shift)), (unsigned long long)waited_ms, sn, (unsigned long long)sk, (unsigned long long)k);
            }
            return 0; }
        sched_yield();
    }
    return 1;
}
/* OOT (2026-11-09) GPU read fences (nvn_dirty_read_mark / rd_check). The game's frame-start fence wait may be relaxed
   (TOTK_SYNC_LAG=2): the CPU frame then runs while the GPU still reads the previous frame's inputs. Every guest range a
   submission reads on the GPU is marked with that submission's fence per page (rd[]) and set read-only; a CPU write to
   such a page faults here and waits until the completed-fence counter reaches the mark, i.e. exactly until the GPU has
   finished reading that page -- not the whole frame. The thread encoding a submission is exempt (it commits the fence it
   would wait for). Fault path: atomics, mach_absolute_time, sched_yield / nanosleep only, as busy_wait. */
static const uint64_t *g_rd_done, *g_rd_committed;   // nvn_mtl.m: highest completed fence / last committed fence (plain stores there, acquire loads here)
static pthread_key_t g_rd_key; static int g_rd_key_ok;   // per-thread exemption flag: the encoding thread while it encodes, the completion threads for good (TSD: readable in the fault handler, no lock, no allocation)
static _Atomic uint64_t g_rd_marks, g_rd_mark_pages, g_rd_waits, g_rd_wait_ns, g_rd_wait_max_ns, g_rd_timeouts;
enum { RD_WAIT_MS = 3000 };
static uint64_t rd_wait(uint64_t fence) {   // returns the wait in ns
    uint64_t t0 = mach_absolute_time();
    for (unsigned n = 1;; n++) {
        if (__atomic_load_n(g_rd_done, __ATOMIC_ACQUIRE) >= fence) break;
        if (!(n & 0x3f)) { uint64_t now = mach_absolute_time();
            if ((now - t0) * g_tb.numer / g_tb.denom > (uint64_t)RD_WAIT_MS * 1000000ull) { atomic_fetch_add(&g_rd_timeouts, 1); break; } }
        if (n < 64) sched_yield(); else { struct timespec ts = { 0, 20000 }; nanosleep(&ts, NULL); }
    }
    uint64_t ns = (mach_absolute_time() - t0) * g_tb.numer / g_tb.denom;
    atomic_fetch_add(&g_rd_waits, 1); atomic_fetch_add(&g_rd_wait_ns, ns);
    uint64_t m = atomic_load(&g_rd_wait_max_ns); while (ns > m && !atomic_compare_exchange_weak(&g_rd_wait_max_ns, &m, ns)) {}
    return ns;
}
/* Before a CPU write to page p of r lands. committed_only: wait only for fences already committed (host writes: the
   caller may hold a lock the encoding thread needs, so an in-progress fence could never be committed meanwhile). */
static nvn_rd_event g_rde[4096]; static _Atomic uint64_t g_rde_n; static uint64_t g_rde_drained;   // the waits, for the attribution log (nvn_dirty_read_wait_events)
static int g_nr_active;   // OOT_NOREAD_PROBE used at least once: read faults are ours to look at too
static nvn_nr_event g_nre[4096]; static _Atomic uint64_t g_nre_n; static uint64_t g_nre_drained;
static void nr_record(uintptr_t addr, uint64_t pc, int write) {
    uint64_t i = atomic_fetch_add(&g_nre_n, 1);
    g_nre[i % 4096] = (nvn_nr_event){ addr, pc, mach_absolute_time() * g_tb.numer / g_tb.denom, (uint8_t)write };
}
static inline void rd_check(range_t *r, uintptr_t p, int committed_only, uint64_t pc, int host, uintptr_t addr) {
    if (!r->rd || !g_rd_done) return;
    uint64_t f = atomic_load_explicit(&r->rd[p], memory_order_relaxed);
    if (!f || __atomic_load_n(g_rd_done, __ATOMIC_ACQUIRE) >= f) return;
    if (g_rd_key_ok && pthread_getspecific(g_rd_key)) return;   // the encoding thread (it commits this fence) and the completion threads (their guest writes are the GPU timeline's; g_done moves after them)
    if (committed_only && f > __atomic_load_n(g_rd_committed, __ATOMIC_ACQUIRE)) return;
    uint64_t ns = rd_wait(f), i = atomic_fetch_add(&g_rde_n, 1);
    g_rde[i % 4096] = (nvn_rd_event){ addr, f, ns, pc, r->rm ? r->rm[p] : 0, r->rk ? r->rk[p] : 0, (uint8_t)host };   // the exact address written (page field)
}
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
#define DREC_N 65536   // OOT (2026-11-09): 4096 held about two frames of page unprotections; the race inventory reads the ring one or two frames after the writes
static nvn_dirty_rec g_drec[DREC_N]; static _Atomic uint64_t g_drec_n; static int g_drec_on;
uint64_t dm_host_write_pc;   // set by dm_guest_write_prepare (runtime/gfx_bridge.c) before calling prep_write
static void drec_add(uintptr_t page, uint64_t pc, uint64_t lr, uint32_t src) {
    if (!g_drec_on) return;
    uint64_t i = atomic_fetch_add(&g_drec_n, 1) % DREC_N;
    g_drec[i] = (nvn_dirty_rec){ page, pc, lr, src, mach_absolute_time() * g_tb.numer / g_tb.denom };   // t on the CLOCK_UPTIME_RAW base (what clock_gettime_nsec_np returns); signal-safe
}
void nvn_dirty_recent_enable(void) { g_drec_on = 1; }
int nvn_dirty_recent(uintptr_t lo, uintptr_t hi, nvn_dirty_rec *out, int max) {
    uint64_t n = atomic_load(&g_drec_n); int k = 0;
    for (uint64_t j = n > DREC_N ? n - DREC_N : 0; j < n && k < max; j++) { nvn_dirty_rec r = g_drec[j % DREC_N]; if (r.page >= lo && r.page < hi) out[k++] = r; }
    return k;
}
int nvn_dirty_recent_snapshot(nvn_dirty_rec *out, int max) {   // records being written meanwhile may come out torn: diagnostic use only
    uint64_t n = atomic_load(&g_drec_n), cnt = n > DREC_N ? DREC_N : n; if (max > 0 && cnt > (uint64_t)max) cnt = (uint64_t)max;
    int k = 0; for (uint64_t j = n - cnt; j < n; j++) out[k++] = g_drec[j % DREC_N];
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
    atomic_store(&r->state[p], PG_NONE);
}
static int on_fault(void *addr, int is_write, void *uc) {
    range_t *r = (is_write || g_nr_active) ? find((uintptr_t)addr) : NULL;
    if (!r) return 0;
    uintptr_t p = ((uintptr_t)addr - r->base) >> g_shift;
    uint64_t pc = uc ? ((ucontext_t *)uc)->uc_mcontext->__ss.__pc : 0;
    if (r->nr && g_nr_active) {   // OOT_NOREAD_PROBE: a page the GPU wrote last frame is inaccessible until its first CPU access
        int was = atomic_exchange(&r->nr[p], 0);   // 1: this is that first access (record it); 0: another thread took it (or an ordinary write fault)
        uint8_t st = atomic_load(&r->state[p]); if (st == PG_BUSY) { busy_wait(r, p, BUSY_SITE_NOREAD); st = atomic_load(&r->state[p]); }
        if (was || !is_write || st != PG_PROT) {   // tracked pages are never inaccessible otherwise: restore the state's protection (idempotent) and re-execute
            mp(r->base + (p << g_shift), g_pg, st == PG_PROT ? PROT_READ : PROT_READ | PROT_WRITE);
            if (was) nr_record((uintptr_t)addr, pc, is_write);
            if (!is_write || st != PG_PROT) return 1;
        }
    } else if (!is_write) return 0;
    rd_check(r, p, 0, pc, 0, (uintptr_t)addr);   // OOT: a pending GPU reader of this page -> this write waits for its submission to complete
    uint8_t st = PG_PROT;
    if (atomic_compare_exchange_strong(&r->state[p], &st, PG_BUSY)) { unprotect_page(r, p); atomic_fetch_add(&g_faults, 1);
        if (g_drec_on && uc) { _STRUCT_MCONTEXT64 *mc = ((ucontext_t *)uc)->uc_mcontext; drec_add(r->base + (p << g_shift), mc->__ss.__pc, mc->__ss.__lr, 0); }
        return 1; }
    if (st == PG_BUSY) return busy_wait(r, p, BUSY_SITE_FAULT);   // 别的线程正在改这页的保护：等它改完重执行（改成了只读就再缺页一次、走上面那一支） | Another thread is changing this page's protection: wait for it to finish and re-execute (if it became read-only, it faults once more and takes the branch above)
    return atomic_load(&r->gen[p]) != 0;   // 刚被别的线程解掉（本次缺页发生在解之前）：重执行；从未保护过的页不是我们的 | Just unprotected by another thread (this fault happened before the unprotect): re-execute; pages that were never protected are not ours
}
static void prep_write_ex(void *addr, size_t len, int rd) {   // rd: wait for pending GPU readers (not from pool destruction, which holds g_slot_mu)
    uintptr_t a = (uintptr_t)addr, e = a + len;
    while (len && a < e) {
        range_t *r = find(a);
        if (!r) { a = (a + g_pg) & ~(g_pg - 1); continue; }
        uintptr_t stop = e < r->end ? e : r->end;
        for (uintptr_t p = (a - r->base) >> g_shift, q = (stop - 1 - r->base) >> g_shift; p <= q; p++) {
            if (rd) rd_check(r, p, 1, dm_host_write_pc, 1, r->base + (p << g_shift));
            for (;;) { uint8_t st = PG_PROT;   // 见 BUSY 等完要重看：对方可能是在设只读，等完这页是 PROT，照样得解 | After waiting out BUSY, look again: the other side may have been setting read-only, in which case the page is PROT after the wait and still has to be unprotected
                if (atomic_compare_exchange_strong(&r->state[p], &st, PG_BUSY)) { unprotect_page(r, p); atomic_fetch_add(&g_preps, 1); drec_add(r->base + (p << g_shift), dm_host_write_pc, 0, 1); break; }
                if (st != PG_BUSY || !busy_wait(r, p, BUSY_SITE_PREP)) break; }
        }
        a = stop;
    }
}
static void prep_write(void *addr, size_t len) { prep_write_ex(addr, len, 1); }

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
            if (st != PG_BUSY || !busy_wait(r, p, BUSY_SITE_GPU)) break; }
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
    _Atomic uint64_t *gen = r->gen, *blk = r->blk, *rd = r->rd; uint16_t *rm = r->rm; uint8_t *rk = r->rk; _Atomic uint8_t *nr = r->nr;
    if (r->cap < np) {
        state = calloc(np, 1); gen = calloc(np, sizeof *gen); blk = calloc(blk_count(np), sizeof *blk); rd = calloc(np, sizeof *rd); rm = calloc(np, sizeof *rm); rk = calloc(np, 1); nr = calloc(np, 1);
        if (!state || !gen || !blk || !rd || !rm || !rk || !nr) die("脏页表分配失败（%zu 页）", np);
        // 旧数组不 free：信号处理里可能仍有极短暂的陈旧读者，泄漏量 = 每页 9 字节。
        // The old array is not freed: the signal handler may still have a very brief stale reader; leak = 9 bytes per page.
        r->cap = np;
    } else {
        memset((void *)state, 0, np); memset((void *)gen, 0, np * sizeof *gen); memset((void *)blk, 0, blk_count(np) * sizeof *blk); memset((void *)rd, 0, np * sizeof *rd); memset(rm, 0, np * sizeof *rm); memset(rk, 0, np); memset((void *)nr, 0, np);
    }
    r->base = base; r->end = end; r->state = state; r->gen = gen; r->blk = blk; r->rd = rd; r->rm = rm; r->rk = rk; r->nr = nr; atomic_store(&r->latest, 0);
    if (slot == n) atomic_store(&g_nr, n + 1);
    atomic_store_explicit(&r->live, 1, memory_order_release);
    pthread_mutex_unlock(&g_slot_mu);
}
// 池销毁：先把仍受保护的页还原成可写（池内存归还游戏后可能另作他用），再标失效
// Pool destruction: first restore still-protected pages to writable (the pool memory may be reused for something else once returned to the game), then mark invalid
void nvn_dirty_remove_range(const void *mem) {
    pthread_mutex_lock(&g_slot_mu);
    range_t *r = find((uintptr_t)mem);
    if (r) { prep_write_ex((void *)r->base, r->end - r->base, 0); atomic_store_explicit(&r->live, 0, memory_order_release); atomic_fetch_sub(&g_live, 1); }
    pthread_mutex_unlock(&g_slot_mu);
}
int nvn_dirty_tracked(const void *addr, size_t len) {
    range_t *r = find((uintptr_t)addr); return r && (uintptr_t)addr + len <= r->end;
}
// 设只读：先把页标成 BUSY，mprotect 落地后才标 PROT（次序与不变式见文件头的状态说明）
// Set read-only: mark the page BUSY first, and mark it PROT only after mprotect has landed (for the order and the invariant see the state notes at the top of the file)
void nvn_dirty_protect(const void *addr, size_t len) {
    /* g_slot_mu from the lookup to the last mprotect (nxnative af616f54, PR #686 there): pool retirement and creation
       hold it too, so a range cannot be retired (its protected pages made writable, the range marked dead) or its slot
       reused between the lookup and the page states below. Without it a retirement finishing in that window left the
       retired storage read-only with no live range to release it (a host read() into it failed with EFAULT; a game
       write faulted with no owner). The page-fault and host pre-write paths still take no lock, so a BUSY page they
       hold finishes while this waits (busy_wait). */
    pthread_mutex_lock(&g_slot_mu);
    if (!len || !nvn_dirty_tracked(addr, len)) { pthread_mutex_unlock(&g_slot_mu); return; }
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
        if (!took && st == PG_BUSY && busy_wait(r, i, BUSY_SITE_PROTECT)) i--;   // this page again
    }
    pthread_mutex_unlock(&g_slot_mu);
}
#endif
uint64_t nvn_dirty_now(void) { return atomic_load(&g_now); }
void nvn_dirty_read_fence_setup(const uint64_t *done, const uint64_t *committed) { g_rd_done = done; g_rd_committed = committed; if (!g_rd_key_ok && !pthread_key_create(&g_rd_key, NULL)) g_rd_key_ok = 1; }
void nvn_dirty_read_wait_exempt(int on) { if (g_rd_key_ok) pthread_setspecific(g_rd_key, (void *)(intptr_t)on); }
void nvn_dirty_read_wait_stats(uint64_t *marks, uint64_t *pages, uint64_t *waits, uint64_t *ns, uint64_t *max_ns, uint64_t *timeouts) {
    *marks = atomic_load(&g_rd_marks); *pages = atomic_load(&g_rd_mark_pages); *waits = atomic_load(&g_rd_waits); *ns = atomic_load(&g_rd_wait_ns);
    *max_ns = atomic_load(&g_rd_wait_max_ns); *timeouts = atomic_load(&g_rd_timeouts);
}
int nvn_dirty_read_mark(const void *addr, size_t len, uint64_t fence, uint16_t marker, uint8_t kind) {
    if (!len || !g_rd_done) return 0;
    range_t *r = find((uintptr_t)addr); if (!r || !r->rd || (uintptr_t)addr + len > r->end) return 0;
    uintptr_t a = (uintptr_t)addr, p = (a - r->base) >> g_shift, q = (a + len - 1 - r->base) >> g_shift;
    for (uintptr_t i = p; i <= q; i++) { uint64_t v = atomic_load_explicit(&r->rd[i], memory_order_relaxed); while (v < fence && !atomic_compare_exchange_weak(&r->rd[i], &v, fence)) {}
        if (r->rm) { r->rm[i] = marker; r->rk[i] = kind; } }   // the latest marker wins (the wait log names it; plain stores, diagnostic)
    atomic_fetch_add(&g_rd_marks, 1); atomic_fetch_add(&g_rd_mark_pages, q - p + 1);
    nvn_dirty_protect(addr, len);   // pages already read-only are left alone; a page the game wrote since is set read-only again so its next write faults
    return 1;
}
void nvn_dirty_noread_mark(const void *addr, size_t len) {
    if (!len) return;
    range_t *r = find((uintptr_t)addr); if (!r || !r->nr || (uintptr_t)addr + len > r->end) return;
    g_nr_active = 1;
    uintptr_t a = (uintptr_t)addr, p = (a - r->base) >> g_shift, q = (a + len - 1 - r->base) >> g_shift;
    for (uintptr_t i = p; i <= q; i++) {
        if (atomic_exchange(&r->nr[i], 1)) continue;
        if (atomic_load(&r->state[i]) == PG_BUSY) busy_wait(r, i, BUSY_SITE_NOREAD);
        mp(r->base + (i << g_shift), g_pg, PROT_NONE);   // the state machine's protection comes back on the first access (on_fault)
    }
}
int nvn_dirty_noread_events(nvn_nr_event *out, int max) {
    uint64_t n = atomic_load(&g_nre_n), d = g_nre_drained; if (n - d > 4096) d = n - 4096;
    int k = 0; for (; d < n && k < max; d++) out[k++] = g_nre[d % 4096];
    g_nre_drained = d; return k;
}
int nvn_dirty_read_wait_events(nvn_rd_event *out, int max) {
    uint64_t n = atomic_load(&g_rde_n), d = g_rde_drained; if (n - d > 4096) d = n - 4096;
    int k = 0; for (; d < n && k < max; d++) out[k++] = g_rde[d % 4096];
    g_rde_drained = d; return k;
}
#if !defined(GAME_BOTW)
/* OOT (2026-11-08): the byte ranges of [addr, addr+len) on pages written after stamp, for an incremental rescan.
   Calls fn(lo, hi) for each run of such pages, clipped to the range. Returns 0 when the range is not tracked (the
   caller rescans everything), 1 otherwise. Same generations as nvn_dirty_since. */
int nvn_dirty_runs_since(const void *addr, size_t len, uint64_t stamp, void (*fn)(void *ctx, uintptr_t lo, uintptr_t hi), void *ctx) {
    if (!len || !nvn_dirty_tracked(addr, len)) return 0;
    range_t *r = find((uintptr_t)addr);
    if (atomic_load(&r->latest) <= stamp) return 1;
    uintptr_t a = (uintptr_t)addr, e = a + len, p = (a - r->base) >> g_shift, q = (e - 1 - r->base) >> g_shift;
    uintptr_t run = 0; int open = 0;
    for (uintptr_t i = p; i <= q + 1; i++) {
        int dirty = 0;
        if (i <= q) {
            if (g_dirty_fast && !(i & (BLK_PAGES - 1)) && i + BLK_PAGES - 1 <= q && atomic_load(&r->blk[i >> BLK_SHIFT]) <= stamp) {
                if (open) { uintptr_t lo = r->base + (run << g_shift), hi = r->base + (i << g_shift); fn(ctx, lo < a ? a : lo, hi > e ? e : hi); open = 0; }
                i += BLK_PAGES - 1; continue;
            }
            dirty = atomic_load(&r->gen[i]) > stamp;
        }
        if (dirty && !open) { run = i; open = 1; }
        if (!dirty && open) { uintptr_t lo = r->base + (run << g_shift), hi = r->base + (i << g_shift); fn(ctx, lo < a ? a : lo, hi > e ? e : hi); open = 0; }
    }
    return 1;
}
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
    _Atomic uint64_t *gen = r->gen, *blk = r->blk, *rd = r->rd; uint16_t *rm = r->rm; uint8_t *rk = r->rk; _Atomic uint8_t *nr = r->nr;
    if (r->cap < np) {
        state = calloc(np, 1); gen = calloc(np, sizeof *gen); blk = calloc(blk_count(np), sizeof *blk); rd = calloc(np, sizeof *rd); rm = calloc(np, sizeof *rm); rk = calloc(np, 1); nr = calloc(np, 1);
        if (!state || !gen || !blk || !rd || !rm || !rk || !nr) die("脏页表分配失败（%zu 页）", np);
        r->cap = np;   // the old arrays are not freed (see nvn_dirty_add_range above)
    } else {
        memset((void *)state, 0, np); memset((void *)gen, 0, np * sizeof *gen); memset((void *)blk, 0, blk_count(np) * sizeof *blk); memset((void *)rd, 0, np * sizeof *rd); memset(rm, 0, np * sizeof *rm); memset(rk, 0, np); memset((void *)nr, 0, np);
    }
    r->base = base; r->end = end; r->state = state; r->gen = gen; r->blk = blk; r->rd = rd; r->rm = rm; r->rk = rk; r->nr = nr; atomic_store(&r->latest, 0);
    g_rmem[slot] = (uintptr_t)mem; atomic_store(&g_rdying[slot], 0);
    if (slot == n) atomic_store(&g_nr, n + 1);
    atomic_store_explicit(&r->live, 1, memory_order_release);
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
        prep_write((void *)r->base, r->end - r->base); atomic_store_explicit(&r->live, 0, memory_order_release); atomic_fetch_sub(&g_live, 1); found++;
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
        if (!took && st == PG_BUSY && busy_wait(r, i, BUSY_SITE_PROTECT_SEG)) i--;   // this page again
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
                if (st != PG_BUSY || !busy_wait(r, p, BUSY_SITE_GPU)) break; }
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
#if defined(GAME_BOTW)
int nvn_dirty_runs_since(const void *addr, size_t len, uint64_t stamp, void (*fn)(void *ctx, uintptr_t lo, uintptr_t hi), void *ctx) { (void)addr; (void)len; (void)stamp; (void)fn; (void)ctx; return 0; }   // pools span ranges there: rescan all
#endif
