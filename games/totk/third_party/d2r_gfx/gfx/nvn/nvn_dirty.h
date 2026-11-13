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
int nvn_dirty_runs_since(const void *addr, size_t len, uint64_t stamp, void (*fn)(void *ctx, uintptr_t lo, uintptr_t hi), void *ctx);   // OOT: runs of pages written after stamp (0: untracked)
void *nvn_dirty_gpu_write_begin(void *addr, size_t len);   // write guest memory on behalf of the GPU: not counted as a CPU write
void nvn_dirty_gpu_write_end(void *tok);
typedef struct { uintptr_t page; uint64_t pc, lr; uint32_t src; uint64_t t; } nvn_dirty_rec;   // src 0: guest write (fault), 1: host write; t: CLOCK_UPTIME_RAW ns of the unprotection
int nvn_dirty_recent_snapshot(nvn_dirty_rec *out, int max);   // OOT_RACE_CHECK: the latest records (at most max), oldest first
void nvn_dirty_recent_enable(void);   // turn the record ring on without DM_NVN_TILELOG
/* OOT (2026-11-09) GPU read fences: the pages a pending GPU submission reads are marked with its fence; a CPU write to such
   a page (page fault, or a host write) waits until the completed-fence counter reaches the mark. The thread encoding a
   submission is exempt while it encodes (it commits the fence it would wait for). */
void nvn_dirty_read_fence_setup(const uint64_t *done, const uint64_t *committed);   // nvn_mtl.m's highest completed / last committed fence counters
int nvn_dirty_read_mark(const void *addr, size_t len, uint64_t fence, uint16_t marker, uint8_t kind);   // mark (marker id / kind: who, for the wait log) and set read-only; 0: untracked
typedef struct { uintptr_t page; uint64_t fence, ns, pc; uint16_t marker; uint8_t kind, host; } nvn_rd_event;
int nvn_dirty_read_wait_events(nvn_rd_event *out, int max);   // drain the recorded waits, oldest first (single consumer)
/* OOT_NOREAD_PROBE (diagnostic): pages the GPU may have written are made inaccessible; the first CPU access (read or write)
   is recorded with its pc and the page made accessible again. Tells which GPU results the CPU reads, and where. */
void nvn_dirty_noread_mark(const void *addr, size_t len);
typedef struct { uintptr_t addr; uint64_t pc, t; uint8_t write; } nvn_nr_event;
int nvn_dirty_noread_events(nvn_nr_event *out, int max);   // drain, oldest first (single consumer)
void nvn_dirty_read_wait_exempt(int on);   // this thread: 1 while it encodes a submission
void nvn_dirty_read_wait_stats(uint64_t *marks, uint64_t *pages, uint64_t *waits, uint64_t *ns, uint64_t *max_ns, uint64_t *timeouts);
int nvn_dirty_recent(uintptr_t lo, uintptr_t hi, nvn_dirty_rec *out, int max);   // latest unprotections in [lo, hi) (DM_NVN_TILELOG) // 覆盖页在戳之后被写过 → 1 | Covered pages were written after the stamp -> 1
void nvn_dirty_stats(uint64_t *faults, uint64_t *preps);           // 累计：游戏写触发的缺页次数、宿主写前解保护的页数 | Cumulative: number of page faults triggered by game writes, number of pages unprotected before host writes
void nvn_dirty_mp_stats(uint64_t *calls, uint64_t *pages, uint64_t *ns);   // 累计：本模块 mprotect 调用次数、覆盖页数、耗时 | Cumulative: this module's mprotect call count, pages covered, time spent
void nvn_dirty_slot_stats(int *live, int *used_slots, uint64_t *adds_total); // 当前存活槽、槽表高水位、登记总次数 | Currently live slots, slot table high-water mark, total number of registrations
void nvn_dirty_fast_stats(int *on, uint64_t *blk_skips, uint64_t *hash_hits, uint64_t *scans);   // TOTK_DIRTY_FAST: blocks skipped by nvn_dirty_since, range lookups answered by the page table / by the scan
