#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdatomic.h>
#include <pthread.h>
static pthread_mutex_t g_smu=PTHREAD_MUTEX_INITIALIZER;
static int g_stor_of_obj;
static _Atomic uint64_t g_stat_views;
typedef struct {void *views;} stor_ent;
static void *nvn_hm_get(void *map,const void *key) { return NULL; }
static uint64_t obj_key(id o) { return (uint64_t)(uintptr_t)(__bridge void *)o; }
@interface NvPkg:NSObject { @public id<MTLTexture> tex;NSMutableDictionary *views;__unsafe_unretained id<MTLTexture> view_base; } @end
@implementation NvPkg @end
static NSMutableArray *retired;
static void defer_obj_unref(id o) { [retired addObject:o]; }
// PRODUCTION
static void require(int ok,const char *why) {if(!ok){fprintf(stderr,"FAIL %s\n",why);exit(1);}}
static int alive(id __weak *p) {@autoreleasepool{return *p!=nil;}}
int main(void) {@autoreleasepool{
    id<MTLDevice> device=MTLCreateSystemDefaultDevice();if(!device)return 77;
    retired=[NSMutableArray array];NvPkg *pkg=[NvPkg new];
    MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:64 height:64 mipmapped:YES];
    d.textureType=MTLTextureType2DArray;d.arrayLength=2;d.mipmapLevelCount=3;d.usage=MTLTextureUsageShaderRead|MTLTextureUsagePixelFormatView;
    pkg->tex=[device newTextureWithDescriptor:d];require(pkg->tex!=nil,"base allocation");
    uint64_t rid=0;__weak id weakView=nil;
    @autoreleasepool {
        for(int i=0;i<100;i++) {
            id<MTLTexture> v=stor_sampling_view(pkg->tex,d.pixelFormat,d.textureType,NSMakeRange(1,2),NSMakeRange(0,2),MTLTextureSwizzleChannelsDefault,pkg);
            require(v!=nil,"view allocation");if(!i){rid=v.gpuResourceID._impl;weakView=v;}
            require(v.gpuResourceID._impl==rid,"same base/layout returns same GPU view");
            require(v.width==32&&v.mipmapLevelCount==2&&v.arrayLength==2,"full mip/layer view layout");
        }
        require(g_stor_sampling_misses==1&&g_stor_sampling_hits==99,"one view per owned storage/layout");
        id<MTLTexture> other=stor_sampling_view(pkg->tex,d.pixelFormat,d.textureType,NSMakeRange(0,1),NSMakeRange(1,1),MTLTextureSwizzleChannelsDefault,pkg);
        require(other.gpuResourceID._impl!=rid&&other.width==64&&other.arrayLength==1,"different mip/layer key");
        MTLTextureSwizzleChannels sw=MTLTextureSwizzleChannelsMake(MTLTextureSwizzleBlue,MTLTextureSwizzleGreen,MTLTextureSwizzleRed,MTLTextureSwizzleAlpha);
        other=stor_sampling_view(pkg->tex,d.pixelFormat,d.textureType,NSMakeRange(1,2),NSMakeRange(0,2),sw,pkg);
        require(other.gpuResourceID._impl!=rid,"swizzle belongs to the cache key");
        pkg_views_drop(pkg);pkg->tex=[device newTextureWithDescriptor:d];
    }
    require(alive(&weakView),"retired views wait for their submission");
    [retired removeAllObjects];require(!alive(&weakView),"old views leave the owner after retirement");
    @autoreleasepool { id<MTLTexture> v=stor_sampling_view(pkg->tex,d.pixelFormat,d.textureType,NSMakeRange(1,2),NSMakeRange(0,2),MTLTextureSwizzleChannelsDefault,pkg);
        require(v.gpuResourceID._impl!=rid,"replacement storage has a new view identity");pkg_views_drop(pkg); }
    [retired removeAllObjects];printf("PASS package views: 100 requests / 1 creation; layout, swizzle and replacement lifetime checked\n");return 0;
}}
