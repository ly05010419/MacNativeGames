// Execute make_atomic_probe.py's translated shader and verify actual image writes.
// clang -fobjc-arc -framework Foundation -framework Metal atomic_probe.m -o atomic_probe
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

int main(int argc, const char **argv) {
    @autoreleasepool {
        if (argc < 2 || argc > 3) return 2;
        BOOL grid = argc == 3;
        BOOL gridY = grid && !strcmp(argv[2], "grid-y");
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) { fprintf(stderr, "No Metal device\n"); return 1; }
        NSError *error = nil;
        NSString *source = [NSString stringWithContentsOfFile:@(argv[1]) encoding:NSUTF8StringEncoding error:&error];
        MTLCompileOptions *options = [MTLCompileOptions new];
        options.languageVersion = MTLLanguageVersion3_1;
        id<MTLLibrary> library = source ? [device newLibraryWithSource:source options:options error:&error] : nil;
        id<MTLFunction> function = [library newFunctionWithName:@"main0"];
        id<MTLComputePipelineState> pipeline = function ? [device newComputePipelineStateWithFunction:function error:&error] : nil;
        if (!pipeline) { fprintf(stderr, "%s\n", error.description.UTF8String); return 1; }
        MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Uint width:2 height:2 mipmapped:NO];
        descriptor.storageMode = MTLStorageModeShared;
        descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsageShaderAtomic;
        id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
        if (!texture) return 1;
        uint32_t values[4] = {10, 20, 30, 40};
        [texture replaceRegion:MTLRegionMake2D(0, 0, 2, 2) mipmapLevel:0 withBytes:values bytesPerRow:8];
        id<MTLArgumentEncoder> arguments = [function newArgumentEncoderWithBufferIndex:0];
        id<MTLBuffer> buffer = [device newBufferWithLength:arguments.encodedLength options:MTLResourceStorageModeShared];
        [arguments setArgumentBuffer:buffer offset:0];
        id<MTLBuffer> constants = nil;
        if (grid) {
            constants = [device newBufferWithLength:65536 options:MTLResourceStorageModeShared];
            memset(constants.contents, 0, constants.length);
            // Distinct values detect accidental use of header y or the wrong grid lane.
            ((uint32_t *)constants.contents)[1] = 99;
            ((uint32_t *)constants.contents)[128] = 7;
            ((uint32_t *)constants.contents)[129] = 11;
            [arguments setBuffer:constants offset:0 atIndex:0];
        }
        [arguments setTexture:texture atIndex:grid ? 1 : 0];
        id<MTLCommandQueue> queue = [device newCommandQueue];
        id<MTLCommandBuffer> command = [queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
        [encoder setComputePipelineState:pipeline];
        [encoder setBuffer:buffer offset:0 atIndex:0];
        [encoder useResource:texture usage:MTLResourceUsageRead | MTLResourceUsageWrite];
        if (constants) [encoder useResource:constants usage:MTLResourceUsageRead];
        [encoder dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
        [encoder endEncoding];
        [command commit];
        [command waitUntilCompleted];
        if (command.status != MTLCommandBufferStatusCompleted) {
            fprintf(stderr, "%s\n", command.error.description.UTF8String); return 1;
        }
        [texture getBytes:values bytesPerRow:8 fromRegion:MTLRegionMake2D(0, 0, 2, 2) mipmapLevel:0];
        if (values[0] != (gridY ? 714 : grid ? 458 : 74) || values[1] != 20 || values[2] != 30 || values[3] != 40) {
            fprintf(stderr, "Unexpected pixels: %u %u %u %u\n", values[0], values[1], values[2], values[3]); return 1;
        }
        puts(gridY ? "NVN2 grid scalar GPU readback passed: c0+8 uses canonical dispatch Y count" : grid ? "NVN2 grid scalar GPU readback passed: c0+4 uses canonical dispatch X count" : "SURED GPU readback passed: 64 atomic increments; adjacent pixels unchanged");
    }
    return 0;
}
