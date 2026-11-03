// Geometry stages run as compute (NVN2; Metal has no geometry stage). A draw whose program has a geometry stage becomes:
//   1. capture: the program's vertex shader runs once per vertex of the draw's index range through the same vertex fetch as an
//      ordinary draw (its translation's extra entry main0_gs, rasterization off) and writes a 144-word record per vertex
//      (position 0..3, point size 4, clip distances 5..12, layer 13, viewport 14, attribute a component c at 16 + 4a + c);
//   2. the geometry shader, translated with Ryujinx's Translate(asCompute) for the draw's input topology, runs as a kernel per
//      (primitive, instance, invocation), reading the records through the primitive map (trb_data) and writing its output vertices
//      (geometry_vb_output) and a strip index block per invocation (geometry_ib_output, 0xFFFFFFFF = restart); its own storage
//      buffer and memory writes happen there;
//   3. raster: the reader vertex shader (the translator's pass-through of the output vertices) and the program's fragment shader
//      draw those indices as the output topology's strips, with every other piece of state as the draw has it (draw_inner).
// Steps 1 and 2 cannot run inside a render pass. The first geometry draw of a run looks ahead (as seedds/nxnative's
// nvn_geometry.inc geo_run_walk does) for the geometry draws that follow it with only state changes between them and no
// memory dependency on each other (geo_access).
// All their captures go into one render encoder and all their kernels into one compute encoder, ahead of the render pass, each
// with the state the replay will have at its draw (a copy of S taken on the walk); each draw then only rasterizes, inside the
// pass. Bananza's terrain: some 300 geometry draws a frame, which split into three encoders each took the frame to 160 ms.
// The translator side (geometry requests, capture entry, reflection "geometry") is games/totk/tools/totk_xlate Geometry.cs /
// GeometryService.cs and patch 0007; the scheme and its ABI follow seedds/nxnative (dmsh_geometry.py, nvn_geometry.inc; MIT).
// Not yet: indirect geometry draws, transform feedback, splitting a draw whose scratch exceeds the bound (reported in red).
#include "nvn_geometry.h"

typedef struct {
    const void *op;   // the draw's record payload (p_draw / p_draw_idx), as the replay hands it to draw()
    NvShader *vsc, *gs, *reader; id<MTLRenderPipelineState> cap;   // gs: the kernel dispatched (the lean one when lean)
    uint64_t g_in, g_out, g_ib, g_map, g_info, g_cap, ib_n, np, slots;
    int lean; uint64_t g_dst, g_lean, g_pack, g_cnt, g_args; uint32_t icap, list_prim;   // lean draws (icap: index capacity) (nvn_geometry_lean.inc): 1 direct, 2 packed
    uint32_t first, span, instances, first_instance, inv;
    void *state;   // the replay state S at this draw (geo_run walk), for its capture and kernel
} geo_job;
enum { GEO_RUN = 64 };
static geo_job g_geo_run[GEO_RUN]; static int g_geo_nrun; static uint64_t g_geo_run_sub;   // the submission the run belongs to (g_sub_serial)
static struct { __unsafe_unretained NvProg *pr; uint64_t ib, args; size_t ib_len; } g_geo_ras;   // while a geometry draw rasterizes its output through draw_inner (pr is held by its program: geo_ras / geo_lean_ras)
static uint64_t g_geo_draws, g_geo_prims, g_geo_big, g_geo_runs, g_geo_hoisted;
static NSMutableDictionary<NSData *, id<MTLRenderPipelineState>> *g_geo_cap_pso;

/* Scratch for one geometry draw: from the submission's arena when it fits a slice of it, else a buffer of its own kept until the
   GPU is done with it (defer_obj_release) */
