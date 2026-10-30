// General geometry replay: native VS capture -> GS compute -> ordered index
// compaction -> ordinary rendering. Included after the resource/pipeline helpers.
static NvProg *g_geo_program;
static uint64_t g_geo_indices, g_geo_index_bytes, g_geo_args;
static const p_draw *g_geo_vrange;
static uint32_t g_geo_count;
static uint64_t g_geo_hoisted, g_geo_hoist_runs;
static int geo_hoist_on(void) { static int on = -1; if (on < 0) { const char *e = getenv("DM_NVN_GEO_HOIST"); on = !e || strcmp(e, "0"); } return on; }
static id<MTLComputePipelineState> g_geo_lean_k[3];
static void draw(const p_draw *dw, const p_draw_idx *di);
static void lc_land(void);
static void sub_cut(void);

static int geo_cmp_index(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}
enum { GEO_CAPTURE_VERTICES = (256u << 20) / (144 * 4) };
// The capture span is already bounded by its scratch allocation. A bit per
// vertex gives the same ascending unique list without sorting every reference.
// Keep an in-place sort fallback for allocation failure or a wider caller.
// order may be changed; a short destination is never partially written.
static size_t geo_capture_unique(uint32_t *order, size_t count, int restart, uint32_t restart_index,
                                 uint32_t lo, uint64_t span, uint32_t *dst, size_t capacity) {
    if (!span || span > (uint64_t)UINT32_MAX + 1 - lo) return SIZE_MAX;
    for (size_t i = 0; i < count; i++) if (!(restart && order[i] == restart_index) &&
        (order[i] < lo || (uint64_t)order[i] - lo >= span)) return SIZE_MAX;
    size_t nw = (size_t)((span + 63) / 64), n = 0;
    uint64_t *bits = span <= GEO_CAPTURE_VERTICES ? calloc(nw, sizeof *bits) : NULL;
    if (bits) {
        for (size_t i = 0; i < count; i++) if (!(restart && order[i] == restart_index)) {
            uint32_t v = order[i] - lo; bits[v >> 6] |= 1ull << (v & 63);
        }
        for (size_t i = 0; i < nw; i++) n += (size_t)__builtin_popcountll(bits[i]);
        if (n <= capacity) for (size_t i = 0, k = 0; i < nw; i++) {
            uint64_t word = bits[i];
            while (word) { dst[k++] = lo + (uint32_t)(i * 64 + __builtin_ctzll(word)); word &= word - 1; }
        }
        free(bits);
    } else {
        size_t references = 0;
        for (size_t i = 0; i < count; i++) if (!(restart && order[i] == restart_index)) order[references++] = order[i];
        qsort(order, references, sizeof *order, geo_cmp_index);
        for (size_t i = 0; i < references; i++) if (!i || order[i] != order[i - 1]) n++;
        if (n <= capacity) for (size_t i = 0, k = 0; i < references; i++)
            if (!i || order[i] != order[i - 1]) dst[k++] = order[i];
    }
    return n <= capacity ? n : SIZE_MAX;
}
// Only CPU reads need a completed snapshot. Native vertex capture stays on the
// GPU and follows earlier writes through normal encoder ordering. Unknown or
// overflowing write sets always retain the conservative drain.
static int geo_cpu_hazard(uint64_t lo, uint64_t n) {
    if (!n) return 0;
    if (!lo || n > UINT64_MAX - lo || g_sbw_over) return 1;
    return pend_hits(lo, n) || sbw_hits(lo, lo + n) || gw_live(lo, lo + n);
}
static int geo_conversion_hazard(NvShader *vs) {
    for (int v = 0; v < vs->rf.nv; v++) {
        int a = vs->rf.v[v].nvn;
        if (a >= S.nattr || !vconv_wanted(&vs->rf, v, NULL, 0)) continue;
        int st = S.attr[a].stream;
        if (st < 16 && S.vbuf[st].address && geo_cpu_hazard(S.vbuf[st].address, S.vbuf[st].size)) return 1;
    }
    return 0;
}
// GS output -> counts -> offsets/indirect args -> packed indices stay in one
// compute encoder. Each transition needs dispatch ordering and device-memory
// visibility; full queue barriers remain at the encoder's boundaries.
static void geo_pack_encode(id<MTL4ComputeCommandEncoder> ce, id<MTLComputePipelineState> __strong *pack, uint64_t job, uint32_t groups) {
    enc_c_args(ce, job, 0);
    for (int i = 0; i < 3; i++) {
        [ce barrierAfterEncoderStages:MTLStageDispatch beforeEncoderStages:MTLStageDispatch visibilityOptions:MTL4VisibilityOptionDevice];
        enc_c_pso(ce, pack[i]);
        NSUInteger threads = i == 1 ? MIN((NSUInteger)256, pack[i].maxTotalThreadsPerThreadgroup) : 64;
        enc_c_dispatch(ce, i == 1 ? MTLSizeMake(1, 1, 1) : MTLSizeMake((groups + 63) / 64, 1, 1), MTLSizeMake(threads, 1, 1));
    }
}

// CPU primitive assembly may read static indices without draining unrelated GPU
// work. Unknown writes and both current/prior submission hazards retain the wait.
static int geo_index_wait(uint64_t address, uint64_t bytes) {
    static int always = -1;
    if (always < 0) { const char *e = getenv("DM_NVN_GEOMETRY_WAIT"); always = e && !strcmp(e, "always"); }
    return always || !address || !bytes || bytes > (16ull << 20) || bytes > UINT64_MAX - address ||
           g_pend_over || nvn_sparse_alias(address, address + bytes) || geo_cpu_hazard(address, bytes);
}

