// Passthrough geometry drawn as mesh draws through mtl_exec, and those draws gathered into runs (nvn_geometry.inc geo_mesh_take).
// Six draws of the test lean kernel (test_dmsh_geometry.py --fixture): each with its own uniform (its depth) and its own storage
// counter, a scissor between the fourth and the fifth. Each triangle covers one pixel column of a 128x4 depth target; the kernel
// keeps it when bit 0 of vertex t's x is set. The depth of every column and every draw's counter must be the same whether the draws
// are dispatched one by one (DM_NVN_GEO_MESH_RUN=0) or in runs, and in runs the scissor must cut only the draws after it.
#define main geometry_fixture_main
#include "t_geometry.m"
#undef main
enum { MR_W = 128, MR_H = 4, MR_TRIS = 64, MR_DRAWS = 6 };
static const unsigned mr_first[MR_DRAWS] = {0, 5, 60, 37, 41, 61}, mr_count[MR_DRAWS] = {5, 32, 1, 4, 17, 3}, mr_scissored = 4;   // first triangle, triangles; draws from 4 on after the scissor
static int mr_bit(unsigned v) { return v % 5 != 2; }   // the kernel keeps triangle t of a draw when vertex (first + t)'s bit is set
static int run_case(NSString *directory, int pass) {
    NSError *error = nil;
    id<MTLLibrary> vl = [g_dev newLibraryWithURL:[NSURL fileURLWithPath:[directory stringByAppendingPathComponent:@"vertex.metallib"]] error:&error];
    id<MTLLibrary> ll = [g_dev newLibraryWithURL:[NSURL fileURLWithPath:[directory stringByAppendingPathComponent:@"lean.metallib"]] error:&error];
    if (!vl || !ll) { fprintf(stderr, "fixture missing: %s\n", error.description.UTF8String); return 1; }
    static NvProg *pr; static prog_t cp;
    if (!pr) {
        g_msl_dir = directory; g_shcache = [NSMutableDictionary dictionary];
        pr = [NvProg new];
        NvShader *vs = shader(vl, @"main0", 0, NULL, 0); strcpy(vs->key, "7e57000000000a00_b");
        vs->rf.nb = 1; vs->rf.b[0] = (nvn_rbind){.kind = NVN_RB_CBUF, .reg = 0, .aid = 0}; vs->ae = [vs->fn newArgumentEncoderWithBufferIndex:0]; vs->arg_len = vs->ae.encodedLength;
        vs->has_pull = 1; vs->rf.nv = 1; vs->rf.v[0].ct = NVN_CT_FLOAT; vs->rf.v[0].nvn = vs->rf.v[0].loc = 0;
        NvShader *fs = shader(vl, @"depth_only", 1, NULL, 0);
        NvShader *gs = shader(ll, @"main0", 2, NULL, 0); strcpy(gs->key, "7e57000000000b00_b"); gs->geo_vertices = 3; gs->geo_invocations = 1; gs->geo_output = 3; gs->geo_stride = 134; gs->geo_index_stride = 6;
        NvShader *lean = shader(ll, @"main0", 2, NULL, 0); strcpy(lean->key, "7e57000000000b00_bl3"); lean->geo_vertices = 3; lean->geo_invocations = 1;
        // slots by member order (the block is a plain struct: the translator numbers its ids 0, 1, 2 ...; the test kernel's 1, 2, 4 are only names)
        lean->rf.nb = 3; lean->rf.b[0] = (nvn_rbind){.kind = NVN_RB_GEOMETRY, .reg = 0, .aid = 0}; lean->rf.b[1] = (nvn_rbind){.kind = NVN_RB_GEOMETRY, .reg = 1, .aid = 1};
        lean->rf.b[2] = (nvn_rbind){.kind = NVN_RB_SBUFP, .reg = 0, .aid = 2, .rw = 1};
        lean->ae = [lean->fn newArgumentEncoderWithBufferIndex:0]; lean->arg_len = lean->ae.encodedLength;
        lean->geo_slot = malloc(GEO_CAPTURE_WORDS * sizeof *lean->geo_slot); for (int k = 0; k < GEO_CAPTURE_WORDS; k++) lean->geo_slot[k] = -1;
        lean->geo_slot[36] = 0; lean->geo_words = 1;   // reads.json: [36]
        g_shcache[@"7e57000000000b00_bl3"] = lean;
        pr->sh[0] = vs; pr->sh[1] = fs; pr->sh[2] = gs;
        static NSMutableArray *keep; keep = [NSMutableArray arrayWithObjects:pr, vs, fs, gs, lean, nil];
        cp.serial = 8950; serial_put(cp.serial, pr);
    }
    tex_t depth = {.magic = MAGIC('T','E','X',0), .serial = 8960 + (uint64_t)pass};
    depth.d.width = MR_W; depth.d.height = MR_H; depth.d.depth = depth.d.samples = depth.d.levels = 1; depth.d.target = NVN_TEXTURE_TARGET_2D; depth.d.format = NVN_FORMAT_DEPTH32F;
    uint8_t *mem; if (posix_memalign((void **)&mem, getpagesize(), 131072)) abort(); memset(mem, 0, 131072); mtl_pool_add(mem, 131072);
    float *vtx = (float *)(mem + 65536);   // triangle G covers pixel column 2G at every row: (x0, -1) (x0, 3) (x0 + 2w, -1), w a column
    for (unsigned g = 0; g < MR_TRIS; g++) {
        float w = 2.0f / MR_W, x0 = -1 + 2 * g * w;
        const float xy[3][2] = {{x0, -1}, {x0, 3}, {x0 + 2 * w, -1}};
        for (unsigned c = 0; c < 3; c++) { unsigned v = 3 * g + c; float *p = vtx + 4 * v; p[0] = xy[c][0]; p[1] = xy[c][1]; p[2] = 0; p[3] = 1;
            uint32_t bits; memcpy(&bits, &p[0], 4); bits = (bits & ~1u) | (uint32_t)mr_bit(v); memcpy(&p[0], &bits, 4); }
    }
    float *uni = (float *)(mem + 98304); uint32_t *ctr = (uint32_t *)(mem + 102400);
    for (unsigned k = 0; k < MR_DRAWS; k++) uni[64 * k + 2] = 0.1f * (k + 1);   // each draw's own depth, 256 bytes apart
    size_t used = 0;
    #define EMIT(OP,X) do {size_t z=(8+sizeof(X)+7)&~7ull;op_hdr h={OP,(uint32_t)z};memcpy(mem+used,&h,8);memcpy(mem+used+8,&(X),sizeof(X));used+=z;}while(0)
    p_set_rt rt = {.n = 0}; rt.depth = &depth; EMIT(OP_SET_RT, rt);
    p_viewports vp = {.n = 1}; vp.v[0][2] = MR_W; vp.v[0][3] = MR_H; EMIT(OP_VIEWPORTS, vp);
    p_scissor all = {0, 0, MR_W, MR_H}; EMIT(OP_SCISSOR, all);
    p_clear_ds cl = {.depth = 1, .depth_mask = 1}; EMIT(OP_CLEAR_DS, cl);
    p_bind_program bp = {&cp, 7}; EMIT(OP_BIND_PROGRAM, bp);
    p_bind_vattr va = {.n = 1}; va_st attribute = {.fmt = NVN_FORMAT_RGBA32F}; memcpy(va.st, &attribute, 4); EMIT(OP_BIND_VATTR, va);
    p_bind_vstream stream = {.n = 1}; stream.st[0].stride = 16; EMIT(OP_BIND_VSTREAM, stream);
    p_bind_vbuf vb = {.n = 1}; vb.r[0] = (NVNbufferRange){(uint64_t)vtx, MR_TRIS * 3 * 16}; EMIT(OP_BIND_VBUF, vb);
    for (unsigned k = 0; k < MR_DRAWS; k++) {
        if (k == mr_scissored) { p_scissor cut = {0, 0, 120, MR_H}; EMIT(OP_SCISSOR, cut); }   // ends the run: columns 120 on keep the earlier draws, not the later ones
        p_bind_buf u = {.stage = 0, .index = 0, .addr = (uint64_t)(uni + 64 * k), .size = 16}; EMIT(OP_BIND_UBO, u);
        p_bind_buf c = {.stage = 2, .index = 0, .addr = (uint64_t)(ctr + 4 * k), .size = 16}; EMIT(OP_BIND_SSBO, c);
        p_draw d = {.prim = 4, .first = (int32_t)(3 * mr_first[k]), .count = (int32_t)(3 * mr_count[k]), .instances = 1}; EMIT(OP_DRAW, d);
    }
    op_hdr end = {OP_END, 8}; memcpy(mem + used, &end, 8);
    #undef EMIT
    state_reset(); memset(&R, 0, sizeof R); S.ds.dt = 1; S.ds.dw = 1; S.ds.df = 2;   // LESS, written
    uint64_t runs0 = g_mrun_runs, rdraws0 = g_mrun_draws, mesh0 = g_geo_mesh_n;
    mtl_sub_begin(); uint64_t h = (uint64_t)mem; mtl_sub_index(&h, 1); mtl_exec(mem);
    id<MTLBuffer> b = [g_dev newBufferWithLength:MR_W * MR_H * 4 options:MTLResourceStorageModeShared]; res_add(b);
    [blit() copyFromTexture:tex_mtl(&depth) sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(MR_W, MR_H, 1) toBuffer:b destinationOffset:0 destinationBytesPerRow:MR_W * 4 destinationBytesPerImage:MR_W * MR_H * 4];
    finish();
    int ready = g_geo_mesh_n - mesh0 == MR_DRAWS && (!geo_mesh_run_on() || g_mrun_draws - rdraws0 == MR_DRAWS);   // the pipelines are built in the background: earlier passes draw some other way
    int bad = 0;
    if (ready) {
        const float *px = b.contents;
        for (unsigned g = 0; g < MR_TRIS; g++) {
            float want = 1;
            for (unsigned k = 0; k < MR_DRAWS; k++) if (g >= mr_first[k] && g < mr_first[k] + mr_count[k] && mr_bit(3 * mr_first[k] + g - mr_first[k]) && (k < mr_scissored || 2 * g < 120)) want = 0.1f * (k + 1);
            for (unsigned y = 0; y < MR_H; y++) { float got = px[y * MR_W + 2 * g];
                if (fabsf(got - want) > 1e-5f) { fprintf(stderr, "triangle %u row %u: depth %g, expected %g\n", g, y, got, want); bad = 1; } }
        }
        for (unsigned k = 0; k < MR_DRAWS; k++) EXPECT("each draw's kernel ran once per triangle with its own counter", mr_count[k], ctr[4 * k]);
        if (geo_mesh_run_on()) EXPECT("the scissor and the end split the draws into two runs", 2, g_mrun_runs - runs0);
        else EXPECT("no runs with DM_NVN_GEO_MESH_RUN=0", 0, g_mrun_runs - runs0);
    }
    res_remove(b); mtl_tex_release(&depth); mtl_pool_del(mem); mtl_drain(); free(mem);
    return bad ? -1 : ready;
}
int main(int argc, char **argv) { @autoreleasepool {
    if (argc != 2) return 2; mtl_init();
    int r = 0;
    for (int pass = 0; pass < 40 && !(r = run_case(@(argv[1]), pass)); pass++) usleep(100000);   // until the mesh pipelines (single and run) are built
    if (r < 0 || failed) return 1;
    if (!r) { fprintf(stderr, "the mesh pipelines were never ready\n"); return 1; }
    printf("geometry mesh %s: %d checks passed\n", geo_mesh_run_on() ? "runs" : "single draws", checks); return 0;
}}
