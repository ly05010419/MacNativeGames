#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
// COMMIT_OPTIONS
static void require(int ok, const char *why) { if (!ok) { fprintf(stderr, "FAIL: %s\n", why); exit(1); } }
static int alive(id __weak *buffer) { @autoreleasepool { return *buffer != nil; } }
int main(void) { @autoreleasepool {
    id<MTLDevice> device=MTLCreateSystemDefaultDevice();
    if (!device || ![device supportsFamily:MTLGPUFamilyMetal4]) return 77;
    NSError *error=nil;
    id<MTL4CommandQueue> queue=[device newMTL4CommandQueueWithDescriptor:[MTL4CommandQueueDescriptor new] error:&error];
    id<MTL4CommandAllocator> allocator=[device newCommandAllocator];
    id<MTLSharedEvent> event=[device newSharedEvent];
    id<MTLResidencySet> residency=[device newResidencySetWithDescriptor:[MTLResidencySetDescriptor new] error:&error];
    require(queue && allocator && event && residency, "Metal 4 setup");
    [queue addResidencySet:residency];
    id __weak weakBuffer=nil;
    TotkCommitOptions *options=[TotkCommitOptions new];
    __block unsigned checked=0;
    @autoreleasepool {
        id<MTLBuffer> buffer=[device newBufferWithLength:4u<<20 options:MTLResourceStorageModeShared];
        require(buffer!=nil, "readback allocation"); weakBuffer=buffer;
        [residency addAllocation:buffer]; [residency commit];
        id<MTL4CommandBuffer> command=[device newCommandBuffer];
        [command beginCommandBufferWithAllocator:allocator];
        id<MTL4ComputeCommandEncoder> encoder=[command computeCommandEncoder];
        [encoder fillBuffer:buffer range:NSMakeRange(0,4096) value:0x5a];
        [encoder endEncoding]; [command endCommandBuffer];
        id<MTL4CommandBuffer> commands[]={command}; [queue commit:commands count:1];
        [queue signalEvent:event value:1];
        require([event waitUntilSignaledValue:1 timeoutMS:10000], "GPU producer completes");
        void *owned=(void *)CFBridgingRetain(buffer);
        [options addFeedbackHandler:^(id<MTL4CommitFeedback> feedback) {
            (void)feedback;
            const uint8_t *bytes=((__bridge id<MTLBuffer>)owned).contents;
            for (unsigned i=0;i<4096;i++) require(bytes[i]==0x5a, "completion reads GPU-written bytes");
            checked++;
            CFRelease(owned);
        }];
        [residency removeAllocation:buffer]; [residency commit];
    }
    require(alive(&weakBuffer), "explicit completion ownership protects readback");
    // The caller's outer pool stays open, as on a long-lived completion/waiter thread.
    id<MTL4CommitFeedback> feedback=(id)[NSObject new]; // this handler only consumes its GPU-written readback
    for (MTL4CommitFeedbackHandler handler in options->handlers) handler(feedback);
    require(checked==1, "completion executes once");
    require(!alive(&weakBuffer), "completion's temporary receiver must not escape to caller pool");
    puts("PASS: GPU payload checked; completion readback destroyed before caller pool drains");
} }
