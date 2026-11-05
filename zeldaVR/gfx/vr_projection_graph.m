#import "vr_projection_graph.h"
#include <string.h>
#include <stdio.h>
#include "../runtime/vr_view_math.h"
typedef struct {float fov[4],rotation[9];int oriented;} projection_view;
@interface VRProjectionGrid : NSObject
@property(nonatomic) uint32_t width,height;
@property(nonatomic) id projection;
@end
@implementation VRProjectionGrid
@end
/* Replay-thread owned. The real pool allocation is weak: replacing a Metal
   texture or finalizing an NVN view does not destroy its pixels, but freeing
   the pool does. A reused address cannot inherit a dead allocation's metadata. */
static NSMapTable<id,NSMutableDictionary<NSNumber *,VRProjectionGrid *> *> *pools;
static uint64_t trace_frame,trace_pass;static int trace_on;
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
    grid.width=image.width;grid.height=image.height;grid.projection=projection;
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
    if([old isKindOfClass:NSData.class]&&[old length]==sizeof p&&!memcmp([old bytes],&p,sizeof p))return;
    assign(image,[NSData dataWithBytes:&p length:sizeof p]);
}
int vr_projection_graph_get(vr_projection_image image,float fov[4]){
    id data=value(image);trace("read",image,data);if(![data isKindOfClass:NSData.class]||[data length]!=sizeof(projection_view))return 0;
    memcpy(fov,[data bytes],4*sizeof(float));return 1;
}
int vr_projection_graph_rotation(vr_projection_image image,float rotation[9]){
    id data=value(image);if(![data isKindOfClass:NSData.class]||[data length]!=sizeof(projection_view))return 0;
    const projection_view *p=[data bytes];if(!p->oriented)return 0;memcpy(rotation,p->rotation,sizeof p->rotation);return 1;
}
void vr_projection_graph_copy(vr_projection_image target,vr_projection_image coordinate_source){id p=value(coordinate_source);trace("copy-source",coordinate_source,p);assign(target,p);}
void vr_projection_graph_inherit(vr_projection_image target,const vr_projection_image *sources,size_t count){
    id projection=nil;
    for(size_t i=0;i<count;i++){
        id next=value(sources[i]);if(!next)continue;
        if(next==NSNull.null||(projection&&![projection isEqual:next])){projection=NSNull.null;break;}
        projection=next;
    }
    if(projection)assign(target,projection);
}
