#import "../games/worldend/runtime/gfx/nvn/mng_pipeline_archive.h"
int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc != 3) return 2;
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device || ![device supportsFamily:MTLGPUFamilyMetal4]) return 3;
        setenv("DM_PSO_ARCHIVE", argv[1], 1);
        BOOL verify = !strcmp(argv[2], "verify"), changed = !strcmp(argv[2], "changed");
        setenv(verify || changed ? "DM_PSO_REQUIRE_ARCHIVE" : "DM_PSO_ARCHIVE_BUILD", "1", 1);
        NSError *error = nil;
        id<MTLLibrary> library = [device newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\nvertex float4 v(uint id [[vertex_id]]) { return float4(id,0,0,1); }\nfragment float4 f() { return float4(1); }\nkernel void k(device uint* out [[buffer(0)]], uint id [[thread_position_in_grid]]) { out[id]=id; }" options:nil error:&error];
        if (!library) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 4; }
        __block _Atomic unsigned count = 0;
        dispatch_apply(8, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^(size_t i) {
            @autoreleasepool {
                NSError *failure = nil;
                MTLRenderPipelineDescriptor *descriptor = [MTLRenderPipelineDescriptor new];
                descriptor.vertexFunction = [library newFunctionWithName:@"v"];
                descriptor.fragmentFunction = [library newFunctionWithName:@"f"];
                descriptor.colorAttachments[0].pixelFormat = changed ? MTLPixelFormatRGBA32Float :
                    (i & 1 ? MTLPixelFormatRGBA8Unorm : MTLPixelFormatRGBA16Float);
                if (mng_render_pipeline(device, descriptor, &failure)) atomic_fetch_add(&count, 1);
                else if (!changed) fprintf(stderr, "%s\n", failure.localizedDescription.UTF8String);
                if (!changed && mng_compute_pipeline(device, [library newFunctionWithName:@"k"], &failure)) atomic_fetch_add(&count, 1);
            }
        });
        if (!mng_archive_flush(&error)) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 5; }
        printf("mng-archive: %u checks, %lu hits, %lu misses\n", count, mng_archive_hits, mng_archive_misses);
        return count == (changed ? 0 : 16) ? 0 : 6;
    }
}
