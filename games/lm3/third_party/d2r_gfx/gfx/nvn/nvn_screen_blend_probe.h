/* Included by nvn_mtl.m after probe_buf: exercise the production state replay,
   pipeline key and descriptor on actual Metal 4 draws. The CPU oracle uses the
   three NV coverage regions rather than the fixed-function factor reduction. */
#pragma once
#include <math.h>

static double screen_probe_encode(double x, int srgb) {
    x = fmax(0.0, fmin(1.0, x));
    return srgb ? (x <= 0.0031308 ? 12.92 * x : 1.055 * pow(x, 1.0 / 2.4) - 0.055) : x;
}
static double screen_probe_decode(double x, int srgb) {
    return srgb ? (x <= 0.04045 ? x / 12.92 : pow((x + 0.055) / 1.055, 2.4)) : x;
}
static int screen_probe_byte(double x, int srgb) {
    return (int)lround(screen_probe_encode(x, srgb) * 255.0);
}
static void screen_probe_expected(const float source[4], const double clear[4], int srgb,
                                  int mode, int npm, int enabled, unsigned mask, uint8_t expected[4]) {
    double dest[4]; int stored[4];
    for (int c = 0; c < 4; c++) {
        stored[c] = screen_probe_byte(clear[c], srgb && c < 3);
        dest[c] = screen_probe_decode(stored[c] / 255.0, srgb && c < 3);
    }
    /* NVN clamps normalized-target fragment inputs before blending, including
       before the advanced operation converts a straight source to premult. */
    double src[4]; for (int c = 0; c < 4; c++) src[c] = fmax(0.0, fmin(1.0, source[c]));
    double as = src[3], ad = dest[3];
    double p0 = as * ad, p1 = as * (1.0 - ad), p2 = ad * (1.0 - as);
    for (int c = 0; c < 4; c++) {
        double value;
        if (!enabled) value = src[c];
        else if (c == 3) value = p0 + p1 + p2;
        else if (!mode) value = src[c] + dest[c] * (1.0 - as);
        else {
            double s = npm ? src[c] : as == 0.0 ? 0.0 : src[c] / as;
            double d = ad == 0.0 ? 0.0 : dest[c] / ad;
            double overlap = 1.0 - (1.0 - s) * (1.0 - d);
            value = overlap * p0 + s * p1 + d * p2;
        }
        expected[c] = mask & (1u << c) ? screen_probe_byte(value, srgb && c < 3) : stored[c];
    }
}
static NvShader *screen_probe_float_shader(const float rgba[4], const char *key) {
    NSString *source = [NSString stringWithFormat:
        @"#include <metal_stdlib>\nusing namespace metal;\n"
         "struct main0_out\n{\n    float4 color0 [[color(0)]];\n};\n"
         "fragment main0_out main0()\n{\n    main0_out out = {};\n"
         "    out.color0 = float4(%.9g, %.9g, %.9g, %.9g);\n    return out;\n}\n",
        rgba[0], rgba[1], rgba[2], rgba[3]];
    NSError *error = nil; id<MTLLibrary> library = totk_msl_library(source, &error);
    if (!library) MDIE("screen probe float shader: %s", error.localizedDescription.UTF8String);
    NvShader *fs = [NvShader new]; fs->fn = [library newFunctionWithName:@"main0"];
    fs->src = source; fs->io = @{}; fs->stage = 1; fs->rf.color_mask = 1;
    snprintf(fs->key, sizeof fs->key, "%s", key); return fs;
}
static void screen_probe_bind(int kind, const void *value, size_t size) {
    p_bind_state recorded = {.kind = kind};
    memcpy(recorded.bytes, value, size);
    if (!state_op(OP_BIND_STATE, &recorded)) MDIE("screen probe bind state was not handled");
}

