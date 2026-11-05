/* Quest per-eye projection at the native renderer's actual view owner.
 * Never changes the shared CameraState or guesses a camera from its pose. */
#pragma once
#include "../zeldaVR/gfx/vr_projection.h"
#ifndef GAME_BOTW190
static void botw_vr_projection_prepare(uintptr_t holder) {
    if(!botw_vr_base||!holder||!vr_labo_active())return;
    uintptr_t camera=0,projection=0,vt=0;unsigned short flags=0;
    memcpy(&camera,(void *)(holder+0x80),8);memcpy(&projection,(void *)(holder+0x220),8);
    memcpy(&flags,(void *)(holder+0x90),2);
    /* The original native conversion owns this embedded PerspectiveProjection.
       Its source is the layer's input camera; bit 0x1000 selects the right eye
       in the original projection-offset and IPD calculations. */
    if(projection!=holder+0x288)return;
    memcpy(&vt,(void *)projection,8);unsigned eye=(flags&0x1000)!=0;
    uintptr_t expected=botw_vr_selected();
    int owned=expected&&camera==expected&&vt==(uintptr_t)botw_vr_base+0x2bb4d48;
    extern _Atomic unsigned g_vr_quest_trace;
    static unsigned epoch,trace[2];unsigned requested=atomic_load(&g_vr_quest_trace);
    if(epoch!=requested){epoch=requested;trace[0]=trace[1]=0;}

    if((owned||requested)&&trace[owned]++<16){float params[8];memcpy(params,(void *)(projection+0x98),sizeof params);
        fprintf(stderr,"[vr-quest-projection] owner %d eye %u holder %p camera %p expected %p projection %p vtable main+%llx params %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f\n",owned,eye,(void *)holder,(void *)camera,(void *)expected,(void *)projection,(unsigned long long)(vt-(uintptr_t)botw_vr_base),params[0],params[1],params[2],params[3],params[4],params[5],params[6],params[7]);}
    if(requested&&trace[owned]<=16){float matrix[16];memcpy(matrix,(void *)(projection+0xc),sizeof matrix);
        fprintf(stderr,"[vr-quest-projection] canonical P %.6f %.6f %.6f %.6f\n",matrix[0],matrix[2],matrix[5],matrix[6]);}
    if(!owned)return;
    extern _Atomic int g_vr_quest_fov_on;
    vr_views v;float target[4];if(!atomic_load(&g_vr_quest_fov_on)||!vr_link_views(&v)||!vr_eye_intrinsics_overscan(v.fov[eye],1.06f,target))return;
    float tangent=tanf(target[0]*.5f),values[7]={target[0],sinf(target[0]*.5f),cosf(target[0]*.5f),tangent,target[1],target[2],target[3]};
    if(memcmp((void *)(projection+0xa0),values,sizeof values)){
        memcpy((void *)(projection+0xa0),values,sizeof values);
        unsigned char dirty=1;memcpy((void *)(projection+8),&dirty,1);
    }
    static unsigned fitted;if(fitted++<4)fprintf(stderr,"[vr-quest-projection] applied eye %u FOV %.6f %.6f %.6f %.6f, fovy %.6f aspect %.6f shift %.6f %.6f\n",eye,v.fov[eye][0],v.fov[eye][1],v.fov[eye][2],v.fov[eye][3],target[0],target[1],target[2],target[3]);
}
#endif
static void botw_vr_projection_install(Module *m) {
#ifdef GAME_BOTW190
    (void)m;
#else
    if(ios_code()||!vr_labo_enabled())return;
    const struct {uint64_t off;uint32_t word;} quest_sites[]={
        {0x12bb1a8,0x910a22d8},{0x12bb2e4,0xbd0336c0},{0x12bb2e8,0x390a42d3},
        {0x12bb388,0xf90112d8},{0x12bb38c,0xf94042c9},{0x12bb460,0x794122c8},
        {0x12bb464,0x7214011f},{0x12bb4b4,0xd63f0100},{0x12bb510,0xf9000337},
        {0x1271860,0xbd40a000},{0x1271870,0xbd40b000},{0x1271880,0xb940b408},
        {0x12718b0,0xbd409801},{0x12718bc,0x2d558c02}};
    for(size_t i=0;i<sizeof quest_sites/sizeof*quest_sites;i++){
        uint32_t have=0;if(quest_sites[i].off+4<=m->text_end)memcpy(&have,m->base+quest_sites[i].off,4);
        if(have!=quest_sites[i].word)runtime_fail("BotW Quest projection owner/ABI does not match the original game");
    }
    const uint64_t site=0x12bb4b4,stub=((m->text_end+15)&~15ull)+0x120;
    uint32_t code[]={0xd10083ff,0xf9000ffe,0xd63f0100,0xf9000be0,0xaa1603e0,0xd503201f,0xd503201f,0x580000f0,
        0xd63f0200,0xf9400be0,0xf9400ffe,0x910083ff,0xd65f03c0,0xd503201f,0,0};
    if(stub+sizeof code>((m->text_end+0xfff)&~0xfffull))runtime_fail("BotW Quest projection veneer has no text padding");
    for(size_t i=0;i<sizeof code;i++)if(m->base[stub+i])runtime_fail("BotW Quest projection veneer overlaps another patch");
    uintptr_t host=(uintptr_t)runtime_hle_wrap_named((void *)botw_vr_projection_prepare,"BotW Quest per-eye projection");memcpy(code+14,&host,8);
    put_words(m,stub,code,sizeof code/4,"BotW Quest projection owner");
    put_word(m,site,branch_to(site,stub)|0x80000000u,"BotW Quest projection preparation");
    fprintf(stderr,"[vr] Quest projection hook at the final native per-eye projection conversion\n");
#endif
}

/* Runs the game's own projection calculation, after the loader seals text.
 * No constructors, game entry, GPU work or headset is needed. */
void runtime_botw_projection_probe(void) {
#ifdef GAME_BOTW190
    runtime_fail("Quest projection probe currently requires BotW 1.6.0");
#else
    if(!botw_vr_base)runtime_fail("Quest projection probe requires our VR runtime enabled");
    const float f[4]={-.942f,.698f,.768f,-.890f};float target[4],matrix[16]={0},actual[4];
    if(!vr_eye_intrinsics_overscan(f,1.06f,target))runtime_fail("Quest probe calibration rejected");
    if(getenv("TOTK_PROBE_NEGATIVE")){target[0]=.7853981634f;target[1]=.8888888889f;target[2]=-.0625f;target[3]=0;}
    _Alignas(16) unsigned char projection[0xc0]={0};const float depth[2]={.1f,1000};memcpy(projection+0x98,depth,sizeof depth);
    float values[7]={target[0],sinf(target[0]*.5f),cosf(target[0]*.5f),tanf(target[0]*.5f),target[1],target[2],target[3]};
    memcpy(projection+0xa0,values,sizeof values);
    void (*calculate)(const void *,float *)=(void *)((uintptr_t)botw_vr_base+0x12718b0);calculate(projection,matrix);
    int ok=vr_projection_fov(matrix,actual);
    if(ok)for(int i=0;i<4;i++)if(fabsf(tanf(actual[i])/tanf(f[i])-1.06f)>1e-5f)ok=0;
    if(!ok)runtime_fail("Quest projection ray coverage mismatch");
    fprintf(stdout,"{\"status\":\"pass\",\"probe\":\"quest-fov\",\"scope\":\"original game PerspectiveProjection code, four asymmetric rays and overscan; no game entry\"}\n");
#endif
}
