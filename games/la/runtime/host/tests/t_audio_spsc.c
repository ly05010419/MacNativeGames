// hle_audio.c lock-free queue stress test: three threads producing / playing / reclaiming 100 thousand blocks concurrently, the byte stream and reclaim order checked one by one by sequence number, no event lost.
#include "../hle_audio.c"
#include <sched.h>
#include <time.h>

enum { NBUF = 100000, MAX_SIZE = 37 };

uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec; }
static struct { const void *k; void *v; } om[8];
void *objmap_get(const void *k) { for (int i = 0; i < 8; i++) if (om[i].k == k) return om[i].v; return NULL; }
void objmap_put(const void *k, void *v) { for (int i = 0; i < 8; i++) if (!om[i].k) { om[i].k = k; om[i].v = v; return; } }
void *objmap_del(const void *k) { for (int i = 0; i < 8; i++) if (om[i].k == k) { om[i].k = 0; return om[i].v; } return NULL; }

static _Atomic int g_sig, g_after_last;
static _Atomic int g_last_pull;
void hle_signal_system_event(void *g) {
    (void)g;
    atomic_fetch_add_explicit(&g_sig, 1, memory_order_seq_cst);
    if (atomic_load_explicit(&g_last_pull, memory_order_seq_cst)) atomic_store(&g_after_last, 1);
}
void hle_bind_system_event(void *g, int autoclear) { (void)g; (void)autoclear; }
module_t *module_of(const void *p) { (void)p; return NULL; }   // call sites in the log: no guest modules in the unit test
void dm_host_helper_qos(qos_class_t q) { (void)q; }   // hle_thread.c: the helper threads keep the default class in the unit test

typedef struct {
    void *out;
    aout *a;
    gbuf *bufs;
    uint8_t (*data)[MAX_SIZE];
    uint8_t *scratch;
    uint32_t *sizes;
    uint64_t total_bytes;
    _Atomic int failed;
    _Atomic int released;
} test_ctx;

static uint32_t next_random(uint32_t *state) {
    *state = *state * 1664525u + 1013904223u;
    return *state;
}

static uint64_t available_bytes(aout *a) {
    uint32_t qh = atomic_load_explicit(&a->q_head, memory_order_relaxed);
    uint32_t qt = atomic_load_explicit(&a->q_tail, memory_order_acquire);
    if (qh == qt) return 0;
    uint64_t bytes = a->q[qh & (QMAX - 1)]->size - a->pos;
    for (uint32_t i = qh + 1; i != qt; i++) bytes += a->q[i & (QMAX - 1)]->size;
    return bytes;
}

static void *producer(void *p) {
    test_ctx *t = p;
    for (uint32_t i = 0; i < NBUF; i++) {
        t->bufs[i].ptr = t->data[i]; t->bufs[i].cap = MAX_SIZE; t->bufs[i].size = t->sizes[i];
        // Real buffer pool: buffers in flight (appended, not yet reclaimed) do not exceed QMAX, and more can be appended only after reclaiming
        while (i - (uint32_t)atomic_load_explicit(&t->released, memory_order_acquire) >= QMAX) sched_yield();
        while (!h_au_Append(t->out, &t->bufs[i])) sched_yield();
    }
    return NULL;
}

static void *consumer(void *p) {
    test_ctx *t = p;
    uint32_t state = 7;
    uint64_t want = 0, done = 0;
    while (done < t->total_bytes) {
        uint64_t avail = available_bytes(t->a);
        if (!avail) {
            if (atomic_load_explicit(&t->released, memory_order_acquire) == NBUF) break;
            sched_yield();
            continue;
        }
        uint64_t n = 1 + next_random(&state) % 512;
        if (n > avail) n = avail;
        if (done + n == t->total_bytes) atomic_store(&g_last_pull, 1);
        pull(t->a, t->scratch, n);
        for (uint64_t i = 0; i < n; i++) {
            if (t->scratch[i] != (uint8_t)(want++)) atomic_store(&t->failed, 1);
        }
        done += n;
        if (atomic_load(&t->released) == NBUF && !available_bytes(t->a)) break;
    }
    return NULL;
}

static void *release_reader(void *p) {
    test_ctx *t = p;
    for (int i = 0; i < NBUF; ) {
        gbuf *b = h_au_Released(t->out);
        if (!b) { sched_yield(); continue; }
        if (b != &t->bufs[i]) atomic_store(&t->failed, 1);
        i++;
        atomic_store_explicit(&t->released, i, memory_order_release);
    }
    return NULL;
}

int main(void) {
    char out[64], ev[64]; uint8_t prm[16];
    test_ctx t = {0};
    t.bufs = calloc(NBUF, sizeof *t.bufs); t.data = calloc(NBUF, sizeof *t.data); t.sizes = calloc(NBUF, sizeof *t.sizes); t.scratch = malloc(512);
    if (!t.bufs || !t.data || !t.sizes || !t.scratch) return 2;
    h_au_InitParam(prm);
    uint32_t state = 1; uint64_t cursor = 0;
    for (uint32_t i = 0; i < NBUF; i++) {
        t.sizes[i] = 1 + next_random(&state) % MAX_SIZE;
        for (uint32_t j = 0; j < t.sizes[i]; j++) t.data[i][j] = (uint8_t)(cursor++);
        t.total_bytes += t.sizes[i];
    }
    if (h_au_Open(out, ev, NULL, prm)) return 3;
    t.out = out; t.a = ao(out);
    atomic_store(&t.a->state, STATE_STARTED);
    pthread_t p, c, r;
    pthread_create(&p, NULL, producer, &t);
    pthread_create(&c, NULL, consumer, &t);
    pthread_create(&r, NULL, release_reader, &t);
    pthread_join(p, NULL); pthread_join(c, NULL); pthread_join(r, NULL);
    atomic_store(&t.a->state, STATE_STOPPED);
    h_au_Close(out);
    int sig = atomic_load(&g_sig), after = atomic_load(&g_after_last), released = atomic_load(&t.released), failed = atomic_load(&t.failed);
    printf("SPSC: appended/released %d/%d, events %d, signaled after last release=%d, failed=%d\n", NBUF, released, sig, after, failed);
    free(t.scratch); free(t.sizes); free(t.data); free(t.bufs);
    return released != NBUF || failed || sig < 1 || !after;
}
