/* Included only by nvn_mtl.m (before mtl_sub_split). TOTK_PIPELINE (experiment, off by default): the game's frame wait
   is one frame deeper, so the GPU already holds frame N+1 when it finishes frame N.

   The frame loop (1.2.1, hle_nvn.c "one frame deeper"): the main thread records frame N (main+0xb592a8), the render
   thread submits it and fences sys+0x1d8 (main+0x8c1474), the main thread runs the game logic of N+1 and then waits for
   that fence (main+0xb23d58) before it records N+1. The GPU idled from the end of N until N+1's first command buffer
   arrived (wake-up, recording, submit call, first chunk: ~1.6 ms a frame at Kakariko, plus translation starvation).
   With TOTK_PIPELINE=1 that wait returns once frame N-1 has completed and frame N has been translated (mtl_pl_release):
   frame N still runs on the GPU while the game records N+1 and runs the logic of N+2. Everything the game could see
   differently is made to look as if N had completed, without holding the game's hot path:
   - Reads in place: the GPU reads vertex / index / storage / uniform / copy-source memory of the pools directly (zero-
     copy buffers). A binding whose pages the CPU wrote recently ("volatile", nvn_dirty.c pl_wfr, or on probation) is
     copied into this submission's arena when it is translated and the GPU reads the copy (pl_read: the way uniforms are
     captured at the submit call). A static decision is kept per range across submissions until a page of it is
     written (nvn_dirty_since), so unchanged bindings cost a hash lookup. Every other access (in place, and GPU writes)
     is an exact byte range of the submission, published at its commit (nvn_pl_publish). At the release the pages of
     those ranges that CPU writes unprotected meanwhile (or that belong to non-static ranges) are write-protected again;
     a CPU write while N still runs that hits one of N's ranges (the 64-byte line written) waits in the fault handler
     until the GPU is done (a hold). A write elsewhere on such a page goes through ("passes"): the game keeps CPU data
     next to GPU data in 16 KiB pages (main+0x8d4574 flips a double-buffer index stored in a page the GPU writes).
   - GPU writes are never snapshotted (later readers must see the GPU's result).
   - CPU-visible results: event signals and the constant counter reports (TIMESTAMP / TIMESTAMP_TOP, which write counter
     0: the game re-arms its conditional clears by storing 1 over them right after its frame wait, see cond_eval) are
     written at the release, in the order the game expects, and their pending records retire then; the completion does not
     write them again. Occlusion query sums are only known at the completion: the game reads the previous ones (one frame
     staler); TOTK_PIPELINE_QUERY=visible writes "visible" (1 << 20) at the release instead, replaced at the completion.
     The screen depth statistics the CPU copies right after its frame wait (cs 4c8f872d -> cd68063a -> 65a96cf0, read by
     main+0xcc1d7c) are compute-written storage buffers at a stable address in the last command buffer: they are
     redirected to a host buffer and the newest completed copy is written into guest memory at each release (the
     snapshot ring of TOTK_OVERLAP, perf/overlap ae6209a): the game gets the statistics of N-1 instead of N.
   - Destroying or reading back state (pool / program finalize, ReadTexels, write-backs, virtual pool remaps: x_drain)
     waits until every released submission has really completed (mtl_pl_gate); an invalidate does only when it overlaps
     an exact GPU write range of a released, unfinished submission (the game's readback buffers are double-buffered:
     main+0x8d452c / main+0xcc1d7c read the slot the previous frame wrote). The present ring, fences, arenas, allocators
     and deferred releases keep using the real completion.
   TOTK_PIPELINE=2 (dry run): the frame wait stays exact; at its return the previous frame's fence plays "completed" for
   one frame, so the writes that would have been held are counted (nothing waits) and the snapshots are made as with 1.
   Knobs: TOTK_PIPELINE_SNAP_MAX=<bytes> (largest snapshot, default 2 MiB, at most 4 MiB), TOTK_PIPELINE_BUDGET=<bytes>
   (snapshot bytes per submission, default 96 MiB), TOTK_PIPELINE_KEEP=<frames> (a written page stays volatile, default
   600), TOTK_PIPELINE_PROBE=<frames> (probation, default 4), TOTK_PIPELINE_FIRST=0 (pages seen for the first time are
   not put on probation), TOTK_PIPELINE_DSTATS=0 (no depth statistics ring), TOTK_PIPELINE_TEXHOLD=0 (texture uploads
   read on the GPU are not noted), TOTK_PIPELINE_RWATCH=<n> (dry run: from submission n, default 1800 for 1, watch the
   pages GPU work wrote), TOTK_PIPELINE_LOG=1 (first releases, snapshots and invalidates in detail),
   TOTK_PIPELINE_XCOMP=<percent> (the translate thread's declared real-time computation, default 90 % of the present
   period; 50 without the pipeline), TOTK_PIPELINE_ECOMP=<percent> (the encode stage becomes a real-time thread with this
   computation, default 50; 0 = it stays user-interactive): hle_nvn.c x_thread. The stats lines come every 600
   submissions ("translate-thread hooks per submission": the cost of these hooks on the translate thread); hle_nvn.c logs
   "cores to frame N" every 300 frames in every mode (P / E core time and clocks of the translate and encode threads, the
   process's CPU time, P share, waiting for a core, CPU energy). */

#define PL_SLOTS 16
#define PL_MEMO 16384
#define PL_SCACHE 65536
#define PLDS_SNAPS 64
#define PLDS_NEAR 1024   // learned ranges this close to a snapshot join it (the slots of one index-times-size buffer)
#define PLDS_MAX (256u << 10)
#define PLDS_RING 4
static const char *const g_plk_name[PLK_N] = {"vertex", "index", "storage", "uniform", "copy source", "texture source"};
static uint32_t g_pl_keep = 600, g_pl_probe = 4; static int g_pl_first = 1, g_pl_query_vis, g_pl_log, g_pl_dsnap = 1, g_pl_texhold = 1, g_pl_htime;
static uint64_t g_pl_rwatch;   // dry run: watch from this submission (0 off)
static uint64_t g_pl_snap_max = 2u << 20, g_pl_budget = 96u << 20, g_pl_sub_bytes, g_pl_first_max = 64u << 10;
static uint64_t g_pl_rel;   // under g_mu: newest fence whose frame wait was released early (mode 1)
static int g_pl_wprot;      // TOTK_PIPELINE_WWHOLD=1: GPU write ranges are protected too (a CPU write that meets only GPU writes passes otherwise)
static void pl_cfg_once(void) {
    const char *e;
    g_pl = nvn_pl_mode();
    if (g_pl <= 0) return;
    if ((e = getenv("TOTK_PIPELINE_SNAP_MAX"))) { uint64_t v = strtoull(e, NULL, 0); g_pl_snap_max = v > (4u << 20) ? (4u << 20) : v; }
    if ((e = getenv("TOTK_PIPELINE_BUDGET"))) g_pl_budget = strtoull(e, NULL, 0);
    if ((e = getenv("TOTK_PIPELINE_KEEP")) && atoi(e) > 0) g_pl_keep = (uint32_t)atoi(e);
    if ((e = getenv("TOTK_PIPELINE_PROBE")) && atoi(e) > 0) g_pl_probe = (uint32_t)atoi(e);
    if ((e = getenv("TOTK_PIPELINE_FIRST"))) g_pl_first = atoi(e) != 0;
    if ((e = getenv("TOTK_PIPELINE_FIRST_MAX"))) g_pl_first_max = strtoull(e, NULL, 0);
    if ((e = getenv("TOTK_PIPELINE_QUERY"))) g_pl_query_vis = !strcmp(e, "visible");
    if ((e = getenv("TOTK_PIPELINE_DSTATS"))) g_pl_dsnap = atoi(e) != 0;
    if ((e = getenv("TOTK_PIPELINE_TEXHOLD"))) g_pl_texhold = atoi(e) != 0;
    if ((e = getenv("TOTK_PIPELINE_LOG"))) g_pl_log = atoi(e) > 0 ? 40 : 0;
    if ((e = getenv("TOTK_PIPELINE_HOOKTIME"))) g_pl_htime = atoi(e) != 0;
    g_pl_wprot = nvn_pl_wwhold();
    if ((e = getenv("TOTK_PIPELINE_RWATCH")) && g_pl == 2 && atoll(e) > 0) g_pl_rwatch = atoll(e) == 1 ? 1800 : (uint64_t)atoll(e);
    MLOG("TOTK_PIPELINE=%d: %s; snapshots up to %llu KB, volatile for %u frames, probation %u frames%s; occlusion results %s; depth statistics ring %s%s",
         g_pl, g_pl == 1 ? "the frame wait returns once the previous frame has completed (one frame deeper)" : "dry run (exact frame wait, would-be holds counted)",
         (unsigned long long)(g_pl_snap_max >> 10), g_pl_keep, g_pl_probe, g_pl_first ? " (also for pages seen the first time)" : "",
         g_pl_query_vis ? "\"visible\" until known" : "of the frame before (stale)", g_pl_dsnap ? "on" : "off",
         g_pl_rwatch ? [NSString stringWithFormat:@"; GPU-written pages watched from submission %llu", (unsigned long long)g_pl_rwatch].UTF8String : "");
}
static void pl_cfg(void) { static dispatch_once_t once; dispatch_once(&once, ^{ pl_cfg_once(); }); }
static uint64_t pl_done_fn(void) { return __atomic_load_n(&g_done, __ATOMIC_ACQUIRE); }   // signal-safe
static uint64_t pl_now(void) { return mach_absolute_time(); }   // ticks (cheap); pl_ns converts
static double pl_ns(uint64_t t) { static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb); return (double)t * tb.numer / tb.denom; }

/* ---------- statistics (replay thread unless atomic) ---------- */
static struct {
    uint64_t subs, over_budget, snaps[PLK_N], snap_bytes, snap_ns, memo_hits, cache_hits, live[PLK_N], live_static, live_gpuw, live_big, live_noshot, live_untracked;
    uint64_t ranges, nonstatic, writes, snap_drops, dsnap_redirects, ns, t_decide, t_commit, decides, dc[7];
} g_pls_st;
static _Atomic uint64_t g_pl_rel_t[3], g_pl_pool_skips;   // release on the game thread: early results / protection / snapshots published
static _Atomic uint64_t g_pl_waits, g_pl_early, g_pl_lead_ns, g_pl_lead_n, g_pl_lead_max, g_pl_gate_n, g_pl_gate_ns, g_pl_inval_n, g_pl_inval_gates, g_pl_rel_ns, g_pl_rel_n;
static _Atomic uint64_t g_plds_pub, g_plds_merge, g_plds_conflict, g_plds_skip, g_plds_act, g_plds_learned, g_plds_grown, g_plds_noslot, g_plds_gaveup;

/* ---------- translate-thread cost of the hooks (replay thread): calls counted, time sampled 1 in 16 (pseudo-random, so
   the fixed call pattern of a draw does not alias), outermost hook only ---------- */
enum { PLH_REDIR, PLH_VB, PLH_IB, PLH_READ, PLH_LIVE, PLH_WRITE, PLH_BEGIN, PLH_COMMIT, PLH_N };
/* 2026-10-25: even sampled, the clock reads were ~1.3% of the translate thread at Kakariko (prof-up-kak3: pl_read,
   plds_redirect, bind_vertex_streams); the times are diagnostics, so they are taken only with TOTK_PIPELINE_HOOKTIME=1
   (the stats lines then show 0 ms; call counts stay). g_pl_htime is set in pl_cfg_once. */
