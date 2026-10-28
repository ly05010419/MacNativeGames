// Run the composed mesh entry, using a translated Maxwell GS whose viewport
// mask depends on PrimitiveId >> 5 and a captured vertex attribute. The CPU
// visibility oracle crosses the 32-primitive group boundary independently.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct { uint64_t base; uint32_t stride, divisor; } stream;
typedef struct { stream s[16]; uint32_t attr[16]; } layout;
typedef struct { uint64_t ib; uint32_t type, first, count; int32_t base_vertex; uint32_t base_instance, pad; } draw_args;

int main(int argc, char **argv) { @autoreleasepool {
    if (argc != 2) return 2;
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) return 77;
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&error];
    if (!library) { fprintf(stderr, "%s\n", error.description.UTF8String); return 1; }
    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLBuffer> vertices = [device newBufferWithLength:1024 * 16 options:MTLResourceStorageModeShared];
    float *v = vertices.contents;
    const float corners[3][2] = {{-0.8f, -0.8f}, {0.8f, -0.8f}, {0, 0.8f}};
    for (unsigned i = 0; i < 1024; i++) {
        unsigned primitive = i / 3;
        v[4 * i] = (2.0f * (primitive % 16) + 1 + corners[i % 3][0]) / 16 - 1;
        v[4 * i + 1] = (2.0f * (primitive / 16) + 1 + corners[i % 3][1]) / 8 - 1;
        v[4 * i + 2] = primitive & 1; v[4 * i + 3] = 1;
    }
    id<MTLBuffer> indices = [device newBufferWithLength:1024 * 4 options:MTLResourceStorageModeShared];
    id<MTLBuffer> info = [device newBufferWithLength:2048 options:MTLResourceStorageModeShared];
    float ones[] = {1, 1, 1, 1};
    id<MTLBuffer> constants = [device newBufferWithBytes:ones length:sizeof ones options:MTLResourceStorageModeShared];
    layout pulling = {0}; pulling.s[0] = (stream){vertices.gpuAddress, 16, 0};
    pulling.attr[0] = 0x2e; // NVN RGBA32F, one float4 per vertex.
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:16 height:8 mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModeShared;
    id<MTLTexture> target = [device newTextureWithDescriptor:td];
    const unsigned counts[] = {1, 31, 32, 33, 63, 64, 65, 97};
    for (int depth = 0; depth < 2; depth++) {
        id<MTLFunction> fn = [library newFunctionWithName:depth ? @"dm_mesh_z" : @"dm_mesh"];
        MTLMeshRenderPipelineDescriptor *pd = [MTLMeshRenderPipelineDescriptor new];
        pd.meshFunction = fn; pd.fragmentFunction = [library newFunctionWithName:@"mesh_pixel"];
        pd.colorAttachments[0].pixelFormat = td.pixelFormat;
        id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithMeshDescriptor:pd options:0 reflection:nil error:&error];
        if (!pipeline) { fprintf(stderr, "%s\n", error.description.UTF8String); return 1; }
        id<MTLArgumentEncoder> va = [fn newArgumentEncoderWithBufferIndex:0], ga = [fn newArgumentEncoderWithBufferIndex:13];
        id<MTLBuffer> vb = [device newBufferWithLength:va.encodedLength options:MTLResourceStorageModeShared];
        id<MTLBuffer> gb = [device newBufferWithLength:ga.encodedLength options:MTLResourceStorageModeShared];
        [va setArgumentBuffer:vb offset:0]; [va setBuffer:constants offset:0 atIndex:0];
        [ga setArgumentBuffer:gb offset:0]; [ga setBuffer:info offset:0 atIndex:0];
        for (int indexed = 0; indexed < 3; indexed++) for (unsigned test = 0; test < sizeof counts / sizeof *counts; test++) {
            unsigned n = counts[test]; uint32_t *vi = info.contents;
            memset(info.contents, 0, info.length); vi[1] = 1; vi[4] = n;
            for (unsigned i = 0; i < 1024; i++) {
                if (indexed == 1) ((uint16_t *)indices.contents)[i] = (uint16_t)(i + 3);
                else ((uint32_t *)indices.contents)[i] = i + 3;
            }
            draw_args args = {indexed ? indices.gpuAddress : 0, indexed == 1 ? 2 : indexed == 2 ? 4 : 0, 0, n * 3, indexed ? -3 : 0, 7, 0};
            MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = target; pass.colorAttachments[0].loadAction = MTLLoadActionClear;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLCommandBuffer> command = [queue commandBuffer];
            id<MTLRenderCommandEncoder> en = [command renderCommandEncoderWithDescriptor:pass];
            [en setRenderPipelineState:pipeline];
            [en setMeshBuffer:vb offset:0 atIndex:0]; [en setMeshBuffer:gb offset:0 atIndex:13];
            [en setMeshBytes:&args length:sizeof args atIndex:12]; [en setMeshBytes:&pulling length:sizeof pulling atIndex:16];
            for (id<MTLBuffer> resource in @[vertices, indices, info, constants]) [en useResource:resource usage:MTLResourceUsageRead stages:MTLRenderStageMesh];
            [en drawMeshThreadgroups:MTLSizeMake((n + 31) / 32, 1, 1) threadsPerObjectThreadgroup:MTLSizeMake(1, 1, 1) threadsPerMeshThreadgroup:MTLSizeMake(96, 1, 1)];
            [en endEncoding]; [command commit]; [command waitUntilCompleted];
            if (command.error) { fprintf(stderr, "%s\n", command.error.description.UTF8String); return 1; }
            uint8_t pixels[8][16][4]; [target getBytes:pixels bytesPerRow:16 * 4 fromRegion:MTLRegionMake2D(0, 0, 16, 8) mipmapLevel:0];
            for (unsigned primitive = 0; primitive < 128; primitive++) {
                unsigned red = pixels[7 - primitive / 16][primitive % 16][0];
                int visible = primitive < n && (((primitive >> 5) + primitive) & 1);
                if ((red > 200) != visible) { fprintf(stderr, "PrimitiveId: %u triangles, primitive %u, index mode %d, depth %d: red=%u, expected visible=%d\n", n, primitive, indexed, depth, red, visible); return 1; }
            }
        }
    }
    puts("mesh PrimitiveId: 48 GPU cases passed"); return 0;
} }
