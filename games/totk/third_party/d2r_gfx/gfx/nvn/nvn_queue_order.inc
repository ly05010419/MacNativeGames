/* Guest queue ordering (2026-11-08, Ocarina of Time white frames).
 *
 * Ocarina of Time records cross-queue dependencies with nvnCommandBufferWaitEvent: queue 1 signals timeline events at
 * points through its submission; three other queues wait on them (and queue 1 waits on theirs). The host replays every
 * guest queue into one Metal queue in submit order, so a consumer submitted before its producer read the previous
 * frame's data: measured 56 % of the waits unready at replay, and 19-32 % white frames in the village when the replay ran
 * ahead of the GPU (nxnative #1703 / #1245 found the same on this game).
 *
 * Each guest queue keeps a FIFO of work. A submission runs at once when its queue has nothing parked. The replay stops
 * at an unready wait (nvn_mtl.m mtl_exec), commits what it encoded and parks the rest: a cursor into the guest's own
 * command memory, the queue's replay state and the uniforms captured inside the submit call. Whenever work progresses,
 * the oldest ready queue head runs next. A signal that readies a parked head also stops its own replay right after it.
 * Syncs fenced on a queue with parked work resolve when that work has been committed. Nothing is reordered when no wait
 * is unready, which is the common case on games without cross-queue waits.
 *
 * Safety: a head parked for more than OOT_QUEUE_ORDER_TIMEOUT_MS (200) while nothing can progress is resumed past its
 * wait with a red line: a wait the host cannot see satisfied (signalled by memory writes it does not track) must not hang
 * the game. OOT_QUEUE_ORDER=0 restores the former in-order replay. */
#include <pthread.h>
extern int mtl_qo_event_ready(volatile uint32_t *val, uint32_t mode, uint32_t target);
extern void mtl_qo_cpu_signal(volatile uint32_t *val);
extern int mtl_qo_event_gpu_projected(volatile uint32_t *val);
extern void mtl_qo_arm(int on);
extern int mtl_qo_stopped(void);
extern int mtl_qo_cursor(const uint8_t **out, int max);
extern void mtl_qo_set_callers(const uint8_t *const *outer_first, int n);
extern void *mtl_qo_state_save(void);
extern void mtl_qo_state_restore(void *p);
extern int (*g_qo_signal_releases)(volatile uint32_t *val);
extern void *mtl_sub_capture_ubos(const uint64_t *h, int n);
extern void mtl_ucap_free(void *c);
extern void *mtl_ucap_overlay(const void *base, const void *over);   // over NULL: a copy (the snapshot takes ownership of what it is given)
extern void mtl_sub_snapshot_ubos_cap(const uint64_t *h, int n, void *cap);
extern void mtl_sub_snapshot_ubos_rest(const uint64_t *h, int n, int ncur, uint64_t top, void *cap);
extern void mtl_sub_index(const uint64_t *h, int n);

typedef struct qo_q qo_q;
typedef struct qo_item {
    struct qo_item *next; qo_q *Q;
    uint64_t seq, qseq;                    // global order, order within the guest queue
    uint64_t *h; int n, next_h;           // the submission's handles; handles not yet started after the cursor
    const uint8_t *cur[40]; int ncur;      // innermost first: the stop point, then each caller's next op
    int started, force, flushed;           // force: resume past the wait at cur[0] (safety); flushed: nvnQueueFlush sent it to the GPU
    void *ucap, *state;                    // uniforms as of the submit call; replay state S / R at the stop
    volatile uint32_t *wval; uint32_t wmode, wtarget;   // the wait it stopped at (NULL: stopped after a releasing signal)
    const sync_t *wsync;                   // a GPU-side WaitSync on a sync another queue has not reached yet
    uint64_t parked_ns;
    win_t *pwin; int pidx, pcrop[4]; double pperiod; uint64_t pframe; int present;   // a queued nvnQueuePresentTexture
    uint64_t sub_frame, sub_ns, flush_ns;   // diagnostics: game frame and time at the submit / the flush
} qo_item;
struct qo_q { const NVNqueue *q; qo_item *head, *tail; uint64_t seq_in, seq_done; };
static qo_q g_qo_q[32]; static int g_qo_nq;
static uint64_t g_qo_seq, g_qo_parks, g_qo_resumes, g_qo_forced, g_qo_items, g_qo_cpu_resumes, g_qo_gpu_resumes;
static qo_item *g_qo_running;
/* Diagnostics: the last 512 queue events, printed when the process aborts (runtime_abort_hook). */
typedef struct { uint64_t t, q, a, b, c; uint32_t kind, frame; } qo_ev;
static qo_ev g_qo_ring[512]; static _Atomic uint32_t g_qo_ring_n;
enum { QE_SUB, QE_FLUSH, QE_RUN, QE_END, QE_PRES_Q, QE_PRES_X, QE_FENCE, QE_PSYNC, QE_SYNCW_IN, QE_SYNCW_OUT, QE_ACQ, QE_CPUSIG, QE_CBSYNC, QE_DRAIN_IN, QE_DRAIN_OUT, QE_GPUW_IN, QE_GPUW_OUT, QE_ACQ_OUT };
static void qo_ev_add(uint32_t kind, const void *q, uint64_t a, uint64_t b, uint64_t c) {
    uint32_t i = atomic_fetch_add(&g_qo_ring_n, 1) & 511;
    g_qo_ring[i] = (qo_ev){now_ns(), (uint64_t)(uintptr_t)q, a, b, c, kind, (uint32_t)g_frame_pub};
    static int at = -1; if (at < 0) { const char *e = getenv("OOT_QO_RING_AT"); at = e ? atoi(e) : 0; }   // diagnostics: print the ring once at this frame
    if (at > 0 && g_frame_pub >= (uint64_t)at + 3) { at = 0; void qo_ring_dump(void); qo_ring_dump(); }
}
void qo_note(uint32_t kind, const void *q, uint64_t a, uint64_t b) { qo_ev_add(kind, q, a, b, 0); }
void qo_ring_dump(void) {
    static const char *nm[] = {"submit(seq,h0,n)", "flush", "run(seq,started,next_h)", "end(seq,stopped,0)", "present-queued(idx,seq)", "present-run(idx,frame)", "fence-sync(sync,pending)", "sync-resolved(sync,qseq)",
        "sync-wait-in(sync)", "sync-wait-out(sync,ok)", "acquire(idx)", "cpu-signal(event,value)", "cb-sync(sync,qseq)", "drain-in", "drain-out", "gpu-wait-in(fence,want)", "gpu-wait-out(fence,ok)", "acquire-out(idx)"};
    uint32_t n = atomic_load(&g_qo_ring_n), first = n > 512 ? n - 512 : 0; uint64_t tl = n ? g_qo_ring[(n - 1) & 511].t : 0;
    fprintf(stderr, "[queue-order] last %u queue events (ms before the abort; thread-unsafe snapshot):\n", n - first);
    for (uint32_t k = first; k < n; k++) { const qo_ev *e = &g_qo_ring[k & 511];
        fprintf(stderr, "  %9.3f F%u q=%llx %-26s %llx %llx %llx\n", (double)(tl - e->t) / 1e6, e->frame, (unsigned long long)e->q, e->kind < 18 ? nm[e->kind] : "?", (unsigned long long)e->a, (unsigned long long)e->b, (unsigned long long)e->c); }
    const qo_item *r = g_qo_running;
    if (r) { fprintf(stderr, "[queue-order] running: q=%p seq=%llu started=%d next_h=%d n=%d ncur=%d sub F%llu present=%d handles:", (const void *)r->Q->q, (unsigned long long)r->seq, r->started, r->next_h, r->n, r->ncur, (unsigned long long)r->sub_frame, r->present);
        for (int i = 0; i < r->n && i < 64; i++) fprintf(stderr, " %llx", (unsigned long long)r->h[i]); fprintf(stderr, "\n cursor:"); for (int k = 0; k < r->ncur; k++) fprintf(stderr, " %p", (const void *)r->cur[k]); fprintf(stderr, "\n"); }
}
static int g_qo_pres_pending[4];
/* Releases that must wait for the submissions made before them (OOT_QO_ASYNC). NVN's CPU objects may be finalized or
   their control memory added again as soon as the submit call returns; the translation thread did the same in queue order
   (XJ_TEXREL, retired command copies). after: g_qo_seq at the release; it runs once every item up to it has finished. */
