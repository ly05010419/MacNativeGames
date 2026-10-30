// Channel mapping of registered textures (tex_by_id): the descriptor's swizzle composed with the format's own mapping, read back from the GPU through the production lookup.
// The RGB565 family's red / blue swap is for uploaded textures only; a texture Metal cleared reads as cleared, and an image slot gets a writable view without the mapping (img_fit)
#include "../nvn/nvn_mtl.m"
uint64_t g_t0_ns;
int dm_guest_as_contains(uintptr_t address) { (void)address; return 0; }
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)
static id<MTLComputePipelineState> pipe_of(NSString *src, NSString *name) {
    NSError *error = nil; id<MTLLibrary> library = [g_dev newLibraryWithSource:src options:nil error:&error];
    id<MTLComputePipelineState> p = [g_dev newComputePipelineStateWithFunction:[library newFunctionWithName:name] error:&error];
    CHECK(p != nil, "pipeline created");
    return p;
}
static void run2(id<MTLComputePipelineState> p, id<MTLTexture> t, id<MTLBuffer> out) {
    id<MTLCommandBuffer> command = q_cb(); id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
    [encoder setComputePipelineState:p]; [encoder setTexture:t atIndex:0]; if (out) [encoder setBuffer:out offset:0 atIndex:0];
    [encoder dispatchThreads:MTLSizeMake(2, 1, 1) threadsPerThreadgroup:MTLSizeMake(2, 1, 1)];
    [encoder endEncoding]; [command commit]; [command waitUntilCompleted];
    CHECK(command.status == MTLCommandBufferStatusCompleted, "GPU work completed");
}
static void read_check(int tid, id<MTLTexture> view, const float expected[8], float tol) {   // the two texels as the shader reads them
    static id<MTLComputePipelineState> p;
    if (!p) p = pipe_of(@"#include <metal_stdlib>\nusing namespace metal;\nkernel void sample_packed(texture2d<float, access::read> t [[texture(0)]], device float4 *out [[buffer(0)]], uint i [[thread_position_in_grid]]) { out[i] = t.read(uint2(i, 0)); }\n", @"sample_packed");
    id<MTLBuffer> output = [g_dev newBufferWithLength:32 options:MTLResourceStorageModeShared];
    run2(p, view, output);
    float *actual = output.contents;
    for (int i = 0; i < 8; i++) { if (fabsf(actual[i] - expected[i]) > tol) fprintf(stderr, "case %d component %d: got %g expected %g\n", tid, i, actual[i], expected[i]); CHECK(fabsf(actual[i] - expected[i]) <= tol, "sampled channel matches NVN format and mapping"); }
}
// uploaded: as tex_mtl leaves a texture uploaded from guest memory, an NvPkg in the serial table, clean since its upload (no guest write since, so its pages are not looked at)
static void swizzle_case(int tid, int format, int view_format, int uploaded, const void *pixels, unsigned bytes, const uint8_t *builder, const uint8_t *mapping, const float expected[8]) {
    tex_t t = {.magic=MAGIC('T','E','X',0), .serial=98000+(unsigned)tid};
    t.d = (nvn_texdesc){.target=NVN_TEXTURE_TARGET_2D, .format=format, .width=2, .height=1, .depth=1, .levels=1, .samples=1};
    if (builder) { t.set_swz = 1; for (int c = 0; c < 4; c++) t.swz |= (uint16_t)builder[c] << (3 * c); }
    MTLTextureDescriptor *d = tex_desc(&t, 1); d.storageMode = MTLStorageModeShared;
    id<MTLTexture> base = [g_dev newTextureWithDescriptor:d];
    CHECK(base != nil, "packed texture created");
    [base replaceRegion:MTLRegionMake2D(0, 0, 2, 1) mipmapLevel:0 withBytes:pixels bytesPerRow:bytes];
    if (uploaded) { t.has_layout = 1; t.mem = (uint8_t *)pixels; NvPkg *pk = [NvPkg new]; pk->tex = base; pk->span = pixels; pk->seen = nvn_dirty_now(); serial_put(t.serial, pk); }
    else serial_put(t.serial, base);
    view_t v = {0}; if (mapping) { v.set_swizzle = 1; memcpy(v.swz, mapping, 4); }
    if (view_format) { v.set_format = 1; v.format = view_format; }
    mtl_reg_tex(tid, &t, mapping || view_format ? &v : NULL);
    os_unfair_lock_lock(&g_rmu);
    id<MTLTexture> view = tex_by_id(tid);
    CHECK(view != nil, "registered view created");
    CHECK(tex_by_id(tid) == view, "cached view retained");
    os_unfair_lock_unlock(&g_rmu);
    read_check(tid, view, expected, 0.001f);
}
// What Metal itself wrote into a texture of the RGB565 family is in Metal's order: a clear to red reads red, with no swap
static void cleared_case(int tid, int format) {
    tex_t t = {.magic=MAGIC('T','E','X',0), .serial=98000+(unsigned)tid};
    t.d = (nvn_texdesc){.target=NVN_TEXTURE_TARGET_2D, .format=format, .width=2, .height=1, .depth=1, .levels=1, .samples=1};
    id<MTLTexture> base = [g_dev newTextureWithDescriptor:tex_desc(&t, 0)];
    CHECK(base != nil && (base.usage & MTLTextureUsageRenderTarget), "render target created");
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = base; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(1, 0, 0, 1);
    id<MTLCommandBuffer> command = q_cb(); [[command renderCommandEncoderWithDescriptor:rp] endEncoding]; [command commit]; [command waitUntilCompleted];
    serial_put(t.serial, base);
    mtl_reg_tex(tid, &t, NULL);
    os_unfair_lock_lock(&g_rmu); id<MTLTexture> view = tex_by_id(tid); os_unfair_lock_unlock(&g_rmu);
    CHECK(view == base, "a cleared RGB565-family texture needs no view");
    read_check(tid, view, (const float[8]){1,0,0,1, 1,0,0,1}, 0.001f);
}
// An RGBX8 image texture: sampled through the swizzled view (alpha 1), which Metal makes read-only; the image slot's view (img_fit) is writable, without the mapping, and stores what the shader wrote
static void image_case(int tid) {
    tex_t t = {.magic=MAGIC('T','E','X',0), .serial=98000+(unsigned)tid};
    t.d = (nvn_texdesc){.target=NVN_TEXTURE_TARGET_2D, .format=NVN_FORMAT_RGBX8, .width=2, .height=1, .depth=1, .levels=1, .samples=1, .flags=NVN_TEXTURE_FLAGS_IMAGE};
    MTLTextureDescriptor *d = tex_desc(&t, 0); d.storageMode = MTLStorageModeShared;
    id<MTLTexture> base = [g_dev newTextureWithDescriptor:d];
    CHECK(base != nil && (base.usage & MTLTextureUsageShaderWrite), "image texture created");
    serial_put(t.serial, base);
    mtl_reg_tex(tid, &t, NULL);
    os_unfair_lock_lock(&g_rmu);
    id<MTLTexture> view = tex_by_id(tid); uint64_t rid = 0, rid2 = 0;
    CHECK(view != base && !(view.usage & MTLTextureUsageShaderWrite), "the sampled view is swizzled and read-only");
    id<MTLTexture> img = img_fit(tid, view, NVN_DPF_FLOAT, &rid);
    MTLTextureSwizzleChannels s = img.swizzle;
    CHECK(img && (img.usage & MTLTextureUsageShaderWrite) && rid == img.gpuResourceID._impl, "the image slot gets a writable view");
    CHECK(s.red == MTLTextureSwizzleRed && s.green == MTLTextureSwizzleGreen && s.blue == MTLTextureSwizzleBlue && s.alpha == MTLTextureSwizzleAlpha, "the image view has no channel mapping");
    CHECK(img_fit(tid, view, NVN_DPF_FLOAT, &rid2) == img && rid2 == rid, "the image view is kept for the next bind");
    os_unfair_lock_unlock(&g_rmu);
    run2(pipe_of(@"#include <metal_stdlib>\nusing namespace metal;\nkernel void store(texture2d<float, access::write> t [[texture(0)]], uint i [[thread_position_in_grid]]) { t.write(float4(0.25, 0.5, 0.75, 0.125), uint2(i, 0)); }\n", @"store"), img, nil);
    uint8_t px[8]; [base getBytes:px bytesPerRow:8 fromRegion:MTLRegionMake2D(0, 0, 2, 1) mipmapLevel:0];
    for (int i = 0; i < 8; i++) CHECK(abs((int)px[i] - (int[4]){64, 128, 191, 32}[i % 4]) <= 1, "the image write is stored as written");
    read_check(tid, view, (const float[8]){px[0]/255.f, px[1]/255.f, px[2]/255.f, 1, px[4]/255.f, px[5]/255.f, px[6]/255.f, 1}, 0.001f);
}
int main(void) { @autoreleasepool {
    mtl_init();
    uint16_t pixels[2] = {0x001f, 0xf800};
    uint8_t rgb1[4] = {NVN_SWZ_R, NVN_SWZ_G, NVN_SWZ_B, NVN_SWZ_ONE};
    uint8_t map[4] = {NVN_SWZ_B, NVN_SWZ_R, NVN_SWZ_ONE, NVN_SWZ_ZERO};
    float rgb[8] = {1,0,0,1, 0,0,1,1};
    float bgr[8] = {0,0,1,1, 1,0,0,1};
    float composed[8] = {0,1,1,0, 1,0,1,0};
    swizzle_case(400, NVN_FORMAT_RGB565, 0, 1, pixels, 4, NULL, NULL, rgb);
    swizzle_case(401, NVN_FORMAT_RGB565, 0, 1, pixels, 4, rgb1, NULL, rgb);
    swizzle_case(402, NVN_FORMAT_RGB565, 0, 1, pixels, 4, rgb1, map, composed);
    swizzle_case(403, NVN_FORMAT_BGR565, 0, 1, pixels, 4, NULL, NULL, bgr);
    swizzle_case(404, NVN_FORMAT_RG8, 0, 1, pixels, 4, NULL, NULL, (float[8]){31/255.f,0,0,1, 0,248/255.f,0,1});
    uint8_t rgba[8] = {255,0,0,0, 0,0,255,64};
    swizzle_case(405, NVN_FORMAT_RGBX8, 0, 0, rgba, 8, NULL, NULL, rgb);
    swizzle_case(406, NVN_FORMAT_RGBX8, 0, 0, rgba, 8, NULL, (uint8_t[4]){NVN_SWZ_A,NVN_SWZ_A,NVN_SWZ_A,NVN_SWZ_A}, (float[8]){1,1,1,1,1,1,1,1});
    swizzle_case(407, NVN_FORMAT_RGBA8, NVN_FORMAT_RGBX8, 0, rgba, 8, NULL, NULL, rgb);
    swizzle_case(408, NVN_FORMAT_RGBA8, 0, 0, rgba, 8, NULL, NULL, (float[8]){1,0,0,0, 0,0,1,64/255.f});
    swizzle_case(409, NVN_FORMAT_RGBX8, NVN_FORMAT_RGBA8, 0, rgba, 8, NULL, NULL, (float[8]){1,0,0,0, 0,0,1,64/255.f});
    swizzle_case(410, NVN_FORMAT_RGB565, -1, 1, pixels, 4, NULL, NULL, rgb);
    swizzle_case(411, NVN_FORMAT_RGBX8, -1, 0, rgba, 8, NULL, NULL, rgb);
    swizzle_case(412, NVN_FORMAT_RGB565, 0, 0, pixels, 4, NULL, NULL, bgr);   // the same bits in a texture Metal made: Metal's order
    cleared_case(413, NVN_FORMAT_RGB565);
    cleared_case(414, NVN_FORMAT_BGR565);
    image_case(415);
    printf("texture view swizzle: %d checks, %d failed\n", checks, fails);
    return fails != 0;
} }
