#include "../nvn/nvn_mtl.m"
#include <errno.h>
#include <pthread.h>
#include <time.h>

void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t g_mod[MAX_MOD]; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }

static int g_checks, g_failed;
#define EXPECT_EQ(name, want, got) do { \
    long long w_ = (long long)(want), g_ = (long long)(got); g_checks++; \
    printf("%s expected %lld -> got %lld %s\n", name, w_, g_, w_ == g_ ? "OK" : "FAIL"); \
    g_failed += w_ != g_; \
} while (0)
#define EXPECT_TRUE(name, condition) EXPECT_EQ(name, 1, !!(condition))

@interface TestPipelineState : NSObject { @public int call; } @end
@implementation TestPipelineState @end

// The fake compiler blocks selected invocation numbers, so the test can hold
// real pso_pend_kick workers in the compiler without relying on driver timing.
@interface GateDevice : NSObject {
    pthread_mutex_t _mu;
    pthread_cond_t _cv;
    int _entered, _released, _block_through, _nil_call;
    int _sync_calls, _async_calls;
    pthread_t _submitter;
    NSMutableArray *_results;
    BOOL _submitter_call[1024];
    int _check_absent_call;
    pso_key _absent_key;
    BOOL _absent_key_observed[1024];
}
- (instancetype)initBlockingThrough:(int)call nilCall:(int)nilCall;
- (id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)descriptor error:(NSError **)error;
- (BOOL)waitForEntered:(int)count timeoutMs:(int)timeoutMs;
- (void)releaseThrough:(int)count;
- (void)releaseAll;
- (int)entered;
- (int)syncCalls;
- (int)asyncCalls;
- (BOOL)callWasOnSubmitter:(int)call;
- (void)expectAbsentPendingKey:(const pso_key *)key onSubmitterCall:(int)call;
- (BOOL)submitterCallObservedKeyAbsent:(int)call;
- (TestPipelineState *)resultForCall:(int)call;
@end

@implementation GateDevice
- (instancetype)initBlockingThrough:(int)call nilCall:(int)nilCall {
    if ((self = [super init])) {
        pthread_mutex_init(&_mu, NULL); pthread_cond_init(&_cv, NULL);
        _block_through = call; _nil_call = nilCall; _submitter = pthread_self();
        _results = [NSMutableArray array];
    }
    return self;
}
- (void)dealloc { pthread_cond_destroy(&_cv); pthread_mutex_destroy(&_mu); }
- (id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)descriptor error:(NSError **)error {
    (void)descriptor;
    pthread_mutex_lock(&_mu);
    int n = ++_entered;
    TestPipelineState *result = [TestPipelineState new]; result->call = n;
    [_results addObject:result];
    BOOL on_submitter = pthread_equal(pthread_self(), _submitter);
    _submitter_call[n] = on_submitter;
    BOOL check_absent = on_submitter && n == _check_absent_call;
    pso_key absent_key = _absent_key;
    if (on_submitter) _sync_calls++; else _async_calls++;
    pthread_cond_broadcast(&_cv);
    while (n <= _block_through && _released < n) pthread_cond_wait(&_cv, &_mu);
    BOOL fail = n == _nil_call;
    pthread_mutex_unlock(&_mu);
    if (check_absent) {
        pthread_mutex_lock(&g_pp_mu); BOOL absent = pso_pend_find(&absent_key) < 0; pthread_mutex_unlock(&g_pp_mu);
        pthread_mutex_lock(&_mu); _absent_key_observed[n] = absent; pthread_mutex_unlock(&_mu);
    }
    if (error) *error = nil;
    return fail ? nil : (id<MTLRenderPipelineState>)result;
}
- (BOOL)waitForEntered:(int)count timeoutMs:(int)timeoutMs {
    struct timespec end; clock_gettime(CLOCK_REALTIME, &end);
    end.tv_sec += timeoutMs / 1000; end.tv_nsec += (long)(timeoutMs % 1000) * 1000000L;
    if (end.tv_nsec >= 1000000000L) { end.tv_sec++; end.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&_mu);
    int rc = 0;
    while (_entered < count && rc != ETIMEDOUT) rc = pthread_cond_timedwait(&_cv, &_mu, &end);
    BOOL ok = _entered >= count;
    pthread_mutex_unlock(&_mu);
    return ok;
}
- (void)releaseThrough:(int)count {
    pthread_mutex_lock(&_mu); if (count > _released) _released = count; pthread_cond_broadcast(&_cv); pthread_mutex_unlock(&_mu);
}
- (void)releaseAll { [self releaseThrough:INT_MAX]; }
- (int)entered { pthread_mutex_lock(&_mu); int n = _entered; pthread_mutex_unlock(&_mu); return n; }
- (int)syncCalls { pthread_mutex_lock(&_mu); int n = _sync_calls; pthread_mutex_unlock(&_mu); return n; }
- (int)asyncCalls { pthread_mutex_lock(&_mu); int n = _async_calls; pthread_mutex_unlock(&_mu); return n; }
- (BOOL)callWasOnSubmitter:(int)call { pthread_mutex_lock(&_mu); BOOL b = call > 0 && call < 1024 && _submitter_call[call]; pthread_mutex_unlock(&_mu); return b; }
- (void)expectAbsentPendingKey:(const pso_key *)key onSubmitterCall:(int)call {
    pthread_mutex_lock(&_mu); if (key) _absent_key = *key; _check_absent_call = call; pthread_mutex_unlock(&_mu);
}
- (BOOL)submitterCallObservedKeyAbsent:(int)call {
    pthread_mutex_lock(&_mu); BOOL b = call > 0 && call < 1024 && _absent_key_observed[call]; pthread_mutex_unlock(&_mu); return b;
}
- (TestPipelineState *)resultForCall:(int)call {
    pthread_mutex_lock(&_mu); TestPipelineState *o = call > 0 && call <= (int)_results.count ? _results[(NSUInteger)call - 1] : nil; pthread_mutex_unlock(&_mu); return o;
}
@end

