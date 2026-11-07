/* Original synthetic intervals; accepts externally generated edge0/edge1 Metal helpers.
   No game shaders or captured inputs are stored in the source tree.
   Run with a GameSlot and MTL_DEBUG_LAYER=1; SAME_HIGH_NEGATIVE=1 tests a deliberately broken upper-word guard. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define REQ(x,...) do{if(!(x)){fprintf(stderr,__VA_ARGS__);fputc('\n',stderr);return 1;}}while(0)
typedef struct{uint64_t lo,hi,delta;} Range;
static uint64_t random64(uint64_t*s){*s^=*s<<13;*s^=*s>>7;*s^=*s<<17;return *s;}
int main(int argc,const char**argv){@autoreleasepool{
 REQ(argc==2,"shader directory");id<MTLDevice>d=MTLCreateSystemDefaultDevice();NSError*error=nil;NSString*dir=@(argv[1]);int negative=getenv("SAME_HIGH_NEGATIVE")?atoi(getenv("SAME_HIGH_NEGATIVE")):0;
 Range r[]={ {0,0x1000,0xabcdef0123456789ULL},{0x10000,0x11000,0xfedcba9876543210ULL},{0xfffffff0ULL,0x100000000ULL,0x100000001ULL},{0x1fffff000ULL,0x200000000ULL,0x500000003ULL},{0x2fffff000ULL,0x300001000ULL,0x700000000ULL},{0x400000000ULL,0x400001000ULL,0x1000040ULL},{0x600000000ULL,0x700000000ULL,0x900000004ULL},{0x700001000ULL,0x900002000ULL,0xfffffffedcba0000ULL},{0xfffffffe00001000ULL,0xffffffff00002000ULL,0xdeadbeef12345678ULL},{0xffffffffffff0000ULL,0xffffffffffffffffULL,0x123456789abcdef0ULL}};
 const unsigned nr=sizeof(r)/sizeof(*r),threads=16384,steps=32,n=threads*steps,capacity=32;
 uint64_t table[capacity*4];memset(table,0,sizeof table);for(unsigned k=1;k<capacity;k++)table[k*4]=UINT64_MAX;
 for(unsigned k=0;k<nr;k++){table[(k+1)*4]=r[k].lo;table[(k+1)*4+1]=r[k].hi;table[(k+1)*4+2]=r[k].delta;}
 id<MTLBuffer>map=[d newBufferWithBytes:table length:sizeof table options:MTLResourceStorageModeShared];uint64_t pointers[8]={0};pointers[6]=map.gpuAddress;id<MTLBuffer>arg=[d newBufferWithBytes:pointers length:sizeof pointers options:MTLResourceStorageModeShared];
 id<MTLBuffer>queries=[d newBufferWithLength:(size_t)n*8 options:MTLResourceStorageModeShared],out=[d newBufferWithLength:(size_t)n*8 options:MTLResourceStorageModeShared];uint64_t*q=queries.contents;uint64_t seed=0x1859ae42bc910a17ULL;
 for(unsigned t=0;t<threads;t++)for(unsigned k=0;k<steps;k++){
  Range x=r[(t+k/8)%nr];uint64_t span=x.hi-x.lo,v;
  switch(k%8){case 0:v=x.lo;break;case 1:v=x.lo+(span>4?4:0);break;case 2:v=(x.lo+(span>4?4:0))^(UINT64_C(0x100)<<32);break;case 3:v=x.hi-1;break;case 4:v=x.hi;break;case 5:v=x.lo-1;break;case 6:v=x.lo+random64(&seed)%span;break;default:v=random64(&seed);break;}
  q[t*steps+k]=v;
 }
 MTLCompileOptions*co=[MTLCompileOptions new];co.mathMode=MTLMathModeSafe;co.languageVersion=MTLLanguageVersion4_0;id<MTLCommandQueue>queue=[d newCommandQueue];uint64_t checked=0,eligible=0;NSMutableArray*timings=[NSMutableArray array];
 for(unsigned mode=0;mode<2;mode++){
  NSString*file=mode?(negative?@"edge1-negative.metal":@"edge1.metal"):@"edge0.metal";NSString*s=[NSString stringWithContentsOfFile:[dir stringByAppendingPathComponent:file]encoding:NSUTF8StringEncoding error:&error];id<MTLLibrary>lib=[d newLibraryWithSource:s options:co error:&error];REQ(lib,"compile %s",error.description.UTF8String);id<MTLComputePipelineState>p=[d newComputePipelineStateWithFunction:[lib newFunctionWithName:@"edge_cases"]error:&error];REQ(p,"pipeline %s",error.description.UTF8String);
  for(unsigned on=0;on<2;on++){
   ((uint32_t*)map.contents)[4]=capacity|(on?0x80000000u:0u);id<MTLCommandBuffer>cb=[queue commandBuffer];id<MTLComputeCommandEncoder>e=[cb computeCommandEncoder];[e setComputePipelineState:p];[e setBuffer:arg offset:0 atIndex:0];[e setBuffer:queries offset:0 atIndex:1];[e setBuffer:out offset:0 atIndex:2];[e useResource:map usage:MTLResourceUsageRead];[e dispatchThreadgroups:MTLSizeMake(threads/32,1,1)threadsPerThreadgroup:MTLSizeMake(32,1,1)];[e endEncoding];[cb commit];[cb waitUntilCompleted];REQ(cb.status==MTLCommandBufferStatusCompleted,"GPU %s",cb.error.description.UTF8String);[timings addObject:@((cb.GPUEndTime-cb.GPUStartTime)*1000)];
   const uint32_t*z=out.contents;
   for(unsigned t=0;t<threads;t++){
    uint32_t flags=0;
    for(unsigned k=0;k<steps;k++){
     unsigned at=t*steps+k;uint64_t a=q[at];uint32_t expected=0;int found=-1;
     for(unsigned ri=0;ri<nr;ri++)if(r[ri].lo<=a&&a<r[ri].hi){found=(int)ri;break;}
     if(found>=0){Range x=r[found];uint64_t host=a+x.delta;expected=(uint32_t)host^(uint32_t)(host>>32)^0xa5c39e17u;if(on){int fast=(x.lo>>32)==((x.hi-1)>>32)&&(uint32_t)(x.hi-x.lo)!=0;flags=mode&&fast?3:1;eligible+=mode&&fast;}}
     REQ(z[at*2]==expected&&z[at*2+1]==flags,"typed resolver mismatch mode=%u on=%u thread=%u step=%u address=%llx value=%08x expected=%08x flags=%u expectedFlags=%u",mode,on,t,k,a,z[at*2],expected,z[at*2+1],flags);checked+=2;
    }
   }
  }
 }
 printf("{\"status\":\"pass\",\"checked\":%llu,\"eligible_queries\":%llu,\"device\":\"%s\"}\n",checked,eligible,d.name.UTF8String);return 0;
}}
