// Ocarina of Time on the Mac: render and compute pipelines through Metal 4 and its archive (nvn_pso4.h). Included by
// nvn_mtl.m just before pso_new / cs_new_, which try these first; nil sends them down the old path (MTLDevice and the old
// archive), unchanged: a description Metal 4 cannot take, a key the old files hold, or a compile Metal 4 refused.
#pragma once
#ifdef PSO4_BUILT
/* pso_desc's description for Metal 4 (pso_desc sets only these fields). No depth or stencil format: a Metal 4 render
   pipeline names none. nil: a function without a note (lib_fn), or rasterization without a fragment function, which
   Metal 4 requires (nxnative keeps depth-only draws on the MTLDevice path for the same reason). */
static MTL4RenderPipelineDescriptor *pso4_desc(MTLRenderPipelineDescriptor *d) {
    if (!d.vertexFunction || (d.rasterizationEnabled && !d.fragmentFunction)) return nil;
    MTL4FunctionDescriptor *vf = fn4_of(d.vertexFunction), *ff = d.fragmentFunction ? fn4_of(d.fragmentFunction) : nil;
    if (!vf || (d.fragmentFunction && !ff)) return nil;
    MTL4RenderPipelineDescriptor *m = [MTL4RenderPipelineDescriptor new];
    m.label = d.label; m.vertexFunctionDescriptor = vf; m.fragmentFunctionDescriptor = ff; m.vertexDescriptor = d.vertexDescriptor;
    m.rasterSampleCount = d.rasterSampleCount; m.rasterizationEnabled = d.rasterizationEnabled; m.inputPrimitiveTopology = d.inputPrimitiveTopology;
    m.alphaToCoverageState = d.alphaToCoverageEnabled ? MTL4AlphaToCoverageStateEnabled : MTL4AlphaToCoverageStateDisabled;
    m.alphaToOneState = d.alphaToOneEnabled ? MTL4AlphaToOneStateEnabled : MTL4AlphaToOneStateDisabled;
    for (int i = 0; i < 8; i++) {
        MTLRenderPipelineColorAttachmentDescriptor *c = d.colorAttachments[i];
        MTL4RenderPipelineColorAttachmentDescriptor *a = m.colorAttachments[i];
        a.pixelFormat = c.pixelFormat; a.writeMask = c.writeMask;
        a.blendingState = c.blendingEnabled ? MTL4BlendStateEnabled : MTL4BlendStateDisabled;
        a.sourceRGBBlendFactor = c.sourceRGBBlendFactor; a.destinationRGBBlendFactor = c.destinationRGBBlendFactor; a.rgbBlendOperation = c.rgbBlendOperation;
        a.sourceAlphaBlendFactor = c.sourceAlphaBlendFactor; a.destinationAlphaBlendFactor = c.destinationAlphaBlendFactor; a.alphaBlendOperation = c.alphaBlendOperation;
    }
    return m;
}
static MTL4ComputePipelineDescriptor *cs4_desc(MTLComputePipelineDescriptor *d) {
    MTL4FunctionDescriptor *f = fn4_of(d.computeFunction);
    if (!f) return nil;
    MTL4ComputePipelineDescriptor *m = [MTL4ComputePipelineDescriptor new];
    m.label = d.label; m.computeFunctionDescriptor = f;
    m.maxTotalThreadsPerThreadgroup = d.maxTotalThreadsPerThreadgroup;
    m.threadGroupSizeIsMultipleOfThreadExecutionWidth = d.threadGroupSizeIsMultipleOfThreadExecutionWidth;
    return m;
}
/* Two capturing compiles that share a vertex-stage function (a render pipeline's vertex function, a compute kernel) never
   run at once: Metal crashes when one capture frees a program entry another one is reading (SIGSEGV in objc_msgSend under
   MultiLevelAirntBinaryCache::getElementAtDestination; nxnative found it on Ocarina of Time's prebuild, 3 runs of 3, GitHub
   issue #815, and guards it the same way). A function is its library object and entry name (a hash collision only makes two
   compiles wait). A capture that waits longer than 2 s builds with the plain compiler instead (not archived this run). */
static pthread_mutex_t g_fn4_busy_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_fn4_busy_cv = PTHREAD_COND_INITIALIZER;
static NSMutableSet<NSNumber *> *g_fn4_busy;
static _Atomic uint64_t g_fn4_gave_up;
static uint64_t fn4_id(MTL4FunctionDescriptor *fd) {
    if (![fd isKindOfClass:MTL4LibraryFunctionDescriptor.class]) return 0;
    MTL4LibraryFunctionDescriptor *l = (MTL4LibraryFunctionDescriptor *)fd;
    return ((uint64_t)(uintptr_t)(__bridge void *)l.library * 0x9E3779B97F4A7C15ull) ^ (uint64_t)l.name.hash;
}
/* The packaging tool waits as long as it takes: every listed pipeline must reach the archive (2026-11-17, the first Metal 4
   packaging gave up 2 s waits on 1,945 of 28,933 pipelines, built them uncaptured, and the App kept 1,278 old files for
   them). In play a draw waits 2 s at most. */