static uint32_t g_plh_depth, g_plh_rnd = 0x9e3779b9u; static uint64_t g_plh_t[PLH_N], g_plh_c[PLH_N], g_plmi_scans, g_plmi_idx;
static inline uint64_t plh_in(int cat) {
    if (g_plh_depth++) return 0;
    g_plh_c[cat]++; if (!g_pl_htime) return 0;
    g_plh_rnd ^= g_plh_rnd << 13; g_plh_rnd ^= g_plh_rnd >> 17; g_plh_rnd ^= g_plh_rnd << 5;
    return g_plh_rnd & 15 ? 0 : pl_now();
}
static inline void plh_out(int cat, uint64_t t0) { g_plh_depth--; if (t0) g_plh_t[cat] += (pl_now() - t0) * 16; }
/* 2026-10-24: the snapshot / decision timers of pl_read_i and pl_write_i read the clock on every call (~10% of pl_read,
   2% of a draw on the translate thread, tl-dghot); sampled 1 in 16 like the hooks, scaled when added. */
static uint32_t g_pls_rnd = 0x2545f491u;
static inline uint64_t pls_t0(void) { if (!g_pl_htime) return 0; g_pls_rnd ^= g_pls_rnd << 13; g_pls_rnd ^= g_pls_rnd >> 17; g_pls_rnd ^= g_pls_rnd << 5; return g_pls_rnd & 15 ? 0 : pl_now(); }

/* ---------- per submission (replay thread) ---------- */
typedef struct { uint64_t lo, hi, gpu, off; void *buf; uint32_t gen; int8_t snap, kind; } pl_memo;
static pl_memo g_plm[PL_MEMO]; static uint32_t g_plm_gen = 1;
typedef struct { uint64_t lo, hi; unsigned slot; uint32_t gen; } pl_snaprec;
static pl_snaprec *g_plsr; static int g_plsr_n, g_plsr_cap; static uint64_t g_plsr_lo = UINT64_MAX, g_plsr_hi;
typedef struct { uintptr_t lo, hi; } pl_rng;
typedef struct { pl_rng *r; int n, cap; } pl_rvec;
static pl_rvec g_plr_all, g_plr_ns;   // the submission's in-place accesses (exact ranges): all, not static
typedef struct { uintptr_t lo, hi; int kind; } pl_wrng;
static struct { pl_wrng *r; int n, cap; } g_plr_w;   // its GPU writes, with what wrote them (g_pl_wk)
static const char *const g_plw_name[PLW_N] = {"other", "inline uniform update", "renamed uniform updates (flush)", "buffer copy", "texture-to-buffer copy", "storage buffer", "storage pointer (64 KiB taken)", "built-in kernel", "image store"};
static uint64_t *g_plq; static int g_plq_n, g_plq_cap;   // occlusion report addresses (TOTK_PIPELINE_QUERY=visible)
#define PL_PROTQ_RUN(lo, hi) nvn_pl_protect_span((const void *)(lo), (size_t)((hi) - (lo)))
#define PL_PROTQ_DIE(m) MDIE(m)
#include "totk_pl_protq.h"   // write protection of decided spans on a helper thread
static void pl_rvec_add(pl_rvec *v, uintptr_t lo, uintptr_t hi) {
    if (v->n == v->cap) { v->cap = v->cap ? v->cap * 2 : 4096; v->r = realloc(v->r, (size_t)v->cap * sizeof *v->r); if (!v->r) MDIE("pipeline ranges"); }
    v->r[v->n++] = (pl_rng){lo, hi};
}
static void pl_rng_add(uintptr_t lo, uintptr_t hi, int nonstatic, int write) {
    if (!write) pl_rvec_add(&g_plr_all, lo, hi);   // reads; the writes are published as their own set
    if (nonstatic) { pl_rvec_add(&g_plr_ns, lo, hi); if (!write || g_pl_wprot) pl_protq_push(lo, hi); }   // protected before the release (helper); CPU writes until the release then go through the ring. GPU writes: a CPU write that meets only them passes (unless WWHOLD), their pages need no protection
    if (write) {
        if (g_plr_w.n == g_plr_w.cap) { g_plr_w.cap = g_plr_w.cap ? g_plr_w.cap * 2 : 1024; g_plr_w.r = realloc(g_plr_w.r, (size_t)g_plr_w.cap * sizeof *g_plr_w.r); if (!g_plr_w.r) MDIE("pipeline ranges"); }
        g_plr_w.r[g_plr_w.n++] = (pl_wrng){lo, hi, g_pl_wk};
    }
}
/* decisions per submission: -1 GPU-written / untracked, 0 static, 1 volatile */
typedef struct { uint64_t lo, hi; uint32_t gen, wep; int8_t c, pub; uint64_t ulo, uhi; } pl_dm;   // pub: read in place this submission (static), [ulo, uhi): the bytes read (published at the commit); wep: g_pl_wep when decided
static uint32_t *g_pldl; static int g_pldl_n, g_pldl_cap;   // decision entries read in place (static) this submission
static uint32_t g_pl_wep;   // GPU writes noted in this submission (a volatile decision is checked again after one)
static pl_dm g_pldm[PL_MEMO];
static inline unsigned pl_memo_slot(uint64_t lo, uint64_t hi) { uint64_t h = (lo >> 4) ^ (lo >> 19) ^ (hi * UINT64_C(0x9e3779b97f4a7c15)); return (unsigned)(h ^ (h >> 31)) & (PL_MEMO - 1); }
/* a probe sequence of 8: the entry for [lo, hi) of this submission, or a free one (*fresh = 1), or NULL (table crowded) */
static pl_memo *pl_memo_find(uint64_t lo, uint64_t hi, int *fresh) {
    unsigned s = pl_memo_slot(lo, hi);
    for (int i = 0; i < 8; i++, s = (s + 1) & (PL_MEMO - 1)) {
        pl_memo *m = &g_plm[s];
        if (m->gen != g_plm_gen) { *fresh = 1; m->gen = g_plm_gen; m->lo = lo; m->hi = hi; m->snap = -1; return m; }
        if (m->lo == lo && m->hi == hi) { *fresh = 0; return m; }
    }
    return NULL;
}
/* Decisions across submissions: [lo, hi) was static (no page volatile or on probation, every page write-protected) as of
   tracker generation stamp, and stays static while nvn_dirty_since finds no page written after it; or volatile, which is
   reused for PL_VOL_REUSE frames (a page stays volatile KEEP frames after a write: reusing it longer only snapshots a
   little longer). */
typedef struct { uint64_t lo, hi, stamp; uint32_t frame; int8_t c; } pl_sc;   // c 0 static (valid while unwritten), 1 volatile (valid PL_VOL_REUSE frames), -2 untracked (stamp: range generation)
#define PL_VOL_REUSE 32
static pl_sc g_plsc[PL_SCACHE];
static inline pl_sc *pl_sc_slot(uint64_t lo, uint64_t hi) { uint64_t h = (lo >> 4) ^ (lo >> 21) ^ (hi * UINT64_C(0xc2b2ae3d27d4eb4f)); return &g_plsc[(h ^ (h >> 29)) & (PL_SCACHE - 1)]; }

/* ---------- depth statistics: snapshot ring of small compute-written storage buffers (from TOTK_OVERLAP) ---------- */
typedef struct {
    int state;   // 0 free, 1 candidate, 2 active, 3 retiring (whole bindings still use H; at the next submission H goes back to memory), 4 dead (buffers kept until dead_fence)
    int hits, used_rw, base_valid, gone, inval, nmask; uint64_t wait_since;   // wait_since: a candidate waiting for overlapping snapshots to go   // inval: the CPU invalidates it (published there, not at frame waits); nmask: CPU words to merge
    uint64_t lo, hi, last_sub, base_fence, pub_fence, dead_fence;
    __strong id<MTLBuffer> H, S[PLDS_RING]; uint64_t s_fence[PLDS_RING];
    uint64_t *base, *mval; uint8_t *mmask;   // guest words at the last sync; CPU words still to merge into H
} plds_t;
static plds_t g_plds[PLDS_SNAPS];
static os_unfair_lock g_plds_lk = OS_UNFAIR_LOCK_INIT;   // publish (game thread) / pool finalize vs the replay thread
static _Atomic int g_plds_live;
static uint64_t g_plds_sub, g_plds_cand_lo[16], g_plds_cand_hi[16]; static int g_plds_ncand;
static uint64_t g_plds_learn_lo[16], g_plds_learn_hi[16]; static int g_plds_nlearn;   // under g_plds_lk: write ranges the CPU invalidated in flight (pl_invalidate)
static struct { uint64_t lo, hi, until; } g_plds_black[16]; static int g_plds_nblack;   // retired after a conflict: not again for a while
/* Replay thread: the slots in state 2 or 3 (only plds_begin moves a slot into or out of them; 2 -> 3 elsewhere keeps it
   in), with their bounds: the binding checks walk these few instead of all 64 slots (each draw makes several). */
static int g_plds_ln; static uint8_t g_plds_li[PLDS_SNAPS]; static uint64_t g_plds_llo[PLDS_SNAPS], g_plds_lhi[PLDS_SNAPS];
static void plds_live_list(void) {
    int n = 0;
    for (int i = 0; i < PLDS_SNAPS; i++) if (g_plds[i].state == 2 || g_plds[i].state == 3) { g_plds_li[n] = (uint8_t)i; g_plds_llo[n] = g_plds[i].lo; g_plds_lhi[n++] = g_plds[i].hi; }
    g_plds_ln = n;
}
static int plds_blacklisted(uint64_t lo, uint64_t hi) { for (int i = 0; i < g_plds_nblack; i++) if (g_plds_black[i].until > g_plds_sub && lo < g_plds_black[i].hi && hi > g_plds_black[i].lo) return 1; return 0; }
static const char *plds_learn(uint64_t lo, uint64_t hi) {   // game thread; returns what happens (for the log)
    if (hi - lo > PLDS_MAX) return "too big (over 256 KiB): stays a wait";
    if (plds_blacklisted(lo, hi)) return "retired after a conflict, not learned again for 1200 submissions";
    const char *r = "queued for the next submission";
    os_unfair_lock_lock(&g_plds_lk);
    for (int i = 0; i < PLDS_SNAPS; i++) if (g_plds[i].state >= 1 && g_plds[i].state <= 4 && g_plds[i].lo < hi && g_plds[i].hi > lo)
        r = g_plds[i].state == 1 ? "a candidate over it waits for older snapshots to go" : g_plds[i].state == 2 ? "an active snapshot overlaps it partly (union queued)" : "an overlapping snapshot is retiring (union queued)";
    int dup = 0; for (int i = 0; i < g_plds_nlearn; i++) dup |= g_plds_learn_lo[i] == lo && g_plds_learn_hi[i] == hi;
    if (!dup && g_plds_nlearn < 16) { g_plds_learn_lo[g_plds_nlearn] = lo; g_plds_learn_hi[g_plds_nlearn++] = hi; }
    os_unfair_lock_unlock(&g_plds_lk);
    return r;
}
static void plds_conflict(plds_t *s, const char *what, uint64_t a, uint64_t n) {   // replay thread
    os_unfair_lock_lock(&g_plds_lk); if (s->state == 2) s->state = 3; os_unfair_lock_unlock(&g_plds_lk);
    { int k = g_plds_nblack < 16 ? g_plds_nblack++ : (int)(g_plds_sub % 16); g_plds_black[k].lo = s->lo; g_plds_black[k].hi = s->hi; g_plds_black[k].until = g_plds_sub + 1200; }
    if (atomic_fetch_add(&g_plds_conflict, 1) < 24) MLOG("【红】[pipeline] readback snapshot %#llx+%llu retired: %s %#llx+%llu reaches it through memory (not learned again for 1200 submissions)",
                                                         (unsigned long long)s->lo, (unsigned long long)(s->hi - s->lo), what, (unsigned long long)a, (unsigned long long)n);
}
/* A GPU binding of [a, a+n): inside a live snapshot it uses H (1, *buf / *gpu set); partly over one retires it. */
static int plds_redirect_i(uint64_t a, uint64_t n, int rw, void **buf, uint64_t *gpu) {
    if (!g_plds_ln || !a) return 0;
    if (!n) n = 1;
    for (int j = 0; j < g_plds_ln; j++) {
        if (a + n <= g_plds_llo[j] || a >= g_plds_lhi[j]) continue;
        plds_t *s = &g_plds[g_plds_li[j]];
        if (s->state != 2 && s->state != 3) continue;
        if (a >= s->lo && a + n <= s->hi) { *buf = (__bridge void *)s->H; *gpu = s->H.gpuAddress + (a - s->lo); if (rw) s->used_rw = 1; g_pls_st.dsnap_redirects++; return 1; }
        if (a <= s->lo && a + n >= s->hi && n <= PLDS_MAX) {   // a binding over the whole snapshot and more: the snapshot grows to it (no blacklist)
            os_unfair_lock_lock(&g_plds_lk); if (s->state == 2) s->state = 3; os_unfair_lock_unlock(&g_plds_lk);
            atomic_fetch_add(&g_plds_grown, 1); plds_learn(a, a + n);
        } else plds_conflict(s, "binding", a, n);
    }
    return 0;
}
static int plds_redirect(uint64_t a, uint64_t n, int rw, void **buf, uint64_t *gpu) { uint64_t t = plh_in(PLH_REDIR); int r = plds_redirect_i(a, n, rw, buf, gpu); plh_out(PLH_REDIR, t); return r; }
static int plds_redirect_off(uint64_t a, uint64_t n, int rw, void **buf, uint64_t *off) {   // as plds_redirect, with the offset in the host buffer
    uint64_t g; if (!plds_redirect(a, n, rw, buf, &g)) return 0;
    *off = g - ((__bridge id<MTLBuffer>)*buf).gpuAddress; return 1;
}
static void plds_touch(uint64_t a, uint64_t n, const char *what) {   // a copy / update / built-in kernel reaches [a, a+n) through memory
    if (!g_plds_ln || !a || !n) return;
    for (int j = 0; j < g_plds_ln; j++) { if (a >= g_plds_lhi[j] || a + n <= g_plds_llo[j]) continue; plds_t *s = &g_plds[g_plds_li[j]]; if (s->state == 2 || s->state == 3) plds_conflict(s, what, a, n); }
}
static void plds_cand(uint64_t a, uint64_t n) {   // a compute shader writes [a, a+n) in the current command buffer
    if (g_pl <= 0 || !g_pl_dsnap || !a || !n || n > PLDS_MAX || g_plds_ncand >= 16) return;
    for (int i = 0; i < g_plds_ncand; i++) if (g_plds_cand_lo[i] == a && g_plds_cand_hi[i] == a + n) return;
    g_plds_cand_lo[g_plds_ncand] = a; g_plds_cand_hi[g_plds_ncand++] = a + n;
}
static void plds_free(plds_t *s) {
    if (s->H) res_remove(s->H);
    for (int k = 0; k < PLDS_RING; k++) if (s->S[k]) res_remove(s->S[k]);
    free(s->base); free(s->mval); free(s->mmask);
    *s = (plds_t){0};
}
/* mtl_sub_commit: storage ranges compute shaders wrote in the last command buffer become candidates; seen in two last
   command buffers within 8 submissions they are activated at the next submission (plds_begin). */