typedef struct { uint64_t after; tex_t tex; } qo_texrel;
static qo_texrel *g_qo_texrel; static size_t g_qo_ntexrel, g_qo_ctexrel;
static int qo_replay_async(void);
static uint64_t qo_replayed_locked(void);
static uint64_t g_qo_park_max_ns, g_qo_sig_stops, g_qo_sig_empty;   // longest park before a resume since the last stats line   // queued presents per swapchain image (one window)
static void present_exec(win_t *o, int idx, double period, const int crop[4], uint64_t frame);   // hle_nvn.c
typedef struct { sync_t *s; qo_q *Q; uint64_t qseq; } qo_psync;
static qo_psync *g_qo_psync; static int g_qo_npsync, g_qo_psync_cap;
static pthread_cond_t g_qo_cv = PTHREAD_COND_INITIALIZER;   // broadcast (under g_qo_mu) whenever an item finishes or parks
/* Locks (2026-11-08, frame pacing): g_qo_mu guards the queues, items' links and flags and the pending syncs; the replay
   section guards the encoding. Order: replay section, then g_qo_mu; g_qo_mu is never held across a replay. */
static pthread_mutex_t g_qo_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_qo_kick_cv = PTHREAD_COND_INITIALIZER; static int g_qo_kick;
/* OOT_QO_ASYNC=1 (off by default): flushed work replays on the queue-order thread, the way the GPU runs a flushed queue
   while the CPU goes on; presents are queued behind their frame; a guest thread waits only where NVN makes it wait (a
   sync, an acquire, a finish). Measured 2026-11-08 (village, same App and save): the main thread had spent 67 % of its
   time replaying inside nvnEventSignal, but moved off it the game waits for the previous frame's GPU completion every
   frame (SyncWait), so the frame chain stays GPU done -> CPU signal -> replay the rest -> GPU: 38.0 fps queued against
   39.5 fps in place. Kept for A/B; its lifetime rules (command copies and texture releases after the submissions made
   before them) are needed whenever it is on. */
