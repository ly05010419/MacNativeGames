// Tessellation as compute (The Witcher 3's terrain, 2026-11-23). Metal 4's render encoders have no tessellator (no
// drawPatches), so a draw whose program has tessellation control and evaluation stages (NVN 3 / 4) becomes:
//   1. capture: the program's vertex shader runs once per vertex of the draw through the same vertex fetch as an ordinary draw
//      (its translation's extra entry main0_gs, rasterization off; games/totk/tools/totk_xlate GeometryService.cs) and writes a
//      144-word record per vertex (position 0..3, attribute a component c at 16 + 4a + c), as games/totk's geometry stages do;
//   2. the control stage as a kernel over every patch of the draw (TessService.cs "tcs": SPIRV-Cross's multi-patch control
//      kernel reading its inputs from the records through the patch vertex map): control points, patch data, levels;
//   3. the tessellator (tess_kernel below): integer partitioning of each patch from its levels into triangles of domain points
//      (patch, u = a/b, v = c/d), with the vertex count of an indirect draw summed on the GPU;
//   4. raster: the evaluation stage as an ordinary vertex function over those points (TessService.cs "tes") and the program's
//      fragment shader, drawn indirectly with every other piece of state as the draw has it (draw_inner).
// Steps 1-3 cannot run inside a render pass: the pass is ended before them. Not yet: triangle and isoline domains, fractional
// spacing's smooth level blending (levels are rounded up to the spacing's integers: same shape, edges shared exactly by
// neighbouring patches), indirect draws.

enum { TESS_REC_WORDS = 144, TESS_CAP_SLOT = 13 };
/* main0_gs's dm_geometry_capture { device uint* dst; uint4 info; uint stride; short slot[144]; }: Metal aligns the uint4 to
   16 bytes (info at 16, stride at 32, slot at 36); with info at 8 the capture read a wrong vertex range and wrote one record */
typedef struct { uint64_t dst, pad; uint32_t info[4]; uint32_t stride; int16_t slot[TESS_REC_WORDS]; } tess_capture;
_Static_assert(__builtin_offsetof(tess_capture, info) == 16 && __builtin_offsetof(tess_capture, stride) == 32 && __builtin_offsetof(tess_capture, slot) == 36, "dm_geometry_capture layout");
typedef struct { uint32_t patches, quad, spacing, cw, capacity, pad[3]; } tess_params;   // tess_kernel's constant block
static struct { int active; __unsafe_unretained NvProg *pr; uint64_t dom, cp, patch, args; } g_tess_ras;   // while a tessellated draw rasterizes through draw_inner
static NSMutableDictionary<NSData *, id<MTLRenderPipelineState>> *g_tess_cap_pso;
static uint64_t g_tess_draws, g_tess_patches, g_tess_skipped;

