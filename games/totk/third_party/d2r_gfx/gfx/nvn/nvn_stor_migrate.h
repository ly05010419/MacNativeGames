/* Growth migration (on by default with the alias heaps, OOT_STOR_MIGRATE=0 off; nvn_stor_alias.h stor_alias_migrate_on).
   The new heap has not been used by its first caller
   yet. Copy retired generations oldest first, then replace their storage
   owners; existing command buffers keep the original objects until their fence.
   Included after stor_publish, with no alias mutex held. */
static void stor_alias_migrate_to(stor_alias_heap *destination) {
    if (!stor_alias_migrate_on()) return;
    if (!on_sub_thread() || rp_active() || !g_cb) { g_alias_migration_skipped++; return; }
    registry_lock();
    NSMutableArray *rows = [NSMutableArray array];
    pthread_mutex_lock(&g_smu);
    for (int detached = 0; detached < 2; detached++) {
        uint32_t count = detached ? (uint32_t)g_stor_ndet : g_by_storage.cap;
        for (uint32_t i = 0; i < count; i++) {
            stor_ent *entry = detached ? g_stor_det[i] : g_by_storage.vals[i];
            /* Entries no serial holds move too (2026-11-17): they keep their content for STOR_IDLE_SUBMITS so a
               texture re-created with the same layout finds it, and while they stayed behind they kept the earlier
               generation alive. Entering the world in Ocarina of Time, 225-1,127 of them held 1.5 GB of earlier
               heaps for a few hundred frames, the memory peak. Detached ones without a holder are on their way out. */
            if (!entry || !entry->tex || (detached && !entry->nser)) continue;
            id<MTLTexture> base = (__bridge id)entry->tex;
            /* A serial owning a distinct format view needs its own rebinding
               transaction. Keep those entries intact until that path is tested. */
            int plain = 1;
            for (int s=0;s<entry->nser;s++)
                if (hm_get_id(&g_by_serial,&entry->ser[s]) != base) { plain=0; break; }
            if (!plain) continue;
            NvAliasTrack *track = objc_getAssociatedObject(base, &g_alias_track_key);
            stor_alias_heap *old = track ? track->h : NULL;
            if (!old || old == destination || old->current || old->key.pf != destination->key.pf ||
                old->key.samples != destination->key.samples || base.sampleCount != 1 ||
                (base.textureType != MTLTextureType2D && base.textureType != MTLTextureType2DArray) ||
                base.pixelFormat == MTLPixelFormatDepth32Float_Stencil8 ||
                old->lo < destination->lo || old->size > destination->size ||
                old->lo - destination->lo > destination->size - old->size) continue;
            [rows addObject:@{ @"base":base, @"generation":@(old->generation),
                @"offset":@(old->lo - destination->lo + base.heapOffset) }];
        }
    }
    pthread_mutex_unlock(&g_smu);
    [rows sortUsingComparator:^NSComparisonResult(NSDictionary *a, NSDictionary *b) {
        return [a[@"generation"] compare:b[@"generation"]];
    }];
    for (NSDictionary *row in rows) { @autoreleasepool {
        id<MTLTexture> base = row[@"base"];
        MTLTextureDescriptor *d = [MTLTextureDescriptor new];
        d.textureType=base.textureType; d.pixelFormat=base.pixelFormat;
        d.width=base.width; d.height=base.height; d.depth=base.depth;
        d.arrayLength=base.arrayLength; d.mipmapLevelCount=base.mipmapLevelCount; d.sampleCount=base.sampleCount;
        d.storageMode=base.storageMode; d.cpuCacheMode=base.cpuCacheMode;
        d.hazardTrackingMode=base.hazardTrackingMode; d.usage=base.usage; d.swizzle=base.swizzle;
        d.allowGPUOptimizedContents=base.allowGPUOptimizedContents;
        NSUInteger offset=[row[@"offset"] unsignedLongLongValue];
        MTLSizeAndAlign sa=[g_dev heapTextureSizeAndAlignWithDescriptor:d];
        if (!sa.align || offset%sa.align || offset>destination->size || sa.size>destination->size-offset) continue;
        pthread_mutex_lock(&g_alias_mu);
        id<MTLTexture> next=stor_alias_place_locked(destination,d,offset);
        pthread_mutex_unlock(&g_alias_mu);
        if (!next) continue;
        id<MTL4ComputeCommandEncoder> encoder=blit();
        NSUInteger slices=base.textureType==MTLTextureType3D?1:base.arrayLength*
            ((base.textureType==MTLTextureTypeCube||base.textureType==MTLTextureTypeCubeArray)?6:1);
        for (NSUInteger level=0;level<base.mipmapLevelCount;level++) {
            MTLSize size=MTLSizeMake(MAX(base.width>>level,1u),MAX(base.height>>level,1u),
                base.textureType==MTLTextureType3D?MAX(base.depth>>level,1u):1);
            for (NSUInteger slice=0;slice<slices;slice++)
                enc_b_copy_texture(encoder,base,slice,level,MTLOriginMake(0,0,0),size,next,slice,level,MTLOriginMake(0,0,0));
        }
        uint64_t key=obj_key(base);
        pthread_mutex_lock(&g_smu);
        stor_ent *entry=nvn_hm_get(&g_stor_of_obj,&key);
        if (!entry || entry->tex!=(__bridge void *)base) {
            pthread_mutex_unlock(&g_smu); defer_obj_release(next); continue;
        }
        NSMutableArray *serials=[NSMutableArray arrayWithCapacity:(NSUInteger)entry->nser];
        for (int i=0;i<entry->nser;i++) [serials addObject:@(entry->ser[i])];
        stor_release_holders_locked(entry,"heap-growth");
        nvn_hm_del(&g_stor_of_obj,&key);
        defer_obj_release(base); CFRelease(entry->tex);
        entry->tex=(void *)CFBridgingRetain(next);
        key=obj_key(next); nvn_hm_put(&g_stor_of_obj,&key,entry);
        /* The game still owns these descriptors. Rebind them now: leaving
           nser at zero would let the idle sweep discard their valid contents. */
        for (NSNumber *number in serials) {
            uint64_t serial=number.unsignedLongLongValue;
            hm_put_id(&g_by_serial,&serial,next); serial_resource_ref(next,1);
            stor_ref_locked(next,serial); ser_bump_locked(serial);
        }
        pthread_mutex_unlock(&g_smu);
        for (NSNumber *serial in serials) treg_drop_serial(serial.unsignedLongLongValue);
        g_alias_migrated++;
    } }
    registry_unlock();
    if (rows.count && (g_alias_migrated < 16 || g_alias_migrated % 100 < rows.count))
        MLOG("[heap-migrate] generation %llu: %lu candidates, %llu moved total, %llu unsafe boundaries kept",
            (unsigned long long)destination->generation, (unsigned long)rows.count,
            (unsigned long long)g_alias_migrated, (unsigned long long)g_alias_migration_skipped);
}