static void plds_candidates(void) {
    g_plds_sub++;
    for (int c = 0; c < g_plds_ncand; c++) {
        uint64_t lo = g_plds_cand_lo[c], hi = g_plds_cand_hi[c]; plds_t *f = NULL, *s = NULL;
        if (plds_blacklisted(lo, hi)) continue;
        { int inside = 0; for (int i = 0; i < PLDS_SNAPS; i++) inside |= g_plds[i].state >= 2 && g_plds[i].state <= 3 && lo >= g_plds[i].lo && hi <= g_plds[i].hi; if (inside) continue; }   // already served
        for (int i = 0; i < PLDS_SNAPS && !s; i++) if (g_plds[i].state == 1 && g_plds[i].lo == lo && g_plds[i].hi == hi) s = &g_plds[i];
        for (int i = 0; i < PLDS_SNAPS && !s && !f; i++) if (g_plds[i].state == 0) f = &g_plds[i];
        if (!s && !f) continue;
        if (!s) { s = f; *s = (plds_t){0}; s->state = 1; s->lo = lo; s->hi = hi; }
        s->hits = s->last_sub && g_plds_sub - s->last_sub <= 8 ? s->hits + 1 : 1; s->last_sub = g_plds_sub;
    }
    g_plds_ncand = 0;
}
static void plds_merge_into_h(plds_t *s) {   // CPU words written since the last sync go into H (GPU timeline)
    size_t nw = (size_t)(s->hi - s->lo) / 8; const volatile uint64_t *g = (const volatile uint64_t *)(uintptr_t)s->lo;
    os_unfair_lock_lock(&g_plds_lk);
    if (!s->base_valid && pl_done_fn() >= s->base_fence) { for (size_t w = 0; w < nw; w++) s->base[w] = g[w]; s->base_valid = 1; }
    if (s->base_valid && memcmp((const void *)g, s->base, nw * 8))   // the CPU wrote words since the last sync
        for (size_t w = 0; w < nw; w++) { uint64_t v = g[w]; if (v != s->base[w]) { s->mval[w] = v; if (!s->mmask[w]) { s->mmask[w] = 1; s->nmask++; } s->base[w] = v; } }
    if (!s->nmask) { os_unfair_lock_unlock(&g_plds_lk); return; }
    s->nmask = 0;
    size_t w = 0; int runs = 0;
    while (w < nw) {
        if (!s->mmask[w]) { w++; continue; }
        size_t w0 = w; while (w < nw && s->mmask[w]) { s->mmask[w] = 0; w++; }
        id<MTLBuffer> sb; size_t so = arena_alloc((w - w0) * 8, &sb);
        memcpy((uint8_t *)sb.contents + so, &s->mval[w0], (w - w0) * 8);
        id<MTL4ComputeCommandEncoder> b = blit(); enc_b_copy_buffer(b, sb, so, s->H, w0 * 8, (w - w0) * 8); runs++;
    }
    os_unfair_lock_unlock(&g_plds_lk);
    if (runs) { enc_b_end(g_blit); g_blit = nil; atomic_fetch_add(&g_plds_merge, (uint64_t)runs); }
}
static void plds_begin(void) {   // mtl_sub_begin (command buffer open, nothing encoded yet): activate, merge, retire, free
    if (g_plds_nlearn) {   // write ranges the CPU invalidated while they were in flight: candidates ready for activation
        os_unfair_lock_lock(&g_plds_lk);
        uint64_t lo[16], hi[16]; int n = g_plds_nlearn; memcpy(lo, g_plds_learn_lo, sizeof lo); memcpy(hi, g_plds_learn_hi, sizeof hi); g_plds_nlearn = 0;
        os_unfair_lock_unlock(&g_plds_lk);
        for (int c = 0; c < n; c++) {
            if (plds_blacklisted(lo[c], hi[c])) continue;
            /* overlapping snapshots (e.g. the 64 KiB a storage pointer of unknown extent is taken to write, over a smaller
               snapshot): one snapshot over the union, the old ones retire first (activation waits until they are gone) */
            uint64_t ulo = lo[c] & ~15ull, uhi = (hi[c] + 15) & ~15ull; int same = 0;
            for (int pass = 0; pass < 2; pass++)   // twice: a union can reach a further neighbour
                for (int i = 0; i < PLDS_SNAPS; i++) { plds_t *o = &g_plds[i]; if (o->state >= 1 && o->state <= 3 && o->lo < uhi + PLDS_NEAR && o->hi + PLDS_NEAR > ulo) {
                    uint64_t nlo = o->lo < ulo ? o->lo : ulo, nhi = o->hi > uhi ? o->hi : uhi;
                    if (nhi - nlo <= PLDS_MAX) { ulo = nlo; uhi = nhi; } } }
            if (uhi - ulo > PLDS_MAX) continue;
            for (int i = 0; i < PLDS_SNAPS; i++) { plds_t *o = &g_plds[i]; if (o->state >= 1 && o->state <= 3 && o->lo < uhi && o->hi > ulo) {
                if (o->state == 1) { if (o->lo == ulo && o->hi == uhi) same = 1; else *o = (plds_t){0}; }
                else if (o->lo != ulo || o->hi != uhi) { os_unfair_lock_lock(&g_plds_lk); if (o->state == 2) o->state = 3; os_unfair_lock_unlock(&g_plds_lk); }
                else same = 1; } }
            if (same) continue;
            int put = 0;
            for (int i = 0; i < PLDS_SNAPS; i++) if (g_plds[i].state == 0) { g_plds[i] = (plds_t){0}; g_plds[i].state = 1; g_plds[i].lo = ulo; g_plds[i].hi = uhi; g_plds[i].hits = 2; g_plds[i].last_sub = g_plds_sub; atomic_fetch_add(&g_plds_learned, 1); put = 1; break; }
            if (!put && atomic_fetch_add(&g_plds_noslot, 1) < 8) {
                int st[5] = {0}; for (int i = 0; i < PLDS_SNAPS; i++) { int k = g_plds[i].state; if (k >= 0 && k <= 4) st[k]++; }
                MLOG("【红】[pipeline] readback snapshot %#llx+%llu: no free slot (candidates %d, active %d, retiring %d, dead %d)", (unsigned long long)ulo, (unsigned long long)(uhi - ulo), st[1], st[2], st[3], st[4]); }
        }
    }
    for (int i = 0; i < PLDS_SNAPS; i++) {
        plds_t *s = &g_plds[i];
        if (s->state == 4 && pl_done_fn() >= s->dead_fence) plds_free(s);
        else if (s->state == 2) plds_merge_into_h(s);
        else if (s->state == 3) {   // retire: H (with the CPU's words) back to memory, then plain bindings again
            if (!s->gone) plds_merge_into_h(s);
            void *mb; uint64_t mo;
            if (!s->gone && mtl_addr(s->lo, s->hi - s->lo, &mb, &mo)) { id<MTL4ComputeCommandEncoder> b = blit(); enc_b_copy_buffer(b, s->H, 0, (__bridge id<MTLBuffer>)mb, mo, s->hi - s->lo); enc_b_end(g_blit); g_blit = nil; }
            os_unfair_lock_lock(&g_plds_lk); s->state = 4; s->dead_fence = g_fence + 1; os_unfair_lock_unlock(&g_plds_lk);
            atomic_fetch_sub(&g_plds_live, 1);
        } else if (s->state == 1) {
            if (g_plds_sub - s->last_sub > 8) { s->state = 0; continue; }
            if (s->hits < 2) continue;
            uint64_t lo = s->lo & ~15ull, hi = (s->hi + 15) & ~15ull;   // union with overlapping candidates (one shader binds part of another's range)
            for (int j = 0; j < PLDS_SNAPS; j++) { plds_t *o = &g_plds[j]; if (o != s && o->state == 1 && o->lo < hi && o->hi > lo) { if (o->lo < lo) lo = o->lo & ~15ull; if (o->hi > hi) hi = (o->hi + 15) & ~15ull; } }
            int clash = 0; for (int j = 0; j < PLDS_SNAPS; j++) { plds_t *o = &g_plds[j]; if (o->state >= 2 && o->state <= 4 && o->lo < hi && o->hi > lo) clash = 1; }
            void *mb; uint64_t mo;
            if (clash && s->hits >= 2 && hi - lo <= PLDS_MAX) {   // make way: active snapshots inside the union retire; give up after 64 submissions
                if (!s->wait_since) s->wait_since = g_plds_sub;
                if (g_plds_sub - s->wait_since > 64) { s->state = 0; atomic_fetch_add(&g_plds_gaveup, 1); continue; }
                for (int j = 0; j < PLDS_SNAPS; j++) { plds_t *o = &g_plds[j]; if (o->state == 2 && o->lo < hi && o->hi > lo) { os_unfair_lock_lock(&g_plds_lk); o->state = 3; os_unfair_lock_unlock(&g_plds_lk); } }
                s->last_sub = g_plds_sub; continue; }
            if (clash || hi - lo > PLDS_MAX || !mtl_addr(lo, hi - lo, &mb, &mo)) { s->state = 0; continue; }
            for (int j = 0; j < PLDS_SNAPS; j++) { plds_t *o = &g_plds[j]; if (o != s && o->state == 1 && o->lo < hi && o->hi > lo) *o = (plds_t){0}; }
            size_t len = (size_t)(hi - lo), nw = len / 8;
            s->lo = lo; s->hi = hi;
            s->H = [g_dev newBufferWithLength:len options:MTLResourceStorageModeShared]; res_add(s->H);
            for (int k = 0; k < PLDS_RING; k++) { s->S[k] = [g_dev newBufferWithLength:len options:MTLResourceStorageModeShared]; res_add(s->S[k]); s->s_fence[k] = 0; }
            s->base = calloc(nw, 8); s->mval = calloc(nw, 8); s->mmask = calloc(nw, 1);
            if (!s->H || !s->S[PLDS_RING - 1] || !s->base || !s->mval || !s->mmask) MDIE("pipeline depth statistics buffers");
            id<MTL4ComputeCommandEncoder> b = blit(); enc_b_copy_buffer(b, (__bridge id<MTLBuffer>)mb, mo, s->H, 0, len); enc_b_end(g_blit); g_blit = nil;   // after every earlier GPU write of it
            s->base_valid = 0; s->base_fence = g_fence; s->pub_fence = g_fence; s->used_rw = 0;
            os_unfair_lock_lock(&g_plds_lk); s->state = 2; os_unfair_lock_unlock(&g_plds_lk);
            atomic_fetch_add(&g_plds_live, 1); atomic_fetch_add(&g_plds_act, 1);
            MLOG("[pipeline] readback snapshot %#llx+%zu active: GPU work uses a host buffer, frame waits and invalidates hand the game the newest completed copy", (unsigned long long)lo, len);
        }
    }
    plds_live_list();
}
static void plds_commit(uint64_t fence) {   // mtl_sub_commit, before the last command buffer ends: H -> a ring slot neither in flight nor the newest completed one
    if (!atomic_load_explicit(&g_plds_live, memory_order_relaxed)) return;
    for (int i = 0; i < PLDS_SNAPS; i++) {
        plds_t *s = &g_plds[i];
        if (s->state != 2 || !s->used_rw) continue;
        s->used_rw = 0;
        os_unfair_lock_lock(&g_plds_lk);
        uint64_t done = pl_done_fn(); int newest = -1, pick = -1;
        for (int k = 0; k < PLDS_RING; k++) if (s->s_fence[k] && s->s_fence[k] <= done && (newest < 0 || s->s_fence[k] > s->s_fence[newest])) newest = k;
        for (int k = 0; k < PLDS_RING; k++) if (k != newest && s->s_fence[k] <= done && (pick < 0 || s->s_fence[k] < s->s_fence[pick])) pick = k;
        if (pick >= 0) s->s_fence[pick] = fence;
        os_unfair_lock_unlock(&g_plds_lk);
        if (pick < 0) { atomic_fetch_add(&g_plds_skip, 1); continue; }
        enc_b_copy_buffer(blit(), s->H, 0, s->S[pick], 0, s->hi - s->lo);   // one encoder for all of them (ended by the commit)
    }
}
/* Game thread (frame wait return, pool invalidate): the newest completed copy of each snapshot goes into guest memory.
   Words the CPU wrote since the last sync are kept (and merged into H at the next submission). */
