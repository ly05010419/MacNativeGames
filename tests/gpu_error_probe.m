#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdatomic.h>
#include <pthread.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#define MLOG(...) do { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)
#include "nvn_gpu_error.h"

#define CHECK(c) do { checks++; if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)
static NSError *err(NSString *domain, NSInteger code) {
    return [NSError errorWithDomain:domain code:code userInfo:@{NSLocalizedDescriptionKey:@"synthetic failure"}];
}
int main(int argc, char **argv) { @autoreleasepool {
    unsigned checks = 0;
    if (argc != 2) return 2;
    setenv("OOT_GPU_ERROR_LOG", argv[1], 1);
    CHECK(gpu_error_is_hang(err(MTL4CommandQueueErrorDomain, MTL4CommandQueueErrorTimeout)));
    CHECK(!gpu_error_is_hang(err(MTL4CommandQueueErrorDomain, MTL4CommandQueueErrorNotPermitted)));
    CHECK(!gpu_error_is_hang(err(MTL4CommandQueueErrorDomain, MTL4CommandQueueErrorOutOfMemory)));
    CHECK(gpu_error_is_hang(err(MTLCommandBufferErrorDomain, MTLCommandBufferErrorTimeout)));
    CHECK(!gpu_error_is_hang(err(@"unrelated", MTLCommandBufferErrorTimeout)));
    CHECK(!gpu_error_is_hang(nil));
    NSError *nested = [NSError errorWithDomain:MTL4CommandQueueErrorDomain code:MTL4CommandQueueErrorInternal
        userInfo:@{NSUnderlyingErrorKey:err(MTLCommandBufferErrorDomain, MTLCommandBufferErrorTimeout)}];
    CHECK(gpu_error_is_hang(nested));
    CHECK(gpu_error_is_hang([NSError errorWithDomain:@"IOGPU" code:0 userInfo:@{NSLocalizedDescriptionKey:@"InnocentVictim"}]));
    gpu_trace_pipeline(@"pipeline=m0123 task=taskA mesh=meshA fs=fsA");
    NVNGPUErrorTrace *first = gpu_trace_take(42, 9, 0, @"main-mid");
    for (unsigned i = 0; i < GPU_TRACE_PIPELINES + 7; i++) gpu_trace_pipeline([NSString stringWithFormat:@"pipeline=r%u", i]);
    NVNGPUErrorTrace *second = gpu_trace_take(42, 9, 1, @"main-final");
    CHECK(first->sequence != second->sequence); // Same frame/fence, distinct submissions.
    CHECK(first->pipelines.count == 1 && [first->pipelines[0] containsString:@"meshA"]);
    CHECK(second->binds == GPU_TRACE_PIPELINES + 7);
    CHECK(second->pipelines.count == GPU_TRACE_PIPELINES);
    CHECK([second->pipelines[0] isEqualToString:@"pipeline=r7"]);
    CHECK([second->pipelines.lastObject isEqualToString:@"pipeline=r102"]);
    CHECK(gpu_trace_take(43, 10, 0, @"main-final")->pipelines.count == 0);
    // Simulate late, out-of-order feedback after the next frame has begun.
    gpu_error_report(nested, second, 1, 2, @"second context");
    gpu_error_report(err(MTL4CommandQueueErrorDomain, 3), first, 0, 0, @"first context");
    gpu_compile_error(err(@"compiler", 1), @"pipeline=mcompile task=taskB mesh=meshB fs=fsB");
    NSString *text = [NSString stringWithContentsOfFile:@(argv[1]) encoding:NSUTF8StringEncoding error:nil];
    CHECK([text containsString:@"frame=42 fence=9 chunk=0 role=main-mid"]);
    CHECK([text containsString:@"frame=42 fence=9 chunk=1 role=main-final"]);
    CHECK([text containsString:@"omitted=7"]);
    CHECK([text containsString:@"pipeline=m0123 task=taskA mesh=meshA fs=fsA"]);
    CHECK([text containsString:@"[gpu-compile-error] pipeline=pipeline=mcompile"]);
    CHECK([text containsString:MTL4CommandQueueErrorDomain]);
    // Error storms remain bounded; the first failure is never overwritten.
    for (unsigned i = 0; i < 100; i++) gpu_error_report(nested, second, 1, 2, @"storm");
    text = [NSString stringWithContentsOfFile:@(argv[1]) encoding:NSUTF8StringEncoding error:nil];
    CHECK([text componentsSeparatedByString:@"[gpu-error]"].count == 9);
    CHECK([text containsString:@"first context"]);
    printf("gpu-error: %u checks passed\n", checks);
    return 0;
} }