static int qo_async(void) { static int on = -1; if (on < 0) { const char *e = getenv("OOT_QO_ASYNC"); on = e && atoi(e); LOG("[nvn] queue replay %s", on ? "on the queue-order thread (OOT_QO_ASYNC)" : "on the guest thread that flushes / signals (OOT_QO_ASYNC=0)"); } return on; }
static void qo_kick_locked(void) { g_qo_kick = 1; pthread_cond_signal(&g_qo_kick_cv); }
static int qo_defer_to_flush(void) { static int on = -1; if (on < 0) { const char *e = getenv("OOT_REPLAY_AT_FLUSH"); on = !(e && !atoi(e)); LOG("[nvn] submissions replay %s", on ? "at nvnQueueFlush (NVN semantics)" : "inside the submit call (OOT_REPLAY_AT_FLUSH=0)"); } return on; }
static int qo_on(void) { static int on = -1; if (on < 0) { const char *e = getenv("OOT_QUEUE_ORDER"); on = !(e && !atoi(e)); LOG("[nvn] guest queue ordering %s", on ? "on: cross-queue waits are honoured" : "off (OOT_QUEUE_ORDER=0)"); } return on; }
static uint64_t qo_timeout_ns(void) { static uint64_t t; if (!t) { const char *e = getenv("OOT_QUEUE_ORDER_TIMEOUT_MS"); t = (uint64_t)(e ? atoi(e) : 200) * 1000000ull; if (!t) t = 1; } return t; }
static qo_q *qo_queue(const NVNqueue *q) {
    for (int i = 0; i < g_qo_nq; i++) if (g_qo_q[i].q == q) return &g_qo_q[i];
    if (g_qo_nq >= 32) die("more than 32 guest queues");
    g_qo_q[g_qo_nq] = (qo_q){.q = q}; return &g_qo_q[g_qo_nq++];
}
static int qo_pending(void) { for (int i = 0; i < g_qo_nq; i++) if (g_qo_q[i].head) return 1; return 0; }
static int qo_queue_pending(const NVNqueue *q) { for (int i = 0; i < g_qo_nq; i++) if (g_qo_q[i].q == q) return g_qo_q[i].head != NULL; return 0; }
static void qo_psync_resolve(void) {
    for (int i = 0; i < g_qo_npsync; i++) {
        qo_psync *p = &g_qo_psync[i];
        if (p->Q->seq_done >= p->qseq) { qo_ev_add(QE_PSYNC, p->Q->q, (uint64_t)(uintptr_t)p->s, p->qseq, 0); p->s->fence = mtl_last_fence(); p->s->fenced = 1; g_qo_psync[i--] = g_qo_psync[--g_qo_npsync]; }
    }
    pthread_cond_broadcast(&g_qo_cv);
}
static int qo_ready(const qo_item *it) {
    if (!it->flushed) return 0;   // NVN: submitted commands reach the GPU at nvnQueueFlush
    if (it->present) return 1;     // its queue's earlier work is done: it is the head
    if (it->wsync) return !(it->wsync->fenced & 8);
    if (!it->started || it->force || !it->wval) return 1;
    return mtl_qo_event_ready(it->wval, it->wmode, it->wtarget);
}
/* nvn_mtl.m, while replaying a SignalEvent: does it ready another queue's parked head? */
static int qo_signal_releases(volatile uint32_t *val) {
    int r = 0;
    pthread_mutex_lock(&g_qo_mu);
    for (int i = 0; i < g_qo_nq && !r; i++) {
        const qo_item *h = g_qo_q[i].head;
        if (h && h != g_qo_running && h->started && h->wval == val && mtl_qo_event_ready(val, h->wmode, h->wtarget)) r = 1;
    }
    pthread_mutex_unlock(&g_qo_mu);
    return r;
}
static void qo_finish_item(qo_item *it) {
    /* Syncs it fences were registered when it was queued (qo_mark_cb_syncs) and resolve in qo_psync_resolve: a sync the
       next frame queued again must stay pending, which a rewrite here would undo. */
    atomic_store(&g_sub_fence_prev, atomic_load(&g_sub_fence_last)); atomic_store(&g_sub_fence_last, mtl_last_fence());
    it->Q->seq_done = it->qseq;
    if (it->ucap) mtl_ucap_free(it->ucap);
    free(it->state); free(it->h); free(it);
}
/* After a releasing signal: is nothing left but the ends of the lists (each cursor level at OP_END, no handle to start)?
   Ocarina of Time ends a producer's submission with the signal its consumers wait for. Parking that empty rest let the
   consumer finish and its sync resolve first; the game then recycled the producer's command memory (it may: on the GPU
   that submission is complete) while the host still had its OP_END to read. */