static id<MTLRenderPipelineState> geo_capture_pso(NvShader *vs) {
    pso_key k; static const uint32_t cf[8]; pso_key_fill(&k, vs, nil, cf, 0, 0, 1, 0); k.fsfn = GEO_MARK;
    id<MTLRenderPipelineState> ps = hm_get_id(&g_pso, &k);
    if (ps || hm_get_id(&g_pso_fail, &k)) return ps;
    NSError *e = nil;
    ps = [g_dev newRenderPipelineStateWithDescriptor:pso_desc(&k, vs, nil, nil, 0) error:&e];
    if (ps) hm_put_id(&g_pso, &k, ps);
    else { hm_put_id(&g_pso_fail, &k, NSNull.null); MLOG("[RED] geometry vertex capture %s: %s", vs->key, e.localizedDescription.UTF8String); }
    return ps;
}
// One diagnostic snapshot through the same native vertex fetch as geometry
// capture. Refuse shaders with writes: observing them must not run their side
// effects twice. Records retain the documented 144-word geometry capture ABI.
static void vertex_dump(NvShader *vs, const p_draw *dw, const p_draw_idx *di) {
    static const char *key, *path; static int configured, captured, limit; static uint64_t frame;
    if (!configured) { key = getenv("DM_NVN_VSDUMP_KEY"); path = getenv("DM_NVN_VSDUMP");
        const char *f = getenv("DM_NVN_VSDUMP_FROM"), *n = getenv("DM_NVN_VSDUMP_MAX");
        frame = f ? strtoull(f,NULL,0) : 0; limit = n ? atoi(n) : key && !strcmp(key,"*") ? 256 : 1; configured = 1; }
    int all = key && !strcmp(key,"*");
    if (captured >= limit || !key || !path || atomic_load(&g_cur_frame) < frame || !vs || (!all && strncmp(vs->key, key, 16)) || !vs->geo_fn ||
        (dw ? dw->ind || dw->mdi_cnt : di->ind || di->mdi_cnt) || S.rast_discard) return;
    for (int i = 0; i < vs->rf.nb; i++) if (vs->rf.b[i].rw) return;
    int count = dw ? dw->count : di->count, instances = dw ? dw->instances : di->instances;
    if (count <= 0 || instances > 1 || (di && (di->itype < 0 || di->itype > 2))) return;
    unsigned number = captured++; lc_land(); rp_flush(); mdi_wait();
    uint32_t lo = dw ? (uint32_t)dw->first : UINT32_MAX, hi = dw ? lo + count - 1 : 0;
    if (di) {
        int size = 1 << di->itype;
        if (!nvn_host_readable(di->ibuf, (uint64_t)count * size)) return;
        for (int i = 0; i < count; i++) { uint32_t n = 0; memcpy(&n, (void *)(uintptr_t)(di->ibuf + (uint64_t)i * size), size);
            if (S.restart.on && n == (S.restart.index & (size == 1 ? 255u : size == 2 ? 65535u : UINT32_MAX))) continue;
            lo = MIN(lo, n); hi = MAX(hi, n); }
        if (lo == UINT32_MAX || (int64_t)lo + di->base_vertex < 0 || (int64_t)hi + di->base_vertex > UINT32_MAX) return;
        lo += di->base_vertex; hi += di->base_vertex;
    }
    uint64_t span = (uint64_t)hi - lo + 1, bytes = span * 144 * 4;
    if (!span || bytes > (64u << 20)) return;
    id<MTLRenderPipelineState> ps = geo_capture_pso(vs); if (!ps) return;
    id<MTLBuffer> output; size_t offset = arena_alloc(bytes + 512, &output);
    uint64_t gpu = output.gpuAddress + offset;
    uint64_t info[4] = {gpu + 512, 0, (uint64_t)lo | span << 32, (uint32_t)(dw ? dw->base_instance : di->base_instance) | 1ull << 32};
    nvn_geo_capture gc = {info[0], 0, {(uint32_t)lo, (uint32_t)span, (uint32_t)info[3], 1}, GEO_CAPTURE_WORDS, {0}};
    for (int k = 0; k < GEO_CAPTURE_WORDS; k++) gc.slot[k] = (int16_t)k;   // every word, in order
    memcpy((uint8_t *)output.contents + offset, &gc, sizeof gc);
    vbuf_prepare();
    if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    MTL4RenderPassDescriptor *rd = [MTL4RenderPassDescriptor new]; rd.renderTargetWidth = rd.renderTargetHeight = rd.defaultRasterSampleCount = 1;
    id<MTL4RenderCommandEncoder> en = enc_r_begin(rd); enc_r_pso(en, ps); bind_vbufs(en);
    p_draw range = {.first = (int32_t)lo, .count = (int32_t)span, .instances = 1, .base_instance = (int32_t)(info[3] & UINT32_MAX)};
    vconv_bind(en, vs, 1, &range, NULL); args_dirty_all(); encode_args(en, vs, 0, 1); enc_r_vbuf(en, 13, gpu);
    enc_r_draw_instanced(en, MTLPrimitiveTypePoint, lo, span, 1, (uint32_t)info[3]); enc_r_end(en);
    mdi_wait();
    char filename[1200]; if (all) snprintf(filename,sizeof filename,"%s_%04u_%s.bin",path,number,vs->key); else strlcpy(filename,path,sizeof filename);
    FILE *f = fopen(filename, "wb"); if (f) { uint32_t header[4] = {lo, (uint32_t)span, all ? 4u : 144u, (uint32_t)info[3]}; fwrite(header, sizeof header, 1, f);
        if (all) for (uint64_t i=0;i<span;i++) fwrite((uint8_t *)output.contents + offset + 512 + i*144*4,1,16,f);
        else fwrite((uint8_t *)output.contents + offset + 512, 1, bytes, f); fclose(f); }
    MLOG("vertex dump %s: first %u, count %llu, %s", vs->key, lo, (unsigned long long)span, filename);
    args_dirty_all(); g_enc_pso = nil; g_enc_dss = nil; g_ed.ok = 0;
}
static void geo_draw(NvProg *pr, const p_draw *dw0, const p_draw_idx *di0, uint32_t primitive_base);
// A draw whose capture would need more scratch than the bound, drawn as smaller draws in the hardware's order: one instance at a time, then runs of a list topology's primitives
// (points, lines, triangles, lines and triangles with adjacency), each with its own index range. The capture lays the vertices out by vertex number from the lowest index to the highest for
// every instance, and the geometry shader's output takes room per primitive: OCTOPATH TRAVELER II draws 129,032 triangles of one mesh through a cube shadow map's geometry shader in its
// first battle, which asked for more than the bound and was skipped. The index list is read once and cut into runs of at most GEO_RUN_PRIMS primitives whose index range takes at most
// GEO_RUN_SPAN of capture; the runs skip the GPU wait their draw would take (the outer draw waited already, and nothing in between writes index or vertex data). Halving the list at every
// level instead, each part reading its indices again and waiting for the GPU, made that one frame take 805 ms. Strips, fans and primitive restart cannot be cut that way.
// Each child retains its draw-wide primitive base; only the guest PrimitiveID adds it, while capture and output addresses remain local to that child.
// Returns 0 when the draw cannot be split (the caller reports it)
enum { GEO_RUN_PRIMS = 4096 }; static const uint64_t GEO_RUN_SPAN = 64ull << 20;
static int g_geo_split_depth;   // > 0 while geo_split draws its runs: they skip the GPU wait
static int geo_sync_cpu(int *drained) {
    if (*drained) return 1;
    mdi_wait();
    if (g_mdi_ev.signaledValue < g_mdi_v) return 0;
    memset(g_vconv, 0, sizeof g_vconv); *drained = 1;
    return 1;
}
static int geo_split(NvProg *pr, const p_draw *dw, const p_draw_idx *di, int count, int instances, uint32_t first_instance, uint32_t primitive_base) {
    // geo_prep can pass a record in g_geo_run. Each recursive draw reuses that
    // job, so keep the original range independent of the child draw's record.
    const p_draw original_dw = dw ? *dw : (p_draw){0};
    const p_draw_idx original_di = di ? *di : (p_draw_idx){0};
    dw = dw ? &original_dw : NULL; di = di ? &original_di : NULL;
    int prim = dw ? dw->prim : di->prim, per = prim == 0 ? 1 : prim == 1 ? 2 : prim == 4 ? 3 : prim == 10 ? 4 : prim == 12 ? 6 : 0;
    if (!per || (di && S.restart.on) || count / per < 1) return 0;
    static uint64_t nsplit; if (nsplit++ < 4) MLOG("geometry draw above the scratch bound: drawn in parts (vertex %s, geometry %s, %d primitives, %d instances)",
                                                   pr->sh[0] ? pr->sh[0]->key : "-", pr->sh[2] ? pr->sh[2]->key : "-", count / per, instances);
    p_draw a = dw ? *dw : (p_draw){0}; p_draw_idx b = di ? *di : (p_draw_idx){0};
    g_geo_split_depth++;
    if (instances > 1) {
        for (int i = 0; i < instances; i++) {
            if (dw) { a.instances = 1; a.base_instance = (int32_t)(first_instance + i); geo_draw(pr, &a, NULL, primitive_base); }
            else { b.instances = 1; b.base_instance = (int32_t)(first_instance + i); geo_draw(pr, NULL, &b, primitive_base); }
        }
        g_geo_split_depth--; return 1;
    }
    int np = count / per, isz = di ? 1 << di->itype : 0;
    for (int p0 = 0; p0 < np;) {
        int p1 = p0; uint32_t lo = UINT32_MAX, hi = 0;
        while (p1 < np && p1 - p0 < GEO_RUN_PRIMS) {   // extend the run while its index range fits
            uint32_t l = lo, h = hi;
            for (int k = 0; k < per; k++) {
                uint32_t v = (uint32_t)(p1 * per + k);
                if (di) { v = 0; memcpy(&v, (const uint8_t *)(uintptr_t)di->ibuf + (size_t)(p1 * per + k) * isz, isz); }
                l = MIN(l, v); h = MAX(h, v);
            }
            if (p1 > p0 && ((uint64_t)h - l + 1) * 144 * 4 > GEO_RUN_SPAN) break;
            lo = l; hi = h; p1++;
        }
        if (p0 == 0 && p1 == np) {   // one run is the whole draw: its own outputs are what exceed the bound, so halve it
            p1 = np / 2; if (!p1) { g_geo_split_depth--; return 0; }
        }
        if (dw) { a.first = dw->first + p0 * per; a.count = (p1 - p0) * per; geo_draw(pr, &a, NULL, primitive_base + (uint32_t)p0); }
        else { b.ibuf = di->ibuf + ((uint64_t)(p0 * per) << di->itype); b.count = (p1 - p0) * per; geo_draw(pr, NULL, &b, primitive_base + (uint32_t)p0); }
        p0 = p1;
    }
    g_geo_split_depth--;
    return 1;
}

