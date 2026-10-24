/* Execute the owned game's compose shader with live HDR uniform off/on/off. */
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include "totk_hdr_compose.h"
#include <math.h>
static void check(int ok,const char *s){if(!ok){fprintf(stderr,"FAIL: %s\n",s);exit(1);}}
static NSData *draw(id<MTLDevice> d,id<MTLRenderPipelineState> p,id<MTLFunction> f,int modified,float enabled){
 enum{N=8};float levels[N]={.02,.18,.5,1,2,4,16,100};__fp16 pixels[N][4]={0};
 for(int i=0;i<N;i++){for(int c=0;c<3;c++)pixels[i][c]=levels[i];pixels[i][3]=1;}
 MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:N height:1 mipmapped:NO];td.storageMode=MTLStorageModeShared;td.usage=MTLTextureUsageShaderRead|MTLTextureUsageRenderTarget;
 id<MTLTexture> scene=[d newTextureWithDescriptor:td],bloom=[d newTextureWithDescriptor:td],out=[d newTextureWithDescriptor:td];
 [scene replaceRegion:MTLRegionMake2D(0,0,N,1) mipmapLevel:0 withBytes:pixels bytesPerRow:sizeof pixels];memset(pixels,0,sizeof pixels);[bloom replaceRegion:MTLRegionMake2D(0,0,N,1) mipmapLevel:0 withBytes:pixels bytesPerRow:sizeof pixels];
 id<MTLBuffer> c1=[d newBufferWithLength:65536 options:MTLResourceStorageModeShared],c3=[d newBufferWithLength:65536 options:MTLResourceStorageModeShared];
 float a[4]={.587,.114,1.f/6,0},b[4]={1,0,0,0};memcpy(c1.contents,a,sizeof a);memcpy(c3.contents,b,sizeof b);
 id<MTLArgumentEncoder> ae=[f newArgumentEncoderWithBufferIndex:0];id<MTLBuffer> args=[d newBufferWithLength:ae.encodedLength options:MTLResourceStorageModeShared];[ae setArgumentBuffer:args offset:0];[ae setBuffer:c1 offset:0 atIndex:0];[ae setBuffer:c3 offset:0 atIndex:1];[ae setTexture:scene atIndex:2];[ae setTexture:bloom atIndex:4];
 MTLSamplerDescriptor *sd=[MTLSamplerDescriptor new];sd.supportArgumentBuffers=YES;sd.minFilter=sd.magFilter=MTLSamplerMinMagFilterNearest;id<MTLSamplerState> sampler=[d newSamplerStateWithDescriptor:sd];[ae setSamplerState:sampler atIndex:3];[ae setSamplerState:sampler atIndex:5];
 MTLRenderPassDescriptor *rp=[MTLRenderPassDescriptor renderPassDescriptor];rp.colorAttachments[0].texture=out;rp.colorAttachments[0].loadAction=MTLLoadActionClear;rp.colorAttachments[0].storeAction=MTLStoreActionStore;
 id<MTLCommandQueue> q=[d newCommandQueue];id<MTLCommandBuffer> cb=[q commandBuffer];id<MTLRenderCommandEncoder> en=[cb renderCommandEncoderWithDescriptor:rp];[en setRenderPipelineState:p];[en setFragmentBuffer:args offset:0 atIndex:0];if(modified)[en setFragmentBytes:&enabled length:sizeof enabled atIndex:30];
 [en useResource:c1 usage:MTLResourceUsageRead stages:MTLRenderStageFragment];[en useResource:c3 usage:MTLResourceUsageRead stages:MTLRenderStageFragment];[en useResource:scene usage:MTLResourceUsageRead stages:MTLRenderStageFragment];[en useResource:bloom usage:MTLResourceUsageRead stages:MTLRenderStageFragment];[en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];[en endEncoding];[cb commit];[cb waitUntilCompleted];check(!cb.error,"compose GPU execution");
 NSMutableData *data=[NSMutableData dataWithLength:sizeof pixels];[out getBytes:data.mutableBytes bytesPerRow:sizeof pixels fromRegion:MTLRegionMake2D(0,0,N,1) mipmapLevel:0];return data;
}
int main(int argc,const char **argv){@autoreleasepool{
 check(argc==2,"provide owned compose MSL path");NSError *e=nil;NSString *original=[NSString stringWithContentsOfFile:@(argv[1]) encoding:NSUTF8StringEncoding error:&e];check(original!=nil,"read compose");NSString *modified=totk_hdr_compose_source(original);check(modified!=nil,"recognize compose source");check(totk_hdr_compose_source(@"unknown")==nil,"reject unknown source");check(totk_hdr_compose_source(modified)==nil,"reject duplicate patch");
 id<MTLDevice> d=MTLCreateSystemDefaultDevice();NSString *vs=@"#include <metal_stdlib>\nusing namespace metal;struct V{float4 p [[position]];float4 uv [[user(locn0)]];};vertex V vs(uint i [[vertex_id]]){float2 u=float2((i<<1)&2,i&2);return{float4(u*float2(2,-2)+float2(-1,1),0,1),float4(u,0,0)};}";
 id<MTLLibrary> vl=[d newLibraryWithSource:vs options:nil error:&e];check(vl!=nil,"vertex compile");NSData *reference=nil,*again=nil;float peak=0;
 for(int pass=0;pass<4;pass++){
  BOOL patched=pass>0;id<MTLLibrary> l=[d newLibraryWithSource:patched?modified:original options:nil error:&e];if(!l)fprintf(stderr,"%s\n",e.localizedDescription.UTF8String);check(l!=nil,"compose compile");id<MTLFunction> f=[l newFunctionWithName:@"main0"];
  MTLRenderPipelineDescriptor *pd=[MTLRenderPipelineDescriptor new];pd.vertexFunction=[vl newFunctionWithName:@"vs"];pd.fragmentFunction=f;pd.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA16Float;id<MTLRenderPipelineState> p=[d newRenderPipelineStateWithDescriptor:pd error:&e];check(p!=nil,"pipeline");
  NSData *data=draw(d,p,f,patched,pass==2?1:0);if(pass==0)reference=data;else if(pass!=2)check([data isEqualToData:reference],"disabled HDR is byte-identical to original SDR compose");else{const __fp16 *v=data.bytes;for(int i=0;i<32;i++)check(isfinite((float)v[i]),"finite pixels");peak=v[28];check(peak>1,"enabled HDR retains highlights");again=data;}
 }
 check(again!=nil,"off/on/off sequence completed");printf("{\"passed\":true,\"disabled_matches_original\":true,\"hdr_peak\":%.4f}\n",peak);
}}
