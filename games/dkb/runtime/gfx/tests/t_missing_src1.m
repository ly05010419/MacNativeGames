// An unwritten second blend source uses zero; compare the folded descriptor with a shader that explicitly writes that zero. A written output 1 is the second source through the
// shader's dual-source entry point (dmsh_build.py fs_dual), and without one counts as zero too.
#include "../nvn/nvn_mtl.m"

void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)

static void pixels(id<MTLRenderPipelineState> ps, __fp16 out[4]) {
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:4 height:4 mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModeShared;
    id<MTLTexture> t = [g_dev newTextureWithDescriptor:td];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = t; rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(.75, .5, .25, .875); rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLCommandQueue> q = [g_dev newCommandQueue]; id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
    [e setRenderPipelineState:ps]; [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [e endEncoding];
    [cb commit]; [cb waitUntilCompleted]; CHECK(cb.status == MTLCommandBufferStatusCompleted, "blend draw completes");
    [t getBytes:out bytesPerRow:32 fromRegion:MTLRegionMake2D(1,1,1,1) mipmapLevel:0];
}

int main(void) { @autoreleasepool {
    mtl_init(); NSError *error = nil;
    NSString *src = @"#include <metal_stdlib>\nusing namespace metal;\n"
        "vertex float4 v(uint i [[vertex_id]]) { return float4(i == 1 ? 3.0 : -1.0, i == 2 ? 3.0 : -1.0, 0, 1); }\n"
        "fragment float4 f() { return float4(.125,.25,.5,.375); }\n"
        "struct O { float4 a [[color(0)]]; float4 b [[color(0), index(1)]]; };\n"
        "fragment O f2() { return {float4(.125,.25,.5,.375), float4(0)}; }\n"
        "struct M { float4 a [[color(0)]]; float4 b [[color(1)]]; };\n"
        "fragment M f3() { return {float4(.125,.25,.5,.375), float4(.5,.25,.125,.75)}; }\n"
        "fragment O f3_d() { return {float4(.125,.25,.5,.375), float4(.5,.25,.125,.75)}; }\n";
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:src options:nil error:&error];
    CHECK(lib != nil, "reference shaders compile"); if (!lib) return 1;
    NvShader *vs = [NvShader new], *fs = [NvShader new];
    vs->fn = fn4_note([lib newFunctionWithName:@"v"], lib, @"v", nil);
    fs->fn = fn4_note([lib newFunctionWithName:@"f"], lib, @"f", nil);
    fs->rf.color_known = 1; fs->rf.color_mask = 1;
    pso_key k = {0}; k.samples = 1; k.chmask = 15; k.blend_en = 1; k.cfmt[0] = MTLPixelFormatRGBA16Float;
    int factors[] = {NVN_BLEND_FUNC_SRC1_COLOR, NVN_BLEND_FUNC_ONE_MINUS_SRC1_COLOR, NVN_BLEND_FUNC_SRC1_ALPHA, NVN_BLEND_FUNC_ONE_MINUS_SRC1_ALPHA};
    for (int i = 0; i < 4; i++) {
        k.blend[0] = (blend_st){.src_rgb=factors[i], .dst_rgb=factors[(i+1)%4], .src_a=factors[(i+2)%4], .dst_a=factors[(i+3)%4], .eq_rgb=1, .eq_a=1};
        MTLRenderPipelineDescriptor *d = pso_desc(&k, vs, fs, nil, 0);
        CHECK(d.colorAttachments[0].blendingEnabled, "blending remains enabled");
        MTLRenderPipelineDescriptor *ref = [d copy]; ref.fragmentFunction = [lib newFunctionWithName:@"f2"];
        ref.colorAttachments[0].sourceRGBBlendFactor = bfac(k.blend[0].src_rgb); ref.colorAttachments[0].destinationRGBBlendFactor = bfac(k.blend[0].dst_rgb);
        ref.colorAttachments[0].sourceAlphaBlendFactor = bfac(k.blend[0].src_a); ref.colorAttachments[0].destinationAlphaBlendFactor = bfac(k.blend[0].dst_a);
        id<MTLRenderPipelineState> reference = [g_dev newRenderPipelineStateWithDescriptor:ref error:&error];
        CHECK(reference != nil, "explicit secondary source compiles");
        __fp16 want[4]; if (reference) pixels(reference, want);
        for (int metal4 = 0; metal4 < 2; metal4++) {
            id<MTLRenderPipelineState> ps = metal4 ? pso_build(d, &error) : [g_dev newRenderPipelineStateWithDescriptor:d error:&error];
            CHECK(ps != nil, "folded pipeline compiles on both paths");
            if (ps && reference) { __fp16 got[4]; pixels(ps, got); CHECK(!memcmp(want, got, sizeof got), "folded blending equals explicit zero source"); }
        }
    }
    // Output 1 written: without a dual-source entry point it counts as zero, as an unwritten one; with one, the pipeline takes it and keeps the state
    fs->fn = fn4_note([lib newFunctionWithName:@"f3"], lib, @"f3", nil); fs->rf.color_mask = 3;
    MTLRenderPipelineDescriptor *d = pso_desc(&k, vs, fs, nil, 0);
    CHECK(d.fragmentFunction == fs->fn && d.colorAttachments[0].sourceRGBBlendFactor == bfac(blend_zero_src1(k.blend[0].src_rgb)), "written output 1 without a dual-source entry point counts as zero");
    fs->fn_d = fn4_note([lib newFunctionWithName:@"f3_d"], lib, @"f3_d", nil);
    d = pso_desc(&k, vs, fs, nil, 0);
    CHECK(d.fragmentFunction == fs->fn_d, "the pipeline takes the dual-source entry point");
    CHECK(d.colorAttachments[0].sourceRGBBlendFactor == bfac(k.blend[0].src_rgb) && d.colorAttachments[0].destinationAlphaBlendFactor == bfac(k.blend[0].dst_a), "its source-1 factors are kept");
    id<MTLRenderPipelineState> legacy = [g_dev newRenderPipelineStateWithDescriptor:d error:&error], m4 = pso_build(d, &error);
    CHECK(legacy != nil && m4 != nil, "the dual-source pipeline compiles on both paths");
    if (legacy && m4) { __fp16 a[4], b[4]; pixels(legacy, a); pixels(m4, b); CHECK(!memcmp(a, b, sizeof a), "both paths blend the same"); }
    k.cfmt[1] = MTLPixelFormatRGBA16Float; k.blend_en = 3; k.blend[1] = k.blend[0];
    d = pso_desc(&k, vs, fs, nil, 0);
    CHECK(d.fragmentFunction == fs->fn_d && d.colorAttachments[1].writeMask == MTLColorWriteMaskNone && !d.colorAttachments[1].blendingEnabled, "output 1's attachment is left as it was");
    k.cfmt[1] = 0; k.blend_en = 1; fs->fn_d = nil; fs->fn = fn4_note([lib newFunctionWithName:@"f"], lib, @"f", nil);
    k.blend[0] = (blend_st){.src_rgb=NVN_BLEND_FUNC_SRC1_COLOR, .dst_rgb=NVN_BLEND_FUNC_ONE, .src_a=NVN_BLEND_FUNC_ONE, .dst_a=NVN_BLEND_FUNC_ZERO, .eq_rgb=1, .eq_a=1};
    fs->rf.color_mask = 1; fs->rf.color_known = 0;
    CHECK(pso_desc(&k, vs, fs, nil, 0).colorAttachments[0].sourceRGBBlendFactor == bfac(k.blend[0].src_rgb), "unknown output map is preserved");
    CHECK(g_pb_spec == 5, "all four factors and the dual-source pipeline exercised Metal 4 specialization");
    printf("missing secondary blend source: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
