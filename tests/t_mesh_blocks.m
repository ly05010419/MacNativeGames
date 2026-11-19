// A mesh shader with no task shader launches its workgroups through the object function the translator writes for it
// (MeshStage.cs ObjectEntry main0_o), 1024 at a time, as nvn_mesh_tasks.inc mtk_encode draws it: a direct draw with
// ceil(count / 1024) object threadgroups, an indirect one with MTK_OBJECT_BLOCKS_INDIRECT of them, the count in word 0 of
// the record at object buffer 30. Built and run by tests/test_oot_mesh_blocks.py with the translator's MSL as argv[1].
// A probe mesh function with the same payload adds 1 at first + block base + threadgroup position for every workgroup.
#import <Metal/Metal.h>
#include <stdio.h>
#include <string.h>

enum { BLOCK = 1024, BLOCKS_INDIRECT = 16, COUNT_BUFFER = 30, FIRST = 5, SLOTS = BLOCKS_INDIRECT * BLOCK + 64 };

static const char *probe =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "struct dm_block_t { uint base; };\n"
    "struct V { float4 p [[position]]; };\n"
    "using M = metal::mesh<V, void, 3, 1, metal::topology::triangle>;\n"
    "[[mesh]] void probe_mesh(M out, const object_data dm_block_t& b [[payload]], uint tg [[threadgroup_position_in_grid]],\n"
    "    uint lid [[thread_position_in_threadgroup]], device atomic_uint* hits [[buffer(0)]], constant uint* first [[buffer(29)]])\n"
    "{ if (lid == 0u) atomic_fetch_add_explicit(&hits[first[0] + b.base + tg], 1u, memory_order_relaxed); if (lid == 0u) out.set_primitive_count(0); }\n";

static int run(id<MTLDevice> dev, id<MTLRenderPipelineState> ps, id<MTLCommandQueue> q, uint32_t count, int indirect) {
    id<MTLBuffer> hits = [dev newBufferWithLength:SLOTS * 4 options:MTLResourceStorageModeShared];
    memset(hits.contents, 0, SLOTS * 4);
    uint32_t record[4] = {count, 1, 1, 0}, first = FIRST;
    id<MTLBuffer> rec = [dev newBufferWithBytes:record length:sizeof record options:MTLResourceStorageModeShared];
    id<MTLBuffer> fb = [dev newBufferWithBytes:&first length:4 options:MTLResourceStorageModeShared];
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:8 height:8 mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModePrivate;
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = [dev newTextureWithDescriptor:td];
    rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].storeAction = MTLStoreActionDontCare;
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
    [e setRenderPipelineState:ps];
    [e setObjectBuffer:rec offset:0 atIndex:COUNT_BUFFER];
    [e setMeshBuffer:hits offset:0 atIndex:0]; [e setMeshBuffer:fb offset:0 atIndex:29];
    NSUInteger groups = indirect ? BLOCKS_INDIRECT : (count + BLOCK - 1) / BLOCK;
    if (groups) [e drawMeshThreadgroups:MTLSizeMake(groups, 1, 1) threadsPerObjectThreadgroup:MTLSizeMake(1, 1, 1) threadsPerMeshThreadgroup:MTLSizeMake(32, 1, 1)];
    [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
    if (cb.error) { fprintf(stderr, "GPU error: %s\n", cb.error.localizedDescription.UTF8String); return 1; }
    const uint32_t *h = hits.contents; int bad = 0;
    for (uint32_t i = 0; i < SLOTS; i++) {
        uint32_t want = i >= FIRST && i < FIRST + count ? 1 : 0;
        if (h[i] != want) { if (bad++ < 4) fprintf(stderr, "count %u %s: slot %u ran %u times, want %u\n", count, indirect ? "indirect" : "direct", i, h[i], want); }
    }
    printf("count %u %s: %s\n", count, indirect ? "indirect" : "direct", bad ? "WRONG" : "every workgroup once");
    return bad != 0;
}

int main(int argc, char **argv) { @autoreleasepool {
    if (argc < 2) { fprintf(stderr, "usage: t_mesh_blocks <translator .metal>\n"); return 2; }
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    if (!dev || ![dev supportsFamily:MTLGPUFamilyApple7]) { puts("SKIP: no mesh-capable GPU"); return 0; }
    NSError *err = nil;
    NSString *src = [NSString stringWithContentsOfFile:@(argv[1]) encoding:NSUTF8StringEncoding error:&err];
    if (!src) { fprintf(stderr, "read %s: %s\n", argv[1], err.localizedDescription.UTF8String); return 1; }
    MTLCompileOptions *o = [MTLCompileOptions new]; o.mathMode = MTLMathModeSafe;
    id<MTLLibrary> translated = [dev newLibraryWithSource:src options:o error:&err];
    if (!translated) { fprintf(stderr, "the translator's MSL does not compile: %s\n", err.localizedDescription.UTF8String); return 1; }
    id<MTLFunction> object = [translated newFunctionWithName:@"main0_o"];
    if (!object || object.functionType != MTLFunctionTypeObject) { fprintf(stderr, "no object function main0_o in the translation\n"); return 1; }
    id<MTLLibrary> pl = [dev newLibraryWithSource:@(probe) options:o error:&err];
    if (!pl) { fprintf(stderr, "probe: %s\n", err.localizedDescription.UTF8String); return 1; }
    MTLMeshRenderPipelineDescriptor *d = [MTLMeshRenderPipelineDescriptor new];
    d.objectFunction = object; d.meshFunction = [pl newFunctionWithName:@"probe_mesh"];
    d.payloadMemoryLength = 16; d.maxTotalThreadsPerObjectThreadgroup = 1; d.maxTotalThreadsPerMeshThreadgroup = 32;
    d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm; d.rasterizationEnabled = NO;
    id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithMeshDescriptor:d options:MTLPipelineOptionNone reflection:nil error:&err];
    if (!ps) { fprintf(stderr, "pipeline: %s\n", err.localizedDescription.UTF8String); return 1; }
    id<MTLCommandQueue> q = [dev newCommandQueue];
    int failed = 0;
    const uint32_t counts[] = {1, 1023, 1024, 1025, 1238, 1771, 3000};
    for (unsigned i = 0; i < sizeof counts / sizeof *counts; i++) { failed |= run(dev, ps, q, counts[i], 0); failed |= run(dev, ps, q, counts[i], 1); }
    failed |= run(dev, ps, q, 0, 1);   // an indirect draw whose count is 0: every block launches nothing
    if (!failed) puts("mesh blocks passed");
    return failed;
} }
