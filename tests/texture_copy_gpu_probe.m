/* The test runner inserts the production copy_t2t_draw and format predicates. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <assert.h>
#include <math.h>
#include <unistd.h>
#include "nvn.h"
static id<MTLDevice> g_dev;
static id<MTL4CommandBuffer> g_cb;
static id<MTLResidencySet> residency;
static NSMutableArray *held;
static int g_pso_dirty, g_dss_dirty, marked;
typedef struct { uint64_t serial; } tex_t;
#define MLOG(...) ((void)0)
#define RED_ONCE(n,...) do { fprintf(stderr,__VA_ARGS__); fputc('\n',stderr); } while(0)
static void res_add(id<MTLAllocation> obj) { [residency addAllocation:obj]; }
static void defer_obj_release(id obj) { [held addObject:obj]; }
static id<MTLTexture> tex_upgrade_usage(const tex_t *dt,id<MTLTexture> t,MTLPixelFormat f,MTLTextureUsage u) {
    (void)dt;(void)f;(void)u; return t; /* Deliberately unavailable: test the failure return too. */
}
static size_t arena_alloc(size_t size,id<MTLBuffer> __strong *out) {
    *out=[g_dev newBufferWithLength:size options:MTLResourceStorageModeShared];res_add(*out);defer_obj_release(*out);return 0;
}
static void rp_end(void) {} static void blit_close(void) {} static void rs_sync(void) {}
static void pt_mark(id cb,char kind,id pass) {(void)cb;(void)kind;(void)pass;}
static void es_reset(void) {} static void args_dirty_all(void) {}
static void m4_end(id en) {[en endEncoding];}
static void gpu_owned_mark(uint64_t serial) {assert(serial==42);marked++;}
// PRODUCTION_COPY
static id<MTLTexture> texture(MTLPixelFormat format,int array,int mip,int render) {
    MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:16 height:12 mipmapped:mip];
    d.textureType=array?MTLTextureType2DArray:MTLTextureType2D;d.arrayLength=array?3:1;
    d.storageMode=MTLStorageModeShared;d.usage=MTLTextureUsageShaderRead|(render?MTLTextureUsageRenderTarget:0)|MTLTextureUsagePixelFormatView;
    id<MTLTexture> t=[g_dev newTextureWithDescriptor:d];assert(t);res_add(t);defer_obj_release(t);return t;
}
static double decode(double x) {return x<=.04045?x/12.92:pow((x+.055)/1.055,2.4);}
static double encode(double x) {return x<=.0031308?x*12.92:1.055*pow(x,1/2.4)-.055;}
static unsigned value(int x,int y,int layer,int c) {return c==3?255:(unsigned)(17+x*7+y*9+layer*23+c*19)%220;}
static double sample(int x,int y,int layer,int c,int srgb) {double v=value(x,y,layer,c)/255.;return srgb&&c<3?decode(v):v;}
static unsigned cases,channels;
static void check(id<MTL4CommandQueue> q,id<MTL4CommandAllocator> a,id<MTLSharedEvent> event,int linear,int array,int mip,int srgb,MTLPixelFormat format,int partial) {
    [a reset];g_cb=[g_dev newCommandBuffer];[g_cb beginCommandBufferWithAllocator:a];
    id<MTLTexture> s=texture(srgb?MTLPixelFormatRGBA8Unorm_sRGB:MTLPixelFormatRGBA8Unorm,array,mip,0),d=texture(format,array,mip,1);
    unsigned w=16>>mip,h=12>>mip,layers=array?3:1,bpp=format==MTLPixelFormatRGBA16Float?8:4;
    unsigned char input[16*12*4],output[16*12*8];
    for(unsigned l=0;l<layers;l++) {
        for(unsigned y=0;y<h;y++)for(unsigned x=0;x<w;x++)for(unsigned c=0;c<4;c++)input[(y*w+x)*4+c]=value(x,y,l,c);
        [s replaceRegion:MTLRegionMake2D(0,0,w,h) mipmapLevel:mip slice:l withBytes:input bytesPerRow:w*4 bytesPerImage:w*h*4];
        memset(output,0,sizeof output);
        [d replaceRegion:MTLRegionMake2D(0,0,w,h) mipmapLevel:mip slice:l withBytes:output bytesPerRow:w*bpp bytesPerImage:w*h*bpp];
    }
    NVNcopyRegion sr={1,1,0,(int)w-2,(int)h-2,array?2:1},dr={partial?2:0,partial?1:0,0,partial?(int)w-4:(int)w,partial?(int)h-2:(int)h,array?2:1};
    tex_t dt={42};int before=marked;
    assert(copy_t2t_draw(&dt,s,d,mip,array?1:0,mip,0,&sr,&dr,linear));assert(marked==before+1);
    [g_cb endCommandBuffer];[residency commit];id<MTL4CommandBuffer> buffers[]={g_cb};
    __block BOOL failed=NO;MTL4CommitOptions *options=[MTL4CommitOptions new];
    [options addFeedbackHandler:^(id<MTL4CommitFeedback> f){if(f.error){failed=YES;fprintf(stderr,"%s\n",f.error.localizedDescription.UTF8String);}}];
    [q commit:buffers count:1 options:options];uint64_t fence=++cases;[q signalEvent:event value:fence];
    for(unsigned n=0;event.signaledValue<fence&&n<10000;n++)usleep(1000);assert(event.signaledValue>=fence&&!failed);
    for(unsigned l=0;l<layers;l++) {
        [d getBytes:output bytesPerRow:w*bpp bytesPerImage:w*h*bpp fromRegion:MTLRegionMake2D(0,0,w,h) mipmapLevel:mip slice:l];
        for(unsigned y=0;y<h;y++)for(unsigned x=0;x<w;x++)for(unsigned c=0;c<4;c++) {
            double want=0;
            if(l<(unsigned)dr.depth && x>=(unsigned)dr.xoffset && x<(unsigned)(dr.xoffset+dr.width) && y>=(unsigned)dr.yoffset && y<(unsigned)(dr.yoffset+dr.height)) {
                double sx=sr.xoffset+(x+.5-dr.xoffset)*sr.width/dr.width,sy=sr.yoffset+(y+.5-dr.yoffset)*sr.height/dr.height;
                int layer=l+(array?1:0);
                if(!linear)want=sample(floor(sx),floor(sy),layer,c,srgb);
                else {int ix=floor(sx-.5),iy=floor(sy-.5);double fx=sx-.5-ix,fy=sy-.5-iy;
                    want=(1-fy)*((1-fx)*sample(ix,iy,layer,c,srgb)+fx*sample(ix+1,iy,layer,c,srgb))+fy*((1-fx)*sample(ix,iy+1,layer,c,srgb)+fx*sample(ix+1,iy+1,layer,c,srgb));}
                if(format==MTLPixelFormatRGBA8Unorm_sRGB&&c<3)want=encode(want);
            }
            unsigned channel=format==MTLPixelFormatBGRA8Unorm&&c<3?2-c:c;
            double got=bpp==8?((_Float16*)output)[(y*w+x)*4+c]:output[(y*w+x)*4+channel]/255.;
            if(fabs(got-want)>(bpp==8?.0015:2.1/255.)){fprintf(stderr,"case%u l%u %u,%u c%u got%f want%f\n",cases,l,x,y,c,got,want);abort();}channels++;
        }
    }
}
int main(void) {@autoreleasepool {
    g_dev=MTLCreateSystemDefaultDevice();if(!g_dev||![g_dev supportsFamily:MTLGPUFamilyMetal4])return 77;
    NSError *error=nil;held=[NSMutableArray new];residency=[g_dev newResidencySetWithDescriptor:[MTLResidencySetDescriptor new] error:&error];assert(residency);
    id<MTL4CommandQueue> q=[g_dev newMTL4CommandQueueWithDescriptor:[MTL4CommandQueueDescriptor new] error:&error];assert(q);[q addResidencySet:residency];
    id<MTL4CommandAllocator> a=[g_dev newCommandAllocator];id<MTLSharedEvent> e=[g_dev newSharedEvent];
    MTLPixelFormat formats[]={MTLPixelFormatRGBA8Unorm,MTLPixelFormatBGRA8Unorm,MTLPixelFormatRGBA8Unorm_sRGB,MTLPixelFormatRGBA16Float};
    for(int linear=0;linear<2;linear++)for(int i=0;i<4;i++)for(int part=0;part<2;part++)check(q,a,e,linear,i%2,i/2,part,formats[i],part);
    tex_t dt={42};NVNcopyRegion r={0,0,0,4,4,1};id<MTLTexture> normal=texture(MTLPixelFormatRGBA8Unorm,0,0,1);
    id<MTLTexture> integer=texture(MTLPixelFormatRGBA8Uint,0,0,1),noRT=texture(MTLPixelFormatRGBA8Unorm,0,0,0);
    assert(!copy_t2t_draw(&dt,integer,normal,0,0,0,0,&r,&r,0));assert(!copy_t2t_draw(&dt,normal,integer,0,0,0,0,&r,&r,0));
    assert(!copy_t2t_draw(&dt,normal,noRT,0,0,0,0,&r,&r,0));
    for(MTLPixelFormat f=MTLPixelFormatBC1_RGBA;f<=MTLPixelFormatBC7_RGBAUnorm_sRGB;f++)assert(t2t_fmt_block(f));
    assert(t2t_fmt_int_or_ds(MTLPixelFormatDepth32Float));
    printf("PASS: %u production texture-copy GPU cases, %u pixel channels; unsupported formats and missing target usage rejected\n",cases,channels);
}}
