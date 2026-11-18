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
/* A growing region must preserve already-written neighbours and rebind their
   live serials. Old GPU commands keep their original resources until completion.
   OOT_STOR_MIGRATE_TEST=1 runs this assertion even when the fix is disabled. */
static int alias_probe_migration_subresources(void) {
    int checked=0;
    const uint32_t formats[]={0x25,0x32,0x34,0x3f,0x3d};
    for(unsigned mode=0;mode<10;mode++) { @autoreleasepool {
        const uint32_t format=formats[mode%5];const int array=mode>=5;
        void *mem=NULL;const size_t span=8u<<20;
        if(posix_memalign(&mem,(size_t)getpagesize(),span))MDIE("migration subresource memory");
        pool_t pool={.mem=mem,.size=span,.flags=0xa1};
        tex_t small={0};small.serial=UINT64_C(0xfffd5400)+mode*2;small.mem=mem;small.pool=&pool;
        small.d=(nvn_texdesc){.width=64,.height=64,.depth=array?2:1,.levels=3,.target=array?4:1,.format=format,.flags=NVN_TEXTURE_FLAGS_COMPRESSIBLE,
            .swizzle_r=2,.swizzle_g=3,.swizzle_b=4,.swizzle_a=5};
        nvn_tex_layout(&small.d,&small.L);tex_t big=small;big.serial++;big.d.width=big.d.height=256;nvn_tex_layout(&big.d,&big.L);
        int bw,bh,bpp=nvn_fmt_bpb(format,&bw,&bh);if(!bpp||bw!=1||bh!=1)MDIE("migration format fixture");
        id<MTLBuffer> upload=probe_buf(1u<<20),readback=probe_buf(1u<<20);
        const unsigned layers=array?2:1;size_t offsets[2][3],rows[3],total=0;
        for(unsigned layer=0;layer<layers;layer++)for(unsigned level=0;level<3;level++) {
            size_t row=(((64u>>level)*(unsigned)bpp)+255u)&~255u;rows[level]=row;offsets[layer][level]=total;
            size_t size=row*(64u>>level);uint32_t value=UINT32_C(0x3e000000)+(layer*3+level)*UINT32_C(0x00100000);
            for(size_t p=0;p<size;p+=4)memcpy((uint8_t *)upload.contents+total+p,&value,4);total+=size;
        }
        op_hdr end={OP_END,8};mtl_sub_begin();mtl_exec((const uint8_t *)&end);
        id<MTLTexture> old=tex_mtl(&small);
        for(unsigned layer=0;layer<layers;layer++)for(unsigned level=0;level<3;level++)
            enc_b_copy_buffer_texture(blit(),upload,offsets[layer][level],rows[level],rows[level]*(64u>>level),MTLSizeMake(64u>>level,64u>>level,1),old,layer,level,MTLOriginMake(0,0,0));
        /* Growth occurs in the same submission as the writes: copying CPU
           memory or waiting only for the preceding fence would lose them. */
        id<MTLTexture> bigger=tex_mtl(&big),moved=tex_mtl(&small);
        if ((moved!=old && alias_probe_heap(moved)!=alias_probe_heap(bigger)) ||
            (stor_alias_mode()==2 && moved==old)) MDIE("migration subresources not moved mode %u",mode);
        MLOG("migration subresource mode %u: %s",mode,moved==old?"kept compatible original":"moved");
        for(unsigned layer=0;layer<layers;layer++)for(unsigned level=0;level<3;level++)
            enc_b_copy_texture_buffer(blit(),moved,layer,level,MTLOriginMake(0,0,0),MTLSizeMake(64u>>level,64u>>level,1),readback,offsets[layer][level],rows[level],rows[level]*(64u>>level));
        mtl_sub_commit();mtl_finish();
        for(unsigned layer=0;layer<layers;layer++)for(unsigned level=0;level<3;level++) {
            for(unsigned y=0;y<(64u>>level);y++) {
                size_t at=offsets[layer][level]+y*rows[level];
                if(memcmp((uint8_t *)upload.contents+at,(uint8_t *)readback.contents+at,(64u>>level)*(unsigned)bpp))
                    MDIE("migration lost pixels mode %u layer %u level %u row %u",mode,layer,level,y);
            }checked++;
        }
        old=nil;bigger=nil;moved=nil;mtl_tex_release(&small);mtl_tex_release(&big);
        stor_drop_range((uintptr_t)mem,(uintptr_t)mem+span);
        for(int round=0;round<4;round++){mtl_sub_begin();mtl_exec((const uint8_t *)&end);mtl_sub_commit();mtl_finish();defer_drain();}
        stor_alias_collect();res_remove(upload);res_remove(readback);free(mem);
    } }
    MLOG("heap migration subresources passed: %d color/depth/mip/layer checks",checked);
    return checked;
}
static int alias_probe_growth_migration(void) {
    const char *test=getenv("OOT_STOR_MIGRATE_TEST");
    if (!test || !atoi(test)) return 0;
    const size_t span=4u<<20; void *mem=NULL;
    if (posix_memalign(&mem,(size_t)getpagesize(),span)) MDIE("migration probe memory");
    tex_t t[3]={{0}};
    for (int i=0;i<3;i++) {
        t[i].serial=UINT64_C(0xfffd5300)+(uint64_t)i;
        t[i].d=(nvn_texdesc){.width=i==2?512:256,.height=i==2?512:256,.depth=1,.levels=1,.target=1,.format=0x25,
            .swizzle_r=2,.swizzle_g=3,.swizzle_b=4,.swizzle_a=5};
        nvn_tex_layout(&t[i].d,&t[i].L);t[i].mem=mem;
    }
    t[1].mem=(uint8_t *)mem+t[0].L.size;
    id<MTLBuffer> out=probe_buf(512*512*4), before=probe_buf(256*256*4);
    __weak id<MTLHeap> old[2];
    @autoreleasepool {
        old[0]=alias_probe_heap(alias_probe_clear_read(&t[0],out,0.25f,0));
        old[1]=alias_probe_heap(alias_probe_clear_read(&t[1],out,0.75f,0));
    }
    mtl_reg_tex(101,&t[0],NULL);
    @autoreleasepool {
        op_hdr end={OP_END,8};mtl_sub_begin();mtl_exec((const uint8_t *)&end);
        registry_lock();id<MTLTexture> a=(__bridge id)tex_fast(101);registry_unlock();
        enc_b_copy_texture_buffer(blit(),a,0,0,MTLOriginMake(0,0,0),MTLSizeMake(256,256,1),before,0,1024,256*1024);
        id<MTLTexture> big=tex_mtl(&t[2]);
        id<MTLTexture> moved=tex_mtl(&t[0]), neighbour=tex_mtl(&t[1]);
        if (alias_probe_heap(moved)!=alias_probe_heap(big)||alias_probe_heap(neighbour)!=alias_probe_heap(big))
            MDIE("migration probe: live owners still retain old heaps");
        if (!old[0]||!old[1]) MDIE("migration probe: released a heap before its GPU read completed");
        registry_lock();id<MTLTexture> view=(__bridge id)tex_fast(101);registry_unlock();
        if (alias_probe_heap(view)!=alias_probe_heap(big)) MDIE("migration probe: descriptor view retained old storage");
        mtl_sub_commit();mtl_finish();
    }
    const uint8_t expected[4]={64,64,128,255};
    for (size_t i=0;i<256*256;i++) if(memcmp((uint8_t *)before.contents+i*4,expected,4)) MDIE("migration probe: in-flight source content changed");
    alias_probe_read(&t[0],out,0.25f);alias_probe_read(&t[1],out,0.75f);
    for (int round=0;round<4;round++) {op_hdr end={OP_END,8};mtl_sub_begin();mtl_exec((const uint8_t *)&end);mtl_sub_commit();mtl_finish();defer_drain();}
    stor_alias_collect();
    if (old[0]||old[1]) MDIE("migration probe: old heap not freed after GPU completion");
    pthread_mutex_lock(&g_smu);
    uint64_t key=obj_key((__bridge id)nvn_hm_get(&g_by_serial,&t[0].serial));stor_ent *entry=nvn_hm_get(&g_stor_of_obj,&key);
    int owned=entry&&entry->nser>0;pthread_mutex_unlock(&g_smu);
    if (!owned) MDIE("migration probe: live descriptor became idle storage");
    mtl_reg_tex(101,NULL,NULL);
    for(int i=0;i<3;i++)mtl_tex_release(&t[i]);
    stor_drop_range((uintptr_t)mem,(uintptr_t)mem+span);
    for(int i=0;i<4;i++){op_hdr end={OP_END,8};mtl_sub_begin();mtl_exec((const uint8_t *)&end);mtl_sub_commit();mtl_finish();defer_drain();}
    stor_alias_collect();res_remove(out);res_remove(before);free(mem);
    MLOG("heap growth migration probe passed: contents, neighbour, in-flight read, views, owners, release");
    return 7+alias_probe_migration_subresources();
}
/* 2026-11-18 (Ocarina of Time, the user's recorded route):
   Room. A region holding many textures that a texture reaches out of is replaced by a heap with room on that side
   (nvn_alias_plan.h), so the next textures there land on it and nothing moves again: the exact union replaced a 59 MiB
   region 3-7 frames in a row, 4.2 GiB of copies each. Here: C (512x512) opens a region, Q (256x256, written last) is
   the witness and 20 small layouts make it one that takes room; X reaches past its top, then Y lies above X; W reaches
   below its bottom, then V lies below W. Expected: one replacement for X and one for W, none for Y and V, the room is
   ALIAS_ROOM_MIN on that side, Q reads as written after both.
   Order. A migration copies the layout the game made last last (nvn_stor_migrate.h): the layouts on a heap overlap and
   each copy writes its layout's whole content, so in hash order a layout of an earlier frame could land over this
   frame's (two frames of white blocks entering the world). Here: 12 pairs, S (512x512) written first and L (256x256)
   written second at the same address, then G reaches past S and the pair moves. Expected: every L reads as written. */
