/* Direct production eviction regression. Fresh process only: a 1 MiB budget, real idle submissions,
   cached descriptor uses, GPU-reader fences, and CPU-backed restoration through the descriptor path.
   OOT_PKG_BUDGET_PROBE_NEGATIVE=1 disables eviction; =2 omits the hot descriptor's later uses.
   Both controls must fail behavior assertions. This header changes no production policy. */
#ifndef NVN_PKG_BUDGET_PROBE_H
#define NVN_PKG_BUDGET_PROBE_H

static void pkg_budget_probe_sample(id<MTLTexture> texture, id<MTLComputePipelineState> pipeline,
                                    id<MTLBuffer> output, NSUInteger offset) {
    if (!texture) MDIE("package budget probe: missing sample texture");
    id<MTL4ComputeCommandEncoder> encoder = enc_c_begin(NULL);
    enc_c_pso(encoder, pipeline);
    [g_ct setTexture:texture.gpuResourceID atIndex:0];
    enc_c_args(encoder, output.gpuAddress + offset, 0);
    [encoder dispatchThreads:MTLSizeMake(16, 1, 1) threadsPerThreadgroup:MTLSizeMake(16, 1, 1)];
    enc_c_end(encoder);
}
static id<MTLTexture> pkg_budget_probe_resolve(int tid) {
    registry_lock(); id<MTLTexture> texture = tex_by_id(tid); registry_unlock();
    return texture;
}
static int pkg_budget_probe_pixels(id<MTLBuffer> output, NSUInteger offset, const uint8_t expected[4], const char *label) {
    const uint8_t *pixels = (const uint8_t *)output.contents + offset;
    for (unsigned i = 0; i < 64; i++)
        if (pixels[i] != expected[i % 4]) MDIE("package budget probe: %s byte%u got%u expected%u", label, i, pixels[i], expected[i % 4]);
    return 64;
}