static int qo_rest_empty(const uint8_t *const *cur, int ncur, int handles_left) {
    if (handles_left) return 0;
    for (int k = 0; k < ncur; k++) {
        const uint8_t *p = cur[k];
        for (int j = 0; j < 64; j++) { op_hdr h; memcpy(&h, p, sizeof h); if (h.op != OP_JUMP) break; memcpy(&p, p + 8, 8); }
        op_hdr h; memcpy(&h, p, sizeof h); if (h.op != OP_END) return 0;
    }
    return 1;
}
/* Diagnostics: walks a command stream as the uniform snapshot will; 0 at the first op that is not a recording. */
static int qo_stream_ok(const uint8_t *p, int depth, const uint8_t **bad) {
    for (int guard = 0; guard < 1 << 20; guard++) {
        op_hdr h; memcpy(&h, p, sizeof h);
        if (h.size < 8 || (h.size & 7) || h.op >= OP_COUNT_) { *bad = p; return 0; }
        if (h.op == OP_END) return 1;
        if (h.op == OP_JUMP) { memcpy(&p, p + 8, 8); continue; }
        if (h.op == OP_CALL) { uint64_t t; memcpy(&t, p + 8, 8); if (depth < 16 && !qo_stream_ok((const uint8_t *)(uintptr_t)t, depth + 1, bad)) return 0; }
        p += h.size;
    }
    return 1;
}
static void qo_check_rest(const qo_item *it, const uint64_t *w, int nw) {
    static int on = -1; if (on < 0) { const char *e = getenv("OOT_QO_CHECK"); on = e && atoi(e); }   // diagnostics, off by default: walks every stream once more
    if (!on) return;
    const uint8_t *bad = NULL; int i = 0;
    for (; i < nw; i++) if (!qo_stream_ok((const uint8_t *)(uintptr_t)w[i], 0, &bad)) break;
    if (i == nw) return;
    uint64_t now = now_ns();
    LOG("【红】[queue-order] recycled command memory before its replay: queue %p seq %llu (queue seq %llu) started=%d stream %d/%d (cursor levels %d, handles %d..%d) bad op at %p; submitted F%llu %.1f ms ago, flushed %.1f ms ago, parked %.1f ms; now F%llu; event %p mode %u target %u memory %u",
        (const void *)it->Q->q, (unsigned long long)it->seq, (unsigned long long)it->qseq, it->started, i, nw, it->ncur, it->next_h, it->n, (const void *)bad,
        (unsigned long long)it->sub_frame, (now - it->sub_ns) / 1e6, it->flush_ns ? (now - it->flush_ns) / 1e6 : -1.0, it->started ? (now - it->parked_ns) / 1e6 : 0.0,
        (unsigned long long)g_frame_pub, (void *)it->wval, it->wmode, it->wtarget, it->wval ? *it->wval : 0);
    pthread_mutex_lock(&g_qo_mu);
    for (int k = 0; k < g_qo_nq; k++) { int c = 0; for (qo_item *x = g_qo_q[k].head; x; x = x->next) c++;
        const qo_item *hd = g_qo_q[k].head;
        LOG("【红】[queue-order]   queue %p: %d queued, done %llu / in %llu; head seq %llu started=%d flushed=%d present=%d wsync=%p sub F%llu", (const void *)g_qo_q[k].q, c,
            (unsigned long long)g_qo_q[k].seq_done, (unsigned long long)g_qo_q[k].seq_in, hd ? (unsigned long long)hd->seq : 0ull, hd ? hd->started : -1, hd ? hd->flushed : -1, hd ? hd->present : -1, hd ? (const void *)hd->wsync : NULL, hd ? (unsigned long long)hd->sub_frame : 0ull); }
    for (int k = 0; k < g_qo_npsync; k++) LOG("【红】[queue-order]   pending sync %p on queue %p up to %llu", (void *)g_qo_psync[k].s, (const void *)g_qo_psync[k].Q->q, (unsigned long long)g_qo_psync[k].qseq);
    pthread_mutex_unlock(&g_qo_mu);
    die("guest queue: command memory recycled before its replay (see the lines above)");
}
/* Runs (or resumes) an item on the replay thread, replay section held. Returns 1 when it stopped again. */
static int qo_run(qo_item *it) {
    gfx_enter(); totk_tl(TL_REP_BEGIN, 0, 0);
    pin_step(__atomic_load_n(&g_submits, __ATOMIC_RELAXED));
    uint64_t t0 = now_ns(), c0 = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID), i0, y0, s0 = thr_sys_ns(); thr_counts(&i0, &y0);
    atomic_fetch_add(&dm_submit_busy, 1);
    g_qwait_queue_set((uint64_t)(uintptr_t)it->Q->q);
    g_qo_running = it;
    const uint64_t *h = it->h; const int n = it->n;
    mtl_qo_arm(1);
    mtl_sub_begin();
    /* Uniforms of what is still to run. A resumed item walks only its rest: each cursor level's next op, then the handles
       not started. The part already replayed may have been recycled by the game once its syncs said so (seen with the
       queued present: the game ran a frame ahead and rewrote a command list the snapshot walked: corrupt recording). */
    if (it->started) {
        int nr = it->ncur + (n - it->next_h); uint64_t rest[40 + 256], *rp = nr <= (int)(sizeof rest / sizeof *rest) ? rest : malloc(sizeof *rp * (size_t)nr);
        if (!rp) die("guest queue rest");
        for (int k = 0; k < it->ncur; k++) rp[k] = (uint64_t)(uintptr_t)it->cur[k];
        for (int i = it->next_h; i < n; i++) rp[it->ncur + i - it->next_h] = h[i];
        qo_check_rest(it, rp, nr);
        mtl_sub_snapshot_ubos_rest(rp, nr, it->ncur, it->ncur && it->next_h > 0 ? h[it->next_h - 1] : 0, it->ucap ? mtl_ucap_overlay(it->ucap, NULL) : NULL);   /* the rest from the copies' sorted lists, by the cursors' places in them */
        if (rp != rest) free(rp);
    } else { qo_check_rest(it, h, n); mtl_sub_snapshot_ubos_cap(h, n, it->ucap ? mtl_ucap_overlay(it->ucap, NULL) : NULL);   /* uniforms as of the submit call (a copy: the snapshot owns it) */ }
    mtl_sub_index(h + it->next_h, n - it->next_h);
    int stopped = 0; const uint8_t *cur[40]; int ncur = 0;
    if (it->started) {
        mtl_qo_state_restore(it->state);
        g_qo_resumes++; { uint64_t age = now_ns() - it->parked_ns; if (age > g_qo_park_max_ns) g_qo_park_max_ns = age; }
        if (it->force) { op_hdr hh; memcpy(&hh, it->cur[0], sizeof hh); it->cur[0] += hh.size; it->force = 0; }   // past the wait the host cannot see satisfied
        for (int k = 0; k < it->ncur; k++) {
            const uint8_t *outer[40]; int no = 0;
            for (int j = it->ncur - 1; j > k; j--) outer[no++] = it->cur[j];
            mtl_qo_set_callers(outer, no);
            mtl_exec_rest(h + it->next_h, n - it->next_h);
            mtl_exec(it->cur[k]);
            if (mtl_qo_stopped()) {
                ncur = mtl_qo_cursor(cur, 40);
                for (int j = k + 1; j < it->ncur; j++) { if (ncur >= 40) die("guest queue cursor overflow"); cur[ncur++] = it->cur[j]; }
                stopped = 1; break;
            }
        }
        mtl_qo_set_callers(NULL, 0);
    }
    for (int i = it->next_h; !stopped && i < n; i++) {
        it->next_h = i + 1;
        mtl_exec_rest(h + i + 1, n - i - 1); mtl_exec((const uint8_t *)h[i]);
        if (mtl_qo_stopped()) { ncur = mtl_qo_cursor(cur, 40); stopped = 1; }
    }
    mtl_exec_rest(NULL, 0);
    if (stopped) {   // stopped after a releasing signal with nothing left: the item is complete
        op_hdr sh; memcpy(&sh, cur[0], sizeof sh);
        if (sh.op != OP_WAIT_EVENT) { g_qo_sig_stops++; if (qo_rest_empty(cur, ncur, it->next_h < n)) { g_qo_sig_empty++; stopped = 0; } }
    }
    if (stopped) {
        free(it->state); it->state = mtl_qo_state_save();
        memcpy(it->cur, cur, sizeof *cur * (size_t)ncur); it->ncur = ncur;
        op_hdr wh; memcpy(&wh, cur[0], sizeof wh);
        if (wh.op == OP_WAIT_EVENT) { memcpy((void *)&it->wval, cur[0] + 8, 8); memcpy(&it->wmode, cur[0] + 16, 4); memcpy(&it->wtarget, cur[0] + 20, 4); }
        else it->wval = NULL;   // stopped after a signal that readied another queue: ready itself
        it->parked_ns = now_ns();
        /* No uniform capture here: the rest runs on the GPU after its producer, and reads memory as it is then. A capture
           taken at the stop handed the forward pass blocks the game's worker threads had not filled yet (white frames). */
        if (!it->started) g_qo_parks++;
        it->started = 1;
    }
    mtl_sub_commit();
    mtl_qo_arm(0); g_qo_running = NULL;
    atomic_fetch_sub(&dm_submit_busy, 1);
    uint64_t dw = now_ns() - t0;
    atomic_fetch_add(&g_nvn_ns, dw); atomic_fetch_add(&g_sub_wall_ns, dw);
    uint64_t i1, y1; thr_counts(&i1, &y1); uint64_t dc = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID) - c0, di = i1 - i0;
    atomic_fetch_add(&g_cum_sub_cyc, y1 - y0); atomic_fetch_add(&g_cum_sub_sys_ns, thr_sys_ns() - s0); atomic_fetch_add(&g_cum_sub_wall_ns, dw);
    atomic_fetch_add(&g_sub_cpu_ns, dc); atomic_fetch_add(&g_cum_sub_cpu_ns, dc);
    atomic_fetch_add(&g_sub_instr, di); atomic_fetch_add(&g_cum_sub_instr, di);
    totk_tl(TL_REP_END, 0, 0); gfx_leave();
    return stopped;
}
/* The replay section is held (g_qo_mu is not). Runs ready queue heads, oldest first, until none can progress; with
   one_item, returns after one run so the caller can let other users of the replay section in. Returns runs made. */
