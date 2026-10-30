/* TOTK: cube-face-set pass groups ("cube merge", TOTK_CUBE_MERGE, default on; runtime `flag cube_merge 0|1`).
   Included by nvn_mtl.m before draw_inner; the globals it needs (g_cg, g_cg_on, g_cg_force_layers, g_cg_cur) are
   declared near the top of nvn_mtl.m because rp_begin_ex and dm_gfx_set_flag use them.

   The environment-probe filter (fragment f3fc460c) renders the six faces of one cube level through six colour attachments
   with one triangle, in its own pass: 6 cubes x 2 levels = 12 passes per frame, each paying the ~50 us per-pass cost of
   the M4 for 16x16 / 8x8 pixels of work. Here consecutive passes of that shape are taken off the replay (the draw's
   state S, scissor and draw record are snapshotted, like the TBDR deferral) and landed as ONE layered pass per
   (format, level size): attachment f is a six-layer temp 2D array for face f, the draw of cube c writes layer c (the
   per-draw layer constant of the existing layered passes, vs_fn_for), and 36 single-slice copies move the faces from the
   cubes into the temps before the pass (the pass loads the cube contents exactly, whatever the draw does) and back after
   it. The six attachments must be six different textures: the Metal validation layer refuses six slice windows of one
   texture even when the written layers are disjoint (scripts/diag/metal_cube_group_probe.m: layout A).

   Safety: a group pends only across consecutive cube-face-set passes. Any other SetRenderTargets, any op that is not a
   pure state / binding op (copies, dispatches, clears, counters, events, inline uniform updates), the end of the
   submission, a second draw in a taken pass, and any draw that is not taken itself land the pending groups first, in
   record order, so nothing can observe the cubes between the game's pass and the merged one. A member that reads a cube
   another member already wrote is preceded by a flush (it would read the pre-pass copy otherwise). A group of one, or
   a group whose attachments cannot be matched to the temps, is replayed as plain passes in order. */
#define CG_MAX 6          // members (cubes) per group = temp layers
#define CG_GROUPS 4       // groups pending at once (the game interleaves the two levels of each cube: two groups)
#define CG_TEMPS 8        // cached temp sets (one per attachment format and level size)