static int alias_probe_room_and_order(void) {
    if (stor_alias_mode() != 1 || !stor_alias_migrate_on() || !stor_alias_room()) return 0;
    int checked = 0; op_hdr end = {OP_END, 8};
    id<MTLBuffer> rb = probe_buf(512 * 512 * 4);
    uint64_t heaps0 = g_alias_heaps_live;
    {   enum { SMALL = 20, KQ = 1, KX = SMALL + 2, KY, KW, KV, KN };   // 0 C, 1 the witness, 2..21 small
        const size_t span = 16u << 20; void *mem = NULL;
        if (posix_memalign(&mem, (size_t)getpagesize(), span)) MDIE("room probe memory");
        memset(mem, 0, span);
        static tex_t t[KN]; memset(t, 0, sizeof t);
        for (int i = 0; i < KN; i++) {
            uint32_t side = i == 0 ? 512 : (i == KQ || i >= KX) ? 256 : 64;
            t[i].serial = UINT64_C(0xfffd5500) + (uint64_t)i;
            t[i].d = (nvn_texdesc){.width = side, .height = side, .depth = 1, .levels = 1, .target = 1, .format = 0x25, .swizzle_r = 2, .swizzle_g = 3, .swizzle_b = 4, .swizzle_a = 5};
            nvn_tex_layout(&t[i].d, &t[i].L);
        }
        const uint64_t c = t[0].L.size, s = t[2].L.size, x = t[KX].L.size; uint8_t *base = (uint8_t *)mem + (6u << 20);
        if (x + s * SMALL > c || x % 4 || (x / 4) % 65536) MDIE("room probe fixture: region %llu, small %llu, larger %llu", (unsigned long long)c, (unsigned long long)s, (unsigned long long)x);
        t[0].mem = base; t[KQ].mem = base;
        for (int i = 0; i < SMALL; i++) t[2 + i].mem = base + x + (uint64_t)i * s;
        t[KX].mem = base + c - x / 4;            // a quarter inside the region, the rest past its top
        t[KY].mem = base + c + x;                // above X, inside the room
        t[KW].mem = base - 3 * x / 4;            // a quarter inside the region, the rest below its bottom
        t[KV].mem = t[KW].mem - x - x / 4;        // below W, inside the room
        id<MTLHeap> first = alias_probe_heap(alias_probe_clear_read(&t[0], rb, 3.0f / 8.0f, 0));
        alias_probe_clear_read(&t[KQ], rb, 7.0f / 8.0f, 0);
        mtl_sub_begin(); mtl_exec((const uint8_t *)&end);
        for (int i = 0; i < SMALL; i++) { id<MTLTexture> m = tex_mtl(&t[2 + i]); if (!m || alias_probe_heap(m) != first) MDIE("room probe: small layout %d is not on the region's heap", i); }
        mtl_sub_commit(); mtl_finish();
        NSUInteger region = first.size; first = nil;
        uint64_t made = g_alias_heaps_made, moved = g_alias_migrated;
        id<MTLTexture> px = alias_probe_clear_read(&t[KX], rb, 5.0f / 8.0f, 0);
        id<MTLHeap> up = alias_probe_heap(px);
        if (g_alias_heaps_made - made != 1 || g_alias_migrated - moved != SMALL + 2) MDIE("room probe: growing up made %llu heaps and moved %llu textures", (unsigned long long)(g_alias_heaps_made - made), (unsigned long long)(g_alias_migrated - moved));
        { NSUInteger want = region + 3 * (NSUInteger)x / 4 + ALIAS_ROOM_MIN;
          if (up.size < want || up.size > want + 65536) MDIE("room probe: %lu bytes after growing up, expected %lu", (unsigned long)up.size, (unsigned long)want); }
        checked += 2;
        alias_probe_read(&t[KQ], rb, 7.0f / 8.0f); checked++;
        made = g_alias_heaps_made; moved = g_alias_migrated;
        id<MTLTexture> py = alias_probe_clear_read(&t[KY], rb, 6.0f / 8.0f, 0);
        if (alias_probe_heap(py) != up || g_alias_heaps_made != made || g_alias_migrated != moved) MDIE("room probe: the texture in the room above replaced the region again");
        if (alias_probe_offset(py) - alias_probe_offset(px) != (NSUInteger)(t[KY].mem - t[KX].mem)) MDIE("room probe: offsets above do not follow guest offsets");
        checked += 2;
        NSUInteger before = up.size; up = nil; px = nil; py = nil;
        id<MTLTexture> pw = alias_probe_clear_read(&t[KW], rb, 1.0f / 8.0f, 0);
        id<MTLHeap> down = alias_probe_heap(pw);
        if (g_alias_heaps_made - made != 1) MDIE("room probe: growing down made %llu heaps", (unsigned long long)(g_alias_heaps_made - made));
        { NSUInteger want = before + 3 * (NSUInteger)x / 4 + ALIAS_ROOM_MIN;
          if (down.size < want || down.size > want + 65536) MDIE("room probe: %lu bytes after growing down, expected %lu", (unsigned long)down.size, (unsigned long)want); }
        if (alias_probe_offset(pw) != ALIAS_ROOM_MIN) MDIE("room probe: the texture below is at offset %lu, expected the room %llu", (unsigned long)alias_probe_offset(pw), (unsigned long long)ALIAS_ROOM_MIN);
        checked += 3;
        made = g_alias_heaps_made; moved = g_alias_migrated;
        id<MTLTexture> pv = alias_probe_clear_read(&t[KV], rb, 2.0f / 8.0f, 0);
        if (alias_probe_heap(pv) != down || g_alias_heaps_made != made || g_alias_migrated != moved) MDIE("room probe: the texture in the room below replaced the region again");
        checked++;
        /* W wrote the region's first bytes, the witness's own; Y, written before the region grew down, is far from them */
        mtl_sub_begin(); mtl_exec((const uint8_t *)&end);
        { id<MTLTexture> q = tex_mtl(&t[KQ]); if (alias_probe_heap(q) != down) MDIE("room probe: the witness did not move with its region"); }
        mtl_sub_commit(); mtl_finish();
        alias_probe_read(&t[KY], rb, 6.0f / 8.0f);
        checked += 2;
        down = nil; pw = nil; pv = nil;
        for (int i = 0; i < KN; i++) mtl_tex_release(&t[i]);
        stor_drop_range((uintptr_t)mem, (uintptr_t)mem + span);
        for (int round = 0; round < 4; round++) { mtl_sub_begin(); mtl_exec((const uint8_t *)&end); mtl_sub_commit(); mtl_finish(); defer_drain(); }
        stor_alias_collect(); free(mem);
    }
    {   /* 1000x561 and 720x405: sizes like the game's scaled targets, whose Metal size with lossless compression fits their
           guest size, so they keep it on a region heap (the 512x512 above does not) */
        enum { PAIRS = 12 }; const size_t stride = 4u << 20, span = PAIRS * stride; void *mem = NULL;
        if (posix_memalign(&mem, (size_t)getpagesize(), span)) MDIE("order probe memory");
        memset(mem, 0, span);
        id<MTLBuffer> big = probe_buf(1000 * 561 * 4);
        static tex_t t[PAIRS][3]; memset(t, 0, sizeof t);   // S, L, G
        for (int k = 0; k < PAIRS; k++) for (int i = 0; i < 3; i++) {
            t[k][i].serial = UINT64_C(0xfffd5600) + (uint64_t)(k * 3 + i);
            t[k][i].d = (nvn_texdesc){.width = i == 0 ? 1000 : i == 1 ? 720 : 256, .height = i == 0 ? 561 : i == 1 ? 405 : 256, .depth = 1, .levels = 1, .target = 1, .format = 0x25,
                                      .swizzle_r = 2, .swizzle_g = 3, .swizzle_b = 4, .swizzle_a = 5};
            nvn_tex_layout(&t[k][i].d, &t[k][i].L);
            t[k][i].mem = (uint8_t *)mem + (uint64_t)k * stride;
        }
        int optimized = 0;
        for (int k = 0; k < PAIRS; k++) {
            id<MTLTexture> ms = alias_probe_clear_read(&t[k][0], big, 2.0f / 8.0f, 0), ml = alias_probe_clear_read(&t[k][1], big, 6.0f / 8.0f, 0);
            id<MTLHeap> heap = alias_probe_heap(ms);
            if (!heap || alias_probe_heap(ml) != heap) MDIE("order probe: pair %d is not on one region heap", k);
            if (heap.size < 131072 || heap.size + (192u << 10) > stride) MDIE("order probe: pair %d on a heap of %lu bytes", k, (unsigned long)heap.size);
            t[k][2].mem += heap.size - 65536;   // G: a quarter inside the pair's region, the rest past its top
            optimized += ms.allowGPUOptimizedContents + ml.allowGPUOptimizedContents;
        }
        uint64_t moved = g_alias_migrated;
        for (int k = 0; k < PAIRS; k++) alias_probe_clear_read(&t[k][2], rb, 4.0f / 8.0f, 0);
        if (g_alias_migrated - moved != 2 * PAIRS) MDIE("order probe: %llu textures moved, expected %d", (unsigned long long)(g_alias_migrated - moved), 2 * PAIRS);
        checked++;
        for (int k = 0; k < PAIRS; k++) alias_probe_read(&t[k][1], big, 6.0f / 8.0f);   // MDIEs on the first texel that is not the later layout's
        checked++;
        MLOG("order probe: %d pairs moved, the layout made last reads as written in each (%d of %d textures keep lossless compression)", PAIRS, optimized, 2 * PAIRS);
        res_remove(big);
        for (int k = 0; k < PAIRS; k++) for (int i = 0; i < 3; i++) mtl_tex_release(&t[k][i]);
        stor_drop_range((uintptr_t)mem, (uintptr_t)mem + span);
        for (int round = 0; round < 4; round++) { mtl_sub_begin(); mtl_exec((const uint8_t *)&end); mtl_sub_commit(); mtl_finish(); defer_drain(); }
        stor_alias_collect(); free(mem);
    }
    if (g_alias_heaps_live != heaps0) MDIE("room and order probe: %llu heaps live after the pools were finalised", (unsigned long long)(g_alias_heaps_live - heaps0));
    checked++;
    res_remove(rb);
    MLOG("room and order probe passed: growth with room on both sides without a second replacement, migration in the order the layouts were made");
    return checked;
}
int mtl_probe_stor_alias(void) {
    mtl_init(); int checked = 0;
    if (getenv("OOT_STOR_MIGRATE_SUBRESOURCES_ONLY")) return alias_probe_migration_subresources();
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
    checked+=alias_probe_growth_migration();
    return checked+alias_probe_room_and_order();
}