int mtl_probe_pkg_budget(void) {
    if (g_sub_serial) MDIE("package budget probe: requires a fresh process before any submission");
    int negative = getenv("OOT_PKG_BUDGET_PROBE_NEGATIVE") ? atoi(getenv("OOT_PKG_BUDGET_PROBE_NEGATIVE")) : 0;
    setenv("TOTK_TEX_EVICT_MB", "1", 1);
    setenv("TOTK_TEX_EVICT", negative == 1 ? "0" : "1", 1);
    setenv("TOTK_TEX_MIRROR", "1", 1);
    setenv("TOTK_PKG_HASH", "1", 1);
    setenv("TOTK_TEX_CHECK_ONCE", "0", 1);  // unchanged guest pages exercise descriptor fast-hit last-use merging
    setenv("TOTK_TEX_PURGE", "0", 1);       // isolate explicit eviction from iOS purgeability
    mtl_init();
    if (tex_check_once()) MDIE("package budget probe: descriptor mode was initialized before fixture configuration");
    enum { COLD_A, COLD_B, HOT, OWNED, HASHED, COUNT, FIRST_ID = 91 };
    tex_t textures[COUNT] = {0}; pool_t pools[COUNT] = {0}; void *memory[COUNT] = {0}; size_t sizes[COUNT] = {0};
    NvPkg *packages[COUNT] = {nil};
    __weak id<MTLTexture> retired[2] = {nil, nil};
    int checked = 0, saved_pool = S.tex_pool_n, saved_base = S.tex_base;
    S.tex_pool_n = 128; S.tex_base = 0;
    uint8_t expected[COUNT][4];
    for (int i = 0; i < COUNT; i++) {
        memset(expected[i], (i + 1) * 17, 4);
        tex_t *texture = &textures[i]; texture->serial = 0xfffd9000u + i;
        texture->d = (nvn_texdesc){.width = i == HASHED ? 4 : 512, .height = i == HASHED ? 4 : 512,
            .depth = 1, .levels = 1, .target = 1, .format = 0x25, .swizzle_r = 2, .swizzle_g = 3, .swizzle_b = 4, .swizzle_a = 5};
        nvn_tex_layout(&texture->d, &texture->L);
        sizes[i] = (texture->L.size + getpagesize() - 1) & ~((size_t)getpagesize() - 1);
        memory[i] = mmap(NULL, sizes[i], PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (memory[i] == MAP_FAILED) MDIE("package budget probe: pool allocation");
        memset(memory[i], expected[i][0], sizes[i]);
        pools[i] = (pool_t){.mem = memory[i], .size = sizes[i], .flags = 0x24};
        texture->mem = memory[i]; texture->pool = &pools[i];
        mtl_pool_add(memory[i], sizes[i]); nvn_dirty_add_range(memory[i], sizes[i]);
        view_t view = {.set_levels = 1, .nlevels = 1};
        mtl_reg_tex(FIRST_ID + i, texture, &view);
    }
    NSError *error = nil;
    id<MTLLibrary> library = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\nkernel void read_pixels(texture2d<float, access::read> image [[texture(0)]], device uchar4 *out [[buffer(0)]], uint i [[thread_position_in_grid]]) { out[i] = uchar4(round(image.read(uint2(i%4,i/4))*255.0f)); }\n" options:nil error:&error];
    id<MTLComputePipelineState> pipeline = library ? [g_dev newComputePipelineStateWithFunction:[library newFunctionWithName:@"read_pixels"] error:&error] : nil;
    if (!pipeline) MDIE("package budget probe: pipeline %s", error.localizedDescription.UTF8String);
    id<MTLBuffer> output = probe_buf(1024); op_hdr end = {OP_END, 8};
    @autoreleasepool {
        mtl_sub_begin(); mtl_exec((const uint8_t *)&end);
        for (int i = 0; i < COUNT; i++) {
            id<MTLTexture> texture = pkg_budget_probe_resolve(FIRST_ID + i);
            id holder = (__bridge id)serial_ptr(textures[i].serial);
            if (!texture || ![holder isKindOfClass:[NvPkg class]]) MDIE("package budget probe: CPU mirror fixture");
            packages[i] = holder;
            if (i < 2) retired[i] = packages[i]->tex;
        }
        // Render through the production target path: Metal contents now differ from the CPU source.
        memset(&R, 0, sizeof R); R.has_rt = 1; R.rt.n = 1; R.rt.color[0] = &textures[OWNED];
        R.clear_c[0] = 1; R.cc[0] = MTLClearColorMake(1, 0, 1, 1); rp_begin(); rp_end(); memset(&R, 0, sizeof R);
        const uint8_t rendered[4] = {255, 0, 255, 255}; memcpy(expected[OWNED], rendered, 4);
        for (int i = 0; i < COUNT; i++) pkg_budget_probe_sample(pkg_budget_probe_resolve(FIRST_ID + i), pipeline, output, i * 64);
        mtl_sub_commit(); mtl_finish();
    }
    for (int i = 0; i < COUNT; i++) checked += pkg_budget_probe_pixels(output, i * 64, expected[i], "initial upload/render");
    if (!packages[OWNED]->owned || !packages[HASHED]->hashed) MDIE("package budget probe: exclusion fixture not established");
    checked += 2;
    uint64_t evicted_before = g_pkg_evicted, restored_before = g_pkg_restored;
    // Hold the first eligible four-submission sweep until a real GPU reader has been queued.
    uint64_t sweep = (g_sub_serial + EVICT_IDLE + 3) & ~3ull;
    while (g_sub_serial + 1 < sweep) {
        @autoreleasepool {
            mtl_sub_begin(); mtl_exec((const uint8_t *)&end);
            if (negative != 2) pkg_budget_probe_sample(pkg_budget_probe_resolve(FIRST_ID + HOT), pipeline, output, HOT * 64);
            mtl_sub_commit(); mtl_finish();
        }
    }
    id<MTLSharedEvent> hold = [g_dev newSharedEvent];
    if (!hold) MDIE("package budget probe: GPU reader event allocation");
    uint64_t reader_fence;
    @autoreleasepool {
        mtl_sub_begin(); mtl_exec((const uint8_t *)&end);
        if (negative != 2) pkg_budget_probe_sample(pkg_budget_probe_resolve(FIRST_ID + HOT), pipeline, output, HOT * 64);
        // An already-selected GPU reader can outlive its CPU descriptor use. Do not resolve COLD_A:
        // it remains idle to the production LRU while its old Metal allocation is queued behind a GPU event.
        pkg_budget_probe_sample(packages[COLD_A]->tex, pipeline, output, COUNT * 64);
        [g_q4 waitForEvent:hold value:1]; reader_fence = mtl_sub_commit();
        const char *failure = NULL;
        if (packages[COLD_A]->tex || packages[COLD_B]->tex || g_pkg_evicted <= evicted_before)
            failure = "idle eligible copies were not evicted";
        else if (!packages[HOT]->tex) failure = "recently resolved descriptor was evicted";
        else if (!packages[OWNED]->tex || !packages[HASHED]->tex) failure = "owned/hashed copy was evicted";
        pthread_mutex_lock(&g_mu); uint64_t done = g_done; pthread_mutex_unlock(&g_mu);
        id<MTLTexture> old = retired[COLD_A];
        if (!failure && (done >= reader_fence || !old)) failure = "reader hold did not preserve the old allocation";
        pthread_mutex_lock(&g_res_mu); int resident = old && [g_res_objects containsObject:old]; pthread_mutex_unlock(&g_res_mu);
        if (!failure && !resident) failure = "old reader allocation lost residency before completion";
        // Negative controls fail here too: release the GPU queue before any assertion aborts the process.
        hold.signaledValue = 1;
        if (failure) MDIE("package budget probe: %s", failure);
        checked += 5;
        mtl_finish();
    }
    checked += pkg_budget_probe_pixels(output, COUNT * 64, expected[COLD_A], "reader submitted before retirement");
    // Cross the conservative retirement fence, then let callbacks release their temporary references.
    @autoreleasepool {
        mtl_sub_begin(); mtl_exec((const uint8_t *)&end);
        pkg_budget_probe_sample(pkg_budget_probe_resolve(FIRST_ID + HOT), pipeline, output, HOT * 64);
        mtl_sub_commit(); mtl_finish();
    }
    int old_alive; uint64_t deadline = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + 1000000000ull;
    do {
        @autoreleasepool { defer_drain(); res_commit(); old_alive = retired[0] != nil || retired[1] != nil; }
        if (!old_alive) break;
        usleep(1000);
    } while (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) < deadline);
    if (old_alive) MDIE("package budget probe: dropped copies retained after GPU completion");
    checked++;
    for (int i = 0; i < 2; i++) { memset(expected[i], 0xa7 + i * 17, 4); dm_guest_write_prepare(memory[i], sizes[i]); memset(memory[i], expected[i][0], sizes[i]); }
    @autoreleasepool {
        mtl_sub_begin(); mtl_exec((const uint8_t *)&end);
        for (int i = 0; i < COUNT; i++) pkg_budget_probe_sample(pkg_budget_probe_resolve(FIRST_ID + i), pipeline, output, i * 64);
        mtl_sub_commit(); mtl_finish();
    }
    if (g_pkg_restored < restored_before + 2) MDIE("package budget probe: descriptor return did not restore dropped copies");
    checked++;
    for (int i = 0; i < COUNT; i++) checked += pkg_budget_probe_pixels(output, i * 64, expected[i], "restored descriptor/current contents");
    for (int i = 0; i < COUNT; i++) { mtl_reg_tex(FIRST_ID + i, NULL, NULL); mtl_tex_release(&textures[i]); packages[i] = nil; }
    for (int i = 0; i < 2; i++) { mtl_sub_begin(); mtl_exec((const uint8_t *)&end); mtl_sub_commit(); mtl_finish(); }
    for (int i = 0; i < COUNT; i++) { mtl_pool_del(memory[i]); nvn_dirty_remove_range(memory[i]); munmap(memory[i], sizes[i]); }
    res_remove(output); S.tex_pool_n = saved_pool; S.tex_base = saved_base;
    MLOG("package budget probe: real evictions %llu, restore attempts %llu, %d checks", (unsigned long long)(g_pkg_evicted - evicted_before), (unsigned long long)(g_pkg_restored - restored_before), checked);
    return checked;
}
#endif
