/* Lean geometry draws (included by nvn_geometry_rt.inc). A passthrough geometry shader's lean kernel (the translator's
   GeometryLean.cs, from seedds/nxnative dmsh_geometry.lean) writes only each output vertex's viewport mask and reads only
   the capture words it decides with. The host kernels below (seedds/nxnative dm/gfx/msl/geo_pack.metal, MIT) turn the kept
   primitives into the original vertex numbers, and the draw rasterizes the game's own vertex and fragment shaders through
   them: no 144-word capture record, no 134-word output vertex, no reader.
   - triangle input (direct): geo_lean_tri writes three vertex numbers per input triangle, a dropped triangle collapsing onto
     the draw's first vertex (zero area: nothing drawn), and the draw takes all of them;
   - point and line input (packed): a dropped point cannot be collapsed, so geo_lean_cull turns dropped output vertices into
     restarts, the pack kernels compact the strips into a list in primitive order (count per primitive, scan, write; the
     count lands in the indirect arguments), geo_lean_index maps them to vertex numbers, and the draw is indirect.
   Taken for one instance, one invocation, and a vertex shader without memory writes (it runs twice: capture and raster);
   TOTK_GEO_LEAN=0 keeps the full kernel (comparison). */
static const char k_geo_lean_msl[] =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "struct geo_lean { device uint *raw; device const uint *mask; device const uint *map; device uint *dst; device uint *args; uint groups, stride, first, first_instance, capacity, collapse; };\n"
    "kernel void geo_lean_tri(constant geo_lean& j [[buffer(0)]], uint g [[thread_position_in_grid]]) {\n"
    " if (g >= j.groups) return;\n"
    " uint v[3] = {0, 0, 0}, n = 0; bool keep = true;\n"
    " for (uint i = 0; i < j.stride; i++) { uint x = j.raw[g * j.stride + i]; if (x == 0xffffffffu) { if (n) break; continue; } if (n < 3) v[n] = x; n++; }\n"
    " keep = n == 3; for (uint k = 0; k < 3 && keep; k++) keep = (j.mask[v[k]] & 1u) != 0u;\n"
    " for (uint k = 0; k < 3; k++) j.dst[3 * g + k] = keep ? j.map[v[k]] + j.first : j.collapse;\n"
    "}\n"
    "kernel void geo_lean_cull(constant geo_lean& j [[buffer(0)]], uint g [[thread_position_in_grid]]) {\n"
    " if (g >= j.groups) return;\n"
    " for (uint i = 0; i < j.stride; i++) { uint v = j.raw[g * j.stride + i]; if (v != 0xffffffffu && !(j.mask[v] & 1u)) j.raw[g * j.stride + i] = 0xffffffffu; }\n"
    "}\n"
    "kernel void geo_lean_index(constant geo_lean& j [[buffer(0)]], uint i [[thread_position_in_grid]]) {\n"
    " if (i == 0) j.args[4] = j.first_instance;\n"
    " if (i < j.args[0] && i < j.capacity) j.dst[i] = j.map[j.dst[i]] + j.first;\n"
    "}\n"
    "struct geo_pack { device const uint *src; device uint *dst; device uint *args; uint groups, stride, topology, capacity; device uint *cnt; };\n"
    "static bool geo_room(constant geo_pack& j, uint o, uint k, uint n) { return o <= j.capacity && k <= j.capacity - o && n <= j.capacity - o - k; }\n"
    "static uint geo_group(constant geo_pack& j, uint g, bool w, uint o) {\n"
    " uint a = 0, b = 0, n = 0, k = 0;\n"
    " for (uint i = 0; i < j.stride; i++) { uint v = j.src[g * j.stride + i];\n"
    "  if (v == 0xffffffffu) { n = 0; continue; }\n"
    "  if (j.topology == 1) { if (w && geo_room(j, o, k, 1)) j.dst[o + k] = v; k += 1; }\n"
    "  else if (j.topology == 6) { if (n) { if (w && geo_room(j, o, k, 2)) { j.dst[o + k] = b; j.dst[o + k + 1] = v; } k += 2; } }\n"
    "  else if (n >= 2) { if (w && geo_room(j, o, k, 3)) { j.dst[o + k] = (n & 1) ? b : a; j.dst[o + k + 1] = (n & 1) ? a : b; j.dst[o + k + 2] = v; } k += 3; }\n"
    "  a = b; b = v; n++;\n"
    " }\n"
    " return k;\n"
    "}\n"
    "kernel void geo_pack_count(constant geo_pack& j [[buffer(0)]], uint g [[thread_position_in_grid]], uint block [[threadgroup_position_in_grid]],\n"
    " uint li [[thread_index_in_threadgroup]], uint si [[simdgroup_index_in_threadgroup]], uint sl [[thread_index_in_simdgroup]], uint sw [[threads_per_simdgroup]]) {\n"
    " threadgroup uint sums[64]; uint c = g < j.groups ? geo_group(j, g, false, 0) : 0u;\n"
    " uint local = simd_prefix_exclusive_sum(c), total = simd_sum(c); if (sl == 0) sums[si] = total;\n"
    " threadgroup_barrier(mem_flags::mem_threadgroup); uint before = 0; for (uint s = 0; s < si; s++) before += sums[s];\n"
    " if (g < j.groups) j.cnt[g] = before + local;\n"
    " if (li == 0) { uint n = 0; for (uint s = 0; s < (64u + sw - 1u) / sw; s++) n += sums[s]; j.cnt[j.groups + block] = n; }\n"
    "}\n"
    "kernel void geo_pack_scan(constant geo_pack& j [[buffer(0)]], uint li [[thread_index_in_threadgroup]], uint3 nt [[threads_per_threadgroup]]) {\n"
    " threadgroup ulong sums[256]; uint blocks = j.groups / 64u + uint(j.groups % 64u != 0u), chunk = (blocks + nt.x - 1u) / nt.x, lo = li * chunk, hi = min(lo + chunk, blocks); ulong s = 0;\n"
    " for (uint b = lo; b < hi; b++) s += j.cnt[j.groups + b]; sums[li] = s; threadgroup_barrier(mem_flags::mem_threadgroup);\n"
    " for (uint step = 1; step < nt.x; step <<= 1) { ulong add = li >= step ? sums[li - step] : 0; threadgroup_barrier(mem_flags::mem_threadgroup); sums[li] += add; threadgroup_barrier(mem_flags::mem_threadgroup); }\n"
    " s = li ? sums[li - 1] : 0; for (uint b = lo; b < hi; b++) { uint c = j.cnt[j.groups + b]; j.cnt[j.groups + b] = uint(min(s, ulong(0xffffffffu))); s += c; }\n"
    " if (li == 0) { ulong total = sums[nt.x - 1]; uint w = j.topology == 1 ? 1u : j.topology == 6 ? 2u : 3u, n = total <= j.capacity ? uint(total) : j.capacity - j.capacity % w;\n"
    "  j.args[0] = n; j.args[1] = 1; j.args[2] = 0; j.args[3] = 0; j.args[4] = 0; }\n"
    "}\n"
    "kernel void geo_pack_write(constant geo_pack& j [[buffer(0)]], uint g [[thread_position_in_grid]]) {\n"
    " if (g < j.groups) { ulong o = ulong(j.cnt[g]) + j.cnt[j.groups + g / 64u]; if (o <= 0xffffffffu) geo_group(j, g, true, uint(o)); }\n"
    "}\n";
