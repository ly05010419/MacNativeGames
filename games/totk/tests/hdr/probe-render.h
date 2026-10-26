/* Shared runner for the standalone presentation GPU probes. */
#include <stdio.h>
#include <stdlib.h>
static void check(int ok,const char *s){if(!ok){fprintf(stderr,"FAIL: %s\n",s);exit(1);}}
static NSData *render(id<MTLDevice> dev,id<MTLTexture> input,MTLPixelFormat format,int hdr,float headroom,BOOL hdrAvailable,NSString *sourceOverride){
 NSError *e=nil;MTLCompileOptions *co=[MTLCompileOptions new];co.mathMode=MTLMathModeSafe;
 id<MTLLibrary> lib=[dev newLibraryWithSource:(sourceOverride?:totk_hdr_present_source(format,hdrAvailable)) options:hdrAvailable?co:nil error:&e];
 if(!lib)fprintf(stderr,"%s\n",e.localizedDescription.UTF8String);check(lib!=nil,"production MSL compiles");
 MTLRenderPipelineDescriptor *pd=[MTLRenderPipelineDescriptor new];pd.vertexFunction=[lib newFunctionWithName:@"vs"];pd.fragmentFunction=[lib newFunctionWithName:hdr?@"fshdr":@"fs"];pd.colorAttachments[0].pixelFormat=format;
 id<MTLRenderPipelineState> ps=[dev newRenderPipelineStateWithDescriptor:pd error:&e];check(ps!=nil,"presentation PSO");
 MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:input.width height:1 mipmapped:NO];td.storageMode=MTLStorageModeShared;td.usage=MTLTextureUsageRenderTarget;
 id<MTLTexture> out=[dev newTextureWithDescriptor:td];check(out!=nil,"output allocation");
 id<MTLCommandQueue> q=[dev newCommandQueue];id<MTLCommandBuffer> cb=[q commandBuffer];MTLRenderPassDescriptor *rp=[MTLRenderPassDescriptor renderPassDescriptor];rp.colorAttachments[0].texture=out;rp.colorAttachments[0].loadAction=MTLLoadActionClear;rp.colorAttachments[0].storeAction=MTLStoreActionStore;
 id<MTLRenderCommandEncoder> en=[cb renderCommandEncoderWithDescriptor:rp];MTLSamplerDescriptor *sd=[MTLSamplerDescriptor new];sd.minFilter=sd.magFilter=MTLSamplerMinMagFilterNearest;
 float uv[12]={0,0,1,1,0.5f/input.width,0.5f,1-0.5f/input.width,0.5f,headroom,0,0,0};
 [en setRenderPipelineState:ps];[en setFragmentTexture:input atIndex:0];[en setFragmentSamplerState:[dev newSamplerStateWithDescriptor:sd] atIndex:0];[en setFragmentBytes:uv length:sizeof uv atIndex:0];[en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];[en endEncoding];[cb commit];[cb waitUntilCompleted];check(!cb.error,"GPU execution completes");
 size_t bpp=format==MTLPixelFormatRGBA16Float?8:4;NSMutableData *bytes=[NSMutableData dataWithLength:input.width*bpp];[out getBytes:bytes.mutableBytes bytesPerRow:input.width*bpp fromRegion:MTLRegionMake2D(0,0,input.width,1) mipmapLevel:0];return bytes;
}
