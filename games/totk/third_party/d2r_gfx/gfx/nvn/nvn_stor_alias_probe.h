/* probe-gpu-stor-alias (included after the production backend): render targets of several layouts at one pool address
   go through tex_mtl / rp_begin as in the game. Checks that same-format layouts share one placement heap (device
   allocation grows by one heap, not by the sum), that each layout's own clear reads back exactly on the GPU, that
   another pixel format and a larger layout get heaps of their own, that the heap (not its textures) is the residency
   allocation, and that finalising the pool frees the heaps and takes them out of the residency set.
   Negative control: OOT_STOR_ALIAS=0 must fail the sharing check. */
static id<MTLTexture> alias_probe_clear_read(tex_t *t, id<MTLBuffer> rb, float red, int bgra) {
    op_hdr end = {OP_END, 8}; mtl_sub_begin(); mtl_exec((const uint8_t *)&end);
    memset(&R, 0, sizeof R); R.has_rt = 1; R.rt.n = 1; R.rt.color[0] = t;
    R.clear_c[0] = 1; R.cc[0] = MTLClearColorMake(red, 0.25, 0.5, 1); rp_begin(); rp_end();
    id<MTLTexture> mt = tex_mtl(t); if (!mt) MDIE("alias probe: no texture for %ux%u", t->d.width, t->d.height);
    NSUInteger w = mt.width, h = mt.height;
    enc_b_copy_texture_buffer(blit(), mt, 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(w, h, 1), rb, 0, w * 4, w * h * 4);
    mtl_sub_commit(); mtl_finish();
    const uint8_t *p = rb.contents; uint8_t want = (uint8_t)lrintf(red * 255.0f), e[4] = {want, 64, 128, 255};
    if (bgra) { e[0] = 128; e[2] = want; }
    for (NSUInteger i = 0; i < w * h; i += 97) if (memcmp(p + i * 4, e, 4))
        MDIE("alias probe: %lux%lu texel %lu reads %u,%u,%u,%u, expected %u,%u,%u,%u", (unsigned long)w, (unsigned long)h, (unsigned long)i, p[i * 4], p[i * 4 + 1], p[i * 4 + 2], p[i * 4 + 3], e[0], e[1], e[2], e[3]);
    return mt;
}
static id<MTLHeap> alias_probe_heap(id<MTLTexture> t) { while (t.parentTexture) t = t.parentTexture; return t.heap; }   // a sampling view reports no heap itself
static NSUInteger alias_probe_offset(id<MTLTexture> t) { while (t.parentTexture) t = t.parentTexture; return t.heapOffset; }
/* Reads t back without writing it: its last clear must still be there. */
static void alias_probe_read(tex_t *t, id<MTLBuffer> rb, float red) {
    op_hdr end = {OP_END, 8}; mtl_sub_begin(); mtl_exec((const uint8_t *)&end);
    id<MTLTexture> mt = tex_mtl(t); NSUInteger w = mt.width, h = mt.height;
    enc_b_copy_texture_buffer(blit(), mt, 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(w, h, 1), rb, 0, w * 4, w * h * 4);
    mtl_sub_commit(); mtl_finish();
    const uint8_t *p = rb.contents, e[4] = {(uint8_t)lrintf(red * 255.0f), 64, 128, 255};
    for (NSUInteger i = 0; i < w * h; i++) if (memcmp(p + i * 4, e, 4))
        MDIE("alias probe: disjoint neighbour corrupted %lux%lu at texel %lu: %u,%u,%u,%u, cleared to %u,64,128,255", (unsigned long)w, (unsigned long)h, (unsigned long)i, p[i * 4], p[i * 4 + 1], p[i * 4 + 2], p[i * 4 + 3], e[0]);
}
static int alias_probe_is_window(id<MTLTexture> texture, uint64_t *pool_generation) {
    @autoreleasepool {
        while (texture.parentTexture) texture = texture.parentTexture;
        NvAliasTrack *track = objc_getAssociatedObject(texture, &g_alias_track_key);
        if (pool_generation) *pool_generation = track ? track->h->window_key.pool_generation : 0;
        return track && track->h->window;
    }
}
/* Mode 3 tests use real host pool registrations. Fixed windows must not grow with each new layout, cross a pool
   boundary, inherit a finalized registration's heap, or turn an ambiguous lookup into an allocator identity. */