enum { GLK_TRI, GLK_CULL, GLK_INDEX, GLK_COUNT, GLK_SCAN, GLK_WRITE, GLK_N };
enum { GEO_SCAN_THREADS = 128 };   // geo_pack_scan's one threadgroup (its sums hold 256)
static id<MTLComputePipelineState> g_geo_lean_k[GLK_N];
static int geo_lean_kernels(void) {   // the host kernels, built once; 0 when they cannot be (every draw keeps its full kernel)
    static int state;   // 0 not tried, 1 built, -1 failed
    if (state) return state > 0;
    static const char *names[GLK_N] = {"geo_lean_tri", "geo_lean_cull", "geo_lean_index", "geo_pack_count", "geo_pack_scan", "geo_pack_write"};
    NSError *e = nil; id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(k_geo_lean_msl) options:nil error:&e];
    state = lib ? 1 : -1;
    for (int i = 0; i < GLK_N && state > 0; i++) {
        id<MTLFunction> fn = [lib newFunctionWithName:@(names[i])];
        g_geo_lean_k[i] = fn ? [g_dev newComputePipelineStateWithFunction:fn error:&e] : nil;
        if (!g_geo_lean_k[i] || g_geo_lean_k[i].maxTotalThreadsPerThreadgroup < GEO_SCAN_THREADS) state = -1;
    }
    if (state < 0) MLOG("[RED] lean geometry host kernels: %s; geometry draws keep their full kernels", e ? e.localizedDescription.UTF8String : "no function or too small a threadgroup");
    return state > 0;
}
static int geo_vs_writes(NvShader *vs) {   // a vertex shader that writes memory: running it for the capture and again for the raster would write twice
    for (int i = 0; i < vs->rf.nb; i++) { const nvn_rbind *b = &vs->rf.b[i];
        if ((b->kind == NVN_RB_SBUFP && b->rw) || b->kind == NVN_RB_SBUFU || b->kind == NVN_RB_GMEML || b->kind == NVN_RB_IMG) return 1; }
    return 0;
}
// The lean kernel this draw can take instead of gs (its points, lines or triangles in, one vertex per input vertex out), or nil
static NvShader *geo_lean_pick(NvProg *pr, NvShader *gs, NvShader *lean, int topology, int instances) {
    static int on = -1; if (on < 0) { const char *e = getenv("TOTK_GEO_LEAN"); on = !(e && !strcmp(e, "0")); }
    int width = topology == 0 ? 1 : topology == 1 ? 2 : topology == 3 ? 3 : 0;
    if (!on || !lean || !width || instances != 1 || gs->geo_invocations != 1 || lean->geo_invocations != 1 || lean->geo_vertices != width || !lean->geo_slot ||
        (width == 1 && lean->geo_output != 1) || (width == 2 && lean->geo_output != 6) || !pr->sh[1] || geo_vs_writes(pr->sh[0])) return nil;
    if (!lean->geo_pso) { NSError *e = nil; lean->geo_pso = [g_dev newComputePipelineStateWithFunction:lean->fn error:&e];
        if (!lean->geo_pso) { RED_ONCE(172, "lean geometry kernel %s: %s; the full kernel draws", lean->key, e.localizedDescription.UTF8String); lean->geo_slot = NULL; return nil; } }
    return geo_lean_kernels() ? lean : nil;
}
/* Whether a lean draw takes the packed form: point and line input always; triangle input only under TOTK_GEO_LEAN_PACK=1
   (a test of the packed path on triangle draws, which every game here has; Bananza's geometry draws are all triangles) */
