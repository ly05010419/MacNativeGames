// #702: real pass setup, pipeline lookup and literal GPU plane readback, without game files.
#include "../nvn/nvn_mtl.m"
#include "../nvn/nvn_pso_ds.h"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)

static void keys(void) {
    // Expected tuples are literal API formats, independent of the normalizer.
    const uint32_t cases[][4] = {
        {0, 0, 0, 0}, {250, 0, 250, 0}, {252, 0, 252, 0}, {0, 253, 0, 253}, {0, 261, 0, 261},
        {260, 260, 260, 260}, {0, 260, 260, 260}, {260, 0, 260, 260},
        {250, 260, 250, 260}, {260, 253, 260, 253}, {70, 0, 70, 0}, {0, 0xffffffffu, 0, 0xffffffffu},
    };
    for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++) {
        pso_key raw = {.dfmt = cases[i][0], .sfmt = cases[i][1]}, k = raw;
        pso_key_norm(&k, 0);
        CHECK(k.dfmt == cases[i][2] && k.sfmt == cases[i][3], "live key resolves only a missing combined plane");
        pso_skey sk, parsed; char hex[sizeof sk * 2 + 1];
        pso_skey_from(&raw, nil, nil, 0, NULL, &sk); pso_skey_hex(&sk, hex);
        int rc = pso_skey_parse(hex, &parsed);
        CHECK(!rc, "historical prewarm row parses"); if (rc) continue;
        CHECK(parsed.k.dfmt == cases[i][2] && parsed.k.sfmt == cases[i][3], "prewarm and live formats agree");
        CHECK(pso_ds_valid(k.dfmt, k.sfmt) == (i < 8), "conflicting and non-renderable formats remain invalid");
        pso_key again = k; pso_key_norm(&again, 0);
        CHECK(again.dfmt == k.dfmt && again.sfmt == k.sfmt, "format normalization is idempotent");
    }
    CHECK(PSO_D16 == MTLPixelFormatDepth16Unorm && PSO_D32 == MTLPixelFormatDepth32Float && PSO_S8 == MTLPixelFormatStencil8
          && PSO_D32S8 == MTLPixelFormatDepth32Float_Stencil8 && PSO_X32S8 == MTLPixelFormatX32_Stencil8, "serialized constants match the Metal SDK");
}

