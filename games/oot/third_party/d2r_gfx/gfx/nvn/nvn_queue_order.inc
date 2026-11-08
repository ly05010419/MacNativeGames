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
} qo_item;
struct qo_q { const NVNqueue *q; qo_item *head, *tail; uint64_t seq_in, seq_done; };
static qo_q g_qo_q[32]; static int g_qo_nq;
static uint64_t g_qo_seq, g_qo_parks, g_qo_resumes, g_qo_forced, g_qo_items, g_qo_cpu_resumes, g_qo_gpu_resumes;
static qo_item *g_qo_running;
typedef struct { sync_t *s; qo_q *Q; uint64_t qseq; } qo_psync;
static qo_psync g_qo_psync[256]; static int g_qo_npsync;
static pthread_cond_t g_qo_cv = PTHREAD_COND_INITIALIZER;
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
        if (p->Q->seq_done >= p->qseq) { p->s->fence = mtl_last_fence(); p->s->fenced = 1; g_qo_psync[i--] = g_qo_psync[--g_qo_npsync]; }
    }
    pthread_cond_broadcast(&g_qo_cv);
}
static int qo_ready(const qo_item *it) {
    if (!it->flushed) return 0;   // NVN: submitted commands reach the GPU at nvnQueueFlush
    if (it->wsync) return !(it->wsync->fenced & 8);
    if (!it->started || it->force || !it->wval) return 1;
    return mtl_qo_event_ready(it->wval, it->wmode, it->wtarget);
}
/* nvn_mtl.m, while replaying a SignalEvent: does it ready another queue's parked head? */
static int qo_signal_releases(volatile uint32_t *val) {
    for (int i = 0; i < g_qo_nq; i++) {
        const qo_item *h = g_qo_q[i].head;
        if (h && h != g_qo_running && h->started && h->wval == val && mtl_qo_event_ready(val, h->wmode, h->wtarget)) return 1;
    }
    return 0;
}
static void qo_finish_item(qo_item *it) {
    if (atomic_load(&g_fence_sync_used)) for (int i = 0; i < it->n; i++) fence_syncs((const uint8_t *)it->h[i], 0, mtl_last_fence(), 0);
    atomic_store(&g_sub_fence_prev, atomic_load(&g_sub_fence_last)); atomic_store(&g_sub_fence_last, mtl_last_fence());
    it->Q->seq_done = it->qseq;
    if (it->ucap) mtl_ucap_free(it->ucap);
    free(it->state); free(it->h); free(it);
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
    mtl_sub_snapshot_ubos_cap(h, n, it->ucap ? mtl_ucap_overlay(it->ucap, NULL) : NULL);   // uniforms as of the submit call (a copy: the snapshot owns it)
    mtl_sub_index(h + it->next_h, n - it->next_h);
    int stopped = 0; const uint8_t *cur[40]; int ncur = 0;
    if (it->started) {
        mtl_qo_state_restore(it->state);
        g_qo_resumes++;
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
/* The replay section is held. Runs ready queue heads, oldest first, until none can progress. */
static void qo_pump(void) {
    for (;;) {
        qo_item *best = NULL;
        for (int i = 0; i < g_qo_nq; i++) { qo_item *h = g_qo_q[i].head; if (h && qo_ready(h) && (!best || h->seq < best->seq)) best = h; }
        if (!best) {
            qo_item *oldest = NULL;   // safety: nothing can progress and a head has waited too long
            for (int i = 0; i < g_qo_nq; i++) { qo_item *h = g_qo_q[i].head; if (h && h->started && !h->wsync && (!oldest || h->parked_ns < oldest->parked_ns)) oldest = h; }
            if (!oldest || now_ns() - oldest->parked_ns < qo_timeout_ns()) break;
            g_qo_forced++;
            if (g_qo_forced <= 16) LOG("【红】[queue-order] queue %p waited %.0f ms on event %p (mode %u, value %u; memory %u) with nothing able to progress: resumed past the wait",
                (const void *)oldest->Q->q, (now_ns() - oldest->parked_ns) / 1e6, (void *)oldest->wval, oldest->wmode, oldest->wtarget, oldest->wval ? *oldest->wval : 0);
            oldest->force = 1; best = oldest;
        }
        if (best->wsync) { best->Q->head = best->next; if (!best->Q->head) best->Q->tail = NULL; best->Q->seq_done = best->qseq; free(best); qo_psync_resolve(); continue; }
        if (best->started && best->wval && !best->force) { if (mtl_qo_event_gpu_projected(best->wval)) g_qo_gpu_resumes++; else g_qo_cpu_resumes++; }
        if (qo_run(best)) continue;   // stopped again: its new wait (or the head it readied) decides next
        qo_q *Q = best->Q; Q->head = best->next; if (!Q->head) Q->tail = NULL;
        qo_finish_item(best); qo_psync_resolve();
    }
    static uint64_t last; uint64_t f = g_frame_pub;
    if (f >= last + 600) { last = f; LOG("[queue-order] F%llu items=%llu parked=%llu resumed=%llu (after a CPU signal %llu, a GPU signal %llu) forced=%llu", (unsigned long long)f, (unsigned long long)g_qo_items,
        (unsigned long long)g_qo_parks, (unsigned long long)g_qo_resumes, (unsigned long long)g_qo_cpu_resumes, (unsigned long long)g_qo_gpu_resumes, (unsigned long long)g_qo_forced); }
}
static void *qo_worker(void *unused) {
    (void)unused; pthread_setname_np("NVN queue order");
    for (;;) {
        usleep(1000);
        if (!qo_pending()) continue;   // racy read: a missed item is caught on the next tick or by the next submit
        replay_enter(); qo_pump(); replay_leave();
    }
    return NULL;
}
static void qo_start_worker(void) {
    static int started; if (started) return; started = 1;
    pthread_t t; pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 4u << 20);
    if (pthread_create(&t, &a, qo_worker, NULL)) die("guest queue order worker"); pthread_attr_destroy(&a); pthread_detach(t);
}
/* nvnQueueSubmitCommands without the translation thread, replay section held. */
static void qo_submit(const NVNqueue *q, int n, const uint64_t *h) {
    qo_q *Q = qo_queue(q);
    qo_pump();
    qo_item *it = calloc(1, sizeof *it); if (!it) die("guest queue item");
    it->Q = Q; it->seq = ++g_qo_seq; it->qseq = ++Q->seq_in; it->n = n; g_qo_items++;
    it->h = malloc(sizeof *it->h * (size_t)(n > 0 ? n : 1)); if (!it->h) die("guest queue handles"); memcpy(it->h, h, sizeof *h * (size_t)n);
    /* NVN semantics (2026-11-08): a submission reaches the GPU at nvnQueueFlush (or a present / finish / wait that
       needs it). Ocarina of Time submits a frame's command buffers, then its worker threads fill the per-object and
       scene uniform blocks those commands read, then it flushes. Replaying (and taking the uniform snapshot) inside the
       submit call copied half-written blocks: NaN in the forward pass, white frames through bloom. The submission is
       queued here and replayed when its queue is flushed. */
    it->flushed = !qo_defer_to_flush();
    if (Q->head) { Q->tail->next = it; Q->tail = it; } else Q->head = Q->tail = it;
    qo_start_worker();
    qo_pump();
}
/* nvnQueueFlush (and what implies it): everything submitted on q so far may run. */
static void qo_flush_queue(const NVNqueue *q) {
    for (int i = 0; i < g_qo_nq; i++) if (g_qo_q[i].q == q) for (qo_item *it = g_qo_q[i].head; it; it = it->next) it->flushed = 1;
}
/* nvnQueueFenceSync on a queue with parked work: the sync stands for that work, resolved once it is committed. */
static int qo_fence_sync(const NVNqueue *q, sync_t *s) {
    if (!qo_on()) return 0;
    replay_enter();
    qo_q *Q = NULL; for (int i = 0; i < g_qo_nq; i++) if (g_qo_q[i].q == q) Q = &g_qo_q[i];
    int pend = Q && Q->head;
    if (pend) {
        if (g_qo_npsync >= 256) { qo_pump(); }
        if (g_qo_npsync >= 256) die("guest queue syncs");
        for (int i = 0; i < g_qo_npsync; i++) if (g_qo_psync[i].s == s) g_qo_psync[i--] = g_qo_psync[--g_qo_npsync];
        g_qo_psync[g_qo_npsync++] = (qo_psync){s, Q, Q->seq_in}; s->fenced = 9; s->fence = 0;
    }
    replay_leave();
    return pend;
}
/* A CPU wait (SyncWait) or a GPU-side WaitSync on a sync whose queue still has parked work. */
static int qo_sync_wait(const sync_t *s, uint64_t timeout) {
    uint64_t t0 = now_ns();
    replay_enter(); for (int i = 0; i < g_qo_npsync; i++) if (g_qo_psync[i].s == s) qo_flush_queue(g_qo_psync[i].Q->q); replay_leave();
    for (;;) {
        replay_enter(); qo_pump(); int pend = (s->fenced & 8) != 0; replay_leave();
        if (!pend) return 1;
        if (!timeout || (timeout != UINT64_MAX && now_ns() - t0 >= timeout)) return 0;
        usleep(200);
    }
}
static void qo_queue_wait_sync(const NVNqueue *q, const sync_t *s) {
    if (!qo_on() || !(s->fenced & 8)) return;
    replay_enter();
    if (s->fenced & 8) {
        qo_q *Q = qo_queue(q); qo_item *it = calloc(1, sizeof *it); if (!it) die("guest queue item");
        it->Q = Q; it->seq = ++g_qo_seq; it->qseq = ++Q->seq_in; it->wsync = s; it->parked_ns = now_ns(); it->flushed = 1;
        if (Q->head) { Q->tail->next = it; Q->tail = it; } else Q->head = Q->tail = it;
        qo_start_worker();
    }
    replay_leave();
}
/* Before a present or a QueueFinish on q: its parked work must be committed first. */
static void qo_drain(const NVNqueue *q) {
    if (!qo_on()) return;
    replay_enter(); qo_flush_queue(q); replay_leave();
    uint64_t t0 = now_ns();
    for (;;) {
        replay_enter(); qo_pump(); int pend = qo_queue_pending(q); replay_leave();
        if (!pend) return;
        if (now_ns() - t0 > 2 * qo_timeout_ns()) { LOG("【红】[queue-order] queue %p still has parked work after %.0f ms at present / finish", (const void *)q, (now_ns() - t0) / 1e6); return; }
        usleep(200);
    }
}
static void qo_cpu_signal(volatile uint32_t *val) {
    mtl_qo_cpu_signal(val);
    if (!qo_on() || !qo_pending()) return;
    replay_enter(); qo_pump(); replay_leave();
}
static void qo_init(void) { static int once; if (!once) { once = 1; g_qo_signal_releases = qo_signal_releases; } }
static void qo_queue_flush(const NVNqueue *q) {
    if (!qo_on()) return;
    replay_enter(); qo_flush_queue(q); qo_pump(); replay_leave();
}