static int qo_pump_n(int one_item) {
    int runs = 0;
    for (;;) {
        pthread_mutex_lock(&g_qo_mu);
        qo_item *best = NULL;
        for (int i = 0; i < g_qo_nq; i++) { qo_item *h = g_qo_q[i].head; if (h && qo_ready(h) && (!best || h->seq < best->seq)) best = h; }
        if (!best) {
            qo_item *oldest = NULL;   // safety: nothing can progress and a head has waited too long
            for (int i = 0; i < g_qo_nq; i++) { qo_item *h = g_qo_q[i].head; if (h && h->started && !h->wsync && (!oldest || h->parked_ns < oldest->parked_ns)) oldest = h; }
            if (!oldest || now_ns() - oldest->parked_ns < qo_timeout_ns()) { pthread_mutex_unlock(&g_qo_mu); break; }
            g_qo_forced++;
            if (g_qo_forced <= 16) LOG("【红】[queue-order] queue %p waited %.0f ms on event %p (mode %u, value %u; memory %u) with nothing able to progress: resumed past the wait",
                (const void *)oldest->Q->q, (now_ns() - oldest->parked_ns) / 1e6, (void *)oldest->wval, oldest->wmode, oldest->wtarget, oldest->wval ? *oldest->wval : 0);
            oldest->force = 1; best = oldest;
        }
        if (best->present) {   // the frame's work on its queue is committed: show it
            pthread_mutex_unlock(&g_qo_mu);
            qo_ev_add(QE_PRES_X, best->Q->q, (uint64_t)best->pidx, best->pframe, 0);
            present_exec(best->pwin, best->pidx, best->pperiod, best->pcrop, best->pframe);
            pthread_mutex_lock(&g_qo_mu);
            best->Q->head = best->next; if (!best->Q->head) best->Q->tail = NULL; best->Q->seq_done = best->qseq;
            if (best->pidx >= 0 && best->pidx < 4) g_qo_pres_pending[best->pidx]--;
            free(best); qo_psync_resolve(); pthread_mutex_unlock(&g_qo_mu); runs++;
            if (one_item) break; else continue;
        }
        if (best->wsync) { best->Q->head = best->next; if (!best->Q->head) best->Q->tail = NULL; best->Q->seq_done = best->qseq; free(best); qo_psync_resolve(); pthread_mutex_unlock(&g_qo_mu); continue; }
        if (best->started && best->wval && !best->force) { if (mtl_qo_event_gpu_projected(best->wval)) g_qo_gpu_resumes++; else g_qo_cpu_resumes++; }
        pthread_mutex_unlock(&g_qo_mu);
        qo_ev_add(QE_RUN, best->Q->q, best->seq, (uint64_t)best->started, (uint64_t)best->next_h);
        int stopped = qo_run(best);
        qo_ev_add(QE_END, best->Q->q, best->seq, (uint64_t)stopped, 0);   // the item stays its queue's head while it runs: syncs fenced on that queue stay pending
        runs++;
        pthread_mutex_lock(&g_qo_mu);
        if (!stopped) { qo_q *Q = best->Q; Q->head = best->next; if (!Q->head) Q->tail = NULL; qo_finish_item(best); qo_psync_resolve(); }
        else pthread_cond_broadcast(&g_qo_cv);
        pthread_mutex_unlock(&g_qo_mu);
        if (one_item) break;
    }
    static uint64_t last; uint64_t f = g_frame_pub;
    if (f >= last + 600) { last = f; LOG("[queue-order] F%llu items=%llu parked=%llu resumed=%llu (after a CPU signal %llu, a GPU signal %llu) forced=%llu longest-park=%.1f ms signal-stops=%llu (rest empty %llu)", (unsigned long long)f, (unsigned long long)g_qo_items,
        (unsigned long long)g_qo_parks, (unsigned long long)g_qo_resumes, (unsigned long long)g_qo_cpu_resumes, (unsigned long long)g_qo_gpu_resumes, (unsigned long long)g_qo_forced, g_qo_park_max_ns / 1e6, (unsigned long long)g_qo_sig_stops, (unsigned long long)g_qo_sig_empty); g_qo_park_max_ns = 0; }
    return runs;
}
static void qo_pump(void) { qo_pump_n(0); }
/* g_qo_mu held: every item with seq <= the result has finished (queues are FIFO: the oldest unfinished is a head). */
static uint64_t qo_replayed_locked(void) {
    uint64_t m = g_qo_seq;
    for (int i = 0; i < g_qo_nq; i++) if (g_qo_q[i].head && g_qo_q[i].head->seq - 1 < m) m = g_qo_q[i].head->seq - 1;
    return m;
}
static int qo_replay_async(void) { return qo_on() && qo_async() && !async_replay(); }
static uint64_t qo_pushed(void) { pthread_mutex_lock(&g_qo_mu); uint64_t v = g_qo_seq; pthread_mutex_unlock(&g_qo_mu); return v; }
static uint64_t qo_replayed(void) { pthread_mutex_lock(&g_qo_mu); uint64_t v = qo_replayed_locked(); pthread_mutex_unlock(&g_qo_mu); return v; }
int qo_texrel_defer(const tex_t *o) {
    if (!qo_replay_async()) return 0;
    pthread_mutex_lock(&g_qo_mu);
    int later = qo_replayed_locked() < g_qo_seq;
    if (later) {
        if (g_qo_ntexrel == g_qo_ctexrel) { g_qo_ctexrel = g_qo_ctexrel ? 2 * g_qo_ctexrel : 64; g_qo_texrel = realloc(g_qo_texrel, sizeof *g_qo_texrel * g_qo_ctexrel); if (!g_qo_texrel) die("queued texture releases"); }
        g_qo_texrel[g_qo_ntexrel++] = (qo_texrel){g_qo_seq, *o};
    }
    pthread_mutex_unlock(&g_qo_mu);
    return later;
}
/* Replay section held, g_qo_mu not: the deferred releases whose submissions have all been replayed. */
static void qo_texrel_sweep(void) {
    qo_texrel run[64]; size_t nr = 0;
    pthread_mutex_lock(&g_qo_mu);
    uint64_t done = qo_replayed_locked(); size_t k = 0;
    for (size_t i = 0; i < g_qo_ntexrel; i++) { if (g_qo_texrel[i].after <= done && nr < 64) run[nr++] = g_qo_texrel[i]; else g_qo_texrel[k++] = g_qo_texrel[i]; }
    g_qo_ntexrel = k;
    pthread_mutex_unlock(&g_qo_mu);
    for (size_t i = 0; i < nr; i++) { void live_del_x(const tex_t *o); live_del_x(&run[i].tex); mtl_tex_release(&run[i].tex); }
}
static int qo_pending_locked(void) { return qo_pending(); }
/* The queue-order thread: woken by a flush, a CPU signal, a sync wait or a present; otherwise every millisecond (events
   set by GPU completion, the safety valve). The replay section is released between items. */