static tex_t texture(int format) {
    static uint64_t serial = 980000;
    serial++;
    tex_t t = {.magic = MAGIC('T','E','X',0), .serial = serial, .off = (int64_t)serial << 20};
    t.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = format, .width = 8, .height = 8, .depth = 1, .levels = 1, .samples = 1};
    return t;
}
static void seed(const tex_t *t) {
    id<MTLTexture> base = tex_mtl(t);
    CHECK(base != nil, "NVN target allocates"); if (!base) exit(1);
    id<MTLCommandBuffer> cb = q_cb();   // finish the production zero-fill before poisoning the planes
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor new];
    if (t->d.format != NVN_FORMAT_STENCIL8) {
        p.depthAttachment.texture = base; p.depthAttachment.loadAction = MTLLoadActionClear;
        p.depthAttachment.storeAction = MTLStoreActionStore; p.depthAttachment.clearDepth = .75;
    }
    if (t->d.format == NVN_FORMAT_STENCIL8 || t->d.format == NVN_FORMAT_DEPTH24_STENCIL8 || t->d.format == NVN_FORMAT_DEPTH32F_STENCIL8) {
        p.stencilAttachment.texture = base; p.stencilAttachment.loadAction = MTLLoadActionClear;
        p.stencilAttachment.storeAction = MTLStoreActionStore; p.stencilAttachment.clearStencil = 0xa5;
    }
    [[cb renderCommandEncoderWithDescriptor:p] endEncoding]; [cb commit]; [cb waitUntilCompleted];
    CHECK(cb.status == MTLCommandBufferStatusCompleted, "initial plane values reach the GPU");
}
static NSData *read_plane(const tex_t *t, unsigned bpp, MTLBlitOption option) {
    id<MTLTexture> source = tex_mtl(t);
    CHECK(source != nil, "attachment exists for readback"); if (!source) exit(1);
    id<MTLBuffer> b = [g_dev newBufferWithLength:2048 options:MTLResourceStorageModeShared];
    CHECK(b != nil, "readback buffer allocates"); if (!b) exit(1);
    id<MTLCommandBuffer> cb = q_cb(); id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
    [e copyFromTexture:source sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(8,8,1)
        toBuffer:b destinationOffset:0 destinationBytesPerRow:256 destinationBytesPerImage:2048 options:option];
    [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
    CHECK(cb.status == MTLCommandBufferStatusCompleted, "literal plane readback completes");
    NSMutableData *result = [NSMutableData dataWithLength:64 * bpp];
    for (unsigned y = 0; y < 8; y++) memcpy((uint8_t *)result.mutableBytes + y * 8 * bpp, (uint8_t *)b.contents + y * 256, 8 * bpp);
    return result;
}
static NvProg *program(id<MTLLibrary> lib) {
    NvProg *p = [NvProg new];
    p->sh[0] = [NvShader new]; p->sh[1] = [NvShader new];
    strcpy(p->sh[0]->key, "ds-vs"); strcpy(p->sh[1]->key, "ds-fs"); p->sh[1]->stage = 1;
    p->sh[0]->fn = fn4_note([lib newFunctionWithName:@"v"], lib, @"v", nil);
    p->sh[1]->fn = fn4_note([lib newFunctionWithName:@"f"], lib, @"f", nil);
    p->sh[1]->rf.color_known = 1; p->sh[1]->rf.color_mask = 1;
    return p;
}

static void draws(NvProg *p) {
    uint32_t cache_before = g_pso.n;
    const struct { int format, mode, dw, sw, color; uint32_t df, sf; } cases[] = {
        {0, 0, 0, 0, 1, 0, 0},
        {NVN_FORMAT_DEPTH16, 0, 1, 0, 1, 250, 0}, {NVN_FORMAT_DEPTH32F, 0, 1, 0, 1, 252, 0},
        {NVN_FORMAT_STENCIL8, 0, 0, 1, 1, 0, 253},
        {NVN_FORMAT_DEPTH24_STENCIL8, 0, 1, 1, 1, 260, 260}, {NVN_FORMAT_DEPTH32F_STENCIL8, 0, 1, 1, 1, 260, 260},
        {NVN_FORMAT_DEPTH32F_STENCIL8, 3, 0, 1, 1, 260, 260}, {NVN_FORMAT_DEPTH32F_STENCIL8, 4, 1, 0, 1, 260, 260},
        {NVN_FORMAT_DEPTH32F_STENCIL8, 2, 0, 0, 1, 0, 0},
        {NVN_FORMAT_DEPTH32F, 0, 1, 0, 1, 252, 0}, {0, 0, 0, 0, 1, 0, 0}, {0, 0, 0, 0, 0, 0, 0},
    };
    id<MTLRenderPipelineState> seen[sizeof cases / sizeof *cases]; memset(seen, 0, sizeof seen);
    tex_t color = texture(NVN_FORMAT_RGBA8);
    for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++) {
        tex_t ds = texture(cases[i].format); if (cases[i].format) seed(&ds);
        state_reset(); memset(&R, 0, sizeof R);
        S.ds = (ds_st){.dt = cases[i].dw, .dw = cases[i].dw, .df = NVN_DEPTH_FUNC_ALWAYS, .st = cases[i].sw,
            .sf_f = NVN_STENCIL_FUNC_ALWAYS, .sf_b = NVN_STENCIL_FUNC_ALWAYS,
            .fail_f = NVN_STENCIL_OP_KEEP, .zfail_f = NVN_STENCIL_OP_KEEP, .zpass_f = NVN_STENCIL_OP_REPLACE,
            .fail_b = NVN_STENCIL_OP_KEEP, .zfail_b = NVN_STENCIL_OP_KEEP, .zpass_b = NVN_STENCIL_OP_REPLACE};
        S.st_vmask[0] = S.st_vmask[1] = S.st_wmask[0] = S.st_wmask[1] = 255;
        R.has_rt = R.has_sc = 1; R.sc = (p_scissor){0,0,8,8}; R.rt.n = cases[i].color;
        if (cases[i].color) { R.rt.color[0] = &color; R.clear_c[0] = 1; R.cc[0] = MTLClearColorMake(0,0,1,1); }
        if (cases[i].format) R.rt.depth = &ds;
        mtl_sub_begin(); id<MTL4RenderCommandEncoder> e = rp_begin_ex(cases[i].mode);
        CHECK(e != nil, "production pass opens");
        CHECK(g_pass.dfmt == cases[i].df && g_pass.sfmt == cases[i].sf, "physical formats match literal expected pipeline formats");
        if (cases[i].mode == 3) CHECK(!g_pass.hd && g_pass.hs, "stencil-only usage keeps depth disabled");
        if (cases[i].mode == 4) CHECK(g_pass.hd && !g_pass.hs, "depth-only usage keeps stencil disabled");
        uint64_t before = g_pb_legacy + g_pb_spec;
        id<MTLRenderPipelineState> ps = pso_get_(p, nil); seen[i] = ps;
        CHECK(ps != nil, "production pipeline lookup succeeds");
        for (unsigned j = 0; j < i; j++) if (cases[i].color == cases[j].color && cases[i].df == cases[j].df && cases[i].sf == cases[j].sf) {
            CHECK(ps == seen[j], "returning to an effective format pair reuses its pipeline");
            CHECK(g_pb_legacy + g_pb_spec == before, "format transition cache hit compiles nothing");
        }
        // NOLINTBEGIN(clang-analyzer-osx.cocoa.RetainCount): dss_get retains the state in g_dss through hm_put_id's CFBridgingRetain
        if (e && ps) {
            id<MTLDepthStencilState> ds_state = dss_get(g_pass.hd, g_pass.hs);
            enc_r_pso(e, ps);
            enc_r_dss(e, ds_state);
            enc_r_stencil_ref(e, 0x5a, 0x5a);
            enc_r_viewport(e, (MTLViewport){0,0,8,8,0,1}); enc_r_scissor(e, (MTLScissorRect){0,0,8,8});
            enc_r_cull(e, MTLCullModeNone); enc_r_draw(e, MTLPrimitiveTypeTriangle, 0, 3);
        }
        // NOLINTEND(clang-analyzer-osx.cocoa.RetainCount)
        mtl_sub_commit(); mtl_drain(); CHECK(!mtl_last_commit_error(), "production draw completes without Metal errors");
        if (cases[i].color) {
            NSData *bytes = read_plane(&color, 4, MTLBlitOptionNone); const uint8_t *v = bytes.bytes; int good = 1;
            for (unsigned n = 0; n < 64; n++) good &= v[4*n] == 255 && v[4*n+1] == 0 && v[4*n+2] == 0 && v[4*n+3] == 255;
            CHECK(good, "draw writes literal red pixels through the resolved pipeline");
        }
        int combined = cases[i].format == NVN_FORMAT_DEPTH24_STENCIL8 || cases[i].format == NVN_FORMAT_DEPTH32F_STENCIL8;
        if (cases[i].format && cases[i].format != NVN_FORMAT_STENCIL8) {
            unsigned bpp = cases[i].format == NVN_FORMAT_DEPTH16 ? 2 : 4;
            NSData *bytes = read_plane(&ds, bpp, combined ? MTLBlitOptionDepthFromDepthStencil : MTLBlitOptionNone); int good = 1;
            for (unsigned n = 0; n < 64; n++) {
                if (bpp == 2) good &= abs((int)((const uint16_t *)bytes.bytes)[n] - (cases[i].dw ? 16384 : 49151)) <= 1;
                else good &= ((const float *)bytes.bytes)[n] == (cases[i].dw ? .25f : .75f);
            }
            CHECK(good, "depth writes are correct; stencil-only and omitted passes preserve poisoned depth");
        }
        if (combined || cases[i].format == NVN_FORMAT_STENCIL8) {
            NSData *bytes = read_plane(&ds, 1, combined ? MTLBlitOptionStencilFromDepthStencil : MTLBlitOptionNone); int good = 1;
            for (unsigned n = 0; n < 64; n++) good &= ((const uint8_t *)bytes.bytes)[n] == (cases[i].sw ? 0x5a : 0xa5);
            CHECK(good, "stencil writes are correct; depth-only and omitted passes preserve poisoned stencil");
        }
        if (cases[i].format) mtl_tex_release(&ds);
    }
    CHECK(g_pso.n == cache_before + 6, "five format pairs and the colour-free pass have distinct cache entries");
    mtl_tex_release(&color);
}

