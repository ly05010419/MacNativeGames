/* Existing blockrt probe: production cache replacement/finalization with a real
   held GPU reader. An isolated negative build omits defer_obj_release in
   blk_aux_retire_locked; the completed old allocation must then fail release. */
#ifndef NVN_BLOCK_AUX_PROBE_H
#define NVN_BLOCK_AUX_PROBE_H

static int block_aux_probe_resident(id<MTLTexture> texture) {
    pthread_mutex_lock(&g_res_mu);
    int resident = texture && [g_res_objects containsObject:texture] &&
                   [g_res containsAllocation:texture];
    pthread_mutex_unlock(&g_res_mu);
    return resident;
}

static void block_aux_probe_released(NSHashTable *witnesses, const char *phase) {
    NSUInteger alive;
    uint64_t deadline = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + 1000000000ull;
    do {
        @autoreleasepool {
            defer_drain(); res_commit();
            alive = witnesses.allObjects.count;
        }
        if (!alive) return;
        usleep(1000);
    } while (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) < deadline);
    MDIE("block auxiliary probe: %s retained %lu completed allocations", phase, (unsigned long)alive);
}

static int probe_block_aux_retire(void) {
    setenv("TOTK_GPU_BLOCKRT", "1", 1);
    enum { ID = 25000, OLD_BYTES = 512, NEW_BYTES = 1024 };
    tex_t texture = {0}; texture.serial = 0xfffb0000u;
    texture.d.format = NVN_FORMAT_RGTC1_UNORM; texture.d.target = 1;
    texture.d.width = texture.d.height = 32; texture.d.depth = texture.d.levels = 1;
    view_t view = {.set_format = 1, .format = NVN_FORMAT_RGBA16UI};
    id<MTLBuffer> upload = probe_buf(OLD_BYTES + NEW_BYTES);
    id<MTLBuffer> old_read = probe_buf(OLD_BYTES), new_read = probe_buf(NEW_BYTES);
    for (unsigned i = 0; i < OLD_BYTES + NEW_BYTES; i++)
        ((uint8_t *)upload.contents)[i] = (uint8_t)((i * 17 + 3) ^ (i >= OLD_BYTES ? 0x5b : 0));
    memset(old_read.contents, 0xa5, old_read.length);
    memset(new_read.contents, 0xa5, new_read.length);
    NSHashTable *old_witness = [NSHashTable weakObjectsHashTable];
    NSHashTable *new_witness = [NSHashTable weakObjectsHashTable];
    id<MTLSharedEvent> hold = [g_dev newSharedEvent];
    if (!hold) MDIE("block auxiliary probe: event allocation");
    pthread_mutex_lock(&g_smu);
    uint64_t initial_count = g_blkaux.count, initial_bytes = g_blkaux_bytes;
    uint64_t initial_created = g_blkaux_created, initial_retired = g_blkaux_retired;
    pthread_mutex_unlock(&g_smu);
    int checked = 0;
    op_hdr end = {OP_END, 8};
    @autoreleasepool {
        mtl_sub_begin(); mtl_exec((const uint8_t *)&end);
        MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
            MTLPixelFormatBC4_RUnorm width:32 height:32 mipmapped:NO];
        descriptor.storageMode = MTLStorageModePrivate; descriptor.usage = MTLTextureUsageShaderRead;
        id<MTLTexture> base = [g_dev newTextureWithDescriptor:descriptor];
        if (!base) MDIE("block auxiliary probe: base allocation");
        res_add(base); serial_put(texture.serial, base);
        blk_wb writeback;
        id<MTLTexture> old = blk_aux(&texture, &view, 1, &writeback);
        if (!old || old.width != 8 || old.height != 8) MDIE("block auxiliary probe: old production allocation");
        [old_witness addObject:old];
        enc_b_copy_buffer_texture(blit(), upload, 0, 64, OLD_BYTES, MTLSizeMake(8,8,1), old, 0, 0, MTLOriginMake(0,0,0));
        [g_blit barrierAfterEncoderStages:MTLStageBlit beforeEncoderStages:MTLStageBlit visibilityOptions:MTL4VisibilityOptionDevice];
        enc_b_copy_texture_buffer(blit(), old, 0, 0, MTLOriginMake(0,0,0), MTLSizeMake(8,8,1), old_read, 0, 64, OLD_BYTES);

        // Changing the actual serial's layout replaces its cached auxiliary.
        descriptor.width = texture.d.width = 64;
        base = [g_dev newTextureWithDescriptor:descriptor];
        if (!base) MDIE("block auxiliary probe: replacement base allocation");
        res_add(base); serial_put(texture.serial, base);
        id<MTLTexture> current = blk_aux(&texture, &view, 1, &writeback);
        if (!current || current == old || current.width != 16) MDIE("block auxiliary probe: production cache did not replace layout");
        [new_witness addObject:current];
        enc_b_copy_buffer_texture(blit(), upload, OLD_BYTES, 128, NEW_BYTES, MTLSizeMake(16,8,1), current, 0, 0, MTLOriginMake(0,0,0));
        mtl_reg_tex(ID, &texture, &view);
        mtl_tex_release(&texture);   // The real descriptor slot still names this finalized serial.
        pthread_mutex_lock(&g_smu);
        int cached = g_blkaux[@(texture.serial)] == current;
        pthread_mutex_unlock(&g_smu);
        if (!cached) MDIE("block auxiliary probe: finalized but registered contents were discarded");
        checked += 3;

        // All allocation/registration calls are above the gate. Every subsequent
        // assertion path signals it before aborting, including negative controls.
        [g_q4 waitForEvent:hold value:1];
        uint64_t fence = mtl_sub_commit();
        const char *failure = NULL;
        if (__atomic_load_n(&g_done, __ATOMIC_ACQUIRE) >= fence) failure = "reader was not held pending";
        else if (!block_aux_probe_resident(old)) failure = "replaced allocation left residency before its reader";
        else if (!block_aux_probe_resident(current)) failure = "registered current allocation lost residency";
        hold.signaledValue = 1;
        if (failure) MDIE("block auxiliary probe: %s", failure);
        checked += 3;
        mtl_finish();
    }
    for (unsigned i = 0; i < OLD_BYTES; i++) {
        if (((uint8_t *)old_read.contents)[i] != ((uint8_t *)upload.contents)[i])
            MDIE("block auxiliary probe: pending reader byte %u", i);
        checked++;
    }
    block_aux_probe_released(old_witness, "replaced layout");
    checked++;

    @autoreleasepool {
        pthread_mutex_lock(&g_smu);
        id<MTLTexture> current = g_blkaux[@(texture.serial)];
        pthread_mutex_unlock(&g_smu);
        if (!current || !block_aux_probe_resident(current)) MDIE("block auxiliary probe: registered auxiliary did not survive completion");
        mtl_sub_begin(); mtl_exec((const uint8_t *)&end);
        enc_b_copy_texture_buffer(blit(), current, 0, 0, MTLOriginMake(0,0,0), MTLSizeMake(16,8,1), new_read, 0, 128, NEW_BYTES);
        mtl_sub_commit(); mtl_finish();
    }
    for (unsigned i = 0; i < NEW_BYTES; i++) {
        if (((uint8_t *)new_read.contents)[i] != ((uint8_t *)upload.contents)[OLD_BYTES+i])
            MDIE("block auxiliary probe: preserved registered contents byte %u", i);
        checked++;
    }
    mtl_reg_tex(ID, NULL, NULL);   // Real unregistration now proves the finalized serial unreachable.
    for (int i = 0; i < 2; i++) @autoreleasepool {
        mtl_sub_begin(); mtl_exec((const uint8_t *)&end); mtl_sub_commit(); mtl_finish();
    }
    block_aux_probe_released(new_witness, "unregistered finalized serial");
    pthread_mutex_lock(&g_smu);
    int balanced = g_blkaux.count == initial_count && g_blkaux_bytes == initial_bytes &&
                   g_blkaux_created == initial_created + 2 && g_blkaux_retired == initial_retired + 2;
    pthread_mutex_unlock(&g_smu);
    if (!balanced) MDIE("block auxiliary probe: cache accounting did not return to baseline");
    checked += 2;
    for (id resource in @[upload, old_read, new_read]) defer_obj_release(resource);
    return checked;
}

#endif
