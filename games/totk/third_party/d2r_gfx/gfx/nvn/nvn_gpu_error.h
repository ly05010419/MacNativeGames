/* CPU-side evidence for #2112. Only the replay thread records pipeline binds;
 * feedback owns an immutable copy, never the next submission's live state.
 * Labels identify candidates in a failed submission, not the guilty shader.
 * No GPU work, resources, waits or shader semantics are changed here. */
#define GPU_TRACE_PIPELINES 96
static NSString *g_gpu_trace_labels[GPU_TRACE_PIPELINES];
static uint64_t g_gpu_trace_binds;
static _Atomic uint64_t g_gpu_trace_sequence;

@interface NVNGPUErrorTrace : NSObject {
@public
    uint64_t sequence, frame, fence, binds;
    unsigned chunk;
    NSString *role;
    NSArray<NSString *> *pipelines;
}
@end
@implementation NVNGPUErrorTrace
@end

static void gpu_trace_pipeline(NSString *label) {
    g_gpu_trace_labels[g_gpu_trace_binds++ % GPU_TRACE_PIPELINES] = label ?: @"unlabelled helper pipeline";
}
static NVNGPUErrorTrace *gpu_trace_take(uint64_t frame, uint64_t fence, unsigned chunk, NSString *role) {
    NVNGPUErrorTrace *t = [NVNGPUErrorTrace new];
    t->sequence = atomic_fetch_add(&g_gpu_trace_sequence, 1) + 1;
    t->frame = frame; t->fence = fence; t->chunk = chunk; t->role = role; t->binds = g_gpu_trace_binds;
    NSMutableArray *labels = [NSMutableArray array];
    uint64_t start = t->binds > GPU_TRACE_PIPELINES ? t->binds - GPU_TRACE_PIPELINES : 0;
    for (uint64_t i = start; i < t->binds; i++) {
        unsigned slot = i % GPU_TRACE_PIPELINES;
        [labels addObject:g_gpu_trace_labels[slot]];
        g_gpu_trace_labels[slot] = nil;
    }
    t->pipelines = [labels copy]; g_gpu_trace_binds = 0;
    return t;
}
static int gpu_error_is_hang(NSError *error) {
    /* Metal 4 uses a different domain AND numbering. Code 2 means NotPermitted,
       not the old MTLCommandBufferErrorTimeout. Inspect nested errors too. */
    NSError *current = error;
    for (unsigned depth = 0; current && depth < 8; depth++) {
        if (([current.domain isEqualToString:MTL4CommandQueueErrorDomain] && current.code == MTL4CommandQueueErrorTimeout) ||
            ([current.domain isEqualToString:MTLCommandBufferErrorDomain] && current.code == MTLCommandBufferErrorTimeout)) return 1;
        NSString *d = current.description.lowercaseString;
        if ([d containsString:@"timeout"] || [d containsString:@"hang"] || [d containsString:@"innocentvictim"]) return 1;
        id underlying = current.userInfo[NSUnderlyingErrorKey];
        current = [underlying isKindOfClass:NSError.class] ? underlying : nil;
    }
    return 0;
}
static void gpu_error_write(NSString *text) {
    /* Separate, failure-only file: normal launcher stderr can be /dev/null.
       Preserve the previous run's report until a new failure occurs. Bound the
       number of reports at the callers; never persist shader source or inputs. */
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static int wrote;
    pthread_mutex_lock(&lock);
    MLOG("【红】%s", text.UTF8String);
    const char *path = getenv("OOT_GPU_ERROR_LOG");
    if (path && *path) {
        FILE *f = fopen(path, wrote ? "a" : "w");
        if (f) { fputs(text.UTF8String, f); fputc('\n', f); fflush(f); fclose(f); wrote = 1; }
        else MLOG("cannot save GPU error report: %s", strerror(errno));
    }
    fflush(stderr);
    pthread_mutex_unlock(&lock);
}
static void gpu_error_report(NSError *error, NVNGPUErrorTrace *t, double start, double end, NSString *context) {
    static unsigned reports; // Metal feedback queue is serial.
    if (!error || reports++ >= 8) return;
    NSMutableString *s = [NSMutableString stringWithFormat:
        @"[gpu-error] submit=%llu frame=%llu fence=%llu chunk=%u role=%@ domain=%@ code=%ld GPU=%.9f..%.9f\n%@\n%@\n"
         "pipeline_binds=%llu retained=%lu omitted=%llu (CPU encoded candidates; not proof of GPU execution/fault):",
        (unsigned long long)t->sequence, (unsigned long long)t->frame, (unsigned long long)t->fence, t->chunk, t->role,
        error.domain, (long)error.code, start, end, error.description, context ?: @"",
        (unsigned long long)t->binds, (unsigned long)t->pipelines.count, (unsigned long long)(t->binds - t->pipelines.count)];
    NSUInteger i = 0;
    for (NSString *label in t->pipelines) [s appendFormat:@"\n  bind[%llu] %@", (unsigned long long)(t->binds - t->pipelines.count + i++), label];
    gpu_error_write(s);
}
static void gpu_compile_error(NSError *error, NSString *label) {
    static _Atomic unsigned reports;
    if (error && atomic_fetch_add(&reports, 1) < 8)
        gpu_error_write([NSString stringWithFormat:@"[gpu-compile-error] pipeline=%@ domain=%@ code=%ld\n%@",
            label, error.domain, (long)error.code, error.description]);
}