static int geo_lean_packed(int width) {
    static int force = -1; if (force < 0) { const char *e = getenv("TOTK_GEO_LEAN_PACK"); force = e && !strcmp(e, "1"); if (force) MLOG("TOTK_GEO_LEAN_PACK=1: lean triangle draws take the packed form (test)"); }
    return width != 3 || force;
}
// Scratch a lean draw adds: its vertex numbers, and for the packed form the per-primitive counts and the indirect arguments
static uint64_t geo_lean_bytes(uint64_t np, int width) {
    uint64_t cap = np * (uint64_t)width, blocks = (np + 63) / 64;
    return cap * 4 + (geo_lean_packed(width) ? (np + blocks) * 4 + 32 : 0) + sizeof(nvn_geo_lean_job) + sizeof(nvn_geo_pack_job) + 4 * 256;
}
// Lays out and fills a lean draw's records in [gpu, cpu) (geo_lean_bytes of it); the job's g_ib, g_out and g_map are set
static void geo_lean_setup(geo_job *j, NvShader *lean, int width, uint64_t gpu, uint8_t *cpu) {
    uint64_t cap = j->np * (uint64_t)width, blocks = (j->np + 63) / 64; size_t used = 0; uint8_t *p_lean, *p_pack, *p_args, *unused;
    #define LEAN_ALLOC(N, G, P) do { used = (used + 255) & ~(size_t)255; (G) = gpu + used; (P) = cpu + used; used += (size_t)(N); } while (0)
    LEAN_ALLOC(cap * 4, j->g_dst, unused); LEAN_ALLOC(sizeof(nvn_geo_lean_job), j->g_lean, p_lean); LEAN_ALLOC(sizeof(nvn_geo_pack_job), j->g_pack, p_pack);
    int packed = geo_lean_packed(width);
    if (packed) { LEAN_ALLOC((j->np + blocks) * 4, j->g_cnt, unused); LEAN_ALLOC(32, j->g_args, p_args); memset(p_args, 0, 32); }
    #undef LEAN_ALLOC
    (void)unused;
    if (used > geo_lean_bytes(j->np, width)) MDIE("lean geometry scratch accounting");
    nvn_geo_lean_job lj = {j->g_ib, j->g_out, j->g_map, j->g_dst, j->g_args, (uint32_t)j->np, (uint32_t)lean->geo_index_stride, j->first, j->first_instance, (uint32_t)cap, j->first};
    nvn_geo_pack_job pj = {j->g_ib, j->g_dst, j->g_args, (uint32_t)j->np, (uint32_t)lean->geo_index_stride, (uint32_t)lean->geo_output, (uint32_t)cap, j->g_cnt};
    memcpy(p_lean, &lj, sizeof lj); memcpy(p_pack, &pj, sizeof pj);
    j->lean = packed ? 2 : 1; j->icap = (uint32_t)cap; j->list_prim = width == 1 ? NVN_DRAW_PRIMITIVE_POINTS : width == 2 ? NVN_DRAW_PRIMITIVE_LINES : NVN_DRAW_PRIMITIVE_TRIANGLES;
}
/* After a run's geometry kernels: one phase per step, each over every lean job of the run, with one barrier between phases
   (the jobs share no memory: geo_run_conflict), instead of a barrier after every job */
