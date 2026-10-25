/* Native checks for the six registered BotW 1.6.0 JobQueue cuts. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>
#include <time.h>
#include <sys/sysctl.h>
#include <limits.h>
#include <libkern/OSCacheControl.h>
#include <pthread/qos.h>
#include <sys/mman.h>

_Noreturn void runtime_fail(const char *what) { fprintf(stderr, "FAIL: %s\n", what); exit(1); }
static void need(int ok, const char *what) { if (!ok) runtime_fail(what); }
static uint64_t ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
static struct { uint32_t pc, length, original[5], patched[5]; } cuts[] = {
    {.pc = 0x12c0e58, .length = 4}, {.pc = 0x12c0ebc, .length = 5},
    {.pc = 0x12c0f04, .length = 4}, {.pc = 0x12c0f60, .length = 5},
    {.pc = 0x12c0f8c, .length = 4}, {.pc = 0x12c0fb8, .length = 5},
};
static void load_cuts(const char *image) {
    char path[PATH_MAX], line[512], before[41], after[41];
    const char *slash = strrchr(image, '/');
    need(strlen(image) < sizeof path, "image path length");
    need(snprintf(path, sizeof path, "%.*smain.cuts.tsv", (int)(slash ? slash - image + 1 : 0), image) < (int)sizeof path,
         "cut manifest path length");
    FILE *file = fopen(path, "r"); need(file != NULL, "open registered cuts");
    unsigned seen = 0;
    while (fgets(line, sizeof line, file)) {
        unsigned pc;
        if (sscanf(line, "%x\t%40[0-9a-f]\t%40[0-9a-f]", &pc, before, after) != 3) continue;
        for (unsigned i = 0; i < sizeof cuts / sizeof *cuts; i++) if (pc == cuts[i].pc) {
            need(!(seen & (1u << i)), "no duplicate cuts");
            need(strlen(before) == cuts[i].length * 8 && strlen(after) == strlen(before), "equal-length cuts");
            for (unsigned j = 0; j < cuts[i].length * 4; j++) {
                need(sscanf(before + 2 * j, "%2hhx", (unsigned char *)cuts[i].original + j) == 1 &&
                     sscanf(after + 2 * j, "%2hhx", (unsigned char *)cuts[i].patched + j) == 1, "decode registered instructions");
            }
            seen |= 1u << i;
        }
    }
    need(!ferror(file) && seen == (1u << (sizeof cuts / sizeof *cuts)) - 1, "read all six cuts");
    need(!fclose(file), "close registered cuts");
}
static void semantic_cases(unsigned char *code) {
    for (unsigned i = 0; i < sizeof cuts / sizeof *cuts; i++) {
        unsigned length = cuts[i].length, release = length == 5;
        unsigned old = cuts[i].original[0] & 31;
        unsigned value = cuts[i].original[release ? 3 : 2] & 31;
        need(((cuts[i].original[0] >> 5) & 31) == 8 && old >= 9 && old <= 15 &&
             (value == 31 || (value >= 9 && value <= 15)), "caller-saved JobQueue register layout");
        uint32_t cas = cuts[i].patched[1];
        need((cas & 0xffe0fc00u) == 0x88e07c00u && ((cas >> 16) & 31) == old &&
             ((cas >> 5) & 31) == 8 && (cas & 31) == value, "CASA width, ordering and operands");
        for (int converted = 0; converted < 2; converted++) for (unsigned flags = 0; flags < 16; flags++) {
            uint64_t high = 0xfeedface00000000ull;
            _Alignas(128) uint64_t cells[64] = {0}; cells[0] = high | (release ? 1u : 0u);
            uint64_t status[2] = {UINT64_MAX, UINT64_MAX};
            uint32_t stub[16] = {0xd51b4201u, 0xaa0003e8u, 0xd503201fu, 0xc85f7c71u};
            if (value != 31) stub[2] = 0xaa0403e0u | value;
            memcpy(stub + 4, converted ? cuts[i].patched : cuts[i].original, length * 4);
            stub[length + 4] = 0xf9000040u | old;
            stub[length + 5] = 0xc8107c64u;   /* STXR w16, x4, [x3]: an older reservation must be cleared. */
            stub[length + 6] = 0xf9000450u;
            stub[length + 7] = 0xd53b4200u;
            stub[length + 8] = 0xd65f03c0u;
            pthread_jit_write_protect_np(0); memcpy(code, stub, sizeof stub); sys_icache_invalidate(code, sizeof stub); pthread_jit_write_protect_np(1);
            uint64_t (*run)(void *, uint64_t, uint64_t *, void *, uint64_t) = (void *)code;
            uint64_t actual_flags = run(cells, (uint64_t)flags << 28, status, cells + 32, 1);
            need(cells[0] == (high | (release ? 0 : 1)) && status[0] == 0, "memory width, value and successful STXR status preserved");
            need(status[1] == 1 && cells[32] == 0, "successful loop clears an older exclusive reservation");
            need(actual_flags == (release ? 0x60000000u : (uint64_t)flags << 28), "NZCV preserved");
        }
    }
    puts("PASS: six registered cuts: CASA operands, successful status, all NZCV inputs and monitor clearing");
}