static void *qo_worker(void *unused) {
    (void)unused; pthread_setname_np("NVN queue order");
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);   // the replay is the frame's critical path
    for (;;) {
        pthread_mutex_lock(&g_qo_mu);
        if (!g_qo_kick) { struct timespec ts = {0, 1000000}; pthread_cond_timedwait_relative_np(&g_qo_kick_cv, &g_qo_mu, &ts); }
        g_qo_kick = 0; int pend = qo_pending_locked();
        pthread_mutex_unlock(&g_qo_mu);
        if (!pend) continue;
        for (;;) { replay_enter(); int r = qo_pump_n(1); if (g_qo_ntexrel) qo_texrel_sweep(); replay_leave(); if (!r) break; }
    }
    return NULL;
}
static void qo_start_worker(void) {
    static int started; if (started) return; started = 1;
    pthread_t t; pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 4u << 20);
    if (pthread_create(&t, &a, qo_worker, NULL)) die("guest queue order worker"); pthread_attr_destroy(&a); pthread_detach(t);
}
static void qo_kick(void) { pthread_mutex_lock(&g_qo_mu); qo_kick_locked(); pthread_mutex_unlock(&g_qo_mu); }
/* Waits (g_qo_mu held) until done() or the deadline; the queue-order thread is kept awake. */
static int qo_wait_locked(int (*done)(const void *), const void *arg, uint64_t timeout, uint64_t t0) {
    for (;;) {
        if (done(arg)) return 1;
        if (!timeout || (timeout != UINT64_MAX && now_ns() - t0 >= timeout)) return 0;
        qo_kick_locked();
        struct timespec ts = {0, 500000}; pthread_cond_timedwait_relative_np(&g_qo_cv, &g_qo_mu, &ts);
    }
}
/* g_qo_mu held: s stands for queue Q's work up to qseq; resolved (fenced 1, the commit's fence) once that is committed. */
static void qo_psync_add(sync_t *s, qo_q *Q, uint64_t qseq) {
    for (int i = 0; i < g_qo_npsync; i++) if (g_qo_psync[i].s == s) g_qo_psync[i--] = g_qo_psync[--g_qo_npsync];
    if (g_qo_npsync == g_qo_psync_cap) { g_qo_psync_cap = g_qo_psync_cap ? 2 * g_qo_psync_cap : 256; g_qo_psync = realloc(g_qo_psync, sizeof *g_qo_psync * (size_t)g_qo_psync_cap); if (!g_qo_psync) die("guest queue syncs"); }
    g_qo_psync[g_qo_npsync++] = (qo_psync){s, Q, qseq}; s->fenced = 9; s->fence = 0;
}
/* g_qo_mu held: FenceSync recorded in a queued submission. The submit call used to replay it at once, so a SyncWait right
   after the flush found it fenced; queued, it is pending (fenced & 8) until its submission has been committed. */
