#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <assert.h>
#include <string.h>

/* Compare the actual GPU output of the overlapping fetch Metal accepts online
   with the narrowed layout accepted by its offline device compiler. */
int main(void) { @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) return 77;
    NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
        "struct In { float4 pos [[attribute(0)]]; float4 col [[attribute(4)]]; };\n"
        "struct Out { float4 pos [[position]]; float3 col; };\n"
        "vertex Out vs(In in [[stage_in]]) { return {float4(in.pos.xyz, 1), in.col.rgb}; }\n"
        "fragment float4 fs(Out in [[stage_in]]) { return float4(in.col, 1); }\n";
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
    assert(library);
    const __fp16 positions[9] = {-1,-1,0, 3,-1,0, -1,3,0};
    unsigned char colors[40] = {0};
    for (int i = 0; i < 3; i++) { colors[i * 12 + 9] = 64; colors[i * 12 + 10] = 128; colors[i * 12 + 11] = 192; }
    id<MTLBuffer> positionBuffer = [device newBufferWithBytes:positions length:sizeof positions options:MTLResourceStorageModeShared];
    id<MTLBuffer> colorBuffer = [device newBufferWithBytes:colors length:sizeof colors options:MTLResourceStorageModeShared];
    id<MTLCommandQueue> queue = [device newCommandQueue];
    unsigned char pixels[2][16 * 16 * 4];
    for (int narrowed = 0; narrowed < 2; narrowed++) {
        MTLVertexDescriptor *layout = [MTLVertexDescriptor vertexDescriptor];
        layout.attributes[0].format = narrowed ? MTLVertexFormatHalf3 : MTLVertexFormatHalf4;
        layout.attributes[0].bufferIndex = 0; layout.layouts[0].stride = 6;
        layout.attributes[4].format = narrowed ? MTLVertexFormatUChar3Normalized : MTLVertexFormatUChar4Normalized;
        layout.attributes[4].bufferIndex = 1; layout.attributes[4].offset = 9; layout.layouts[1].stride = 12;
        MTLRenderPipelineDescriptor *pipeline = [MTLRenderPipelineDescriptor new];
        pipeline.vertexFunction = [library newFunctionWithName:@"vs"];
        pipeline.fragmentFunction = [library newFunctionWithName:@"fs"];
        pipeline.vertexDescriptor = layout;
        pipeline.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        id<MTLRenderPipelineState> state = [device newRenderPipelineStateWithDescriptor:pipeline error:&error];
        if (!state) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 1; }
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:16 height:16 mipmapped:NO];
        td.storageMode = MTLStorageModeShared; td.usage = MTLTextureUsageRenderTarget;
        id<MTLTexture> texture = [device newTextureWithDescriptor:td];
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = texture;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLCommandBuffer> commands = [queue commandBuffer];
        id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
        [encoder setRenderPipelineState:state];
        [encoder setVertexBuffer:positionBuffer offset:0 atIndex:0];
        [encoder setVertexBuffer:colorBuffer offset:0 atIndex:1];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [encoder endEncoding]; [commands commit]; [commands waitUntilCompleted];
        assert(commands.status == MTLCommandBufferStatusCompleted);
        [texture getBytes:pixels[narrowed] bytesPerRow:16 * 4 fromRegion:MTLRegionMake2D(0, 0, 16, 16) mipmapLevel:0];
    }
    assert(!memcmp(pixels[0], pixels[1], sizeof pixels[0]));
    for (int i = 0; i < 16 * 16; i++) {
        assert(pixels[1][i * 4] == 64 && pixels[1][i * 4 + 1] == 128 && pixels[1][i * 4 + 2] == 192 && pixels[1][i * 4 + 3] == 255);
    }
    puts("vertex fetch GPU output matches (256 pixels, both overlapping layouts)");
    return 0;
} }
