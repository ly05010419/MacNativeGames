#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#include <time.h>
#include "nvn_deferred_queue.h"
@implementation NvDeferredRelease @end
static uint64_t g_fence,g_done;
static id g_cb;
static _Atomic int g_q4_inflight,g_up_pending;
static pthread_mutex_t g_smu=PTHREAD_MUTEX_INITIALIZER,g_res_mu=PTHREAD_MUTEX_INITIALIZER,g_sync_replay_mu=PTHREAD_RECURSIVE_MUTEX_INITIALIZER;
static NSHashTable *g_res_objects;
static id<MTLResidencySet> residency;
static _Atomic unsigned drains;
static int g_upstream_negative;
static int serial_resource_held(id o){return 0;}
static int storage_resource_owned(id o){return 0;}
static void retired_resource_wait(id o){}
static void reup_forget(id o){}
static void res_remove(id<MTLResource> o){[residency removeAllocation:o];[g_res_objects removeObject:o];}
static void res_commit(void){[residency commit];}
static int rt_heap_on(void){return 0;}
static void rt_heap_trim_at(uint64_t n,int all){}
static int async_replay(void){return 0;}
static void replay_owner_in(void){} static void replay_owner_out(void){}   // hle_nvn.c: TOTK_STALL_SAMPLE_MS bookkeeping only
static void retire_idle_kick(void);
// REPLAY_LOCK
// DEFER_QUEUE
// DEFER_DRAIN
static void defer_drain(void){atomic_fetch_add(&drains,1);defer_drain_impl();}
// IDLE_WORKER
@interface RetiredParent:NSObject { @public id child; } @end
@implementation RetiredParent
- (void)dealloc { defer_obj_unref(child); }
@end
static void require(int ok,const char *why){if(!ok){fprintf(stderr,"FAIL %s\n",why);exit(1);}}
static int alive(id __weak *p){@autoreleasepool{return *p!=nil;}}
int main(void){@autoreleasepool{
    id<MTLDevice> device=MTLCreateSystemDefaultDevice();if(!device||![device supportsFamily:MTLGPUFamilyMetal4])return 77;
    NSError *error=nil;residency=[device newResidencySetWithDescriptor:[MTLResidencySetDescriptor new] error:&error];
    id<MTL4CommandQueue> queue=[device newMTL4CommandQueueWithDescriptor:[MTL4CommandQueueDescriptor new] error:&error];
    require(residency&&queue,"Metal setup");[queue addResidencySet:residency];g_res_objects=[NSHashTable weakObjectsHashTable];
    __weak id weakBuffer=nil;pthread_mutex_lock(&g_smu);
    @autoreleasepool{
        id<MTLBuffer> buffer=[device newBufferWithLength:4u<<20 options:MTLResourceStorageModeShared];weakBuffer=buffer;
        [residency addAllocation:buffer];[g_res_objects addObject:buffer];[residency commit];
        id<MTL4CommandAllocator> a=[device newCommandAllocator];id<MTL4CommandBuffer> cb=[device newCommandBuffer];
        [cb beginCommandBufferWithAllocator:a];id<MTL4ComputeCommandEncoder> e=[cb computeCommandEncoder];
        [e fillBuffer:buffer range:NSMakeRange(0,4096) value:0x43];[e endEncoding];[cb endCommandBuffer];
        id<MTL4CommandBuffer> cs[]={cb};[queue commit:cs count:1];id<MTLSharedEvent> done=[device newSharedEvent];[queue signalEvent:done value:1];
        require([done waitUntilSignaledValue:1 timeoutMS:10000],"GPU writes complete");
        const uint8_t *bytes=buffer.contents;for(int i=0;i<4096;i++)require(bytes[i]==0x43,"actual GPU output");
        g_fence=g_done=1;defer_obj_at(buffer,0,1);
    }
    for(int i=0;i<500&&!atomic_load(&drains);i++)usleep(2000);
    require(atomic_load(&drains)>0,"idle drain attempts while owner lock busy");
    usleep(10000);pthread_mutex_unlock(&g_smu);
    for(int i=0;i<500&&alive(&weakBuffer);i++)usleep(2000);
    require(!alive(&weakBuffer),"idle retry destroys buffer without another GPU submission");
    require(g_res_objects.count==0,"idle removal reaches residency commit");
    @autoreleasepool {
        RetiredParent *parent=[RetiredParent new]; parent->child=[device newBufferWithLength:1u<<20 options:MTLResourceStorageModeShared];
        weakBuffer=parent->child; defer_obj_at(parent,1,1);
    }
    for(int i=0;i<500&&alive(&weakBuffer);i++)usleep(2000);
    require(!alive(&weakBuffer),"idle destructors retire children at the completed fence");
    @autoreleasepool{
        id<MTLBuffer> buffer=[device newBufferWithLength:4096 options:MTLResourceStorageModeShared];weakBuffer=buffer;
        defer_obj_at(buffer,1,2);
    }
    usleep(30000);require(alive(&weakBuffer),"future fence stays owned");
    pthread_mutex_lock(&g_sync_replay_mu);g_fence=g_done=2;pthread_mutex_unlock(&g_sync_replay_mu);retire_idle_kick();
    for(int i=0;i<500&&alive(&weakBuffer);i++)usleep(2000);
    require(!alive(&weakBuffer),"completed future reference drains");
    puts("PASS idle retirement: real GPU bytes; busy-owner retry, residency commit, future fence protected");return 0;
}}