static void qo_mark_cb_syncs(const uint8_t *p, int depth, qo_q *Q, uint64_t qseq) {
    for (;;) {
        op_hdr h; memcpy(&h, p, sizeof h);
        if (h.size < 8 || (h.size & 7) || h.op >= OP_COUNT_) die("corrupt recording at %p while queueing syncs (op %u size %u)", (const void *)p, h.op, h.size);
        if (h.op == OP_END) return;
        if (h.op == OP_JUMP) { memcpy(&p, p + 8, 8); continue; }
        if (h.op == OP_CALL) { uint64_t t; memcpy(&t, p + 8, 8); if (depth >= 16) die("queued sync scan: call depth over 16"); qo_mark_cb_syncs((const uint8_t *)(uintptr_t)t, depth + 1, Q, qseq); }
        else if (h.op == OP_FENCE_SYNC) { qo_psync_add(((const p_fence_sync *)(p + 8))->sync, Q, qseq); qo_ev_add(QE_CBSYNC, Q->q, (uint64_t)(uintptr_t)((const p_fence_sync *)(p + 8))->sync, qseq, 0); }
        p += h.size;
    }
}
/* nvnQueueSubmitCommands without the translation thread. */
static void qo_submit(const NVNqueue *q, int n, const uint64_t *h) {
    qo_item *it = calloc(1, sizeof *it); if (!it) die("guest queue item");
    it->h = malloc(sizeof *it->h * (size_t)(n > 0 ? n : 1)); if (!it->h) die("guest queue handles"); memcpy(it->h, h, sizeof *h * (size_t)n);
    it->n = n;
    /* NVN semantics (2026-11-08): a submission reaches the GPU at nvnQueueFlush (or a present / finish / wait that
       needs it). Ocarina of Time submits a frame's command buffers, then its worker threads fill the per-object and
       scene uniform blocks those commands read, then it flushes. Replaying (and taking the uniform snapshot) inside the
       submit call copied half-written blocks: NaN in the forward pass, white frames through bloom. The submission is
       queued here and replayed when its queue is flushed. */
    it->flushed = !qo_defer_to_flush(); it->sub_frame = g_frame_pub; it->sub_ns = now_ns(); if (it->flushed) it->flush_ns = it->sub_ns;
    qo_start_worker();
    pthread_mutex_lock(&g_qo_mu);
    qo_q *Q = qo_queue(q);
    it->Q = Q; it->seq = ++g_qo_seq; it->qseq = ++Q->seq_in; g_qo_items++;
    if (Q->head) { Q->tail->next = it; Q->tail = it; } else Q->head = Q->tail = it;
    if (atomic_load(&g_fence_sync_used)) for (int i = 0; i < n; i++) qo_mark_cb_syncs((const uint8_t *)h[i], 0, Q, it->qseq);
    qo_ev_add(QE_SUB, q, it->seq, n ? h[0] : 0, (uint64_t)n);
    if (it->flushed && qo_async()) qo_kick_locked();
    pthread_mutex_unlock(&g_qo_mu);
    if (it->flushed && !qo_async()) { replay_enter(); qo_pump(); replay_leave(); }   // OOT_REPLAY_AT_FLUSH=0 / OOT_QO_ASYNC=0: inside the call
}
/* nvnQueueFlush (and what implies it): everything submitted on q so far may run. g_qo_mu held. */
static void qo_flush_queue(const NVNqueue *q) {
    for (int i = 0; i < g_qo_nq; i++) if (g_qo_q[i].q == q) for (qo_item *it = g_qo_q[i].head; it; it = it->next) if (!it->flushed) { it->flushed = 1; it->flush_ns = now_ns(); }
}
/* nvnQueueFenceSync on a queue with pending work: the sync stands for that work, resolved once it is committed. */
static int qo_fence_sync(const NVNqueue *q, sync_t *s) {
    if (!qo_on()) return 0;
    pthread_mutex_lock(&g_qo_mu);
    qo_q *Q = NULL; for (int i = 0; i < g_qo_nq; i++) if (g_qo_q[i].q == q) Q = &g_qo_q[i];
    int pend = Q && Q->head;
    if (pend) {
        for (int i = 0; i < g_qo_npsync; i++) if (g_qo_psync[i].s == s) g_qo_psync[i--] = g_qo_psync[--g_qo_npsync];
        qo_psync_add(s, Q, Q->seq_in);
    }
    qo_ev_add(QE_FENCE, q, (uint64_t)(uintptr_t)s, (uint64_t)pend, 0);
    pthread_mutex_unlock(&g_qo_mu);
    return pend;
}
static int qo_sync_done(const void *s) { return !(((const sync_t *)s)->fenced & 8); }
/* A CPU wait (SyncWait) on a sync whose queue still has pending work. */
static int qo_sync_wait(const sync_t *s, uint64_t timeout) {
    uint64_t t0 = now_ns();
    if (!qo_async()) {
        replay_enter(); pthread_mutex_lock(&g_qo_mu); for (int i = 0; i < g_qo_npsync; i++) if (g_qo_psync[i].s == s) qo_flush_queue(g_qo_psync[i].Q->q); pthread_mutex_unlock(&g_qo_mu); replay_leave();
        for (;;) {
            replay_enter(); qo_pump(); pthread_mutex_lock(&g_qo_mu); int pend = (s->fenced & 8) != 0; pthread_mutex_unlock(&g_qo_mu); replay_leave();
            if (!pend) return 1;
            if (!timeout || (timeout != UINT64_MAX && now_ns() - t0 >= timeout)) return 0;
            usleep(200);
        }
    }
    pthread_mutex_lock(&g_qo_mu);
    qo_ev_add(QE_SYNCW_IN, NULL, (uint64_t)(uintptr_t)s, 0, 0);
    for (int i = 0; i < g_qo_npsync; i++) if (g_qo_psync[i].s == s) qo_flush_queue(g_qo_psync[i].Q->q);
    int ok = qo_wait_locked(qo_sync_done, s, timeout, t0);
    qo_ev_add(QE_SYNCW_OUT, NULL, (uint64_t)(uintptr_t)s, (uint64_t)ok, 0);
    pthread_mutex_unlock(&g_qo_mu);
    return ok;
}
static void qo_queue_wait_sync(const NVNqueue *q, const sync_t *s) {
    if (!qo_on() || !(s->fenced & 8)) return;
    pthread_mutex_lock(&g_qo_mu);
    if (s->fenced & 8) {
        qo_q *Q = qo_queue(q); qo_item *it = calloc(1, sizeof *it); if (!it) die("guest queue item");
        it->Q = Q; it->seq = ++g_qo_seq; it->qseq = ++Q->seq_in; it->wsync = s; it->parked_ns = now_ns(); it->flushed = 1;
        if (Q->head) { Q->tail->next = it; Q->tail = it; } else Q->head = Q->tail = it;
    }
    pthread_mutex_unlock(&g_qo_mu);
    qo_start_worker();
}
/* g_qo_mu held. A present / finish on q needs q's pending work committed, and with it the queues its GPU-side WaitSyncs
   wait on (the sync's own queue may not have been flushed yet: seen at loading, the present waited out the 400 ms valve). */
