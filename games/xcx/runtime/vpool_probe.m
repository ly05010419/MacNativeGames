/* Verify NVN physical/virtual pool aliasing through production NVN thunks, on CPU and on the Metal GPU. */
#import <Metal/Metal.h>
#include "runtime.h"
#include "../third_party/d2r_gfx/gfx/nvn/nvn_protos.h"
#include "../third_party/d2r_gfx/gfx/nvn/nvn_obj.h"
#include "../third_party/d2r_gfx/gfx/nvn/nvn_mtl.h"
#include "../third_party/d2r_gfx/gfx/nvn/totk_vpool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGE TOTK_VPOOL_PAGE
#define PAGES 4
static void *(*lookup)(const char *);
static void *fn(const char *name){void *f=lookup(name);if(!f)runtime_fail("NVN vpool probe lookup");return f;}
#define CALL(name,...) ((NVNT_##name)fn(#name))(__VA_ARGS__)
static int checks;
static void expect(int ok,const char *what){if(!ok)runtime_fail(what);checks++;}

static void pool_init(NVNmemoryPool *pool,void *storage,uint64_t size,int flags,int expect_ok){
 NVNmemoryPoolBuilder b;memset(&b,0,sizeof b);
 CALL(nvnMemoryPoolBuilderSetDefaults,&b);CALL(nvnMemoryPoolBuilderSetDevice,&b,NULL);
 CALL(nvnMemoryPoolBuilderSetStorage,&b,storage,size);CALL(nvnMemoryPoolBuilderSetFlags,&b,flags);
 expect(CALL(nvnMemoryPoolInitialize,pool,&b)==expect_ok,"memory pool initialize result");
}
static int map(NVNmemoryPool *v,const NVNmemoryPool *p,int64_t poff,int64_t voff,uint64_t n){
 totk_mapping_request r={p,poff,voff,n,1,0xdeadbeef};
 return CALL(nvnMemoryPoolMapVirtual,v,1,(const NVNmappingRequest *)(const void *)&r);
}
static uint8_t *mem(NVNmemoryPool *p){return ((pool_t *)(void *)p)->mem;}
static int all(const uint8_t *p,uint8_t v,size_t n){for(size_t i=0;i<n;i++)if(p[i]!=v)return 0;return 1;}

static void gpu_checks(NVNmemoryPool *v1,NVNmemoryPool *v2){
 id<MTLDevice> dev=MTLCreateSystemDefaultDevice();id<MTLCommandQueue> q=[dev newCommandQueue];
 void *b1=NULL,*b2=NULL;uint64_t o1=0,o2=0;
 expect(mtl_addr((uint64_t)(uintptr_t)(mem(v1)+PAGE),PAGE,&b1,&o1),"virtual pool 1 has zero-copy Metal buffer");
 expect(mtl_addr((uint64_t)(uintptr_t)(mem(v2)+PAGE),PAGE,&b2,&o2),"virtual pool 2 has zero-copy Metal buffer");
 id<MTLBuffer> buf1=(__bridge id<MTLBuffer>)b1,buf2=(__bridge id<MTLBuffer>)b2;
 id<MTLBuffer> scratch=[dev newBufferWithLength:PAGE options:MTLResourceStorageModeShared];
 // GPU writes through V1 page 1 (= P page 2); CPU reads through V2 page 2 (= P page 2).
 id<MTLCommandBuffer> cb=[q commandBuffer];id<MTLBlitCommandEncoder> e=[cb blitCommandEncoder];
 [e fillBuffer:buf1 range:NSMakeRange(o1,PAGE) value:0x5c];
 // GPU reads through V2 page 1 (= P page 1), last written by the CPU through V1 page 0.
 [e copyFromBuffer:buf2 sourceOffset:o2 toBuffer:scratch destinationOffset:0 size:PAGE];
 [e endEncoding];[cb commit];[cb waitUntilCompleted];
 expect(cb.status==MTLCommandBufferStatusCompleted,"GPU alias command buffer completed");
 expect(all(mem(v2)+2*PAGE,0x5c,PAGE),"GPU write through virtual pool 1 visible to CPU through virtual pool 2");
 expect(all(scratch.contents,0x3a,PAGE),"GPU read through virtual pool 2 sees CPU write through virtual pool 1");
}

