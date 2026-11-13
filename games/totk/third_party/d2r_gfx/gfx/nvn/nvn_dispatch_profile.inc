/* Opt-in precise Metal 4 dispatch timestamps. This serializes diagnostic work;
 * these frames must never be used for FPS comparisons. Indirect grids are copied
 * on the GPU immediately before their consumer, not read from stale CPU memory. */
typedef struct { uint64_t frame, indirect; int encoder; uint16_t tg[3]; char key[128]; } dp_time;
#define DP_TIME_CAP 512
static id<MTL4CounterHeap> g_dp_heap;
static id<MTLBuffer> g_dp_grids;
static dp_time g_dp_times[DP_TIME_CAP];
static int g_dp_count;
static int dp_time_begin(id<MTL4ComputeCommandEncoder> ce, const char *key, const p_dispatch *dp,
                         const uint16_t *tg, id<MTLBuffer> indirect, uint64_t offset) {
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("OOT_DISPATCH_GPU_FILE") != NULL;
    if (!enabled || !g_pt_heap || g_dp_count == DP_TIME_CAP) return -1;
    if (!g_dp_heap) {
        MTL4CounterHeapDescriptor *d = [MTL4CounterHeapDescriptor new]; d.type = MTL4CounterHeapTypeTimestamp; d.count = DP_TIME_CAP * 2;
        NSError *error = nil; g_dp_heap = [g_dev newCounterHeapWithDescriptor:d error:&error];
        g_dp_grids = [g_dev newBufferWithLength:DP_TIME_CAP * 16 options:MTLResourceStorageModeShared];
        if (!g_dp_heap || !g_dp_grids) MDIE("dispatch profiler allocation: %s", error.localizedDescription.UTF8String);
        res_add(g_dp_grids); defer_obj_release(g_dp_grids);
    }
    int index = g_dp_count++; dp_time *r = &g_dp_times[index];
    *r = (dp_time){.frame=atomic_load(&g_cur_frame), .indirect=dp->indirect, .encoder=g_pt_n-1};
    memcpy(r->tg, tg, sizeof r->tg); snprintf(r->key, sizeof r->key, "%s", key);
    uint32_t *grid = (uint32_t *)g_dp_grids.contents + index * 4;
    grid[0] = dp->x; grid[1] = dp->y; grid[2] = dp->z; grid[3] = 0x47524944;
    if (indirect) {
        [ce copyFromBuffer:indirect sourceOffset:offset toBuffer:g_dp_grids destinationOffset:index * 16 size:12];
        [ce barrierAfterEncoderStages:MTLStageBlit beforeEncoderStages:MTLStageDispatch visibilityOptions:MTL4VisibilityOptionDevice];
    }
    [ce writeTimestampWithGranularity:MTL4TimestampGranularityPrecise intoHeap:g_dp_heap atIndex:index * 2];
    return index;
}
static void dp_time_end(id<MTL4ComputeCommandEncoder> ce, int index) {
    if (index >= 0) [ce writeTimestampWithGranularity:MTL4TimestampGranularityPrecise intoHeap:g_dp_heap atIndex:index * 2 + 1];
}
static void dp_time_finish(TotkCommitOptions *options, uint64_t fence) {
    if (!g_dp_count) return;
    int count = g_dp_count; dp_time *records = malloc(count * sizeof *records); memcpy(records, g_dp_times, count * sizeof *records);
    id<MTL4CounterHeap> heap = g_dp_heap; id<MTLBuffer> grids = g_dp_grids; double hz = (double)[g_dev queryTimestampFrequency];
    g_dp_count = 0; g_dp_heap = nil; g_dp_grids = nil;
    [options addFeedbackHandler:^(id<MTL4CommitFeedback> feedback) {
        NSData *data = [heap resolveCounterRange:NSMakeRange(0, count * 2)];
        if (feedback.error || data.length != count * 2 * sizeof(MTL4TimestampHeapEntry) || hz <= 0) {
            MLOG("[dispatch-gpu] invalid GPU feedback/counters at submit %llu", (unsigned long long)fence); free(records); return;
        }
        const MTL4TimestampHeapEntry *ts = data.bytes; const uint32_t *grid = grids.contents;
        FILE *f = fopen(getenv("OOT_DISPATCH_GPU_FILE"), "a");
        if (f) {
            for (int i = 0; i < count; i++) {
                const dp_time *r = &records[i]; uint64_t lo = ts[2*i].timestamp, hi = ts[2*i+1].timestamp;
                fprintf(f, "%llu\t%llu\t%d\t%d\t%s\t%.6f\t%u\t%u\t%u\t%u\t%u\t%u\t%llu\t%llu\t%llu\t%u\n",
                    (unsigned long long)r->frame, (unsigned long long)fence, r->encoder, i, r->key, hi >= lo ? (hi-lo)/hz*1e3 : -1.,
                    grid[4*i], grid[4*i+1], grid[4*i+2], r->tg[0], r->tg[1], r->tg[2], (unsigned long long)r->indirect,
                    (unsigned long long)lo, (unsigned long long)hi, grid[4*i+3]);
            }
            fclose(f);
        } else MLOG("[dispatch-gpu] cannot open output file");
        free(records);
    }];
}
