/* Growth migration (on by default with the alias heaps, OOT_STOR_MIGRATE=0 off; nvn_stor_alias.h stor_alias_migrate_on).
   The new heap has not been used by its first caller
   yet. Copy retired generations oldest first, then replace their storage
   owners; existing command buffers keep the original objects until their fence.
   Included after stor_publish, with no alias mutex held. */
/* Per frame in the per-frame record (@mig_n, @mig_rows, @mig_mb, @mig_ms; hle_nvn.c): growth migrations, the textures
   they copied, the heap bytes of those textures and the submit thread's time in them. */
static _Atomic uint64_t g_mig_events, g_mig_rows, g_mig_bytes, g_mig_ns;
void mtl_migrate_stats(uint64_t *n, uint64_t *rows, uint64_t *bytes, uint64_t *ns) {
    *n = atomic_load(&g_mig_events); *rows = atomic_load(&g_mig_rows); *bytes = atomic_load(&g_mig_bytes); *ns = atomic_load(&g_mig_ns);
}
/* OOT_STOR_MIGRATE_LOG=1: one line per migration with what grew and what was copied (2026-11-18: the long GPU frames
   while walking in Ocarina of Time were migrations of 2,400-2,700 textures, 3-5 frames in a row). Off by default. */
uint64_t dm_nvn_frame_count(void);
static int stor_migrate_log(void) { static int on = -1; if (on < 0) { const char *e = getenv("OOT_STOR_MIGRATE_LOG"); on = e && atoi(e) != 0; } return on; }
/* An entry nobody has held for this many submissions does not move to the new heap: it is released. Such an entry is a
   layout of an earlier frame, kept (STOR_IDLE_SUBMITS) in case the game makes the same layout at the same address
   again at once, which it does within a frame when it does. Ocarina of Time lays its targets out again every frame at
   shifting offsets, so a region holds about five such layouts for each one in use: 1,680-2,326 of the 2,122-2,697
   textures of its big migrations, 3.2-4.2 GiB of copies of 50-59 MiB of heap (2026-11-18, oot-route/migdiag-1).
   64 submissions are about 25 of its frames. OOT_MIGRATE_IDLE=<submissions> sets it, 0 moves every entry as before. */