static struct timespec deadline_after_ms(int timeout_ms) {
    struct timespec end; clock_gettime(CLOCK_REALTIME, &end);
    end.tv_sec += timeout_ms / 1000; end.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (end.tv_nsec >= 1000000000L) { end.tv_sec++; end.tv_nsec -= 1000000000L; }
    return end;
}

static int pending_done(const pso_key *k, int timeout_ms) {
    struct timespec end = deadline_after_ms(timeout_ms);
    pthread_mutex_lock(&g_pp_mu);
    int at = pso_pend_find(k), rc = 0;
    while (at >= 0 && !g_pp[at].done && rc != ETIMEDOUT) {
        rc = pthread_cond_timedwait(&g_pp_cv, &g_pp_mu, &end);
        at = pso_pend_find(k);
    }
    int done = at >= 0 && g_pp[at].done;
    pthread_mutex_unlock(&g_pp_mu);
    return done;
}

static int queue_index(const pso_key *k) {
    pthread_mutex_lock(&g_pp_mu); int at = pso_pend_find(k); pthread_mutex_unlock(&g_pp_mu); return at;
}
static int queue_used_count(void) {
    int n = 0; pthread_mutex_lock(&g_pp_mu);
    for (int i = 0; i < PSO_PEND_MAX; i++) n += g_pp[i].used != 0;
    pthread_mutex_unlock(&g_pp_mu); return n;
}
static int queue_active_count(void) {
    int n = 0; pthread_mutex_lock(&g_pp_mu);
    for (int i = 0; i < PSO_PEND_MAX; i++) n += g_pp[i].used && !g_pp[i].done;
    pthread_mutex_unlock(&g_pp_mu); return n;
}
static int queue_done_at(int at) {
    pthread_mutex_lock(&g_pp_mu); int done = at >= 0 && at < PSO_PEND_MAX ? g_pp[at].done : -1; pthread_mutex_unlock(&g_pp_mu); return done;
}
static int drain_queue(int timeout_ms) {
    struct timespec end = deadline_after_ms(timeout_ms);
    pthread_mutex_lock(&g_pp_mu);
    int active = 1, rc = 0;
    while (active && rc != ETIMEDOUT) {
        active = 0;
        for (int i = 0; i < PSO_PEND_MAX; i++) active |= g_pp[i].used && !g_pp[i].done;
        if (active) rc = pthread_cond_timedwait(&g_pp_cv, &g_pp_mu, &end);
    }
    pthread_mutex_unlock(&g_pp_mu);
    return !active;
}
static void reset_queue(void) {
    pthread_mutex_lock(&g_pp_mu);
    for (int i = 0; i < PSO_PEND_MAX; i++) {
        g_pp[i].ps = nil; memset(&g_pp[i].k, 0, sizeof g_pp[i].k);
        g_pp[i].used = g_pp[i].done = 0; g_pp[i].order = 0;
    }
    g_pp_kicked = g_pp_taken = 0;
    pthread_mutex_unlock(&g_pp_mu);
}
static void release_cf_value(void *p) { if (p) CFRelease(p); }
static void reset_pso_cache(void) {
    nvn_hm_clear(&g_pso, release_cf_value);
    nvn_hm_clear(&g_pso_fail, release_cf_value);
}

