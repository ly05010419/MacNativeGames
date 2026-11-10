/* probe-gpu-fout, second part: a pipeline asking for blending on an integer colour output (Metal aborts the process on
   that, Metal debug layer or not), through the real pso_key_fill and pso_desc. Every integer format pfmt_class names:
   blending off, the write mask kept, the pipeline made. Three other formats: blending kept. Two outputs in one pipeline,
   the integer one first and last, both asking for blending with their own masks: the choice is per attachment, and a real
   draw shows the integer output's masked bits next to the other output's blended and masked colour. Included by nvn_mtl.m
   (its statics), run by mtl_probe_fragment_output_types. */
static void ib_state(uint32_t enabled, uint32_t mask, int src, int dst) {   // the game's blend state, as pso_key_fill reads it
    S.nattr = 0; memset(S.attr, 0, sizeof S.attr); S.color.blend_en = enabled; S.chmask.mask = mask;
    for (int i = 0; i < 2; i++) S.blend[i] = (blend_st){.src_rgb = (uint64_t)src, .dst_rgb = (uint64_t)dst, .src_a = (uint64_t)src, .dst_a = (uint64_t)dst, .eq_rgb = NVN_BLEND_EQUATION_ADD, .eq_a = NVN_BLEND_EQUATION_ADD};
}
static NvShader *ib_shader(NSString *src, const char *key) {
    NSError *error = nil; id<MTLLibrary> lib = [g_dev newLibraryWithSource:src options:nil error:&error];
    if (!lib) MDIE("integer blend probe shader %s: %s", key, error.localizedDescription.UTF8String);
    NvShader *fs = [NvShader new]; fs->fn = [lib newFunctionWithName:@"main0"]; fs->src = src; fs->io = @{}; fs->stage = 1;
    snprintf(fs->key, sizeof fs->key, "%s", key);
    return fs;
}
static id<MTLRenderPipelineState> ib_pipeline(MTLRenderPipelineDescriptor *pd, const char *what) {
    NSError *error = nil; id<MTLRenderPipelineState> ps = [g_dev newRenderPipelineStateWithDescriptor:pd error:&error];
    if (!ps) MDIE("integer blend probe pipeline %s: %s", what, error.localizedDescription.UTF8String);
    return ps;
}
static void ib_expect(MTLRenderPipelineColorAttachmentDescriptor *c, BOOL blending, MTLColorWriteMask mask, const char *what) {
    if (c.blendingEnabled != blending) MDIE("integer blend probe %s: blending %s", what, c.blendingEnabled ? "on" : "off");
    if (c.writeMask != mask) MDIE("integer blend probe %s: write mask %lu, expected %lu", what, (unsigned long)c.writeMask, (unsigned long)mask);
}
static int ib_formats(NvShader *vs) {
    static const MTLPixelFormat integer[] = {
        MTLPixelFormatR8Uint, MTLPixelFormatRG8Uint, MTLPixelFormatRGBA8Uint, MTLPixelFormatR16Uint, MTLPixelFormatRG16Uint, MTLPixelFormatRGBA16Uint,
        MTLPixelFormatR32Uint, MTLPixelFormatRG32Uint, MTLPixelFormatRGBA32Uint, MTLPixelFormatRGB10A2Uint,
        MTLPixelFormatR8Sint, MTLPixelFormatRG8Sint, MTLPixelFormatRGBA8Sint, MTLPixelFormatR16Sint, MTLPixelFormatRG16Sint, MTLPixelFormatRGBA16Sint,
        MTLPixelFormatR32Sint, MTLPixelFormatRG32Sint, MTLPixelFormatRGBA32Sint};
    static const MTLPixelFormat other[] = {MTLPixelFormatRGBA8Unorm, MTLPixelFormatRGBA16Float, MTLPixelFormatR32Float};
    enum { N_INT = sizeof integer / sizeof *integer, N_ALL = N_INT + sizeof other / sizeof *other };
    NvShader *fs = ib_shader(@"#include <metal_stdlib>\nusing namespace metal;\n\nstruct main0_out\n{\n    float4 out_var_SV_Target0 [[color(0)]];\n};\n\n"
        "fragment main0_out main0()\n{\n    main0_out out = {};\n    out.out_var_SV_Target0 = float4(1.0);\n    return out;\n}\n", "probe-integer-blend-one");
    const MTLColorWriteMask mask = MTLColorWriteMaskRed | MTLColorWriteMaskBlue | MTLColorWriteMaskAlpha;
    int checked = 0;
    for (int i = 0; i < N_ALL; i++) {
        MTLPixelFormat f = i < N_INT ? integer[i] : other[i - N_INT]; char what[32]; snprintf(what, sizeof what, "format %lu", (unsigned long)f);
        if ((pfmt_class(f) == 'f') != (i >= N_INT)) MDIE("integer blend probe %s: class %c", what, pfmt_class(f));
        ib_state(1, 0xd, NVN_BLEND_FUNC_SRC_ALPHA, NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA);
        uint32_t formats[8] = {(uint32_t)f}; pso_key key; pso_key_fill(&key, vs, fs, formats, 0, 0, 1, 0);
        if (!(key.blend_en & 1)) MDIE("integer blend probe %s: the key lost the blend request", what);
        MTLRenderPipelineDescriptor *pd = pso_desc(&key, vs, fs, nil, 0);
        ib_expect(pd.colorAttachments[0], i >= N_INT, mask, what);
        ib_pipeline(pd, what); checked += 2;
    }
    return checked;
}
/* Output `whole` is RGBA8_UI writing (7, 200, 3, 1) under the mask red and blue; the other is RGBA8_UNORM cleared to 0.25,
   adding 0.25 under the mask green and alpha. */