static const char *const g_tess_kernel_src =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct tess_params { uint patches, quad, spacing, cw, capacity, pad0, pad1, pad2; };\n"
"struct tess_count { atomic_uint vertices; atomic_uint overflow; uint pad[2]; };\n"
"struct tess_args { uint vertex_count, instance_count, vertex_start, base_instance; };\n"
"struct quad_levels { half edge[4]; half inside[2]; };\n"
"// GL / Maxwell spacing: 0 equal, 1 fractional even, 2 fractional odd (Ryujinx TessSpacing); 0 = the patch is culled\n"
"static uint level_of(float f, uint spacing, bool outer) {\n"
"    if (outer && !(f > 0.0f)) return 0;\n"
"    if (!(f > 0.0f)) f = 1.0f;\n"
"    if (spacing == 1) { uint n = uint(ceil(clamp(f, 2.0f, 64.0f))); return min(n + (n & 1u), 64u); }\n"
"    if (spacing == 2) { uint n = uint(ceil(clamp(f, 1.0f, 63.0f))); return n | 1u; }\n"
"    return uint(ceil(clamp(f, 1.0f, 64.0f)));\n"
"}\n"
"// a domain point: u = a / b, v = c / d (b, d <= 64)\n"
"struct pt { uint a, b, c, d; };\n"
"static float2 uv_of(pt p) { return float2(float(p.a) / float(p.b), float(p.c) / float(p.d)); }\n"
"static uint pack(pt p) { return p.a | p.b << 8 | p.c << 16 | p.d << 24; }\n"
"struct emitter { device uint2* dom; uint at, end, patch; bool cw; };\n"
"static void tri(thread emitter& e, pt p0, pt p1, pt p2) {\n"
"    float2 a = uv_of(p0), b = uv_of(p1), c = uv_of(p2);\n"
"    float area = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);   // > 0: counter-clockwise with v up\n"
"    if ((area < 0.0f) != e.cw) { pt t = p1; p1 = p2; p2 = t; }\n"
"    if (e.at + 3 > e.end) return;\n"
"    e.dom[e.at++] = uint2(e.patch, pack(p0)); e.dom[e.at++] = uint2(e.patch, pack(p1)); e.dom[e.at++] = uint2(e.patch, pack(p2));\n"
"}\n"
"// the points of the domain's edge e (0: u = 0, 1: v = 0, 2: u = 1, 3: v = 1, as gl_TessLevelOuter) from its first corner on,\n"
"// walking the square counter-clockwise (v up): bottom (0,0)->(1,0), right (1,0)->(1,1), top (1,1)->(0,1), left (0,1)->(0,0)\n"
"static pt outer_pt(uint e, uint k, uint n) {\n"
"    switch (e) { case 1: return pt{k, n, 0, 1}; case 2: return pt{1, 1, k, n}; case 3: return pt{n - k, n, 1, 1}; default: return pt{0, 1, n - k, n}; }\n"
"}\n"
"// the inner ring's side facing edge e: point j of m (m = n - 1 of that direction's inner level n)\n"
"static pt inner_pt(uint e, uint j, uint nu, uint nv) {\n"
"    switch (e) { case 1: return pt{j + 1, nu, 1, nv}; case 2: return pt{nu - 1, nu, j + 1, nv}; case 3: return pt{nu - 1 - j, nu, nv - 1, nv}; default: return pt{1, nu, nv - 1 - j, nv}; }\n"
"}\n"
"static uint quad_triangles(uint o[4], uint nu, uint nv) {\n"
"    if (nu == 1 && nv == 1) return 2;\n"
"    uint t = 2 * (nu - 2) * (nv - 2);\n"
"    for (uint e = 0; e < 4; e++) t += o[e] + ((e & 1) ? nu : nv) - 2;\n"
"    return t;\n"
"}\n"
"kernel void tess_quads(constant tess_params& p [[buffer(0)]], const device quad_levels* lv [[buffer(1)]], device uint2* dom [[buffer(2)]],\n"
"                       device tess_count* cnt [[buffer(3)]], uint id [[thread_position_in_grid]]) {\n"
"    if (id >= p.patches) return;\n"
"    uint o[4]; bool culled = false;\n"
"    for (uint e = 0; e < 4; e++) { o[e] = level_of(float(lv[id].edge[e]), p.spacing, true); culled = culled || o[e] == 0; }\n"
"    if (culled) return;\n"
"    uint nu = level_of(float(lv[id].inside[0]), p.spacing, false), nv = level_of(float(lv[id].inside[1]), p.spacing, false);\n"
"    bool any = o[0] > 1 || o[1] > 1 || o[2] > 1 || o[3] > 1 || nu > 1 || nv > 1;\n"
"    uint least = p.spacing == 2 ? 3u : 2u;   // an inner level of 1 with anything above 1 counts as 1 + epsilon\n"
"    if (any) { nu = max(nu, least); nv = max(nv, least); }\n"
"    uint n = quad_triangles(o, nu, nv) * 3;\n"
"    uint at = atomic_fetch_add_explicit(&cnt->vertices, n, memory_order_relaxed);\n"
"    if (at + n > p.capacity) { atomic_fetch_add_explicit(&cnt->overflow, 1, memory_order_relaxed);\n"
"        for (uint i = at; i < min(at + n, p.capacity); i++) dom[i] = uint2(id, 0x01000100u);   // degenerate: u = v = 0\n"
"        return; }\n"
"    emitter em = {dom, at, at + n, id, p.cw != 0};\n"
"    if (!any) { pt c0 = {0, 1, 0, 1}, c1 = {1, 1, 0, 1}, c2 = {1, 1, 1, 1}, c3 = {0, 1, 1, 1}; tri(em, c0, c1, c2); tri(em, c0, c2, c3); return; }\n"
"    for (uint i = 1; i + 1 < nu; i++) for (uint j = 1; j + 1 < nv; j++) {\n"
"        pt a = {i, nu, j, nv}, b = {i + 1, nu, j, nv}, c = {i + 1, nu, j + 1, nv}, d = {i, nu, j + 1, nv};\n"
"        tri(em, a, b, c); tri(em, a, c, d);\n"
"    }\n"
"    // each outer edge zipped to the inner ring's side facing it (corner to corner, so neighbouring trapezoids meet on the diagonals)\n"
"    for (uint e = 0; e < 4; e++) {\n"
"        uint n_out = o[e], ni = (e & 1) ? nu : nv, m = ni - 1;   // inner side points: m; parameter (j + 1) / ni along the edge\n"
"        uint i = 0, j = 0;\n"
"        while (i < n_out || j + 1 < m) {\n"
"            bool step_out = j + 1 >= m || (i < n_out && float(i + 1) / float(n_out) <= float(j + 2) / float(ni));\n"
"            if (step_out) { tri(em, outer_pt(e, i, n_out), inner_pt(e, j, nu, nv), outer_pt(e, i + 1, n_out)); i++; }\n"
"            else { tri(em, outer_pt(e, i, n_out), inner_pt(e, j, nu, nv), inner_pt(e, j + 1, nu, nv)); j++; }\n"
"        }\n"
"    }\n"
"}\n"
"kernel void tess_finish(constant tess_params& p [[buffer(0)]], device tess_count* cnt [[buffer(3)]], device tess_args* args [[buffer(4)]],\n"
"                        device uint* stat [[buffer(5)]]) {\n"
"    uint total = atomic_load_explicit(&cnt->vertices, memory_order_relaxed), n = min(total, p.capacity);\n"
"    args->vertex_count = n - n % 3; args->instance_count = 1; args->vertex_start = 0; args->base_instance = 0;\n"
"    stat[0] = total; stat[1] = atomic_load_explicit(&cnt->overflow, memory_order_relaxed);   // the program's last count (the next capacity)\n"
"}\n";