static NvShader *test_vs, *test_fs;
static uint64_t g_key_vs[1024], g_key_fs[1024];
static pso_key make_key(unsigned tag) {
    pso_key k = {0};
    g_key_vs[tag] = tag * 3 + 1; g_key_fs[tag] = tag * 3 + 2;
    k.vs = &g_key_vs[tag]; k.fs = &g_key_fs[tag];
    k.cfmt[0] = MTLPixelFormatRGBA8Unorm; k.samples = 1; k.chmask = 0xffffffffu;
    return k;
}

typedef struct { pso_key k; NvShader *vs, *fs; pthread_mutex_t mu; pthread_cond_t cv; int done, result; } kick_job;
static void *kick_job_main(void *arg) {
    kick_job *j = arg; int result = pso_pend_kick(&j->k, j->vs, j->fs);
    pthread_mutex_lock(&j->mu); j->result = result; j->done = 1; pthread_cond_broadcast(&j->cv); pthread_mutex_unlock(&j->mu);
    return NULL;
}
static int wait_kick_job(kick_job *j, int timeout_ms) {
    struct timespec end = deadline_after_ms(timeout_ms);
    pthread_mutex_lock(&j->mu); int rc = 0;
    while (!j->done && rc != ETIMEDOUT) rc = pthread_cond_timedwait(&j->cv, &j->mu, &end);
    int done = j->done; pthread_mutex_unlock(&j->mu); return done;
}