/* lo < hi: the snapshots over [lo, hi) (an invalidate; they are marked as invalidated); lo == hi: those never
   invalidated (a frame wait: the CPU may read them without an invalidate). */
static void plds_publish_x(uint64_t lo, uint64_t hi) {
    if (!atomic_load_explicit(&g_plds_live, memory_order_relaxed)) return;
    uint64_t done = pl_done_fn();
    os_unfair_lock_lock(&g_plds_lk);
    for (int i = 0; i < PLDS_SNAPS; i++) {
        plds_t *s = &g_plds[i];
        if (s->state != 2) continue;
        if (lo < hi) { if (s->hi <= lo || s->lo >= hi) continue; s->inval = 1; }
        else if (s->inval) continue;
        int k = -1;
        for (int j = 0; j < PLDS_RING; j++) if (s->s_fence[j] > s->pub_fence && s->s_fence[j] <= done && (k < 0 || s->s_fence[j] > s->s_fence[k])) k = j;
        if (k < 0) continue;
        size_t len = (size_t)(s->hi - s->lo), nw = len / 8; volatile uint64_t *g = (volatile uint64_t *)(uintptr_t)s->lo;
        if (!s->base_valid) { if (done < s->base_fence) continue; for (size_t w = 0; w < nw; w++) s->base[w] = g[w]; s->base_valid = 1; }
        void *tok = nvn_dirty_tracked((const void *)(uintptr_t)s->lo, len) ? nvn_dirty_gpu_write_begin((void *)(uintptr_t)s->lo, len) : NULL;
        if (!tok) dm_guest_write_prepare((void *)(uintptr_t)s->lo, len);
        const uint64_t *src = (const uint64_t *)s->S[k].contents;
        if (!memcmp((const void *)g, s->base, nw * 8)) { memcpy((void *)g, src, nw * 8); memcpy(s->base, src, nw * 8); }   // no CPU word since the last sync
        else for (size_t w = 0; w < nw; w++) {
            uint64_t cur = g[w];
            if (cur != s->base[w]) { s->mval[w] = cur; if (!s->mmask[w]) { s->mmask[w] = 1; s->nmask++; } }   // the CPU wrote it: its word stays (and goes into H)
            else if (cur != src[w]) g[w] = src[w];
            s->base[w] = g[w];
        }
        nvn_dirty_gpu_write_end(tok);
        s->pub_fence = s->s_fence[k]; atomic_fetch_add(&g_plds_pub, 1);
    }
    os_unfair_lock_unlock(&g_plds_lk);
}
static void plds_publish(void) { plds_publish_x(0, 0); }
static void plds_pool_gone(uintptr_t lo, uintptr_t hi) {   // mtl_pool_del (the translation queue is drained)
    if (!atomic_load_explicit(&g_plds_live, memory_order_relaxed)) return;
    os_unfair_lock_lock(&g_plds_lk);
    for (int i = 0; i < PLDS_SNAPS; i++) { plds_t *s = &g_plds[i]; if (s->state == 2 && s->lo < hi && s->hi > lo) { s->state = 3; s->gone = 1; } }
    os_unfair_lock_unlock(&g_plds_lk);
}


/* ---------- reads, writes (replay thread) ---------- */
static size_t arena_alloc_u(size_t n, __unsafe_unretained id<MTLBuffer> *buf, uint8_t **cpu, uint64_t *gpu);
/* A GPU write of guest [a, a+n) in this submission: never snapshotted, its range published; snapshots of it made
   earlier in this submission are dropped (later readers must see the GPU's result in memory). */
static void pl_write_i(uint64_t a, uint64_t n) {
    if (g_pl <= 0 || !a || !n) return;
    for (int j = 0; j < g_plds_ln; j++) if (a >= g_plds_llo[j] && a + n <= g_plds_lhi[j]) { const plds_t *d = &g_plds[g_plds_li[j]]; if (d->state == 2 || d->state == 3) return; }   // its bindings write the host buffer
    uint64_t t0 = pls_t0();
    nvn_pl_gpu_write((const void *)(uintptr_t)a, (size_t)n, g_fence + 1);
    pl_rng_add(a, a + n, 1, 1); g_pls_st.writes++; g_pl_wep++;
    if (g_plsr_n && a < g_plsr_hi && a + n > g_plsr_lo)
        for (int i = 0; i < g_plsr_n; i++) {
            pl_snaprec *r = &g_plsr[i];
            if (r->gen != g_plm_gen || a >= r->hi || a + n <= r->lo) continue;
            pl_memo *m = &g_plm[r->slot]; if (m->gen == g_plm_gen && m->lo == r->lo && m->hi == r->hi) { m->snap = 0; m->gpu = 0; }   // from here on: memory
            r->gen = 0; g_pls_st.snap_drops++; g_usnap_drops++;   // g_usnap_drops is in the binding caches' context (cbc_ctx, arg_epoch_key): a cached address of the dropped copy misses
        }
    if (t0) g_pls_st.ns += (pl_now() - t0) * 16;
}
static void pl_write(uint64_t a, uint64_t n) { if (g_pl <= 0 || !a || !n) return; uint64_t t = plh_in(PLH_WRITE); pl_write_i(a, n); plh_out(PLH_WRITE, t); }
/* The decision for a GPU read of [a, a+n) (clamped to its pool): memoised per submission, a static one kept across
   submissions while no page of it is written. */
static int pl_decide(uint64_t a0, uint64_t n0, pl_dm **out) {
    static uint64_t pgm; if (!pgm) pgm = (uint64_t)getpagesize() - 1;
    uint64_t a = a0 & ~pgm, n = ((a0 + n0 + pgm) & ~pgm) - a;   // decided per page span: draws of one buffer share it
    unsigned h = pl_memo_slot(a, a + n) ^ 0x5bd1; pl_dm *d = NULL;
    for (int i = 0; i < 8; i++, h = (h + 1) & (PL_MEMO - 1)) {
        pl_dm *e = &g_pldm[h & (PL_MEMO - 1)];
        if (e->gen == g_plm_gen && e->lo == a && e->hi == a + n) {
            g_pls_st.memo_hits++; *out = e;
            if (e->c == 1 && e->wep != g_pl_wep) { e->wep = g_pl_wep; if (nvn_pl_gpu_written((const void *)(uintptr_t)a, (size_t)n, pl_done_fn())) e->c = -1; }   // written by the GPU since: no snapshot from here on
            return e->c;
        }
        if (e->gen != g_plm_gen) { d = e; break; }
    }
    int samp = !(++g_pls_st.decides & 31) && g_pl_htime; uint64_t t0 = samp ? pl_now() : 0;   // timed one in 32 (scaled in the stats)
    int c; pl_sc *sc = pl_sc_slot(a, a + n); int same = sc->lo == a && sc->hi == a + n;
    uint64_t rgen = nvn_dirty_range_gen();
    if (same && sc->c == -2 && sc->stamp == rgen) { c = -1; g_pls_st.dc[0]++; }   // untracked, no range came or went since
    else if (same && sc->c == 0 && !nvn_dirty_since((const void *)(uintptr_t)a, (size_t)n, sc->stamp)) { c = 0; g_pls_st.dc[1]++; }   // static, not written since
    else if (same && sc->c == 1 && (uint32_t)g_sub_serial - sc->frame < PL_VOL_REUSE) {   // volatile, recently decided
        c = nvn_pl_gpu_written((const void *)(uintptr_t)a, (size_t)n, pl_done_fn()) ? -1 : 1; g_pls_st.dc[2]++; }
    else {
        g_pls_st.dc[!same ? 3 : sc->c == 0 ? 4 : sc->c == 1 ? 5 : 6]++;   // new (or evicted), static written, volatile expired, ranges changed
        if (nvn_pl_gpu_written((const void *)(uintptr_t)a, (size_t)n, pl_done_fn())) c = -1;   // a GPU write in flight: read in place (not cached)
        else {
            uint64_t stamp = nvn_dirty_now();
            c = nvn_pl_classify((const void *)(uintptr_t)a, (size_t)n, g_pl_keep, g_pl_probe, g_pl_first && n <= g_pl_first_max, pl_protq_cb);   // first-seen spans over FIRST_MAX are static (no probation copy)
            if (c == 0) pl_protq_push(a, a + n);   // static: every page protected (helper), so a CPU write is seen
            if (c >= 0) *sc = (pl_sc){a, a + n, stamp, (uint32_t)g_sub_serial, (int8_t)c};
            else *sc = (pl_sc){a, a + n, rgen, (uint32_t)g_sub_serial, -2};   // untracked
        }
    }
    if (d) *d = (pl_dm){a, a + n, g_plm_gen, g_pl_wep, (int8_t)c, 0, 0, 0};
    *out = d;
    if (samp) g_pls_st.t_decide += (pl_now() - t0) * 32;
    return c;
}
static uint64_t pl_clamp(uint64_t a, uint64_t n) { uintptr_t plo, phi; if (mtl_addr_range(a, 1, &plo, &phi) && a + n > phi) n = phi - a; return n; }
/* A GPU read of guest [a, a+n) (kind PLK_*). 1: read the snapshot *buf / *off / *gpu (this submission's arena) instead
   of memory. 0: read memory. Decided per page span and submission (a static decision kept across submissions, pl_sc).
   What is read in place is published for the fault handler: a static page span once (CPU writes to static pages are
   rare: a write there is held when the span is in flight), anything else as its exact bytes. */
