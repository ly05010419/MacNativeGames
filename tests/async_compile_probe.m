#include "../games/totk/third_party/d2r_gfx/gfx/nvn/totk_compile_queue.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>

@interface CompileTestDevice : NSObject
@property NSUInteger maximumConcurrentCompilationTaskCount;
@end
@implementation CompileTestDevice
@end

typedef struct { atomic_int active, peak, done; } Counts;
int main(int argc, const char **argv) {
    @autoreleasepool {
        assert(argc == 2);
        if (!strcmp(argv[1], "metal")) {
            id<MTLDevice> device = MTLCreateSystemDefaultDevice();
            if (!device) { puts("skip: no Metal device in this environment"); return 77; }
            NSOperationQueue *queue = totk_compile_queue(device);
            assert(queue.maxConcurrentOperationCount >= 1);
            assert((NSUInteger)queue.maxConcurrentOperationCount <= MAX((NSUInteger)1, device.maximumConcurrentCompilationTaskCount));
            __block BOOL built = NO;
            totk_compile_submit(device, ^{
                NSError *error = nil;
                NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
                    "vertex float4 test_vertex(uint i [[vertex_id]]) { return float4(float(i), 0, 0, 1); }\n"
                    "fragment float4 test_fragment() { return float4(1, 0, 0, 1); }\n";
                id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
                assert(library && !error);
                MTLRenderPipelineDescriptor *descriptor = [MTLRenderPipelineDescriptor new];
                descriptor.vertexFunction = [library newFunctionWithName:@"test_vertex"];
                descriptor.fragmentFunction = [library newFunctionWithName:@"test_fragment"];
                descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
                id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
                assert(pipeline && !error);
                built = YES;
            });
            [queue waitUntilAllOperationsAreFinished];
            assert(built);
            puts("pass: background Metal library and pipeline compilation");
            return 0;
        }
        CompileTestDevice *fake = [CompileTestDevice new];
        fake.maximumConcurrentCompilationTaskCount = (NSUInteger)atoi(argv[1]);
        NSInteger limit = MAX((NSUInteger)1, fake.maximumConcurrentCompilationTaskCount);
        id<MTLDevice> device = (id<MTLDevice>)fake;
        NSOperationQueue *queue = totk_compile_queue(device);
        assert(queue.maxConcurrentOperationCount == limit);
        assert(queue.qualityOfService == NSQualityOfServiceDefault);
        assert(dispatch_queue_get_qos_class(queue.underlyingQueue, NULL) == QOS_CLASS_DEFAULT);
        dispatch_semaphore_t started = dispatch_semaphore_create(0), release = dispatch_semaphore_create(0);
        Counts *counts = calloc(1, sizeof *counts);
        for (int i = 0; i < 12; i++) totk_compile_submit(device, ^{
            int active = atomic_fetch_add(&counts->active, 1) + 1;
            int peak = atomic_load(&counts->peak);
            while (active > peak && !atomic_compare_exchange_weak(&counts->peak, &peak, active)) {}
            dispatch_semaphore_signal(started);
            assert(dispatch_semaphore_wait(release, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC)) == 0);
            atomic_fetch_sub(&counts->active, 1);
            atomic_fetch_add(&counts->done, 1);
        });
        // All 12 submissions returned while the first workers are still blocked.
        for (NSInteger i = 0; i < limit; i++)
            assert(dispatch_semaphore_wait(started, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC)) == 0);
        assert(atomic_load(&counts->active) == limit);
        assert(atomic_load(&counts->peak) == limit);
        for (int i = 0; i < 12; i++) dispatch_semaphore_signal(release);
        [queue waitUntilAllOperationsAreFinished];
        assert(atomic_load(&counts->done) == 12);
        assert(atomic_load(&counts->peak) <= limit);
        dispatch_group_t group = dispatch_group_create();
        dispatch_group_async(group, dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{
            totk_compile_from_translator(device, ^{ atomic_fetch_add(&counts->done, 1); });
            assert(atomic_load(&counts->done) == 13);
        });
        assert(dispatch_group_wait(group, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC)) == 0);
        free(counts);
        puts("pass: bounded default-QoS compiler queue and translator publication ordering");
    }
    return 0;
}