static id<MTLComputePipelineState> tess_kernel(int finish) {
    static id<MTLComputePipelineState> quads, fin; static int tried;
    if (!tried) { tried = 1; NSError *e = nil;
        id<MTLLibrary> lib = totk_msl_library(@(g_tess_kernel_src), &e);
        if (!lib) { MLOG("【红】曲面细分器编译失败：%s", e.localizedDescription.UTF8String); return nil; }
        quads = [g_dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"tess_quads"] error:&e];
        fin = [g_dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"tess_finish"] error:&e];
        if (!quads || !fin) MLOG("【红】曲面细分器管线失败：%s", e.localizedDescription.UTF8String); }
    return finish ? fin : quads;
}

/* Scratch for one tessellated draw: from the submission's arena when it fits a slice of it, else a buffer of its own kept
   until the GPU is done with it (defer_obj_release), as games/totk geo_scratch */
static uint64_t tess_scratch_b(size_t n, uint8_t **cpu, id<MTLBuffer> *buf) {
    n = MAX(n, (size_t)256);
    if (n <= (1u << 20)) { id<MTLBuffer> b; size_t off = arena_alloc(n, &b); if (cpu) *cpu = (uint8_t *)b.contents + off; if (buf) *buf = b; return g_cur_arena->gpu + off; }
    id<MTLBuffer> b = [g_dev newBufferWithLength:n options:MTLResourceStorageModeShared];
    if (!b) return 0;
    res_add(b); res_commit(); defer_obj_release(b);
    if (cpu) *cpu = (uint8_t *)b.contents;
    if (buf) *buf = b;
    return b.gpuAddress;
}
static uint64_t tess_scratch(size_t n, uint8_t **cpu) { return tess_scratch_b(n, cpu, NULL); }

