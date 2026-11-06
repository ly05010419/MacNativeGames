#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>
#define CHECK(v) do { if (!(v)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,error.localizedDescription.UTF8String); return 1; } } while(0)
int main(int argc,char **argv) { @autoreleasepool {
    NSError *error=nil; CHECK(argc==2); NSString *dir=@(argv[1]);
    NSArray *cases=[NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfFile:[dir stringByAppendingPathComponent:@"cases.json"]] options:0 error:&error]; CHECK(cases.count);
    NSString *source=[NSString stringWithContentsOfFile:[dir stringByAppendingPathComponent:@"probe.metal"] encoding:NSUTF8StringEncoding error:&error]; CHECK(source);
    id<MTLDevice> device=MTLCreateSystemDefaultDevice(); CHECK(device);
    MTLCompileOptions *options=[MTLCompileOptions new]; options.mathMode=MTLMathModeSafe;
    id<MTLLibrary> library=[device newLibraryWithSource:source options:options error:&error]; CHECK(library);
    id<MTLBuffer> output=[device newBufferWithLength:4096 options:MTLResourceStorageModeShared]; CHECK(output);
    MTLResidencySetDescriptor *rd=[MTLResidencySetDescriptor new]; rd.initialCapacity=1;
    id<MTLResidencySet> residency=[device newResidencySetWithDescriptor:rd error:&error]; CHECK(residency);
    [residency addAllocation:output]; [residency commit];
    id<MTL4CommandQueue> queue=[device newMTL4CommandQueueWithDescriptor:[MTL4CommandQueueDescriptor new] error:&error]; CHECK(queue); [queue addResidencySet:residency];
    id<MTL4CommandAllocator> allocator=[device newCommandAllocatorWithDescriptor:[MTL4CommandAllocatorDescriptor new] error:&error]; CHECK(allocator);
    MTL4ArgumentTableDescriptor *td=[MTL4ArgumentTableDescriptor new]; td.maxBufferBindCount=1;
    id<MTL4ArgumentTable> table=[device newArgumentTableWithDescriptor:td error:&error]; CHECK(table); [table setAddress:output.gpuAddress atIndex:0];
    for (NSDictionary *test in cases) {
        id<MTLFunction> function=[library newFunctionWithName:test[@"function"]]; CHECK(function);
        id<MTLComputePipelineState> pipeline=[device newComputePipelineStateWithFunction:function error:&error]; CHECK(pipeline);
        *(uint32_t *)output.contents=0xdeadbeef;
        [allocator reset]; id<MTL4CommandBuffer> command=[device newCommandBuffer]; [command beginCommandBufferWithAllocator:allocator];
        id<MTL4ComputeCommandEncoder> encoder=[command computeCommandEncoder]; [encoder setArgumentTable:table]; [encoder setComputePipelineState:pipeline];
        [encoder dispatchThreadgroups:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)]; [encoder endEncoding]; [command endCommandBuffer];
        dispatch_semaphore_t done=dispatch_semaphore_create(0); __block BOOL failed=NO;
        MTL4CommitOptions *co=[MTL4CommitOptions new]; [co addFeedbackHandler:^(id<MTL4CommitFeedback> feedback){ failed=feedback.error!=nil; dispatch_semaphore_signal(done); }];
        id<MTL4CommandBuffer> batch[]={command}; [queue commit:batch count:1 options:co];
        CHECK(!dispatch_semaphore_wait(done,dispatch_time(DISPATCH_TIME_NOW,30*NSEC_PER_SEC))&&!failed);
        uint32_t expected=[test[@"expected"] unsignedIntValue],got=*(uint32_t *)output.contents;
        if (getenv("OOT_STAGE_READ_NEGATIVE") && [test[@"mode"] intValue]==0xb8) expected^=1;
        if (got!=expected) { fprintf(stderr,"FAIL %s: got %08x expected %08x\n",[test[@"function"] UTF8String],got,expected); return 2; }
    }
    printf("PASS %lu Metal 4 ISBERD reads: input/output separation, workgroup, offsets and exact bit patterns\n",(unsigned long)cases.count); return 0;
} }