static uint64_t stor_migrate_idle(void) {
    static int64_t n = -1;
    if (n < 0) { const char *e = getenv("OOT_MIGRATE_IDLE"); n = e ? atoll(e) : 64; if (n < 0) n = 0; }
    return (uint64_t)n;
}
static uint64_t g_alias_migrate_released;
static void stor_alias_migrate_to(stor_alias_heap *destination) {
    if (!stor_alias_migrate_on()) return;
    if (!on_sub_thread() || rp_active() || !g_cb) { g_alias_migration_skipped++; return; }
    uint64_t mig_t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW), mig_bytes = 0, mig_copies = 0, mig_moved = 0, mig_held = 0, mig_unheld = 0, mig_detached = 0;
    NSCountedSet *mig_shapes = stor_migrate_log() ? [NSCountedSet set] : nil;
    registry_lock();
    NSMutableArray *rows = [NSMutableArray array];
    stor_ent **idle = NULL; int nidle = 0, cidle = 0; const uint64_t idle_after = stor_migrate_idle();
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
            if (!detached && !entry->nser && idle_after && g_fence > entry->idle_fence + idle_after) {   // released below, not moved
                if (nidle == cidle) { cidle = cidle ? cidle * 2 : 256; idle = realloc(idle, (size_t)cidle * sizeof *idle); if (!idle) MDIE("migration idle list"); }
                idle[nidle++] = entry; continue;
            }
            [rows addObject:@{ @"base":base, @"generation":@(old->generation), @"made":@(entry->acq),
                @"offset":@(old->lo - destination->lo + base.heapOffset) }];
            if (detached) mig_detached++; else if (entry->nser) mig_held++; else mig_unheld++;
        }
    }
    for (int i = 0; i < nidle; i++) stor_drop_locked(idle[i]);   // their textures retire after the GPU work that used them; the earlier heap goes with the last
    g_alias_migrate_released += (uint64_t)nidle; g_stor_dropped += (uint64_t)nidle;
    pthread_mutex_unlock(&g_smu);
    free(idle);
    /* The layouts on a heap overlap, and each copy writes its layout's whole content into the new heap: the copy made
       last is what the shared bytes hold. So the layout the game made last is copied last (stor_ent.acq): it is the one
       whose content the bytes held. Until 2026-11-18 only the heaps were ordered, the layouts of one heap came in hash
       order, and a layout of an earlier frame could be copied over this frame's: entering the world in Ocarina of Time,
       two frames with white blocks and black dashes all over the picture (oot-route/room-1/burst/b01-22.png; 87,870 and
       147,801 flashing pixels against under 250 in the frames around). OOT_MIGRATE_ORDER=0: by heap only, as before. */
    static int by_made = -1;
    if (by_made < 0) { const char *e = getenv("OOT_MIGRATE_ORDER"); by_made = e ? atoi(e) != 0 : 1; }
    [rows sortUsingComparator:^NSComparisonResult(NSDictionary *a, NSDictionary *b) {
        if (by_made) { NSComparisonResult made = [a[@"made"] compare:b[@"made"]]; if (made != NSOrderedSame) return made; }
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
            mig_copies += slices;
        }
        mig_bytes += sa.size; mig_moved++;
        if (mig_shapes) [mig_shapes addObject:[NSString stringWithFormat:@"%lux%lu/%lu levels/%lu slices", (unsigned long)base.width, (unsigned long)base.height, (unsigned long)base.mipmapLevelCount, (unsigned long)slices]];
        uint64_t key=obj_key(base);
        pthread_mutex_lock(&g_smu);
        stor_ent *entry=nvn_hm_get(&g_stor_of_obj,&key);
        if (!entry || entry->tex!=(__bridge void *)base) {
            pthread_mutex_unlock(&g_smu); defer_obj_release(next); continue;
        }
        { NvAliasTrack *from=objc_getAssociatedObject(base,&g_alias_track_key),*to=objc_getAssociatedObject(next,&g_alias_track_key);   /* OOT_STOR_TRACE: the same texture, moved */
          if (from&&to) { to->number=from->number; from->moved=1; } }
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
    uint64_t mig_ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - mig_t0;
    if (rows.count || nidle) { atomic_fetch_add(&g_mig_events, 1); atomic_fetch_add(&g_mig_rows, mig_moved); atomic_fetch_add(&g_mig_bytes, mig_bytes); atomic_fetch_add(&g_mig_ns, mig_ns); }
    if ((rows.count || nidle) && mig_shapes) {
        NSArray *top = [mig_shapes.allObjects sortedArrayUsingComparator:^NSComparisonResult(id a, id b) {
            NSUInteger x = [mig_shapes countForObject:a], y = [mig_shapes countForObject:b]; return x > y ? NSOrderedAscending : x < y ? NSOrderedDescending : NSOrderedSame; }];
        NSMutableString *shapes = [NSMutableString string];
        for (NSUInteger i = 0; i < MIN(top.count, (NSUInteger)5); i++) [shapes appendFormat:@" %@ x%lu", top[i], (unsigned long)[mig_shapes countForObject:top[i]]];
        pthread_mutex_lock(&g_alias_mu); __typeof__(g_alias_grow_note) note = g_alias_grow_note; pthread_mutex_unlock(&g_alias_mu);
        MLOG("[heap-migrate] detail frame %llu generation %llu %s format %llu: texture [%#llx, +%llu KiB), merged %d earlier [%#llx, +%llu KiB) -> new [%#llx, +%llu KiB): low +%llu KiB, high +%llu KiB; "
             "candidates %lu (held %llu, unheld %llu, detached %llu), released %d idle, copied %llu textures / %.1f MiB in %llu copies, %.2f ms on the submit thread; %lu shapes, most:%@",
             (unsigned long long)dm_nvn_frame_count(), (unsigned long long)destination->generation, note.region ? "region" : "exact", (unsigned long long)destination->key.pf,
             (unsigned long long)note.lo, (unsigned long long)((note.hi - note.lo) >> 10), note.merged, (unsigned long long)note.old_lo, (unsigned long long)((note.old_hi - note.old_lo) >> 10),
             (unsigned long long)note.nlo, (unsigned long long)((note.nhi - note.nlo) >> 10), (unsigned long long)((note.old_lo - note.nlo) >> 10), (unsigned long long)((note.nhi - note.old_hi) >> 10),
             (unsigned long)rows.count, (unsigned long long)mig_held, (unsigned long long)mig_unheld, (unsigned long long)mig_detached, nidle,
             (unsigned long long)mig_moved, mig_bytes / 1048576.0, (unsigned long long)mig_copies, mig_ns / 1e6, (unsigned long)top.count, shapes);
    }
    if (rows.count && (g_alias_migrated < 16 || g_alias_migrated % 100 < rows.count))
        MLOG("[heap-migrate] generation %llu: %lu candidates, %llu moved total, %llu unsafe boundaries kept",
            (unsigned long long)destination->generation, (unsigned long)rows.count,
            (unsigned long long)g_alias_migrated, (unsigned long long)g_alias_migration_skipped);
}
