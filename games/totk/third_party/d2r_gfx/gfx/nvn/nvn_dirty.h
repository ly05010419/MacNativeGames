// 客体内存写保护脏页跟踪（nvn_dirty.c）
// Write-protect dirty-page tracking for guest memory (nvn_dirty.c)
#pragma once
#include <stddef.h>
#include <stdint.h>

void nvn_dirty_add_range(const void *mem, size_t size);           // 登记一个内存池为跟踪区间（首次调用时注册缺页钩子与宿主写前钩子） | Register a memory pool as a tracked range (the first call registers the page-fault hook and the host pre-write hook)
void nvn_dirty_stat(int *live, int *peak, int *cap);                // 在册的内存池数、峰值、跟踪表容量（内存读数行用） | number of registered memory pools, peak, tracking table capacity (for the memory readout line)
void nvn_dirty_remove_range(const void *mem);                     // 池销毁：受保护页还原可写并标失效 | Pool destruction: restore protected pages to writable and mark invalid
int nvn_dirty_tracked(const void *addr, size_t len);
void nvn_dirty_protect(const void *addr, size_t len);              // 覆盖页设只读 | Set the covered pages read-only
uint64_t nvn_dirty_now(void);                                      // 当前代号：上传前取，作为该份内容的戳 | Current generation: taken before upload, as the stamp of that content
int nvn_dirty_since(const void *addr, size_t len, uint64_t stamp);
void *nvn_dirty_gpu_write_begin(void *addr, size_t len);   // write guest memory on behalf of the GPU: not counted as a CPU write
void nvn_dirty_gpu_write_end(void *tok);
typedef struct { uintptr_t page; uint64_t pc, lr; uint32_t src; } nvn_dirty_rec;   // src 0: guest write (fault), 1: host write
int nvn_dirty_recent(uintptr_t lo, uintptr_t hi, nvn_dirty_rec *out, int max);   // latest unprotections in [lo, hi) (DM_NVN_TILELOG) // 覆盖页在戳之后被写过 → 1 | Covered pages were written after the stamp -> 1
void nvn_dirty_stats(uint64_t *faults, uint64_t *preps);           // 累计：游戏写触发的缺页次数、宿主写前解保护的页数 | Cumulative: number of page faults triggered by game writes, number of pages unprotected before host writes
void nvn_dirty_mp_stats(uint64_t *calls, uint64_t *pages, uint64_t *ns);   // 累计：本模块 mprotect 调用次数、覆盖页数、耗时 | Cumulative: this module's mprotect call count, pages covered, time spent
void nvn_dirty_slot_stats(int *live, int *used_slots, uint64_t *adds_total); // 当前存活槽、槽表高水位、登记总次数 | Currently live slots, slot table high-water mark, total number of registrations
void nvn_dirty_fast_stats(int *on, uint64_t *blk_skips, uint64_t *hash_hits, uint64_t *scans);   // TOTK_DIRTY_FAST: blocks skipped by nvn_dirty_since, range lookups answered by the page table / by the scan
/* TOTK_PIPELINE (nvn_dirty.c "one frame deeper", used by nvn_mtl.m totk_pipeline.h): page state and in-place ranges of
   the deeper frame wait. Mode: 0 off, 1 on, 2 dry run (TOTK_PIPELINE, read once). */
enum { NVN_PLS_HOLD, NVN_PLS_HOLD_HOST, NVN_PLS_HOLD_NS, NVN_PLS_TIMEOUT, NVN_PLS_WOULD, NVN_PLS_HOSTSKIP, NVN_PLS_PROBATION,
       NVN_PLS_REPROT, NVN_PLS_UNTRACKED, NVN_PLS_WATCH_READ, NVN_PLS_WATCH_WRITE, NVN_PLS_PASS, NVN_PLS_URING_OVER,
       NVN_PLS_PASSKEEP, NVN_PLS_SPANPROT, NVN_PLS_BIGSPAN, NVN_PLS_WWPASS, NVN_PLS_N };
enum { NVN_PL_BT = 3 };
typedef struct { uint64_t pc, addr; uint32_t frame, host; uint64_t rfence, released, bt[NVN_PL_BT]; uint32_t kinds; } nvn_pl_hold_rec;   // kinds: 1 met a read range, 2 a GPU write range   // bt: lr, then the frame chain   // host: 0 guest write fault, 1 announced host write, 2 / 3 read / write of a watched GPU-written page
uint64_t nvn_dirty_range_gen(void);   // changes whenever a tracked range goes live or dies
int nvn_pl_mode(void);
int nvn_pl_wwhold(void);   // TOTK_PIPELINE_WWHOLD=1
void nvn_pl_set_done(uint64_t (*done)(void));                   // completed fence, signal-safe
void nvn_pl_set_frame(uint32_t frame);                          // the frame CPU writes are stamped with (pl_wfr)
void nvn_pl_set_released(uint64_t released, uint64_t vdone);    // newest fence released early; dry run: the fence treated as complete
int nvn_pl_classify(const void *addr, size_t len, uint32_t keep, uint32_t probe, int first, void (*prot)(uintptr_t lo, uintptr_t hi));   // -1 untracked, 1 snapshot, 0 static; prot: protects the probation runs (NULL: inline)
void nvn_pl_gpu_write(const void *addr, size_t len, uint64_t fence);
int nvn_pl_gpu_written(const void *addr, size_t len, uint64_t done);
int nvn_pl_inflight(const void *addr, size_t len, uint64_t done, uint64_t released);   // overlaps an in-place range of a submission in (done, released]
void nvn_pl_publish(uint64_t fence, const uintptr_t *lo, const uintptr_t *hi, int n, int writes);   // in-place reads (writes 0) or GPU writes (1), sorted by start
int nvn_pl_reprotect(uint64_t done, uint64_t released, const uintptr_t *nlo, const uintptr_t *nhi, int nn);
void nvn_pl_protect_span(const void *addr, size_t len);   // translate thread: a non-static in-place range, protected at its publication
void nvn_pl_stats(uint64_t v[NVN_PLS_N]);
int nvn_pl_holds(nvn_pl_hold_rec *out, int max);
/* TOTK_PIPELINE=2 with TOTK_PIPELINE_RWATCH (diagnostic): the pages GPU work of the released frame wrote are made
   inaccessible until the next frame wait returns; the first CPU access of each is recorded (nvn_pl_holds, host 2 / 3)
   and the page goes back to what the tracker wants. nvn_pl_watch ends the previous watch first. */
int nvn_pl_watch(const uintptr_t *pages, int n);
