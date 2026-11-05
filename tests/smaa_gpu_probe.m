/* Synthetic pixels only. Exercise the production Metal 4 adapter, not a shader reimplementation. */
#import <Metal/Metal.h>
#include <TargetConditionals.h>
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <unistd.h>
static id<MTLDevice> g_dev;
static id<MTLResidencySet> residency;
static NSMutableArray *retired;
static void res_add(id<MTLAllocation> a) { [residency addAllocation:a]; }
static void defer_obj_release(id a) { [retired addObject:a]; }
#define MLOG(...) do { fprintf(stderr,__VA_ARGS__); fputc('\n',stderr); } while(0)
#include "../games/totk/third_party/d2r_gfx/gfx/nvn/totk_smaa.h"
#include "../games/totk/third_party/d2r_gfx/gfx/nvn/totk_fxaa_quality.h"

static unsigned cases;
static unsigned long render_case(id<MTL4CommandQueue> q,id<MTL4CommandAllocator> alloc,id<MTLSharedEvent> event,
                                int quality,int method,unsigned pattern,MTLPixelFormat format) {
        atomic_store(&g_smaa_quality,quality);
        unsigned w=pattern==2?192:128,h=96,slot=cases%3;
        MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:w height:h mipmapped:NO];
        d.storageMode=MTLStorageModeShared; d.usage=MTLTextureUsageShaderRead|MTLTextureUsageRenderTarget;
        id<MTLTexture> src=[g_dev newTextureWithDescriptor:d],dst=[g_dev newTextureWithDescriptor:d]; assert(src&&dst);
        unsigned char *pixels=malloc(w*h*4),*out=malloc(w*h*4);
        for(unsigned y=0;y<h;y++)for(unsigned x=0;x<w;x++) {
            unsigned v=pattern==0?96:pattern==1?(x>y*3/2+12?255:0):(x>y*3/2+12?115:100),p=(y*w+x)*4;
            pixels[p]=pixels[p+1]=pixels[p+2]=v;pixels[p+3]=(unsigned char)(128+y%128);
            if(pattern==3){   // Nearly equal Rec.709 luma, strong chromatic edge: preserve format handling and alpha.
                int red=x>y*3/2+12;
                pixels[p]=red?255:0;pixels[p+1]=red?0:76;pixels[p+2]=0;
                if(format==MTLPixelFormatBGRA8Unorm){pixels[p+2]=pixels[p];pixels[p]=0;}
            }
        }
        [src replaceRegion:MTLRegionMake2D(0,0,w,h) mipmapLevel:0 withBytes:pixels bytesPerRow:w*4];
        res_add(src);res_add(dst);assert(method==1?fxaa_quality_prepare(src,dst,slot):smaa_prepare(src,dst,slot));[residency commit];
        id<MTL4CommandBuffer> cb=[g_dev newCommandBuffer];[cb beginCommandBufferWithAllocator:alloc];
        assert(method==1?fxaa_quality_encode(cb,src,dst,slot):smaa_encode(cb,src,dst,slot));[cb endCommandBuffer];
        id<MTL4CommandBuffer> buffers[]={cb};[q commit:buffers count:1 options:[MTL4CommitOptions new]];
        uint64_t fence=++cases;[q signalEvent:event value:fence];
        for(unsigned n=0;event.signaledValue<fence&&n<10000;n++)usleep(1000);
        assert(event.signaledValue>=fence);
        [dst getBytes:out bytesPerRow:w*4 fromRegion:MTLRegionMake2D(0,0,w,h) mipmapLevel:0];
        unsigned long difference=0;
        for(unsigned p=0;p<w*h*4;p+=4) {
            assert(out[p+3]==pixels[p+3]);
            if(pattern!=3)assert(out[p]==out[p+1]&&out[p]==out[p+2]);
            for(unsigned c=0;c<3;c++){unsigned delta=(unsigned)abs((int)out[p+c]-pixels[p+c]);if(pattern==0)assert(delta<=1);difference+=delta;}
        }
        if(pattern==1)assert(difference>0);
        [alloc reset];free(pixels);free(out);return difference;
}
int main(void) { @autoreleasepool {
    g_dev=MTLCreateSystemDefaultDevice();
    if(!g_dev||![g_dev supportsFamily:MTLGPUFamilyMetal4]) { puts("SKIP: Metal 4 GPU unavailable"); return 77; }
    retired=[NSMutableArray new];
    NSError *err=nil; residency=[g_dev newResidencySetWithDescriptor:[MTLResidencySetDescriptor new] error:&err]; assert(residency);
    id<MTL4CommandQueue> q=[g_dev newMTL4CommandQueueWithDescriptor:[MTL4CommandQueueDescriptor new] error:&err]; assert(q);
    [q addResidencySet:residency];
    id<MTL4CommandAllocator> alloc=[g_dev newCommandAllocator];
    id<MTLSharedEvent> event=[g_dev newSharedEvent];
    const MTLPixelFormat formats[]={MTLPixelFormatRGBA8Unorm,MTLPixelFormatRGBA8Unorm_sRGB,MTLPixelFormatBGRA8Unorm};
    for(int quality=0;quality<4;quality++)for(unsigned pattern=0;pattern<4;pattern++)for(unsigned f=0;f<3;f++)
        render_case(q,alloc,event,quality,0,pattern,formats[f]);
    for(unsigned pattern=0;pattern<4;pattern++)for(unsigned f=0;f<3;f++)
        render_case(q,alloc,event,SMAA_ULTRA,1,pattern,formats[f]);
    SmaaParams fixed=smaa_config(SMAA_HIGH,0);assert(fixed.blend==1&&fabsf(fixed.threshold-.025f)<1e-6f);
    printf("PASS: production SMAA presets; %u GPU cases including fixed 400/100 and FXAA Quality, alpha and format/size changes\n",cases);
    return 0;
} }
