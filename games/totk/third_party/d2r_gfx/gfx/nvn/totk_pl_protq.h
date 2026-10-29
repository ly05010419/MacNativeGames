/* Included by totk_pipeline.h (and tests/pipeline_hold/probe.c). TOTK_PIPELINE: write protection of spans the replay
   decided on (static spans, probation runs, non-static in-place ranges) runs on a helper thread, not on the translate
   thread (the mprotect calls of new data made the translations of streaming frames slow). A release first makes sure
   every span queued up to its submission is done (pl_protq_sync).
   One producer (the translate thread), consumers: the helper and a release. Two buffers: the producer fills one, the
   helper swaps it for its own (capacities travel with their buffers) and protects outside the lock; a release takes what
   is still queued by copy. Needs: typedef pl_rng { uintptr_t lo, hi; }, PL_PROTQ_RUN(lo, hi), PL_PROTQ_DIE(msg).
   The producer collects its spans in a local batch first (a span touching the previous one joins it) and hands the batch
   over when it is full or when pl_protq_seq is read (the commit): one lock and at most one wake-up per batch. Pushing
   every span under the lock woke the helper for nearly each of ~550 spans a submission (a condition broadcast, a system
   call, each time the helper had emptied the queue): kernel time and core wake-ups on the translate thread. */
static pthread_mutex_t g_plpq_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_plpq_cv = PTHREAD_COND_INITIALIZER;
static pl_rng *g_plpq; static int g_plpq_n, g_plpq_cap; static uint64_t g_plpq_seq, g_plpq_done, g_plpq_busy_to; static int g_plpq_busy, g_plpq_started;
static _Atomic uint64_t g_plpq_inline, g_plpq_spans;
static void pl_protq_run(const pl_rng *v, int n) { for (int i = 0; i < n; i++) PL_PROTQ_RUN(v[i].lo, v[i].hi); atomic_fetch_add(&g_plpq_spans, (uint64_t)n); }
static void *pl_protq_main(void *arg) {
    (void)arg; pthread_setname_np("totk-pl-protect"); pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
    pl_rng *own = NULL; int own_cap = 0;
    for (;;) {
        pthread_mutex_lock(&g_plpq_mu);
        while (!g_plpq_n) pthread_cond_wait(&g_plpq_cv, &g_plpq_mu);
        pl_rng *v = g_plpq; int n = g_plpq_n, v_cap = g_plpq_cap;
        g_plpq = own; g_plpq_cap = own_cap; g_plpq_n = 0;   // the producer goes on in the other buffer
        own = v; own_cap = v_cap;
        uint64_t to = g_plpq_seq; g_plpq_busy = 1; g_plpq_busy_to = to;
        pthread_mutex_unlock(&g_plpq_mu);
        pl_protq_run(v, n);
        pthread_mutex_lock(&g_plpq_mu); g_plpq_busy = 0; if (to > g_plpq_done) g_plpq_done = to; pthread_cond_broadcast(&g_plpq_cv); pthread_mutex_unlock(&g_plpq_mu);
    }
    return NULL;
}
enum { PL_PROTQ_LOCAL = 512 };
static pl_rng g_plpq_loc[PL_PROTQ_LOCAL]; static int g_plpq_ln;   // the producer's batch (not yet visible to the helper or a release)
static _Atomic uint64_t g_plpq_pushed, g_plpq_batches;   // spans pushed (before joining), batches handed over
static void pl_protq_flush(void) {   // the producer thread only: its batch goes to the queue
    if (!g_plpq_ln) return;
    if (!g_plpq_started) { g_plpq_started = 1; pthread_t t; if (pthread_create(&t, NULL, pl_protq_main, NULL)) PL_PROTQ_DIE("pipeline protection thread"); pthread_detach(t); }
    pthread_mutex_lock(&g_plpq_mu);
    if (g_plpq_n + g_plpq_ln > g_plpq_cap) {
        int cap = g_plpq_cap ? g_plpq_cap : 1024; while (cap < g_plpq_n + g_plpq_ln) cap *= 2;
        pl_rng *b = realloc(g_plpq, sizeof *b * (size_t)cap);
        if (!b) PL_PROTQ_DIE("pipeline protection queue");
        g_plpq = b; g_plpq_cap = cap;
    }
    int was = g_plpq_n;
    memcpy(g_plpq + g_plpq_n, g_plpq_loc, sizeof *g_plpq * (size_t)g_plpq_ln); g_plpq_n += g_plpq_ln; g_plpq_seq += (uint64_t)g_plpq_ln;
    if (!was) pthread_cond_broadcast(&g_plpq_cv);
    pthread_mutex_unlock(&g_plpq_mu);
    g_plpq_ln = 0; atomic_fetch_add_explicit(&g_plpq_batches, 1, memory_order_relaxed);
}
static void pl_protq_push(uintptr_t lo, uintptr_t hi) {   // the producer thread only
    atomic_fetch_add_explicit(&g_plpq_pushed, 1, memory_order_relaxed);
    if (g_plpq_ln) { pl_rng *l = &g_plpq_loc[g_plpq_ln - 1]; if (lo <= l->hi && hi >= l->lo) { if (lo < l->lo) l->lo = lo; if (hi > l->hi) l->hi = hi; return; } }   // touches the previous span: one span
    if (g_plpq_ln == PL_PROTQ_LOCAL) pl_protq_flush();
    g_plpq_loc[g_plpq_ln++] = (pl_rng){lo, hi};
}
static void pl_protq_cb(uintptr_t lo, uintptr_t hi) { pl_protq_push(lo, hi); }
/* the producer thread: hands its batch over; the sequence number covering everything it pushed so far */
static uint64_t pl_protq_seq(void) { pl_protq_flush(); pthread_mutex_lock(&g_plpq_mu); uint64_t q = g_plpq_seq; pthread_mutex_unlock(&g_plpq_mu); return q; }
/* any thread: the spans queued up to seq (a value of pl_protq_seq) are protected: what is still queued is done here, a batch the helper works on is waited for */
static void pl_protq_sync(uint64_t seq) {
    pthread_mutex_lock(&g_plpq_mu);
    while (g_plpq_done < seq) {
        if (g_plpq_n) {
            int n = g_plpq_n; uint64_t to = g_plpq_seq; pl_rng *v = malloc(sizeof *v * (size_t)n);
            if (!v) PL_PROTQ_DIE("pipeline protection queue");
            memcpy(v, g_plpq, sizeof *v * (size_t)n); g_plpq_n = 0;
            pthread_mutex_unlock(&g_plpq_mu);
            pl_protq_run(v, n); free(v); atomic_fetch_add(&g_plpq_inline, (uint64_t)n);
            pthread_mutex_lock(&g_plpq_mu);
            while (g_plpq_busy && g_plpq_busy_to < to) pthread_cond_wait(&g_plpq_cv, &g_plpq_mu);   // an older batch still in the helper
            if (to > g_plpq_done) g_plpq_done = to;
            pthread_cond_broadcast(&g_plpq_cv);
        } else pthread_cond_wait(&g_plpq_cv, &g_plpq_mu);   // the helper is protecting the last batch
    }
    pthread_mutex_unlock(&g_plpq_mu);
}