static int pl_read_i(uint64_t a, uint64_t n, int kind, void **buf, uint64_t *off, uint64_t *gpu) {
    if (g_pl <= 0 || !a || !n) return 0;
    if (n > (64u << 20)) { n = pl_clamp(a, n); if (!n) return 0; }   // a binding past its pool's end (whole-pool sizes)
    pl_dm *d = NULL; int c = pl_decide(a, n, &d);
    if (c == 0) {
        g_pls_st.live_static++; g_pls_st.live[kind]++;
        if (kind == PLK_TX && !g_pl_texhold) return 0;
        if (d && !d->pub) {   // the bytes read of a static span: their union is published at the commit (exact enough for the holds:
            d->pub = 1; d->ulo = a; d->uhi = a + n;   // the game streams new data next to data in use in the same 16 KiB page)
            if (g_pldl_n == g_pldl_cap) { g_pldl_cap = g_pldl_cap ? g_pldl_cap * 2 : 2048; g_pldl = realloc(g_pldl, sizeof *g_pldl * (size_t)g_pldl_cap); if (!g_pldl) MDIE("pipeline ranges"); }
            g_pldl[g_pldl_n++] = (uint32_t)(d - g_pldm);
        } else if (d) { if (a < d->ulo) d->ulo = a; if (a + n > d->uhi) d->uhi = a + n; }
        else pl_rng_add(a, a + n, 0, 0);
        return 0;
    }
    int fresh = 0; pl_memo *m = pl_memo_find(a, a + n, &fresh);
    if (m && !fresh) { g_pls_st.memo_hits++; if (m->snap > 0) { *buf = m->buf; *off = m->off; *gpu = m->gpu; return 1; } return 0; }
    uint64_t t0 = pls_t0();
    if (c == 1 && (n > (64u << 20) ? (n = pl_clamp(a, n)) : n) <= g_pl_snap_max && g_pl_sub_bytes + n > g_pl_budget) g_pls_st.over_budget++;
    else if (c == 1 && n <= g_pl_snap_max) {
        uint64_t t1 = t0 ? pl_now() : 0;
        __unsafe_unretained id<MTLBuffer> ab; uint8_t *acpu; uint64_t agpu; size_t ao = arena_alloc_u((size_t)n, &ab, &acpu, &agpu);
        sigjmp_buf jb; int ok = 0;
        if (sigsetjmp(jb, 0) == 0) { runtime_fault_guard = &jb; memcpy(acpu, (const void *)(uintptr_t)a, (size_t)n); ok = 1; }   // mask not saved: ~0.6% of the translate thread (2026-10-25)
        runtime_fault_guard = NULL;
        if (t1) g_pls_st.snap_ns += (pl_now() - t1) * 16;
        if (ok) {
            g_pls_st.snaps[kind]++; g_pls_st.snap_bytes += n; g_pl_sub_bytes += n;
            if (m) {
                m->snap = 1; m->buf = (__bridge void *)ab; m->off = ao; m->gpu = agpu; m->kind = (int8_t)kind;
                if (g_plsr_n == g_plsr_cap) { g_plsr_cap = g_plsr_cap ? g_plsr_cap * 2 : 1024; g_plsr = realloc(g_plsr, (size_t)g_plsr_cap * sizeof *g_plsr); if (!g_plsr) MDIE("pipeline snapshot list"); }
                g_plsr[g_plsr_n++] = (pl_snaprec){a, a + n, (unsigned)(m - g_plm), g_plm_gen};
                if (a < g_plsr_lo) g_plsr_lo = a; if (a + n > g_plsr_hi) g_plsr_hi = a + n;
            }
            if (g_pl_log > 0) { g_pl_log--; MLOG("[pipeline] F%llu %s %#llx+%llu snapshot (volatile pages)", (unsigned long long)atomic_load(&g_cur_frame), g_plk_name[kind], (unsigned long long)a, (unsigned long long)n); }
            *buf = (__bridge void *)ab; *off = ao; *gpu = agpu;
            if (t0) g_pls_st.ns += (pl_now() - t0) * 16;
            return 1;
        }
    }
    if (c == 1) g_pls_st.live_big++; else g_pls_st.live_gpuw++;
    g_pls_st.live[kind]++;
    if (kind != PLK_TX || g_pl_texhold) pl_rng_add(a, a + n, 1, 0);
    if (m) { m->snap = 0; m->kind = (int8_t)kind; }
    if (t0) g_pls_st.ns += (pl_now() - t0) * 16;
    return 0;
}
static int pl_read(uint64_t a, uint64_t n, int kind, void **buf, uint64_t *off, uint64_t *gpu) {
    if (g_pl <= 0 || !a || !n) return 0;
    int cat = kind == PLK_VB ? PLH_VB : kind == PLK_IB ? PLH_IB : PLH_READ; uint64_t t = plh_in(cat); int r = pl_read_i(a, n, kind, buf, off, gpu); plh_out(cat, t); return r;
}
/* A vertex stream bound at [a, a+full): a draw reads ext(...) bytes of it (bindings often reach to the end of their
   pool): those are decided (per page span), snapshotted or published. */
static int pl_read_vb(uint64_t a, uint64_t full, uint64_t (*ext)(unsigned, uint64_t), unsigned stream, void **buf, uint64_t *gpu) {
    if (g_pl <= 0 || !a) return 0;
    if (!full) full = 1;
    uint64_t t = plh_in(PLH_VB);
    uint64_t n = ext(stream, full); if (!n) n = full;
    uint64_t off; int r = pl_read(a, n, PLK_VB, buf, &off, gpu);
    plh_out(PLH_VB, t); return r;
}
/* A GPU read of guest [a, a+n) that cannot use a snapshot (typed buffer views over pool memory, storage pointers of
   unknown extent, GPU-side texture uploads and index widening): its range is published (not static). */
static void pl_live_read(uint64_t a, uint64_t n, int kind) {
    if (g_pl <= 0 || !a || !n || (kind == PLK_TX && !g_pl_texhold)) return;
    uint64_t t = plh_in(PLH_LIVE);
    n = pl_clamp(a, n);
    int fresh = 0; pl_memo *m = n ? pl_memo_find(a, a + n, &fresh) : NULL;
    if (!n) {}
    else if (m && !fresh && m->snap == 0) g_pls_st.memo_hits++;
    else {
        if (m && fresh) { m->snap = 0; m->kind = (int8_t)kind; }
        g_pls_st.live[kind]++; g_pls_st.live_noshot++;
        pl_rng_add(a, a + n, 1, 0);
    }
    plh_out(PLH_LIVE, t);
}
/* A binding (graphics or compute) of guest [a, a+n): the depth statistics host buffer, else a write (rw) or a read
   (pl_read). 1: use *buf / *gpu. */
static int pl_bind(uint64_t a, uint64_t n, int rw, int kind, int compute, void **buf, uint64_t *gpu) {
    if (g_pl <= 0 || !a) return 0;
    int cat = rw ? PLH_WRITE : PLH_READ, r; uint64_t t = plh_in(cat);
    if (plds_redirect(a, n ? n : 1, rw, buf, gpu)) r = 1;
    else if (rw) { pl_write(a, n ? n : 1); if (compute) plds_cand(a, n); r = 0; }
    else { uint64_t off; r = pl_read(a, n ? n : 1, kind, buf, &off, gpu); }
    plh_out(cat, t); return r;
}

/* ---------- submissions: slots for the release / completion ---------- */
typedef struct {
    uint64_t fence; int state; post_op *early; int nearly; uint64_t *qaddr; int nq; uint64_t t_rel;
    uintptr_t *nlo, *nhi; int nn;          // in-place ranges that are not static (all their pages protected at the release)
    uint64_t protq;                        // protection queue sequence at the commit (pl_protq_sync before the release)
    uintptr_t *wlo, *whi, *wmx; uint8_t *wk; int nw;    // GPU write ranges, sorted, running maximum of the ends, kind (invalidates, RWATCH)
} pl_slot;
static pl_slot g_plslot[PL_SLOTS];   // under g_mu. state: 1 pending, 2 being released, 3 released (early results applied), 4 taken by the completion
static void pl_slot_free_locked(pl_slot *s) { free(s->early); free(s->qaddr); free(s->nlo); free(s->nhi); free(s->wlo); free(s->whi); free(s->wmx); free(s->wk); *s = (pl_slot){0}; }
static struct { uint64_t n, wall, cpu, rs_sync, rs_full, w[3]; } g_plx;
static int plx_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("TOTK_XLATE_STATS"); v = e && atoi(e) > 0; } return v || g_pl > 0; }
/* end of mtl_sub_commit: the translate thread's time for this submission; a line every 600 */
static void pl_sub_end_stats(void) {
    if (!plx_on()) return;
    uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW), cpu = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID);
    g_plx.n++; g_plx.wall += now - g_plx_t0; g_plx.cpu += cpu - g_plx_c0;
    g_plx.rs_sync += g_rs_sync_ns - g_plx_rs0; g_plx.rs_full += g_rs_full_ns - g_plx_rsf0;
    for (int i = 0; i < 3; i++) { g_plx.w[i] += g_plx_wait[i]; g_plx_wait[i] = 0; }
    if (g_plx.n < 600) return;
    double k = (double)g_plx.n * 1e6;
    MLOG("[translate] per submission (%llu): wall %.3f ms, on CPU %.3f ms, off CPU %.3f ms (sleeping waits or preempted); waits: encode stage %.3f (join) + %.3f (queue full) ms (spinning: on CPU), command allocator %.3f, visibility ring %.3f, pipeline slot %.3f ms (sleeping)",
         (unsigned long long)g_plx.n, g_plx.wall / k, g_plx.cpu / k, (g_plx.wall > g_plx.cpu ? g_plx.wall - g_plx.cpu : 0) / k, g_plx.rs_sync / k, g_plx.rs_full / k, g_plx.w[0] / k, g_plx.w[1] / k, g_plx.w[2] / k);
    memset(&g_plx, 0, sizeof g_plx);
}
static void pl_sub_begin(void) {   // end of mtl_sub_begin
    g_plx_rs0 = g_rs_sync_ns; g_plx_rsf0 = g_rs_full_ns;
    pl_cfg();
    if (g_pl <= 0) return;
    nvn_pl_set_done(pl_done_fn);
    nvn_pl_set_frame((uint32_t)g_sub_serial);
    if (++g_plm_gen == 0) g_plm_gen = 1;
    g_pl_sub_bytes = 0; g_plsr_n = 0; g_plsr_lo = UINT64_MAX; g_plsr_hi = 0; g_plr_all.n = g_plr_ns.n = g_plr_w.n = 0; g_pldl_n = 0; g_plq_n = 0; g_plds_ncand = 0;
    if (g_pl_dsnap) { uint64_t t = pl_now(); plds_begin(); g_plh_t[PLH_BEGIN] += pl_now() - t; g_plh_c[PLH_BEGIN]++; }
}
static void pl_sub_split(void) { if (g_pl > 0) g_plds_ncand = 0; }   // the candidates of the depth statistics are those of the last command buffer
/* mtl_sub_commit: the submission's in-place ranges are published (sorted, overlapping ones merged); its early results
   (event signals and constant counter reports), its non-static ranges and its write ranges go to the slot of its fence;
   the late results (occlusion sums) stay with the completion handler. Returns the late count (ops compacted to them). */