static int cg_on(void) {
    int v = atomic_load(&g_cg_on);
    if (v < 0) { const char *e = getenv("TOTK_CUBE_MERGE"); v = !(e && !strcmp(e, "0")); atomic_store(&g_cg_on, v); if (!v) MLOG("TOTK_CUBE_MERGE=0：立方体面通道合并关闭"); }
    return v && lay_on();
}
// A SetRenderTargets that is the six faces of one cube level: the same cube in all six slots, slot i viewing layer i,
// one common level, no depth. Gives the level and its size.
static int cg_match(const p_set_rt *r, int *lv, uint32_t *w, uint32_t *h) {
    if (r->n != 6 || r->depth || !r->color[0] || r->color[0]->d.target != 8 || r->color[0]->d.samples > 1 || !r->has_cview[0]) return 0;
    const view_t *v0 = &r->cview[0];
    if (!v0->set_layers || v0->nlayers != 1 || v0->min_layer != 0 || (v0->set_levels && v0->nlevels != 1)) return 0;
    for (int i = 1; i < 6; i++) {
        if (!r->color[i] || r->color[i]->serial != r->color[0]->serial || !r->has_cview[i]) return 0;
        view_t v = r->cview[i]; if (!v.set_layers || v.nlayers != 1 || v.min_layer != (uint16_t)i) return 0;
        v.min_layer = 0; if (memcmp(&v, v0, sizeof v)) return 0;
    }
    int l = v0->set_levels ? v0->base_level : 0, levels = r->color[0]->d.levels ? (int)r->color[0]->d.levels : 1;
    if (l >= levels) return 0;
    *lv = l; *w = (uint32_t)mip_extent(r->color[0]->d.width, (NSUInteger)l); *h = (uint32_t)mip_extent(r->color[0]->d.height, (NSUInteger)l);
    return *w <= LAY_MAX_EXTENT && *h <= LAY_MAX_EXTENT;
}
static int cg_rt_match(const p_set_rt *r) { int lv; uint32_t w, h; return cg_match(r, &lv, &w, &h); }
static uint32_t cg_vfmt(const p_set_rt *r) { return r->cview[0].set_format ? r->cview[0].format : 0; }
// Ops that may run while groups pend: state and bindings only (snapshotted per member), list structure, render-enable.
static int cg_op_safe(unsigned op) {
    switch (op) {
        case OP_END: case OP_JUMP: case OP_BEGIN: case OP_CALL: case OP_VIEWPORTS: case OP_SCISSOR: case OP_DEPTH_RANGE: case OP_BIND_PROGRAM:
        case OP_BIND_STATE: case OP_BIND_VATTR: case OP_BIND_VSTREAM: case OP_BIND_VBUF: case OP_BIND_UBO: case OP_BIND_SSBO: case OP_BIND_TEX:
        case OP_BIND_SAMP: case OP_BIND_IMG: case OP_STENCIL: case OP_POLY_OFFSET: case OP_BLEND_COLOR: case OP_DEPTH_CLAMP: case OP_RASTER_DISCARD:
        case OP_ALPHA_REF: case OP_SET_TEX_POOL: case OP_SET_SAMP_POOL: case OP_RENDER_ENABLE: case OP_RENDER_COND: case OP_DRAW: case OP_DRAW_IDX: case OP_SET_RT: return 1;
        default: return 0;
    }
}
// Lookahead from the op after the taken draw: -1 = this pass draws again (not mergeable as a one-draw pass), 1 = another
// cube-face-set pass of the same key follows within the safe window, 0 = neither.
static int cg_scan(const uint8_t *start, uint32_t nvnfmt, uint32_t vfmt, uint32_t w, uint32_t h) {
    fz_cursor cur = {.p = start}; int in_this = 1, found = 0;
    for (int guard = 0; guard < 2048 && !found; guard++) {
        const uint8_t *q; op_hdr oh; const void *a;
        if (!fz_next(&cur, &q, &oh, &a)) break;
        if (oh.op == OP_SET_RT) { int lv; uint32_t ww, hh; const p_set_rt *r = a;
            if (!cg_match(r, &lv, &ww, &hh)) break;
            in_this = 0;
            if (ww == w && hh == h && r->color[0]->d.format == nvnfmt && cg_vfmt(r) == vfmt) found = 1;
        } else if (oh.op == OP_DRAW || oh.op == OP_DRAW_IDX) { if (in_this) return -1; }
        else if (!cg_op_safe(oh.op)) break;
    }
    return found;
}
// Does the bound program read a cube a pending member already wrote?
static int cg_reads_pending(void) {
    for (int gi = 0; gi < g_cg.ng; gi++) for (int c = 0; c < g_cg.g[gi].n; c++)
        if (prog_samples_ser(S.prog, S.tex, S.img, g_cg.g[gi].m[c].ser)) return 1;
    return 0;
}
static cg_temp *cg_temps(MTLPixelFormat pf, uint32_t w, uint32_t h, const cg_member *m0) {
    for (int i = 0; i < g_cg.ntemps; i++) { cg_temp *t = &g_cg.temps[i]; if (t->pf == pf && t->w == w && t->h == h) return t; }
    if (g_cg.ntemps >= CG_TEMPS) { static int once; if (!once++) MLOG("[cube-merge] 临时贴图组已满（%d 种格式 / 尺寸），更多的组按原样回放", CG_TEMPS); return NULL; }
    cg_temp *t = &g_cg.temps[g_cg.ntemps]; memset(t, 0, sizeof *t); t->pf = pf; t->w = w; t->h = h;
    MTLTextureDescriptor *d = [MTLTextureDescriptor new]; d.textureType = MTLTextureType2DArray; d.pixelFormat = pf; d.width = w; d.height = h; d.arrayLength = CG_MAX;
    d.mipmapLevelCount = 1; d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; d.storageMode = MTLStorageModePrivate;
    for (int f = 0; f < 6; f++) {
        id<MTLTexture> tex = [g_dev newTextureWithDescriptor:d]; if (!tex) { MLOG("【红】[cube-merge] 建不出 %ux%u 格式 %lu 的临时贴图，更多的组按原样回放", w, h, (unsigned long)pf); return NULL; }
        if (!g_cg_hold) g_cg_hold = [NSMutableArray array]; [g_cg_hold addObject:tex];   // the struct holds an unretained pointer: the array keeps the temps alive
        tex.label = [NSString stringWithFormat:@"cube-merge temp %ux%u f%d", w, h, f]; res_add(tex); t->t[f] = tex;
        tex_t *fk = &t->fake[f]; memset(fk, 0, sizeof *fk); fk->serial = 0xffffff00ull + (uint64_t)g_cg.ntemps * 8 + (uint64_t)f;   // reserved serials, below the probes' 0xffffffb0
        fk->d = m0->rt.color[0]->d; fk->d.target = 4; fk->d.depth = CG_MAX; fk->d.levels = 1; fk->d.width = w; fk->d.height = h; fk->d.flags = 0; fk->d.samples = 1;
        serial_put(fk->serial, t->t[f]);
    }
    g_cg.ntemps++;
    MLOG("[cube-merge] 临时贴图组 %d：%ux%u 格式 %lu，6 x %d 层", g_cg.ntemps, w, h, (unsigned long)pf, CG_MAX);
    return t;
}
/* The copies between the cube levels and the temps: one compute dispatch per direction and group (12 textures through an
   argument table) instead of 36 single-slice blits each way -- a blit costs about 1 us of replay CPU on Metal 4, the 144 of
   a frame were 0.15 ms, more than the 12 merged passes cost to begin with. The copy-out writes the cubes, so they need
   ShaderWrite (small cube render targets get it at creation, nvn_mtl.m tex creation); without it, blits. */
