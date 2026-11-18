// Logic operations through the replay: ColorStateSetLogicOp records, the pipeline from the shader's logic operation library (<key>_bl, loaded by the host from the library
// directory) and the operation and channel masks at its fragment buffer, drawn on the GPU. For every operation a row gets a destination by a COPY draw, then the operation's draw
// with a source, into RG32Uint, RGBA8Uint, RGBA8Unorm and RGBA16Unorm targets (the result cut to the target's bits); every operation shares one pipeline; a target with no bits to combine keeps COPY (lop_key).
// make GAME=<id> check_t_logic (the fixture comes from test_dmsh_logic.py --fixture <dir>; ./t_logic <dir>)
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); } } while (0)
static uint32_t op(int f, uint32_t s, uint32_t d) {
    switch (f) { case 0: return 0; case 1: return s & d; case 2: return s & ~d; case 4: return ~s & d; case 5: return d; case 6: return s ^ d; case 7: return s | d; case 8: return ~(s | d);
        case 9: return ~(s ^ d); case 10: return ~d; case 11: return s | ~d; case 12: return ~s; case 13: return ~s | d; case 14: return ~(s & d); case 15: return ~0u; default: return s; }
}
static NvProg *prog(NSString *dir, prog_t *program, NvShader **out) {
    NSError *error = nil; NSString *source = [NSString stringWithContentsOfFile:[dir stringByAppendingString:@"/base.metal"] encoding:NSUTF8StringEncoding error:&error];
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:source options:nil error:&error];
    if (!lib) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); abort(); }
    NvProg *pr = [NvProg new];
    pr->sh[0] = [NvShader new]; pr->sh[0]->fn = [lib newFunctionWithName:@"vertex_main"]; pr->sh[0]->stage = 0; snprintf(pr->sh[0]->key, sizeof pr->sh[0]->key, "vl_b");
    NvShader *ps = [NvShader new]; pr->sh[1] = ps; *out = ps;
    ps->fn = fn4_note([lib newFunctionWithName:@"main0"], lib, @"main0", nil); ps->fn_u = fn4_note([lib newFunctionWithName:@"main0_u"], lib, @"main0_u", nil);
    ps->stage = 1; ps->has_logic = 1; ps->rf.color_known = 1; ps->rf.color_mask = 1;
    memset(ps->key, '1', 16); memcpy(ps->key + 16, "_b", 3);   // the host loads <dir>/<16 digits>_bl.metallib
    ps->ae = [ps->fn newArgumentEncoderWithBufferIndex:0]; ps->arg_len = ps->ae.encodedLength; ps->rf.nb = 1; ps->rf.b[0] = (nvn_rbind){.kind = NVN_RB_CBUF, .reg = 0, .aid = 0};
    program->serial = 97000; serial_put(program->serial, pr);
    return pr;
}
static void run(NvProg *pr, prog_t *program, NvShader *ps, int fmt, int bits) {   // fmt: the target's NVN format; bits: per channel
    tex_t target = {.magic = MAGIC('T', 'E', 'X', 0), .serial = 97100 + (uint64_t)fmt};
    target.d = (nvn_texdesc){.width = 4, .height = 16, .depth = 1, .samples = 1, .levels = 1, .target = NVN_TEXTURE_TARGET_2D, .format = fmt};
    uint8_t *commands; if (posix_memalign((void **)&commands, getpagesize(), 65536)) abort(); memset(commands, 0, 65536); mtl_pool_add(commands, 65536);
    uint32_t mask = bits == 32 ? ~0u : (1u << bits) - 1, dst[4] = {0x0ff0f00f & mask, 0x12345678 & mask, 0xa5 & mask, 0x3c & mask}, src[4] = {0x00ffff00 & mask, 0x9abcdef0 & mask, 0x5a & mask, 0x0f & mask};
    memcpy(commands + 60000, dst, 16); memcpy(commands + 60016, src, 16);
    int normalized = fmt == NVN_FORMAT_RGBA8 || fmt == NVN_FORMAT_RGBA16;   // main0 writes the uniform's words as floats: the values that store as dst and src
    if (normalized) { float d[4], s[4]; for (int c = 0; c < 4; c++) { d[c] = (float)dst[c] / (float)mask; s[c] = (float)src[c] / (float)mask; } memcpy(commands + 60000, d, 16); memcpy(commands + 60016, s, 16); }
    size_t used = 0;
    #define EMIT(OP, X) do { size_t z = (8 + sizeof(X) + 7) & ~7ull; op_hdr h = {OP, (uint32_t)z}; memcpy(commands + used, &h, 8); memcpy(commands + used + 8, &(X), sizeof(X)); used += z; } while (0)
    p_set_rt rt = {.n = 1}; rt.color[0] = &target; EMIT(OP_SET_RT, rt);
    p_viewports vp = {.n = 1}; vp.v[0][2] = 4; vp.v[0][3] = 16; EMIT(OP_VIEWPORTS, vp);
    p_bind_program bp = {program, 3}; EMIT(OP_BIND_PROGRAM, bp);
    for (int f = 0; f < 16; f++) {
        p_scissor sc = {0, f, 4, 1}; EMIT(OP_SCISSOR, sc);
        for (int k = 0; k < 2; k++) {   // the destination by COPY, then the operation with the source
            p_bind_state bs = {.kind = 1}; color_st cs = {0, k ? (uint32_t)f : NVN_LOGIC_OP_COPY, NVN_ALPHA_FUNC_ALWAYS}; memcpy(bs.bytes, &cs, sizeof cs); EMIT(OP_BIND_STATE, bs);
            p_bind_buf ub = {.stage = 1, .index = 0, .addr = (uint64_t)(commands + 60000 + 16 * k), .size = 16}; EMIT(OP_BIND_UBO, ub);
            p_draw dr = {.prim = 4, .count = 3, .instances = 1}; EMIT(OP_DRAW, dr);
        }
    }
    op_hdr end = {OP_END, 8}; memcpy(commands + used, &end, 8);
    #undef EMIT
    state_reset(); CHECK(S.color.logic_op == NVN_LOGIC_OP_COPY);
    uint32_t psos = g_pso.n;
    memset(&R, 0, sizeof R); mtl_sub_begin(); uint64_t handle = (uint64_t)commands; mtl_sub_index(&handle, 1); mtl_exec(commands);
    id<MTLBuffer> pixels = [g_dev newBufferWithLength:256 * 16 options:MTLResourceStorageModeShared];
    [blit() copyFromTexture:tex_mtl(&target) sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(4, 16, 1) toBuffer:pixels destinationOffset:0 destinationBytesPerRow:256 destinationBytesPerImage:256 * 16];
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull)); CHECK(!mtl_last_commit_error());
    CHECK(g_pso.n - psos == 2u);   // the operation is data: one pipeline with it (every operation but COPY), one without
    CHECK(ps->lfn_state == 1 && ps->lfn.count == (normalized ? 2u : 1u));   // main0_u, then main0 for the normalized targets too: one entry per targets and entry point
    int nch = fmt == NVN_FORMAT_RG32UI ? 2 : 4;
    for (int f = 0; f < 16; f++) for (int x = 0; x < 4; x++) for (int c = 0; c < nch; c++) {
        const uint8_t *p = (const uint8_t *)pixels.contents + f * 256 + x * nch * bits / 8;
        uint32_t got = bits == 32 ? ((const uint32_t *)p)[c] : bits == 16 ? ((const uint16_t *)p)[c] : p[c], want = op(f, src[c], dst[c]) & mask;
        if (got != want) fprintf(stderr, "format %#x operation %d x %d channel %d: %#x, want %#x\n", fmt, f, x, c, got, want);
        CHECK(got == want);
    }
    mtl_pool_del(commands); mtl_drain(); free(commands);
}
int main(int argc, char **argv) { @autoreleasepool {
    if (argc != 2) return 2;
    setenv("DM_MSL_DIR", argv[1], 1);
    mtl_init(); g_msl_dir = @(argv[1]);   // the fixture is the library directory itself (no b-<chain> folder)
    static prog_t program; NvShader *ps; NvProg *pr = prog(g_msl_dir, &program, &ps);
    run(pr, &program, ps, NVN_FORMAT_RG32UI, 32); run(pr, &program, ps, NVN_FORMAT_RGBA8UI, 8);
    run(pr, &program, ps, NVN_FORMAT_RGBA8, 8); run(pr, &program, ps, NVN_FORMAT_RGBA16, 16);
    pso_key k; memset(&k, 0, sizeof k); k.cfmt[0] = MTLPixelFormatRG32Uint; k.chmask = 0xf; S.color.logic_op = NVN_LOGIC_OP_OR;
    CHECK(lop_key(&k, ps, 0) == 1); CHECK(lop_key(&k, ps, 1) == 0);   // not in a merged pass
    k.cfmt[0] = MTLPixelFormatRGBA8Unorm; CHECK(lop_key(&k, ps, 0) == 1);
    k.atest = 1; CHECK(lop_key(&k, ps, 0) == 0); k.atest = 0;   // no library has both
    for (int i = 0; i < 3; i++) { MTLPixelFormat none[] = {MTLPixelFormatRGBA16Float, MTLPixelFormatRGBA8Unorm_sRGB, MTLPixelFormatRGBA8Snorm}; k.cfmt[0] = (uint32_t)none[i]; CHECK(lop_key(&k, ps, 0) == 0); }   // no bits to combine: COPY
    uint32_t bits[4] = {0};
    CHECK(unorm_chan_masks(MTLPixelFormatRGB10A2Unorm, bits) && bits[0] == 0x3ff && bits[3] == 3); CHECK(unorm_chan_masks(MTLPixelFormatB5G6R5Unorm, bits) && bits[0] == 0x1f && bits[1] == 0x3f && bits[2] == 0x1f);
    CHECK(unorm_chan_masks(MTLPixelFormatBGRA8Unorm, bits) && bits[2] == 0xff); CHECK(!unorm_chan_masks(MTLPixelFormatRGBA16Float, bits));
    k.cfmt[0] = MTLPixelFormatRG32Uint; k.chmask = 0; CHECK(lop_key(&k, ps, 0) == 0);   // no colour written: nothing to combine, no red line
    k.chmask = 0xf; S.color.logic_op = NVN_LOGIC_OP_COPY; CHECK(lop_key(&k, ps, 0) == 0);
    printf("logic operation replay: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