static void geo_lean_phases(id<MTL4ComputeCommandEncoder> ce, const geo_job *jobs, int n) {
    int direct = 0, packed = 0;
    for (int i = 0; i < n; i++) { direct += jobs[i].lean == 1; packed += jobs[i].lean == 2; }
    if (!direct && !packed) return;
    #define PHASE(KERNEL, WHICH, RECORD, GROUPS, THREADS) do { \
        [ce barrierAfterEncoderStages:MTLStageDispatch beforeEncoderStages:MTLStageDispatch visibilityOptions:MTL4VisibilityOptionDevice]; \
        enc_c_pso(ce, g_geo_lean_k[KERNEL]); \
        for (int i = 0; i < n; i++) { const geo_job *j = &jobs[i]; if (!(WHICH)) continue; \
            enc_c_args(ce, j->RECORD, 0); enc_c_dispatch(ce, MTLSizeMake((NSUInteger)(GROUPS), 1, 1), MTLSizeMake(THREADS, 1, 1)); } } while (0)
    // the masks the kernels wrote: kept triangles to vertex numbers; dropped points / lines to restarts
    [ce barrierAfterEncoderStages:MTLStageDispatch beforeEncoderStages:MTLStageDispatch visibilityOptions:MTL4VisibilityOptionDevice];
    for (int i = 0; i < n; i++) { const geo_job *j = &jobs[i]; if (!j->lean) continue;
        enc_c_pso(ce, g_geo_lean_k[j->lean == 1 ? GLK_TRI : GLK_CULL]); enc_c_args(ce, j->g_lean, 0);
        enc_c_dispatch(ce, MTLSizeMake((NSUInteger)((j->np + 63) / 64), 1, 1), MTLSizeMake(64, 1, 1)); }
    if (packed) {
        PHASE(GLK_COUNT, j->lean == 2, g_pack, (j->np + 63) / 64, 64);
        PHASE(GLK_SCAN, j->lean == 2, g_pack, 1, GEO_SCAN_THREADS);
        PHASE(GLK_WRITE, j->lean == 2, g_pack, (j->np + 63) / 64, 64);
        PHASE(GLK_INDEX, j->lean == 2, g_lean, (j->icap + 63) / 64, 64);
    }
    #undef PHASE
    args_dirty_all();
}
// A lean draw: the program's own vertex and fragment stages (as prog_fit fits them to the bind state) through the kept primitives
static void geo_raster_lean(NvProg *pr, const geo_job *j) {
    if (!pr->sh[1]) return;
    NvProg *r = pr->geo_lean_ras;
    if (!r) { r = [NvProg new]; pr->geo_lean_ras = r; }
    for (int st = 0; st < 2; st++) { r->sh[st] = pr->sh[st]; r->ctl[st] = pr->ctl[st]; r->code[st] = pr->code[st]; r->key[st] = pr->key[st]; r->sig[st] = pr->sig[st]; r->alt[st] = pr->alt[st]; }
    r->keeps = pr->keeps;
    p_draw_idx od = {0}; od.prim = j->list_prim; od.count = (int32_t)j->icap; od.itype = 2; od.instances = 1; od.base_instance = (int32_t)j->first_instance;
    g_geo_ras.pr = r; g_geo_ras.ib = j->g_dst; g_geo_ras.ib_len = (size_t)j->icap * 4; g_geo_ras.args = j->lean == 2 ? j->g_args : 0;   // packed: the GPU's count
    args_dirty_stage(0);
    draw_inner(NULL, &od);
    memset(&g_geo_ras, 0, sizeof g_geo_ras); args_dirty_stage(0); g_pso_dirty = 1;
}