static void geo_draw_(NvProg *pr, const p_draw *dw0, const p_draw_idx *di0, uint32_t primitive_base);
static int g_geo_split_compute = -1, g_geo_full_colors = -1, g_geo_pack_legacy = -1;
static int geo_pack_legacy(void) { if (g_geo_pack_legacy < 0) { const char *e = getenv("DM_NVN_GEOMETRY_PACK"); g_geo_pack_legacy = e && !strcmp(e, "legacy"); } return g_geo_pack_legacy; }
static uint64_t g_geo_pack_small, g_geo_pack_large;
static uint64_t g_geo_color_omits;
// Keep CPU primitive maps and unique capture IDs, never vertex results or GPU
// output. Every reuse compares the original index bytes after the existing
// hazard/wait checks, so CPU edits, GPU writes and storage reuse all invalidate
// the result without depending on write-protection or a hash collision.
typedef struct { uint64_t address; int32_t count, type, prim, restart; uint32_t restart_index, pad; } geo_ix_key;
typedef struct geo_ix_ent {
    geo_ix_key key; uint32_t lo, hi;
    size_t nu, words, unique, raw_n, bytes;
    struct geo_ix_ent *prev, *next;
    uint8_t data[];   // original bytes, normalized primitive map, unique IDs
} geo_ix_ent;
static nvn_hmap g_geo_ix = {.klen = sizeof(geo_ix_key)};
static geo_ix_ent *g_geo_ix_first, *g_geo_ix_last;
static size_t g_geo_ix_bytes;
static uint64_t g_geo_ix_hits, g_geo_ix_misses;
static int g_geo_ix_on = -1;
enum { GEO_IX_BYTES = 32 << 20, GEO_IX_ENTRIES = 4096 };
static int geo_ix_on(void) { if (g_geo_ix_on < 0) { const char *e=getenv("DM_NVN_GEOMETRY_INDEX_CACHE");g_geo_ix_on=!e||strcmp(e,"0"); }return g_geo_ix_on; }
static void geo_ix_unlink(geo_ix_ent *e) {
    if(e->prev)e->prev->next=e->next;else g_geo_ix_first=e->next;
    if(e->next)e->next->prev=e->prev;else g_geo_ix_last=e->prev;
}
static void geo_ix_front(geo_ix_ent *e) {
    e->prev=NULL;e->next=g_geo_ix_first;if(e->next)e->next->prev=e;else g_geo_ix_last=e;g_geo_ix_first=e;
}
static void geo_ix_drop(geo_ix_ent *e) {geo_ix_unlink(e);nvn_hm_del(&g_geo_ix,&e->key);g_geo_ix_bytes-=e->bytes;free(e);}
static geo_ix_ent *geo_ix_get(const geo_ix_key *key,const void *raw,size_t raw_n) {
    if(!geo_ix_on())return NULL;
    geo_ix_ent *e=nvn_hm_get(&g_geo_ix,key);
    if(e&&e->raw_n==raw_n&&!memcmp(e->data,raw,raw_n)){geo_ix_unlink(e);geo_ix_front(e);g_geo_ix_hits++;return e;}
    if(e)geo_ix_drop(e);g_geo_ix_misses++;return NULL;
}
static void geo_ix_put(const geo_ix_key *key,const void *raw,size_t raw_n,uint32_t lo,uint32_t hi,size_t nu,const void *map,size_t words,const void *unique,size_t n_unique) {
    if(!geo_ix_on())return;
    size_t bytes=sizeof(geo_ix_ent)+raw_n+(words+n_unique)*4;
    if(bytes>GEO_IX_BYTES)return;
    geo_ix_ent *old=nvn_hm_get(&g_geo_ix,key);if(old)geo_ix_drop(old);
    while(g_geo_ix_last&&(g_geo_ix_bytes+bytes>GEO_IX_BYTES||g_geo_ix.n>=GEO_IX_ENTRIES))geo_ix_drop(g_geo_ix_last);
    geo_ix_ent *e=malloc(bytes);if(!e)return;
    *e=(geo_ix_ent){.key=*key,.lo=lo,.hi=hi,.nu=nu,.words=words,.unique=n_unique,.raw_n=raw_n,.bytes=bytes};
    memcpy(e->data,raw,raw_n);memcpy(e->data+raw_n,map,words*4);memcpy(e->data+raw_n+words*4,unique,n_unique*4);
    nvn_hm_put(&g_geo_ix,key,e);geo_ix_front(e);g_geo_ix_bytes+=bytes;
}
static int geo_color_omit(void) {
    if (g_geo_full_colors < 0) { const char *e = getenv("DM_NVN_GEOMETRY_COLOR"); g_geo_full_colors = e && !strcmp(e, "full"); }
    if (g_geo_full_colors || !R.has_rt || !R.rt.depth || R.clear_d || R.clear_s) return 0;
    // Removing an inactive color must not change raster bounds, sample count or
    // layer selection. Keep the general case on the original attachment path.
    id<MTLTexture> d = tex_mtl(R.rt.depth);
    if (!d || d.textureType != MTLTextureType2D || !mtl_fmt_depth(d.pixelFormat) || d.pixelFormat == MTLPixelFormatStencil8) return 0;
    NSUInteger dl = R.rt.has_dview && R.rt.dview.set_levels ? R.rt.dview.base_level : 0;
    NSUInteger w = mip_extent(d.width, dl), h = mip_extent(d.height, dl);
    int n = 0;
    for (int i = 0; i < R.rt.n && i < 8; i++) if (R.rt.color[i]) {
        if (R.clear_c[i] || ((S.chmask.mask >> (4 * i)) & 15)) return 0;
        id<MTLTexture> c = tex_mtl(R.rt.color[i]);
        NSUInteger l = R.rt.has_cview[i] && R.rt.cview[i].set_levels ? R.rt.cview[i].base_level : 0;
        if (!c || c.textureType != MTLTextureType2D || c.sampleCount != d.sampleCount || mip_extent(c.width, l) != w || mip_extent(c.height, l) != h) return 0;
        n++;
    }
    return n != 0;
}
// A run of geometry draws separated only by state changes: all vertex captures
// in one render pass, all GS and compaction dispatches in one compute pass,
// then the raster draws in the open render pass as replay reaches each one.
typedef struct { uint64_t lo, hi; int write; } geo_range;
typedef struct { geo_range r[64]; uint64_t writes; int n, unknown; } geo_access;
static void geo_access_add(geo_access *a, uint64_t lo, uint64_t size, int write) {
    if (!lo) return;
    if (!size || lo > UINT64_MAX-size || nvn_sparse_alias(lo, lo + size)) { a->unknown=1; return; }
    for (int i=0;i<a->n;i++) if (a->r[i].lo==lo && a->r[i].hi==lo+size) {
        a->r[i].write|=write; if(write)a->writes|=1ull<<i; return;
    }
    if (a->n==64) { a->unknown=1; return; }
    a->r[a->n]=(geo_range){lo,lo+size,write};
    if(write)a->writes|=1ull<<a->n; a->n++;
}
// A run moves every capture/GS ahead of every raster draw. Keep that ordering
// only when external buffer accesses cannot communicate between its jobs.
static geo_access geo_access_of(NvProg *pr, NvShader *gs) {
    geo_access a={0};
    for (int i=0;i<16;i++) geo_access_add(&a,S.vbuf[i].address,S.vbuf[i].size,0);
    for (int st=0;st<3;st++) {
        NvShader *sh=st==2?gs:pr->sh[st]; if (!sh) continue;
        if (sh->rf.drv_read[0]) a.unknown=1;
        for (int i=0;i<sh->rf.nb;i++) {
            const nvn_rbind *b=&sh->rf.b[i];
            if ((b->kind==NVN_RB_TEX || b->kind==NVN_RB_TEXU || b->kind==NVN_RB_TEXD || b->kind==NVN_RB_TEXA) && b->dtt==MTLTextureTypeTextureBuffer) a.unknown=1;
            if (b->kind==NVN_RB_CBUF || b->kind==NVN_RB_UBOP || b->kind==NVN_RB_SBUFP || b->kind==NVN_RB_TEXU || b->kind==NVN_RB_SAMPU) {
                if (b->reg>=16) { a.unknown=1; continue; }
                const p_bind_buf *u=b->kind==NVN_RB_SBUFP?&S.ssbo[st][b->reg]:&S.ubo[st][b->reg];
                int write=b->kind==NVN_RB_SBUFP && b->rw && !(sh->sb_ro>>b->reg&1);
                geo_access_add(&a,u->addr,b->kind==NVN_RB_CBUF?cb_len(sh,b->reg,u->size):u->size,write);
            } else if (b->kind==NVN_RB_SBUFU || b->kind==NVN_RB_IMG || b->kind==NVN_RB_TBUF) a.unknown=1;
        }
    }
    return a;
}
static int geo_access_conflict(const geo_access *a,const geo_access *b) {
    if (a->unknown || b->unknown) return 1;
    // Read/read pairs cannot communicate. Walk only A's writes against B,
    // then B's writes against A's reads; every potentially dependent pair is
    // still checked exactly once, including shared read/write aliases.
    if (!(a->writes|b->writes)) return 0;
    for(uint64_t bits=a->writes;bits;bits&=bits-1) {
        const geo_range *x=&a->r[__builtin_ctzll(bits)];
        for(int k=0;k<b->n;k++) if(x->lo<b->r[k].hi && b->r[k].lo<x->hi)return 1;
    }
    uint64_t reads=(a->n==64?UINT64_MAX:(1ull<<a->n)-1)&~a->writes;
    for(uint64_t bits=b->writes;bits;bits&=bits-1) {
        const geo_range *y=&b->r[__builtin_ctzll(bits)];
        for(uint64_t r=reads;r;r&=r-1) {
            const geo_range *x=&a->r[__builtin_ctzll(r)];
            if(x->lo<y->hi && y->lo<x->hi)return 1;
        }
    }
    return 0;
}
// A passthrough geometry shader cannot change the vertices it passes on: with input of points, lines or triangles, its lean
// kernel (dmsh_geometry.lean) reads only the capture words it decides with, writes the viewport masks and its storage, and the
// program's own vertex and fragment stages draw the kept primitives. Instanced or multi-invocation draws, vertex shaders with
// storage writes (they would run twice) and transform feedback keep the full kernel and its reader. DM_NVN_GEO_LEAN=0 turns it off.
static NvShader *geo_lean_pick(NvProg *pr, NvShader *gs, int topology, int instances) {
    static int lean_on = -1; if (lean_on < 0) { const char *e = getenv("DM_NVN_GEO_LEAN"); lean_on = !(e && !strcmp(e, "0")); }
    NvShader *vs = pr->sh[0], *base = pr->sh[2];
    if (!lean_on || instances > 1 || (gs && gs->geo_invocations != 1) || base->geo_invocations != 1 || !vs || vs->discard_fn || S.tf_on || !pr->sh[1] || !(topology == 0 || topology == 1 || topology == 3)) return NULL;
    if (!(base->geo_lean_tried >> topology & 1)) {
        base->geo_lean_tried |= 1 << topology;
        NSString *key = [[NSString stringWithUTF8String:base->key] substringToIndex:16];
        base->geo_lean_v[topology] = shader_get([key stringByAppendingFormat:@"_bl%d", topology], 2);
    }
    NvShader *lean = base->geo_lean_v[topology];
    if (!lean || !lean->geo_slot || lean->geo_vertices != (uint32_t)nvn_geo_width(topology) || lean->geo_invocations != 1) return NULL;
    if (!lean->geo_pso && !lean->geo_pso_failed) { NSError *e = nil; lean->geo_pso = [g_dev newComputePipelineStateWithFunction:lean->fn error:&e];
        if (!lean->geo_pso || lean->geo_pso.maxTotalThreadsPerThreadgroup < 32) { lean->geo_pso = nil; lean->geo_pso_failed = 1; MLOG("[RED] lean geometry compute %s: %s; the full kernel draws", lean->key, e ? e.localizedDescription.UTF8String : "no 32-thread group"); } }
    if (!lean->geo_pso) return NULL;
    if (!g_geo_lean_k[2]) {
        static int failed;
        if (failed) return NULL;
        NSError *error = nil;
        id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(k_nvn_geo_pack_msl) options:nil error:&error];
        const char *names[] = {"geo_lean_cull", "geo_lean_index", "geo_lean_tri"};
        for (int i = 0; i < 3 && lib; i++) g_geo_lean_k[i] = [g_dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@(names[i])] error:&error];
        if (!g_geo_lean_k[0] || !g_geo_lean_k[1] || !g_geo_lean_k[2]) {
            failed = 1; g_geo_lean_k[0] = g_geo_lean_k[1] = g_geo_lean_k[2] = nil;
            MLOG("[RED] lean geometry index kernels: %s", error.localizedDescription.UTF8String); return NULL;
        }
    }
    return lean;
}
// Passthrough geometry as mesh draws: a lean triangle-list draw whose pair has a mesh library, <vertex key>_<kernel key>_m3 (dm/gfx/tools/dmsh_mesh.py, built for the
// pairs in the profile's geo_mesh.tsv), is one mesh draw inside the render pass. A threadgroup per 32 triangles runs the pulling vertex shader per index and the kernel body
// per triangle, and culls what the kernel culls: no capture, no compute, no index compaction, no pass break. Its pipeline is built in the background; until it is ready, and
// for every other pair, the compute path draws. DM_NVN_GEO_MESH=0 turns it off
static int geo_mesh_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("DM_NVN_GEO_MESH"); on = !(e && !strcmp(e, "0")); if (!on) MLOG("DM_NVN_GEO_MESH=0: passthrough geometry draws through the compute path"); }
    return on;
}
enum { GEO_MESH_PRIMS = 32, GEO_MESH_DRAW_SLOT = 12, GEO_MESH_GS_SLOT = 13 };   // = dmsh_mesh.py PRIMS, DRAW_SLOT, GS_SLOT
typedef struct { uint64_t ib; uint32_t itype, first, count; int32_t base_vertex; uint32_t base_instance, pad; } geo_mesh_args;   // dmsh_mesh.py dm_mesh_draw
static struct { int on, miss; __unsafe_unretained id<MTLFunction> fn; __unsafe_unretained NvShader *lean; geo_mesh_args a; } g_geo_mesh;
// The mesh entry for this draw's program, or nil (the compute path draws it)
static id<MTLFunction> geo_mesh_fn(NvProg *pr, const p_draw *dw, const p_draw_idx *di) {
    if (!geo_mesh_on()) return nil;
    int prim = dw ? dw->prim : di->prim, instances = dw ? dw->instances : di->instances, count = dw ? dw->count : di->count;
    NvShader *vs = pr->sh[0];
    if (prim != 4 || count < 3 || instances > 1 || (dw ? dw->ind : di->ind) || (di && (S.restart.on || di->itype < 0 || di->itype > 2)) || S.rast_discard || S.tf_on ||
        !vs || !vs->has_pull || vs->layer_out || vs->discard_fn || !vs->fn || pr->sh[3] || pr->sh[4]) return nil;
    NvShader *lean = geo_lean_pick(pr, NULL, 3, instances); if (!lean) return nil;
    if (!vs->geo_mesh) vs->geo_mesh = [NSMutableDictionary dictionary];
    NSString *k = @(lean->key); id f = vs->geo_mesh[k];
    if (!f) {
        NSString *path = [NSString stringWithFormat:@"%@/%.16s_%.16s_m3.metallib", g_msl_dir, vs->key, lean->key];
        id<MTLLibrary> lib = [NSFileManager.defaultManager fileExistsAtPath:path] ? [g_dev newLibraryWithURL:[NSURL fileURLWithPath:path] error:nil] : nil;
        f = lib ? [lib newFunctionWithName:[vs->fn.name isEqualToString:@"main0_z"] ? @"dm_mesh_z" : @"dm_mesh"] : nil;
        static int n; if (f || n++ < 16) MLOG("geometry mesh: vertex %s kernel %s %s", vs->key, lean->key, f ? "has a mesh library" : "has no mesh library (geo_mesh.tsv): the compute path draws it");
        vs->geo_mesh[k] = f = f ?: NSNull.null;
    }
    return f == NSNull.null ? nil : f;
}
static nvn_hmap g_pso_mesh = {.klen = sizeof(pso_key)}; static os_unfair_lock g_pso_mesh_lock = OS_UNFAIR_LOCK_INIT;
static NSObject *g_pso_mesh_building;
// The draw's mesh pipeline, or nil while it is built in the background (or when it failed)
static id<MTLRenderPipelineState> geo_mesh_pso(NvProg *pr) {
    NvShader *vs = pr->sh[0], *fs = pr->sh[1];
    pso_key k; pso_key_fill(&k, vs, fs, g_pass.cfmt, g_pass.dfmt, g_pass.sfmt, g_pass.samples, 0);
    memset(k.attr, 0, sizeof k.attr); memset(k.stream, 0, sizeof k.stream);   // the pipeline pulls its streams: no vertex layout in it
    k.fsfn = (__bridge void *)g_geo_mesh.fn;
    os_unfair_lock_lock(&g_pso_mesh_lock); id ps = hm_get_id(&g_pso_mesh, &k);
    if (!ps) { if (!g_pso_mesh_building) g_pso_mesh_building = [NSObject new]; hm_put_id(&g_pso_mesh, &k, g_pso_mesh_building); }
    os_unfair_lock_unlock(&g_pso_mesh_lock);
    if (!ps) {
        MTLRenderPipelineDescriptor *d = pso_desc(&k, vs, fs, nil, 0);
        MTLMeshRenderPipelineDescriptor *m = [MTLMeshRenderPipelineDescriptor new];
        m.meshFunction = g_geo_mesh.fn; m.fragmentFunction = d.fragmentFunction;
        m.rasterSampleCount = d.rasterSampleCount; m.alphaToCoverageEnabled = d.alphaToCoverageEnabled;
        for (int i = 0; i < 8; i++) m.colorAttachments[i] = d.colorAttachments[i];
        m.depthAttachmentPixelFormat = d.depthAttachmentPixelFormat; m.stencilAttachmentPixelFormat = d.stencilAttachmentPixelFormat;
        const char *vk = vs->key, *fk = fs ? fs->key : "-", *gk = g_geo_mesh.lean->key; pso_key kk = k;
        dispatch_async(pp_bg_queue(), ^{
            NSError *e = nil; uint64_t t0 = bp_now();
            id<MTLRenderPipelineState> p = [g_dev newRenderPipelineStateWithMeshDescriptor:m options:MTLPipelineOptionNone reflection:nil error:&e];
            MLOG("%sgeometry mesh pipeline: vertex %s fragment %s kernel %s, %.1f ms in the background%s%s", p ? "" : "[RED] ", vk, fk, gk, (bp_now() - t0) / 1e6, p ? "" : ": ", p ? "" : e.localizedDescription.UTF8String);
            os_unfair_lock_lock(&g_pso_mesh_lock); hm_put_id(&g_pso_mesh, &kk, p ?: NSNull.null); os_unfair_lock_unlock(&g_pso_mesh_lock);
        });
        return nil;
    }
    return ps == NSNull.null || ps == g_pso_mesh_building ? nil : ps;
}
// The draw itself (draw() calls it with its pipeline, the vertex and fragment arguments and the pulling layout bound): the kernel's arguments, with vb_info
// m0 = (0, 1 instance) and m1.x = the draw's triangles, and the draw's index source
static void geo_mesh_encode(id<MTLRenderCommandEncoder> en_) {
    id<MTL4RenderCommandEncoder> en = (id<MTL4RenderCommandEncoder>)en_;
    [en setArgumentTable:g_vt atStages:MTLRenderStageMesh];
    uint32_t tris = g_geo_mesh.a.count / 3;
    id<MTLBuffer> b; size_t o = arena_alloc(64, &b); uint8_t *p = (uint8_t *)b.contents + o;
    uint32_t info[8] = {0, 1, 0, 0, tris, 0, 0, 0}; memcpy(p, info, 32); memcpy(p + 32, &g_geo_mesh.a, sizeof g_geo_mesh.a);
    enc_r_vbuf(en, GEO_MESH_DRAW_SLOT, b.gpuAddress + o + 32);
    uint64_t save[5]; memcpy(save, g_geo_bind, sizeof save); memset(g_geo_bind, 0, sizeof g_geo_bind); g_geo_bind[3] = b.gpuAddress + o;
    g_args_dirty[2] = 1; g_args_mesh = GEO_MESH_GS_SLOT; encode_args(en, g_geo_mesh.lean, 2, 0); g_args_mesh = 0; g_args_dirty[2] = 1;
    memcpy(g_geo_bind, save, sizeof save);
    [en drawMeshThreadgroups:MTLSizeMake((tris + GEO_MESH_PRIMS - 1) / GEO_MESH_PRIMS, 1, 1) threadsPerObjectThreadgroup:MTLSizeMake(1, 1, 1) threadsPerMeshThreadgroup:MTLSizeMake(3 * GEO_MESH_PRIMS, 1, 1)];
}
static uint64_t g_geo_mesh_n, g_geo_mesh_wait;   // mesh draws; draws the compute path drew while their mesh pipeline was built
static int geo_mesh_draw(NvProg *pr, const p_draw *dw, const p_draw_idx *di) {
    id<MTLFunction> fn = geo_mesh_fn(pr, dw, di); if (!fn) return 0;
    uint64_t ib = 0; void *mb; uint64_t mo;
    if (di && !mtl_gpu_addr(di->ibuf, 1, &mb, &mo, &ib)) return 0;
    uint32_t bi = (uint32_t)(dw ? dw->base_instance : di->base_instance);
    NvProg *raster = pr->geo_lean_raster;
    if (!raster || raster->sh[0] != pr->sh[0] || raster->sh[1] != pr->sh[1]) { raster = [NvProg new]; raster->sh[0] = pr->sh[0]; raster->sh[1] = pr->sh[1]; pr->geo_lean_raster = raster; }
    g_geo_mesh.on = 1; g_geo_mesh.miss = 0; g_geo_mesh.fn = fn; g_geo_mesh.lean = pr->sh[2]->geo_lean_v[3];
    g_geo_mesh.a = (geo_mesh_args){ib, di ? 1u << di->itype : 0, dw ? (uint32_t)dw->first : 0, (uint32_t)(dw ? dw->count : di->count), di ? di->base_vertex : 0, bi, 0};
    g_geo_program = raster;
    p_draw synthetic = {.prim = 4, .count = 3, .instances = 1, .base_instance = (int32_t)bi};
    g_geo_omit_colors = geo_color_omit();
    draw(&synthetic, NULL);
    g_geo_omit_colors = 0;
    g_geo_program = nil; g_geo_mesh.on = 0; args_dirty_all();
    if (g_geo_mesh.miss) { g_geo_mesh_wait++; return 0; }
    swrite_note(pr); udc_note_draw(pr); g_wr_epoch++; g_mdx_epoch++;   // the kernel's storage writes
    if (!g_geo_mesh_n++) MLOG("geometry mesh: first mesh draw, vertex %s kernel %s, %u triangles", pr->sh[0]->key, g_geo_mesh.lean->key, g_geo_mesh.a.count / 3);
    return 1;
}
typedef struct {
    const uint8_t *op;
    NvProg *pr; NvShader *vs, *gs, *reader;
    id<MTLRenderPipelineState> capture;
    p_draw dw, vertex_range; p_draw_idx di; int has_di;
    uint32_t first_instance, groups, emits;
    uint64_t np; size_t unique_count, nu;
    uint64_t input, output, raw, packed, packed_size, cnt, map, capture_ix, info, capture_info, args, job;
    geo_access access;
    uint64_t lean_job;
    uint32_t capture_words, direct_count;
    int lean;
} geo_job;
enum { GEO_RUN = 64 };
static geo_job g_geo_run[GEO_RUN];
static int g_geo_nrun;
static uint64_t g_geo_dependency_splits;
static int g_geo_query_run = -1;
static int g_geo_cut_on = -1;
static int geo_cut_on(void) { if(g_geo_cut_on<0){const char *e=getenv("DM_NVN_GEOMETRY_CUT");g_geo_cut_on=!e||atoi(e);}return g_geo_cut_on; }
static int geo_query_run(void) {
    if (g_geo_query_run < 0) { const char *e = getenv("DM_NVN_GEOMETRY_QUERY_RUN"); g_geo_query_run = !e || strcmp(e, "0"); }
    return g_geo_query_run;
}
// 1: j is ready; 0: this draw draws nothing; -1: cannot join an open run.
static int geo_prep(NvProg *pr, const p_draw *dw0, const p_draw_idx *di0, geo_job *j, int may_wait, uint32_t primitive_base) {
    *j = (geo_job){.pr = pr, .vs = pr ? pr->sh[0] : nil, .gs = pr ? pr->sh[2] : nil,
                   .dw = dw0 ? *dw0 : (p_draw){0}, .di = di0 ? *di0 : (p_draw_idx){0}, .has_di = di0 != NULL};
    NvShader *vs = j->vs, *base = j->gs;
    if (!vs || !vs->geo_fn || !base || !base->geo_vertices || pr->sh[3] || pr->sh[4]) {
        if (!may_wait) return -1;
        MLOG("[RED] geometry draw cannot capture this pipeline (vertex %s, geometry %s, tessellation %d, feedback %d)", vs ? vs->key : "-", base ? base->key : "-", !!(pr->sh[3] || pr->sh[4]), S.tf_on); return 0;
    }
    if (di0 && (j->di.itype < 0 || j->di.itype > 2)) { if (may_wait) MLOG("[RED] geometry index type %d is invalid", j->di.itype); return 0; }
    uint64_t ind = dw0 ? j->dw.ind : j->di.ind;
    // A run may not read indirect records or prepare an unaligned stream ahead
    // of intervening draws. The single-draw path keeps main's ordered copies.
    if (!may_wait) {
        if (ind) return -1;
        for (int i = 0; i < 16; i++) if (S.vbuf[i].address & 3) return -1;
    }
    int drained = g_geo_split_depth != 0;
    if (geo_conversion_hazard(vs) || (ind && geo_cpu_hazard(ind, dw0 ? 16 : 20))) {
        if (!may_wait) return -1;
        if (!geo_sync_cpu(&drained)) return 0;
    }
    if (ind) {
        uint32_t a[5] = {0}; size_t n = dw0 ? 16 : 20;
        if (!nvn_host_readable(ind, n)) { MLOG("[RED] geometry indirect arguments are not readable"); return 0; }
        memcpy(a, (void *)(uintptr_t)ind, n);
        if (!a[0] || !a[1]) return 0;
        if (a[0] > INT32_MAX || a[1] > INT32_MAX) { MLOG("[RED] geometry indirect draw exceeds the host count range"); return 0; }
        if (dw0) { j->dw.count = a[0]; j->dw.instances = a[1]; j->dw.first = a[2]; j->dw.base_instance = a[3]; j->dw.ind = j->dw.mdi_cnt = 0; }
        else { j->di.count = a[0]; j->di.instances = a[1]; j->di.ibuf += (uint64_t)a[2] << j->di.itype; j->di.base_vertex = (int32_t)a[3]; j->di.base_instance = a[4]; j->di.ind = j->di.mdi_cnt = 0; }
    }
    int prim = dw0 ? j->dw.prim : j->di.prim, topology = nvn_geo_topology(prim);
    int count = dw0 ? j->dw.count : j->di.count, instances = dw0 ? j->dw.instances : j->di.instances;
    uint32_t first_instance = (uint32_t)(dw0 ? j->dw.base_instance : j->di.base_instance);
    g_draw_binst = first_instance;
    if (count <= 0 || instances < 0) return 0;
    if (!instances) instances = 1;
    if (topology < 0 || count > (1 << 22)) { if (may_wait) MLOG("[RED] geometry primitive %d or count %d exceeds the assembly path", prim, count); return 0; }
    if (!drained && di0 && geo_index_wait(j->di.ibuf, (uint64_t)count << j->di.itype)) {
        if (!may_wait) return -1;
        if (!geo_sync_cpu(&drained)) return 0;
    }
    if (cp_on()) { if (drained) g_cp_geo_waited++; else g_cp_geo_safe++; }
    NSString *key = [[NSString stringWithUTF8String:base->key] substringToIndex:16];
    NvShader *gs = shader_get([key stringByAppendingFormat:@"_bg%d", topology], 2);
    NvShader *reader = shader_get([key stringByAppendingString:@"_bgv"], 0);
    if (!gs || !reader) return 0;
    j->access=geo_access_of(pr,gs);
    NvShader *lean = geo_lean_pick(pr, gs, topology, instances);
    if (lean) gs = lean;
    uint32_t record = lean ? gs->geo_words : GEO_CAPTURE_WORDS;
    id<MTLRenderPipelineState> capture = geo_capture_pso(vs); if (!capture) return 0;
    if (!gs->geo_pso) { NSError *e = nil; gs->geo_pso = [g_dev newComputePipelineStateWithFunction:gs->fn error:&e];
        if (!gs->geo_pso) { MLOG("[RED] geometry compute %s: %s", gs->key, e.localizedDescription.UTF8String); return 0; } }
    if (gs->geo_pso.maxTotalThreadsPerThreadgroup < 32) { MLOG("[RED] geometry kernel cannot run a 32-thread group"); return 0; }

    uint32_t lo = UINT32_MAX, hi = 0; size_t nu = 0;
    int restart = di0 && S.restart.on;
    int isz = di0 ? 1 << j->di.itype : 0;
    uint32_t restart_index = S.restart.index & (isz == 1 ? UINT8_MAX : isz == 2 ? UINT16_MAX : UINT32_MAX);
    if (di0 && (j->di.itype < 0 || j->di.itype > 2 || !nvn_host_readable(j->di.ibuf, (uint64_t)count * isz))) {
        if (may_wait) MLOG("[RED] geometry index buffer is not readable"); return 0;
    }
    size_t raw_n=(size_t)count*isz;
    geo_ix_key ixkey={.address=j->di.ibuf,.count=count,.type=j->di.itype,.prim=prim,.restart=restart,.restart_index=restart?restart_index:0};
    geo_ix_ent *cached=di0?geo_ix_get(&ixkey,(const void *)(uintptr_t)j->di.ibuf,raw_n):NULL;
    uint32_t *order=NULL;const uint8_t *source=(const uint8_t *)(uintptr_t)j->di.ibuf;
    int keep=di0&&geo_ix_on();
    if(cached){lo=cached->lo;hi=cached->hi;nu=cached->nu;}
    else {
      order=malloc((size_t)count*4+(keep?raw_n:0));
      if(!order){MLOG("[RED] geometry index allocation failed");return 0;}
      if(keep){uint8_t *snapshot=(uint8_t *)(order+count);memcpy(snapshot,source,raw_n);source=snapshot;}
      for (int i = 0; i < count; i++) {
        uint32_t v = (uint32_t)i;
        if (di0) { v = 0; memcpy(&v, source + (size_t)i * isz, isz); }
        order[i] = v;
        if (restart && v == restart_index) continue;
        lo = MIN(lo, v); hi = MAX(hi, v); nu++;
      }
    }
    if (!nu) { free(order); return 0; }
    int64_t first = di0 ? (int64_t)lo + j->di.base_vertex : j->dw.first;
    uint64_t span = (uint64_t)hi - lo + 1;
    if (first < 0 || first + span > (uint64_t)UINT32_MAX + 1) { free(order); if (may_wait) MLOG("[RED] geometry vertex range is outside 32-bit vertex IDs"); return 0; }
    // Normalize only after assembly, so a restart value cannot collide with a
    // normalized vertex number. Indexed captures use a sorted unique index list.
    size_t words = cached?cached->words:nvn_geo_assemble(prim, order, count, restart, restart_index, NULL, 0);
    int width = nvn_geo_width(topology);
    uint64_t np = words == SIZE_MAX ? 0 : words / width;
    if (!np) { free(order); return 0; }
    uint64_t groups = np * (uint64_t)instances * gs->geo_invocations;
    if (groups > UINT32_MAX || span * (uint64_t)instances > (256ull << 20) / (MAX(record, 1u) * 4) ||
        (uint64_t)first_instance + instances > (uint64_t)UINT32_MAX + 1) {
        free(order);
        if (may_wait && !geo_split(pr, dw0 ? &j->dw : NULL, dw0 ? NULL : &j->di, count, instances, first_instance, primitive_base))
            MLOG("[RED] geometry draw exceeds the capture or invocation bound");
        return may_wait ? 0 : -1;
    }
    uint32_t emits = gs->geo_index_stride ? gs->geo_vertices : 0;   // 0: the GS writes storage buffers only
    uint64_t input_size = MAX(span * instances * record * 4, 4ull);
    uint64_t output_size = emits ? groups * emits * gs->geo_stride * 4 : 4;
    uint64_t raw_size = emits ? groups * gs->geo_index_stride * 4 : 4;
    uint64_t packed_size = emits ? groups * emits * (gs->geo_output == 7 ? 3 : gs->geo_output == 6 ? 2 : 1) * 4 : 4;
    uint64_t count_size = emits ? (groups + (groups + 63) / 64) * 4 : 4;
    uint64_t total = input_size + output_size + raw_size + packed_size + count_size + words * 4 + nu * 4 + sizeof(nvn_geo_capture) + sizeof(nvn_geo_pack_job) + sizeof(nvn_geo_lean_job) + 32 + 20 + 12 * 255;
    if (groups > UINT32_MAX || total > (256ull << 20) || np > UINT32_MAX) {
        free(order);
        if (may_wait && !geo_split(pr, dw0 ? &j->dw : NULL, dw0 ? NULL : &j->di, count, instances, first_instance, primitive_base))
            MLOG("[RED] geometry draw needs %llu MiB of scratch, above the 256 MiB bound", (unsigned long long)(total >> 20));
        return may_wait ? 0 : -1;
    }
    id<MTLBuffer> scratch; size_t off = arena_alloc((size_t)total, &scratch);
    uint8_t *cpu = (uint8_t *)scratch.contents + off; uint64_t gpu = scratch.gpuAddress + off;
    size_t used = 0;
    #define GEO_ALLOC(N, G, P) do { used = (used + 255) & ~(size_t)255; (G) = gpu + used; (P) = cpu + used; used += (size_t)(N); } while (0)
    void *pinput, *poutput, *praw, *ppacked, *pcnt, *pmap, *pcapture_ix, *pinfo, *pcapture_info, *pargs, *pjob, *plean_job;
    GEO_ALLOC(input_size, j->input, pinput); GEO_ALLOC(output_size, j->output, poutput); GEO_ALLOC(raw_size, j->raw, praw); GEO_ALLOC(packed_size, j->packed, ppacked); GEO_ALLOC(count_size, j->cnt, pcnt);
    GEO_ALLOC(words * 4, j->map, pmap); GEO_ALLOC(nu * 4, j->capture_ix, pcapture_ix); GEO_ALLOC(32, j->info, pinfo); GEO_ALLOC(sizeof(nvn_geo_capture), j->capture_info, pcapture_info);
    GEO_ALLOC(20, j->args, pargs); GEO_ALLOC(sizeof(nvn_geo_pack_job), j->job, pjob);
    GEO_ALLOC(sizeof(nvn_geo_lean_job), j->lean_job, plean_job);
    #undef GEO_ALLOC
    (void)pinput; (void)poutput; (void)praw; (void)ppacked; (void)pcnt;
    if (used > total) MDIE("geometry scratch size accounting");
    size_t unique_count = 0;
    if(cached){memcpy(pmap,cached->data+raw_n,words*4);unique_count=cached->unique;memcpy(pcapture_ix,cached->data+raw_n+words*4,unique_count*4);}
    else {
      nvn_geo_assemble(prim, order, count, restart, restart_index, pmap, words);
      for (size_t i = 0; i < words; i++) ((uint32_t *)pmap)[i] -= lo;
      if (di0) {
        unique_count = geo_capture_unique(order, count, restart, restart_index, lo, span, pcapture_ix, nu);
        if (unique_count == SIZE_MAX) { free(order); MLOG("[RED] geometry capture indices exceed their range or allocation"); return 0; }
      }
      if(keep)geo_ix_put(&ixkey,source,raw_n,lo,hi,nu,pmap,words,pcapture_ix,unique_count);
    }
    free(order);
    // geometry_counts.y is the guest primitive base, independent of local maps.
    uint32_t vi[8] = {(uint32_t)span, (uint32_t)instances, 0, 0, (uint32_t)np, primitive_base, 0, 0}; memcpy(pinfo, vi, 32);
    nvn_geo_capture capture_info = {j->input, 0, {(uint32_t)first, (uint32_t)span, first_instance, (uint32_t)instances}, MAX(record, 1u), {0}};
    for (int k = 0; k < GEO_CAPTURE_WORDS; k++) capture_info.slot[k] = lean ? gs->geo_slot[k] : (int16_t)k;
    memcpy(pcapture_info, &capture_info, sizeof capture_info);
    memset(pargs, 0, 20);
    nvn_geo_pack_job pj = {j->raw, j->packed, j->args, (uint32_t)groups, gs->geo_index_stride, gs->geo_output, (uint32_t)(packed_size / 4), j->cnt};
    memcpy(pjob, &pj, sizeof pj);
    nvn_geo_lean_job lj = {j->raw, j->output, j->map, j->packed, j->args, (uint32_t)groups, gs->geo_index_stride,
                          (uint32_t)first, first_instance, (uint32_t)(packed_size / 4), (uint32_t)first};
    memcpy(plean_job, &lj, sizeof lj);
    j->lean = lean != nil; j->capture_words = record;
    j->direct_count = lean && topology == 3 ? (uint32_t)(np * 3) : 0;
    j->gs = gs; j->reader = lean ? vs : reader; j->capture = capture; j->dw.instances = instances; j->di.instances = instances;
    j->vertex_range = (p_draw){.first = (int32_t)first, .count = (int32_t)span, .instances = instances, .base_instance = (int32_t)first_instance};
    j->first_instance = first_instance; j->groups = (uint32_t)groups; j->emits = emits; j->np = np; j->unique_count = unique_count; j->nu = nu; j->packed_size = packed_size;
    for (int i = 0; i < gs->rf.nb; i++) {
        const nvn_rbind *b = &gs->rf.b[i];
        if (b->kind == NVN_RB_SBUFP && b->rw && b->reg < 16 && S.ssbo[2][b->reg].addr)
            sbw_note(S.ssbo[2][b->reg].addr, S.ssbo[2][b->reg].size, SBW_GPU);
    }
    if (may_wait) vbuf_prepare();
    return 1;
}
static void geo_capture_one(id<MTL4RenderCommandEncoder> re, const geo_job *j) {
    if (!j->capture_words) return;
    g_draw_binst = j->first_instance;
    enc_r_pso(re, j->capture); bind_vbufs(re);
    vconv_bind(re, j->vs, 1, &j->vertex_range, NULL);
    args_dirty_all(); encode_args(re, j->vs, 0, 1); enc_r_vbuf(re, 13, j->capture_info);
    if (j->has_di) enc_r_draw_indexed(re, MTLPrimitiveTypePoint, j->unique_count, MTLIndexTypeUInt32, j->capture_ix, j->nu * 4, j->di.instances, j->di.base_vertex, j->first_instance);
    else enc_r_draw_instanced(re, MTLPrimitiveTypePoint, j->dw.first, j->dw.count, j->dw.instances, j->first_instance);
}
static void geo_gs_one(id<MTL4ComputeCommandEncoder> ce, const geo_job *j) {
    g_draw_binst = j->first_instance;
    g_geo_bind[0] = j->input; g_geo_bind[1] = j->output; g_geo_bind[2] = j->raw; g_geo_bind[3] = j->info; g_geo_bind[4] = j->map;
    args_dirty_all(); encode_args(ce, j->gs, 2, 0); enc_c_pso(ce, j->gs->geo_pso);
    enc_c_dispatch(ce, MTLSizeMake((j->np + 31) / 32, j->dw.instances, j->gs->geo_invocations), MTLSizeMake(32, 1, 1));
    if (j->lean) {
        [ce barrierAfterEncoderStages:MTLStageDispatch beforeEncoderStages:MTLStageDispatch visibilityOptions:MTL4VisibilityOptionDevice];
        enc_c_args(ce, j->lean_job, 0); enc_c_pso(ce, g_geo_lean_k[j->direct_count ? 2 : 0]);
        enc_c_dispatch(ce, MTLSizeMake((j->groups + 63) / 64, 1, 1), MTLSizeMake(64, 1, 1));
    }
    swrite_note(j->pr); udc_note_draw(j->pr); g_wr_epoch++; g_mdx_epoch++;
}
static void geo_raster(const geo_job *j, int last_in_run) {
    g_geo_bind[1] = j->output;
    if (S.tf_on) tf_geometry(j->reader, j->packed, j->args, j->gs->geo_output == 1 ? 1 : j->gs->geo_output == 6 ? 2 : 3, j->first_instance);
    if (S.rast_discard || !j->emits) { if (last_in_run && g_enc_geo_nocolor) rp_end(); return; }
    NvProg *pr = j->pr, *raster = pr->geo_raster;
    if (!raster || raster->sh[0] != j->reader || raster->sh[1] != pr->sh[1]) {
        raster = [NvProg new]; raster->sh[0] = j->reader; raster->sh[1] = pr->sh[1]; pr->geo_raster = raster;
    }
    g_geo_vrange = j->lean ? &j->vertex_range : NULL; g_geo_count = j->direct_count;
    g_geo_program = raster; g_geo_indices = j->packed; g_geo_index_bytes = j->packed_size; g_geo_args = j->args;
    g_geo_omit_colors = !(j->reader->layer_out) && geo_color_omit();
    if (g_geo_omit_colors) g_geo_color_omits++;
    args_dirty_all();
    p_draw synthetic = {.prim = j->gs->geo_output == 1 ? 0 : j->gs->geo_output == 6 ? 1 : 4, .count = 3, .instances = 1, .base_instance = (int32_t)j->first_instance};
    draw(&synthetic, NULL);
    g_geo_omit_colors = 0; g_geo_program = nil; g_geo_vrange = NULL; g_geo_count = 0; args_dirty_all();
    // Ordinary draws may enable color writes, so they must open their own pass.
    if (last_in_run && g_enc_geo_nocolor) rp_end();
    static int once; if (!once++) MLOG("geometry compute: vertex %s, geometry %s, %llu input primitives, %d instances", j->vs->key, j->gs->key, (unsigned long long)j->np, j->dw.instances);
}
// Query reports/resets remain at their original places in raster replay. Each
// queried draw selects a separate visibility slot in the shared depth pass;
// vertex capture and GS compute do not contribute raster samples to a query.
static int geo_run_passes(uint32_t op, const void *a) {
    // Feedback offsets and saved counts change at their original replay point.
    if (op == OP_TF_STATE || op == OP_TF_BIND) return 0;
    if (op == OP_REPORT_COUNTER) {
        uint64_t t, ad; memcpy(&t, a, 8); memcpy(&ad, (const uint8_t *)a + 8, 8);
        if (t == NVN_COUNTER_TYPE_SAMPLES_PASSED && !geo_query_run()) return 0;
        sbw_note(ad, 16, SBW_REPORT); return 1;
    }
    if (op == OP_RESET_COUNTER) { uint64_t t; memcpy(&t, a, 8); return t != NVN_COUNTER_TYPE_SAMPLES_PASSED || geo_query_run(); }
    if (op == OP_SIGNAL_EVENT) return 1;
    return state_op(op, a);
}
static int geo_run_walk(id<MTL4RenderCommandEncoder> re) {
    int n = 1;
    if (!geo_hoist_on() || S.tf_on || g_geo_split_depth || g_defer_replaying || !g_exec_next || g_ret_n > 8) return n;
    typeof(S) s0 = S; typeof(R) r0 = R; int pd = g_pso_dirty, dd = g_dss_dirty;
    fz_cursor cur = {.p = g_exec_next, .ret_n = g_ret_n}; memcpy(cur.ret, g_ret, sizeof(*g_ret) * (size_t)g_ret_n);
    const uint8_t *p; op_hdr h; const void *a;
    for (int guard = 0; guard < 10000 && n < GEO_RUN && fz_next(&cur, &p, &h, &a); guard++) {
        if (geo_run_passes(h.op, a)) continue;
        if (h.op != OP_DRAW && h.op != OP_DRAW_IDX) break;
        p_draw dt; p_draw_idx it;
        const p_draw *dw = h.op == OP_DRAW ? draw_rec(a, h.size, &dt) : NULL;
        const p_draw_idx *di = h.op == OP_DRAW_IDX ? draw_idx_rec(a, h.size, &it) : NULL;
        if ((dw && dw->mdi_cnt) || (di && di->mdi_cnt)) break;
        NvProg *pr = S.prog ? prog_get(S.prog) : nil;
        if (!pr || !pr->sh[2] || !pr->sh[2]->geo_vertices) break;
        if (geo_mesh_fn(pr, dw, di)) break;
        int r = geo_prep(pr, dw, di, &g_geo_run[n], 0, 0);
        if (r <= 0) break;
        int conflict=0;
        for (int i=0;i<n;i++) if (geo_access_conflict(&g_geo_run[i].access,&g_geo_run[n].access)) { conflict=1; break; }
        if (conflict) { g_geo_dependency_splits++; break; }
        g_geo_run[n].op = p;
        geo_capture_one(re, &g_geo_run[n]);
        n++;
    }
    S = s0; R = r0; g_pso_dirty = pd; g_dss_dirty = dd;
    return n;
}
static void geo_draw(NvProg *pr, const p_draw *dw0, const p_draw_idx *di0, uint32_t primitive_base) { cp_stamp start = cp_begin(); geo_draw_(pr, dw0, di0, primitive_base); cp_end(6, start); }
static void geo_draw_(NvProg *pr, const p_draw *dw0, const p_draw_idx *di0, uint32_t primitive_base) {
    for (int i = 1; i < g_geo_nrun; i++) if (g_geo_run[i].op && g_geo_run[i].op == g_exec_cur) {
        g_geo_run[i].op = NULL; geo_raster(&g_geo_run[i], i == g_geo_nrun - 1); return;
    }
    g_geo_nrun = 0;
    if (!g_geo_split_depth && geo_mesh_draw(pr, dw0, di0)) return;
    // Land deferred commands before checking their writes against the CPU read.
    if (defer_has_active()) defer_fallback();
    lc_land(); rp_flush(); rp_end(); ubd_land();
    // This is already an encoder boundary. Start earlier GPU work before the
    // CPU prepares the next run; retain sub_cut's deferral/fence safety gates.
    if (geo_cut_on()) sub_cut();
    geo_job *j0 = &g_geo_run[0];
    if (geo_prep(pr, dw0, di0, j0, 1, primitive_base) <= 0) return;
    static id<MTLComputePipelineState> pack[5];   // count, parallel scan, write, fused small draw, legacy scan
    if (!pack[4]) { NSError *e = nil; id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(k_nvn_geo_pack_msl) options:nil error:&e];
        const char *fn[5] = {"geo_pack_count", "geo_pack_scan", "geo_pack_write", "geo_pack_small", "geo_pack_scan_legacy"};
        for (int i = 0; i < 5 && lib; i++) if (!(pack[i] = [g_dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@(fn[i])] error:&e])) break;
        if (!pack[4]) { for (int i = 0; i < 5; i++) pack[i] = nil; MLOG("[RED] geometry index compaction: %s", e.localizedDescription.UTF8String); return; } }
    if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    if (g_geo_split_compute < 0) { const char *e = getenv("DM_NVN_GEOMETRY_COMPUTE"); g_geo_split_compute = e && !strcmp(e, "split"); }
    MTL4RenderPassDescriptor *rd = [MTL4RenderPassDescriptor new]; rd.renderTargetWidth = rd.renderTargetHeight = rd.defaultRasterSampleCount = 1;
    id<MTL4RenderCommandEncoder> re = enc_r_begin(rd);
    geo_capture_one(re, j0);
    int n = g_geo_nrun = geo_run_walk(re);
    if (n > 1) { g_geo_hoisted += n - 1; g_geo_hoist_runs++; }
    enc_r_end(re);
    typeof(S) s0 = S; typeof(R) r0 = R; int pd = g_pso_dirty, dd = g_dss_dirty;
    fz_cursor cur = {.p = g_exec_next, .ret_n = g_ret_n};
    if (n > 1) memcpy(cur.ret, g_ret, sizeof(*g_ret) * (size_t)g_ret_n);
    id<MTL4ComputeCommandEncoder> ce = nil;
    int packs = 0;
    for (int i = 0; i < n; i++) {
        if (i) {
            const uint8_t *p; op_hdr h; const void *a;
            while (fz_next(&cur, &p, &h, &a) && p != g_geo_run[i].op) state_op(h.op, a);
        }
        if (!ce) ce = enc_c_begin(NULL);
        geo_gs_one(ce, &g_geo_run[i]);
        if (g_geo_split_compute) { enc_c_end(ce); ce = nil; }
        packs += g_geo_run[i].emits != 0 && !g_geo_run[i].direct_count;
    }
    if (n > 1) { S = s0; R = r0; g_pso_dirty = pd; g_dss_dirty = dd; }
    int legacy = geo_pack_legacy(), small = 0;
    for (int i = 0; i < n; i++) if (!legacy && g_geo_run[i].emits && !g_geo_run[i].direct_count && g_geo_run[i].groups <= 1024) small++;
    if (small) {
        if (!ce) ce = enc_c_begin(NULL);
        else [ce barrierAfterEncoderStages:MTLStageDispatch beforeEncoderStages:MTLStageDispatch visibilityOptions:MTL4VisibilityOptionDevice];
        enc_c_pso(ce, pack[3]);
        for (int i = 0; i < n; i++) if (g_geo_run[i].emits && !g_geo_run[i].direct_count && g_geo_run[i].groups <= 1024) {
            enc_c_args(ce, g_geo_run[i].job, 0); enc_c_dispatch(ce, MTLSizeMake(1, 1, 1), MTLSizeMake(128, 1, 1)); g_geo_pack_small++;
        }
        if (g_geo_split_compute) { enc_c_end(ce); ce = nil; }
    }
    if (packs > small) for (int stage = 0; stage < 3; stage++) {
        if (!ce) ce = enc_c_begin(NULL);
        else [ce barrierAfterEncoderStages:MTLStageDispatch beforeEncoderStages:MTLStageDispatch visibilityOptions:MTL4VisibilityOptionDevice];
        enc_c_pso(ce, pack[stage == 1 && legacy ? 4 : stage]);
        for (int i = 0; i < n; i++) if (g_geo_run[i].emits && !g_geo_run[i].direct_count && (legacy || g_geo_run[i].groups > 1024)) {
            enc_c_args(ce, g_geo_run[i].job, 0);
            enc_c_dispatch(ce, stage == 1 ? MTLSizeMake(1, 1, 1) : MTLSizeMake((g_geo_run[i].groups + 63) / 64, 1, 1), MTLSizeMake(stage == 1 ? (legacy ? 1 : 128) : 64, 1, 1));
            if (!stage) g_geo_pack_large++;
        }
        if (g_geo_split_compute) { enc_c_end(ce); ce = nil; }
    }
    for (int i = 0; i < n; i++) if (g_geo_run[i].lean && !g_geo_run[i].direct_count) {
        if (!ce) ce = enc_c_begin(NULL);
        else [ce barrierAfterEncoderStages:MTLStageDispatch beforeEncoderStages:MTLStageDispatch visibilityOptions:MTL4VisibilityOptionDevice];
        enc_c_pso(ce, g_geo_lean_k[1]); enc_c_args(ce, g_geo_run[i].lean_job, 0);
        enc_c_dispatch(ce, MTLSizeMake((g_geo_run[i].packed_size / 4 + 63) / 64, 1, 1), MTLSizeMake(64, 1, 1));
    }
    if (ce) enc_c_end(ce);
    args_dirty_all(); g_enc_pso = nil; g_enc_dss = nil; g_ed.ok = 0;
    geo_raster(j0, n == 1);
}