static int pl_wrng_cmp(const void *x, const void *y) { const pl_wrng *a = x, *b = y; return a->lo < b->lo ? -1 : a->lo > b->lo; }
static int pl_commit(uint64_t fence, post_op *ops, int n) {
    if (g_pl <= 0) return n;
    uint64_t t0 = pl_now();
    if (g_pl_dsnap) plds_candidates();
    post_op *early = NULL; int ne = 0, nl = 0;
    if (g_pl == 1) for (int i = 0; i < n; i++) {
        if (ops[i].kind == 1 || ops[i].kind == 2) {
            if (!early) { early = malloc(sizeof *early * (size_t)n); if (!early) MDIE("pipeline early results"); }
            early[ne++] = ops[i];
        } else {
            if (ops[i].kind == 3 && g_pl_query_vis) { if (g_plq_n == g_plq_cap) { g_plq_cap = g_plq_cap ? g_plq_cap * 2 : 256; g_plq = realloc(g_plq, sizeof *g_plq * (size_t)g_plq_cap); } g_plq[g_plq_n++] = ops[i].addr; }
            ops[nl++] = ops[i];
        }
    } else nl = n;
    /* ranges: all of them sorted (radix) and merged for the fault handler; the GPU writes sorted with a running maximum
       of their ends (invalidates); the non-static ones as they are (protected page by page at the release) */
    for (int i = 0; i < g_pldl_n; i++) { const pl_dm *d = &g_pldm[g_pldl[i]]; if (d->gen == g_plm_gen && d->pub) pl_rvec_add(&g_plr_all, d->ulo, d->uhi); }   // static reads
    int na = g_plr_all.n, nn = g_plr_ns.n, nw = g_plr_w.n;
    rng_sort((usnap_rng *)g_plr_all.r, na);
    uintptr_t *alo = malloc(sizeof *alo * (size_t)(na ? na : 1)), *ahi = malloc(sizeof *ahi * (size_t)(na ? na : 1));
    uintptr_t *nlo = NULL, *nhi = NULL;   // the non-static ranges were protected at their publication (nvn_pl_protect_span)
    uintptr_t *wlo = nw ? malloc(sizeof *wlo * (size_t)nw) : NULL, *whi = nw ? malloc(sizeof *whi * (size_t)nw) : NULL, *wmx = nw ? malloc(sizeof *wmx * (size_t)nw) : NULL;
    uint8_t *wk = nw ? malloc((size_t)nw) : NULL;
    if (!alo || !ahi || (nw && (!wlo || !whi || !wmx || !wk))) MDIE("pipeline ranges");
    int k = 0;
    for (int i = 0; i < na; i++) { const pl_rng *r = &g_plr_all.r[i]; if (k && r->lo <= ahi[k - 1]) { if (r->hi > ahi[k - 1]) ahi[k - 1] = r->hi; } else { alo[k] = r->lo; ahi[k++] = r->hi; } }
    nvn_pl_publish(fence, alo, ahi, k, 0);
    free(alo); free(ahi);
    if (nw > 1) qsort(g_plr_w.r, (size_t)nw, sizeof *g_plr_w.r, pl_wrng_cmp);
    uintptr_t run = 0; for (int i = 0; i < nw; i++) { wlo[i] = g_plr_w.r[i].lo; whi[i] = g_plr_w.r[i].hi; wk[i] = (uint8_t)g_plr_w.r[i].kind; if (whi[i] > run) run = whi[i]; wmx[i] = run; }
    nvn_pl_publish(fence, wlo, whi, nw, 1);
    na = k;
    uint64_t *q = NULL; int nq = g_plq_n;
    if (nq) { q = malloc(sizeof *q * (size_t)nq); memcpy(q, g_plq, sizeof *q * (size_t)nq); }
    pthread_mutex_lock(&g_mu);
    pl_slot *s = &g_plslot[fence % PL_SLOTS];
    if (s->fence && s->state == 2) { uint64_t w0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW); while (s->fence && s->state == 2) pthread_cond_wait(&g_cv, &g_mu); g_plx_wait[2] += clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - w0; }   // its release is still applying (16 submissions back: never in practice)
    pl_slot_free_locked(s);
    *s = (pl_slot){.fence = fence, .state = 1, .early = early, .nearly = ne, .qaddr = q, .nq = nq, .nlo = nlo, .nhi = nhi, .nn = 0, .protq = pl_protq_seq(), .wlo = wlo, .whi = whi, .wmx = wmx, .wk = wk, .nw = nw};
    pthread_mutex_unlock(&g_mu);
    g_pls_st.subs++; g_pls_st.ranges += (uint64_t)na; g_pls_st.nonstatic += (uint64_t)nn;
    g_pls_st.ns += pl_now() - t0; g_pls_st.t_commit += pl_now() - t0; g_plh_t[PLH_COMMIT] += pl_now() - t0; g_plh_c[PLH_COMMIT]++;
    return nl;
}
static uint64_t gpu_ticks(void);
static void pl_apply_early(post_op *ops, int n) {   // event signals / constant counter reports, written for the GPU (not CPU writes)
    /* the pages are made writable once each (most results of a frame lie in a few pages), then protected again */
    enum { PGS = 64 }; uintptr_t pg[PGS]; void *tok[PGS]; int np = 0; uintptr_t pgm = (uintptr_t)getpagesize() - 1;
    uint64_t t = gpu_ticks();
    for (int i = 0; i < n; i++) {
        void *at = ops[i].kind == 1 ? (void *)ops[i].val : (void *)(uintptr_t)ops[i].addr; size_t len = ops[i].kind == 1 ? 4 : 16;
        uintptr_t p0 = (uintptr_t)at & ~pgm; int k = 0, one = (((uintptr_t)at + len - 1) & ~pgm) == p0;
        while (k < np && pg[k] != p0) k++;
        void *own = NULL;
        if (k == np && one && np < PGS) { pg[np] = p0; tok[np] = nvn_dirty_tracked((const void *)p0, pgm + 1) ? nvn_dirty_gpu_write_begin((void *)p0, pgm + 1) : NULL; np++; }
        else if (k == np) own = nvn_dirty_tracked(at, len) ? nvn_dirty_gpu_write_begin(at, len) : NULL;   // over a page border, or too many pages
        if (ops[i].kind == 1) { if (ops[i].mode == 1) *ops[i].val += ops[i].v; else *ops[i].val = ops[i].v; }
        else { uint64_t rec[2] = {0, t}; memcpy(at, rec, sizeof rec); }
        nvn_dirty_gpu_write_end(own);
    }
    for (int k = 0; k < np; k++) nvn_dirty_gpu_write_end(tok[k]);
}
static void cpend_done_known(uint64_t fence);
/* Completion handler of fence: the early results to apply now (NULL: none, or the release applied them). */
static post_op *pl_complete_take(uint64_t fence, int *n) {
    *n = 0; if (g_pl <= 0) return NULL;
    post_op *e = NULL;
    pthread_mutex_lock(&g_mu);
    pl_slot *s = &g_plslot[fence % PL_SLOTS];
    if (s->fence == fence) {
        while (s->state == 2) pthread_cond_wait(&g_cv, &g_mu);
        if (s->state == 1) { e = s->early; *n = s->nearly; s->early = NULL; s->nearly = 0; s->state = 4; }
        if (s->t_rel) { uint64_t d = pl_now() - s->t_rel, mx = atomic_load(&g_pl_lead_max); atomic_fetch_add(&g_pl_lead_ns, d); atomic_fetch_add(&g_pl_lead_n, 1); if (d > mx) atomic_store(&g_pl_lead_max, d); s->t_rel = 0; }
    }
    pthread_mutex_unlock(&g_mu);
    return e;
}
/* hle_nvn.c, the game's frame wait for submission fence ends now. dry = 0: early (prev, the previous frame's fence, has
   completed; fence has not): its early results are written, the pages of its ranges protected again, the newest
   completed depth statistics published. dry = 1 (TOTK_PIPELINE=2): fence has completed; prev plays "completed" for one
   frame. */