static uint64_t geo_scratch(size_t n, uint8_t **cpu) {
    if (n <= (3u << 20)) { __unsafe_unretained id<MTLBuffer> b = nil; uint64_t gpu = 0; arena_alloc_u(n, &b, cpu, &gpu); return gpu; }
    id<MTLBuffer> b = [g_dev newBufferWithLength:n options:MTLResourceStorageModeShared];
    if (!b) return 0;
    res_add(b); res_commit(); defer_obj_release(b); g_geo_big++;
    *cpu = (uint8_t *)b.contents; return b.gpuAddress;
}
#include "nvn_geometry_lean.inc"   // lean draws: geo_lean_pick, geo_lean_setup, geo_lean_phases, geo_raster_lean
// NVN primitive (nvn_geometry.h nvn_geo_topology input) of the geometry output: 1 points, 6 line strip, 7 triangle strip
static int geo_out_prim(int output) { return output == 1 ? 0 : output == 6 ? 3 : output == 7 ? 5 : -1; }

/* The capture pipeline: the draw's vertex descriptor and vertex function variant as pso_desc builds them, with the capture
   entry, no fragment function, no attachments and rasterization off. Cached by the pipeline key and the vertex variant. */
static id<MTLRenderPipelineState> geo_capture_pso(NvShader *vs) {
    pso_key k; uint32_t cf[8] = {0}; pso_key_fill(&k, vs, nil, cf, 0, 0, 1, 0);
    NSMutableData *key = [NSMutableData dataWithBytes:&k length:sizeof k]; const void *p = (__bridge const void *)vs; [key appendBytes:&p length:sizeof p];
    if (!g_geo_cap_pso) g_geo_cap_pso = [NSMutableDictionary dictionary];
    id<MTLRenderPipelineState> ps = g_geo_cap_pso[key]; if (ps) return ps;
    MTLRenderPipelineDescriptor *d = pso_desc(&k, vs, nil, nil, 0);
    d.vertexFunction = vs->geo_fn; d.fragmentFunction = nil; d.rasterizationEnabled = NO;
    for (int i = 0; i < 8; i++) d.colorAttachments[i].pixelFormat = MTLPixelFormatInvalid;
    d.depthAttachmentPixelFormat = MTLPixelFormatInvalid; d.stencilAttachmentPixelFormat = MTLPixelFormatInvalid; d.rasterSampleCount = 1;
    NSError *e = nil; ps = [g_dev newRenderPipelineStateWithDescriptor:d error:&e];
    if (!ps) { MLOG("【红】几何抓取管线失败 %s：%s", vs->key, e.localizedDescription.UTF8String); return nil; }
    g_geo_cap_pso[key] = ps; return ps;
}

/* The variant of one stage for these answers plus the geometry fields, under its own index name (shader key + suffix), since
   the index matches variants by their texture / vertex dependencies only and would hand the plain variant back */
static NSString *geo_variant_name(NvProg *pr, int st, NSDictionary *extra, NSString *suffix) {
    NSMutableDictionary *a = [rt_answers(st) mutableCopy]; [a addEntriesFromDictionary:extra];
    return totk_rt_variant([pr->key[st] stringByAppendingString:suffix], st, pr->ctl[st], pr->code[st], a, nil, 0);
}

/* Everything one geometry draw needs, with the replay state as it is now: its shaders, its scratch (capture records, output
   vertices, index blocks, primitive map, vb_info, the capture's record description). 0: nothing to draw or not drawable here. */
