#import "vr_projection_graph.h"
#include <string.h>
#include <stdio.h>
#include "../runtime/vr_view_math.h"
typedef struct {float fov[4],rotation[9];int oriented;} projection_view;
@interface VRProjectionGrid : NSObject
@property(nonatomic) uint32_t width,height;
@property(nonatomic) id projection;
@property(nonatomic) NSData *stereo;
@property(nonatomic) NSString *origin,*writer;
@end
@implementation VRProjectionGrid
@end
/* Replay-thread owned. The real pool allocation is weak: replacing a Metal
   texture or finalizing an NVN view does not destroy its pixels, but freeing
   the pool does. A reused address cannot inherit a dead allocation's metadata. */
static NSMapTable<id,NSMutableDictionary<NSNumber *,VRProjectionGrid *> *> *pools;
static uint64_t trace_frame,trace_pass;static int trace_on;
static int origins(void){static int on=-1;if(on<0)on=getenv("TOTK_VR_GRAPH_ORIGIN")!=NULL;return on;}
static NSString *location(vr_projection_image image){return [NSString stringWithFormat:@"pool=%p off=%llx grid=%ux%u",(__bridge void *)image.allocation,(unsigned long long)image.offset,image.width,image.height];}
static VRProjectionGrid *grid(vr_projection_image image){return [pools objectForKey:image.allocation][@(image.offset)];}
static void origin(vr_projection_image target,vr_projection_image source,const char *operation){
    if(!origins())return;VRProjectionGrid *out=grid(target),*in=grid(source);if(!out)return;
    out.writer=[NSString stringWithFormat:@"%s F%llu/P%llu from %@",operation,(unsigned long long)trace_frame,(unsigned long long)trace_pass,location(source)];
    out.origin=source.allocation?in.origin:[NSString stringWithFormat:@"camera F%llu/P%llu %@",(unsigned long long)trace_frame,(unsigned long long)trace_pass,location(target)];
}
void vr_projection_graph_trace(uint64_t frame,uint64_t pass,int enabled){trace_frame=frame;trace_pass=pass;trace_on=enabled;}
static void trace(const char *op,vr_projection_image image,id projection){
    if(!trace_on)return;
    float f[4]={0};if([projection isKindOfClass:NSData.class]&&[projection length]==sizeof(projection_view))memcpy(f,[projection bytes],sizeof f);
    fprintf(stderr,"[vr-provenance] F%llu P%llu %s pool=%p off=%llx grid=%ux%u state=%s rays=%.7f,%.7f,%.7f,%.7f\n",
        (unsigned long long)trace_frame,(unsigned long long)trace_pass,op,(__bridge void *)image.allocation,
        (unsigned long long)image.offset,image.width,image.height,projection==NSNull.null?"conflict":projection?"known":"none",f[0],f[1],f[2],f[3]);
}
static id value(vr_projection_image image){
    if(!image.allocation||!image.width||!image.height)return nil;
    VRProjectionGrid *grid=[pools objectForKey:image.allocation][@(image.offset)];
    return grid.width==image.width&&grid.height==image.height?grid.projection:nil;
}
static void assign(vr_projection_image image,id projection){
    if(!image.allocation||!image.width||!image.height)return;
    NSMutableDictionary *parts=[pools objectForKey:image.allocation];
    if(!projection){if(parts[@(image.offset)])trace("clear",image,nil);[parts removeObjectForKey:@(image.offset)];return;}
    if(!pools)pools=[[NSMapTable alloc]initWithKeyOptions:NSPointerFunctionsWeakMemory|NSPointerFunctionsObjectPointerPersonality valueOptions:NSPointerFunctionsStrongMemory capacity:32];
    if(!parts){parts=[NSMutableDictionary dictionary];[pools setObject:parts forKey:image.allocation];}
    VRProjectionGrid *grid=parts[@(image.offset)];
    if(!grid){grid=[VRProjectionGrid new];parts[@(image.offset)]=grid;}
    if(grid.width==image.width&&grid.height==image.height&&[grid.projection isEqual:projection])return;
    trace("write",image,projection);
    if(grid.width!=image.width||grid.height!=image.height)grid.stereo=nil;
    grid.width=image.width;grid.height=image.height;grid.projection=projection;
}
void vr_projection_graph_stereo(vr_projection_image image,const vr_projection_stereo *stereo){
    if(!image.allocation||!image.width||!image.height)return;
    VRProjectionGrid *grid=[pools objectForKey:image.allocation][@(image.offset)];
    if(!stereo){grid.stereo=nil;return;}
    if(!grid||grid.width!=image.width||grid.height!=image.height){
        assign(image,NSNull.null);grid=[pools objectForKey:image.allocation][@(image.offset)];
    }
    grid.stereo=[NSData dataWithBytes:stereo length:sizeof *stereo];
}
int vr_projection_graph_stereo_get(vr_projection_image image,vr_projection_stereo *stereo){
    VRProjectionGrid *grid=image.allocation?[pools objectForKey:image.allocation][@(image.offset)]:nil;
    if(grid.width!=image.width||grid.height!=image.height||grid.stereo.length!=sizeof *stereo)return 0;
    memcpy(stereo,grid.stereo.bytes,sizeof *stereo);return 1;
}
void vr_projection_graph_reset(void){[pools removeAllObjects];}
void vr_projection_graph_clear(vr_projection_image image){assign(image,nil);}
void vr_projection_graph_camera(vr_projection_image image,const float fov[4]){
    vr_projection_graph_camera_view(image,fov,NULL);
}
void vr_projection_graph_camera_view(vr_projection_image image,const float fov[4],const float view[12]){
    projection_view p={0};memcpy(p.fov,fov,sizeof p.fov);
    if(view){for(int i=0;i<3;i++)memcpy(p.rotation+3*i,view+4*i,3*sizeof(float));p.oriented=vr_rotation_valid(p.rotation);}
    id old=value(image);
    VRProjectionGrid *grid=[pools objectForKey:image.allocation][@(image.offset)];grid.stereo=nil;
    if([old isKindOfClass:NSData.class]&&[old length]==sizeof p&&!memcmp([old bytes],&p,sizeof p))return;
    assign(image,[NSData dataWithBytes:&p length:sizeof p]);
    origin(image,(vr_projection_image){0},"camera");
}
int vr_projection_graph_get(vr_projection_image image,float fov[4]){
    id data=value(image);trace("read",image,data);if(![data isKindOfClass:NSData.class]||[data length]!=sizeof(projection_view))return 0;
    if(trace_on&&origins()){VRProjectionGrid *g=grid(image);fprintf(stderr,"[vr-origin] F%llu P%llu %s root=%s writer=%s\n",(unsigned long long)trace_frame,(unsigned long long)trace_pass,location(image).UTF8String,g.origin.UTF8String,g.writer.UTF8String);}
    memcpy(fov,[data bytes],4*sizeof(float));return 1;
}
int vr_projection_graph_rotation(vr_projection_image image,float rotation[9]){
    id data=value(image);if(![data isKindOfClass:NSData.class]||[data length]!=sizeof(projection_view))return 0;
    const projection_view *p=[data bytes];if(!p->oriented)return 0;memcpy(rotation,p->rotation,sizeof p->rotation);return 1;
}
void vr_projection_graph_copy(vr_projection_image target,vr_projection_image coordinate_source){
    id p=value(coordinate_source);vr_projection_stereo stereo;int retained=vr_projection_graph_stereo_get(coordinate_source,&stereo);
    trace("copy-source",coordinate_source,p);assign(target,p);if(retained)vr_projection_graph_stereo(target,&stereo);
    origin(target,coordinate_source,"copy");
}
void vr_projection_graph_inherit(vr_projection_image target,const vr_projection_image *sources,size_t count){
    id projection=nil;
    for(size_t i=0;i<count;i++){
        id next=value(sources[i]);if(!next)continue;
        if(next==NSNull.null||(projection&&![projection isEqual:next])){projection=NSNull.null;break;}
        projection=next;
    }
    if(projection){assign(target,projection);if(projection!=NSNull.null)for(size_t i=0;i<count;i++)if([value(sources[i])isEqual:projection]){origin(target,sources[i],"inherit");break;}}
    vr_projection_stereo retained={0},next;int have=0,conflict=0;
    for(size_t i=0;i<count;i++)if(vr_projection_graph_stereo_get(sources[i],&next)){
        if(have&&memcmp(&retained,&next,sizeof next)){conflict=1;break;}
        retained=next;have=1;
    }
    if(have)vr_projection_graph_stereo(target,conflict?NULL:&retained);
}