static int fn4_claim(uint64_t id) {
    if (!id) return 1;
    struct timespec until; clock_gettime(CLOCK_REALTIME, &until); until.tv_sec += 2;
    pthread_mutex_lock(&g_fn4_busy_mu);
    if (!g_fn4_busy) g_fn4_busy = [NSMutableSet set];
    while ([g_fn4_busy containsObject:@(id)])
        if (g_compute_archive_tool) pthread_cond_wait(&g_fn4_busy_cv, &g_fn4_busy_mu);
        else if (pthread_cond_timedwait(&g_fn4_busy_cv, &g_fn4_busy_mu, &until) == ETIMEDOUT && [g_fn4_busy containsObject:@(id)]) {
            pthread_mutex_unlock(&g_fn4_busy_mu); atomic_fetch_add(&g_fn4_gave_up, 1); return 0;
        }
    [g_fn4_busy addObject:@(id)];
    pthread_mutex_unlock(&g_fn4_busy_mu);
    return 1;
}
static void fn4_release(uint64_t id) {
    if (!id) return;
    pthread_mutex_lock(&g_fn4_busy_mu); [g_fn4_busy removeObject:@(id)]; pthread_cond_broadcast(&g_fn4_busy_cv); pthread_mutex_unlock(&g_fn4_busy_mu);
}
/* On par4_queue, after a period's write failed (par4_write): its pipelines are captured again in two halves, each a period
   of its own written in turn, and a half that fails is split again. A single pipeline that still fails is left out: it is
   compiled where it is drawn, as before the archive had it. */
/* On par4_queue: the pipelines (keys, their Metal 4 descriptions) compiled into a period of their own and written; a write
   that fails is done again in halves (par4_resplit). */
static void par4_capture_chunk(NSArray<NSString *> *k, NSArray *d) {
    Par4Period *q = par4_new_period(g_dev);
    if (!q) { atomic_fetch_add(&g_par4_lost, k.count); return; }
    NSMutableArray<NSString *> *got = [NSMutableArray array]; NSMutableArray *gotd = [NSMutableArray array];
    NSLock *lk = [NSLock new];
    /* 4 compiles at once: the compiler service builds them side by side (one at a time, lonlon-img-1 had not written a
       file after 3 minutes of a 2,600-pipeline warm-up set). All of them return before the period is written. */
    dispatch_apply(k.count, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^(size_t i) { @autoreleasepool {
        static dispatch_semaphore_t four; static dispatch_once_t once; dispatch_once(&once, ^{ four = dispatch_semaphore_create(4); });
        dispatch_semaphore_wait(four, DISPATCH_TIME_FOREVER);
        id desc = d[i]; BOOL render = [desc isKindOfClass:MTL4RenderPipelineDescriptor.class];
        uint64_t fid = fn4_id(render ? ((MTL4RenderPipelineDescriptor *)desc).vertexFunctionDescriptor : ((MTL4ComputePipelineDescriptor *)desc).computeFunctionDescriptor);
        int claimed = fn4_claim(fid);
        NSError *e = nil;
        id ps = !claimed ? nil : render ? (id)[q->comp newRenderPipelineStateWithDescriptor:desc compilerTaskOptions:nil error:&e]
                                        : (id)[q->comp newComputePipelineStateWithDescriptor:desc compilerTaskOptions:nil error:&e];
        if (claimed) fn4_release(fid);
        dispatch_semaphore_signal(four);
        [lk lock];
        if (ps) { [got addObject:k[i]]; [gotd addObject:desc]; } else atomic_fetch_add(&g_par4_lost, 1);
        [lk unlock];
    } });
    NSError *why = nil;
    BOOL ok = !got.count || par4_publish(q, got, &why);
    par4_release(q);
    if (!ok) {
        if (atomic_fetch_add(&g_par4_failed, 1) < 64) MLOG("【红】Metal 4 管线存档：%lu 条写不了（%s），对半拆开再存", (unsigned long)got.count, why.localizedDescription.UTF8String ?: "?");
        par4_resplit(got, gotd);
    }
}
static void par4_resplit(NSArray<NSString *> *keys, NSArray *descs) {
    if (!keys.count || keys.count != descs.count) { atomic_fetch_add(&g_par4_lost, keys.count); return; }
    if (keys.count == 1) { atomic_fetch_add(&g_par4_lost, 1); MLOG("【红】Metal 4 管线存档：%s 单独也写不了，不存（画到时现编）", keys[0].UTF8String); return; }
    NSUInteger half = keys.count / 2;
    par4_capture_chunk([keys subarrayWithRange:NSMakeRange(0, half)], [descs subarrayWithRange:NSMakeRange(0, half)]);
    par4_capture_chunk([keys subarrayWithRange:NSMakeRange(half, keys.count - half)], [descs subarrayWithRange:NSMakeRange(half, descs.count - half)]);
}
/* Packaging (mtl_pipeline_archive_tool): the rows the memory saver keeps before the title (the title list, the App lists'
   compute / mesh / tile-read rows: warm_resident's) are made first, with g_par4_compact set, so a key in a large file is
   compiled again and captured into a small file of its own: opening a Metal 4 file reads all of it (route vm.log: 4.5-5.5
   GB paged in within the first minute for the App's 89 files), and those rows were in nearly every one, so the title
   waited 8-37 s on a USB drive (played-img-1 / field-img-1). A key already in a small file (PAR4_SMALL pipelines or fewer,
   by the count in its name) is taken from it. User 2026-11-18: "好 这样改吧" (instead of capturing them again during play). */
