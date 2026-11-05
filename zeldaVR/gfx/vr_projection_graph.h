/* The pixel owner is NVN memory-pool storage, not a texture descriptor or a
 * replaceable Metal texture. One offset has one currently written pixel grid. */
#pragma once
#import <Foundation/Foundation.h>
#include <stdint.h>
typedef struct {
    __unsafe_unretained id allocation;
    uint64_t offset;
    uint32_t width,height;
} vr_projection_image;
typedef struct { float fov[2][4],rotation[2][9]; } vr_projection_stereo;
void vr_projection_graph_stereo(vr_projection_image target,const vr_projection_stereo *stereo);
int vr_projection_graph_stereo_get(vr_projection_image source,vr_projection_stereo *stereo);
void vr_projection_graph_reset(void);
void vr_projection_graph_trace(uint64_t frame,uint64_t pass,int enabled);
void vr_projection_graph_clear(vr_projection_image target);
void vr_projection_graph_camera(vr_projection_image target,const float fov[4]);
void vr_projection_graph_camera_view(vr_projection_image target,const float fov[4],const float view[12]);
int vr_projection_graph_get(vr_projection_image source,float fov[4]);
int vr_projection_graph_rotation(vr_projection_image source,float rotation[9]);
void vr_projection_graph_copy(vr_projection_image target,vr_projection_image coordinate_source);
void vr_projection_graph_inherit(vr_projection_image target,const vr_projection_image *sources,size_t count);