int mtl_probe_screen_blend(void) {
    mtl_init(); int checked = 0;
    const char *negative_env = getenv("TOTK_PROBE_NEGATIVE");
    int negative = negative_env ? atoi(negative_env) : 0;
    const NSUInteger W = 16, H = 16;
    const float base_sources[][4] = {
        {0.8f, 0.2f, 0.6f, 0.0f}, /* Straight alpha-zero; PM case converts RGB to zero. */
        {0.8f, 0.2f, 0.6f, 0.5f},
        {0.5f, 0.25f, 0.75f, 1.0f},
        {0.25f, 0.5f, 0.125f, 0.5f},
        {0.5f, 0.25f, 0.8f, 0.125f},
        {-0.25f, 1.5f, 0.5f, 0.5f},
        {0.8f, 0.2f, 0.6f, 1.5f},
        {0.8f, 0.2f, 0.6f, -0.5f},
    };
    const double clear[4] = {0.12, 0.42, 0.24, 0.6};
    NSMutableString *source = [NSMutableString stringWithString:
        @"#include <metal_stdlib>\nusing namespace metal;\nstruct V { float4 p [[position]]; };\n"
         "vertex V screen_vs(uint id [[vertex_id]]) { float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};return {float4(p[id],0,1)}; }\n"
         "fragment uint4 screen_uint() { return uint4(17,63,129,211); }\n"];
    NSError *error = nil; id<MTLLibrary> library = totk_msl_library(source, &error);
    if (!library) MDIE("screen probe shaders: %s", error.localizedDescription.UTF8String);
    NvShader *vs = [NvShader new]; vs->fn = [library newFunctionWithName:@"screen_vs"]; vs->io = @{}; vs->stage = 0;
    strcpy(vs->key, "probe-screen-vs");
    id<MTLBuffer> readback = probe_buf(W * H * 4);

    for (int srgb = 0; srgb < 2; srgb++) {
        MTLPixelFormat format = srgb ? MTLPixelFormatRGBA8Unorm_sRGB : MTLPixelFormatRGBA8Unorm;
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:W height:H mipmapped:NO];
        td.storageMode = MTLStorageModePrivate; td.usage = MTLTextureUsageRenderTarget;
        id<MTLTexture> texture = [g_dev newTextureWithDescriptor:td];
        if (!texture) MDIE("screen probe render target"); res_add(texture);
        for (int npm = 0; npm < 2; npm++) for (unsigned input = 0; input < (npm ? 8u : 5u); input++) {
            float rgba[4]; memcpy(rgba, base_sources[input], sizeof rgba);
            if (!npm) for (int c = 0; c < 3; c++) rgba[c] *= rgba[3];
            char shader_key[48]; snprintf(shader_key, sizeof shader_key, "probe-screen-%d-%u", npm, input);
            NvShader *fs = screen_probe_float_shader(rgba, shader_key);
            state_reset(); pso_key ordinary_key = {0}, screen_key = {0};
            /* NONE -> SCREEN -> NONE -> SCREEN; disabled and masked variants.
               Ordinary factors remain SrcOver while the advanced mode changes. */
            for (int scenario = 0; scenario < 8; scenario++) {
                int mode = scenario != 0 && scenario != 2;
                int enabled = scenario != 4;
                unsigned mask = scenario == 5 ? 5 : scenario == 6 ? 0 : 15;
                blend_st b = {.target = 0, .src_rgb = NVN_BLEND_FUNC_ONE, .dst_rgb = NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA,
                    .src_a = NVN_BLEND_FUNC_ONE, .dst_a = NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA,
                    .eq_rgb = NVN_BLEND_EQUATION_ADD, .eq_a = NVN_BLEND_EQUATION_ADD,
                    .adv = mode ? NVN_BLEND_ADVANCED_MODE_SCREEN : NVN_BLEND_ADVANCED_MODE_NONE, .adv_npm = npm};
                if (!enabled || !mask) { b.adv_npm = 1; b.adv_ovl = 2; b.adv_nnd = 1; }
                /* Advanced blending must replace the ordinary equation and
                   factors, even when those factors would erase all colors. */
                if (scenario == 7) {
                    b.src_rgb = b.dst_rgb = b.src_a = b.dst_a = NVN_BLEND_FUNC_ZERO;
                    b.eq_rgb = b.eq_a = NVN_BLEND_EQUATION_SUB;
                }
                if (scenario == 1 && input == 0 && srgb == 0 && npm == 0) {
                    if (negative == 47) b.adv_ovl = NVN_BLEND_ADVANCED_OVERLAP_CONJOINT;
                    if (negative == 48) b.adv_nnd = 1;
                    if (negative == 49) b.adv = NVN_BLEND_ADVANCED_MODE_MULTIPLY;
                }
                p_bind_state snapshot = {.kind = 0}; memcpy(snapshot.bytes, &b, sizeof b);
                blend_st original = b; b.adv = 0; b.src_rgb = NVN_BLEND_FUNC_ZERO;
                if (!state_op(OP_BIND_STATE, &snapshot) || memcmp(&S.blend[0], &original, sizeof original))
                    MDIE("screen probe recorded blend state did not retain its value"); checked++;
                color_st color = {.blend_en = enabled, .logic_op = NVN_LOGIC_OP_COPY, .alpha_func = NVN_ALPHA_FUNC_ALWAYS};
                chmask_st channels = {.mask = mask};
                screen_probe_bind(1, &color, sizeof color); screen_probe_bind(2, &channels, sizeof channels);
                uint32_t formats[8] = {(uint32_t)format}; pso_key key;
                pso_key_fill(&key, vs, fs, formats, 0, 0, 1, 0);
                if (scenario == 0) ordinary_key = key;
                if (scenario == 1) { screen_key = key; if (!memcmp(&ordinary_key, &key, sizeof key)) MDIE("screen probe pipeline cache key aliases ordinary state"); checked++; }
                if (scenario == 2 && memcmp(&ordinary_key, &key, sizeof key)) MDIE("screen probe ordinary state did not restore");
                if (scenario == 3 && memcmp(&screen_key, &key, sizeof key)) MDIE("screen probe advanced state did not restore");
                if (scenario == 6) {
                    pso_skey disk, parsed; char hex[sizeof disk * 2 + 1];
                    pso_skey_from(&key, vs, fs, 0, NULL, &disk); pso_skey_hex(&disk, hex);
                    if (pso_skey_parse(hex, &parsed) || memcmp(&disk, &parsed, sizeof disk) ||
                        memcmp(&parsed.k.blend[0], &original, sizeof original)) MDIE("screen probe serialized key lost advanced flags"); checked++;
                }
                MTLRenderPipelineDescriptor *pd = pso_desc(&key, vs, fs, nil, 0);
                if (negative == 45 && mode && enabled && mask)
                    pd.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
                if (negative == 46 && npm && mode && enabled && mask) pd.fragmentFunction = fs->fn;
                if ((!enabled || !mask) && pd.colorAttachments[0].blendingEnabled) MDIE("screen probe inactive blend enabled"); checked++;
                id<MTLRenderPipelineState> ps = [g_dev newRenderPipelineStateWithDescriptor:pd error:&error];
                if (!ps) MDIE("screen probe pipeline: %s", error.localizedDescription.UTF8String);
                mtl_sub_begin();
                MTL4RenderPassDescriptor *rp = [MTL4RenderPassDescriptor new];
                rp.colorAttachments[0].texture = texture; rp.colorAttachments[0].loadAction = MTLLoadActionClear;
                rp.colorAttachments[0].storeAction = MTLStoreActionStore;
                rp.colorAttachments[0].clearColor = MTLClearColorMake(clear[0], clear[1], clear[2], clear[3]);
                rp.renderTargetWidth = W; rp.renderTargetHeight = H;
                id<MTL4RenderCommandEncoder> en = enc_r_begin(rp); enc_r_pso(en, ps); enc_r_cull(en, MTLCullModeNone);
                enc_r_viewport(en, (MTLViewport){0,0,W,H,0,1}); enc_r_scissor(en, (MTLScissorRect){0,0,W,H});
                enc_r_draw(en, MTLPrimitiveTypeTriangle, 0, 3); enc_r_end(en);
                enc_b_copy_texture_buffer(blit(), texture, 0, 0, MTLOriginMake(0,0,0), MTLSizeMake(W,H,1), readback, 0, W*4, W*H*4);
                mtl_sub_commit(); mtl_finish();
                uint8_t expected[4]; screen_probe_expected(rgba, clear, srgb, mode, npm, enabled, mask, expected);
                for (NSUInteger i = 0; i < W*H*4; i++) {
                    unsigned got = ((const uint8_t *)readback.contents)[i], want = expected[i % 4];
                    if (abs((int)got - (int)want) > 2) MDIE("screen probe pixel mismatch sRGB%d npm%d input%u scenario%d byte%lu: %u != %u", srgb, npm, input, scenario, (unsigned long)i, got, want);
                    checked++;
                }
            }
        }
        defer_obj_release(texture);
    }
    /* A SPIRV-Cross-shaped shader with two outputs and two returns. Only target
       zero uses straight SCREEN; target one keeps ordinary blending. The guest
       depth export must also be stripped when no depth attachment is present. */
    NSString *multi_source =
        @"#include <metal_stdlib>\nusing namespace metal;\n"
         "struct main0_out\n{\n    float4 color0 [[color(0)]];\n    float4 color1 [[color(1)]];\n    float gl_FragDepth [[depth(any)]];\n};\n"
         "fragment main0_out main0(float4 p [[position]])\n{\n    main0_out out = {};\n"
         "    out.color0 = float4(0.8,0.2,0.6,0.5);\n    out.color1 = float4(0.4,0.15,0.2,0.25);\n"
         "    out.gl_FragDepth = 0.125;\n    if (p.x < 8.0)\n    {\n        return out;\n    }\n"
         "    out.color0 = float4(0.3,0.6,0.1,0.75);\n    out.color1 = float4(0.1,0.2,0.35,0.5);\n    return out;\n}\n";
    id<MTLLibrary> multi_library = totk_msl_library(multi_source, &error);
    if (!multi_library) MDIE("screen probe multiple-output shader: %s", error.localizedDescription.UTF8String);
    NvShader *multi_fs = [NvShader new]; multi_fs->fn = [multi_library newFunctionWithName:@"main0"];
    multi_fs->src = multi_source; multi_fs->io = @{}; multi_fs->stage = 1; multi_fs->rf.color_mask = 3; multi_fs->writes_depth = 1;
    strcpy(multi_fs->key, "probe-screen-multiple-returns");
    state_reset(); S.color.blend_en = 3; S.chmask.mask = 255;
    for (int target = 0; target < 2; target++) {
        S.blend[target].src_rgb = S.blend[target].src_a = NVN_BLEND_FUNC_ONE;
        S.blend[target].dst_rgb = S.blend[target].dst_a = NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA;
    }
    S.blend[0].adv = NVN_BLEND_ADVANCED_MODE_SCREEN; S.blend[0].adv_npm = 1;
    uint32_t multi_formats[8] = {MTLPixelFormatRGBA8Unorm, MTLPixelFormatRGBA8Unorm}; pso_key multi_key;
    pso_key_fill(&multi_key, vs, multi_fs, multi_formats, 0, 0, 1, 0);
    MTLRenderPipelineDescriptor *multi_pd = pso_desc(&multi_key, vs, multi_fs, nil, 0);
    if (multi_pd.fragmentFunction == multi_fs->fn) MDIE("screen probe nodepth straight-source variant missing"); checked++;
    id<MTLRenderPipelineState> multi_ps = [g_dev newRenderPipelineStateWithDescriptor:multi_pd error:&error];
    if (!multi_ps) MDIE("screen probe multiple-output pipeline: %s", error.localizedDescription.UTF8String);
    MTLTextureDescriptor *multi_td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:W height:H mipmapped:NO];
    multi_td.storageMode = MTLStorageModePrivate; multi_td.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> multi0 = [g_dev newTextureWithDescriptor:multi_td], multi1 = [g_dev newTextureWithDescriptor:multi_td];
    if (!multi0 || !multi1) MDIE("screen probe multiple render targets"); res_add(multi0); res_add(multi1);
    id<MTLBuffer> multi_read = probe_buf(W*H*4);
    mtl_sub_begin(); MTL4RenderPassDescriptor *multi_rp = [MTL4RenderPassDescriptor new];
    for (int target = 0; target < 2; target++) {
        multi_rp.colorAttachments[target].texture = target ? multi1 : multi0;
        multi_rp.colorAttachments[target].loadAction = MTLLoadActionClear; multi_rp.colorAttachments[target].storeAction = MTLStoreActionStore;
        multi_rp.colorAttachments[target].clearColor = MTLClearColorMake(clear[0], clear[1], clear[2], clear[3]);
    }
    multi_rp.renderTargetWidth = W; multi_rp.renderTargetHeight = H;
    id<MTL4RenderCommandEncoder> multi_en = enc_r_begin(multi_rp); enc_r_pso(multi_en, multi_ps); enc_r_cull(multi_en, MTLCullModeNone);
    enc_r_viewport(multi_en, (MTLViewport){0,0,W,H,0,1}); enc_r_scissor(multi_en, (MTLScissorRect){0,0,W,H});
    enc_r_draw(multi_en, MTLPrimitiveTypeTriangle, 0, 3); enc_r_end(multi_en);
    enc_b_copy_texture_buffer(blit(), multi0, 0, 0, MTLOriginMake(0,0,0), MTLSizeMake(W,H,1), readback, 0, W*4, W*H*4);
    enc_b_copy_texture_buffer(blit(), multi1, 0, 0, MTLOriginMake(0,0,0), MTLSizeMake(W,H,1), multi_read, 0, W*4, W*H*4);
    mtl_sub_commit(); mtl_finish();
    const float multi_rgba[2][2][4] = {{{.8f,.2f,.6f,.5f},{.3f,.6f,.1f,.75f}},{{.4f,.15f,.2f,.25f},{.1f,.2f,.35f,.5f}}};
    for (int target = 0; target < 2; target++) for (NSUInteger i = 0; i < W*H*4; i++) {
        unsigned side = (i / 4) % W >= W / 2; uint8_t expected[4];
        screen_probe_expected(multi_rgba[target][side], clear, 0, target == 0, target == 0, 1, 15, expected);
        const uint8_t *actual = target ? multi_read.contents : readback.contents;
        if (abs((int)actual[i] - (int)expected[i % 4]) > 2) MDIE("screen probe selected output/early return mismatch target%d byte%lu", target, (unsigned long)i);
        checked++;
    }
    defer_obj_release(multi0); defer_obj_release(multi1); defer_obj_release(multi_read);
    /* Exercise pso_get itself, including its real cache lookup/insertion. A raw
       key comparison alone would miss a broken lookup or a reused pipeline. */
    state_reset(); S.color.blend_en = 1; S.chmask.mask = 15;
    S.blend[0].src_rgb = S.blend[0].src_a = NVN_BLEND_FUNC_ONE;
    S.blend[0].dst_rgb = S.blend[0].dst_a = NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA;
    const float cache_rgba[4] = {.4f,.1f,.3f,.5f};
    NvShader *cache_fs = screen_probe_float_shader(cache_rgba, "probe-screen-cache");
    NvProg *program = [NvProg new]; program->sh[0] = vs; program->sh[1] = cache_fs;
    typeof(g_pass) saved_pass = g_pass; const uint8_t *saved_next = g_exec_next, *saved_lmap = g_pso_lmap;
    id<MTLFunction> saved_fsfn = g_pso_fsfn; int saved_role = g_mfx.role;
    memset(&g_pass, 0, sizeof g_pass); g_pass.cfmt[0] = MTLPixelFormatRGBA8Unorm; g_pass.samples = 1;
    g_pass.tw = W; g_pass.th = H; g_pass.serial = g_sub_serial;
    g_exec_next = NULL; g_pso_lmap = NULL; g_pso_fsfn = nil; g_mfx.role = 0;
    id<MTLRenderPipelineState> cached_screen = nil, cached_straight = nil, cached_ordinary = nil;
    for (int step = 0; step < 5; step++) {
        S.blend[0].adv = step == 1 ? NVN_BLEND_ADVANCED_MODE_NONE : NVN_BLEND_ADVANCED_MODE_SCREEN;
        S.blend[0].adv_npm = step == 2 || step == 4;
        uint64_t before = g_bp_pso_n;
        id<MTLRenderPipelineState> ps = pso_get(program, nil);
        if (!ps) MDIE("screen probe real cache pipeline missing"); checked++;
        uint64_t compiled = g_bp_pso_n;
        if (pso_get(program, nil) != ps || g_bp_pso_n != compiled) MDIE("screen probe repeated real cache lookup missed"); checked++;
        if (step == 0) cached_screen = ps;
        else if (step == 1) { cached_ordinary = ps; if (ps == cached_screen) MDIE("screen probe real cache aliases ordinary pipeline"); checked++; }
        else if (step == 2) { cached_straight = ps; if (ps == cached_screen || ps == cached_ordinary) MDIE("screen probe real cache aliases straight-source variant"); checked++; }
        else if (step == 3) { if (ps != cached_screen || compiled != before) MDIE("screen probe real cache did not reuse PM SCREEN pipeline"); checked++; }
        else { if (ps != cached_straight || compiled != before) MDIE("screen probe real cache did not reuse straight SCREEN pipeline"); checked++; }
    }
    g_pass = saved_pass; g_exec_next = saved_next; g_pso_lmap = saved_lmap; g_pso_fsfn = saved_fsfn; g_mfx.role = saved_role;
    /* Integer targets ignore blend state, even if the advanced combination is
       unsupported for floating-point colors. Read back shader integer bytes. */
    state_reset(); S.color.blend_en = 1; S.chmask.mask = 15;
    S.blend[0].adv = NVN_BLEND_ADVANCED_MODE_SCREEN; S.blend[0].adv_npm = 1; S.blend[0].adv_ovl = 2; S.blend[0].adv_nnd = 1;
    NvShader *fs = [NvShader new]; fs->fn = [library newFunctionWithName:@"screen_uint"]; fs->io = @{}; fs->stage = 1; strcpy(fs->key, "probe-screen-uint");
    uint32_t formats[8] = {MTLPixelFormatRGBA8Uint}; pso_key key; pso_key_fill(&key, vs, fs, formats, 0, 0, 1, 0);
    MTLRenderPipelineDescriptor *pd = pso_desc(&key, vs, fs, nil, 0);
    if (pd.colorAttachments[0].blendingEnabled) MDIE("screen probe integer attachment blending enabled"); checked++;
    id<MTLRenderPipelineState> ps = [g_dev newRenderPipelineStateWithDescriptor:pd error:&error];
    if (!ps) MDIE("screen probe integer pipeline: %s", error.localizedDescription.UTF8String);
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Uint width:W height:H mipmapped:NO];
    td.storageMode = MTLStorageModePrivate; td.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> texture = [g_dev newTextureWithDescriptor:td]; if (!texture) MDIE("screen probe integer texture"); res_add(texture);
    mtl_sub_begin(); MTL4RenderPassDescriptor *rp = [MTL4RenderPassDescriptor new];
    rp.colorAttachments[0].texture = texture; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.renderTargetWidth = W; rp.renderTargetHeight = H;
    id<MTL4RenderCommandEncoder> en = enc_r_begin(rp); enc_r_pso(en, ps); enc_r_cull(en, MTLCullModeNone);
    enc_r_viewport(en, (MTLViewport){0,0,W,H,0,1}); enc_r_scissor(en, (MTLScissorRect){0,0,W,H}); enc_r_draw(en, MTLPrimitiveTypeTriangle, 0, 3); enc_r_end(en);
    enc_b_copy_texture_buffer(blit(), texture, 0, 0, MTLOriginMake(0,0,0), MTLSizeMake(W,H,1), readback, 0, W*4, W*H*4); mtl_sub_commit(); mtl_finish();
    for (NSUInteger i = 0; i < W*H*4; i++) {
        if (((const uint8_t *)readback.contents)[i] != (uint8_t[]){17,63,129,211}[i % 4]) MDIE("screen probe integer pixel mismatch"); checked++;
    }
    defer_obj_release(texture); defer_obj_release(readback); state_reset(); return checked;
}