/* The capture pipeline: the draw's vertex descriptor and vertex function variant as pso_desc builds them, with the capture
   entry, no fragment function, no attachments and rasterization off (games/totk geo_capture_pso). */
static id<MTLRenderPipelineState> tess_capture_pso(NvShader *vs) {
    pso_key k; uint32_t cf[8] = {0}; pso_key_fill(&k, vs, nil, cf, 0, 0, 1, 0);
    NSMutableData *key = [NSMutableData dataWithBytes:&k length:sizeof k]; const void *p = (__bridge const void *)vs; [key appendBytes:&p length:sizeof p];
    if (!g_tess_cap_pso) g_tess_cap_pso = [NSMutableDictionary dictionary];
    id<MTLRenderPipelineState> ps = g_tess_cap_pso[key]; if (ps) return ps;
    MTLRenderPipelineDescriptor *d = pso_desc(&k, vs, nil, nil, 0);
    d.vertexFunction = vs->geo_fn; d.fragmentFunction = nil; d.rasterizationEnabled = NO;
    for (int i = 0; i < 8; i++) d.colorAttachments[i].pixelFormat = MTLPixelFormatInvalid;
    d.depthAttachmentPixelFormat = MTLPixelFormatInvalid; d.stencilAttachmentPixelFormat = MTLPixelFormatInvalid; d.rasterSampleCount = 1;
    NSError *e = nil; ps = [g_dev newRenderPipelineStateWithDescriptor:d error:&e];
    if (!ps) { MLOG("【红】曲面细分抓取管线失败 %s：%s", vs->key, e.localizedDescription.UTF8String); return nil; }
    g_tess_cap_pso[key] = ps; return ps;
}

/* The variants this draw needs, for the bind state as it is now: the vertex shader's capture variant, the control kernel and
   the evaluation vertex function. nil while one is still translating (the draw is skipped this time). */
