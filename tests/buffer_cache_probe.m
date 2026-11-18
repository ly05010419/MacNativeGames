#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>
#include <stdlib.h>
static id<MTLDevice> g_dev;
#include "../games/totk/third_party/d2r_gfx/gfx/nvn/nvn_buffer_cache.h"
static void require(int ok,const char *why) { if(!ok){fprintf(stderr,"FAIL: %s\n",why);exit(1);} }
static int alive(id __weak *p) { @autoreleasepool { return *p!=nil; } }
static NSMutableArray *retired;
static uint64_t retired_fence;
static void retire(id<MTLBuffer> buffer,uint64_t fence) { [retired addObject:buffer];retired_fence=fence; }
int main(void) { @autoreleasepool {
    g_dev=MTLCreateSystemDefaultDevice();
    if(!g_dev||![g_dev supportsFamily:MTLGPUFamilyMetal4])return 77;
    NSError *error=nil;
    id<MTL4CommandQueue> queue=[g_dev newMTL4CommandQueueWithDescriptor:[MTL4CommandQueueDescriptor new] error:&error];
    id<MTL4CommandAllocator> allocator=[g_dev newCommandAllocator];
    id<MTLSharedEvent> gate=[g_dev newSharedEvent],done=[g_dev newSharedEvent];
    id<MTLResidencySet> residency=[g_dev newResidencySetWithDescriptor:[MTLResidencySetDescriptor new] error:&error];
    require(queue&&allocator&&gate&&done&&residency,"Metal 4 setup");[queue addResidencySet:residency];
    nvn_buffer_cache tables={.mutex=PTHREAD_MUTEX_INITIALIZER,.limit=2,.byte_limit=16u<<20};
    @autoreleasepool {
        id<MTLBuffer> first=nvn_buffer_cache_take(&tables,4096,65536,0,1);
        require(first!=nil,"first table");[residency addAllocation:first];[residency commit];
        id<MTL4CommandBuffer> command=[g_dev newCommandBuffer];[command beginCommandBufferWithAllocator:allocator];
        id<MTL4ComputeCommandEncoder> encoder=[command computeCommandEncoder];
        [encoder fillBuffer:first range:NSMakeRange(0,4096) value:0x39];[encoder endEncoding];[command endCommandBuffer];
        [queue waitForEvent:gate value:1];id<MTL4CommandBuffer> commands[]={command};[queue commit:commands count:1];[queue signalEvent:done value:1];
        nvn_buffer_cache_put(&tables,first,1,retire,1);
        id<MTLBuffer> second=nvn_buffer_cache_take(&tables,4096,65536,0,1);
        int protected=first!=second;
        // Always unblock the GPU before an expected negative-control failure.
        gate.signaledValue=1;require([done waitUntilSignaledValue:1 timeoutMS:10000],"bounded GPU completion");
        require(protected,"an in-flight table must not be reused");
        id<MTLBuffer> again=nvn_buffer_cache_take(&tables,4096,65536,1,1);
        require(again==first,"completed table is reused");
        const uint8_t *bytes=again.contents;for(int i=0;i<4096;i++)require(bytes[i]==0x39,"reused table preserves actual GPU output");
        [residency removeAllocation:first];[residency commit];
        nvn_buffer_cache_put(&tables,again,4,retire,1);
        nvn_buffer_cache_put(&tables,second,5,retire,1);
        id<MTLBuffer> third=nvn_buffer_cache_take(&tables,4096,65536,1,1);
        retired=[NSMutableArray array];nvn_buffer_cache_put(&tables,third,6,retire,1);
        require(retired.count==1&&retired_fence==4,"eviction preserves the old table's retirement fence");
        nvn_buffer_cache_clear(&tables,retire);[retired removeAllObjects];
    }
    nvn_buffer_cache readbacks={.mutex=PTHREAD_MUTEX_INITIALIZER,.limit=16,.byte_limit=16u<<20};
    id __weak oldest=nil,oversized=nil;
    for(unsigned i=0;i<17;i++) { @autoreleasepool {
        id<MTLBuffer> b=nvn_buffer_cache_take(&readbacks,(i+1)*65536,4096,0,1);
        require(b!=nil,"readback allocation");if(!i)oldest=b;
        nvn_buffer_cache_put(&readbacks,b,0,NULL,1);
    } }
    require(readbacks.count==16&&!alive(&oldest),"full readback cache evicts the oldest size");
    @autoreleasepool {
        id<MTLBuffer> large=nvn_buffer_cache_take(&readbacks,10u<<20,4096,0,1);
        nvn_buffer_cache_put(&readbacks,large,0,NULL,1);
    }
    require(readbacks.bytes<=16u<<20&&readbacks.count<=16,"readback byte and count bounds");
    @autoreleasepool {
        id<MTLBuffer> large=nvn_buffer_cache_take(&readbacks,17u<<20,4096,0,1);oversized=large;
        nvn_buffer_cache_put(&readbacks,large,0,NULL,1);
    }
    require(!alive(&oversized),"oversized readback is not cached");
    nvn_buffer_cache_clear(&readbacks,NULL);
    @autoreleasepool {
        id<MTLBuffer> a=nvn_buffer_cache_take(&readbacks,4097,4096,0,1);
        nvn_buffer_cache_put(&readbacks,a,0,NULL,1);
        id<MTLBuffer> b=nvn_buffer_cache_take(&readbacks,4097,4096,0,1);
        require(a==b&&b.length==8192,"readback size-class reuse");
        nvn_buffer_cache_put(&readbacks,b,0,NULL,1);
    }
    nvn_buffer_cache_clear(&readbacks,NULL);
    require(!nvn_buffer_cache_take(&readbacks,SIZE_MAX,4096,0,1),"size overflow rejected");
    puts("PASS: real GPU fence protects table reuse; eviction fences and 16-buffer/16-MiB readback bounds checked");
} }
