// A runtime branch chooses one of two cube inputs. An unused input may retain
// a 2D descriptor from an earlier pass: production tex_fit must bind a valid
// zero cube without diagnosing the binding itself as a rendering error.
// The same for a 3D input holding a 2D texture or 2D array (Batman: Arkham Knight's
// volume slots): the zero 3D texture.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)
static unsigned errors(void) {
    unsigned n = 0; for (unsigned i = 0; i < sizeof g_red_once / sizeof *g_red_once; i++) n += g_red_once[i]; return n;
}
static id<MTLTexture> texture(MTLTextureType type, NSUInteger layers) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor new];
    d.textureType = type; d.width = d.height = d.depth = 1; d.arrayLength = layers;
    d.pixelFormat = MTLPixelFormatRGBA8Unorm; d.storageMode = MTLStorageModeShared; d.usage = MTLTextureUsageShaderRead;
    return [g_dev newTextureWithDescriptor:d];
}
static void run_case(id<MTLTexture> descriptor, MTLTextureType want, id<MTLTexture> primary, id<MTLComputePipelineState> pipeline) {
    unsigned before = errors(); uint64_t rid = 0;
    os_unfair_lock_lock(&g_rmu);
    id<MTLTexture> fitted = tex_fit(272, descriptor, want, &rid);
    os_unfair_lock_unlock(&g_rmu);
    CHECK(fitted && fitted.textureType == want, "fallback matches the shader's declared type");
    CHECK(rid == fitted.gpuResourceID._impl && rid != 0, "binding uses the fallback's actual resource ID");
    CHECK(errors() == before, "conditional descriptor binding does not report a rendering error");
    if (!fitted) return;
    id<MTLBuffer> output = [g_dev newBufferWithLength:16 options:MTLResourceStorageModeShared];
    for (uint32_t select = 0; select < 2; select++) {
        memset(output.contents, 0xcd, 16);
        id<MTLCommandBuffer> cb = [g_q commandBuffer]; id<MTLComputeCommandEncoder> encoder = [cb computeCommandEncoder];
        [encoder setComputePipelineState:pipeline]; [encoder setTexture:primary atIndex:0]; [encoder setTexture:fitted atIndex:1];
        [encoder setBytes:&select length:sizeof select atIndex:0]; [encoder setBuffer:output offset:0 atIndex:1];
        [encoder dispatchThreadgroups:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];
        [encoder endEncoding]; [cb commit]; [cb waitUntilCompleted];
        CHECK(cb.status == MTLCommandBufferStatusCompleted && !cb.error, "GPU accepts both statically declared resources");
        const float *value = output.contents;
        CHECK(select ? value[0] == 0 && value[1] == 0 && value[2] == 0 && value[3] == 0 :
                       value[0] == 0 && value[1] == 0 && value[2] == 1 && value[3] == 1,
              select ? "selected fallback reads zero" : "unused incompatible descriptor leaves the real texture's blue output intact");
    }
}
int main(void) { @autoreleasepool {
    mtl_init(); NSError *error = nil;
    NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
        "kernel void cube(texturecube<float> a [[texture(0)]], texturecube<float> b [[texture(1)]], constant uint& select [[buffer(0)]], device float4* out [[buffer(1)]]) { constexpr sampler s(coord::normalized, filter::nearest); if (select) out[0] = b.sample(s,float3(0,0,1),level(0)); else out[0] = a.sample(s,float3(0,0,1),level(0)); }\n"
        "kernel void cubes(texturecube<float> a [[texture(0)]], texturecube_array<float> b [[texture(1)]], constant uint& select [[buffer(0)]], device float4* out [[buffer(1)]]) { constexpr sampler s(coord::normalized, filter::nearest); if (select) out[0] = b.sample(s,float3(0,0,1),0,level(0)); else out[0] = a.sample(s,float3(0,0,1),level(0)); }\n"
        "kernel void volume(texture3d<float> a [[texture(0)]], texture3d<float> b [[texture(1)]], constant uint& select [[buffer(0)]], device float4* out [[buffer(1)]]) { constexpr sampler s(coord::normalized, filter::nearest); if (select) out[0] = b.sample(s,float3(0.5),level(0)); else out[0] = a.sample(s,float3(0.5),level(0)); }\n";
    id<MTLLibrary> library = [g_dev newLibraryWithSource:source options:nil error:&error];
    if (!library) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 2; }
    id<MTLTexture> primary = texture(MTLTextureTypeCube, 1);
    const uint8_t blue[] = {0,0,255,255};
    for (NSUInteger face = 0; face < 6; face++) [primary replaceRegion:MTLRegionMake3D(0,0,0,1,1,1) mipmapLevel:0 slice:face withBytes:blue bytesPerRow:4 bytesPerImage:4];
    for (int array = 0; array < 2; array++) {
        id<MTLComputePipelineState> pipeline = [g_dev newComputePipelineStateWithFunction:[library newFunctionWithName:array ? @"cubes" : @"cube"] error:&error];
        if (!pipeline) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 2; }
        const NSUInteger layer_counts[] = {0, 1, 5};
        for (int i = 0; i < 3; i++) {
            NSUInteger layers = layer_counts[i];
            run_case(texture(layers ? MTLTextureType2DArray : MTLTextureType2D, layers ?: 1), array ? MTLTextureTypeCubeArray : MTLTextureTypeCube, primary, pipeline);
        }
    }
    id<MTLTexture> volume = texture(MTLTextureType3D, 1);
    [volume replaceRegion:MTLRegionMake3D(0,0,0,1,1,1) mipmapLevel:0 slice:0 withBytes:blue bytesPerRow:4 bytesPerImage:4];
    id<MTLComputePipelineState> pipeline = [g_dev newComputePipelineStateWithFunction:[library newFunctionWithName:@"volume"] error:&error];
    if (!pipeline) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 2; }
    const NSUInteger layer_counts[] = {0, 1, 5};
    for (int i = 0; i < 3; i++) run_case(texture(layer_counts[i] ? MTLTextureType2DArray : MTLTextureType2D, layer_counts[i] ?: 1), MTLTextureType3D, volume, pipeline);
    printf("cube and 3D placeholders: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