static void test_two_active_exact_take_and_release(void) {
    reset_queue();
    GateDevice *dev = [[GateDevice alloc] initBlockingThrough:2 nilCall:0]; g_dev = (id<MTLDevice>)(id)dev;
    pso_key a = make_key(1), b = make_key(2), c = make_key(3), wrong_attachment = a;
    wrong_attachment.cfmt[0] = MTLPixelFormatBGRA8Unorm;
    EXPECT_EQ("first prediction admitted", 1, pso_pend_kick(&a, test_vs, test_fs));
    if (![dev waitForEntered:1 timeoutMs:5000]) { EXPECT_TRUE("first worker entered fake compiler", 0); [dev releaseAll]; drain_queue(5000); reset_queue(); return; }
    EXPECT_EQ("second prediction admitted", 1, pso_pend_kick(&b, test_vs, test_fs));
    if (![dev waitForEntered:2 timeoutMs:5000]) { EXPECT_TRUE("second worker entered fake compiler", 0); [dev releaseAll]; drain_queue(5000); reset_queue(); return; }
    EXPECT_EQ("two compilation slots are outstanding", 2, queue_active_count());
    kick_job job = {.k = c, .vs = test_vs, .fs = test_fs}; pthread_mutex_init(&job.mu, NULL); pthread_cond_init(&job.cv, NULL);
    pthread_t thread; int thread_error = pthread_create(&thread, NULL, kick_job_main, &job);
    EXPECT_EQ("start bounded third-kick check", 0, thread_error);
    if (thread_error) { [dev releaseAll]; drain_queue(5000); reset_queue(); pthread_cond_destroy(&job.cv); pthread_mutex_destroy(&job.mu); return; }
    int returned_without_wait = wait_kick_job(&job, 1000);
    EXPECT_TRUE("third kick returns while both compiler gates remain closed", returned_without_wait);
    if (!returned_without_wait) [dev releaseAll];
    pthread_join(thread, NULL);
    EXPECT_EQ("third prediction is rejected at active limit", 0, job.result);
    pthread_cond_destroy(&job.cv); pthread_mutex_destroy(&job.mu);
    if (!returned_without_wait) { drain_queue(5000); reset_queue(); return; }
    EXPECT_EQ("rejected third prediction never reaches compiler", 2, [dev entered]);
    EXPECT_EQ("rejected third prediction does not consume an admission order", 2, g_pp_kicked);
    EXPECT_TRUE("different attachment key does not match pending key", pso_pend_take(&wrong_attachment) == nil);
    EXPECT_TRUE("wrong-key take leaves original pending", queue_index(&a) >= 0);

    [dev releaseThrough:1];
    EXPECT_TRUE("first worker completes", pending_done(&a, 5000));
    TestPipelineState *expected_a = [dev resultForCall:1];
    id<MTLRenderPipelineState> taken_a = pso_pend_take(&a);
    EXPECT_TRUE("exact-key take returns first compilation", taken_a != nil && (__bridge void *)taken_a == (__bridge void *)expected_a);
    EXPECT_EQ("taking completed result releases its slot", -1, queue_index(&a));
    EXPECT_EQ("other worker remains active in its slot", 1, queue_active_count());

    [dev releaseThrough:2];
    EXPECT_TRUE("second worker completes", pending_done(&b, 5000));
    TestPipelineState *expected_b = [dev resultForCall:2];
    id<MTLRenderPipelineState> taken_b = pso_pend_take(&b);
    EXPECT_TRUE("second exact-key take returns its own result", taken_b != nil && (__bridge void *)taken_b == (__bridge void *)expected_b);
    EXPECT_EQ("both take operations leave no active workers", 0, queue_active_count());
    reset_queue();
}