static int g_par4_compact;
static int par4_in_small_file(NSString *key) {
    pthread_mutex_lock(&g_par_mu); NSString *name = g_par_where[key]; pthread_mutex_unlock(&g_par_mu);
    NSUInteger n = name && par4_name(name) ? par4_name_count([name substringFromIndex:3]) : 0;
    return n && n <= PAR4_SMALL;
}
/* The pipeline from the key's Metal 4 file, or compiled through Metal 4 (captured into the archive when it has a key).
   kind: PAR_RENDER / PAR_COMPUTE (desc4 matching). nil: take the old path. */
static id par4_make(int kind, id desc4, NSString *key) {
    if (!desc4) { atomic_fetch_add(&g_par4_old_path, 1); return nil; }
    int old = 0; id ps = nil;
    /* packaging's pass over the rows kept before the title (g_par4_compact): a key in a large file is compiled again into
       the small file this pass writes */
    int compact = g_par4_compact && key && !par4_in_small_file(key);
    if (compact) { pthread_mutex_lock(&g_par_mu); if (!g_par_used) g_par_used = [NSMutableSet set]; [g_par_used addObject:key]; pthread_mutex_unlock(&g_par_mu); }
    id<MTL4Archive> a = key && !compact ? par4_archive_for(g_dev, key, &old) : nil;
    /* A key only an old file holds (this Mac's own files from before 2026-11-17, or an App packaged before) is compiled
       through Metal 4 once and captured: the old file would otherwise be opened at every start that needs it (100-360 ms
       each from a USB drive, under the replay lock), the new one is a few large files opened once. */
    if (old) atomic_fetch_add(&g_par4_migrated, 1);
    if (a) {
        NSError *why = nil;
        ps = kind == PAR_RENDER ? (id)[a newRenderPipelineStateWithDescriptor:desc4 error:&why] : (id)[a newComputePipelineStateWithDescriptor:desc4 error:&why];
        par_pre_note(ps != nil); par_lookup_note(kind, key, 1, ps != nil, why);
        if (ps) {
            atomic_fetch_add(&g_par4_hits, 1); par_count(1);
            return ps;
        }
        par_stale(key);
    } else if (key) par_lookup_note(kind, key, 0, 0, nil);
    if (key && g_par_on) par_count(0);
    uint64_t fid = key ? fn4_id(kind == PAR_RENDER ? ((MTL4RenderPipelineDescriptor *)desc4).vertexFunctionDescriptor
                                                   : ((MTL4ComputePipelineDescriptor *)desc4).computeFunctionDescriptor) : 0;
    int claimed = key && fn4_claim(fid);
    Par4Period *p = claimed ? par4_enter(g_dev) : nil;
    id<MTL4Compiler> c = p ? p->comp : par4_plain(g_dev);
    NSError *ce = nil;
    if (c) ps = kind == PAR_RENDER ? (id)[c newRenderPipelineStateWithDescriptor:desc4 compilerTaskOptions:nil error:&ce]
                                   : (id)[c newComputePipelineStateWithDescriptor:desc4 compilerTaskOptions:nil error:&ce];
    if (p) par4_leave(p, key, desc4, ps != nil);
    if (claimed) fn4_release(fid);
    if (!ps && p) {   // refused through the capturing compiler: the period is written as it is, then once more without capture
        par4_refused(p);
        if (atomic_fetch_add(&g_par4_failed, 1) < 4) MLOG("【红】Metal 4 管线存档：捕获时编不了 %s（%s），改不捕获再编", key.UTF8String, ce.localizedDescription.UTF8String ?: "?");
        id<MTL4Compiler> plain = par4_plain(g_dev); ce = nil;
        if (plain) ps = kind == PAR_RENDER ? (id)[plain newRenderPipelineStateWithDescriptor:desc4 compilerTaskOptions:nil error:&ce]
                                           : (id)[plain newComputePipelineStateWithDescriptor:desc4 compilerTaskOptions:nil error:&ce];
    }
    if (ps) atomic_fetch_add(&g_par4_built, 1);
    return ps;   // nil: the old path compiles it and reports the error
}
#endif
