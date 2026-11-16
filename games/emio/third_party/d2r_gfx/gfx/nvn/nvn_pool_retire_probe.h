/* Included by nvn_mtl.m. Diagnose wrapper lifetime without a full-game run. */
static uint64_t probe_pool_alive(void) {
    uint64_t n = 0;
    for (int i = 0; i < RES_SITES; i++) n += atomic_load(&g_res_site[i].alive);
    return n;
}
int mtl_probe_pool_retire(void) {
    @autoreleasepool { mtl_init(); res_commit(); }
    const char *ne = getenv("TOTK_PROBE_NEGATIVE");
    g_res_remove_no_pool = ne && atoi(ne) == 22;
    const size_t size = 1 << 20; void *mem = NULL;
    if (posix_memalign(&mem, 16384, size) || !mem) MDIE("pool retirement probe memory");
    memset(mem, 0, size);
    uint64_t base = probe_pool_alive(); int checked = 0;
    for (int mode = 0; mode < 7; mode++) {
        @autoreleasepool {
            for (int i = 0; i < 64; i++) {
                void *outer = mode == 1 ? objc_autoreleasePoolPush() : NULL;
                void *inner = mode == 2 ? objc_autoreleasePoolPush() : NULL;
                mtl_pool_add(mem, size);
                if (inner) objc_autoreleasePoolPop(inner);
                inner = mode == 3 ? objc_autoreleasePoolPush() : NULL;
                void *buf = NULL; uint64_t off = 0;
                if (!mtl_addr((uintptr_t)mem, size, &buf, &off) || off) MDIE("pool retirement probe lookup");
                if (inner) objc_autoreleasePoolPop(inner);
                inner = mode == 4 || mode == 5 ? objc_autoreleasePoolPush() : NULL;
                mtl_pool_del(mem);
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
    g_res_remove_no_pool = 0;
    free(mem); return checked;
}
