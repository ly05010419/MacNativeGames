#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
static id<MTLDevice> g_dev;
static id<MTLResidencySet> residency;
static void retire_idle_kick(void) {}
static void res_add(id<MTLResource> r) { [residency addAllocation:(id<MTLAllocation>)r]; }
static void res_remove(id<MTLResource> r) { [residency removeAllocation:(id<MTLAllocation>)r]; }
#include "../games/totk/third_party/d2r_gfx/gfx/nvn/nvn_heap_cache.h"
static void require(int ok,const char *why) { if(!ok){fprintf(stderr,"FAIL %s\n",why);exit(1);} }
static int alive(id __weak *p) { @autoreleasepool { return *p!=nil; } }
static void pixels(id<MTL4CommandQueue> q,id<MTLTexture> t,unsigned value) {
    id<MTLBuffer> src=[g_dev newBufferWithLength:65536 options:MTLResourceStorageModeShared];
    id<MTLBuffer> dst=[g_dev newBufferWithLength:65536 options:MTLResourceStorageModeShared];
    memset(src.contents,value,65536); memset(dst.contents,0,65536);
    [residency addAllocation:src];[residency addAllocation:dst];[residency commit];
    id<MTL4CommandAllocator> a=[g_dev newCommandAllocator];id<MTL4CommandBuffer> cb=[g_dev newCommandBuffer];
    [cb beginCommandBufferWithAllocator:a];id<MTL4ComputeCommandEncoder> e=[cb computeCommandEncoder];
    unsigned offset=0;
    for(unsigned layer=0;layer<2;layer++)for(unsigned level=0;level<3;level++) {
        unsigned w=64>>level,h=w,row=256;
        [e copyFromBuffer:src sourceOffset:offset sourceBytesPerRow:row sourceBytesPerImage:row*h sourceSize:MTLSizeMake(w,h,1)
                toTexture:t destinationSlice:layer destinationLevel:level destinationOrigin:MTLOriginMake(0,0,0)];
        [e barrierAfterEncoderStages:MTLStageBlit beforeEncoderStages:MTLStageBlit visibilityOptions:MTL4VisibilityOptionDevice];
        [e copyFromTexture:t sourceSlice:layer sourceLevel:level sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(w,h,1)
                  toBuffer:dst destinationOffset:offset destinationBytesPerRow:row destinationBytesPerImage:row*h];
        offset+=row*h;
    }
    [e endEncoding];[cb endCommandBuffer];id<MTL4CommandBuffer> cs[]={cb};[q commit:cs count:1];
    id<MTLSharedEvent> done=[g_dev newSharedEvent];[q signalEvent:done value:1];
    require([done waitUntilSignaledValue:1 timeoutMS:10000],"bounded GPU completion");
    offset=0;const uint8_t *p=dst.contents;
    for(unsigned layer=0;layer<2;layer++)for(unsigned level=0;level<3;level++) {
        unsigned w=64>>level;for(unsigned y=0;y<w;y++)for(unsigned x=0;x<w*4;x++)require(p[offset+y*256+x]==value,"all mip/layer GPU bytes");
        offset+=256*w;
    }
    [residency removeAllocation:src];[residency removeAllocation:dst];[residency commit];
}
int main(void) { @autoreleasepool {
    setenv("TOTK_HEAP_REUSE","1",1);g_dev=MTLCreateSystemDefaultDevice();
    if(!g_dev||![g_dev supportsFamily:MTLGPUFamilyMetal4])return 77;
    NSError *err=nil;residency=[g_dev newResidencySetWithDescriptor:[MTLResidencySetDescriptor new] error:&err];
    id<MTL4CommandQueue> q=[g_dev newMTL4CommandQueueWithDescriptor:[MTL4CommandQueueDescriptor new] error:&err];
    require(q&&residency,"Metal setup");[q addResidencySet:residency];
    MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:64 height:64 mipmapped:YES];
    d.textureType=MTLTextureType2DArray;d.arrayLength=2;d.mipmapLevelCount=3;d.storageMode=MTLStorageModePrivate;
    d.usage=MTLTextureUsageShaderRead|MTLTextureUsageRenderTarget|MTLTextureUsagePixelFormatView;
    __weak id weakHeap=nil;void *identity=NULL;id<MTLTexture> view=nil;
    @autoreleasepool {
        id<MTLTexture> t=rt_heap_texture(d);require(t!=nil,"initial placement");weakHeap=t.heap;identity=(__bridge void *)t.heap;
        pixels(q,t,0x39);
        view=[t newTextureViewWithPixelFormat:d.pixelFormat textureType:MTLTextureType2DArray levels:NSMakeRange(0,3) slices:NSMakeRange(0,2)];
        require(view!=nil,"parent view");
    }
    require(g_rt_heap_n==0,"a live view prevents recycling");
    @autoreleasepool { id<MTLTexture> second=rt_heap_texture(d);require((__bridge void *)second.heap!=identity,"cannot recycle a live parent");pixels(q,second,0x71); }
    view=nil;
    @autoreleasepool {
        id<MTLTexture> reused=rt_heap_texture(d);require(reused!=nil,"replacement placement");
        require(g_rt_heap_reused>0,"retired heap is reused");pixels(q,reused,0x8e);
    }
    rt_heap_trim_at(clock_gettime_nsec_np(CLOCK_UPTIME_RAW)+RT_HEAP_IDLE_NS+1,0);[residency commit];
    require(g_rt_heap_n==0&&g_rt_heap_bytes==0,"idle heap cache drains");
    require(!alive(&weakHeap),"trim actually destroys old allocation");
    MTLHeapDescriptor *hd=[MTLHeapDescriptor new];hd.type=MTLHeapTypePlacement;hd.storageMode=MTLStorageModePrivate;
    for(unsigned mode=0;mode<2;mode++) {
        hd.size=mode?5u<<20:65536;
        for(int i=0;i<65;i++) { @autoreleasepool {
            id<MTLHeap> h=[g_dev newHeapWithDescriptor:hd];require(h!=nil,"bounded heap allocation");
            res_add((id<MTLResource>)h);require(rt_heap_put(h),"cache accepts eligible heap");[residency commit];
            require(g_rt_heap_n<=RT_HEAP_SLOTS&&g_rt_heap_bytes<=RT_HEAP_CAP,"hard heap count/byte limits");
        } }
        require(mode?g_rt_heap_n<64:g_rt_heap_n==64,"count and byte budgets evict independently");
        rt_heap_trim_at(0,1);[residency commit];
    }
    hd.size=(uint64_t)RT_HEAP_CAP+65536;
    @autoreleasepool { id<MTLHeap> h=[g_dev newHeapWithDescriptor:hd];require(h!=nil,"oversize fixture");
        require(!rt_heap_put(h)&&g_rt_heap_n==0,"oversize heap is not retained"); }
    printf("PASS heap reuse: GPU bytes across 3 mips / 2 layers, live-view protection, idle destruction, 64-heap/256-MiB bounds\n");return 0;
} }