static int ib_two_outputs(NvShader *vs, int whole) {
    NSString *bits = @"as_type<float4>(uint4(7u, 200u, 3u, 1u))", *quarter = @"float4(0.25)";
    NSString *src = [NSString stringWithFormat:@"#include <metal_stdlib>\nusing namespace metal;\n\nstruct main0_out\n{\n    float4 out_var_SV_Target0 [[color(0)]];\n    float4 out_var_SV_Target1 [[color(1)]];\n};\n\n"
        "fragment main0_out main0()\n{\n    main0_out out = {};\n    out.out_var_SV_Target0 = %@;\n    out.out_var_SV_Target1 = %@;\n    return out;\n}\n", whole ? quarter : bits, whole ? bits : quarter];
    char key_name[40]; snprintf(key_name, sizeof key_name, "probe-integer-blend-two-%d", whole);
    NvShader *fs = ib_shader(src, key_name);
    const int other = !whole; const MTLPixelFormat target[2] = {whole ? MTLPixelFormatRGBA8Unorm : MTLPixelFormatRGBA8Uint, whole ? MTLPixelFormatRGBA8Uint : MTLPixelFormatRGBA8Unorm};
    id<MTLTexture> texture[2]; id<MTLBuffer> rb[2]; op_hdr end = {OP_END, 8};
    for (int i = 0; i < 2; i++) {
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:target[i] width:16 height:16 mipmapped:NO];
        td.storageMode = MTLStorageModePrivate; td.usage = MTLTextureUsageRenderTarget;
        texture[i] = [g_dev newTextureWithDescriptor:td]; res_add(texture[i]); rb[i] = probe_buf(1024);
    }
    mtl_sub_begin(); mtl_exec((const uint8_t *)&end);
    ib_state(3, (whole ? 0x5u << 4 : 0x5u) | (other ? 0xau << 4 : 0xau), NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ONE);
    uint32_t formats[8] = {(uint32_t)target[0], (uint32_t)target[1]}; pso_key key; pso_key_fill(&key, vs, fs, formats, 0, 0, 1, 0);
    MTLRenderPipelineDescriptor *pd = pso_desc(&key, vs, fs, nil, 0);
    ib_expect(pd.colorAttachments[whole], NO, MTLColorWriteMaskRed | MTLColorWriteMaskBlue, "the integer output of two");
    ib_expect(pd.colorAttachments[other], YES, MTLColorWriteMaskGreen | MTLColorWriteMaskAlpha, "the other output of two");
    id<MTLRenderPipelineState> ps = ib_pipeline(pd, "two outputs");
    MTL4RenderPassDescriptor *rp = [MTL4RenderPassDescriptor new];
    for (int i = 0; i < 2; i++) { rp.colorAttachments[i].texture = texture[i]; rp.colorAttachments[i].loadAction = MTLLoadActionClear; rp.colorAttachments[i].storeAction = MTLStoreActionStore; }
    rp.colorAttachments[whole].clearColor = MTLClearColorMake(0, 0, 0, 0); rp.colorAttachments[other].clearColor = MTLClearColorMake(0.25, 0.25, 0.25, 0.25);
    rp.renderTargetWidth = rp.renderTargetHeight = 16; id<MTL4RenderCommandEncoder> en = enc_r_begin(rp); enc_r_pso(en, ps); enc_r_cull(en, MTLCullModeNone);
    enc_r_viewport(en, (MTLViewport){0, 0, 16, 16, 0, 1}); enc_r_scissor(en, (MTLScissorRect){0, 0, 16, 16});
    enc_r_draw(en, MTLPrimitiveTypeTriangle, 0, 3); enc_r_end(en);
    for (int i = 0; i < 2; i++) enc_b_copy_texture_buffer(blit(), texture[i], 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(16, 16, 1), rb[i], 0, 64, 1024);
    mtl_sub_commit(); mtl_finish();
    const uint8_t want[2][4] = {{7, 0, 3, 0}, {64, 128, 64, 128}}; int checked = 2;
    for (int i = 0; i < 2; i++) for (unsigned b = 0; b < 1024; b++) {
        int got = ((uint8_t *)rb[i].contents)[b], expect = want[i != whole][b % 4];
        if (abs(got - expect) > 1) MDIE("integer blend probe, integer output %d: target %d pixel %u channel %u is %d, expected %d", whole, i, b / 4, b % 4, got, expect);
        checked++;
    }
    res_remove(texture[0]); res_remove(texture[1]); ib_state(0, 0, 0, 0);
    return checked;
}
static int integer_blend_probe(NvShader *vs) { return ib_formats(vs) + ib_two_outputs(vs, 0) + ib_two_outputs(vs, 1); }