void runtime_vpool_probe(void){
 lookup=runtime_gfx_override("nvnBootstrapLoader");if(!lookup)runtime_fail("bootstrap override");
 mtl_init();
 const uint64_t size=PAGES*PAGE;
 // Game storage like the observed pool: CPU/GPU no access, only 8 KiB aligned.
 uint8_t *raw=NULL;if(posix_memalign((void **)&raw,PAGE,size+PAGE))runtime_fail("probe storage");
 uint8_t *storage=raw+0x2000;memset(storage,0xee,size);
 NVNmemoryPool phys,v1,v2,bad;
 pool_init(&phys,storage,size,0x189,1);
 expect(mem(&phys)!=storage&&((uintptr_t)mem(&phys)%getpagesize())==0,"no-access physical pool gets aligned host pages");
 expect(CALL(nvnMemoryPoolMap,&phys)==NULL,"CPU_NO_ACCESS physical pool is not CPU mapped");
 pool_init(&v1,NULL,size,0x2a1,1);pool_init(&v2,NULL,size,0x2a1,1);
 expect(all(mem(&v1),0,size),"fresh virtual pool reads zero");
 // V1[0..3) <- P[1..4); V2[1..3) <- P[1..3)
 expect(map(&v1,&phys,PAGE,0,3*PAGE),"map virtual pool 1");
 expect(map(&v2,&phys,PAGE,PAGE,2*PAGE),"map virtual pool 2");
 memset(mem(&v1),0x3a,PAGE);
 expect(all(mem(&v2)+PAGE,0x3a,PAGE),"CPU write through V1 visible through V2");
 memset(mem(&v2)+2*PAGE,0x71,PAGE);
 expect(all(mem(&v1)+PAGE,0x71,PAGE),"CPU write through V2 visible through V1");
 expect(all(storage,0xee,size),"game storage of no-access physical pool untouched");
 gpu_checks(&v1,&v2);
 // Negative controls leave mappings intact.
 expect(!map(&v1,&phys,0x1000,0,PAGE),"reject unaligned physical offset");
 expect(!map(&v1,&phys,0,3*PAGE,2*PAGE),"reject virtual range overflow");
 expect(!map(&v1,&phys,3*PAGE,0,2*PAGE),"reject physical range overflow");
 expect(!map(&v1,&v2,0,0,PAGE),"reject virtual pool as physical source");
 expect(!map(&phys,&phys,0,0,PAGE),"reject physical pool as mapping target");
 expect(all(mem(&v1),0x3a,PAGE),"rejected requests did not change mappings");
 pool_init(&bad,raw+0x1000,size,0x104,0);expect(1,"reject CPU-visible unaligned physical pool");
 // Unmap V1 page 0: reads zero again, the physical page keeps its data for V2.
 expect(map(&v1,NULL,0,0,PAGE),"unmap virtual page");
 expect(all(mem(&v1),0,PAGE),"unmapped virtual page reads zero");
 expect(all(mem(&v2)+PAGE,0x3a,PAGE),"physical data survives unmap of another alias");
 CALL(nvnMemoryPoolFinalize,&v1);CALL(nvnMemoryPoolFinalize,&v2);CALL(nvnMemoryPoolFinalize,&phys);
 free(raw);
 printf("{\"status\":\"pass\",\"checks\":%d,\"request_bytes\":%zu,\"page_bytes\":%llu,\"scope\":\"NVNmappingRequest layout from runtime dump; CPU and Metal GPU aliasing between two virtual pools of one physical pool; unmap; 6 negative controls; no drawing\"}\n",checks,sizeof(totk_mapping_request),(unsigned long long)PAGE);
}