static void test_full_table_reclaims_done_not_running(void) {
    reset_queue();
    GateDevice *dev = [[GateDevice alloc] initBlockingThrough:2 nilCall:0]; g_dev = (id<MTLDevice>)(id)dev;
    pso_key oldest = make_key(4), running = make_key(5);
    EXPECT_EQ("oldest slot admitted", 1, pso_pend_kick(&oldest, test_vs, test_fs));
    if (![dev waitForEntered:1 timeoutMs:5000]) { EXPECT_TRUE("oldest worker entered fake compiler", 0); [dev releaseAll]; drain_queue(5000); reset_queue(); return; }
    EXPECT_EQ("running slot admitted", 1, pso_pend_kick(&running, test_vs, test_fs));
    if (![dev waitForEntered:2 timeoutMs:5000]) { EXPECT_TRUE("running worker entered fake compiler", 0); [dev releaseAll]; drain_queue(5000); reset_queue(); return; }
    int oldest_slot = queue_index(&oldest), running_slot = queue_index(&running);
    [dev releaseThrough:1];
    EXPECT_TRUE("oldest prediction becomes completed but remains in table", pending_done(&oldest, 5000));

    int filled = 0;
    for (int i = 0; i < PSO_PEND_MAX - 2; i++) {
        pso_key k = make_key((unsigned)(10 + i));
        if (pso_pend_kick(&k, test_vs, test_fs) != 1 || !pending_done(&k, 5000)) break;
        filled++;
    }
    EXPECT_EQ("completed entries fill all remaining slots", PSO_PEND_MAX - 2, filled);
    EXPECT_EQ("table is full before overflow admission", PSO_PEND_MAX, queue_used_count());
    EXPECT_EQ("one existing worker remains active in full table", 1, queue_active_count());

    pso_key newest = make_key(500);
    EXPECT_EQ("full table admits by reclaiming a completed entry", 1, pso_pend_kick(&newest, test_vs, test_fs));
    EXPECT_TRUE("overflow compilation completes", pending_done(&newest, 5000));
    EXPECT_EQ("oldest completed key was reclaimed", -1, queue_index(&oldest));
    EXPECT_EQ("new key reused oldest completed slot", oldest_slot, queue_index(&newest));
    EXPECT_EQ("running key kept its original slot", running_slot, queue_index(&running));
    EXPECT_EQ("running slot is still not done", 0, queue_done_at(running_slot));
    EXPECT_EQ("table remains full after replacing done entry", PSO_PEND_MAX, queue_used_count());

    [dev releaseThrough:2];
    EXPECT_TRUE("running compilation eventually completes", pending_done(&running, 5000));
    id<MTLRenderPipelineState> got_running = pso_pend_take(&running);
    EXPECT_TRUE("running result is still retrievable", got_running != nil && (__bridge void *)got_running == (__bridge void *)[dev resultForCall:2]);
    id<MTLRenderPipelineState> got_newest = pso_pend_take(&newest);
    EXPECT_TRUE("replacement result is retrievable", got_newest != nil && (__bridge void *)got_newest == (__bridge void *)[dev resultForCall:PSO_PEND_MAX + 1]);
    EXPECT_TRUE("reclaimed key cannot be taken", pso_pend_take(&oldest) == nil);
    EXPECT_EQ("no workers remain after takes", 0, queue_active_count());
    reset_queue();
}

static void append_op(uint8_t *buf, size_t *used, unsigned op, const void *payload, size_t n) {
    size_t size = (8 + n + 7) & ~7ull;
    op_hdr h = {op, (uint32_t)size}; memcpy(buf + *used, &h, sizeof h);
    if (n) memcpy(buf + *used + 8, payload, n);
    *used += size;
}

