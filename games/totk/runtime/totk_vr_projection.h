/* The real view-set setter receives the native source PerspectiveProjection
 * at x3. Prepare that owner only while copying the controlled gameplay camera
 * into a B2 eye; the renderer then builds its own matrices and camera UBOs. */
#pragma once
#include "../zeldaVR/gfx/totk_vr_link.h"
#include "../zeldaVR/gfx/vr_projection.h"
#include "../zeldaVR/runtime/vr_view_math.h"
static pthread_mutex_t totk_vr_projection_mu=PTHREAD_MUTEX_INITIALIZER;
__attribute__((used,noinline)) static void totk_vr_view_set(uint64_t views,uint32_t index,uint64_t matrix,uint64_t projection,uint64_t extra,uint64_t camera){
    typedef void (*setter)(uint64_t,uint32_t,uint64_t,uint64_t,uint64_t);
    setter set=(setter)(uintptr_t)g_vr_cam.callee;
    uint64_t vt=0;int owned=runtime_vr_camera_owned(camera,projection);
    uint64_t views_owner=__atomic_load_n(&g_vr_cam.main_vs,__ATOMIC_RELAXED);
    if(index==0&&owned&&(!views_owner||views_owner==views))__atomic_store_n(&g_vr_cam.cam,camera,__ATOMIC_RELAXED);
    unsigned phase=__atomic_load_n(&g_vr_b2.phase,__ATOMIC_RELAXED);
    if(!vr_active()||!g_vr_b2.on||phase>1||!owned||camera!=g_vr_cam.cam||!projection){
        set(views,index,matrix,projection,extra);return;
    }
    memcpy(&vt,(const void *)(uintptr_t)projection,8);
    if(vt!=(uintptr_t)diag_main->base+0x39023e0){set(views,index,matrix,projection,extra);return;}
    vr_views headset;float target[4];
    if(!vr_link_views(&headset)||!vr_eye_intrinsics_overscan(headset.fov[phase],1.06f,target)){
        set(views,index,matrix,projection,extra);return;
    }
    float values[7]={target[0],sinf(target[0]*.5f),cosf(target[0]*.5f),tanf(target[0]*.5f),target[1],target[2],target[3]},saved[7];
    uint8_t *owner=(uint8_t *)(uintptr_t)projection;
    pthread_mutex_lock(&totk_vr_projection_mu);
    memcpy(saved,owner+0xa0,sizeof saved);memcpy(owner+0xa0,values,sizeof values);owner[8]=1;
    set(views,index,matrix,projection,extra);
    /* Keep later game-owned writes, otherwise restore this temporary input.
       Dirty stays set: the cached matrix was rebuilt for the eye just copied. */
    if(!memcmp(owner+0xa0,values,sizeof values)){memcpy(owner+0xa0,saved,sizeof saved);owner[8]=1;}
    static unsigned logs[2];if(logs[phase]++<6)fprintf(stderr,
        "[vr-totk-projection] eye=%u view=%u native-camera=%#llx projection=%#llx vtable=main+39023e0 fovy=%.7f aspect=%.7f shift=%.7f,%.7f\n",
        phase,index,(unsigned long long)camera,(unsigned long long)projection,target[0],target[1],target[2],target[3]);
    pthread_mutex_unlock(&totk_vr_projection_mu);
}
void runtime_totk_projection_probe(void){
    if(!diag_main)runtime_fail("TotK projection probe has no original module");
    const uint64_t at=0x27d5438;uint32_t original;memcpy(&original,diag_main->base+at,4);
    if(original!=0xbd409800)runtime_fail("TotK projection calculator does not match 1.4.3");
    const float rays[2][4]={{-.942f,.698f,.768f,-.890f},{-.698f,.942f,.768f,-.890f}};
    for(int eye=0;eye<2;eye++){
        float target[4],actual[4],matrix[16]={0};
        if(!vr_eye_intrinsics_overscan(rays[eye],1.06f,target))runtime_fail("TotK probe calibration rejected");
        if(getenv("TOTK_PROBE_NEGATIVE"))target[2]=0;
        _Alignas(16) uint8_t owner[0xc0]={0};const float depth[2]={.1f,1000};memcpy(owner+0x98,depth,sizeof depth);
        float values[7]={target[0],sinf(target[0]*.5f),cosf(target[0]*.5f),tanf(target[0]*.5f),target[1],target[2],target[3]};
        memcpy(owner+0xa0,values,sizeof values);
        ((void (*)(const void *,float *))(diag_main->base+at))(owner,matrix);
        int ok=vr_projection_fov(matrix,actual);
        if(ok)for(int j=0;j<4;j++)if(fabsf(tanf(actual[j])/tanf(rays[eye][j])-1.06f)>1e-5f)ok=0;
        if(!ok)runtime_fail("TotK native asymmetric projection ray coverage mismatch");
    }
    fprintf(stdout,"{\"status\":\"pass\",\"probe\":\"totk-quest-fov\",\"scope\":\"original 1.4.3 PerspectiveProjection calculator, both asymmetric eyes; no game entry\"}\n");
}
