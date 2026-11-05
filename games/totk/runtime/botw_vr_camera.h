/* Our head controller at the main CameraState LookAt update. Native graphics
 * builds and separates both render views afterwards. Projection provenance is
 * taken from each actual GPU camera block, never inferred from CameraState. */
#pragma once
#include "../zeldaVR/runtime/vr_camera.h"
#include "../zeldaVR/runtime/vr_view_math.h"
#include "../zeldaVR/gfx/totk_vr_link.h"
#include "../zeldaVR/gfx/vr_labo.h"
#include <pthread.h>

static vr_cam_state botw_vr_cam;
static _Atomic int botw_vr_distance;
static uint8_t *botw_vr_base;
static pthread_mutex_t botw_vr_mu=PTHREAD_MUTEX_INITIALIZER;
static uint64_t botw_vr_epoch;
static uint64_t botw_vr_control_epoch=UINT64_MAX;
static vr_cam_state botw_vr_control;
static float botw_vr_control_player[3];
/* The same native eye-separation owner consumed by the guarded load below. */
static float *botw_vr_native_half(void) {
#ifdef GAME_BOTW190
    const uint64_t slot=0x1e36008;
#else
    const uint64_t slot=0x2cc1578;
#endif
    uintptr_t pool=0,array=0,entry=0;uint32_t count=0;
    memcpy(&pool,botw_vr_base+slot,8);
    if(pool){memcpy(&count,(const void *)(pool+0x40),4);memcpy(&array,(const void *)(pool+0x48),8);}
    if(array&&count)memcpy(&entry,(const void *)(array+8*(count>1?1:0)),8);
    return entry?(float *)(entry+0x3f8):NULL;
}
int runtime_camera_distance_available(void){return botw_vr_base!=NULL;}
int runtime_camera_distance_cm(void){return atomic_load(&botw_vr_distance);}
void runtime_camera_distance_set(int cm){
    if(!botw_vr_base)return;
    if(cm < -500)cm=-500;if(cm>1000)cm=1000;atomic_store(&botw_vr_distance,cm);
    const char *path=getenv("TOTK_CAMERA_SETTINGS");FILE *f=path?fopen(path,"w"):NULL;
    if(f){fprintf(f,"%d\n",cm);fclose(f);}
}
static struct {
    uint64_t camera; int written;
    float original[9],owned[9];
    float base_view[12]; int base_valid;
} botw_vr_main_camera;
static uintptr_t botw_vr_selected(void) {
#ifdef GAME_BOTW190
    const uint64_t slot=0x1e39d10;
#else
    const uint64_t slot=0x2cc4da0;
#endif
    uintptr_t manager=0,p=0;memcpy(&manager,botw_vr_base+slot,8);if(!manager)return 0;
    uintptr_t selector=manager+0xf8;int32_t index=-1;
    memcpy(&index,(const void *)(selector+0xc),4);if(index<0)return 0;if(index>=2)index=0;
    memcpy(&p,(const void *)(selector+0x10+8*(uintptr_t)index),8);return p;
}
/* Destination view: the game's selected controller, before our head rotation.
   The source is a separately generated per-eye camera, read from actual GPU
   uniforms. Source and destination need not be the same C++ camera object. */