static int geo_prep(NvProg *pr, const p_draw *dw, const p_draw_idx *di, geo_job *j) {
    int prim = dw ? dw->prim : di->prim, topology = nvn_geo_topology(prim), width = nvn_geo_width(topology);
    int count = dw ? dw->count : di->count, instances = dw ? dw->instances : di->instances;
    uint32_t first_instance = (uint32_t)(dw ? dw->base_instance : di->base_instance);
    if (count <= 0 || instances < 0) return 0;
    if (!instances) instances = 1;
    if (!width) { RED_ONCE(160, "geometry draw with NVN primitive %d: no assembly for it (skipped)", prim); return 0; }
    if (di && di->ind) { RED_ONCE(161, "indirect geometry draw: not implemented yet (skipped)"); return 0; }
    if (!pr->sh[0] || !pr->key[0] || !pr->key[2]) { RED_ONCE(162, "geometry draw without a vertex stage (skipped)"); return 0; }
    if (pr->key[3] || pr->key[4]) { RED_ONCE(163, "geometry draw with tessellation: not implemented (skipped)"); return 0; }
    /* The three variants by the bind state they were translated for (stage_sig, as prog_fit keys a stage's variants) and the
       input topology: looking them up through the translation index on every draw (answers as JSON, its digest, the index,
       the library table) took most of the submit thread at Bananza's ~300 geometry draws a frame */
    NvShader *vsc = nil, *gs = nil, *reader = nil, *lean = nil;
    NSNumber *ck = @(stage_sig(0) * 0x9E3779B97F4A7C15ull ^ stage_sig(2) * 0xC2B2AE3D27D4EB4Full ^ (uint64_t)topology);
    NSArray *cached = pr->geo_cache[ck];
    if (cached) { vsc = cached[0]; gs = cached[1]; reader = cached[2]; lean = cached[3] == (id)NSNull.null ? nil : cached[3]; }
    else {
        NSString *vc = geo_variant_name(pr, 0, @{@"geometry_capture": @YES}, @"-geocap");
        NSString *gv = geo_variant_name(pr, 2, @{@"geometry_topology": @(topology)}, [NSString stringWithFormat:@"-geo%d", topology]);
        vsc = vc ? shader_get([vc stringByAppendingString:@"_b"], 0) : nil; gs = gv ? shader_get([gv stringByAppendingString:@"_b"], 2) : nil;
        reader = gv ? shader_get([gv stringByAppendingString:@"_r"], 0) : nil;
        lean = gs && gs->geo_lean ? shader_get(gs->geo_lean, 2) : nil;   // named in the full kernel's layout (GeometryService.cs)
        if (vsc && gs && reader) { if (!pr->geo_cache) pr->geo_cache = [NSMutableDictionary dictionary]; pr->geo_cache[ck] = @[vsc, gs, reader, lean ?: (id)NSNull.null]; }
    }
    if (!vsc || !vsc->geo_fn || !gs || !gs->geo_input_vertices || !reader) { RED_ONCE(164, "geometry draw: capture %p / kernel %p / reader %p missing (skipped)", (__bridge void *)vsc, (__bridge void *)gs, (__bridge void *)reader); return 0; }
    if (gs->geo_input_vertices != width) { RED_ONCE(165, "geometry kernel %s reads %d vertices a primitive, the draw gives %d (skipped)", gs->key, gs->geo_input_vertices, width); return 0; }
    if (!gs->geo_pso) { NSError *e = nil; gs->geo_pso = [g_dev newComputePipelineStateWithFunction:gs->fn error:&e];
        if (!gs->geo_pso) { MLOG("【红】几何计算管线 %s：%s", gs->key, e.localizedDescription.UTF8String); return 0; } }
    id<MTLRenderPipelineState> cap = geo_capture_pso(vsc); if (!cap) return 0;
    lean = geo_lean_pick(pr, gs, lean, topology, instances);
    NvShader *k = lean ?: gs;   // the kernel this draw dispatches
    int width_out = lean ? lean->geo_vertices : 0;
    uint32_t record = lean ? (uint32_t)MAX(lean->geo_words, 1) : GEO_CAPTURE_WORDS;

    // The draw's vertex numbers (indices as the CPU sees them now; the index range sets the capture's vertices) and its primitives.
    // Primitive restart as the rest of the replay draws it: Metal restarts at the all-ones index (totk_nvn_ext.c
    // nvnCommandBufferSetPrimitiveRestart names any other index in red).
    if (di && (di->itype < 0 || di->itype > 2)) { RED_ONCE(166, "geometry draw index type %d", di->itype); return 0; }
    int restart = di != NULL, isz = di ? 1 << di->itype : 0;
    uint32_t restart_index = isz == 1 ? UINT8_MAX : isz == 2 ? UINT16_MAX : UINT32_MAX;
    uint32_t *order = malloc((size_t)count * 4); if (!order) MDIE("geometry index memory");
    if (di) { void *mb; uint64_t mo; if (!mtl_addr(di->ibuf, (uint64_t)count * isz, &mb, &mo)) { free(order); RED_ONCE(167, "geometry draw index buffer outside its pool"); return 0; }
        if (pend_hits(di->ibuf, (uint64_t)count * isz)) RED_ONCE(168, "geometry draw indices written by GPU work of this submission: read as the CPU sees them");
        const uint8_t *src = (const uint8_t *)(uintptr_t)di->ibuf;
        for (int i = 0; i < count; i++) { uint32_t v = 0; memcpy(&v, src + (size_t)i * isz, isz); order[i] = v; } }
    else for (int i = 0; i < count; i++) order[i] = (uint32_t)i;
    uint32_t lo = UINT32_MAX, hi = 0; int restarts = 0;
    for (int i = 0; i < count; i++) { if (restart && order[i] == restart_index) { restarts++; continue; } lo = MIN(lo, order[i]); hi = MAX(hi, order[i]); }
    if (!restarts) restart = 0;   // no restart index in the list: the assembly copies list topologies whole (nvn_geo_assemble's fast path)
    if (lo > hi) { free(order); return 0; }
    size_t words = nvn_geo_assemble(prim, order, count, restart, restart_index, NULL, 0);
    uint64_t np = words == SIZE_MAX ? 0 : words / width, span = (uint64_t)hi - lo + 1;
    int64_t first = di ? (int64_t)lo + di->base_vertex : (int64_t)(dw->first) + lo;
    if (!np || first < 0 || first > UINT32_MAX) { free(order); return 0; }
    uint64_t inv = (uint64_t)k->geo_invocations, slots = np * (uint64_t)instances * inv;
    uint64_t in_n = span * instances * record * 4, out_n = MAX(slots * k->geo_vertices * k->geo_stride * 4, 16ull),
             ib_n = MAX(slots * k->geo_index_stride * 4, 16ull), map_n = words * 4, lean_n = lean ? geo_lean_bytes(np, width_out) : 0,
             total = in_n + out_n + ib_n + map_n + lean_n + 1056 + sizeof(nvn_geo_capture) + 7 * 256;
    if (total > (512ull << 20) || slots > UINT32_MAX) { free(order); RED_ONCE(169, "geometry draw needs %llu MiB of scratch: above the bound, not drawn (open: split it)", (unsigned long long)(total >> 20)); return 0; }
    uint8_t *cpu; uint64_t gpu = geo_scratch((size_t)total, &cpu);
    if (!gpu) { free(order); RED_ONCE(170, "geometry scratch of %llu MiB not allocated", (unsigned long long)(total >> 20)); return 0; }
    size_t used = 0; uint8_t *p_map, *p_info, *p_cap, *p_lean = NULL, *unused; uint64_t g_lean_area = 0;
    #define GEO_ALLOC(N, G, P) do { used = (used + 255) & ~(size_t)255; (G) = gpu + used; (P) = cpu + used; used += (size_t)(N); } while (0)
    GEO_ALLOC(in_n, j->g_in, unused); GEO_ALLOC(out_n, j->g_out, unused); GEO_ALLOC(ib_n, j->g_ib, unused); GEO_ALLOC(map_n, j->g_map, p_map);
    GEO_ALLOC(1056, j->g_info, p_info); GEO_ALLOC(sizeof(nvn_geo_capture), j->g_cap, p_cap);
    GEO_ALLOC(lean_n, g_lean_area, p_lean);
    #undef GEO_ALLOC
    (void)unused;
    if (used > total) MDIE("geometry scratch accounting");
    nvn_geo_assemble(prim, order, count, restart, restart_index, (uint32_t *)p_map, words);
    for (size_t i = 0; i < words; i++) ((uint32_t *)p_map)[i] -= lo;   // record numbers within an instance
    free(order);
    memset(p_info, 0, 1056);   // Ryujinx VertexInfoBuffer: vertex_counts {records an instance, instances}, geometry_counts {primitives, first primitive}
    uint32_t vi[8] = {(uint32_t)span, (uint32_t)instances, 0, 0, (uint32_t)np, 0, 0, 0}; memcpy(p_info, vi, sizeof vi);
    nvn_geo_capture ci = {j->g_in, 0, {(uint32_t)first, (uint32_t)span, first_instance, (uint32_t)instances}, record, {0}};
    for (int w = 0; w < GEO_CAPTURE_WORDS; w++) ci.slot[w] = lean ? lean->geo_slot[w] : (int16_t)w;
    memcpy(p_cap, &ci, sizeof ci);
    j->vsc = vsc; j->gs = k; j->reader = reader; j->cap = cap; j->ib_n = ib_n; j->np = np; j->slots = slots;
    j->first = (uint32_t)first; j->span = (uint32_t)span; j->instances = (uint32_t)instances; j->first_instance = first_instance; j->inv = (uint32_t)inv;
    j->lean = 0; if (lean) geo_lean_setup(j, lean, width_out, g_lean_area, p_lean);
    return 1;
}
static void geo_capture_one(id<MTL4RenderCommandEncoder> re, const geo_job *j) {
    enc_r_pso(re, j->cap); bind_vertex_streams(re, j->vsc);
    args_dirty_all(); encode_args(re, j->vsc, 0, 1); enc_r_vbuf(re, 13, j->g_cap);
    enc_r_draw_instanced(re, MTLPrimitiveTypePoint, j->first, j->span, j->instances, j->first_instance);
}
static void geo_kernel_one(id<MTL4ComputeCommandEncoder> ce, const geo_job *j) {
    g_geo_bind[0] = j->g_in; g_geo_bind[1] = j->g_out; g_geo_bind[2] = j->g_ib; g_geo_bind[3] = j->g_info; g_geo_bind[4] = j->g_map;
    args_dirty_all(); encode_args(ce, j->gs, 2, 0); enc_c_pso(ce, j->gs->geo_pso);
    enc_c_dispatch(ce, MTLSizeMake((NSUInteger)((j->np + 31) / 32), j->instances, j->inv), MTLSizeMake(32, 1, 1));
    memset(g_geo_bind, 0, sizeof g_geo_bind); args_dirty_stage(2);
}
/* What a job's capture and kernel read and write in guest memory, as nxnative's geo_access_of: the vertex buffers, the uniform
   buffers of its vertex and geometry stages, their storage buffers (written when the binding is writable). Memory reached
   another way (raw global memory, an image, a typed buffer, a storage buffer whose address the shader loads) is unknown.
   A run moves every capture and kernel ahead of every raster draw, which is right only when its jobs cannot communicate
   through memory: a job joins the run only when none of its ranges meets an earlier job's writes and none of its writes
   meets an earlier job's reads (read / read pairs cannot communicate). */