static void invalid_descriptors(NvProg *p) {
    const uint32_t cases[][2] = {{0,260}, {260,0}, {250,260}, {260,253}, {70,0}, {0,70}, {0xffffffffu,0}};
    for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++) for (int compile = 0; compile <= 1; compile++) {
        pso_key k = {.dfmt = cases[i][0], .sfmt = cases[i][1], .samples = 1}; k.cfmt[0] = 70;
        NSError *error = nil;
        CHECK(!pso_build_ex(pso_desc(&k, p->sh[0], p->sh[1], nil, 0), &error, compile), "invalid descriptor is rejected before either Metal compiler path");
        CHECK(error.code == 1 && [error.domain isEqual:@"nxnative.NVN.Pipeline"], "invalid descriptor reports an explicit input error");
    }
}

static void prediction(id<MTLLibrary> lib) {
    NvProg *p = program(lib); static prog_t pg = {.serial = 980100}; serial_put(pg.serial, p);
    state_reset(); S.prog = &pg; memset(&g_pass, 0, sizeof g_pass); g_pass.cfmt[0] = 70; g_pass.dfmt = g_pass.sfmt = 260; g_pass.samples = 1;
    uint8_t commands[1024] = {0}; p_draw draw = {.prim = NVN_DRAW_PRIMITIVE_TRIANGLES, .count = 3};
    unsigned size = (unsigned)((8 + sizeof draw + 7) & ~7u); op_hdr h = {OP_DRAW, size}, end = {OP_END, 8};
    memcpy(commands, &h, 8); memcpy(commands + 8, &draw, sizeof draw); memcpy(commands + size, &end, 8);
    g_exec_next = commands; uint64_t before = g_pp_kicked;
    pso_predict_rest_(); g_exec_next = NULL;
    CHECK(g_pp_kicked - before == 2, "prediction compiles only the paired and attachment-free variants");
    pso_key pending[PSO_PEND_MAX]; int n = 0;
    pthread_mutex_lock(&g_pp_mu);
    for (int i = 0; i < PSO_PEND_MAX; i++) if (g_pp[i].used) pending[n++] = g_pp[i].k;
    pthread_mutex_unlock(&g_pp_mu);
    CHECK(n == 2, "single-plane alternatives deduplicate before background compilation");
    for (int i = 0; i < n; i++) {
        CHECK((!pending[i].dfmt && !pending[i].sfmt) || (pending[i].dfmt == 260 && pending[i].sfmt == 260), "every speculative key has a legal effective format pair");
        CHECK(pso_pend_take(&pending[i]) != nil, "predicted pipeline compiles and can be claimed");
    }
}