static int tess_variants(NvProg *pr, NvShader **vsc, NvShader **tcs, NvShader **tes) {
    NSNumber *ck = @(stage_sig(0) * 0x9E3779B97F4A7C15ull ^ stage_sig(3) * 0xC2B2AE3D27D4EB4Full ^ stage_sig(4) * 0x165667B19E3779F9ull);
    NSArray *cached = pr->tess_cache[ck];
    if (cached) { *vsc = cached[0]; *tcs = cached[1]; *tes = cached[2]; return 1; }
    NSMutableDictionary *a0 = [rt_answers(0) mutableCopy]; a0[@"geometry_capture"] = @YES;
    NSString *vc = totk_rt_variant([pr->key[0] stringByAppendingString:@"-tesscap"], 0, pr->ctl[0], pr->code[0], a0, nil, 1);
    NSString *cv = totk_rt_variant(pr->key[3], 3, pr->ctl[3], pr->code[3], rt_answers(3), nil, 1);
    NvShader *v = vc ? shader_get([vc stringByAppendingString:@"_b"], 0) : nil, *c = cv ? shader_get([cv stringByAppendingString:@"_b"], 3) : nil;
    if (!v || !c) return 0;
    if (!v->geo_fn || !c->tess_out_vertices) { RED_ONCE(190, "曲面细分：抓取入口 %p / 控制核布局 %d 缺失（跳过）", (__bridge void *)v->geo_fn, c->tess_out_vertices); return 0; }
    // the evaluation stage's mode from its control section (GL enums: +0x738 primitive, +0x73c spacing, +0x744 vertex order)
    uint32_t prim = 0, spacing = 0, order = 0; memcpy(&prim, pr->ctl[4] + 0x738, 4); memcpy(&spacing, pr->ctl[4] + 0x73c, 4); memcpy(&order, pr->ctl[4] + 0x744, 4);
    int patch = prim == 7 ? 2 : prim == 4 ? 1 : prim == 0x8E7A ? 0 : -1, sp = spacing == 0x8E7C ? 1 : spacing == 0x8E7B ? 2 : spacing == 0x202 || !spacing ? 0 : -1;
    if (patch < 0 || sp < 0) { RED_ONCE(191, "曲面细分：求值阶段模式 %#x / %#x 不认识（跳过）", prim, spacing); return 0; }
    NSMutableDictionary *a4 = [rt_answers(4) mutableCopy];
    a4[@"tess_patch"] = @(patch); a4[@"tess_spacing"] = @(sp); a4[@"tess_cw"] = @(order == 0x900); a4[@"tess_in_vertices"] = @(c->tess_out_vertices);
    NSString *ev = totk_rt_variant(pr->key[4], 4, pr->ctl[4], pr->code[4], a4, nil, 1);
    NvShader *t = ev ? shader_get([ev stringByAppendingString:@"_b"], 4) : nil;
    if (!t) return 0;
    if (t->tes_in_stride != c->tess_out_stride || t->tes_patch_stride != c->tess_patch_stride) {
        RED_ONCE(192, "曲面细分：控制核输出（%d / %d 字节）与求值阶段输入（%d / %d 字节）对不上（跳过）", c->tess_out_stride, c->tess_patch_stride, t->tes_in_stride, t->tes_patch_stride); return 0; }
    if (!t->tes_quad) { RED_ONCE(193, "曲面细分：三角形 / 等值线域还没实现（跳过）"); return 0; }
    if (!c->geo_pso) { NSError *e = nil; c->geo_pso = [g_dev newComputePipelineStateWithFunction:c->fn error:&e];
        if (!c->geo_pso) { MLOG("【红】曲面细分控制核管线 %s：%s", c->key, e.localizedDescription.UTF8String); return 0; } }
    if (!pr->tess_cache) pr->tess_cache = [NSMutableDictionary dictionary];
    pr->tess_cache[ck] = @[v, c, t];
    t->tess_cw = order == 0x900; t->tess_spacing = sp;
    *vsc = v; *tcs = c; *tes = t; return 1;
}