static void test_nil_prediction_falls_back_then_predicts(void) {
    reset_queue(); reset_pso_cache(); state_reset();
    unsetenv("DM_NVN_HILITE"); unsetenv("DM_NVN_MAGSCAN"); unsetenv("DM_PSO_RECORD"); unsetenv("DM_PSO_USER"); unsetenv("DM_SAVE");
    GateDevice *dev = [[GateDevice alloc] initBlockingThrough:1 nilCall:1]; g_dev = (id<MTLDevice>)(id)dev;
    NvShader *vs1 = [NvShader new], *fs1 = [NvShader new], *vs2 = [NvShader new], *fs2 = [NvShader new];
    snprintf(vs1->key, sizeof vs1->key, "pending-vs1"); snprintf(fs1->key, sizeof fs1->key, "pending-fs1");
    snprintf(vs2->key, sizeof vs2->key, "pending-vs2"); snprintf(fs2->key, sizeof fs2->key, "pending-fs2");
    NvProg *demand = [NvProg new]; demand->sh[0] = vs1; demand->sh[1] = fs1;
    NvProg *predicted = [NvProg new]; predicted->sh[0] = vs2; predicted->sh[1] = fs2;
    pso_key demand_key; pso_key_fill(&demand_key, vs1, fs1, g_pass.cfmt, g_pass.dfmt, g_pass.sfmt, g_pass.samples, 0);
    EXPECT_EQ("speculative version admitted", 1, pso_pend_kick(&demand_key, vs1, fs1));
    if (![dev waitForEntered:1 timeoutMs:5000]) { EXPECT_TRUE("speculative worker entered fake compiler", 0); [dev releaseAll]; drain_queue(5000); reset_queue(); return; }
    [dev releaseThrough:1];
    EXPECT_TRUE("fake compiler can complete speculative job with nil", pending_done(&demand_key, 5000));
    EXPECT_TRUE("completed nil result remains available to demand lookup", queue_index(&demand_key) >= 0);

    // A repeated demanded draw must use the freshly populated cache, while a
    // second program supplies a real prediction without a Metal encoder.
    prog_t predicted_control = {.magic = MAGIC('P', 'R', 'O', 'G'), .nstage = 2, .serial = 0x7f010001};
    serial_put(predicted_control.serial, predicted);
    prog_t demand_control = {.magic = MAGIC('P', 'R', 'O', 'G'), .nstage = 2, .serial = 0x7f010002};
    serial_put(demand_control.serial, demand);
    uint8_t stream[256] = {0}; size_t used = 0;
    p_bind_program repeated = {&demand_control, 3}; append_op(stream, &used, OP_BIND_PROGRAM, &repeated, sizeof repeated);
    p_draw repeated_draw = {.prim = NVN_DRAW_PRIMITIVE_TRIANGLES, .first = 0, .count = 3, .instances = 1};
    append_op(stream, &used, OP_DRAW, &repeated_draw, sizeof repeated_draw);
    p_bind_program bind = {&predicted_control, 3}; append_op(stream, &used, OP_BIND_PROGRAM, &bind, sizeof bind);
    p_draw draw = {.prim = NVN_DRAW_PRIMITIVE_TRIANGLES, .first = 0, .count = 3, .instances = 1};
    append_op(stream, &used, OP_DRAW, &draw, sizeof draw); append_op(stream, &used, OP_END, NULL, 0);
    pso_key predicted_key; pso_key_fill(&predicted_key, vs2, fs2, g_pass.cfmt, g_pass.dfmt, g_pass.sfmt, g_pass.samples, 0);
    [dev expectAbsentPendingKey:&predicted_key onSubmitterCall:2];
    g_exec_next = stream; g_sub_serial = 1; g_pp_submit_mark = 0;

    id<MTLRenderPipelineState> got = pso_get_(demand, nil);
    EXPECT_TRUE("demand compile synchronously falls back after speculative nil", got != nil && (__bridge void *)got == (__bridge void *)[dev resultForCall:2]);
    EXPECT_TRUE("repeated demanded draw is not queued as duplicate speculation", queue_index(&demand_key) < 0);
    EXPECT_TRUE("demand compiler call ran on submitter thread", [dev callWasOnSubmitter:2]);
    EXPECT_TRUE("predicted key was not reserved when demand compile entered", [dev submitterCallObservedKeyAbsent:2]);
    EXPECT_TRUE("prediction compiler call ran after demand call", [dev waitForEntered:3 timeoutMs:5000] && ![dev callWasOnSubmitter:3]);
    EXPECT_EQ("fake compiler observed one synchronous and two async calls", 1, [dev syncCalls]);
    EXPECT_EQ("fake compiler observed speculative and predicted async calls", 2, [dev asyncCalls]);
    EXPECT_TRUE("lookahead prediction was admitted after demand", pending_done(&predicted_key, 5000));
    id<MTLRenderPipelineState> pred_result = pso_pend_take(&predicted_key);
    EXPECT_TRUE("predicted key remains distinct from demanded key", pred_result != nil && queue_index(&demand_key) < 0);

    [dev releaseAll]; EXPECT_TRUE("all fake compiler work has drained", drain_queue(5000));
    g_exec_next = NULL; reset_queue(); reset_pso_cache();
}

int main(void) {
    @autoreleasepool {
        setvbuf(stdout, NULL, _IONBF, 0);
        test_vs = [NvShader new]; test_fs = [NvShader new];
        snprintf(test_vs->key, sizeof test_vs->key, "pending-base-vs"); snprintf(test_fs->key, sizeof test_fs->key, "pending-base-fs");
        test_two_active_exact_take_and_release();
        test_full_table_reclaims_done_not_running();
        test_nil_prediction_falls_back_then_predicts();
        printf("== %d PSO pending checks, %d failed ==\n", g_checks, g_failed);
        return g_failed != 0;
    }
}