int main(int argc, char **argv) { @autoreleasepool {
    keys();
    if (argc == 2 && !strcmp(argv[1], "--keys-only")) { printf("depth/stencil keys: %d checks, %d failed\n", checks, failed); return failed != 0; }
    setenv("DM_PSO_ARCHIVE", "off", 1); setenv("DM_PSO_MANIFEST", "off", 1); setenv("DM_PSO_USER", "/dev/null", 1);
    setenv("DM_PSO_PREBUILD", "0", 1); setenv("DM_PSO_HYBRID", "0", 1); setenv("DM_NVN_PREDICT", "0", 1);
    mtl_init(); NSError *error = nil;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "vertex float4 v(uint n[[vertex_id]]) { float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)}; return float4(p[n],.25,1); }\n"
        "fragment float4 f() { return float4(1,0,0,1); }\n" options:nil error:&error];
    CHECK(lib != nil, "synthetic shaders compile"); if (!lib) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 1; }
    NvProg *p = program(lib); draws(p); invalid_descriptors(p); prediction(lib);
    const char *mode = getenv("DM_PSO_MTL4");
    CHECK((g_comp4 != nil) == (!mode || strcmp(mode, "0") != 0), "requested pipeline compiler path is exercised");
    printf("depth/stencil pipelines: %d checks, %d failed (%s)\n", checks, failed, g_comp4 ? "Metal 4" : "MTLDevice");
    return failed != 0;
} }
