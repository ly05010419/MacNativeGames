/* Included beside the texture-retirement probe. Exercise the production mip
 * feedback selector/cache, its real overflow boundary, and deferred GPU lifetime.
 * A negative-control build omits the defer_obj_release call in cache flush. */
#ifndef NVN_MIP_FEEDBACK_PROBE_H
#define NVN_MIP_FEEDBACK_PROBE_H

static void mip_feedback_probe_sample(id<MTLTexture> view, id<MTLComputePipelineState> pipeline,
                                      id<MTLBuffer> output) {
    // Every prior sampled submission is complete before this output is reused.
    // Require this dispatch to write pixels and metadata, rather than accept stale bytes.
    memset(output.contents, 0xa5, output.length);
    id<MTL4ComputeCommandEncoder> encoder = enc_c_begin(NULL);
    enc_c_pso(encoder, pipeline);
    [g_ct setTexture:view.gpuResourceID atIndex:0];
    enc_c_args(encoder, output.gpuAddress, 0);
    [encoder dispatchThreads:MTLSizeMake(16, 1, 1) threadsPerThreadgroup:MTLSizeMake(16, 1, 1)];
    enc_c_end(encoder);
}

static int probe_mip_feedback_retire(void) {
    if (g_sub_serial) MDIE("mip feedback probe: requires a fresh process");
    setenv("OOT_STOR_ALIAS", "2", 1);   // small, separate production exact heaps; no region-growth experiment
    mtl_init();
    if (stor_alias_mode() != 2) MDIE("mip feedback probe: alias policy initialized before fixture");
    NSError *error = nil;
    id<MTLLibrary> library = [g_dev newLibraryWithSource:
        @"#include <metal_stdlib>\nusing namespace metal;\n"
         "kernel void sample_feedback(texture2d<float, access::read> image [[texture(0)]], "
         "device uchar4 *out [[buffer(0)]], uint i [[thread_position_in_grid]]) { "
         "out[i]=uchar4(round(image.read(uint2(i%4,i/4))*255.0f)); "
         "if(i==0) out[16]=uchar4(image.get_num_mip_levels(),0,0,0); }\n"
        options:nil error:&error];
    id<MTLComputePipelineState> pipeline = library ? [g_dev newComputePipelineStateWithFunction:
        [library newFunctionWithName:@"sample_feedback"] error:&error] : nil;
    if (!pipeline) MDIE("mip feedback probe: sample pipeline %s", error.localizedDescription.UTF8String);
    id<MTLBuffer> output = probe_buf(256), upload = probe_buf(4096);
    NvShader *feedback_shader = [NvShader new];   // upstream selector: no combined sampler pins a mip
    const uint8_t color[4] = {33, 67, 101, 255};
    memset(upload.contents, 0, upload.length);
    for (unsigned y = 0; y < 8; y++) for (unsigned x = 0; x < 8; x++)
        memcpy((uint8_t *)upload.contents + y * 256 + x * 4, color, 4);
    memset((uint8_t *)upload.contents + 2048, 0xff, 1024);   // attached mip differs from the sampled mip
    enum { COUNT = 514 };   // the existing >512 rule flushes 513 entries on this request
    size_t bytes = COUNT * 4096u;
    void *memory = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (memory == MAP_FAILED) MDIE("mip feedback probe: guest-address fixture");
    op_hdr end = {OP_END, 8}; int checked = 0;
    for (int heap = 0; heap < 2; heap++) {
        NSHashTable *roots = [NSHashTable weakObjectsHashTable], *views = [NSHashTable weakObjectsHashTable];
        NSHashTable *heaps = [NSHashTable weakObjectsHashTable];
        __weak id<MTLTexture> first_root = nil, first_view = nil;
        __weak id<MTLHeap> first_heap = nil;
        uintptr_t first_root_id = 0, first_view_id = 0, last_root_id = 0, last_view_id = 0;
        id<MTLSharedEvent> hold = [g_dev newSharedEvent];
        if (!hold) MDIE("mip feedback probe: GPU reader event");
        @autoreleasepool {
            mtl_sub_begin(); mtl_exec((const uint8_t *)&end);
            p_set_rt saved_rt = R.rt; int saved_has_rt = R.has_rt;
            for (unsigned i = 0; i < COUNT; i++) @autoreleasepool {
                MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
                    MTLPixelFormatRGBA8Unorm width:8 height:8 mipmapped:YES];
                d.mipmapLevelCount = 2; d.storageMode = heap ? MTLStorageModePrivate : MTLStorageModeShared;
                d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget | MTLTextureUsagePixelFormatView;
                tex_t texture = {0}; texture.serial = 0xfffc0000u + heap * COUNT + i;
                texture.mem = (uint8_t *)memory + i * 4096u; texture.L.size = 4096;
                id<MTLTexture> root = heap ? stor_alias_texture(&texture, d) : [g_dev newTextureWithDescriptor:d];
                if (!root || (!!root.heap != heap)) MDIE("mip feedback probe: texture allocation kind%d", heap);
                res_add(root);
                memset(&R.rt, 0, sizeof R.rt); R.rt.n = 1; R.rt.color[0] = &texture;
                R.rt.has_cview[0] = 1; R.rt.cview[0] = (view_t){.set_levels = 1, .base_level = 1, .nlevels = 1};
                R.has_rt = 1;
                id<MTLTexture> view = mip_feedback_view(root, texture.serial, feedback_shader, 0, 0);
                if (!view || view.parentTexture != root || view.mipmapLevelCount != 1)
                    MDIE("mip feedback probe: production selector did not exclude the attached mip");
                [roots addObject:root]; [views addObject:view];
                if (!i) { first_root_id = obj_key(root); first_view_id = obj_key(view); }
                if (i == COUNT - 1) { last_root_id = obj_key(root); last_view_id = obj_key(view); }
                if (root.heap) [heaps addObject:root.heap];
                if (!i) {
                    if (mip_feedback_view(root, texture.serial, feedback_shader, 0, 0) != view) MDIE("mip feedback probe: cache miss on identical view");
                    checked++; first_root = root; first_view = view; first_heap = root.heap;
                    enc_b_copy_buffer_texture(blit(), upload, 0, 256, 2048, MTLSizeMake(8, 8, 1), root, 0, 0, MTLOriginMake(0, 0, 0));
                    enc_b_copy_buffer_texture(blit(), upload, 2048, 256, 1024, MTLSizeMake(4, 4, 1), root, 0, 1, MTLOriginMake(0, 0, 0));
                    mip_feedback_probe_sample(view, pipeline, output);
                }
                defer_obj_release(root);   // the cache view is now the parent's remaining CPU owner
            }
            R.rt = saved_rt; R.has_rt = saved_has_rt;
            [g_q4 waitForEvent:hold value:1];
            uint64_t fence = mtl_sub_commit();
            const char *failure = NULL;
            if (!first_root || !first_view) failure = "overflow released an in-flight view/parent";
            else if (__atomic_load_n(&g_done, __ATOMIC_ACQUIRE) >= fence) failure = "reader was not held pending";
            else if (g_mip_feedback_cache.count != 1) failure = "fixture did not cross the production overflow boundary";
            else {
                pthread_mutex_lock(&g_res_mu);
                int registered = [g_res_objects containsObject:first_view];
                int heap_registered = first_heap && [g_res_objects containsObject:first_heap];
                pthread_mutex_unlock(&g_res_mu);
                if (registered == heap) failure = "pending view residency did not match its allocation kind";
                else if (heap && !heap_registered) failure = "pending reader's alias heap left residency";
            }
            hold.signaledValue = 1;   // every post-submit assertion path releases the GPU gate before aborting
            if (failure) MDIE("mip feedback probe: %s", failure);
            checked += 4; mtl_finish();
        }
        for (unsigned i = 0; i < 64; i++) {
            if (((uint8_t *)output.contents)[i] != color[i % 4]) MDIE("mip feedback probe: queued reader pixel%u", i);
            checked++;
        }
        if (((uint8_t *)output.contents)[64] != 1) MDIE("mip feedback probe: sampled view exposes the attached mip");
        checked++;
        mip_feedback_cache_flush();   // deterministic probe teardown also retires the last cached entry
        for (unsigned i = 0; i < 3; i++) @autoreleasepool {
            mtl_sub_begin(); mtl_exec((const uint8_t *)&end); mtl_sub_commit(); mtl_finish();
        }
        NSUInteger alive_roots, alive_views, alive_heaps;
        uint64_t deadline = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + 1000000000ull;
        do {
            @autoreleasepool {
                defer_drain(); res_commit();
                alive_roots = roots.allObjects.count; alive_views = views.allObjects.count;
                alive_heaps = heaps.allObjects.count;
            }
            if (!alive_roots && !alive_views && !alive_heaps) break;
            usleep(1000);
        } while (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) < deadline);
        if (alive_roots || alive_views || alive_heaps) {
            MLOG("mip feedback probe: retirement diagnostic kind%d cache%lu done%llu fence%llu first=%p/%p last=%p/%p",
                heap, (unsigned long)g_mip_feedback_cache.count, (unsigned long long)g_done, (unsigned long long)g_fence,
                (void *)first_root_id, (void *)first_view_id, (void *)last_root_id, (void *)last_view_id);
            for (id<MTLTexture> view in views.allObjects) {
                pthread_mutex_lock(&g_res_mu); int resident = [g_res_objects containsObject:view]; pthread_mutex_unlock(&g_res_mu);
                MLOG("mip feedback probe: remaining view %p parent%p resident%d first%d last%d",
                    (__bridge void *)view, (__bridge void *)view.parentTexture, resident,
                    obj_key(view) == first_view_id, obj_key(view) == last_view_id);
            }
            for (id<MTLTexture> root in roots.allObjects) {
                pthread_mutex_lock(&g_res_mu); int resident = [g_res_objects containsObject:root]; pthread_mutex_unlock(&g_res_mu);
                MLOG("mip feedback probe: remaining root %p resident%d first%d last%d",
                    (__bridge void *)root, resident, obj_key(root) == first_root_id, obj_key(root) == last_root_id);
            }
            MDIE("mip feedback probe: retired kind%d retained %lu parents / %lu views / %lu heaps",
                heap, (unsigned long)alive_roots, (unsigned long)alive_views, (unsigned long)alive_heaps);
        }
        checked += COUNT * (heap ? 3 : 2);
    }
    // A retired view must not remove residency for a still-live parent or sibling.
    // The overflow cases above retire every parent; this case keeps both owners
    // registered across the actual view retirement and reads them in later work.
    id<MTLTexture> parent = nil, sibling = nil;
    __weak id<MTLTexture> retired_view = nil, parent_witness = nil, sibling_witness = nil;
    @autoreleasepool {
        mtl_sub_begin(); mtl_exec((const uint8_t *)&end);
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
            MTLPixelFormatRGBA8Unorm width:8 height:8 mipmapped:YES];
        d.mipmapLevelCount = 2; d.storageMode = MTLStorageModeShared;
        d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget | MTLTextureUsagePixelFormatView;
        parent = [g_dev newTextureWithDescriptor:d];
        sibling = [parent newTextureViewWithPixelFormat:parent.pixelFormat textureType:MTLTextureType2D
            levels:NSMakeRange(0, 2) slices:NSMakeRange(0, 1)];
        if (!parent || !sibling) MDIE("mip feedback probe: surviving parent/sibling allocation");
        res_add(parent); res_add(sibling); parent_witness = parent; sibling_witness = sibling;
        res_commit();
        pthread_mutex_lock(&g_res_mu);
        int initial_parent = [g_res containsAllocation:parent], initial_sibling = [g_res containsAllocation:sibling];
        pthread_mutex_unlock(&g_res_mu);
        if (!initial_parent || !initial_sibling) MDIE("mip feedback probe: initial parent/sibling Metal membership");
        checked += 2;
        tex_t texture = {0}; texture.serial = 0xfffb0000u;
        p_set_rt saved_rt = R.rt; int saved_has_rt = R.has_rt;
        memset(&R.rt, 0, sizeof R.rt); R.rt.n = 1; R.rt.color[0] = &texture;
        R.rt.has_cview[0] = 1; R.rt.cview[0] = (view_t){.set_levels = 1, .base_level = 1, .nlevels = 1};
        R.has_rt = 1;
        id<MTLTexture> view = mip_feedback_view(parent, texture.serial, feedback_shader, 0, 0);
        if (!view || view.mipmapLevelCount != 1) MDIE("mip feedback probe: surviving-parent feedback selector");
        retired_view = view;
        enc_b_copy_buffer_texture(blit(), upload, 0, 256, 2048, MTLSizeMake(8, 8, 1), parent, 0, 0, MTLOriginMake(0, 0, 0));
        enc_b_copy_buffer_texture(blit(), upload, 2048, 256, 1024, MTLSizeMake(4, 4, 1), parent, 0, 1, MTLOriginMake(0, 0, 0));
        mip_feedback_probe_sample(view, pipeline, output);
        mip_feedback_cache_flush();
        R.rt = saved_rt; R.has_rt = saved_has_rt;
        mtl_sub_commit(); mtl_finish();
    }
    for (unsigned i = 0; i < 2; i++) @autoreleasepool {
        mtl_sub_begin(); mtl_exec((const uint8_t *)&end); mtl_sub_commit(); mtl_finish();
    }
    int view_alive; uint64_t deadline = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + 1000000000ull;
    do {
        @autoreleasepool { defer_drain(); res_commit(); view_alive = retired_view != nil; }
        if (!view_alive) break;
        usleep(1000);
    } while (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) < deadline);
    if (view_alive) MDIE("mip feedback probe: feedback view retained before surviving-parent read");
    checked++;
    pthread_mutex_lock(&g_res_mu);
    int parent_resident = [g_res containsAllocation:parent], sibling_resident = [g_res containsAllocation:sibling];
    pthread_mutex_unlock(&g_res_mu);
    if (!parent_resident || !sibling_resident)
        MDIE("mip feedback probe: retiring view removed surviving parent/sibling Metal residency");
    checked += 2;
    for (unsigned source = 0; source < 2; source++) {
        @autoreleasepool {
            mtl_sub_begin(); mtl_exec((const uint8_t *)&end);
            mip_feedback_probe_sample(source ? sibling : parent, pipeline, output);
            mtl_sub_commit(); mtl_finish();
        }
        for (unsigned i = 0; i < 64; i++) {
            if (((uint8_t *)output.contents)[i] != color[i % 4])
                MDIE("mip feedback probe: surviving source%u pixel%u after view retirement", source, i);
            checked++;
        }
        if (((uint8_t *)output.contents)[64] != 2)
            MDIE("mip feedback probe: surviving source%u lost its original mip range", source);
        checked++;
    }
    @autoreleasepool { defer_obj_release(sibling); defer_obj_release(parent); sibling = nil; parent = nil; }
    for (unsigned i = 0; i < 2; i++) @autoreleasepool {
        mtl_sub_begin(); mtl_exec((const uint8_t *)&end); mtl_sub_commit(); mtl_finish();
    }
    int owners_alive; deadline = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + 1000000000ull;
    do {
        @autoreleasepool { defer_drain(); res_commit(); owners_alive = parent_witness != nil || sibling_witness != nil; }
        if (!owners_alive) break;
        usleep(1000);
    } while (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) < deadline);
    if (owners_alive) MDIE("mip feedback probe: surviving-parent fixture owners retained after final release");
    checked += 2;
    munmap(memory, bytes); defer_obj_release(output); defer_obj_release(upload);
    MLOG("mip feedback probe: overflow, pending reader, mip pixels and parent retirement; %d checks", checked);
    return checked;
}
#endif