void mtl_pl_release(uint64_t fence, uint64_t prev, int dry) {
    pl_cfg(); if (g_pl <= 0) return;
    uint64_t t0 = pl_now();
    enum { MAXS = 4 }; pl_slot take[MAXS]; int nt = 0;
    pthread_mutex_lock(&g_mu);
    if (!dry && fence > g_pl_rel) g_pl_rel = fence;
    uint64_t done = dry ? prev : g_done;
    for (uint64_t f = fence; f > done && nt < MAXS; f--) {
        pl_slot *s = &g_plslot[f % PL_SLOTS];
        if (s->fence != f) continue;
        take[nt] = *s;
        if (!dry && s->state == 1) { s->state = 2; s->early = NULL; s->nearly = 0; s->t_rel = t0; } else { take[nt].early = NULL; take[nt].nearly = 0; }
        s->nlo = s->nhi = NULL; s->nn = 0; s->qaddr = NULL; s->nq = 0;   // taken (the write ranges stay for invalidates)
        nt++;
    }
    pthread_mutex_unlock(&g_mu);
    nvn_pl_set_released(fence, prev);
    int nn = 0; for (int i = 0; i < nt; i++) nn += take[i].nn;
    uintptr_t *nlo = nn ? malloc(sizeof *nlo * (size_t)nn) : NULL, *nhi = nn ? malloc(sizeof *nhi * (size_t)nn) : NULL; nn = 0;
    uint64_t ta = pl_now();
    for (int i = 0; i < nt; i++) {
        if (take[i].nearly) { pl_apply_early(take[i].early, take[i].nearly); free(take[i].early); cpend_done_known(take[i].fence); }
        if (take[i].nq && !dry) for (int q = 0; q < take[i].nq; q++) {   // TOTK_PIPELINE_QUERY=visible
            void *at = (void *)(uintptr_t)take[i].qaddr[q]; void *tok = nvn_dirty_tracked(at, 16) ? nvn_dirty_gpu_write_begin(at, 16) : NULL;
            uint64_t rec[2] = {1u << 20, gpu_ticks()}; memcpy(at, rec, sizeof rec); nvn_dirty_gpu_write_end(tok);
        }
        if (take[i].nn && nlo && nhi) { memcpy(nlo + nn, take[i].nlo, sizeof *nlo * (size_t)take[i].nn); memcpy(nhi + nn, take[i].nhi, sizeof *nhi * (size_t)take[i].nn); nn += take[i].nn; }
        free(take[i].nlo); free(take[i].nhi); free(take[i].qaddr);
    }
    uint64_t tb = pl_now();
    { uint64_t q = 0; for (int i = 0; i < nt; i++) if (take[i].protq > q) q = take[i].protq; if (q) pl_protq_sync(q); }   // its spans are protected (normally long done by the helper)
    nvn_pl_reprotect(done, fence, NULL, NULL, 0);   // the non-static ranges were protected when published; CPU writes since: the ring
    free(nlo); free(nhi);
    uint64_t tc = pl_now();
    atomic_fetch_add(&g_pl_rel_t[0], tb - ta); atomic_fetch_add(&g_pl_rel_t[1], tc - tb);
    if (dry && g_pl_rwatch && g_sub_serial >= g_pl_rwatch) {   // TOTK_PIPELINE_RWATCH: the pages the released frame's GPU work wrote, until the next frame wait returns
        enum { WMAX = 4096 }; uintptr_t *w = malloc(sizeof *w * WMAX); int k = 0; uintptr_t pg = (uintptr_t)getpagesize();
        pthread_mutex_lock(&g_mu);
        for (int i = 0; i < nt && w; i++) { pl_slot *s = &g_plslot[take[i].fence % PL_SLOTS]; if (s->fence != take[i].fence) continue;
            for (int j = 0; j < s->nw && k < WMAX; j++) for (uintptr_t p = s->wlo[j] & ~(pg - 1); p < s->whi[j] && k < WMAX; p += pg) w[k++] = p; }
        pthread_mutex_unlock(&g_mu);
        nvn_pl_watch(w, k); free(w);
    }
    if (!dry) {
        pthread_mutex_lock(&g_mu);
        for (int i = 0; i < nt; i++) { pl_slot *s = &g_plslot[take[i].fence % PL_SLOTS]; if (s->fence == take[i].fence && s->state == 2) s->state = 3; }
        pthread_cond_broadcast(&g_cv);
        pthread_mutex_unlock(&g_mu);
        atomic_fetch_add(&g_pl_early, 1);
        totk_tl(TL_EARLY, fence, prev);
    }
    uint64_t td = pl_now();
    plds_publish();
    atomic_fetch_add(&g_pl_rel_t[2], pl_now() - td);
    atomic_fetch_add(&g_pl_rel_ns, pl_now() - t0); atomic_fetch_add(&g_pl_rel_n, 1);
    if (g_pl_log > 0 && !dry) { g_pl_log--; MLOG("[pipeline] frame wait for fence %llu released at fence %llu (%d submissions in flight)", (unsigned long long)fence, (unsigned long long)prev, nt); }
}
void mtl_pl_frame_wait_note(void) { atomic_fetch_add(&g_pl_waits, 1); }
void mtl_pl_publish(void) { if (g_pl > 0) plds_publish(); }   // a frame wait returned without a release: the depth statistics of the newest completed frame
/* Before state is destroyed or read back: every early-released submission has really completed. */
static _Atomic uint64_t g_pl_gate_r[PLG_N], g_pl_gate_rns[PLG_N];
static const char *const g_plg_name[PLG_N] = {"other", "invalidate", "pool finalize", "program finalize", "texel read / write", "virtual remap", "queue finish / suspend"};
void mtl_pl_gate_r(int reason) {
    if (g_pl != 1) return;
    pthread_mutex_lock(&g_mu);
    if (g_done < g_pl_rel) {
        uint64_t t0 = pl_now(); totk_tl(TL_GATE_IN, g_pl_rel, (uint64_t)reason);
        while (g_done < g_pl_rel) pthread_cond_wait(&g_cv, &g_mu);
        uint64_t dt = pl_now() - t0;
        atomic_fetch_add(&g_pl_gate_n, 1); atomic_fetch_add(&g_pl_gate_ns, dt); totk_tl(TL_GATE_OUT, g_done, 0);
        if (reason >= 0 && reason < PLG_N) { atomic_fetch_add(&g_pl_gate_r[reason], 1); atomic_fetch_add(&g_pl_gate_rns[reason], dt); }
    }
    pthread_mutex_unlock(&g_mu);
}
void mtl_pl_gate(void) { mtl_pl_gate_r(PLG_OTHER); }
/* Pool finalize: wait only when GPU work of a released, unfinished submission accesses the pool's memory in place (its
   published ranges); its other bindings read snapshots, its textures are Metal copies. 1 = waited. */
int mtl_pl_gate_pool(const void *mem, uint64_t size) {
    if (g_pl != 1 || !mem || !size) return 0;
    pthread_mutex_lock(&g_mu); uint64_t rel = g_pl_rel, done = g_done; pthread_mutex_unlock(&g_mu);
    if (rel <= done) return 0;
    if (!nvn_pl_inflight(mem, (size_t)size, done, rel)) { atomic_fetch_add(&g_pl_pool_skips, 1); return 0; }
    mtl_pl_gate_r(PLG_POOL); return 1;
}
/* nvnMemoryPoolInvalidateMappedRange: the CPU reads GPU results there next. The game's readback buffers (the depth
   statistics, the double-buffered objects of main+0x8d3xxx-0x8d4xxx, ...) are served from the snapshot ring: GPU work
   writes a host buffer, and the newest completed copy goes into guest memory here and at every frame wait, so the game
   gets the results of the last completed frame (one or, for a double-buffered slot, two frames older than before).
   An invalidate over an exact GPU write range of a released, unfinished submission that is not served that way yet
   waits for the real completion once, and that write range (up to PLDS_MAX) becomes a snapshot from the next
   submission on. Per caller: invalidates, waits and wait time (stats line). */
typedef struct { uint64_t caller, n, waited, ns; } pl_ivs;
static pl_ivs g_plivs[32]; static os_unfair_lock g_plivs_lk = OS_UNFAIR_LOCK_INIT;
static void pl_invalidate(const void *ap, uint64_t n, uint64_t caller) {
    if (g_pl <= 0) return;
    uintptr_t a = (uintptr_t)ap, b = a + n; int hit = 0, wkind = 0; uint64_t hf = 0, wlo = 0, whi = 0;
    atomic_fetch_add(&g_pl_inval_n, 1);
    pthread_mutex_lock(&g_mu);
    if (g_pl == 1 && g_pl_rel > g_done)
        for (int i = 0; i < PL_SLOTS && !hit; i++) {
            pl_slot *s = &g_plslot[i];
            if (!s->fence || s->fence <= g_done || s->fence > g_pl_rel || !s->nw) continue;
            int l = 0, h = s->nw; while (l < h) { int m = (l + h) / 2; if (s->wlo[m] < b) l = m + 1; else h = m; }
            if (l > 0 && s->wmx[l - 1] > a) {
                hit = 1; hf = s->fence;
                for (int j = l - 1; j >= 0 && s->wmx[j] > a; j--) if (s->whi[j] > a) { wlo = s->wlo[j]; whi = s->whi[j]; wkind = s->wk[j]; break; }   // the write range found first (nearest below b)
            }
        }
    pthread_mutex_unlock(&g_mu);
    uint64_t t0 = pl_now(); const char *why = "";
    if (hit) {
        atomic_fetch_add(&g_pl_inval_gates, 1);
        why = whi <= wlo ? "no write range found" : plds_learn(wlo, whi);   // from the next submission on: served from the snapshot ring
        mtl_pl_gate_r(PLG_INVAL);
    }
    uint64_t dt = hit ? pl_now() - t0 : 0;
    os_unfair_lock_lock(&g_plivs_lk);
    { int k = 0; while (k < 32 && g_plivs[k].caller && g_plivs[k].caller != caller) k++;
      if (k < 32) { int fresh = !g_plivs[k].n; g_plivs[k].caller = caller;   /* first invalidate of this caller (0 = not found: logged once too) */ g_plivs[k].n++; g_plivs[k].waited += hit; g_plivs[k].ns += dt;
          os_unfair_lock_unlock(&g_plivs_lk);
          static uint64_t waits_logged[32]; int log_wait = hit && waits_logged[k]++ < 3;   // the first 3 waits per caller
          if (fresh || log_wait || (hit && g_pl_log > 0)) MLOG("[pipeline] invalidate %#llx+%llu from main+%#llx%s", (unsigned long long)a, (unsigned long long)n, (unsigned long long)caller,
               hit ? [NSString stringWithFormat:@": GPU work of released fence %llu (%s) writes %#llx+%llu there, waited; snapshot: %s", (unsigned long long)hf, g_plw_name[wkind % PLW_N], (unsigned long long)wlo, (unsigned long long)(whi - wlo), why].UTF8String : "");
      } else os_unfair_lock_unlock(&g_plivs_lk); }
    plds_publish_x(a, b > a ? b : a + 1);
}
uint64_t mtl_done_fence(void) { return pl_done_fn(); }

