/* probe-gpu-arena-trim (included after the production backend): a burst of argument blocks in one submission (as while
   loading) is released after the arena idle horizon and leaves residency, while the reused block stays resident.
   Negative OOT_ARENA_TRIM_PROBE_NEGATIVE=1 skips the idle window; =2 releases the pending CPU source before checking it. */
int mtl_probe_arena_trim(void) {
    mtl_init(); int checked = 0; op_hdr end = {OP_END, 8};
    int negative = getenv("OOT_ARENA_TRIM_PROBE_NEGATIVE") ? atoi(getenv("OOT_ARENA_TRIM_PROBE_NEGATIVE")) : 0;
    enum { BURST = 20 }; const size_t block = 4u << 20;
    uint64_t released0 = g_arenas_released;
    mtl_sub_begin(); mtl_exec((const uint8_t *)&end);
    __unsafe_unretained id<MTLBuffer> b = nil;
    NSHashTable *burst = [NSHashTable weakObjectsHashTable];
    NSMutableArray<NvShader *> *cached = [NSMutableArray array];
    id<MTLBuffer> pixels = probe_buf((BURST + 1) * 64);
    for (int i = 0; i < BURST; i++) {
        uint8_t *cpu; uint64_t gpu; size_t off = arena_alloc_u(block, &b, &cpu, &gpu); [burst addObject:b];
        NvShader *sh = [NvShader new]; sh->ab_buf = b; sh->ab_cpu = cpu; sh->ab_gpu = gpu; sh->ab_off = off; sh->ab_arena = g_arena_epoch;
        shader_args_invalidate_stale(sh);
        if (sh->ab_buf != b || sh->ab_cpu != cpu) MDIE("arena trim probe: current cache was invalidated");
        [cached addObject:sh]; checked++;
        memset(cpu, 17 + i, 64);
        enc_b_copy_buffer(blit(), b, off, pixels, i * 64, 64);
    }
    mtl_sub_commit(); mtl_finish();
    uint64_t burst_fence = g_fence, idle = arena_idle_submits();
    for (int i = 0; i < BURST; i++) for (int j = 0; j < 64; j++) {
        if (((uint8_t *)pixels.contents)[i * 64 + j] != 17 + i) MDIE("arena trim probe: initial GPU bytes changed");
        checked++;
    }
    int resident = 0;
    @autoreleasepool { pthread_mutex_lock(&g_res_mu); for (id x in burst) resident += [g_res_objects containsObject:x]; pthread_mutex_unlock(&g_res_mu); }
    if (resident != BURST) MDIE("arena trim probe: %d of %d burst blocks resident", resident, BURST);
    checked++;
    id<MTLBuffer> kept = nil;
    int rounds = negative == 1 ? 8 : (int)idle + 8;
    for (int r = 0; r < rounds; r++) {
        mtl_sub_begin(); mtl_exec((const uint8_t *)&end);
        uint8_t *working; size_t off = arena_alloc_u(256, &b, &working, NULL); kept = b;
        memset(working, 0x6d, 64);
        enc_b_copy_buffer(blit(), b, off, pixels, BURST * 64, 64);
        mtl_sub_commit(); mtl_finish();   // one submission in flight: the working set is one or two blocks
        for (int j = 0; j < 64; j++) {
            if (((uint8_t *)pixels.contents)[BURST * 64 + j] != 0x6d) MDIE("arena trim probe: reused working block GPU bytes changed");
            checked++;
        }
        if (g_fence == burst_fence + idle) {
            @autoreleasepool {
                pthread_mutex_lock(&g_res_mu); int retained = 0;
                for (id object in burst) retained += [g_res_objects containsObject:object];
                pthread_mutex_unlock(&g_res_mu);
                if (retained != BURST) MDIE("arena trim probe: block released at the inclusive idle boundary");
            }
            checked++;
        }
    }
    mtl_finish(); mtl_sub_begin(); mtl_exec((const uint8_t *)&end); mtl_sub_commit(); mtl_finish();
    uint64_t released = g_arenas_released - released0;
    if (released < BURST - 2) MDIE("arena trim probe: %llu of %d idle burst blocks released", (unsigned long long)released, BURST);
    checked++;
    pthread_mutex_lock(&g_res_mu); resident = 0; for (id x in burst) resident += [g_res_objects containsObject:x];
    int kept_res = [g_res_objects containsObject:kept]; pthread_mutex_unlock(&g_res_mu);
    if (resident > 2) MDIE("arena trim probe: %d burst blocks still resident, the working set is at most 2", resident);
    if (!kept_res) MDIE("arena trim probe: the block in use left the residency set");
    checked += 2;
    // A weak hash table's count includes emptied slots; allObjects enumerates only allocations still alive.
    NSUInteger burst_alive;
    @autoreleasepool { burst_alive = burst.allObjects.count; }
    MLOG("arena trim probe: weak slots %lu, live burst allocations %lu", (unsigned long)burst.count, (unsigned long)burst_alive);
    if (burst_alive > 2) MDIE("arena trim probe: cached shaders retained %lu burst allocations", (unsigned long)burst_alive);
    checked++;
    for (NvShader *sh in cached) {
        shader_args_invalidate_stale(sh);
        if (sh->ab_buf || sh->ab_cpu || sh->ab_gpu || sh->ab_pass) MDIE("arena trim probe: stale cache survived its epoch");
        checked++;
    }
    /* A real pending inline write freezes its CPU source cutoff across GPU completion. The explicit future-age
       argument isolates that guard; it does not claim this one replay naturally lasts 120 submissions. */
    size_t page = (size_t)getpagesize(); void *memory = NULL;
    if (posix_memalign(&memory, page, page)) MDIE("arena trim probe: pending destination allocation");
    memset(memory, 0, page); mtl_pool_add(memory, page);
    id<MTLSharedEvent> hold = [g_dev newSharedEvent];
    if (!hold) MDIE("arena trim probe: pending GPU event allocation");
    __weak id<MTLBuffer> pending = nil;
    uint8_t value[64]; memset(value, 0x5a, sizeof value);
    mtl_sub_begin(); mtl_exec((const uint8_t *)&end);
    __unsafe_unretained id<MTLBuffer> source;
    size_t prefix = (4u << 20) + 256;  // distinct 8 MiB block, not the strongly held 4 MiB working witness
    arena_alloc_u(prefix, &source, NULL, NULL); pending = source;
    mtl_sub_update(value, (uint64_t)(uintptr_t)memory, sizeof value);
    enc_b_copy_buffer(blit(), source, prefix, pixels, 0, sizeof value);
    [g_q4 waitForEvent:hold value:1]; uint64_t writer = mtl_sub_commit();
    mtl_sub_begin(); mtl_exec((const uint8_t *)&end);
    const char *failure = NULL;
    if (g_pend_reuse_done >= writer || !g_npend) failure = "pending CPU source was not captured";
    arena_trim(writer + idle + 1);
    @autoreleasepool {
        id object = pending;
        pthread_mutex_lock(&g_res_mu); int pinned = object && [g_res_objects containsObject:object]; pthread_mutex_unlock(&g_res_mu);
        if (!pinned && !failure) failure = "GPU-incomplete arena was trimmed";
    }
    hold.signaledValue = 1;  // release the GPU queue before any assertion/negative-control failure
    if (failure) MDIE("arena trim probe: %s", failure);
    mtl_wait(writer, UINT64_MAX);
    if (negative == 2) pend_begin();  // actual completed-writer cleanup removes the frozen CPU source
    int pending_alive;
    @autoreleasepool { arena_trim(writer + idle + 1); res_commit(); pending_alive = pending != nil; }
    if (!pending_alive) MDIE("arena trim probe: frozen pending CPU source was trimmed after GPU completion");
    uint8_t got[64]; pend_read((uint64_t)(uintptr_t)memory, got, sizeof got, 0);
    if (memcmp(got, value, sizeof got) || memcmp(pixels.contents, value, sizeof value))
        MDIE("arena trim probe: pending CPU or queued GPU bytes changed");
    checked += 4;
    mtl_sub_commit(); mtl_finish();
    mtl_sub_begin(); mtl_exec((const uint8_t *)&end); // pend_begin now consumes the completed writer
    uint64_t pending_deadline = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + 1000000000ull;
    do {
        @autoreleasepool { arena_trim(writer + idle + 1); res_commit(); pending_alive = pending != nil; }
        if (!pending_alive) break;
        usleep(1000);
    } while (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) < pending_deadline);
    if (pending_alive) MDIE("arena trim probe: unpinned idle source stayed alive after its CPU readers ended");
    checked++;
    mtl_sub_commit(); mtl_finish(); mtl_pool_del(memory); free(memory);
    // Retire a temporary exclusive buffer and one shared-bank shader at the same fence.
    // The weak exclusive witness is queued first, so its release also follows the shared-bank retirement check.
    __weak id<MTLBuffer> exclusive = nil;
    NvShader *survivor = nil; id<MTLBuffer> shared_bank = nil;
    mtl_sub_begin(); mtl_exec((const uint8_t *)&end);
    @autoreleasepool {
        NvShader *first = [NvShader new]; survivor = [NvShader new];
        first->rf.c1_len = survivor->rf.c1_len = 16; survivor->rf.c1[0] = 0x6d;
        NSString *bounded = @"float a = (*spvDescriptorSet0.fp_c1)._m0[0].x;";
        c1_store(first, bounded); c1_store(survivor, bounded);
        if (first->c1_exclusive || survivor->c1_exclusive || first->c1buf != survivor->c1buf)
            MDIE("shader lifetime probe: expected shared c1 bank");
        shared_bank = survivor->c1buf;
        os_unfair_lock_lock(&g_c1_lock);
        uint64_t exclusive0 = g_c1_exclusive; size_t allocated0 = g_c1_alloc_bytes, data0 = g_c1_data_bytes;
        os_unfair_lock_unlock(&g_c1_lock);
        NvShader *sh = [NvShader new]; sh->rf.c1_len = 16;
        c1_store(sh, nil); exclusive = sh->c1buf;
        if (!sh->c1_exclusive) MDIE("shader lifetime probe: missing exclusive c1 ownership");
        sh = nil; first = nil; defer_drain();
        os_unfair_lock_lock(&g_c1_lock);
        int accounted = g_c1_exclusive == exclusive0 && g_c1_alloc_bytes == allocated0 && g_c1_data_bytes == data0;
        os_unfair_lock_unlock(&g_c1_lock);
        if (!accounted) MDIE("shader lifetime probe: retired c1 still counted as shader-owned");
        id<MTLBuffer> held = exclusive;
        if (!held) MDIE("shader lifetime probe: c1 retired before its submission completed");
        pthread_mutex_lock(&g_res_mu); int c1_resident = [g_res_objects containsObject:held]; pthread_mutex_unlock(&g_res_mu);
        if (!c1_resident) MDIE("shader lifetime probe: c1 left residency before its fence");
        checked += 3;
    }
    mtl_sub_commit(); mtl_finish();
    mtl_sub_begin(); mtl_exec((const uint8_t *)&end); mtl_sub_commit(); mtl_finish();
    // g_done is published before a feedback callback drops its local ready array. Weak loads need a pool per retry.
    int alive = 1; uint64_t deadline = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + 1000000000ull;
    do {
        @autoreleasepool { defer_drain(); res_commit(); alive = exclusive != nil; }
        if (!alive) break;
        usleep(1000);
    } while (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) < deadline);
    if (alive) MDIE("shader lifetime probe: completed exclusive c1 allocation still alive after 1 second");
    checked++;
    defer_drain(); res_commit();
    pthread_mutex_lock(&g_res_mu); int bank_resident = [g_res_objects containsObject:shared_bank]; pthread_mutex_unlock(&g_res_mu);
    if (!bank_resident || !survivor->c1buf || ((uint8_t *)shared_bank.contents)[survivor->c1off] != 0x6d)
        MDIE("shader lifetime probe: shared bank changed or retired with one shader");
    checked += 2;
    MLOG("arena trim probe: %llu idle blocks released, working block kept", (unsigned long long)released);
    res_remove(pixels);
    return checked;
}