static void tess_draw(NvProg *pr, const p_draw *dw, const p_draw_idx *di) {
    if (S.render_disabled || S.raster_discard) { g_draw_skip[S.render_disabled ? 3 : 4]++; return; }
    int ps = S.patch_size, count = dw ? dw->count : di->count, instances = dw ? dw->instances : di->instances;
    uint32_t first_instance = (uint32_t)(dw ? dw->base_instance : di->base_instance);
    if (!instances) instances = 1;
    if (ps < 1 || ps > 32 || count < ps || instances < 0) { RED_ONCE(194, "曲面细分绘制：面片 %d 个点、%d 个顶点（跳过）", ps, count); return; }
    NvShader *vsc = nil, *tcs = nil, *tes = nil;
    if (!tess_variants(pr, &vsc, &tcs, &tes)) { g_tess_skipped++; async_skip_targets(); g_draw_skip[15]++; return; }
    if (!tess_kernel(0) || !tess_kernel(1)) return;
    id<MTLRenderPipelineState> cap = tess_capture_pso(vsc); if (!cap) return;
    // the draw's vertex numbers (indices as the CPU sees them now) and the record of each patch vertex (games/totk geo_prep)
    if (di && (di->itype < 0 || di->itype > 2)) { RED_ONCE(195, "曲面细分绘制：索引类型 %d", di->itype); return; }
    int isz = di ? 1 << di->itype : 0, ppi = count / ps;
    uint32_t *order = malloc((size_t)count * 4); if (!order) MDIE("tessellation index memory");
    if (di) { void *mb; uint64_t mo; if (!mtl_addr(di->ibuf, (uint64_t)count * isz, &mb, &mo)) { free(order); RED_ONCE(196, "曲面细分绘制：索引缓冲不在内存池里"); return; }
        if (pend_hits(di->ibuf, (uint64_t)count * isz)) RED_ONCE(197, "曲面细分绘制：索引由本次提交的 GPU 工作写入，按 CPU 所见读");
        const uint8_t *src = (const uint8_t *)(uintptr_t)di->ibuf;
        for (int i = 0; i < count; i++) { uint32_t v = 0; memcpy(&v, src + (size_t)i * isz, isz); order[i] = v; } }
    else for (int i = 0; i < count; i++) order[i] = (uint32_t)i;
    uint32_t lo = UINT32_MAX, hi = 0;
    for (int i = 0; i < ppi * ps; i++) { lo = MIN(lo, order[i]); hi = MAX(hi, order[i]); }
    uint64_t span = (uint64_t)hi - lo + 1, patches = (uint64_t)ppi * instances;
    int64_t first = di ? (int64_t)lo + di->base_vertex : (int64_t)dw->first + lo;
    if (!patches || first < 0 || first > UINT32_MAX || span * instances * TESS_REC_WORDS * 4 > (256ull << 20)) { free(order); RED_ONCE(198, "曲面细分绘制：%llu 个面片 / %llu 条记录超出范围（跳过）", (unsigned long long)patches, (unsigned long long)(span * instances)); return; }
    int outv = tcs->tess_out_vertices;
    // domain points: from the last frame's count for this program (tess_stat), at least 64 triangles a patch, at most 6 M points
    uint64_t want = patches * 3 * 64; if (pr->tess_stat) { uint32_t last = ((const uint32_t *)pr->tess_stat.contents)[0]; want = MAX(want, (uint64_t)last + last / 4); }
    uint32_t capacity = (uint32_t)MIN(want, 6ull << 20);
    uint8_t *p_rec, *p_map, *p_par, *p_cap, *p_lv, *p_patch, *p_out, *p_dom, *p_args;
    id<MTLBuffer> b_rec, b_dom;   // held by the probe's completion block (W3_TESS_PROBE)
    uint64_t g_rec = tess_scratch_b((size_t)(span * instances * TESS_REC_WORDS * 4), &p_rec, &b_rec),
             g_map = tess_scratch((size_t)patches * ps * 4, &p_map),
             g_lv = tess_scratch((size_t)patches * 12, &p_lv),
             g_patch = tess_scratch((size_t)patches * MAX(tcs->tess_patch_stride, 16), &p_patch),
             g_out = tess_scratch((size_t)patches * outv * MAX(tcs->tess_out_stride, 16), &p_out),
             g_ind = tess_scratch(16, &p_par),
             g_capinfo = tess_scratch(sizeof(tess_capture), &p_cap),
             g_dom = tess_scratch_b((size_t)capacity * 8, &p_dom, &b_dom),
             g_args = tess_scratch(16, &p_args);
    if (!g_rec || !g_map || !g_lv || !g_patch || !g_out || !g_dom) { free(order); RED_ONCE(199, "曲面细分：暂存分配失败"); return; }
    for (uint64_t inst = 0; inst < (uint64_t)instances; inst++)
        for (int j = 0; j < ppi * ps; j++) ((uint32_t *)p_map)[inst * ppi * ps + j] = (uint32_t)(inst * span + order[j] - lo);
    free(order);
    uint32_t ind[4] = {(uint32_t)ps, (uint32_t)patches, 0, 0}; memcpy(p_par, ind, sizeof ind);
    tess_capture ci = {g_rec, 0, {(uint32_t)first, (uint32_t)span, first_instance, (uint32_t)instances}, TESS_REC_WORDS, {0}};
    for (int w = 0; w < TESS_REC_WORDS; w++) ci.slot[w] = (int16_t)w;
    memcpy(p_cap, &ci, sizeof ci);
    if (!pr->tess_stat) { pr->tess_stat = [g_dev newBufferWithLength:16 options:MTLResourceStorageModeShared]; res_add(pr->tess_stat); res_commit(); }
    uint8_t *p_cnt; uint64_t g_cnt = tess_scratch(16, &p_cnt); memset(p_cnt, 0, 16);   // this draw's own counter (the arena is CPU-written before the submission)
    tess_params tp = {(uint32_t)patches, 1, (uint32_t)tes->tess_spacing, (uint32_t)tes->tess_cw, capacity, {0}};
    uint8_t *p_tp; uint64_t g_tp = tess_scratch(sizeof tp, &p_tp); memcpy(p_tp, &tp, sizeof tp);

    rp_flush(); rp_end();
    // 1. capture
    MTL4RenderPassDescriptor *rd = [MTL4RenderPassDescriptor new]; rd.renderTargetWidth = rd.renderTargetHeight = 1; rd.defaultRasterSampleCount = 1;
    id<MTL4RenderCommandEncoder> re = enc_r_begin(rd);
    enc_r_pso(re, cap); bind_vertex_streams(re, vsc);
    args_dirty_all(); encode_args(re, vsc, 0, 1); enc_r_vbuf(re, TESS_CAP_SLOT, g_capinfo);
    enc_r_draw_instanced(re, MTLPrimitiveTypePoint, (NSUInteger)first, (NSUInteger)span, (NSUInteger)instances, first_instance);
    enc_r_end(re);
    // 2. control kernel, 3. tessellator (the counter cleared first), the indirect arguments
    id<MTL4ComputeCommandEncoder> ce = enc_c_begin(NULL);
    g_args_compute = 1; args_dirty_all(); encode_args(ce, tcs, 3, 0); g_args_compute = 0;
    enc_c_pso(ce, tcs->geo_pso);
    enc_c_args(ce, g_rec, 22); enc_c_args(ce, g_map, 23); enc_c_args(ce, g_lv, 26); enc_c_args(ce, g_patch, 27); enc_c_args(ce, g_out, 28); enc_c_args(ce, g_ind, 29);
    NSUInteger tg = MAX((NSUInteger)outv, (NSUInteger)(64 / outv) * outv), threads = (NSUInteger)patches * outv;
    enc_c_dispatch(ce, MTLSizeMake((threads + tg - 1) / tg, 1, 1), MTLSizeMake(tg, 1, 1));
    [ce barrierAfterEncoderStages:MTLStageDispatch beforeEncoderStages:MTLStageDispatch visibilityOptions:MTL4VisibilityOptionDevice];
    enc_c_pso(ce, tess_kernel(0));
    enc_c_args(ce, g_tp, 0); enc_c_args(ce, g_lv, 1); enc_c_args(ce, g_dom, 2); enc_c_args(ce, g_cnt, 3);
    enc_c_dispatch(ce, MTLSizeMake(((NSUInteger)patches + 63) / 64, 1, 1), MTLSizeMake(64, 1, 1));
    [ce barrierAfterEncoderStages:MTLStageDispatch beforeEncoderStages:MTLStageDispatch visibilityOptions:MTL4VisibilityOptionDevice];
    enc_c_pso(ce, tess_kernel(1));
    enc_c_args(ce, g_tp, 0); enc_c_args(ce, g_cnt, 3); enc_c_args(ce, g_args, 4); enc_c_args(ce, pr->tess_stat.gpuAddress, 5);
    enc_c_dispatch(ce, MTLSizeMake(1, 1, 1), MTLSizeMake(1, 1, 1));
    enc_c_end(ce);
    args_dirty_all(); g_enc_pso = nil; g_pso_dirty = 1; g_dss_dirty = 1;
    // 4. raster: the evaluation function and the program's fragment shader (fitted to the bind state by prog_fit)
    if (!pr->tess_ras || pr->tess_ras->sh[0] != tes || pr->tess_ras_fs != (__bridge void *)pr->sh[1]) {
        NvProg *r = [NvProg new]; r->sh[0] = tes; r->sh[1] = pr->sh[1]; r->ctl[1] = pr->ctl[1]; r->code[1] = pr->code[1]; r->key[1] = pr->key[1];
        r->sig[1] = pr->sig[1]; r->alt[1] = pr->alt[1]; r->keeps = pr->keeps; pr->tess_ras = r; pr->tess_ras_fs = (__bridge void *)pr->sh[1]; }
    p_draw od = {4, 0, (int32_t)MIN(patches * 6, (uint64_t)INT32_MAX), 0, 1};
    g_tess_ras.active = 1; g_tess_ras.pr = pr->tess_ras; g_tess_ras.dom = g_dom; g_tess_ras.cp = g_out; g_tess_ras.patch = g_patch; g_tess_ras.args = g_args;
    draw_inner(&od, NULL);
    memset(&g_tess_ras, 0, sizeof g_tess_ras); args_dirty_stage(0); g_pso_dirty = 1;
    /* W3_TESS_PROBE=<first frame>: from that frame, for the first three tessellated draws, what the GPU produced (read when
       the submission completes): capture records, levels, control outputs, domain points, the indirect arguments */
    { static long from = -2; static int nprobe; if (from == -2) { const char *e = getenv("W3_TESS_PROBE"); from = e ? atol(e) : -1; }
      if (from >= 0 && (long)atomic_load(&g_cur_frame) >= from && nprobe < 3) { int np_ = ++nprobe; uint64_t P = patches; int ow = tcs->tess_out_stride / 4, pw = tcs->tess_patch_stride / 4;
        NSString *keys = [NSString stringWithFormat:@"vs %.12s tcs %.12s tes %.12s", vsc->key, tcs->key, tes->key];
        [g_commit_blocks addObject:^{ (void)b_rec; (void)b_dom;
            const float *rec = (const float *)p_rec; const uint16_t *lv = (const uint16_t *)p_lv; const float *out = (const float *)p_out, *pat = (const float *)p_patch;
            const uint32_t *dom = (const uint32_t *)p_dom, *args = (const uint32_t *)p_args;
            MLOG("[tess-probe %d] %s: %llu patches; args %u %u %u %u", np_, keys.UTF8String, (unsigned long long)P, args[0], args[1], args[2], args[3]);
            for (int r = 0; r < 2; r++) MLOG("[tess-probe %d] record %d: pos %g %g %g %g a0 %g %g %g %g a1 %g %g %g %g a2 %g %g %g %g", np_, r, rec[r * 144], rec[r * 144 + 1], rec[r * 144 + 2], rec[r * 144 + 3],
                                             rec[r * 144 + 16], rec[r * 144 + 17], rec[r * 144 + 18], rec[r * 144 + 19], rec[r * 144 + 20], rec[r * 144 + 21], rec[r * 144 + 22], rec[r * 144 + 23], rec[r * 144 + 24], rec[r * 144 + 25], rec[r * 144 + 26], rec[r * 144 + 27]);
            for (uint64_t q = 0; q < MIN(P, (uint64_t)4); q++) { __fp16 h[6]; memcpy(h, lv + q * 6, 12);
                MLOG("[tess-probe %d] patch %llu: levels outer %g %g %g %g inner %g %g; cp0 %g %g %g %g; patch %g %g %g %g", np_, (unsigned long long)q, (double)h[0], (double)h[1], (double)h[2], (double)h[3], (double)h[4], (double)h[5],
                     out[q * ow], out[q * ow + 1], out[q * ow + 2], out[q * ow + 3], pat[q * pw], pat[q * pw + 1], pat[q * pw + 2], pat[q * pw + 3]); }
            for (int k = 0; k < 6; k++) MLOG("[tess-probe %d] domain %d: patch %u uv %u/%u %u/%u", np_, k, dom[k * 2], dom[k * 2 + 1] & 255, (dom[k * 2 + 1] >> 8) & 255, (dom[k * 2 + 1] >> 16) & 255, dom[k * 2 + 1] >> 24);
        }]; } }
    g_tess_draws++; g_tess_patches += patches;
    if (g_tess_draws == 1 || g_tess_draws % 3000 == 0)
        MLOG("[tess] %llu 次曲面细分绘制在计算核上做完（%llu 个面片；变体未就绪跳过 %llu 次）；本程序上帧域点 %u 个（容量 %u，溢出面片 %u）", (unsigned long long)g_tess_draws,
             (unsigned long long)g_tess_patches, (unsigned long long)g_tess_skipped, ((const uint32_t *)pr->tess_stat.contents)[0], capacity, ((const uint32_t *)pr->tess_stat.contents)[1]);
}
