/* Included by nvn_mtl.m. Diagnose wrapper lifetime without a full-game run. */
static uint64_t probe_pool_alive(void) {
    uint64_t n = 0;
    for (int i = 0; i < RES_SITES; i++) n += atomic_load(&g_res_site[i].alive);
    return n;
}
/* Real Metal reproduction: pending CPU writeback retains an uploaded texture
   after TextureFinalize; destroying its pool must release that extra ownership. */
static int probe_writeback_pool_retire(void) {
    setenv("TOTK_GPU_KEEPALIVE", "0", 1);
    setenv("TOTK_PKG_FINAL_CACHE", "0", 1);
    setenv("TOTK_TEX_MIRROR", "1", 1);
    setenv("TOTK_RENDER_STREAM", "0", 1);
    mtl_init();
    op_hdr end={OP_END,8}; int checked=0;
    for (int cycle=0;cycle<32;cycle++) {
        tex_t t={0};t.serial=0xffec0000+cycle;
        t.d=(nvn_texdesc){.width=64,.height=64,.depth=1,.levels=1,.target=1,.format=0x25,.flags=0x40,
            .swizzle_r=2,.swizzle_g=3,.swizzle_b=4,.swizzle_a=5};
        nvn_tex_layout(&t.d,&t.L);
        size_t size=(t.L.size+getpagesize()-1)&~((size_t)getpagesize()-1);
        void *mem=mmap(NULL,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
        if(mem==MAP_FAILED)MDIE("writeback pool fixture mmap");
        memset(mem,17,size);pool_t pool={.mem=mem,.size=size,.flags=0x24};t.mem=mem;t.pool=&pool;
        mtl_pool_add(mem,size);nvn_dirty_add_range(mem,size);
        __weak id old=nil;
        void *scope=objc_autoreleasePoolPush();
        @autoreleasepool {
            mtl_sub_begin();mtl_exec((const uint8_t *)&end);
            memset(&R,0,sizeof R);R.has_rt=1;R.rt.n=1;R.rt.color[0]=&t;
            R.clear_c[0]=1;R.cc[0]=MTLClearColorMake(1,0,0,1);rp_begin();rp_end();
            old=((NvPkg *)serial_get(t.serial))->tex;
            mtl_sub_commit();mtl_finish();memset(&R,0,sizeof R);
        }
        if(!old || g_wb.n!=1)MDIE("writeback pool fixture did not retain rendering");
        mtl_tex_release(&t);
        mtl_sub_begin();mtl_exec((const uint8_t *)&end);mtl_sub_commit();mtl_finish();
        if(serial_ptr(t.serial) || !old || g_wb.n!=1)MDIE("writeback lost before pool destruction");
        // Check that the rendering is still valid after TextureFinalize.
        if(cycle==0) {
            mtl_pool_invalidate(&pool,0,size);
            uint8_t pixels[64*64*4];
            if(nvn_deswizzle(&t.d,0,0,mem,pixels,64*4))MDIE("writeback probe deswizzle");
            for(int i=0;i<64*64;i++) {
                if(pixels[i*4]!=255||pixels[i*4+1]||pixels[i*4+2]||pixels[i*4+3]!=255)MDIE("writeback after finalize lost pixels");
                checked++;
            }
            if(g_wb.n)MDIE("writeback invalidate did not drain");
        }
        nvn_dirty_remove_range(mem);
        // Negative control models the old pool deletion path without modifying production policy.
        int negative=getenv("TOTK_PROBE_NEGATIVE") && atoi(getenv("TOTK_PROBE_NEGATIVE"))==23;
        nvn_hmap saved={0};
        if(negative){saved=g_wb;g_wb=(nvn_hmap){.klen=8};}
        mtl_pool_del(mem);
        if(negative)g_wb=saved;
        if(g_wb.n)MDIE("writeback retained destroyed pool");
        mtl_sub_begin();mtl_exec((const uint8_t *)&end);mtl_sub_commit();mtl_finish();
        defer_drain();res_commit();
        objc_autoreleasePoolPop(scope); // weak loads on the probe thread also autorelease
        // g_done is signalled before the completion handler drains its local
        // deferred-release batch. Wait for that callback tail, not just the GPU.
        int retained=1;
        for(int wait=0;wait<200 && retained;wait++) {
            @autoreleasepool { retained=old!=nil; }
            // Completion-tail destruction can enqueue residency removal after
            // the first res_commit. Flush that removal before the next weak check.
            if(retained){res_commit();usleep(1000);}
        }
        if(retained)MDIE("writeback texture retained after retirement fences cycle%d",cycle);
        munmap(mem,size);checked+=3;
    }
    MLOG("[writeback-pool-probe] cycles=32 pool-retired=%llu",(unsigned long long)g_wb_pool_dropped);
    return checked;
}
int mtl_probe_pool_retire(void) {
    if (getenv("TOTK_PROBE_WB_POOL")) return probe_writeback_pool_retire();
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
