/* Production arena ownership and shader-cache field on real Metal allocations. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#pragma clang attribute push(__attribute__((objc_externally_retained)), apply_to = function)
#define MDIE(...) do { fprintf(stderr,__VA_ARGS__); fputc('\n',stderr); exit(1); } while(0)
#define MLOG(...) ((void)0)
static id<MTLDevice> g_dev;
static id<MTLResidencySet> residency;
static uint64_t g_done, g_pend_reuse_done, g_pend_arena_hazards;
static int g_npend, g_pend_kind[1];
static void *g_pend_src[1];
static uint64_t g_pend_lo[1];
static _Atomic int g_pend_arena_pin=1;
static pthread_mutex_t g_mu=PTHREAD_MUTEX_INITIALIZER;
static void res_add(id<MTLAllocation> a) {[residency addAllocation:a];}
static void res_remove(id<MTLAllocation> a) {[residency removeAllocation:a];}
// SHADER_FIELD
// ARENA_IMPLEMENTATION
static void require(int test,const char *why) {if(!test)MDIE("FAIL: %s",why);}
// Drain the weak enumeration immediately so the observer cannot pin buffers.
static NSUInteger live_buffers(NSHashTable *alive) {@autoreleasepool { return alive.allObjects.count; }}
int main(void) {@autoreleasepool {
    g_dev=MTLCreateSystemDefaultDevice();if(!g_dev||![g_dev supportsFamily:MTLGPUFamilyMetal4])return 77;
    NSError *error=nil;residency=[g_dev newResidencySetWithDescriptor:[MTLResidencySetDescriptor new] error:&error];
    id<MTL4CommandQueue> queue=[g_dev newMTL4CommandQueueWithDescriptor:[MTL4CommandQueueDescriptor new] error:&error];
    require(residency&&queue,"Metal 4 queue/residency");[queue addResidencySet:residency];
    id<MTL4CommandAllocator> allocator=[g_dev newCommandAllocator];id<MTLSharedEvent> event=[g_dev newSharedEvent];
    NSMutableArray<NvShader *> *shaders=[NSMutableArray new];NSHashTable *alive=[NSHashTable weakObjectsHashTable];
    enum {BURST=32};size_t block=4u<<20;
    @autoreleasepool {
        arena_begin();
        for(int i=0;i<BURST;i++) {
            __unsafe_unretained id<MTLBuffer> b;uint8_t *cpu;uint64_t gpu;
            size_t offset=arena_alloc_u(block,&b,&cpu,&gpu);
            memset(cpu,i+1,64);
            NvShader *sh=[NvShader new];sh->ab_buf=b;sh->ab_gpu=gpu;sh->ab_off=offset;sh->ab_cpu=cpu;sh->ab_arena=g_arena_epoch;
            [shaders addObject:sh];[alive addObject:b];require(shader_ab_current(sh),"same-submission cache valid");
        }
        [residency commit];
        id<MTL4CommandBuffer> cb=[g_dev newCommandBuffer];[cb beginCommandBufferWithAllocator:allocator];
        id<MTL4ComputeCommandEncoder> en=[cb computeCommandEncoder];
        for(int i=0;i<BURST;i++) {[en fillBuffer:shaders[i]->ab_buf range:NSMakeRange(0,64) value:(uint8_t)(i+91)];}
        [en endEncoding];[cb endCommandBuffer];id<MTL4CommandBuffer> buffers[]={cb};
        [queue commit:buffers count:1];[queue signalEvent:event value:1];
        arena_commit(1);arena_trim(STOR_IDLE_SUBMITS+2);
        require(live_buffers(alive)==BURST,"unretired GPU buffers retained");
        require([event waitUntilSignaledValue:1 timeoutMS:10000],"GPU completion");
        for(int i=0;i<BURST;i++)for(int j=0;j<64;j++)require(shaders[i]->ab_cpu[j]==(uint8_t)(i+91),"GPU-written bytes before retirement");
        g_done=1;arena_trim(STOR_IDLE_SUBMITS+2);
        require(live_buffers(alive)==BURST,"pending CPU cutoff retains allocations");
        arena_begin();
        for(NvShader *sh in shaders)require(!shader_ab_current(sh),"new submission rejects old cache even with unchanged pass");
        g_pend_reuse_done=1;arena_trim(STOR_IDLE_SUBMITS+2);[residency commit];
    }
    require(g_arenas.count==0&&g_arenas_released==BURST,"arena pool retired");
    // NSHashTable.count includes zeroed weak entries until they are scavenged.
    // Enumerating live objects measures actual owners rather than hash slots.
    NSUInteger live=live_buffers(alive);
    printf("arena retired: %llu bytes; surviving buffers: %lu; live shaders: %lu; Metal allocated: %lu\n",(unsigned long long)g_arena_bytes_released,(unsigned long)live,(unsigned long)shaders.count,(unsigned long)g_dev.currentAllocatedSize);
    require(live==0,"inactive shaders must not retain retired argument allocations");
    // Re-enter a later submission with the shaders still alive: stale non-owning pointers are never dereferenced.
    arena_begin();for(NvShader *sh in shaders)require(!shader_ab_current(sh),"freed old cache rejected");
    puts("PASS: 32 buffers/128 MiB deallocated, GPU/CPU retirement protected, cross-submission reuse rejected");
}}
#pragma clang attribute pop
