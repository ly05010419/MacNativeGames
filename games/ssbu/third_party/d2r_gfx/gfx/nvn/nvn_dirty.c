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
enum { BUSY_WAIT_MS = 2000 };
// 跟踪区间 = 各内存池（MemoryPoolInitialize 登记）。池销毁后复用 live=0 的槽，信号处理里无锁扫描。
// Tracked ranges = the memory pools (registered by MemoryPoolInitialize). After a pool is destroyed its live=0 slot is reused; the signal handler scans without locks.
#define MAX_RANGE 16384 // 原为 256：内存池多于它时超出的池不跟踪，里面的贴图每次取用都当成脏的重传（暗黑实测峰值见内存读数行的「内存池」；塞尔达约 300 个，踩过）。扫描只到已用的槽数，容量大不增加开销 | was 256: with more memory pools than that the excess pools are untracked and their textures are treated as dirty and re-uploaded on every use (for Diablo's measured peak see "memory pools" in the memory readout line; Zelda has about 300 and hit this). The scan only covers slots in use, so a larger capacity costs nothing. TOTK (2026-10-17): 4096 was full in the user's own games and most long test games (about 3,900 live pools in the world, more when moving between regions); now 16384
typedef struct { uintptr_t base, end; _Atomic uint8_t *state; _Atomic uint64_t *gen; size_t cap; _Atomic int live; _Atomic uint64_t latest; } range_t;   // latest (TOTK): highest page generation in the range
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
static range_t *find(uintptr_t a) {
    /* TOTK: the replay asks about the same few pools over and over; the last hits per thread (8, round robin) are checked
       first (a slot reused for another pool fails the bounds or live test and falls through to the scan), so the scan over
       the ~4,000 live pools of the world mostly runs for the first touch of a pool. */
    static __thread range_t *recent[8]; static __thread unsigned next;
    for (int k = 0; k < 8; k++) {
        range_t *l = recent[k];
        if (l && atomic_load_explicit(&l->live, memory_order_acquire) && a >= l->base && a < l->end) return l;
    }
    int n = atomic_load(&g_nr);
    for (int i = 0; i < n; i++) if (atomic_load_explicit(&g_r[i].live, memory_order_acquire) && a >= g_r[i].base && a < g_r[i].end) { recent[next++ & 7] = &g_r[i]; return &g_r[i]; }
    return NULL;
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
static void unprotect_page(range_t *r, uintptr_t p) {   // 调用方已把状态从 PROT 改成 BUSY | The caller has already changed the state from PROT to BUSY
    mp(r->base + (p << g_shift), g_pg, PROT_READ | PROT_WRITE);
    uint64_t g = atomic_fetch_add(&g_now, 1) + 1, l = atomic_load(&r->latest);
    atomic_store(&r->gen[p], g);
    while (l < g && !atomic_compare_exchange_weak(&r->latest, &l, g)) {}
    /* TOTK (#90): "now" moves on once more after the page's generation is stored. A checker that read "now" between the
       first step and the stores found the page clean and kept that "now" as its last look (tex_mtl's now == seen); the page
       is writable from here on, so without a newer "now" every later write went unseen until something else faulted. */
    atomic_fetch_add(&g_now, 1);
    atomic_store(&r->state[p], PG_NONE);
}
static int on_fault(void *addr, int is_write, void *uc) {
    (void)uc;
    range_t *r = is_write ? find((uintptr_t)addr) : NULL;
    if (!r) return 0;
    uintptr_t p = ((uintptr_t)addr - r->base) >> g_shift;
    uint8_t st = PG_PROT;
    if (atomic_compare_exchange_strong(&r->state[p], &st, PG_BUSY)) { unprotect_page(r, p); atomic_fetch_add(&g_faults, 1);
        if (g_drec_on && uc) { _STRUCT_MCONTEXT64 *mc = ((ucontext_t *)uc)->uc_mcontext; drec_add(r->base + (p << g_shift), mc->__ss.__pc, mc->__ss.__lr, 0); }
        return 1; }
    if (st == PG_BUSY) return busy_wait(r, p);   // 别的线程正在改这页的保护：等它改完重执行（改成了只读就再缺页一次、走上面那一支） | Another thread is changing this page's protection: wait for it to finish and re-execute (if it became read-only, it faults once more and takes the branch above)
    return atomic_load(&r->gen[p]) != 0;   // 刚被别的线程解掉（本次缺页发生在解之前）：重执行；从未保护过的页不是我们的 | Just unprotected by another thread (this fault happened before the unprotect): re-execute; pages that were never protected are not ours
}
static void prep_write(void *addr, size_t len) {
    uintptr_t a = (uintptr_t)addr, e = a + len;
    while (len && a < e) {
        range_t *r = find(a);
        if (!r) { a = (a + g_pg) & ~(g_pg - 1); continue; }
        uintptr_t stop = e < r->end ? e : r->end;
        for (uintptr_t p = (a - r->base) >> g_shift, q = (stop - 1 - r->base) >> g_shift; p <= q; p++) {
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

uint64_t nvn_dirty_now(void) { return atomic_load(&g_now); }
/* BOTW versions of the functions above: a pool's pages can lie in several ranges (its own gaps and the neighbours' shared
   pages, see g_rmem), so the lookups walk range by range; a page in no live range makes the span untracked. */
static void range_put(int slot, int n, uintptr_t base, uintptr_t end, const void *mem) {   // under g_slot_mu
    { int lv = atomic_fetch_add(&g_live, 1) + 1, pk = atomic_load(&g_live_peak); while (lv > pk && !atomic_compare_exchange_weak(&g_live_peak, &pk, lv)) {} }
    range_t *r = &g_r[slot];
    size_t np = (end - base) >> g_shift;
    _Atomic uint8_t *state = r->state;
    _Atomic uint64_t *gen = r->gen;
    if (r->cap < np) {
        state = calloc(np, 1); gen = calloc(np, sizeof *gen);
        if (!state || !gen) die("脏页表分配失败（%zu 页）", np);
        r->cap = np;   // the old arrays are not freed (see nvn_dirty_add_range above)
    } else {
        memset((void *)state, 0, np); memset((void *)gen, 0, np * sizeof *gen);
    }
    r->base = base; r->end = end; r->state = state; r->gen = gen; atomic_store(&r->latest, 0);
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
    if (!inited) { inited = 1; g_pg = (uintptr_t)getpagesize(); g_shift = __builtin_ctzl(g_pg); mach_timebase_info(&g_tb); dm_fault_hook_register(on_fault); dm_guest_write_prep_register(prep_write); g_drec_on = getenv("DM_NVN_TILELOG") != NULL; }
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
        if (atomic_load(&r->latest) > stamp)
            for (uintptr_t i = (a - r->base) >> g_shift, q = (stop - 1 - r->base) >> g_shift; i <= q; i++) if (atomic_load(&r->gen[i]) > stamp) return 1;
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