enum { JOBS = 29 };
static unsigned (*run_queue)(void *, unsigned, unsigned *, void *, unsigned);
static _Alignas(128) unsigned char queue_storage[0x400], contexts[3][0x200];
static void *queue = queue_storage + 0x10;
static _Thread_local unsigned thread_index;
static _Atomic unsigned seen[JOBS], finished;
static unsigned count, claim_count, with_context, restrict_first, rounds;
static pthread_mutex_t barrier_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t barrier_cv = PTHREAD_COND_INITIALIZER;
static unsigned arrivals, generation;
static void barrier(void) {
    pthread_mutex_lock(&barrier_mu);
    unsigned g = generation;
    if (++arrivals == 3) { arrivals = 0; generation++; pthread_cond_broadcast(&barrier_cv); }
    else while (g == generation) pthread_cond_wait(&barrier_cv, &barrier_mu);
    pthread_mutex_unlock(&barrier_mu);
}
typedef struct { void **vtable; unsigned index; } Job;
static void job_run(Job *job) {
    need(job->index < count && atomic_fetch_add(&seen[job->index], 1) == 0, "each job exactly once");
    if (!job->index && restrict_first) need(thread_index == 0, "job zero belongs to thread zero");
}
static void drain(void) {
    unsigned got;
    while (!run_queue(queue, claim_count, &got, with_context ? contexts[thread_index] : NULL, thread_index)) {}
}
static void *worker(void *arg) {
    thread_index = (unsigned)(uintptr_t)arg;
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    for (unsigned i = 0; i < rounds; i++) { barrier(); drain(); barrier(); }
    return NULL;
}
static int cmp(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}
static void reset_queue(void) {
    *(uint32_t *)((char *)queue + 0x90) = count;
    *(uint32_t *)((char *)queue + 0x94) = 0;
    *((unsigned char *)queue + 0x98) = (unsigned char)restrict_first;
    for (unsigned j = 0; j < JOBS; j++) atomic_store(&seen[j], 0);
}
static void concurrent_case(void) {
    pthread_t threads[2];
    uint64_t *waits = calloc(rounds, sizeof *waits); need(waits != NULL, "timing allocation");
    for (unsigned i = 0; i < 2; i++) need(!pthread_create(&threads[i], NULL, worker, (void *)(uintptr_t)(i + 1)), "create worker");
    for (unsigned i = 0; i < rounds; i++) {
        reset_queue(); barrier();
        uint64_t t = ns(); drain(); waits[i] = ns() - t;
        barrier();
        need(*(uint32_t *)((char *)queue + 0x94) == count, "all indices claimed");
        for (unsigned j = 0; j < count; j++) need(atomic_load(&seen[j]) == 1, "no missing jobs");
    }
    for (unsigned i = 0; i < 2; i++) need(!pthread_join(threads[i], NULL), "join worker");
    qsort(waits, rounds, sizeof *waits, cmp);
    printf("context=%u first-only=%u jobs=%u claim=%u rounds=%u p99=%.3fms max=%.3fms\n", with_context, restrict_first, count,
           claim_count, rounds, waits[rounds * 99 / 100] / 1e6, waits[rounds - 1] / 1e6);
    need(waits[rounds * 99 / 100] < 33333333, "p99 below one 30-fps frame");
    free(waits);
}
static void *single_worker(void *arg) { (void)arg; thread_index = 1; drain(); atomic_store(&finished, 1); return NULL; }
static void clear_flag_case(void) {
    count = claim_count = 1; restrict_first = 0; reset_queue();
    *((unsigned char *)queue + 0x98) = 1; atomic_store(&finished, 0);
    pthread_t thread; need(!pthread_create(&thread, NULL, single_worker, NULL), "create restricted worker");
    struct timespec pause = {0, 10000000}; nanosleep(&pause, NULL);
    need(!atomic_load(&finished) && !atomic_load(&seen[0]), "worker obeys the first-job restriction");
    __atomic_store_n((unsigned char *)queue + 0x98, 0, __ATOMIC_RELEASE);
    uint64_t deadline = ns() + 1000000000ull;
    while (!atomic_load(&finished) && ns() < deadline) { struct timespec tick = {0, 1000000}; nanosleep(&tick, NULL); }
    need(atomic_load(&finished), "clearing the restriction releases the worker");
    need(!pthread_join(thread, NULL) && atomic_load(&seen[0]) == 1, "cleared restriction executes once");
}
static void queue_cases(unsigned char *code, const char *path) {
    enum { START = 0x12c0dc0, END = 0x12c1080 };
    uint8_t original[END - START];
    FILE *f = fopen(path, "rb"); need(f != NULL, "open rewritten main");
    need(!fseek(f, START, SEEK_SET) && fread(original, 1, sizeof original, f) == sizeof original, "read real JobQueue");
    pthread_jit_write_protect_np(0); memcpy(code, original, sizeof original);
    for (unsigned i = 0; i < sizeof cuts / sizeof *cuts; i++)
        need(!memcmp(original + cuts[i].pc - START, cuts[i].patched, cuts[i].length * 4), "actual JobQueue contains each registered cut");
    fclose(f);
    const struct { uint32_t off, word; } tls[] = {
        {0x12c0de0, 0xd503201f}, {0x12c0de8, 0x2a0403e0}, /* synthetic index from fifth argument, not guest TLS */
        {0x12c0df8, 0xd503201f}, {0x12c0dfc, 0x14000004}
    };
    for (size_t i = 0; i < sizeof tls / sizeof *tls; i++) memcpy(code + tls[i].off - START, &tls[i].word, 4);
    sys_icache_invalidate(code, sizeof original); pthread_jit_write_protect_np(1);
    run_queue = (void *)code;
    void *vtable[3] = {NULL, NULL, (void *)job_run};
    Job jobs[JOBS]; void *pointers[JOBS];
    for (unsigned i = 0; i < JOBS; i++) { jobs[i] = (Job){vtable, i}; pointers[i] = &jobs[i]; }
    *(uint32_t *)((char *)queue + 0x80) = JOBS;
    *(void **)((char *)queue + 0x88) = pointers;
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    count = JOBS; claim_count = restrict_first = with_context = 1; rounds = 3000;
    concurrent_case();
    rounds = 300;
    for (with_context = 0; with_context < 2; with_context++) {
        restrict_first = 1; claim_count = JOBS; count = JOBS; concurrent_case();
        restrict_first = 0; claim_count = 1; concurrent_case();
        count = 0; concurrent_case();
        clear_flag_case();
    }
    puts("PASS: real three-thread JobQueue, unchanged first-job rules, both context paths, unrestricted/empty queues and restriction removal");
}
int main(int argc, char **argv) {
    need(argc == 2, "usage: probe botw160/main.patched.bin");
    int supported = 0; size_t supported_size = sizeof supported;
    need(!sysctlbyname("hw.optional.arm.FEAT_LSE", &supported, &supported_size, NULL, 0) && supported,
         "native LSE atomics required by this probe");
    unsigned char *code = mmap(NULL, 16384, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    need(code != MAP_FAILED, "map native code");
    load_cuts(argv[1]);
    semantic_cases(code);
    queue_cases(code, argv[1]);
    need(!munmap(code, 16384), "unmap code");
}