static void qo_flush_deps_locked(const NVNqueue *q) {
    for (int i = 0; i < g_qo_nq; i++) if (g_qo_q[i].q == q)
        for (qo_item *it = g_qo_q[i].head; it; it = it->next) if (it->wsync)
            for (int k = 0; k < g_qo_npsync; k++) if (g_qo_psync[k].s == it->wsync) qo_flush_queue(g_qo_psync[k].Q->q);
}
static int qo_queue_idle(const void *q) { qo_flush_deps_locked((const NVNqueue *)q); return !qo_queue_pending((const NVNqueue *)q); }
/* Before a present or a QueueFinish on q: its pending work must be committed first. */
static void qo_drain(const NVNqueue *q) {
    if (!qo_on()) return;
    uint64_t t0 = now_ns();
    if (!qo_async()) {
        replay_enter(); pthread_mutex_lock(&g_qo_mu); qo_flush_queue(q); pthread_mutex_unlock(&g_qo_mu); replay_leave();
        for (;;) {
            replay_enter(); qo_pump(); pthread_mutex_lock(&g_qo_mu); int pend = qo_queue_pending(q); pthread_mutex_unlock(&g_qo_mu); replay_leave();
            if (!pend) return;
            if (now_ns() - t0 > 2 * qo_timeout_ns()) { LOG("【红】[queue-order] queue %p still has parked work after %.0f ms at present / finish", (const void *)q, (now_ns() - t0) / 1e6); return; }
            usleep(200);
        }
    }
    pthread_mutex_lock(&g_qo_mu);
    qo_flush_queue(q);
    if (!qo_wait_locked(qo_queue_idle, q, 2 * qo_timeout_ns(), t0)) {
        const qo_item *h = NULL; for (int i = 0; i < g_qo_nq; i++) if (g_qo_q[i].q == q) h = g_qo_q[i].head;
        LOG("【红】[queue-order] queue %p still has parked work after %.0f ms at present / finish: head flushed=%d started=%d event=%p (mode %u, value %u; memory %u) wsync=%p",
            (const void *)q, (now_ns() - t0) / 1e6, h ? h->flushed : -1, h ? h->started : -1, h ? (void *)h->wval : NULL, h ? h->wmode : 0, h ? h->wtarget : 0, h && h->wval ? *h->wval : 0, h ? (const void *)h->wsync : NULL);
    }
    pthread_mutex_unlock(&g_qo_mu);
}
/* nvnQueuePresentTexture with OOT_QO_ASYNC: queued behind the frame's work on q (a present flushes q), shown by the
   queue-order thread; the game's thread goes on to its next frame. frame: the replay frame number (mtl_set_frame), as
   the translation thread's present did. Returns 0 when presents run on the calling thread. */
static int qo_present(const NVNqueue *q, win_t *o, int idx, double period, const int crop[4], uint64_t frame) {
    if (!qo_on() || !qo_async()) return 0;
    qo_item *it = calloc(1, sizeof *it); if (!it) die("guest queue present");
    it->present = 1; it->pwin = o; it->pidx = idx; it->pperiod = period; memcpy(it->pcrop, crop, sizeof it->pcrop); it->pframe = frame; it->flushed = 1; it->h = NULL;
    qo_start_worker();
    pthread_mutex_lock(&g_qo_mu);
    qo_q *Q = qo_queue(q); qo_flush_queue(q); qo_flush_deps_locked(q);
    it->Q = Q; it->seq = ++g_qo_seq; it->qseq = ++Q->seq_in;
    if (Q->head) { Q->tail->next = it; Q->tail = it; } else Q->head = Q->tail = it;
    if (idx >= 0 && idx < 4) g_qo_pres_pending[idx]++;
    qo_ev_add(QE_PRES_Q, q, (uint64_t)idx, it->seq, 0);
    qo_kick_locked();
    pthread_mutex_unlock(&g_qo_mu);
    return 1;
}
static int qo_image_free(const void *idx) { return g_qo_pres_pending[(intptr_t)idx] == 0; }
/* nvnWindowAcquireTexture: that image's last queued present has run (its ticket is set). */
static void qo_wait_present(int idx) {
    if (idx < 0 || idx >= 4 || !qo_on() || !qo_async()) return;
    uint64_t t0 = now_ns();
    pthread_mutex_lock(&g_qo_mu);
    qo_ev_add(QE_ACQ, NULL, (uint64_t)idx, 0, 0);
    if (!qo_wait_locked(qo_image_free, (const void *)(intptr_t)idx, 4 * qo_timeout_ns(), t0))
        LOG("【红】[queue-order] swapchain image %d: its last present still queued after %.0f ms at acquire", idx, (now_ns() - t0) / 1e6);
    qo_ev_add(QE_ACQ_OUT, NULL, (uint64_t)idx, 0, 0);
    pthread_mutex_unlock(&g_qo_mu);
}
static void qo_cpu_signal(volatile uint32_t *val) {
    if (!qo_on()) { mtl_qo_cpu_signal(val); return; }
    pthread_mutex_lock(&g_qo_mu);
    qo_ev_add(QE_CPUSIG, NULL, (uint64_t)(uintptr_t)val, *val, 0);
    mtl_qo_cpu_signal(val);
    int pend = qo_pending();
    if (pend && qo_async()) qo_kick_locked();
    pthread_mutex_unlock(&g_qo_mu);
    if (pend && !qo_async()) { replay_enter(); qo_pump(); replay_leave(); }
}
extern void (*runtime_abort_hook)(void) __attribute__((weak));
static void qo_init(void) { static int once; if (!once) { once = 1; g_qo_signal_releases = qo_signal_releases; if (&runtime_abort_hook) runtime_abort_hook = qo_ring_dump; } }
static void qo_queue_flush(const NVNqueue *q) {
    if (!qo_on()) return;
    pthread_mutex_lock(&g_qo_mu); qo_ev_add(QE_FLUSH, q, 0, 0, 0); qo_flush_queue(q); if (qo_async()) qo_kick_locked(); pthread_mutex_unlock(&g_qo_mu);
    if (!qo_async()) { replay_enter(); qo_pump(); replay_leave(); }
}
