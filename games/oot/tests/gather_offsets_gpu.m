// gather_offsets_gpu: the one-gather form GatherOffsets.cs writes for a 2x2 square of offsets equals the four gathers
// with each offset written out, texel for texel. Build and run (outside the checkout):
//   clang -fobjc-arc -O2 -framework Metal -framework Foundation -o <work>/gather_offsets_gpu gather_offsets_gpu.m && <work>/gather_offsets_gpu
// A 64x64 R32Float texture of distinct values, repeat and clamp samplers, 4,096 coordinates over and past the edges, the
// offset orders (0,0) (1,0) (0,1) (1,1) and (1,0) (0,0) (1,1) (0,1), and a square at (2,2). Exit status 1 on a difference.
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include <stdio.h>
static NSString *SRC = @
"#include <metal_stdlib>\nusing namespace metal;\n"
"kernel void k(texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], device float4* o [[buffer(0)]], uint3 id [[thread_position_in_grid]]) {\n"
"  uint i = id.y * 64u + id.x; float2 uv = (float2(id.xy) * 1.37 - 12.3) / 64.0;\n"
"  o[i * 6u + 0u] = float4(t.gather(s, uv, int2(0, 0), component::x).w, t.gather(s, uv, int2(1, 0), component::x).w, t.gather(s, uv, int2(0, 1), component::x).w, t.gather(s, uv, int2(1, 1), component::x).w);\n"
"  o[i * 6u + 1u] = t.gather(s, uv, int2(0, 0), component::x).wzxy;\n"
"  o[i * 6u + 2u] = float4(t.gather(s, uv, int2(1, 0), component::x).w, t.gather(s, uv, int2(0, 0), component::x).w, t.gather(s, uv, int2(1, 1), component::x).w, t.gather(s, uv, int2(0, 1), component::x).w);\n"
"  o[i * 6u + 3u] = t.gather(s, uv, int2(0, 0), component::x).zwyx;\n"
"  o[i * 6u + 4u] = float4(t.gather(s, uv, int2(3, 2), component::x).w, t.gather(s, uv, int2(2, 2), component::x).w, t.gather(s, uv, int2(3, 3), component::x).w, t.gather(s, uv, int2(2, 3), component::x).w);\n"
"  o[i * 6u + 5u] = t.gather(s, uv, int2(2, 2), component::x).zwyx;\n}\n";
int main(void) { @autoreleasepool {
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice(); id<MTLCommandQueue> q = [dev newCommandQueue]; NSError *e = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:SRC options:nil error:&e]; if (!lib) { printf("%s\n", e.localizedDescription.UTF8String); return 1; }
    id<MTLComputePipelineState> p = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"k"] error:&e]; if (!p) { printf("%s\n", e.localizedDescription.UTF8String); return 1; }
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Float width:64 height:64 mipmapped:NO]; id<MTLTexture> t = [dev newTextureWithDescriptor:td];
    float px[4096]; for (int i = 0; i < 4096; i++) px[i] = (float)(i * 7 + 1); [t replaceRegion:MTLRegionMake2D(0, 0, 64, 64) mipmapLevel:0 withBytes:px bytesPerRow:256];
    id<MTLBuffer> o = [dev newBufferWithLength:4096 * 6 * 16 options:MTLResourceStorageModeShared]; int failed = 0;
    printf("%s\n", dev.name.UTF8String);
    for (int mode = 0; mode < 2; mode++) {
        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new]; sd.sAddressMode = sd.tAddressMode = mode ? MTLSamplerAddressModeClampToEdge : MTLSamplerAddressModeRepeat; sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
        id<MTLSamplerState> s = [dev newSamplerStateWithDescriptor:sd];
        id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLComputeCommandEncoder> en = [cb computeCommandEncoder]; [en setComputePipelineState:p]; [en setTexture:t atIndex:0]; [en setSamplerState:s atIndex:0]; [en setBuffer:o offset:0 atIndex:0];
        [en dispatchThreadgroups:MTLSizeMake(8, 8, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)]; [en endEncoding]; [cb commit]; [cb waitUntilCompleted];
        const float *r = o.contents; long diff[3] = {0};
        for (int i = 0; i < 4096; i++) for (int k = 0; k < 3; k++) for (int c = 0; c < 4; c++) diff[k] += r[(i * 6 + k * 2) * 4 + c] != r[(i * 6 + k * 2 + 1) * 4 + c];
        printf("%s: texels that differ of 16,384: offsets in order %ld, reordered %ld, square at (2,2) %ld  %s\n", mode ? "clamp " : "repeat", diff[0], diff[1], diff[2], diff[0] + diff[1] + diff[2] ? "FAILED" : "ok");
        failed += diff[0] + diff[1] + diff[2] != 0;
    }
    printf(failed ? "FAILED\n" : "PASSED\n"); return failed != 0; } }
