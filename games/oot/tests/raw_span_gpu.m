/* Synthetic mapping oracle for externally generated typed raw-load helpers.
   All actual physical reads become an address hash; the real resolver, cache,
   span guards and word results execute on the GPU. No game code is stored here. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdint.h>
#include <stdio.h>
#define REQ(x,...) do{if(!(x)){fprintf(stderr,__VA_ARGS__);fputc('\n',stderr);return 1;}}while(0)
typedef struct{uint64_t lo,hi,delta;} Range;
static uint64_t rnd(uint64_t*s){*s^=*s<<13;*s^=*s>>7;*s^=*s<<17;return *s;}
int main(int argc,const char**argv){@autoreleasepool{
 REQ(argc==2||argc==3,"shader directory [width]");id<MTLDevice>d=MTLCreateSystemDefaultDevice();NSError*error=nil;
 Range r[]={{0,0x1000,0xabcdef0123456789ULL},{0x10000,0x11000,0xfedcba9876543210ULL},
 {0xfffffff0ULL,0x100000000ULL,0x100000001ULL},{0x1fffff000ULL,0x200000000ULL,0x500000003ULL},
 {0x2fffff000ULL,0x300001000ULL,0x700000000ULL},{0x400000000ULL,0x400001000ULL,0x1000040ULL},
 {0x600000000ULL,0x700000000ULL,0x900000004ULL},{0x700001000ULL,0x900002000ULL,0xfffffffedcba0000ULL},
 {0xfffffffe00001000ULL,0xffffffff00002000ULL,0xdeadbeef12345678ULL},
 {0xffffffffffff0000ULL,0xffffffffffffffffULL,0x123456789abcdef0ULL}};
 const unsigned nr=sizeof(r)/sizeof(*r),threads=2048,steps=32,n=threads*steps,capacity=32;
 uint64_t table[capacity*4];memset(table,0,sizeof table);for(unsigned k=1;k<capacity;k++)table[k*4]=UINT64_MAX;
 for(unsigned k=0;k<nr;k++){table[(k+1)*4]=r[k].lo;table[(k+1)*4+1]=r[k].hi;table[(k+1)*4+2]=r[k].delta;}
 id<MTLBuffer>map=[d newBufferWithBytes:table length:sizeof table options:MTLResourceStorageModeShared];
 id<MTLBuffer>queries=[d newBufferWithLength:(size_t)n*8 options:MTLResourceStorageModeShared];
 id<MTLBuffer>out=[d newBufferWithLength:(size_t)n*16 options:MTLResourceStorageModeShared];uint64_t*q=queries.contents,seed=0x231dcbe96897a17ULL;
 for(unsigned t=0;t<threads;t++)for(unsigned k=0;k<steps;k++){
  Range x=r[(t+k/16)%nr];uint64_t v;
  switch(k%16){case 0:v=x.lo;break;case 1:v=x.hi-4;break;case 2:v=x.hi-8;break;case 3:v=x.hi-12;break;
   case 4:v=x.hi-16;break;case 5:v=x.hi;break;case 6:v=x.lo-4;break;case 7:v=x.lo+2;break;
   case 8:v=x.lo^(UINT64_C(0x100)<<32);break;case 9:v=UINT64_MAX-3;break;case 10:v=0xfffffffcULL;break;
   default:v=x.lo+rnd(&seed)%(x.hi-x.lo);break;}q[t*steps+k]=v;
 }
 BOOL negative=getenv("RAW_SPAN_NEGATIVE")&&atoi(getenv("RAW_SPAN_NEGATIVE"));
 NSString*source=[NSString stringWithContentsOfFile:[@(argv[1])stringByAppendingPathComponent:negative?@"edge-negative.metal":@"edge.metal"]encoding:NSUTF8StringEncoding error:&error];
 MTLCompileOptions*co=[MTLCompileOptions new];co.mathMode=MTLMathModeSafe;co.languageVersion=MTLLanguageVersion4_0;
 id<MTLLibrary>lib=[d newLibraryWithSource:source options:co error:&error];REQ(lib,"compile %s",error.description.UTF8String);
 id<MTLCommandQueue>queue=[d newCommandQueue];uint64_t checked=0;
 for(unsigned width=argc==3?atoi(argv[2]):3;width<=(argc==3?(unsigned)atoi(argv[2]):4);width++)for(unsigned span=0;span<2;span++){
  NSString*name=[NSString stringWithFormat:@"%@%u",span?@"span":@"scalar",width];
  id<MTLComputePipelineState>p=[d newComputePipelineStateWithFunction:[lib newFunctionWithName:name]error:&error];REQ(p,"pipeline %s",error.description.UTF8String);
  for(unsigned on=0;on<2;on++){
   ((uint32_t*)map.contents)[4]=capacity|(on?0x80000000u:0u);
   id<MTLCommandBuffer>cb=[queue commandBuffer];id<MTLComputeCommandEncoder>e=[cb computeCommandEncoder];
   [e setComputePipelineState:p];[e setBuffer:map offset:0 atIndex:0];[e setBuffer:queries offset:0 atIndex:1];[e setBuffer:out offset:0 atIndex:2];
   [e dispatchThreadgroups:MTLSizeMake(threads/32,1,1)threadsPerThreadgroup:MTLSizeMake(32,1,1)];[e endEncoding];[cb commit];[cb waitUntilCompleted];
   REQ(cb.status==MTLCommandBufferStatusCompleted,"GPU %s",cb.error.description.UTF8String);const uint32_t*z=out.contents;
   for(unsigned at=0;at<n;at++)for(unsigned w=0;w<width;w++){
    uint64_t a=q[at]+w*4;uint32_t expected=0;
    for(unsigned k=0;k<nr;k++)if(r[k].lo<=a&&a<r[k].hi){uint64_t host=a+r[k].delta;expected=(uint32_t)host^(uint32_t)(host>>32)^0xa5c39e17u;break;}
    REQ(z[at*width+w]==expected,"raw_span mismatch fn=%s cache=%u query=%u word=%u address=%llx got=%x expected=%x",name.UTF8String,on,at,w,a,z[at*width+w],expected);checked++;
   }
  }
 }
 printf("{\"probe\":\"raw_span\",\"status\":\"pass\",\"checked\":%llu,\"device\":\"%s\"}\n",checked,d.name.UTF8String);return 0;
}}