int runtime_vr_base_view(float out[12]) {
    if(!botw_vr_base)return 0;
    pthread_mutex_lock(&botw_vr_mu);
    uintptr_t camera=botw_vr_main_camera.camera;
    int valid=botw_vr_main_camera.base_valid&&camera&&camera==botw_vr_selected();
    if(valid)memcpy(out,botw_vr_main_camera.base_view,12*sizeof(float));
    pthread_mutex_unlock(&botw_vr_mu);return valid;
}
static void botw_vr_camera_update(uint8_t *camera,unsigned stage) {
    if(!botw_vr_base)return;
    if((uintptr_t)camera!=botw_vr_selected())return; /* Main source state, not an eye identifier. */
    pthread_mutex_lock(&botw_vr_mu);
    if(botw_vr_main_camera.camera!=(uintptr_t)camera){memset(&botw_vr_main_camera,0,sizeof botw_vr_main_camera);botw_vr_main_camera.camera=(uintptr_t)camera;}
    if(!stage){
        /* Restore only the exact values this adapter still owns. Game-written
           movement/cutscene parameters always take precedence. */
        if(botw_vr_main_camera.written){
            if(!memcmp(camera+0x38,botw_vr_main_camera.owned,36))memcpy(camera+0x38,botw_vr_main_camera.original,36);
            botw_vr_main_camera.written=0;
        }
        pthread_mutex_unlock(&botw_vr_mu);return;
    }
    if(botw_vr_control_epoch!=botw_vr_epoch){
        vr_camera_control_read(&botw_vr_control);botw_vr_control_epoch=botw_vr_epoch;
        memset(botw_vr_control_player,0,12);
        if(botw_vr_control.fp_pos){
            uintptr_t player=runtime_player_pos_address();vm_size_t copied=0;
            if(!player||vm_read_overwrite(mach_task_self(),(vm_address_t)player,12,(vm_address_t)botw_vr_control_player,&copied)!=KERN_SUCCESS||copied!=12)
                botw_vr_control.fp_pos=0;
        }
    }
    vr_cam_state control=botw_vr_control;
    if(!control.on||!vr_labo_active()){botw_vr_main_camera.base_valid=0;pthread_mutex_unlock(&botw_vr_mu);return;}
    float m[12],p[3];memcpy(m,camera+8,sizeof m);
    for(int i=0;i<12;i++)if(!isfinite(m[i])){pthread_mutex_unlock(&botw_vr_mu);return;}
    for(int i=0;i<12;i++)if(!isfinite(control.h[i])){pthread_mutex_unlock(&botw_vr_mu);return;}
    for(int i=0;i<3;i++)if(!isfinite(control.off[i])||!isfinite(control.fp_up[i])||!isfinite(botw_vr_control_player[i])){pthread_mutex_unlock(&botw_vr_mu);return;}
    if(!isfinite(control.cam_back)||!isfinite(control.fp_fwd)||!isfinite(control.fp_blend)){pthread_mutex_unlock(&botw_vr_mu);return;}
    memcpy(botw_vr_main_camera.original,camera+0x38,36);
    /* This is the source CameraState. Native rendering separates the eyes
       afterwards, along this final view's right axis: apply IPD there once. */
    if(control.level)vr_view_level(m);
    memcpy(botw_vr_main_camera.base_view,m,sizeof m);botw_vr_main_camera.base_valid=1;
    memcpy(botw_vr_cam.game_right,m,16);
    for(int i=0;i<3;i++)m[4*i+3]-=vr_dot3(m+4*i,control.off);
    m[11]-=control.cam_back+atomic_load(&botw_vr_distance)/100.0f;
    vr_view_turn(m,control.h,m);
    float player[3]={0};
    if(control.fp_pos){
        memcpy(player,botw_vr_control_player,12);
        vr_view_eye(m,p);
        for(int k=0;k<3;k++){float target=player[k]+control.fp_up[k]-control.fp_fwd*m[8+k];p[k]+=(target-p[k])*control.fp_blend;}
        vr_view_translation(m,p);
    }
    vr_view_eye(m,p);
    float d[3];for(int k=0;k<3;k++)d[k]=botw_vr_main_camera.original[k+3]-botw_vr_main_camera.original[k];
    float distance=sqrtf(vr_dot3(d,d));if(!(distance>0))distance=1;
    float pose[9];for(int k=0;k<3;k++){pose[k]=p[k];pose[k+3]=p[k]-m[8+k]*distance;pose[k+6]=m[4+k];}
    memcpy(camera+8,m,sizeof m);memcpy(camera+0x38,pose,sizeof pose);memcpy(botw_vr_main_camera.owned,pose,sizeof pose);
    botw_vr_main_camera.written=1;
    uint32_t calls=__atomic_load_n(&botw_vr_cam.calls,__ATOMIC_RELAXED)+1;
    memcpy(botw_vr_cam.ring[calls&15].m,m,sizeof m);botw_vr_cam.ring[calls&15].seq=control.seq;
    memcpy(botw_vr_cam.ring[calls&15].pos,player,12);
    __atomic_store_n(&botw_vr_cam.used,control.seq,__ATOMIC_RELEASE);
    __atomic_store_n(&botw_vr_cam.cam,(uintptr_t)camera,__ATOMIC_RELEASE);
    __atomic_store_n(&botw_vr_cam.calls,calls,__ATOMIC_RELEASE);
    if(calls<5)fprintf(stderr,"[vr] BotW controlled main camera at %p, head sequence %u, view t %.3f %.3f %.3f\n",(void *)camera,control.seq,m[3],m[7],m[11]);
    pthread_mutex_unlock(&botw_vr_mu);
}
static void botw_vr_camera_install(Module *m) {
    if(ios_code()||!vr_labo_enabled())return;
#ifdef GAME_BOTW190
    const struct {uint64_t off;uint32_t word;} sites[]={
        {0x10ec504,0xaa1303e1},{0x10ec508,0xaa1303e0},{0x10ec50c,0xf8408428},{0x10ec510,0xf9401108},
        {0x10ec514,0xd63f0100},{0x12610c8,0xbd43f900}};
#else
    const struct {uint64_t off;uint32_t word;} sites[]={
        {0x15c2048,0xf9400008},{0x15c204c,0xf9401108},{0x15c2050,0x91002001},{0x15c2054,0xaa0003f3},
        {0x15c2058,0xd63f0100},{0x17f0294,0xbd43f911}};
#endif
    for(size_t i=0;i<sizeof sites/sizeof*sites;i++){
        if(sites[i].off+4>m->text_end)runtime_fail("BotW VR camera requires the selected original game version");
        uint32_t have;memcpy(&have,m->base+sites[i].off,4);
        if(have!=sites[i].word)runtime_fail("BotW world-camera update does not match original instructions");
    }
    const uint64_t site=sites[4].off;
    const uint64_t stub=((m->text_end+15)&~15ull)+128;
    uint32_t code[]={0xa9be07e0,0xa9017be8,0xaa1303e0,0x52800001,0x58000190,0xd63f0200,0xa94007e0,0xf9400be8,
        0xd63f0100,0xaa1303e0,0x52800021,0x580000b0,0xd63f0200,0xf9400ffe,0x910083ff,0xd65f03c0,0,0};
    if(stub+sizeof code>((m->text_end+0xfff)&~0xfffull))runtime_fail("BotW VR camera veneer has no text padding");
    for(size_t i=0;i<sizeof code;i++)if(m->base[stub+i])runtime_fail("BotW VR camera veneer padding is occupied");
    uintptr_t host=(uintptr_t)runtime_hle_wrap_named((void *)botw_vr_camera_update,"our BotW world-camera controller");memcpy(code+16,&host,8);
    put_words(m,stub,code,sizeof code/4,"our BotW camera controller");
    put_word(m,site,branch_to(site,stub)|0x80000000u,"our BotW camera update call"); /* BL */
    botw_vr_base=m->base;
    const char *settings=getenv("TOTK_CAMERA_SETTINGS");FILE *f=settings?fopen(settings,"r"):NULL;
    if(f){int cm;if(fscanf(f,"%d",&cm)==1&&cm>=-500&&cm<=1000)atomic_store(&botw_vr_distance,cm);fclose(f);}
    g_vr_headlook=&botw_vr_cam;
    runtime_player_init(m);
    fprintf(stderr,"[vr] BotW: our camera controller installed at the native LookAt update; main-camera manager owns its identity\n");
}
