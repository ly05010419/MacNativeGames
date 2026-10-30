// tex_fit's type-compatible view of a texture that is a view itself (the view cache's: a level and layer range, a swizzle) is made from the texture under it, with the same levels,
// layers and swizzle. Metal takes a view's residency from the texture it was made from: a view of a view read as not resident (shader validation) although the texture under both was
// in the residency set (Breath of the Wild's 64x64 cube read from a 2D-array view, found with nvn_replay)
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)
enum { W = 8, LEVELS = 3, LAYERS = 4, TID = 5 };
static uint8_t texel(NSUInteger level, NSUInteger layer, int c) { return (uint8_t)(16 * level + 4 * layer + c + 1); }   // every level, layer and channel its own value
static void read2d(id<MTLComputePipelineState> pipeline, id<MTLTexture> t, uint32_t level, float out[4]) {
    id<MTLBuffer> b = [g_dev newBufferWithLength:16 options:MTLResourceStorageModeShared]; memset(b.contents, 0xcd, 16);
    id<MTLCommandBuffer> cb = [g_q commandBuffer]; id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
    [e setComputePipelineState:pipeline]; [e setTexture:t atIndex:0]; [e setBytes:&level length:sizeof level atIndex:0]; [e setBuffer:b offset:0 atIndex:1];
    [e dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)]; [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
    CHECK(cb.status == MTLCommandBufferStatusCompleted && !cb.error, "the GPU reads the fitted view");
    memcpy(out, b.contents, 16);
}
static int same(const float v[4], NSUInteger level, NSUInteger layer, const int ch[4]) {
    for (int c = 0; c < 4; c++) if (fabsf(v[c] - texel(level, layer, ch[c]) / 255.0f) > 1e-4f) return 0;
    return 1;
}
int main(void) { @autoreleasepool {
    mtl_init(); NSError *error = nil;
    NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
        "kernel void rd(texture2d<float> t [[texture(0)]], constant uint& level [[buffer(0)]], device float4* out [[buffer(1)]]) { out[0] = t.read(uint2(0, 0), level); }\n";
    id<MTLLibrary> library = [g_dev newLibraryWithSource:source options:nil error:&error];
    id<MTLComputePipelineState> pipeline = library ? [g_dev newComputePipelineStateWithFunction:[library newFunctionWithName:@"rd"] error:&error] : nil;
    if (!pipeline) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 2; }
    MTLTextureDescriptor *d = [MTLTextureDescriptor new];
    d.textureType = MTLTextureType2DArray; d.width = d.height = W; d.arrayLength = LAYERS; d.mipmapLevelCount = LEVELS;
    d.pixelFormat = MTLPixelFormatRGBA8Unorm; d.storageMode = MTLStorageModeShared; d.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> root = [g_dev newTextureWithDescriptor:d];
    for (NSUInteger lv = 0; lv < LEVELS; lv++) for (NSUInteger l = 0; l < LAYERS; l++) {
        NSUInteger w = W >> lv; uint8_t px[W * W * 4];
        for (NSUInteger i = 0; i < w * w; i++) for (int c = 0; c < 4; c++) px[4 * i + c] = texel(lv, l, c);
        [root replaceRegion:MTLRegionMake2D(0, 0, w, w) mipmapLevel:lv slice:l withBytes:px bytesPerRow:4 * w bytesPerImage:4 * w * w];
    }
    // the view cache's view: levels 1-2, layers 2-3, red and blue swapped
    MTLTextureSwizzleChannels sw = MTLTextureSwizzleChannelsMake(MTLTextureSwizzleBlue, MTLTextureSwizzleGreen, MTLTextureSwizzleRed, MTLTextureSwizzleAlpha);
    id<MTLTexture> view = [root newTextureViewWithPixelFormat:root.pixelFormat textureType:MTLTextureType2DArray levels:NSMakeRange(1, 2) slices:NSMakeRange(2, 2) swizzle:sw];
    os_unfair_lock_lock(&g_rmu);
    g_treg = calloc(TID + 1, sizeof *g_treg); g_ntreg = TID + 1;
    uint64_t rid = 0, rid2 = 0;
    id<MTLTexture> fit = tex_fit(TID, view, MTLTextureType2D, &rid);   // a shader that declares 2D reads the 2D array's first layer
    id<MTLTexture> again = tex_fit(TID, view, MTLTextureType2D, &rid2);
    os_unfair_lock_unlock(&g_rmu);
    CHECK(fit && fit.textureType == MTLTextureType2D, "a 2D view of the 2D-array view");
    CHECK(fit.parentTexture == root, "made from the texture under the view, not from the view");
    CHECK(fit.parentRelativeLevel == 1 && fit.mipmapLevelCount == 2 && fit.parentRelativeSlice == 2, "the view's levels and first layer");
    CHECK(fit.swizzle.red == MTLTextureSwizzleBlue && fit.swizzle.blue == MTLTextureSwizzleRed, "the view's swizzle");
    CHECK(rid == fit.gpuResourceID._impl && again == fit && rid2 == rid, "kept for the next binding of the same texture");
    const int swapped[4] = {2, 1, 0, 3};
    float v[4];
    read2d(pipeline, fit, 0, v); CHECK(same(v, 1, 2, swapped), "level 0 of the fitted view is level 1, layer 2, swizzled");
    read2d(pipeline, fit, 1, v); CHECK(same(v, 2, 2, swapped), "level 1 of the fitted view is level 2, layer 2, swizzled");
    printf("tex_fit views from the root: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
