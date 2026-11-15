/* Included by nvn_mtl.m. Diagnose wrapper lifetime without a full-game run. */
static uint64_t probe_pool_alive(void) {
    uint64_t n = 0;
    for (int i = 0; i < RES_SITES; i++) n += atomic_load(&g_res_site[i].alive);
    return n;
}
/* Actual dirty tracking and page protection, not the Foundation model. Metal
   need not reject a read-only mapping, so this does not force a retry. */
static int probe_pool_protected(void *mem, size_t size) {
    const char *failure = NULL;
    int checked = 0, registered = 0;
    uint64_t stamp = 0, off = 0; void *buffer = NULL;
    stor_alias_pool before = {0}, after = {0};
    op_hdr end = {OP_END, 8};
    id<MTLBuffer> readback = probe_buf(4096);
    for (int i = 0; i < 1024; i++) ((uint32_t *)mem)[i] = (uint32_t)(i * 65537u ^ 0x81a527dbu);
    nvn_dirty_add_range(mem, size);
    if (!nvn_dirty_tracked(mem, size)) { failure = "range not tracked"; goto cleanup; }
    checked++;
    stamp = nvn_dirty_now(); nvn_dirty_protect(mem, size);
    {
        mach_vm_address_t address = (uintptr_t)mem; mach_vm_size_t length = 0;
        vm_region_basic_info_data_64_t info; mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object = MACH_PORT_NULL;
        kern_return_t result = mach_vm_region(mach_task_self(), &address, &length, VM_REGION_BASIC_INFO_64,
                                             (vm_region_info_t)&info, &count, &object);
        if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
        if (result != KERN_SUCCESS || address > (uintptr_t)mem || !(info.protection & VM_PROT_READ) ||
            (info.protection & VM_PROT_WRITE)) { failure = "tracker did not protect the source page"; goto cleanup; }
        checked++;
    }
    mtl_pool_add(mem, size); registered = 1;
    if (!stor_alias_pool_range((uintptr_t)mem, size, &before)) { failure = "protected registration missing"; goto cleanup; }
    checked++;
#if defined(GAME_OOT) && !TARGET_OS_IPHONE
    if (!pool_res_all()) {
        pthread_mutex_lock(&g_mu);
        const pidx_ent *pool = pidx_find(&g_pools, (uintptr_t)mem, size);
        int dormant = pool && !pool->buf && !pool->res;
        pthread_mutex_unlock(&g_mu);
        if (!dormant) { failure = "protected source materialized before first use"; goto cleanup; }
        checked++;
    }
#endif
    if (!mtl_addr((uintptr_t)mem, 4096, &buffer, &off) || off || !buffer ||
        ![g_res containsAllocation:(__bridge id<MTLBuffer>)buffer]) { failure = "protected first-use residency"; goto cleanup; }
    checked++;
    if (!stor_alias_pool_range((uintptr_t)mem, size, &after) || before.generation != after.generation ||
        nvn_dirty_since(mem, size, stamp)) { failure = "first use changed identity or CPU dirty generation"; goto cleanup; }
    checked++;
    mtl_sub_begin(); mtl_exec((const uint8_t *)&end);
    memset(readback.contents, 0xa5, 4096);
    enc_b_copy_buffer(blit(), (__bridge id<MTLBuffer>)buffer, off, readback, 0, 4096);
    mtl_sub_commit(); mtl_finish();
    for (int i = 0; i < 1024; i++) {
        if (((uint32_t *)readback.contents)[i] != (uint32_t)(i * 65537u ^ 0x81a527dbu)) {
            failure = "protected GPU byte mismatch"; goto cleanup;
        }
        checked++;
    }
    ((uint32_t *)mem)[0] ^= 0xdeadbeefu;   // real write fault after the reader has completed
    if (!nvn_dirty_since(mem, size, stamp)) { failure = "later CPU write not tracked"; goto cleanup; }
    checked++;
cleanup:
    // Every fixture assertion reaches cleanup; there is no gated GPU reader.
    if (registered) mtl_pool_del(mem);
    nvn_dirty_remove_range(mem);
    res_remove(readback); res_commit();
    if (nvn_dirty_tracked(mem, size) && !failure) failure = "dirty range survived cleanup";
    checked++;
    if (failure) MDIE("protected pool probe: %s", failure);
    MLOG("pool protected-page probe: %d checks, resident-all=%d (driver rejection/retry not forced)", checked, pool_res_all());
    return checked;
}
int mtl_probe_pool_retire(void) {
    @autoreleasepool { mtl_init(); res_commit(); }
    const char *ne = getenv("TOTK_PROBE_NEGATIVE");
    g_res_remove_no_pool = ne && atoi(ne) == 22;
    const size_t size = 1 << 20; void *mem = NULL;
    if (posix_memalign(&mem, 16384, size) || !mem) MDIE("pool retirement probe memory");
    memset(mem, 0, size);
    uint64_t base = probe_pool_alive(), previous_registration = 0; int checked = 0;
    for (int mode = 0; mode < 7; mode++) {
        @autoreleasepool {
            for (int i = 0; i < 64; i++) {
                void *outer = mode == 1 ? objc_autoreleasePoolPush() : NULL;
                void *inner = mode == 2 ? objc_autoreleasePoolPush() : NULL;
                mtl_pool_add(mem, size);
                stor_alias_pool registered = {0};
                if (!stor_alias_pool_range((uintptr_t)mem, size, &registered) ||
                    registered.generation <= previous_registration)
                    MDIE("pool retirement registration missing or reused before first address");
                previous_registration = registered.generation;
#if defined(GAME_OOT) && !TARGET_OS_IPHONE
                if (!pool_res_all()) {
                    pthread_mutex_lock(&g_mu);
                    const pidx_ent *unused = pidx_find(&g_pools, (uintptr_t)mem, size);
                    int dormant = unused && !unused->buf && !unused->gpu && !unused->res;
                    pthread_mutex_unlock(&g_mu);
                    if (!dormant) MDIE("pool retirement unused registration already constructed a wrapper");
                    checked++;
                }
#endif
                checked++;
                if (inner) objc_autoreleasePoolPop(inner);
                inner = mode == 3 ? objc_autoreleasePoolPush() : NULL;
                void *buf = NULL; uint64_t off = 0;
                if (!mtl_addr((uintptr_t)mem, size, &buf, &off) || off) MDIE("pool retirement probe lookup");
                stor_alias_pool materialized = {0};
                if (!buf || ![g_res containsAllocation:(__bridge id<MTLBuffer>)buf] ||
                    !stor_alias_pool_range((uintptr_t)mem, size, &materialized) ||
                    materialized.generation != registered.generation)
                    MDIE("pool retirement first use lost residency or registration identity");
                checked++;
                if (inner) objc_autoreleasePoolPop(inner);
                inner = mode == 4 || mode == 5 ? objc_autoreleasePoolPush() : NULL;
                mtl_pool_del(mem);
                if (mtl_addr((uintptr_t)mem, size, &buf, &off))
                    MDIE("pool retirement TLS still resolves a finalized registration");
                checked++;
                if (mode == 5) { objc_autoreleasePoolPop(inner); inner = NULL; }
                if (mode == 6) inner = objc_autoreleasePoolPush();
                res_commit();
                if (inner) objc_autoreleasePoolPop(inner);
                if (outer) objc_autoreleasePoolPop(outer);
                checked++;
            }
            MLOG("pool retirement diagnosis: mode=%d alive_delta=%lld keep=%lu pools=%d allocated_mib=%llu", mode,
                (long long)(probe_pool_alive()-base),(unsigned long)g_keep.count,g_pools.n,(unsigned long long)(g_dev.currentAllocatedSize>>20));
            if (probe_pool_alive() != base || g_keep.count || g_pools.n)
                MDIE("pool retirement probe retained removed wrappers mode%d", mode);
            checked++;
        }
        MLOG("pool retirement diagnosis: mode=%d drained_alive_delta=%lld",mode,(long long)(probe_pool_alive()-base));
        if (probe_pool_alive() != base) MDIE("pool retirement probe failed outer drain");
        checked++;
    }
    // Use each wrapper on the GPU before retiring it, while the outer scope
    // stays alive for the whole loop as it does on a guest thread.
    @autoreleasepool {
        id<MTLBuffer> readback = probe_buf(4096);
        op_hdr end = {OP_END, 8};
        for (int cycle = 0; cycle < 64; cycle++) {
            for (int i = 0; i < 1024; i++) ((uint32_t *)mem)[i] = (uint32_t)(i*65537u ^ cycle*1234567u);
            mtl_pool_add(mem, size);
            mtl_sub_begin(); mtl_exec((const uint8_t *)&end);
            void *buf = NULL; uint64_t off = 0;
            if (!mtl_addr((uintptr_t)mem, 4096, &buf, &off)) MDIE("pool retirement GPU lookup");
            enc_b_copy_buffer(blit(), (__bridge id<MTLBuffer>)buf, off, readback, 0, 4096);
            mtl_sub_commit(); mtl_finish();
            mtl_pool_del(mem); res_commit();
            if (g_keep.count || g_pools.n) MDIE("pool retirement GPU pool table");
            for (int i = 0; i < 1024; i++) {
                if (((uint32_t *)readback.contents)[i] != ((uint32_t *)mem)[i])
                    MDIE("pool retirement GPU byte mismatch cycle%d word%d",cycle,i);
                checked++;
            }
        }
        res_remove(readback);
    }
    checked += probe_pool_protected(mem, size);
    g_res_remove_no_pool = 0;
    free(mem); return checked;
}