static const char *k_cg_copy_msl =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "struct cg_par { uint n; uint dir; uint pad0, pad1; uint lv[6]; uint pad[6]; };\n"
    "kernel void cg_in(array<texturecube<float, access::read>, 6> cubes [[texture(0)]], array<texture2d_array<float, access::write>, 6> temps [[texture(6)]],\n"
    "                  constant cg_par &p [[buffer(0)]], uint3 id [[thread_position_in_grid]]) {\n"
    "    uint c = id.z / 6u, f = id.z % 6u; if (c >= p.n) return; temps[f].write(cubes[c].read(id.xy, f, p.lv[c]), id.xy, c); }\n"
    "kernel void cg_out(array<texturecube<float, access::write>, 6> cubes [[texture(0)]], array<texture2d_array<float, access::read>, 6> temps [[texture(6)]],\n"
    "                   constant cg_par &p [[buffer(0)]], uint3 id [[thread_position_in_grid]]) {\n"
    "    uint c = id.z / 6u, f = id.z % 6u; if (c >= p.n) return; cubes[c].write(temps[f].read(id.xy, c), id.xy, f, p.lv[c]); }\n";
static id<MTLComputePipelineState> cg_pso(int out) {
    static id<MTLComputePipelineState> ps[2]; static int failed;
    if (failed) return nil;
    if (!ps[out]) { NSError *e = nil; id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(k_cg_copy_msl) options:nil error:&e];
        id<MTLFunction> fn = lib ? [lib newFunctionWithName:out ? @"cg_out" : @"cg_in"] : nil; ps[out] = fn ? [g_dev newComputePipelineStateWithFunction:fn error:&e] : nil;
        if (!ps[out]) { failed = 1; MLOG("【红】[cube-merge] 拷贝计算核建不出：%s（改用 blit 拷贝）", e.localizedDescription.UTF8String); return nil; } }
    return ps[out];
}
static id<MTL4ArgumentTable> cg_table(void) {
    static id<MTL4ArgumentTable> t;
    if (!t) { MTL4ArgumentTableDescriptor *d = [MTL4ArgumentTableDescriptor new]; d.maxTextureBindCount = 12; d.maxBufferBindCount = 1; NSError *e = nil;
        t = [g_dev newArgumentTableWithDescriptor:d error:&e]; if (!t) MDIE("cube-merge argument table: %s", e.localizedDescription.UTF8String); }
    return t;
}
// 1 = done by the kernel, 0 = the caller blits (no kernel, or a cube the copy-out could not write)
static int cg_copy_kernel(int out, const cg_group *g, const cg_temp *tp, __strong id<MTLTexture> *ct) {
    id<MTLComputePipelineState> ps = cg_pso(out); if (!ps) return 0;
    if (out) for (int c = 0; c < g->n; c++) if (!(ct[c].usage & MTLTextureUsageShaderWrite)) { g_cg.blit_out++; return 0; }
    id<MTL4ComputeCommandEncoder> b = blit(); id<MTL4ArgumentTable> at = cg_table();
    for (int c = 0; c < 6; c++) [at setTexture:ct[c < g->n ? c : 0].gpuResourceID atIndex:(NSUInteger)c];   // unused slots repeat cube 0 (never touched: c < n)
    for (int f = 0; f < 6; f++) [at setTexture:tp->t[f].gpuResourceID atIndex:(NSUInteger)(6 + f)];
    id<MTLBuffer> pb; size_t off = arena_alloc(64, &pb); uint32_t *p = (uint32_t *)((uint8_t *)pb.contents + off); memset(p, 0, 64);
    p[0] = (uint32_t)g->n; p[1] = (uint32_t)out; for (int c = 0; c < g->n; c++) p[4 + c] = (uint32_t)g->m[c].lv;
    [at setAddress:pb.gpuAddress + off atIndex:0];
    [b setArgumentTable:at]; [b setComputePipelineState:ps];
    [b dispatchThreads:MTLSizeMake(g->w, g->h, 6 * (NSUInteger)g->n) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
    [b setArgumentTable:g_ct]; args_dirty_all();   // the replay's own dispatches re-bind their table and arguments
    g_cg.kcopies++; return 1;
}
static void cg_member_state(const cg_member *m) {   // the member's state, as at its draw
    memcpy(&S, m->S_, sizeof S); R.sc = m->sc; R.has_sc = m->has_sc; args_dirty_all(); g_pso_dirty = g_dss_dirty = 1;
}
static void cg_member_draw(const cg_member *m) { draw(m->idx ? NULL : &m->dw, m->idx ? &m->di : NULL); }
static void cg_flush_plain(cg_group *g) {   // each member in its own pass, in record order
    for (int c = 0; c < g->n; c++) { const cg_member *m = &g->m[c];
        R.rt = m->rt; R.has_rt = 1; memset(R.clear_c, 0, sizeof R.clear_c); R.clear_d = R.clear_s = 0;
        cg_member_state(m); cg_member_draw(m); rp_end(); g_cg.plain_members++; }
}
static void cg_flush_group(cg_group *g) {
    id<MTLTexture> ct[CG_MAX]; cg_temp *tp = NULL; int ok = g->n >= 2;
    for (int c = 0; ok && c < g->n; c++) { const cg_member *m = &g->m[c];
        ct[c] = attachment_texture(m->rt.color[0], &m->rt.cview[0], m->rt.has_cview[0]);   // may recreate a storage-shared cube (a blit): before the copies
        if (!ct[c] || ct[c].textureType != MTLTextureTypeCube || mip_extent(ct[c].width, (NSUInteger)m->lv) != g->w || mip_extent(ct[c].height, (NSUInteger)m->lv) != g->h) ok = 0;
        else if (!c) { tp = cg_temps(ct[0].pixelFormat, g->w, g->h, m); if (!tp) ok = 0; }
        else if (ct[c].pixelFormat != tp->pf) ok = 0;
    }
    if (!ok) { cg_flush_plain(g); return; }
    MTLSize sz = MTLSizeMake(g->w, g->h, 1); MTLOrigin o0 = MTLOriginMake(0, 0, 0);
    uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (g_cg_probe_neg != 18 && !cg_copy_kernel(0, g, tp, ct)) {   // copy in: the pass loads exactly what the cube levels hold now
        id<MTL4ComputeCommandEncoder> b = blit();
        for (int c = 0; c < g->n; c++) for (int f = 0; f < 6; f++) { enc_b_copy_texture(b, ct[c], (NSUInteger)f, (NSUInteger)g->m[c].lv, o0, sz, tp->t[f], (NSUInteger)c, 0, o0); g_cg.copies++; }
    }
    uint64_t t1 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW); g_cg.ns_in += t1 - t0;
    p_set_rt rt; memset(&rt, 0, sizeof rt); rt.n = 6;
    for (int f = 0; f < 6; f++) { rt.color[f] = &tp->fake[f]; rt.has_cview[f] = 1; view_t v = g->m[0].rt.cview[0];   // the member's view (format) on layer 0 level 0 of temp f
        v.set_levels = v.set_layers = 1; v.base_level = 0; v.nlevels = 1; v.min_layer = 0; v.nlayers = 1; rt.cview[f] = v; }
    R.rt = rt; R.has_rt = 1; memset(R.clear_c, 0, sizeof R.clear_c); R.clear_d = R.clear_s = 0;
    g_cg_force_layers = g->n;
    for (int c = 0; c < g->n; c++) { const cg_member *m = &g->m[c];
        cg_member_state(m); g_cg_cur = g_cg_probe_neg == 19 ? 0 : c; if (g_enc && g_lay.active) g_lay.cur = g_lay.base + g_cg_cur;
        cg_member_draw(m); }
    g_cg_force_layers = 0; rp_end();
    uint64_t t2 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW); g_cg.ns_draw += t2 - t1;
    if (!cg_copy_kernel(1, g, tp, ct)) {   // copy out
        id<MTL4ComputeCommandEncoder> b = blit();
        for (int c = 0; c < g->n; c++) for (int f = 0; f < 6; f++) { enc_b_copy_texture(b, tp->t[f], (NSUInteger)c, 0, o0, sz, ct[c], (NSUInteger)f, (NSUInteger)g->m[c].lv, o0); g_cg.copies++; }
    }
    g_cg.ns_out += clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t2;
    for (int c = 0; c < g->n; c++) { const tex_t *t = g->m[c].rt.color[0]; gpu_owned_mark(t->serial); if (t->mem && tex_mirrored(t)) wb_note(t); }   // what rp_begin_ex notes for a rendered-into texture
    g_cg.merged_passes++; g_cg.merged_members += (uint64_t)g->n;
    if (!g_cg.merged_passes_logged++) MLOG("[cube-merge] 首个合并通道：%d 个 %ux%u 立方体级（格式 %lu）并成一个分层通道", g->n, g->w, g->h, (unsigned long)tp->pf);
    PD("G %llu 立方体面通道合并：%d 个成员 %ux%u", g_stat_pass, g->n, g->w, g->h);
}
static void cg_flush_reason(int why) { if (why >= 0 && why < 8) g_cg.reason[why]++; }
static void cg_flush(void) {
    if (!g_cg.ng || g_cg.replaying) return;
    g_cg.replaying = 1;
    const uint8_t *en0 = g_exec_next; g_exec_next = NULL;
    static uint8_t *saveS; if (!saveS) { saveS = malloc(sizeof S); if (!saveS) MDIE("cube-merge state"); }
    memcpy(saveS, &S, sizeof S); typeof(R) saveR = R;
    rp_end();
    uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    for (int i = 0; i < g_cg.ng; i++) cg_flush_group(&g_cg.g[i]);
    g_cg.ns_flush += clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0;
    g_cg.ng = 0; g_cg_pending = 0; g_cg.take_seq = ~0ull;
    memcpy(&S, saveS, sizeof S); R = saveR; args_dirty_all(); g_pso_dirty = g_dss_dirty = 1;
    g_exec_next = en0; g_cg.replaying = 0;
}
// The first draw of a cube-face-set pass: snapshot it into its group. 0 = not taken (the caller draws it, after landing
// the pending groups).
static int cg_take_(NvProg *pr, const p_draw *dw, const p_draw_idx *di);
static int cg_take(NvProg *pr, const p_draw *dw, const p_draw_idx *di) {
    uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW); int r = cg_take_(pr, dw, di); g_cg.ns_take += clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0; return r;
}
static int cg_take_(NvProg *pr, const p_draw *dw, const p_draw_idx *di) {
    int lv; uint32_t w, h;
    if (g_cg.replaying || !cg_match(&R.rt, &lv, &w, &h)) return 0;
    if (R.clear_d || R.clear_s) return 0; for (int i = 0; i < 8; i++) if (R.clear_c[i]) return 0;
    if (!pr || !lay_draw_ok(pr, dw, di)) return 0;
    if (g_cg.take_seq == g_cg.rt_seq) { cg_flush_reason(3); return 0; }   // a second draw of a taken pass: the caller lands the groups first
    if (!g_exec_next) return 0;
    uint32_t nvnfmt = R.rt.color[0]->d.format, vfmt = cg_vfmt(&R.rt);
    int ahead = cg_scan(g_exec_next, nvnfmt, vfmt, w, h);
    if (ahead < 0) return 0;
    if (g_cg.ng && cg_reads_pending()) { cg_flush_reason(2); cg_flush(); }
    cg_group *g = NULL;
    for (int i = 0; i < g_cg.ng; i++) if (g_cg.g[i].nvnfmt == nvnfmt && g_cg.g[i].vfmt == vfmt && g_cg.g[i].w == w && g_cg.g[i].h == h) { g = &g_cg.g[i]; break; }
    if (g && g->n >= CG_MAX) { cg_flush_reason(5); cg_flush(); g = NULL; }
    if (!g) {
        if (!ahead) return 0;   // alone: a group of one gains nothing
        if (g_cg.ng >= CG_GROUPS) { cg_flush_reason(5); cg_flush(); }
        g = &g_cg.g[g_cg.ng++]; uint8_t *keep[CG_MAX]; for (int c = 0; c < CG_MAX; c++) keep[c] = g->m[c].S_;
        memset(g, 0, sizeof *g); for (int c = 0; c < CG_MAX; c++) g->m[c].S_ = keep[c];
        g->nvnfmt = nvnfmt; g->vfmt = vfmt; g->w = w; g->h = h;
    }
    cg_member *m = &g->m[g->n++];
    m->rt = R.rt; m->lv = lv; m->ser = R.rt.color[0]->serial; m->idx = di != NULL; if (dw) m->dw = *dw; if (di) m->di = *di; m->sc = R.sc; m->has_sc = R.has_sc;
    if (!m->S_) { m->S_ = malloc(sizeof S); if (!m->S_) MDIE("cube-merge member state"); }
    memcpy(m->S_, &S, sizeof S);
    g_cg.take_seq = g_cg.rt_seq; g_cg.taken++; g_cg_pending = 1;
    PD("G %llu 立方体面通道暂存：ser=%llu lv%d %ux%u（组 %d 第 %d 个）", g_stat_pass + 1, (unsigned long long)m->ser, lv, w, h, (int)(g - g_cg.g), g->n);
    return 1;
}
static void cg_stats_line(void) {
    if (!g_cg.taken && !g_cg.ntemps) return;
    MLOG("[nvn] 立方体面通道合并 至提交 %llu：暂存 %llu 次绘制，合成 %llu 个分层通道（%llu 次绘制），单独回放 %llu 次，计算核拷贝 %llu 次、blit 拷贝 %llu 次（拷出退回 blit %llu 组）；落地于 目标 %llu / 命令 %llu / 读取 %llu / 第二次绘制 %llu / 提交结束 %llu / 组满 %llu",
         (unsigned long long)g_sub_serial, (unsigned long long)g_cg.taken, (unsigned long long)g_cg.merged_passes, (unsigned long long)g_cg.merged_members, (unsigned long long)g_cg.plain_members,
         (unsigned long long)g_cg.kcopies, (unsigned long long)g_cg.copies, (unsigned long long)g_cg.blit_out, (unsigned long long)g_cg.reason[0], (unsigned long long)g_cg.reason[1], (unsigned long long)g_cg.reason[2], (unsigned long long)g_cg.reason[3],
         (unsigned long long)g_cg.reason[4], (unsigned long long)g_cg.reason[5]);
    MLOG("[nvn] 立方体面通道合并 CPU（每次提交平均，ms）：判定与暂存 %.3f / 落地 %.3f（拷入 %.3f / 成员绘制 %.3f / 拷出 %.3f）",
         (double)g_cg.ns_take / 1800 / 1e6, (double)g_cg.ns_flush / 1800 / 1e6, (double)g_cg.ns_in / 1800 / 1e6, (double)g_cg.ns_draw / 1800 / 1e6, (double)g_cg.ns_out / 1800 / 1e6);
    g_cg.taken = g_cg.merged_passes = g_cg.merged_members = g_cg.plain_members = g_cg.copies = g_cg.kcopies = g_cg.blit_out = 0; memset(g_cg.reason, 0, sizeof g_cg.reason);
    g_cg.ns_take = g_cg.ns_flush = g_cg.ns_in = g_cg.ns_draw = g_cg.ns_out = 0;
}
void mtl_cube_merge_stats(uint64_t v[4]) { v[0] = g_cg.taken; v[1] = g_cg.merged_passes; v[2] = g_cg.merged_members; v[3] = g_cg.plain_members; }
