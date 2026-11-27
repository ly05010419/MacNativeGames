/* Included by nvn_mtl.m; standalone synthetic data, never a game profile.
   Force the existing copy-budget mode at small scale, not a multi-GiB OOM.
   TOTK_PROBE_EVICT_TOTAL=1 instead exercises the default Mac total-allocation
   policy with a lowered 256 MiB budget and bounded padding.
   Negative control: TOTK_PROBE_NEGATIVE=1 disables eviction and must fail. */
int mtl_probe_texture_evict(void) {
    const char *ne = getenv("TOTK_PROBE_NEGATIVE");
    setenv("TOTK_TEX_EVICT", ne && atoi(ne) == 1 ? "0" : "1", 1);
    int total = getenv("TOTK_PROBE_EVICT_TOTAL") && atoi(getenv("TOTK_PROBE_EVICT_TOTAL"));
    if(total) {
        unsetenv("TOTK_TEX_EVICT_MB");
        setenv("TOTK_GPU_BUDGET_MB", "256", 1);
    } else setenv("TOTK_TEX_EVICT_MB", "1", 1);
    setenv("TOTK_GPU_KEEPALIVE", "0", 1);
    setenv("TOTK_PKG_HASH", "1", 1);
    setenv("TOTK_TEX_MIRROR", "1", 1);
    mtl_init();
    enum { N=4, W=1024, H=1024, BYTES=W*H*4 };
    tex_t t[N]; pool_t pool[N]; void *mem[N];
    id __weak old = nil;
    int checked=0; op_hdr end={OP_END,8};
    id<MTLBuffer> rb=probe_buf(BYTES);
    memset(t,0,sizeof t); memset(pool,0,sizeof pool);
    for (int i=0;i<N;i++) {
        int dim=i==3?16:W;
        t[i].serial=0xffee0000+i;
        t[i].d=(nvn_texdesc){.width=dim,.height=dim,.depth=1,.levels=1,.target=1,.format=0x25,.flags=0x40,
            .swizzle_r=2,.swizzle_g=3,.swizzle_b=4,.swizzle_a=5};
        nvn_tex_layout(&t[i].d,&t[i].L);
        size_t size=(t[i].L.size+getpagesize()-1)&~((size_t)getpagesize()-1);
        mem[i]=mmap(NULL,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
        if(mem[i]==MAP_FAILED)MDIE("texture evict probe mmap");
        memset(mem[i],31+i*17,size);
        pool[i]=(pool_t){.mem=mem[i],.size=size,.flags=0x24};
        t[i].mem=mem[i];t[i].pool=&pool[i];
        mtl_pool_add(mem[i],size);nvn_dirty_add_range(mem[i],size);
    }
    mtl_sub_begin();mtl_exec((const uint8_t *)&end);
    for(int i=0;i<N;i++) {
        mtl_reg_tex(1+i,&t[i],NULL);
        registry_lock();id<MTLTexture> tex=tex_by_id(1+i);registry_unlock();
        if(!tex)MDIE("texture evict missing fixture %d",i);
    }
    /* Produce actual GPU-owned content, distinct from its stale CPU bytes. */
    memset(&R,0,sizeof R);R.has_rt=1;R.rt.n=1;R.rt.color[0]=&t[2];
    R.clear_c[0]=1;R.cc[0]=MTLClearColorMake(1,0,0,1);rp_begin();rp_end();
    mtl_sub_commit();mtl_finish();memset(&R,0,sizeof R);
    id<MTLBuffer> pressure=nil;
    if(total) {
        uint64_t budget=gpu_budget_bytes();
        if(budget!=(UINT64_C(256)<<20))MDIE("texture evict: total-budget fixture unavailable");
        for(int frame=0;frame<100;frame++) {
            mtl_sub_begin();mtl_exec((const uint8_t *)&end);
            registry_lock();(void)tex_by_id(2);registry_unlock();
            mtl_sub_commit();mtl_finish();
        }
        uint64_t allocated=g_dev.currentAllocatedSize;
        if(allocated>=budget*7/10 || g_pkg_evicted)MDIE("texture evict: unexpected baseline pressure or premature eviction");
        NvPkg *cold=(__bridge NvPkg *)serial_ptr(t[0].serial);
        if(!cold->tex)MDIE("texture evict: cold texture lost below threshold");
        checked+=2;
        // Real Metal allocation, not a substituted currentAllocatedSize value.
        pressure=[g_dev newBufferWithLength:(NSUInteger)(budget*85/100-allocated) options:MTLResourceStorageModePrivate];
        if(!pressure)MDIE("texture evict: bounded pressure allocation failed");
        res_add(pressure);
        allocated=g_dev.currentAllocatedSize;
        if(allocated<=budget*8/10 || allocated>=budget*95/100)MDIE("texture evict: pressure outside safe fixture window");
        MLOG("texture evict total-pressure fixture: allocated %llu KiB / budget %llu KiB",
             (unsigned long long)(allocated>>10),(unsigned long long)(budget>>10));
        checked++;
    }
    for(int cycle=0;cycle<3;cycle++) {
        @autoreleasepool {
            NvPkg *cold=(__bridge NvPkg *)serial_ptr(t[0].serial);
            if(!cold->tex || cold->owned || cold->hashed)MDIE("texture evict cold fixture");
            old=cold->tex;
        }
        uint64_t before=g_pkg_evicted;
        /* Keep one descriptor hot through the fast path. Owned and hashed
           textures remain idle but must survive exactly the same sweeps. */
        for(int frame=0;frame<100;frame++) {
            mtl_sub_begin();mtl_exec((const uint8_t *)&end);
            registry_lock();(void)tex_by_id(2);registry_unlock();
            mtl_sub_commit();mtl_finish();
        }
        defer_drain();res_commit();
        @autoreleasepool {
            NvPkg *cold=(__bridge NvPkg *)serial_ptr(t[0].serial);
            if(cold->tex || g_pkg_evicted!=before+1)MDIE("texture evict: idle copy not uniquely evicted cycle%d",cycle);
            if(old)MDIE("texture evict: dropped copy still retained after fences cycle%d",cycle);
            registry_lock();int stale=g_treg[1].view!=nil;registry_unlock();
            if(stale)MDIE("texture evict: stale descriptor");
            for(int i=1;i<N;i++) {
                NvPkg *pk=(__bridge NvPkg *)serial_ptr(t[i].serial);
                if(!pk->tex || (i==2 && !pk->owned) || (i==3 && !pk->hashed))MDIE("texture evict protected fixture %d",i);
                checked++;
            }
            checked+=3;
        }
        uint64_t restored=g_pkg_restored;
        mtl_sub_begin();mtl_exec((const uint8_t *)&end);
        registry_lock();id<MTLTexture> fresh=tex_by_id(1);registry_unlock();
        if(!fresh || g_pkg_restored!=restored+1)MDIE("texture evict: no reupload");
        enc_b_copy_texture_buffer(blit(),fresh,0,0,MTLOriginMake(0,0,0),MTLSizeMake(W,H,1),rb,0,W*4,BYTES);
        mtl_sub_commit();mtl_finish();fresh=nil;
        for(int i=0;i<BYTES;i++){if(((uint8_t *)rb.contents)[i]!=31)MDIE("texture evict reupload cycle%d byte%d",cycle,i);checked++;}
        mtl_sub_begin();mtl_exec((const uint8_t *)&end);
        id<MTLTexture> owned=tex_mtl(&t[2]);
        enc_b_copy_texture_buffer(blit(),owned,0,0,MTLOriginMake(0,0,0),MTLSizeMake(W,H,1),rb,0,W*4,BYTES);
        mtl_sub_commit();mtl_finish();owned=nil;
        for(int i=0;i<BYTES;i++){uint8_t want=(i%4==0||i%4==3)?255:0;if(((uint8_t *)rb.contents)[i]!=want)MDIE("texture evict GPU-owned byte%d",i);checked++;}
    }
    if(total) {
        @autoreleasepool { res_remove(pressure); pressure=nil; res_commit(); }
        if(g_dev.currentAllocatedSize>=gpu_budget_bytes()*8/10)MDIE("texture evict: pressure allocation not released");
        uint64_t before=g_pkg_evicted;
        for(int frame=0;frame<100;frame++) {
            mtl_sub_begin();mtl_exec((const uint8_t *)&end);
            mtl_sub_commit();mtl_finish();
        }
        NvPkg *cold=(__bridge NvPkg *)serial_ptr(t[0].serial);
        if(g_pkg_evicted!=before || !cold->tex)MDIE("texture evict: eviction continued after pressure removed");
        checked+=2;
        MLOG("texture evict total-pressure fixture: no eviction below threshold before/after pressure");
    }
    /* Finalization must retain descriptor-named storage; remove the slots first. */
    for(int i=0;i<N;i++){mtl_reg_tex(1+i,NULL,NULL);mtl_tex_release(&t[i]);mtl_pool_del(mem[i]);nvn_dirty_remove_range(mem[i]);}
    for(int i=0;i<3;i++){mtl_sub_begin();mtl_exec((const uint8_t *)&end);mtl_sub_commit();mtl_finish();}
    defer_drain();res_remove(rb);res_commit();
    for(int i=0;i<N;i++)munmap(mem[i],pool[i].size);
    return checked;
}
