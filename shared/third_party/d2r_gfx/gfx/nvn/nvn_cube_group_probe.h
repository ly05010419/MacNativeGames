/* probe-gpu-cube-merge: the cube-face-set pass groups of nvn_cube_group.h through the production replay (mtl_exec,
   draw_inner, rp_begin_ex, the layered vertex variant, the copies), under the Metal validation layer.
   Scenarios: A three cubes x two levels in the game's order (two merged passes); B the same with a half-level scissor
   (the other half must keep A's content: the copy-in); C a pass with two draws (not taken, and the cube pass after it
   is alone); D another target between two cube passes (nothing taken); E seven cubes of one level (a full group of
   six and one plain). Every cube face of every level is read back and compared.
   TOTK_PROBE_NEGATIVE=18: no copy-in (B's untouched halves come out wrong); =19: every member drawn at layer 0 (the
   faces of the second and third cube come out wrong). */
#define CGP_N 7
static uint8_t *cgp_rt(uint8_t *w, const tex_t *t, int level) {
    p_set_rt r; memset(&r, 0, sizeof r); r.n = 6;
    for (int i = 0; i < 6; i++) { r.color[i] = t; r.has_cview[i] = 1; r.cview[i].set_levels = r.cview[i].set_layers = 1; r.cview[i].base_level = (uint8_t)level; r.cview[i].nlevels = 1;
        r.cview[i].min_layer = (uint16_t)i; r.cview[i].nlayers = 1; }
    return lp_rec(w, OP_SET_RT, &r, sizeof r);
}
static uint32_t cgp_px(int k, int f) { return (uint32_t)lroundf(255.f * (float)(k + 1) / 64.f) | (uint32_t)lroundf(255.f * (float)f / 8.f) << 8 | 128u << 16 | 255u << 24; }
static uint32_t cgp_want[CGP_N][2][6][256];
static void cgp_fill(int c, int lv, int f, int x0, int x1, uint32_t v) { int sz = 16 >> lv; for (int y = 0; y < sz; y++) for (int x = x0; x < x1 && x < sz; x++) cgp_want[c][lv][f][y * sz + x] = v; }
static void cgp_run(const char *name, const uint8_t *ops, uint64_t passes, uint64_t merged, uint64_t members, uint64_t plain, uint64_t taken) {
    uint64_t p0 = g_stat_pass, v0[4], v1[4]; mtl_cube_merge_stats(v0);
    mtl_exec(ops); rp_end(); cg_flush();   // the list ends without a submit: land what pends, as mtl_sub_commit would
    mtl_cube_merge_stats(v1);
    uint64_t dp = g_stat_pass - p0, dm = v1[1] - v0[1], dmm = v1[2] - v0[2], dpl = v1[3] - v0[3], dt = v1[0] - v0[0];
    MLOG("cube-merge probe %s: %llu passes, %llu merged (%llu members), %llu plain, %llu taken", name, (unsigned long long)dp, (unsigned long long)dm, (unsigned long long)dmm, (unsigned long long)dpl, (unsigned long long)dt);
    if (dp != passes || dm != merged || dmm != members || dpl != plain || dt != taken)
        MDIE("cube-merge probe %s: %llu passes / %llu merged / %llu members / %llu plain / %llu taken, expected %llu / %llu / %llu / %llu / %llu", name, (unsigned long long)dp, (unsigned long long)dm,
             (unsigned long long)dmm, (unsigned long long)dpl, (unsigned long long)dt, (unsigned long long)passes, (unsigned long long)merged, (unsigned long long)members, (unsigned long long)plain, (unsigned long long)taken);
}
int mtl_probe_cube_merge(void) {
    mtl_init(); setenv("TOTK_GPU_MIDCLEAR", "1", 1); g_cg_probe_neg = getenv("TOTK_PROBE_NEGATIVE") ? atoi(getenv("TOTK_PROBE_NEGATIVE")) : 0;
    if (!cg_on()) MDIE("cube-merge probe: cube merge is off (TOTK_CUBE_MERGE=0, TOTK_LAYER_MERGE=0 or DM_NVN_TBDR=1)");
    NSString *vsrc = @"#include <metal_stdlib>\nusing namespace metal;\n\nstruct main0_out\n{\n    float4 m_0 [[user(locn0)]];\n    float4 gl_Position [[position, invariant]];\n    float gl_PointSize [[point_size]];\n};\n\n"
        "vertex main0_out main0(uint gl_VertexIndex [[vertex_id]])\n{\n    main0_out out = {};\n    out.gl_PointSize = 1.0;\n    uint k = gl_VertexIndex / 3u, i = gl_VertexIndex % 3u;\n"
        "    out.gl_Position = float4(float((i << 1u) & 2u) * 2.0 - 1.0, float(i & 2u) * 2.0 - 1.0, 0.0, 1.0);\n"
        "    out.m_0 = float4(float(k + 1u) / 64.0, 0.25, 0.5, 1.0);\n    if (k >= 64u)\n    {\n        return out;\n    }\n    return out;\n}\n";
    NSString *fsrc = @"#include <metal_stdlib>\nusing namespace metal;\n\nstruct main0_out\n{\n    float4 c0 [[color(0)]];\n    float4 c1 [[color(1)]];\n    float4 c2 [[color(2)]];\n    float4 c3 [[color(3)]];\n    float4 c4 [[color(4)]];\n    float4 c5 [[color(5)]];\n};\n\n"
        "struct main0_in\n{\n    float4 m_0 [[user(locn0)]];\n};\n\n"
        "fragment main0_out main0(main0_in in [[stage_in]])\n{\n    main0_out out = {};\n    out.c0 = float4(in.m_0.x, 0.0, 0.5, 1.0);\n    out.c1 = float4(in.m_0.x, 0.125, 0.5, 1.0);\n    out.c2 = float4(in.m_0.x, 0.25, 0.5, 1.0);\n"
        "    out.c3 = float4(in.m_0.x, 0.375, 0.5, 1.0);\n    out.c4 = float4(in.m_0.x, 0.5, 0.5, 1.0);\n    out.c5 = float4(in.m_0.x, 0.625, 0.5, 1.0);\n    return out;\n}\n";
    NSError *e = nil; id<MTLLibrary> vl = [g_dev newLibraryWithSource:vsrc options:totk_precise_opts() error:&e], fl = vl ? [g_dev newLibraryWithSource:fsrc options:totk_precise_opts() error:&e] : nil;
    if (!vl || !fl) MDIE("cube-merge probe shaders: %s", e.localizedDescription.UTF8String);
    NvShader *vs = [NvShader new], *fs = [NvShader new];
    vs->fn = [vl newFunctionWithName:@"main0"]; vs->src = vsrc; strcpy(vs->key, "probe-cube-vs");
    fs->fn = [fl newFunctionWithName:@"main0"]; fs->src = fsrc; fs->stage = 1; strcpy(fs->key, "probe-cube-fs");
    NvProg *pr = [NvProg new]; pr->sh[0] = vs; pr->sh[1] = fs;
    prog_t p1; memset(&p1, 0, sizeof p1); p1.serial = 0xffffffc0; serial_put(p1.serial, pr);
    MTLTextureDescriptor *d = [MTLTextureDescriptor new]; d.pixelFormat = MTLPixelFormatRGBA8Unorm; d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; d.storageMode = MTLStorageModePrivate;
    d.textureType = MTLTextureTypeCube; d.width = d.height = 16; d.mipmapLevelCount = 2;
    id<MTLTexture> cubes[CGP_N]; tex_t ct[CGP_N];
    for (int c = 0; c < CGP_N; c++) {
        d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | (c == 5 ? 0 : MTLTextureUsageShaderWrite);   // cube 5: no shader write, its groups' copy-out falls back to blits
        cubes[c] = [g_dev newTextureWithDescriptor:d]; if (!cubes[c]) MDIE("cube-merge probe textures"); res_add(cubes[c]);
        memset(&ct[c], 0, sizeof ct[c]); ct[c].serial = 0xffffffc1 + (uint64_t)c; ct[c].d.format = 0x25; ct[c].d.width = ct[c].d.height = 16; ct[c].d.depth = 1; ct[c].d.levels = 2; ct[c].d.target = 8; ct[c].d.samples = 1;
        serial_put(ct[c].serial, cubes[c]);
    }
    d.textureType = MTLTextureType2D; d.mipmapLevelCount = 1; d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; id<MTLTexture> other = [g_dev newTextureWithDescriptor:d]; if (!other) MDIE("cube-merge probe texture"); res_add(other);
    tex_t ot; memset(&ot, 0, sizeof ot); ot.serial = 0xffffffd0; ot.d.format = 0x25; ot.d.width = ot.d.height = 16; ot.d.depth = 1; ot.d.levels = 1; ot.d.target = 1; ot.d.samples = 1; serial_put(ot.serial, other);
    mtl_sub_begin(); { op_hdr end = {OP_END, 8}; mtl_exec((const uint8_t *)&end); }   // replay state and an open submission
    for (int c = 0; c < CGP_N; c++) for (int lv = 0; lv < 2; lv++) {   // every face of every level starts transparent black
        MTL4RenderPassDescriptor *rp = [MTL4RenderPassDescriptor new]; rp.colorAttachments[0].texture = cubes[c]; rp.colorAttachments[0].level = (NSUInteger)lv;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0); rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        rp.renderTargetArrayLength = 6; ser_gate(); enc_r_end(enc_r_begin(rp));
    }
    memset(cgp_want, 0, sizeof cgp_want);
    static uint8_t ops[65536]; uint8_t *w;
    // A: three cubes x two levels in the game's order (lv0, lv1 of each cube): one merged pass per level
    w = lp_prog(ops, &p1); w = lp_sc(w, 0, 0, 16, 16);
    for (int c = 0; c < 3; c++) for (int lv = 0; lv < 2; lv++) { w = cgp_rt(w, &ct[c], lv); w = lp_draw(w, 2 * c + lv); for (int f = 0; f < 6; f++) cgp_fill(c, lv, f, 0, 16, cgp_px(2 * c + lv, f)); }
    lp_end(w); cgp_run("A three cubes, two levels", ops, 2, 2, 6, 0, 6);
    // A2: new content in the cubes through plain passes (another target between them keeps them apart), so that the
    // temps of A no longer match the cubes: B's copy-in is then the only way to keep the untouched halves right
    w = ops;
    for (int c = 0; c < 3; c++) for (int lv = 0; lv < 2; lv++) { w = cgp_rt(w, &ct[c], lv); w = lp_draw(w, 50 + 2 * c + lv); w = lp_rt(w, &ot, 0, 0); w = lp_draw(w, 63); for (int f = 0; f < 6; f++) cgp_fill(c, lv, f, 0, 16, cgp_px(50 + 2 * c + lv, f)); }
    lp_end(w); cgp_run("A2 plain passes refill the cubes", ops, 12, 0, 0, 0, 0);
    // B: a scissor on the left half of each level: the right half must keep A2's content (the copy-in)
    w = ops;
    for (int c = 0; c < 3; c++) for (int lv = 0; lv < 2; lv++) { w = lp_sc(w, 0, 0, 8 >> lv, 16 >> lv); w = cgp_rt(w, &ct[c], lv); w = lp_draw(w, 10 + 2 * c + lv); for (int f = 0; f < 6; f++) cgp_fill(c, lv, f, 0, 8 >> lv, cgp_px(10 + 2 * c + lv, f)); }
    w = lp_sc(w, 0, 0, 16, 16); lp_end(w); cgp_run("B half-level scissor (copy-in)", ops, 2, 2, 6, 0, 6);
    // C: a pass with two draws is not taken (the lookahead sees the second draw), so the cube pass after it is alone: nothing merges
    w = cgp_rt(ops, &ct[0], 0); w = lp_draw(w, 20); w = lp_draw(w, 21); w = cgp_rt(w, &ct[1], 0); w = lp_draw(w, 22); lp_end(w);
    for (int f = 0; f < 6; f++) { cgp_fill(0, 0, f, 0, 16, cgp_px(21, f)); cgp_fill(1, 0, f, 0, 16, cgp_px(22, f)); }
    cgp_run("C two draws in a pass", ops, 2, 0, 0, 0, 0);
    // D: another target between two cube passes: nothing is taken
    w = cgp_rt(ops, &ct[0], 1); w = lp_draw(w, 30); w = lp_rt(w, &ot, 0, 0); w = lp_draw(w, 31); w = cgp_rt(w, &ct[1], 1); w = lp_draw(w, 32); lp_end(w);
    for (int f = 0; f < 6; f++) { cgp_fill(0, 1, f, 0, 8, cgp_px(30, f)); cgp_fill(1, 1, f, 0, 8, cgp_px(32, f)); }
    cgp_run("D other target between", ops, 3, 0, 0, 0, 0);
    // E: seven cubes of level 0: a full group of six, the seventh alone
    w = ops; for (int c = 0; c < CGP_N; c++) { w = cgp_rt(w, &ct[c], 0); w = lp_draw(w, 40 + c); for (int f = 0; f < 6; f++) cgp_fill(c, 0, f, 0, 16, cgp_px(40 + c, f)); }
    lp_end(w); cgp_run("E seven cubes", ops, 2, 1, 6, 0, 6);
    // read back every face of every level
    id<MTLBuffer> rb[CGP_N]; for (int c = 0; c < CGP_N; c++) rb[c] = probe_buf(6 * 1024 + 6 * 256);
    for (int c = 0; c < CGP_N; c++) for (int lv = 0; lv < 2; lv++) for (int f = 0; f < 6; f++) { NSUInteger sz = (NSUInteger)(16 >> lv), off = lv ? 6 * 1024 + (NSUInteger)f * 256 : (NSUInteger)f * 1024;
        enc_b_copy_texture_buffer(blit(), cubes[c], (NSUInteger)f, (NSUInteger)lv, MTLOriginMake(0, 0, 0), MTLSizeMake(sz, sz, 1), rb[c], off, sz * 4, sz * sz * 4); }
    mtl_sub_commit(); mtl_finish();
    int checked = 0;
    for (int c = 0; c < CGP_N; c++) for (int lv = 0; lv < 2; lv++) for (int f = 0; f < 6; f++) { int sz = 16 >> lv; size_t off = lv ? 6 * 1024 + (size_t)f * 256 : (size_t)f * 1024;
        for (int p = 0; p < sz * sz; p++) { const uint8_t *got = (const uint8_t *)rb[c].contents + off + (size_t)p * 4; uint32_t want = cgp_want[c][lv][f][p];
            for (int ch = 0; ch < 4; ch++) { int wc = (int)(want >> (8 * ch) & 255);
                if (abs((int)got[ch] - wc) > 1) MDIE("cube-merge probe: cube %d level %d face %d pixel %d,%d channel %d = %u, expected %d", c, lv, f, p % sz, p / sz, ch, got[ch], wc);
                checked++; } } }
    uint64_t st[4]; mtl_cube_merge_stats(st);
    MLOG("cube-merge probe: %d pixel channels right; temps %d sets; %llu kernel copies and %llu blit copies in this probe (cube 5 is not shader-writable: its groups' copy-out blits)", checked, g_cg.ntemps, (unsigned long long)g_cg.kcopies, (unsigned long long)g_cg.copies);
    memset(&R, 0, sizeof R); state_reset(); g_cg_probe_neg = 0;
    serial_del(p1.serial); serial_del(ot.serial); for (int c = 0; c < CGP_N; c++) serial_del(ct[c].serial);
    for (int c = 0; c < CGP_N; c++) { defer_obj_release(rb[c]); defer_obj_release(cubes[c]); } defer_obj_release(other);
    return checked + 6 * 5;
}
