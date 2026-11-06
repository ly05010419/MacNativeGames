/* Projection lifetime follows pool pixels, not replaceable view descriptors. */
#import "../zeldaVR/gfx/vr_projection_graph.h"
#include "../zeldaVR/gfx/vr_projection.h"
#include "../zeldaVR/gfx/vr_totk_projection.h"
#include "../zeldaVR/gfx/vr_labo_frame.h"
#include "../zeldaVR/runtime/vr_view_math.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
int main(void){@autoreleasepool{
    NS_VALID_UNTIL_END_OF_SCOPE id pool=[NSObject new],other_pool=[NSObject new];
    const float left[4]={-.7f,.6f,.8f,-.8f},right[4]={-.6f,.7f,.8f,-.8f};float out[4];
    vr_projection_image l={pool,100,960,1080},r={pool,200,960,1080},post={pool,300,960,1080},aux={pool,400,960,1080};
    vr_projection_graph_camera(l,left);vr_projection_graph_camera(r,right);
    /* Real F2718 -> F2807 failure: a glyph-cache draw sampled the world,
       inherited its right-eye rays, and later got HUD mapping only in that
       eye. A sampled dependency alone does not own the target coordinates. */
    vr_projection_image glyph={pool,0x88ea00,113,72};
    int action=vr_totk_projection_color_action(15,0,0,0,0);
    if(action==VR_PROJECTION_INHERIT)vr_projection_graph_inherit(glyph,&r,1);
    assert(!vr_projection_graph_get(glyph,out));
    action=vr_totk_projection_color_action(15,0,0,0,1);
    if(action==VR_PROJECTION_COPY)vr_projection_graph_copy(glyph,r);
    assert(vr_projection_graph_get(glyph,out)&&!memcmp(out,right,sizeof out));
    vr_projection_graph_clear(glyph);
    assert(vr_projection_graph_get(l,out)&&!memcmp(out,left,sizeof out));
    assert(vr_projection_graph_get(r,out)&&!memcmp(out,right,sizeof out));
    /* Real failure states: F1836 P175995 only wrote depth (wm=0), while
       P176056/P176060 blended auxiliary images (blend=1) into the scene.
       Neither may replace the existing color image's coordinate owner. */
    assert(vr_projection_color_action(0,0,0,1,0)==VR_PROJECTION_NO_WRITE);
    assert(vr_projection_color_action(0xffffffffu,0,1,0,0)==VR_PROJECTION_NO_WRITE);
    assert(vr_projection_color_action(0xffffffffu,0,0,1,0)==VR_PROJECTION_CAMERA);
    assert(vr_projection_color_action(0xf,0,0,0,1)==VR_PROJECTION_COPY);
    assert(vr_projection_color_action(0xf,1,0,1,0)==VR_PROJECTION_NO_WRITE);
    /* Destroying/rebuilding an NVN/Metal texture descriptor keeps the pixels. */
    vr_projection_image new_view={pool,100,960,1080};assert(vr_projection_graph_get(new_view,out));
    vr_projection_image other={other_pool,100,960,1080};assert(!vr_projection_graph_get(other,out));
    vr_projection_image sources[]={l,aux};vr_projection_graph_inherit(post,sources,2);
    assert(vr_projection_graph_get(post,out)&&!memcmp(out,left,sizeof out));
    vr_projection_graph_clear(aux);vr_projection_graph_inherit(post,sources,2);assert(vr_projection_graph_get(post,out));
    sources[1]=r;vr_projection_graph_inherit(post,sources,2);assert(!vr_projection_graph_get(post,out));
    /* Verified image post passes take the main scene's grid, not bloom/history. */
    vr_projection_graph_copy(post,l);assert(vr_projection_graph_get(post,out)&&!memcmp(out,left,sizeof out));
    vr_projection_graph_inherit(post,sources,2);
    vr_projection_image next={pool,500,960,1080},conflict[]={post,l};
    vr_projection_graph_inherit(next,conflict,2);assert(!vr_projection_graph_get(next,out));
    vr_projection_graph_camera(next,right);assert(vr_projection_graph_get(next,out));
    /* Mips/layers have actual distinct byte offsets. Reusing the same bytes for
       another pixel grid invalidates the old interpretation, without a guess. */
    vr_projection_image layer={pool,600,960,1080},mip={pool,700,480,540},resized={pool,100,480,540};
    vr_projection_graph_camera(layer,right);vr_projection_graph_camera(mip,right);
    assert(!vr_projection_graph_get(resized,out));vr_projection_graph_camera(resized,right);
    assert(!vr_projection_graph_get(l,out));assert(vr_projection_graph_get(resized,out));
    vr_projection_graph_clear(resized);assert(!vr_projection_graph_get(resized,out));
    assert(vr_projection_graph_get(layer,out)&&vr_projection_graph_get(mip,out));
    __weak id released;
    @autoreleasepool{id temporary=[NSObject new];released=temporary;vr_projection_graph_camera((vr_projection_image){temporary,100,960,1080},left);}
    assert(!released); /* Metadata cannot retain pool allocations. */
    vr_projection_graph_reset();assert(!vr_projection_graph_get(r,out));
    /* Image post processing preserves the real source rotation as well as
       projection. Equal frusta from different views are still ambiguous. */
    const float identity[12]={1,0,0,0,0,1,0,0,0,0,1,0};
    const float yaw[12]={0,0,1,42,0,1,0,12,-1,0,0,-8};float rotation[9],delta[9];
    vr_projection_graph_camera_view(l,left,identity);vr_projection_graph_copy(post,l);
    assert(vr_projection_graph_rotation(post,rotation));
    assert(vr_view_reprojection(rotation,yaw,delta));
    float ray[3]={0,0,-1};vr_rotation_apply(delta,ray);assert(ray[0]==-1&&ray[1]==0&&ray[2]==0);
    vr_projection_graph_camera_view(r,left,yaw);sources[0]=l;sources[1]=r;
    vr_projection_graph_inherit(post,sources,2);assert(!vr_projection_graph_get(post,out));
    assert(!vr_projection_graph_rotation(post,rotation));
    /* Game movement / floating-origin translation does not change ray grids. */
    float moved[12];memcpy(moved,identity,sizeof moved);moved[3]=100;moved[7]=-50;
    vr_projection_graph_camera_view(r,left,moved);vr_projection_graph_inherit(post,sources,2);
    assert(vr_projection_graph_get(post,out)&&vr_projection_graph_rotation(post,rotation));
    assert(vr_view_reprojection(rotation,moved,delta));
    for(int i=0;i<9;i++)assert(delta[i]==(i%4==0?1:0));
    const float pitch[12]={1,0,0,0,0,0,-1,0,0,1,0,0};
    vr_view_turn(yaw,pitch,moved);assert(vr_view_reprojection(rotation,moved,delta));
    float direction[3]={.2f,.3f,-1},expected[3];
    for(int i=0;i<3;i++)expected[i]=vr_dot3(moved+4*i,direction);
    vr_rotation_apply(delta,direction);for(int i=0;i<3;i++)assert(fabsf(direction[i]-expected[i])<1e-6f);
    /* Nonidentity source: yawed game base is shared by both source and target.
       Only the new pitch remains. Reversing the matrix product or omitting
       the source transpose fails this independently specified ray result. */
    vr_projection_graph_camera_view(l,left,yaw);assert(vr_projection_graph_rotation(l,rotation));
    assert(vr_view_reprojection(rotation,moved,delta));
    float pitched[3]={.2f,.3f,-1};vr_rotation_apply(delta,pitched);
    assert(fabsf(pitched[0]-.2f)<1e-6f&&fabsf(pitched[1]-1)<1e-6f&&fabsf(pitched[2]-.3f)<1e-6f);
    float reflection[9]={-1,0,0,0,1,0,0,0,1};assert(!vr_view_reprojection(reflection,moved,delta));
    moved[0]=NAN;assert(!vr_view_reprojection(rotation,moved,delta));
    /* Actual failing GPU matrices -> scene -> tone -> FXAA -> one stereo slot. */
    float matrix[16]={.469956815f,0,0,0,0,.417739362f,-.193187460f,0,0,0,-1,-.2f,0,0,-1,0};
    vr_projection_image scenes[2]={{pool,0x276,960,1080},{pool,0x129,960,1080}};
    vr_projection_image tones[2]={{pool,0x9d2c00,960,1080},{pool,0xa21000,960,1080}};
    vr_labo_frame frame={0};
    for(int eye=0;eye<2;eye++){
        matrix[2]=eye?-.117512606f:-.367512614f;float rays[4];assert(vr_projection_fov(matrix,rays));
        vr_projection_graph_camera(scenes[eye],rays);vr_projection_graph_copy(tones[eye],scenes[eye]);
        float vp[4]={eye*960,0,960,1080};assert(vr_labo_frame_eye(&frame,2816,1920,1080,vp)==eye);
        assert(vr_projection_graph_get(tones[eye],frame.fov[eye]));frame.projected|=1u<<eye;
        float c[3],x[3],y[3];vr_projection_plane(frame.fov[eye],c,x,y);
        float uv=.5f*(1+matrix[0]*.2f-matrix[2]);assert(fabsf(c[0]+(uv-.5f)*x[0]-.2f)<1e-6f);
    }
    assert(frame.projected==3&&memcmp(frame.fov[0],frame.fov[1],sizeof frame.fov[0]));assert(vr_labo_frame_present(&frame,2816));
    /* Paused world -> menu scratch -> downsample/blur -> background. Metadata
       belongs to the actual pixels, survives a display, and rejects new grids. */
    vr_projection_stereo stereo={0},readback;
    memcpy(stereo.fov[0],left,sizeof left);memcpy(stereo.fov[1],right,sizeof right);
    for(int e=0;e<2;e++)for(int i=0;i<9;i++)stereo.rotation[e][i]=i%4==0;
    vr_projection_graph_stereo(l,&stereo);assert(vr_projection_graph_stereo_get(l,&readback));
    assert(!memcmp(&stereo,&readback,sizeof stereo));
    vr_projection_graph_copy(post,l);assert(vr_projection_graph_stereo_get(post,&readback));
    sources[0]=post;vr_projection_graph_inherit(aux,sources,1);assert(vr_projection_graph_stereo_get(aux,&readback));
    assert(!vr_projection_graph_stereo_get((vr_projection_image){other_pool,100,960,1080},&readback));
    assert(!vr_projection_graph_stereo_get((vr_projection_image){pool,100,480,540},&readback));
    vr_projection_graph_stereo(l,NULL);assert(!vr_projection_graph_stereo_get(l,&readback));
    assert(vr_projection_graph_stereo_get(post,&readback));
    vr_projection_graph_clear(post);assert(!vr_projection_graph_stereo_get(post,&readback));
    vr_projection_graph_camera(aux,left);assert(!vr_projection_graph_stereo_get(aux,&readback));
    puts("Pool storage/subresource/image-grid projection ownership passed");
}return 0;}