enum { GEO_ACC = 64 };
typedef struct { uint64_t lo[GEO_ACC], hi[GEO_ACC]; uint8_t w[GEO_ACC]; int n, unknown; } geo_access;
static void geo_access_add(geo_access *a, uint64_t lo, uint64_t size, int write) {
    if (!lo) return;
    if (!size || lo > UINT64_MAX - size || a->n == GEO_ACC) { a->unknown = 1; return; }   // a size of 0 is an unknown extent here (S.vbuf: the pool decides)
    for (int i = 0; i < a->n; i++) if (a->lo[i] == lo && a->hi[i] == lo + size) { a->w[i] |= (uint8_t)write; return; }
    a->lo[a->n] = lo; a->hi[a->n] = lo + size; a->w[a->n] = (uint8_t)write; a->n++;
}
static void geo_access_stage(geo_access *a, NvShader *sh, int st) {
    if (!sh) return;
    for (int i = 0; i < sh->rf.nb; i++) { const nvn_rbind *b = &sh->rf.b[i];
        if (b->kind == NVN_RB_CBUF || b->kind == NVN_RB_SBUFP) {
            if (b->reg >= 16) { a->unknown = 1; continue; }
            const p_bind_buf *u = b->kind == NVN_RB_SBUFP ? &S.ssbo[st][b->reg] : &S.ubo[st][b->reg];
            geo_access_add(a, u->addr, u->size, b->kind == NVN_RB_SBUFP && b->rw);
        } else if (b->kind == NVN_RB_SBUFU || b->kind == NVN_RB_UBOP || b->kind == NVN_RB_GMEM || b->kind == NVN_RB_GMEML || b->kind == NVN_RB_IMG || b->kind == NVN_RB_TBUF) a->unknown = 1; }
}
static void geo_access_of(const geo_job *j, geo_access *a) {
    memset(a, 0, sizeof *a);
    for (int i = 0; i < 16; i++) if (S.vbuf[i].address) geo_access_add(a, S.vbuf[i].address, S.vbuf[i].size, 0);
    geo_access_stage(a, j->vsc, 0); geo_access_stage(a, j->gs, 2);
}
// The run's accesses so far: the union of its jobs' writes and of their reads (a run has at most GEO_RUN jobs)
typedef struct { uint64_t lo[GEO_RUN * GEO_ACC], hi[GEO_RUN * GEO_ACC]; uint8_t w[GEO_RUN * GEO_ACC]; int n, unknown; } geo_run_access;
static int geo_meets(const geo_run_access *u, uint64_t lo, uint64_t hi, int writes_only) {
    for (int i = 0; i < u->n; i++) if ((!writes_only || u->w[i]) && u->lo[i] < hi && lo < u->hi[i]) return 1;
    return 0;
}
static int geo_run_conflict(const geo_run_access *u, const geo_access *a) {
    if (u->unknown || a->unknown) return 1;
    for (int k = 0; k < a->n; k++) if (geo_meets(u, a->lo[k], a->hi[k], !a->w[k])) return 1;   // a read meets earlier writes; a write meets anything earlier
    return 0;
}
static void geo_run_note(geo_run_access *u, const geo_access *a) {
    u->unknown |= a->unknown;
    for (int k = 0; k < a->n && u->n < GEO_RUN * GEO_ACC; k++) { u->lo[u->n] = a->lo[k]; u->hi[u->n] = a->hi[k]; u->w[u->n] = a->w[k]; u->n++; }
}
static uint64_t g_geo_end_conflict, g_geo_end_draw, g_geo_end_op;   // why runs ended: a memory dependency, an ordinary draw, another command
/* The first geometry draw of a run (j0 prepared from the current state) and those found after it: captures and kernels, between
   render passes, each with its own state. Returns how many jobs the run has (g_geo_run). */