static void pl_stats_line(void) {
    if (g_pl <= 0 || g_pls_st.subs % 600) return;
    uint64_t v[NVN_PLS_N]; nvn_pl_stats(v);
    double s = 600.0; static uint64_t last[NVN_PLS_N]; uint64_t d[NVN_PLS_N]; for (int i = 0; i < NVN_PLS_N; i++) { d[i] = v[i] - last[i]; last[i] = v[i]; }
    uint64_t snaps = 0, lives = 0; for (int k = 0; k < PLK_N; k++) { snaps += g_pls_st.snaps[k]; lives += g_pls_st.live[k]; }
    static uint64_t last_gate_n, last_gate_ns, last_inval_gates, last_inval_n, last_rel_n, last_rel_ns;
    uint64_t gn = atomic_load(&g_pl_gate_n), gns = atomic_load(&g_pl_gate_ns), ig = atomic_load(&g_pl_inval_gates), in = atomic_load(&g_pl_inval_n), rn = atomic_load(&g_pl_rel_n), rns = atomic_load(&g_pl_rel_ns);
    static uint64_t last_rt[3]; uint64_t rt[3]; for (int i = 0; i < 3; i++) { rt[i] = atomic_load(&g_pl_rel_t[i]) - last_rt[i]; last_rt[i] += rt[i]; }
    double rdiv = rn > last_rel_n ? (double)(rn - last_rel_n) : 1.0;
    MLOG("[pipeline] release on the game thread per frame wait: early results %.3f, protection %.3f, snapshots published %.3f ms", pl_ns(rt[0]) / rdiv / 1e6, pl_ns(rt[1]) / rdiv / 1e6, pl_ns(rt[2]) / rdiv / 1e6);
    MLOG("[pipeline] %llu submissions: frame waits %llu, released early %llu (lead %.2f ms avg, %.2f max before completion); last 600: release %.3f ms on the game thread, gate waits %llu (%.3f ms avg), invalidates %llu (%llu of them waited)",
         (unsigned long long)g_pls_st.subs, (unsigned long long)atomic_load(&g_pl_waits), (unsigned long long)atomic_load(&g_pl_early),
         atomic_load(&g_pl_lead_n) ? pl_ns(atomic_load(&g_pl_lead_ns)) / (double)atomic_load(&g_pl_lead_n) / 1e6 : 0.0, pl_ns(atomic_exchange(&g_pl_lead_max, 0)) / 1e6,
         rn > last_rel_n ? pl_ns(rns - last_rel_ns) / (double)(rn - last_rel_n) / 1e6 : 0.0,
         (unsigned long long)(gn - last_gate_n), gn > last_gate_n ? pl_ns(gns - last_gate_ns) / (double)(gn - last_gate_n) / 1e6 : 0.0,
         (unsigned long long)(in - last_inval_n), (unsigned long long)(ig - last_inval_gates));
    last_gate_n = gn; last_gate_ns = gns; last_inval_gates = ig; last_inval_n = in; last_rel_n = rn; last_rel_ns = rns;
    { char line[600]; size_t o = 0; static uint64_t last_r[PLG_N], last_rns[PLG_N];
      for (int r = 0; r < PLG_N; r++) { uint64_t n = atomic_load(&g_pl_gate_r[r]) - last_r[r], ns = atomic_load(&g_pl_gate_rns[r]) - last_rns[r]; last_r[r] += n; last_rns[r] += ns;
          if (n) o += (size_t)snprintf(line + o, sizeof line - o, " %s %llu (%.1f ms)", g_plg_name[r], (unsigned long long)n, pl_ns(ns) / 1e6); }
      MLOG("[pipeline] last 600: gate waits by reason:%s; pool finalizes that needed no wait %llu (since the start), readback snapshots given up waiting %llu; spans protected by the helper %llu (by a release itself %llu, since the start)", o ? line : " none",
           (unsigned long long)atomic_load(&g_pl_pool_skips), (unsigned long long)atomic_load(&g_plds_gaveup), (unsigned long long)atomic_load(&g_plpq_spans), (unsigned long long)atomic_load(&g_plpq_inline)); }
    { char line[1600]; size_t o = 0; os_unfair_lock_lock(&g_plivs_lk);   // per caller since the start: invalidates / waited / ms waited
      for (int k = 0; k < 32 && g_plivs[k].caller && o < sizeof line - 80; k++)
          o += (size_t)snprintf(line + o, sizeof line - o, " main+%#llx %llu/%llu/%.0f", (unsigned long long)g_plivs[k].caller, (unsigned long long)g_plivs[k].n, (unsigned long long)g_plivs[k].waited, pl_ns(g_plivs[k].ns) / 1e6);
      os_unfair_lock_unlock(&g_plivs_lk);
      if (o) MLOG("[pipeline] invalidates by caller (count / waited / ms waited, since the start):%s", line); }
    { static uint64_t last_pushed, last_seq, last_batches; uint64_t pushed = atomic_load(&g_plpq_pushed), batches = atomic_load(&g_plpq_batches), seq = g_plpq_seq;   // g_plpq_seq: written by this thread only
      MLOG("[pipeline] translate-thread hooks per submission (calls / ms, time sampled 1 in 16): readback-snapshot checks %.0f / %.3f, vertex streams %.0f / %.3f (largest-index scans %.2f over %.0f indices), index buffers %.0f / %.3f, other reads %.0f / %.3f, live reads %.0f / %.3f, GPU writes %.0f / %.3f; submission begin %.3f ms, commit %.3f ms; all %.3f ms; protection queue: %.0f spans pushed, %.0f after joining, in %.2f batches",
           g_plh_c[PLH_REDIR] / s, pl_ns(g_plh_t[PLH_REDIR]) / s / 1e6, g_plh_c[PLH_VB] / s, pl_ns(g_plh_t[PLH_VB]) / s / 1e6, g_plmi_scans / s, g_plmi_idx / s,
           g_plh_c[PLH_IB] / s, pl_ns(g_plh_t[PLH_IB]) / s / 1e6, g_plh_c[PLH_READ] / s, pl_ns(g_plh_t[PLH_READ]) / s / 1e6, g_plh_c[PLH_LIVE] / s, pl_ns(g_plh_t[PLH_LIVE]) / s / 1e6,
           g_plh_c[PLH_WRITE] / s, pl_ns(g_plh_t[PLH_WRITE]) / s / 1e6, pl_ns(g_plh_t[PLH_BEGIN]) / s / 1e6, pl_ns(g_plh_t[PLH_COMMIT]) / s / 1e6,
           pl_ns(g_plh_t[PLH_REDIR] + g_plh_t[PLH_VB] + g_plh_t[PLH_IB] + g_plh_t[PLH_READ] + g_plh_t[PLH_LIVE] + g_plh_t[PLH_WRITE] + g_plh_t[PLH_BEGIN] + g_plh_t[PLH_COMMIT]) / s / 1e6,
           (double)(pushed - last_pushed) / s, (double)(seq - last_seq) / s, (double)(batches - last_batches) / s);
      last_pushed = pushed; last_seq = seq; last_batches = batches;
      memset(g_plh_t, 0, sizeof g_plh_t); memset(g_plh_c, 0, sizeof g_plh_c); g_plmi_scans = g_plmi_idx = 0; }
    MLOG("[pipeline] decisions per submission by reason: untracked %.1f, static %.1f, volatile %.1f (cached); new %.1f, static written %.1f, volatile expired %.1f, ranges changed %.1f (classified)",
         g_pls_st.dc[0] / s, g_pls_st.dc[1] / s, g_pls_st.dc[2] / s, g_pls_st.dc[3] / s, g_pls_st.dc[4] / s, g_pls_st.dc[5] / s, g_pls_st.dc[6] / s);
    MLOG("[pipeline] per submission: bookkeeping %.3f ms on the translate thread (decisions %.0f in %.3f ms, sampled; commit %.3f ms); snapshots %.1f (%.1f KB, copy %.3f ms; vertex %.1f index %.1f storage %.1f uniform %.1f copy %.1f; %.1f dropped by GPU writes), memo hits %.0f, static cache hits %.0f; in place %.1f (static %.1f, GPU-written or untracked %.1f, volatile but too big or over the budget %.1f (over budget %.1f), no snapshot possible %.1f; vertex %.1f index %.1f storage %.1f uniform %.1f copy %.1f texture %.1f), GPU writes %.1f; published ranges %.0f (not static %.1f)",
         pl_ns(g_pls_st.ns + g_pls_st.t_decide) / s / 1e6, g_pls_st.decides / s, pl_ns(g_pls_st.t_decide) / s / 1e6, pl_ns(g_pls_st.t_commit) / s / 1e6,
         snaps / s, g_pls_st.snap_bytes / s / 1024, pl_ns(g_pls_st.snap_ns) / s / 1e6, g_pls_st.snaps[PLK_VB] / s, g_pls_st.snaps[PLK_IB] / s, g_pls_st.snaps[PLK_SB] / s, g_pls_st.snaps[PLK_UB] / s, g_pls_st.snaps[PLK_CP] / s, g_pls_st.snap_drops / s,
         g_pls_st.memo_hits / s, g_pls_st.cache_hits / s, lives / s, g_pls_st.live_static / s, g_pls_st.live_gpuw / s, g_pls_st.live_big / s, g_pls_st.over_budget / s, g_pls_st.live_noshot / s,
         g_pls_st.live[PLK_VB] / s, g_pls_st.live[PLK_IB] / s, g_pls_st.live[PLK_SB] / s, g_pls_st.live[PLK_UB] / s, g_pls_st.live[PLK_CP] / s, g_pls_st.live[PLK_TX] / s,
         g_pls_st.writes / s, g_pls_st.ranges / s, g_pls_st.nonstatic / s);
    MLOG("[pipeline] last 600: holds %llu (host writes %llu, %.2f ms total, timed out %llu), passes (another part of the page) %llu, writes over GPU writes only (not held) %llu, dry-run would-hold %llu, host threads not held %llu, watched GPU-written pages read %llu / written %llu; pages protected again at releases %llu (unprotect ring overflows %llu), put on probation %llu, protected at publication %llu, left writable after repeated passes %llu, spans over 16 MiB %llu; readback snapshots: active %d (activated %llu, learned from invalidates %llu, grown to a binding %llu), published %llu, CPU merges %llu, conflicts %llu, ring full %llu, redirects %llu",
         (unsigned long long)d[NVN_PLS_HOLD], (unsigned long long)d[NVN_PLS_HOLD_HOST], d[NVN_PLS_HOLD_NS] / 1e6, (unsigned long long)d[NVN_PLS_TIMEOUT], (unsigned long long)d[NVN_PLS_PASS], (unsigned long long)d[NVN_PLS_WWPASS], (unsigned long long)d[NVN_PLS_WOULD],
         (unsigned long long)d[NVN_PLS_HOSTSKIP], (unsigned long long)d[NVN_PLS_WATCH_READ], (unsigned long long)d[NVN_PLS_WATCH_WRITE], (unsigned long long)d[NVN_PLS_REPROT], (unsigned long long)d[NVN_PLS_URING_OVER], (unsigned long long)d[NVN_PLS_PROBATION],
         (unsigned long long)d[NVN_PLS_SPANPROT], (unsigned long long)d[NVN_PLS_PASSKEEP], (unsigned long long)d[NVN_PLS_BIGSPAN],
         atomic_load(&g_plds_live), (unsigned long long)atomic_load(&g_plds_act), (unsigned long long)atomic_load(&g_plds_learned), (unsigned long long)atomic_load(&g_plds_grown), (unsigned long long)atomic_load(&g_plds_pub), (unsigned long long)atomic_load(&g_plds_merge),
         (unsigned long long)atomic_load(&g_plds_conflict), (unsigned long long)atomic_load(&g_plds_skip), (unsigned long long)g_pls_st.dsnap_redirects);
    if (d[NVN_PLS_HOLD] || d[NVN_PLS_WOULD] || d[NVN_PLS_HOLD_HOST] || d[NVN_PLS_WATCH_READ] || d[NVN_PLS_WATCH_WRITE]) {   // who: the latest held / would-be held / watched accesses, with the code that called them
        extern int runtime_module_offset(uint64_t pc, const char **name, uint64_t *off);
        nvn_pl_hold_rec h[64]; int nh = nvn_pl_holds(h, 64);
        struct { uint64_t pc, bt[NVN_PL_BT]; int n; uint32_t kind, kinds; } by[10]; int nb = 0;
        static const char *const kn[4] = {"write", "host write", "READ of GPU result", "write of GPU result"};
        for (int i = 0; i < nh; i++) { int j = 0; while (j < nb && (by[j].pc != h[i].pc || by[j].kind != h[i].host || by[j].bt[0] != h[i].bt[0])) j++;
            if (j == nb) { if (nb == 10) continue; by[nb].pc = h[i].pc; memcpy(by[nb].bt, h[i].bt, sizeof by[nb].bt); by[nb].n = 0; by[nb].kind = h[i].host; by[nb].kinds = 0; nb++; } by[j].n++; by[j].kinds |= h[i].kinds; }
        char line[2400]; size_t o = 0;
        for (int j = 0; j < nb && o < sizeof line - 300; j++) {
            o += (size_t)snprintf(line + o, sizeof line - o, " [%s%s x%d:", kn[by[j].kind & 3], by[j].kinds == 1 ? " over a GPU read" : by[j].kinds == 2 ? " over a GPU write" : by[j].kinds == 3 ? " over a GPU read and write" : "", by[j].n);
            for (int q = -1; q < NVN_PL_BT && o < sizeof line - 120; q++) {
                uint64_t pc = q < 0 ? by[j].pc : by[j].bt[q]; if (!pc) break;
                const char *nm = "?"; uint64_t off = 0; Dl_info di;
                if (runtime_module_offset(pc, &nm, &off)) o += (size_t)snprintf(line + o, sizeof line - o, " %s+%#llx", nm, (unsigned long long)off);
                else if (dladdr((void *)(uintptr_t)pc, &di) && di.dli_fname) {   // host code: image + offset (atos -o <image> -l 0x0 <offset>), and the nearest exported symbol
                    const char *bn = strrchr(di.dli_fname, '/'); bn = bn ? bn + 1 : di.dli_fname;
                    o += (size_t)snprintf(line + o, sizeof line - o, " %s+%#llx(%s)", bn, (unsigned long long)(pc - (uint64_t)(uintptr_t)di.dli_fbase), di.dli_sname ? di.dli_sname : "?"); }
                else o += (size_t)snprintf(line + o, sizeof line - o, " %#llx", (unsigned long long)pc);
            }
            o += (size_t)snprintf(line + o, sizeof line - o, "]");
        }
        MLOG("[pipeline] latest %d holds / watched accesses (pc, then callers):%s; newest at %#llx, fence %llu, released %llu", nh, line, nh ? (unsigned long long)h[0].addr : 0ull,
             nh ? (unsigned long long)h[0].rfence : 0ull, nh ? (unsigned long long)h[0].released : 0ull);
    }
    memset(g_pls_st.snaps, 0, sizeof g_pls_st.snaps); memset(g_pls_st.live, 0, sizeof g_pls_st.live);
    g_pls_st.snap_bytes = g_pls_st.snap_ns = g_pls_st.memo_hits = g_pls_st.cache_hits = g_pls_st.live_static = g_pls_st.live_gpuw = g_pls_st.live_big = g_pls_st.over_budget = 0;
    g_pls_st.live_noshot = g_pls_st.live_untracked = g_pls_st.snap_drops = g_pls_st.ranges = g_pls_st.nonstatic = g_pls_st.writes = g_pls_st.ns = 0;
    g_pls_st.t_decide = g_pls_st.t_commit = g_pls_st.decides = 0; memset(g_pls_st.dc, 0, sizeof g_pls_st.dc);
}
