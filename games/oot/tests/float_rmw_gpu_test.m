/* #2070 on the GPU: a SPIRV-Cross style kernel whose every thread adds to one word, lowered by frmw_lower, gives the
   exact sum, and each thread's returned old value is a distinct prefix of the sum (an atomic order). Runs on any Mac:
   the lowered form uses no float atomic, so it is what an M5 builds. Usage: float_rmw_gpu_test <width> <height> */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "../third_party/d2r_gfx/gfx/nvn/nvn_float_rmw.h"

static const char *k_src =
    "#include <metal_stdlib>\n#include <metal_atomic>\n\nusing namespace metal;\n\n"
    "struct _30 { float _m0[1]; };\n"
    "static inline __attribute__((always_inline))\n"
    "int _54(thread const int& _659, thread const int& _660, device _30& cp_sf0)\n{\n"
    "    uint _663 = uint(int(uint(_659) >> uint(2)));\n"
    "    float _676 = atomic_fetch_add_explicit((device atomic_float*)&cp_sf0._m0[int(_663)], as_type<float>(_660), memory_order_relaxed);\n"
    "    float _665 = _676;\n    return as_type<int>(_665);\n}\n"
    "kernel void main0(device _30& cp_sf0 [[buffer(0)]], device float *old [[buffer(1)]], constant uint &width [[buffer(2)]],\n"
    "                  uint2 g [[thread_position_in_grid]])\n{\n"
    "    int _278 = 0; int _280 = ((g.x + g.y * 7) % 3) == 0 ? 0 : 1065353216;\n"
    "    int _282 = _54(_278, _280, cp_sf0);\n    old[g.y * width + g.x] = as_type<float>(_282);\n}\n";

int main(int argc, const char **argv) { @autoreleasepool {
    uint w = argc > 2 ? atoi(argv[1]) : 256, h = argc > 2 ? atoi(argv[2]) : 256;
    int n = 0; char *lowered = frmw_lower(k_src, &n);
    if (!lowered || n != 1) { fprintf(stderr, "FAIL: not lowered\n"); return 1; }
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    if (!dev) { puts("SKIP: no Metal device"); return 0; }
    MTLCompileOptions *o = [MTLCompileOptions new]; o.preserveInvariance = YES; o.mathMode = MTLMathModeSafe;   // as totk_precise_opts
    NSError *e = nil; id<MTLLibrary> lib = [dev newLibraryWithSource:@(lowered) options:o error:&e]; free(lowered);
    if (!lib) { fprintf(stderr, "FAIL: library: %s\n", e.description.UTF8String); return 1; }
    id<MTLComputePipelineState> ps = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"main0"] error:&e];
    if (!ps) { fprintf(stderr, "FAIL: pipeline: %s\n", e.description.UTF8String); return 1; }
    id<MTLBuffer> sum = [dev newBufferWithLength:16 options:MTLResourceStorageModeShared];
    id<MTLBuffer> old = [dev newBufferWithLength:(NSUInteger)w * h * 4 options:MTLResourceStorageModeShared];
    memset(sum.contents, 0, 16);
    id<MTLCommandQueue> q = [dev newCommandQueue]; id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLComputeCommandEncoder> en = [cb computeCommandEncoder]; [en setComputePipelineState:ps];
    [en setBuffer:sum offset:0 atIndex:0]; [en setBuffer:old offset:0 atIndex:1]; [en setBytes:&w length:4 atIndex:2];
    [en dispatchThreads:MTLSizeMake(w, h, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)]; [en endEncoding];
    [cb commit]; [cb waitUntilCompleted];
    if (cb.error) { fprintf(stderr, "FAIL: GPU: %s\n", cb.error.description.UTF8String); return 1; }
    double expect = 0; for (uint y = 0; y < h; y++) for (uint x = 0; x < w; x++) expect += ((x + y * 7) % 3) != 0;
    float got = ((float *)sum.contents)[0];
    /* Each adding thread saw a distinct old value 0, 1, ..., expect-1 (adds of 1.0 are exact up to 2^24). */
    char *seen = calloc((size_t)expect + 1, 1); const float *olds = old.contents; int bad = 0;
    for (uint y = 0; y < h; y++) for (uint x = 0; x < w; x++) {
        if (((x + y * 7) % 3) == 0) continue;
        float v = olds[y * w + x]; long i = (long)v;
        if (v != (float)i || i < 0 || i >= (long)expect || seen[i]++) bad++;
    }
    free(seen);
    printf("%ux%u: sum %.0f (expected %.0f), %.3f ms GPU, %d bad old values\n", w, h, got, expect, (cb.GPUEndTime - cb.GPUStartTime) * 1e3, bad);
    if (got != expect || bad) { fprintf(stderr, "FAIL: not exact\n"); return 1; }
    puts("float rmw lowering on the GPU: exact");
    return 0;
}}