static int geo_run(const geo_job *j0) {
    typeof(S) *s0 = malloc(sizeof S); if (!s0) MDIE("geometry run state"); memcpy(s0, &S, sizeof S);
    typeof(R) r0 = R; int pso_dirty = g_pso_dirty, dss_dirty = g_dss_dirty;
    g_geo_run[0] = *j0; g_geo_run[0].state = NULL; int n = 1;
    static int walk = -1; if (walk < 0) { const char *e = getenv("TOTK_GEO_RUN"); walk = !(e && !strcmp(e, "0")); }   // TOTK_GEO_RUN=0: one draw at a time (comparison)
    /* The walk, as nxnative's geo_run_walk: only state changes may lie between the run's draws (state_op applies them to S),
       and each further geometry draw joins only when the run's earlier jobs cannot communicate with it through memory. An
       ordinary draw ends the run (its raster would come after kernels that run before it here), as does any other command:
       a render condition, a barrier, a pool change, a copy, a dispatch, a report, a clear. */
    if (walk && g_exec_next) {
        static geo_run_access run;   // 200 KiB: not on the stack
        geo_access acc; geo_access_of(j0, &acc); run.n = run.unknown = 0; geo_run_note(&run, &acc);
        fz_cursor cur = {.p = g_exec_next};
        for (int guard = 0; guard < 4096 && n < GEO_RUN; guard++) {
            const uint8_t *q; op_hdr h; const void *a;
            if (!fz_next(&cur, &q, &h, &a)) break;
            if (h.op == OP_DRAW || h.op == OP_DRAW_IDX) {
                const prog_t *p = S.prog; if (!p) break;
                NvProg *pr = p->serial ? (__bridge NvProg *)serial_ptr(p->serial) : nil;
                if (!pr) { g_prog_prefetch = 1; pr = prog_get(p); g_prog_prefetch = 0; }
                if (!pr || !pr->geo) { g_geo_end_draw++; break; }
                if (S.render_disabled) continue;   // not drawn at all (geo_draw): no capture, no kernel
                geo_job j = {0};
                if (!geo_prep(pr, h.op == OP_DRAW ? a : NULL, h.op == OP_DRAW_IDX ? a : NULL, &j)) break;   // drawn (or skipped) alone when the replay reaches it
                geo_access_of(&j, &acc);
                if (geo_run_conflict(&run, &acc)) { g_geo_end_conflict++; break; }   // its scratch slice stays unused until the arena is reclaimed
                geo_run_note(&run, &acc);
                j.op = a; j.state = malloc(sizeof S); if (!j.state) MDIE("geometry run state"); memcpy(j.state, &S, sizeof S);
                g_geo_run[n++] = j;
                continue;
            }
            if (h.op == OP_RENDER_COND || !state_op(h.op, a)) { g_geo_end_op++; break; }
        }
    }
    MTL4RenderPassDescriptor *rd = [MTL4RenderPassDescriptor new]; rd.renderTargetWidth = rd.renderTargetHeight = 1; rd.defaultRasterSampleCount = 1;
    id<MTL4RenderCommandEncoder> re = enc_r_begin(rd);
    for (int i = 0; i < n; i++) { memcpy(&S, g_geo_run[i].state ? g_geo_run[i].state : s0, sizeof S); geo_capture_one(re, &g_geo_run[i]); }
    enc_r_end(re);
    id<MTL4ComputeCommandEncoder> ce = enc_c_begin(NULL);
    /* The kernels without barriers between them: the run's jobs share no written memory (geo_run_conflict), so their order
       cannot show. (The barrier after every dispatch once taken for this was not the cause of Bananza's garbage triangles: that
       was the reader's argument block, geo_raster.) The lean draws' host kernels follow in phases (geo_lean_phases). */
    for (int i = 0; i < n; i++) { memcpy(&S, g_geo_run[i].state ? g_geo_run[i].state : s0, sizeof S); geo_kernel_one(ce, &g_geo_run[i]); }
    geo_lean_phases(ce, g_geo_run, n);
    enc_c_end(ce);
    for (int i = 1; i < n; i++) { free(g_geo_run[i].state); g_geo_run[i].state = NULL; }
    memcpy(&S, s0, sizeof S); free(s0); R = r0;
    args_dirty_all(); g_enc_pso = nil; g_pso_dirty = pso_dirty | 1; g_dss_dirty = dss_dirty | 1;
    g_geo_runs++; g_geo_hoisted += (uint64_t)(n - 1);
    return n;
}
// Rasterize a prepared geometry draw's output with the reader and the program's fragment shader, inside the current pass
static void geo_raster(NvProg *pr, const geo_job *j) {
    if (j->lean) { geo_raster_lean(pr, j); return; }
    NvShader *fs = pr->sh[1], *gs = j->gs;
    int oprim = geo_out_prim(gs->geo_output);
    if (!gs->geo_vertices || !gs->geo_index_stride || oprim < 0 || !fs) return;   // a kernel that only writes memory
    if (gs->geo_output == 1) RED_ONCE(171, "geometry kernel %s emits points: restart indices are drawn as points (open)", gs->key);
    if (!pr->geo_ras || pr->geo_ras_reader != (__bridge void *)j->reader || pr->geo_ras->sh[1] != fs) {
        NvProg *r = [NvProg new]; r->sh[0] = j->reader; r->sh[1] = fs; r->ctl[1] = pr->ctl[1]; r->code[1] = pr->code[1]; r->key[1] = pr->key[1];
        r->sig[1] = pr->sig[1]; r->alt[1] = pr->alt[1]; r->keeps = pr->keeps; pr->geo_ras = r; pr->geo_ras_reader = (__bridge void *)j->reader; }
    p_draw_idx od = {0}; od.prim = oprim; od.count = (int32_t)(j->slots * gs->geo_index_stride); od.itype = 2; od.instances = 1;
    g_geo_ras.pr = pr->geo_ras; g_geo_ras.ib = j->g_ib; g_geo_ras.ib_len = (size_t)j->ib_n;
    /* The reader's vb_input: this draw's output vertices. g_geo_bind is not part of S, so the argument blocks' reuse within a
       pass (encode_args: same pass, stage not dirty / same epoch) cannot see it change: the stage is marked dirty here. Without
       that, the run's later draws in one pass reused the first one's block and drew their indices over its vertices (full-screen
       garbage triangles in Bananza); one draw at a time hid it, as each geometry draw opened a pass of its own. */
    g_geo_bind[1] = j->g_out; args_dirty_stage(0);
    draw_inner(NULL, &od);
    memset(&g_geo_ras, 0, sizeof g_geo_ras); memset(g_geo_bind, 0, sizeof g_geo_bind); args_dirty_stage(0); g_pso_dirty = 1;
}
static void geo_draw(NvProg *pr, const p_draw *dw, const p_draw_idx *di) {
    const void *op = dw ? (const void *)dw : (const void *)di;
    if (S.render_disabled) { g_draw_skip[3]++; return; }   // SetRenderEnable(false) / a failed render condition: the console skips the whole draw, geometry stage included
    /* Only within the submission the run was made in: the game records into the same command memory every frame, so a job
       whose draw was not reached (render disabled, a conditional render) would match a draw of a later submission */
    if (g_geo_run_sub != g_sub_serial) g_geo_nrun = 0;
    for (int i = 1; i < g_geo_nrun; i++) if (g_geo_run[i].op == op) {   // its capture and kernel ran with the run's first draw
        geo_job j = g_geo_run[i]; g_geo_run[i].op = NULL;
        g_geo_draws++; g_geo_prims += j.np * j.instances;
        geo_raster(pr, &j); return;
    }
    g_geo_nrun = 0;
    geo_job j0 = {0};
    if (!geo_prep(pr, dw, di, &j0)) return;
    rp_flush(); rp_end();
    g_geo_nrun = geo_run(&j0); g_geo_run_sub = g_sub_serial;
    g_geo_draws++; g_geo_prims += j0.np * j0.instances;
    if (g_geo_runs == 1 || g_geo_runs % 2000 == 0) MLOG("[geometry] %llu draws run as compute in %llu runs (%llu ahead of their draw; runs ended by a memory dependency %llu, an ordinary draw %llu, another command %llu; %llu primitives, %llu with scratch of their own); first: vertex %.16s kernel %.16s, %llu primitives, %d output vertices",
        (unsigned long long)g_geo_draws, (unsigned long long)g_geo_runs, (unsigned long long)g_geo_hoisted, (unsigned long long)g_geo_end_conflict, (unsigned long long)g_geo_end_draw, (unsigned long long)g_geo_end_op, (unsigned long long)g_geo_prims, (unsigned long long)g_geo_big, pr->sh[0]->key, j0.gs->key, (unsigned long long)j0.np, j0.gs->geo_vertices);
    geo_raster(pr, &g_geo_run[0]);
}
