/* PoC host for the OoT mesh-stage microbenchmark (fed302d7 scale) — measures mesh kernel GPU time
   for 32/64/128 threads per threadgroup and the SIMD-split read staging, on the local Apple GPU.
   Build: clang -O2 -framework Metal -framework Foundation bench.m -o bench
   Run:   ./bench bench.metal */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GROUPS 1117u          /* fed302d7's real grid: 1117 x 1 x 1 */
#define RUNS 16               /* median of 16 submits, as the 2026-11-07 meshbench */

static int cmp_d(const void*a,const void*b){double x=*(const double*)a,y=*(const double*)b;return x<y?-1:x>y?1:0;}

int main(int argc,const char**argv){@autoreleasepool{
 if(argc<2){fprintf(stderr,"usage: bench <bench.metal>\n");return 2;}
 id<MTLDevice> dev=MTLCreateSystemDefaultDevice(); if(!dev){fprintf(stderr,"no Metal device\n");return 1;}
 NSError*err=nil;
 NSString*src=[NSString stringWithContentsOfFile:@(argv[1]) encoding:NSUTF8StringEncoding error:&err];
 if(!src){fprintf(stderr,"read: %s\n",err.description.UTF8String);return 1;}
 id<MTLLibrary> lib=[dev newLibraryWithSource:src options:nil error:&err];
 if(!lib){fprintf(stderr,"compile: %s\n",err.description.UTF8String);return 1;}
 id<MTLCommandQueue> queue=[dev newCommandQueue];
 /* buffers: address table (GROUPS*32*80 word addresses), raw src memory, scratch slots, hdr, tgstage */
 const size_t nt=(size_t)GROUPS*32*80;
 id<MTLBuffer> addr=[dev newBufferWithLength:nt*8 options:MTLResourceStorageModeShared];
 uint64_t*a=addr.contents; uint64_t seed=0xfeed302d791a4ba4ULL;
 for(size_t i=0;i<nt;i++){seed^=seed<<13;seed^=seed>>7;seed^=seed<<17;a[i]=(seed%(1u<<20))*4;}
 const size_t srcwords=(size_t)1<<22; /* 16 MB raw memory */
 id<MTLBuffer> srcb=[dev newBufferWithLength:srcwords*4 options:MTLResourceStorageModeShared];
 memset(srcb.contents,0x5a,srcwords*4);
 const uint slots=64, slotwords=4096;
 id<MTLBuffer> slot=[dev newBufferWithLength:(8192u+slots*slotwords)*4 options:MTLResourceStorageModeShared];
 uint32_t*hdr=(uint32_t*)slot.contents; hdr[0]=0; hdr[1]=slots-1; hdr[2]=slotwords; memset(hdr+4,0,(slots)*4);
 id<MTLBuffer> tgstage=[dev newBufferWithLength:(size_t)128*80*4 options:MTLResourceStorageModeShared];
 /* dummy color target for the mesh pass */
 MTLTextureDescriptor* td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:64 height:64 mipmapped:NO];
 td.usage=MTLTextureUsageRenderTarget; id<MTLTexture> target=[dev newTextureWithDescriptor:td];
 struct Var{const char*fn;int threads;int split;};
 struct Var vars[]={{"mesh_t",32,0},{"mesh_t",64,0},{"mesh_t",128,0},{"mesh_s",64,2},{"mesh_s",128,4}};
 /* instantiate templates: Metal needs explicit symbols; compile source with -DTHREADS? No: templates
    instantiated below via C++-style names are not addressable from ObjC. Instead the .metal defines
    fixed entry points. We expect: t32,t64,t128,s64,s128. */
 const char*names[]={"t32","t64","t128","s64","s128"};
 int threads[]={32,64,128,64,128};
 printf("device: %s\nGROUPS=%u runs=%u\n",dev.name.UTF8String,GROUPS,RUNS);
 printf("%-6s %8s %8s\n","variant","threads","median ms");
 for(unsigned v=0;v<sizeof names/sizeof*names;v++){
  id<MTLFunction> fn=[lib newFunctionWithName:@(names[v])];
  id<MTLFunction> fragfn=[lib newFunctionWithName:@"frag_main"];
  if(!fn){fprintf(stderr,"missing kernel %s\n",names[v]);return 1;}
  MTLMeshRenderPipelineDescriptor* md=[MTLMeshRenderPipelineDescriptor new];
  md.label=@(names[v]);
  md.meshFunction=fn; md.fragmentFunction=fragfn;
  md.maxTotalThreadsPerMeshThreadgroup=threads[v];
  md.colorAttachments[0].pixelFormat=MTLPixelFormatBGRA8Unorm;
  id<MTLRenderPipelineState> ps=[dev newRenderPipelineStateWithMeshDescriptor:md options:MTLPipelineOptionNone reflection:nil error:&err];
  if(!ps){fprintf(stderr,"pipeline %s: %s\n",names[v],err.description.UTF8String);return 1;}
  double ts[RUNS];
  for(unsigned r=0;r<RUNS;r++){
   MTLRenderPassDescriptor* rp=[MTLRenderPassDescriptor renderPassDescriptor];
   rp.colorAttachments[0].texture=target; rp.colorAttachments[0].loadAction=MTLLoadActionDontCare; rp.colorAttachments[0].storeAction=MTLStoreActionDontCare;
   id<MTLCommandBuffer> cb=[queue commandBuffer];
   id<MTLRenderCommandEncoder> enc=[cb renderCommandEncoderWithDescriptor:rp];
   [enc setRenderPipelineState:ps];
   [enc setVertexBuffer:addr offset:0 atIndex:0]; /* buffers bound by index in kernel signature order */
   [enc setVertexBuffer:srcb offset:0 atIndex:1];
   [enc setVertexBuffer:slot offset:0 atIndex:2];
   [enc setVertexBuffer:slot offset:0 atIndex:3];
   [enc setVertexBuffer:tgstage offset:0 atIndex:4];
   [enc drawMeshThreadgroups:MTLSizeMake(GROUPS,1,1) threadsPerObjectThreadgroup:MTLSizeMake(1,1,1) threadsPerMeshThreadgroup:MTLSizeMake(threads[v],1,1)];
   [enc endEncoding];
   [cb commit]; [cb waitUntilCompleted];
   if(cb.status!=MTLCommandBufferStatusCompleted){fprintf(stderr,"gpu err %s\n",cb.error.description.UTF8String);return 1;}
   ts[r]=(cb.GPUEndTime-cb.GPUStartTime)*1000.0;
  }
  qsort(ts,RUNS,sizeof(double),cmp_d);
  printf("%-6s %8d %8.3f\n",names[v],threads[v],ts[RUNS/2]);
 }
 return 0;
}}
