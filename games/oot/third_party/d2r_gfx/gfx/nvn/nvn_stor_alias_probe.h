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
int mtl_probe_stor_alias(void) {
    mtl_init(); int checked = 0;
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