static int alias_probe_windows(void) {
    int checked = 0;
    const size_t page = (size_t)getpagesize(), tail = 128u << 10;
    const size_t span = 2 * STOR_ALIAS_WINDOW_BYTES + tail;
    void *mem = NULL; if (posix_memalign(&mem, page, span)) MDIE("window alias probe memory");
    memset(mem, 0, span); mtl_pool_add(mem, span);
    uint64_t heaps0 = g_alias_heaps_live, made0 = g_alias_heaps_made, freed0 = g_alias_heaps_freed;
    id<MTLBuffer> rb = probe_buf(512 * 512 * 4);
    const uint32_t sides[] = {256, 256, 256, 256, 512, 128, 256, 256};
    tex_t t[8] = {{0}}; id<MTLTexture> mt[8] = {nil};
    for (int i = 0; i < 8; i++) {
        t[i].serial = UINT64_C(0xfffd5300) + (uint64_t)i;
        t[i].d = (nvn_texdesc){.width=sides[i],.height=sides[i],.depth=1,.levels=1,.target=1,.format=0x25,
                              .swizzle_r=2,.swizzle_g=3,.swizzle_b=4,.swizzle_a=5};
        nvn_tex_layout(&t[i].d, &t[i].L);
    }
    uint64_t q = t[0].L.size;
    t[0].mem = mem; t[1].mem = (uint8_t *)mem + q;
    t[2].mem = (uint8_t *)mem + STOR_ALIAS_WINDOW_BYTES - q / 2;
    t[3].mem = (uint8_t *)mem + STOR_ALIAS_WINDOW_BYTES;
    t[4].mem = mem; t[5].mem = (uint8_t *)mem + 2 * STOR_ALIAS_WINDOW_BYTES;
    t[6].mem = (uint8_t *)((uintptr_t)mem + span + page);   // no registered range, never read on the CPU
    t[7].mem = (uint8_t *)mem + 1;   // cannot place at Metal's alignment; exact fallback stays available
    mt[0] = alias_probe_clear_read(&t[0], rb, 1.0f/8.0f, 0);
    mt[1] = alias_probe_clear_read(&t[1], rb, 2.0f/8.0f, 0);
    if (!alias_probe_is_window(mt[0], NULL) || alias_probe_heap(mt[0]) != alias_probe_heap(mt[1])) MDIE("window alias probe: disjoint textures did not share their window");
    if (alias_probe_heap(mt[0]).size != STOR_ALIAS_WINDOW_BYTES || alias_probe_offset(mt[1]) != q) MDIE("window alias probe: wrong immutable span or guest-relative offset");
    checked += 2;
    alias_probe_read(&t[0], rb, 1.0f/8.0f); checked++;
    tex_t same = t[0]; same.serial = UINT64_C(0xfffd5380);
    alias_probe_read(&same, rb, 1.0f/8.0f); checked++;
    mt[2] = alias_probe_clear_read(&t[2], rb, 3.0f/8.0f, 0);
    mt[3] = alias_probe_clear_read(&t[3], rb, 4.0f/8.0f, 0);
    if (alias_probe_is_window(mt[2], NULL) || !alias_probe_is_window(mt[3], NULL) || alias_probe_heap(mt[3]) == alias_probe_heap(mt[0])) MDIE("window alias probe: boundary fallback or next window incorrect");
    checked++;
    mt[4] = alias_probe_clear_read(&t[4], rb, 5.0f/8.0f, 0);
    if (alias_probe_heap(mt[4]) != alias_probe_heap(mt[0]) || alias_probe_heap(mt[4]).size != STOR_ALIAS_WINDOW_BYTES) MDIE("window alias probe: a larger layout grew the window");
    checked++;
    mt[5] = alias_probe_clear_read(&t[5], rb, 6.0f/8.0f, 0);
    if (!alias_probe_is_window(mt[5], NULL) || alias_probe_heap(mt[5]).size != tail) MDIE("window alias probe: final window was not clipped to the pool");
    checked++;
    mt[6] = alias_probe_clear_read(&t[6], rb, 7.0f/8.0f, 0);
    mt[7] = alias_probe_clear_read(&t[7], rb, 1.0f/8.0f, 0);
    if (alias_probe_is_window(mt[6], NULL) || alias_probe_is_window(mt[7], NULL)) MDIE("window alias probe: unknown range or misalignment used a window");
    checked += 2;
    tex_t too_small = t[0]; too_small.mem = (uint8_t *)mem + (STOR_ALIAS_WINDOW_BYTES / 2); too_small.L.size = 1;
    id<MTLTexture> nonfitting = stor_alias_texture(&too_small, tex_desc(&too_small, 0));
    if (!nonfitting || alias_probe_is_window(nonfitting, NULL)) MDIE("window alias probe: nonfitting layout used a window");
    defer_obj_release(nonfitting); nonfitting = nil; checked++;
    stor_alias_pool found;
    pthread_mutex_lock(&g_mu); int locked = stor_alias_pool_range((uintptr_t)mem, q, &found); pthread_mutex_unlock(&g_mu);
    if (locked || stor_alias_pool_range(UINT64_MAX - 8, 32, &found)) MDIE("window alias probe: recursive lock or overflow accepted");
    checked += 2;
    // A second overlapping registration makes the pool identity ambiguous; no blocking or guessed owner is allowed.
    mtl_pool_add((uint8_t *)mem + page, span - page);
    if (stor_alias_pool_range((uintptr_t)mem + 2 * page, q, &found)) MDIE("window alias probe: overlapping pools accepted");
    tex_t ambiguous = t[0]; ambiguous.mem = (uint8_t *)mem + 2 * page;
    id<MTLTexture> overlap = stor_alias_texture(&ambiguous, tex_desc(&ambiguous, 0));
    if (!overlap || alias_probe_is_window(overlap, NULL)) MDIE("window alias probe: ambiguous range did not fall back");
    defer_obj_release(overlap); overlap = nil; checked += 2;
    mtl_pool_del((uint8_t *)mem + page);
    uint64_t old_generation = 0; alias_probe_is_window(mt[0], &old_generation);
    id<MTLHeap> old_heap = alias_probe_heap(mt[0]);
    mtl_pool_del(mem); mtl_pool_add(mem, span);
    tex_t fresh = t[0]; fresh.serial = UINT64_C(0xfffd5381);
    id<MTLTexture> fresh_mt = alias_probe_clear_read(&fresh, rb, 3.0f/8.0f, 0);
    uint64_t new_generation = 0;
    if (!alias_probe_is_window(fresh_mt, &new_generation) || !new_generation || new_generation == old_generation || alias_probe_heap(fresh_mt) == old_heap)
        MDIE("window alias probe: reused pool inherited the previous registration's heap");
    checked++;
    old_heap = nil; fresh_mt = nil; mtl_tex_release(&fresh); mtl_tex_release(&same);
    for (int i = 0; i < 8; i++) { mt[i] = nil; mtl_tex_release(&t[i]); }
    mtl_pool_del(mem);
    // t[6] deliberately lies outside the registered pool; retire its storage explicitly too.
    stor_drop_range((uintptr_t)t[6].mem, (uintptr_t)t[6].mem + t[6].L.size);
    op_hdr end = {OP_END, 8};
    for (int i = 0; i < 4; i++) { mtl_sub_begin(); mtl_exec((const uint8_t *)&end); mtl_sub_commit(); mtl_finish(); defer_drain(); }
    stor_alias_collect();
    if (g_alias_heaps_live != heaps0 || g_alias_heaps_freed - freed0 != g_alias_heaps_made - made0) MDIE("window alias probe: heaps survived final pool/texture retirement");
    checked++;
    MLOG("window alias probe: %d checks, %llu heaps made and freed", checked, (unsigned long long)(g_alias_heaps_made - made0));
    res_remove(rb); free(mem); return checked;
}
int mtl_probe_stor_alias(void) {
    mtl_init(); if (stor_alias_mode() == 3) return alias_probe_windows();
    int checked = 0;
    const size_t page = (size_t)getpagesize(), span = 4u << 20;
    void *mem = NULL; if (posix_memalign(&mem, page, span)) MDIE("alias probe memory"); memset(mem, 0, span);
    const uint32_t sides[] = {512, 384, 448, 256, 640}, formats[] = {0x25, 0x25, 0x25, 0x25, 0x25};
    tex_t t[6] = {{0}}; id<MTLTexture> mt[6] = {nil};
    id<MTLBuffer> rb = probe_buf(640 * 640 * 4);
    uint64_t heaps0 = g_alias_heaps_live, made0 = g_alias_heaps_made, freed0 = g_alias_heaps_freed;
    uint64_t dev0 = (uint64_t)g_dev.currentAllocatedSize;
    for (int i = 0; i < 6; i++) {
        t[i].serial = UINT64_C(0xfffd5100) + (uint64_t)i; t[i].mem = mem;
        uint32_t side = i < 5 ? sides[i] : 320;   // not 384: the same layout in another format is a format view on the shared base (stor_acquire)
        t[i].d = (nvn_texdesc){.width = side, .height = side, .depth = 1, .levels = 1, .target = 1, .format = i < 5 ? formats[i] : 0x99,
                               .swizzle_r = 2, .swizzle_g = 3, .swizzle_b = 4, .swizzle_a = 5};
        nvn_tex_layout(&t[i].d, &t[i].L);
        if (t[i].L.size > span) MDIE("alias probe layout %u exceeds its pool", side);
    }
    /* Four same-format layouts, the largest first: one heap. Each clear must read back through its own layout. */
    for (int i = 0; i < 4; i++) { mt[i] = alias_probe_clear_read(&t[i], rb, (float)(i + 1) / 8.0f, 0); checked++; }
    for (int i = 0; i < 4; i++) {
        if (!alias_probe_heap(mt[i])) MDIE("alias probe: layout %lux%lu is not on an alias heap", (unsigned long)mt[i].width, (unsigned long)mt[i].height);
        if (alias_probe_heap(mt[i]) != alias_probe_heap(mt[0])) MDIE("alias probe: layouts %lux%lu and %lux%lu at one address do not share a heap", (unsigned long)mt[0].width, (unsigned long)mt[0].height, (unsigned long)mt[i].width, (unsigned long)mt[i].height);
        checked++;
    }
    uint64_t sum = 0; for (int i = 0; i < 4; i++) sum += mt[i].allocatedSize;
    uint64_t grew = (uint64_t)g_dev.currentAllocatedSize - dev0;
    if (grew >= sum - mt[1].allocatedSize) MDIE("alias probe: device allocation grew %llu bytes for %llu bytes of layouts sharing one heap", (unsigned long long)grew, (unsigned long long)sum);
    checked++;
    if (g_alias_heaps_live != heaps0 + 1) MDIE("alias probe: %llu heaps live, expected one more than %llu", (unsigned long long)g_alias_heaps_live, (unsigned long long)heaps0);
    checked++;
    pthread_mutex_lock(&g_res_mu); int heap_res = [g_res_objects containsObject:alias_probe_heap(mt[0])], tex_res = 0;
    for (int i = 0; i < 4; i++) tex_res |= [g_res_objects containsObject:mt[i]];
    pthread_mutex_unlock(&g_res_mu);
    if (!heap_res || tex_res) MDIE("alias probe: residency holds heap=%d textures=%d (want the heap only)", heap_res, tex_res);
    checked++;
    /* The first layout again, after the others overwrote the pages: its own clear still reads back exactly. */
    alias_probe_clear_read(&t[0], rb, 7.0f / 8.0f, 0); checked++;
    /* A larger layout replaces the heap; another pixel format keeps its own. */
    mt[4] = alias_probe_clear_read(&t[4], rb, 6.0f / 8.0f, 0);
    mt[5] = alias_probe_clear_read(&t[5], rb, 5.0f / 8.0f, 1);
    MLOG("alias probe: BGRA texture %p parent %p heap %p pf %lu storage %lu; larger heap %p", (__bridge void *)mt[5], (__bridge void *)mt[5].parentTexture, (__bridge void *)alias_probe_heap(mt[5]), (unsigned long)mt[5].pixelFormat, (unsigned long)mt[5].storageMode, (__bridge void *)alias_probe_heap(mt[4]));
    if (!alias_probe_heap(mt[4]) || alias_probe_heap(mt[4]) == alias_probe_heap(mt[0])) MDIE("alias probe: a larger layout did not get a larger heap");
    if (!alias_probe_heap(mt[5])) MDIE("alias probe: the other pixel format is not on an alias heap");
    if (alias_probe_heap(mt[4]).size < mt[4].allocatedSize) MDIE("alias probe: heap smaller than its texture");
    if (mt[5].pixelFormat == mt[0].pixelFormat) MDIE("alias probe: format fixture maps to the same Metal format");
    if (alias_probe_heap(mt[5]) == alias_probe_heap(mt[0]) || alias_probe_heap(mt[5]) == alias_probe_heap(mt[4])) MDIE("alias probe: another pixel format shares the heap");
    checked += 4;
    /* Region heaps (mode 1): byte offsets follow guest offsets; disjoint guest ranges never share pages; a partial
       overlap merges into a new region. C (512x512) opens a region; A and B (256x256) are its two disjoint quarters. */
    tex_t r[5] = {{0}}; id<MTLTexture> rt[5] = {nil}; uint8_t *mem2 = (uint8_t *)mem + (2u << 20);
    const uint32_t rsides[5] = {512, 256, 256, 256, 256};
    for (int i = 0; i < 5; i++) {
        r[i].serial = UINT64_C(0xfffd5200) + (uint64_t)i;
        r[i].d = (nvn_texdesc){.width = rsides[i], .height = rsides[i], .depth = 1, .levels = 1, .target = 1, .format = 0x25, .swizzle_r = 2, .swizzle_g = 3, .swizzle_b = 4, .swizzle_a = 5};
        nvn_tex_layout(&r[i].d, &r[i].L);
    }
    uint64_t q = r[1].L.size;
    r[0].mem = mem2; r[1].mem = mem2; r[2].mem = mem2 + q; r[3].mem = mem2 + r[0].L.size - q / 2; r[4].mem = mem2 + q / 2;
    if (r[3].mem + r[3].L.size > (uint8_t *)mem + span || r[2].mem + r[2].L.size > mem2 + r[0].L.size) MDIE("alias probe region fixture outside its pool");
    int regions = stor_alias_mode() == 1;
    if (regions) {
        rt[0] = alias_probe_clear_read(&r[0], rb, 1.0f / 8.0f, 0);
        rt[1] = alias_probe_clear_read(&r[1], rb, 2.0f / 8.0f, 0);
        rt[2] = alias_probe_clear_read(&r[2], rb, 3.0f / 8.0f, 0);
        if (alias_probe_heap(rt[1]) != alias_probe_heap(rt[0]) || alias_probe_heap(rt[2]) != alias_probe_heap(rt[0])) MDIE("alias probe: textures inside one guest region are on different heaps");
        if (alias_probe_offset(rt[1]) != alias_probe_offset(rt[0]) || alias_probe_offset(rt[2]) != alias_probe_offset(rt[0]) + q)
            MDIE("alias probe: heap offsets %lu/%lu/%lu do not follow guest offsets 0/0/%llu", (unsigned long)alias_probe_offset(rt[0]), (unsigned long)alias_probe_offset(rt[1]), (unsigned long)alias_probe_offset(rt[2]), (unsigned long long)q);
        checked += 2;
        alias_probe_read(&r[1], rb, 2.0f / 8.0f);   // B was written after A, at disjoint guest bytes: A is intact
        checked++;
        /* D starts inside C's region and ends past it: a merged region; E (inside the union) goes onto D's heap. */
        rt[3] = alias_probe_clear_read(&r[3], rb, 4.0f / 8.0f, 0);
        rt[4] = alias_probe_clear_read(&r[4], rb, 5.0f / 8.0f, 0);
        if (alias_probe_heap(rt[3]) == alias_probe_heap(rt[0])) MDIE("alias probe: a partial overlap did not merge into a new region");
        if (alias_probe_heap(rt[4]) != alias_probe_heap(rt[3])) MDIE("alias probe: a texture inside the merged region is not on its heap");
        if (alias_probe_offset(rt[3]) - alias_probe_offset(rt[4]) != (uint64_t)(r[3].mem - r[4].mem)) MDIE("alias probe: merged region offsets do not follow guest offsets");
        if (alias_probe_heap(rt[3]).size < (uint64_t)(r[3].mem + rt[3].allocatedSize - mem2)) MDIE("alias probe: merged region does not cover the union");
        for (int i = 0; i < 5; i++) if (rt[i].allocatedSize > r[i].L.size) MDIE("alias probe: region texture %d takes %lu bytes past its %llu guest bytes", i, (unsigned long)rt[i].allocatedSize, (unsigned long long)r[i].L.size);
        checked += 5;
    }
    /* Finalise: serials, then the pool range. The heaps go once the GPU work using their textures completed. */
    for (int i = 0; i < 6; i++) { mt[i] = nil; mtl_tex_release(&t[i]); }
    for (int i = 0; i < 5; i++) { rt[i] = nil; if (regions) mtl_tex_release(&r[i]); }
    stor_drop_range((uintptr_t)mem, (uintptr_t)mem + span);
    op_hdr end = {OP_END, 8};
    for (int round = 0; round < 4; round++) { mtl_sub_begin(); mtl_exec((const uint8_t *)&end); mtl_sub_commit(); mtl_finish(); defer_drain(); }
    stor_alias_collect();
    uint64_t want = regions ? 5 : 3;   // shared, larger, other format (+ the region and its merge)
    if (g_alias_heaps_made - made0 != want) MDIE("alias probe: %llu heaps made, expected %llu", (unsigned long long)(g_alias_heaps_made - made0), (unsigned long long)want);
    if (g_alias_heaps_live != heaps0 || g_alias_heaps_freed - freed0 != g_alias_heaps_made - made0)
        MDIE("alias probe: %llu heaps live after the pool was finalised (made %llu, freed %llu)", (unsigned long long)g_alias_heaps_live,
             (unsigned long long)(g_alias_heaps_made - made0), (unsigned long long)(g_alias_heaps_freed - freed0));
    checked++;
    MLOG("storage alias probe: %llu heaps made and freed, device allocation +%llu bytes for %llu bytes of shared layouts",
         (unsigned long long)(g_alias_heaps_made - made0), (unsigned long long)grew, (unsigned long long)sum);
    res_remove(rb); free(mem);
    return checked;
}
