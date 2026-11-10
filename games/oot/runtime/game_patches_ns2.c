/* Switch 2 games (GAME_NS2 in runtime/game_version.h; first Donkey Kong Bananza, GAME_DKB, docs/ns2-port.md): the patch file
   the Makefile builds in place of TOTK's game_patches.c. Bananza's optional output size uses its native framebuffer
   getters and gsys creation parameters; frame pacing stays game-owned. Exclusive loads take acquire, as on
   Breath of the Wild and Super Mario Bros. Wonder (games/wonder/runtime/game_patches_wonder.c): sead's spin locks, which
   Bananza's engine (sead / agl / gsys) uses too, take the lock with a plain ldxr. */
#include "game_patches.h"
#include "game_version.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <math.h>
#include <TargetConditionals.h>
#include "ns2_resolution.h"
#include "bananza_display.h"
#include "pokopia_display.h"

#if defined(GAME_OOT) && !TARGET_OS_IPHONE
#include "oot_timestep.h"
#else
/* Other NS2 titles retain their native time-step policy. */
void runtime_frame_step_pin(double ms) { (void)ms; }
#endif
void runtime_frame_step_update(uint64_t now_ns) { (void)now_ns; }

#if defined(GAME_DKB) || defined(GAME_POKOPIA)
static pthread_once_t output_once=PTHREAD_ONCE_INIT;
static unsigned output_w,output_h;
static unsigned output_start_w,output_start_h;
static int output_has_size;
static _Atomic(uint64_t *) output_page;
static void output_initialize(void) {
    const char *e = getenv("TOTK_OUTPUT_RES");
#if TARGET_OS_IPHONE
    if(!e||!*e)e="1920x1080";
#endif
#if !TARGET_OS_IPHONE
    char saved[32];
    if (!e || !*e) {
        if(runtime_setting_word("graphics-output-size.txt",saved,sizeof saved)){
            if(ns2_parse_saved_resolution(saved,&output_w,&output_h)){saved[strcspn(saved,"\r\n")]=0;e=saved;}
            else
            fprintf(stderr,"[totk] invalid saved render resolution; using 1920x1080\n");
        }
        if(!e||!*e)e="1920x1080";
    }
#endif
    if (!strcmp(e, "0")) {
#ifdef GAME_DKB
        output_start_w=2880;output_start_h=1620;
#else
        output_start_w=1920;output_start_h=1080;
#endif
        return;
    }
    if (!ns2_parse_resolution(e,&output_w,&output_h))
        runtime_fail("TOTK_OUTPUT_RES must be <width>x<height>, each 1..16384 (Metal texture extent)");
    output_has_size=1;
    output_start_w=output_w;output_start_h=output_h;
#if !TARGET_OS_IPHONE
    /* Reserve the largest supported canvas once; render at the saved extent.
       Bananza also reserves its matching gsys pool and parent heap at startup. */
#ifdef GAME_DKB
    const char *root=getenv("TOTK_ROMFS_PATCHES_OUT");char path[4096];
    snprintf(path,sizeof path,"%s/3840x2160/manifest.tsv",root?root:"extracted/dkb/romfs-patches-out");
    if(output_w<=3840&&output_h<=2160&&!access(path,R_OK)){output_w=3840;output_h=2160;}
#else
    if(output_w>3840||output_h>2160)runtime_fail("Pokopia native display supports at most 3840x2160");
    output_w=3840;output_h=2160;
#endif
#endif
}
#endif
int runtime_output_res(unsigned *w, unsigned *h) {
#if defined(GAME_DKB) || defined(GAME_POKOPIA)
    pthread_once(&output_once,output_initialize);
    if(!output_has_size)return 0;
    *w=output_w;*h=output_h;return 1;
#else
    (void)w; (void)h; return 0;
#endif
}
int runtime_render_res(unsigned *w, unsigned *h) { (void)w; (void)h; return 0; }   /* no larger scene size for the heaps (romfs.c) */
uint64_t runtime_output_res_extra_bytes(void) {
#ifdef GAME_DKB
    unsigned w,h;
    if(runtime_output_res(&w,&h))return bananza_render_pool_bytes(w,h)-(200ULL<<20);
#endif
    return 0;
}
uint64_t runtime_output_size_current(void) {
#if defined(GAME_DKB) || defined(GAME_POKOPIA)
    pthread_once(&output_once,output_initialize);
    uint64_t *p=atomic_load(&output_page);
    return p?atomic_load((_Atomic uint64_t *)(p+1)):((uint64_t)output_start_h<<32|output_start_w);
#else
    return 1920ull|(1080ull<<32);
#endif
}
#if defined(GAME_DKB) || defined(GAME_POKOPIA)
int runtime_output_size_set(unsigned w,unsigned h) {
    unsigned mw,mh;uint64_t *p=atomic_load(&output_page);
    if(!p||!w||!h||!runtime_output_res(&mw,&mh)||w>mw||h>mh)return 0;
    atomic_store((_Atomic uint64_t *)(p+1),(uint64_t)h<<32|w);
#if !TARGET_OS_IPHONE
    void runtime_front_resize_output(unsigned,unsigned);runtime_front_resize_output(w,h);
#endif
    fprintf(stderr,"[totk] " GAME_NAME " live resolution requested: %ux%u (capacity %ux%u)\n",w,h,mw,mh);
    return 1;
}
#endif
int runtime_output_res_toggle(void) { return 0; }
int runtime_output_mode_cycle(void) { return 0; }
int runtime_aspect_toggle(void) { return 0; }
#if defined(GAME_OOT) && !TARGET_OS_IPHONE
static _Atomic uint32_t *oot_aspect_cell;   /* the 3D view's aspect ratio for the projection (float bits, 0 = the game's own) */
float runtime_aspect_now(void) { if (!oot_aspect_cell) return 0; uint32_t b = atomic_load(oot_aspect_cell); float f; memcpy(&f, &b, 4); return f; }
#else
float runtime_aspect_now(void) { return 0; }
#endif
int runtime_aspect_step(void) { return -1; }   /* the settings panel selects the ratio */
void runtime_aspect_view(double w, double h) { (void)w; (void)h; }
int runtime_output_modes(void) { return 0; }
int runtime_output_mode(void) { return OUTPUT_MODE_OFF; }
int runtime_output_mode_set(int mode) { (void)mode; return 0; }
int runtime_output_native(int mode, unsigned *w, unsigned *h) { (void)mode; (void)w; (void)h; return 0; }
int runtime_aspect_set_ratio(float r) {
#if defined(GAME_DKB) || defined(GAME_POKOPIA)
    if(!isfinite(r)||r<0.5f||r>8)return 0;
    uint64_t s=runtime_output_size_current();unsigned w,h=(unsigned)(s>>32),mw,mh;
    if(!runtime_output_res(&mw,&mh))return 0;
    if(!ns2_aspect_size(r,h,mw,mh,&w,&h))return 0;
    return runtime_output_size_set(w,h);
#elif defined(GAME_OOT) && !TARGET_OS_IPHONE
    if(!oot_aspect_cell||!isfinite(r)||r<1||r>4)return 0;
    float v=fabsf(r-16.0f/9)<0.002f?0:r;uint32_t b;memcpy(&b,&v,4);atomic_store(oot_aspect_cell,b);
    fprintf(stderr,"[oot] aspect ratio %g\n",v?v:16.0/9);return 1;
#else
    (void)r;return 0;
#endif
}
/* TOTK's VR and camera-distance hooks (zeldaVR, the settings panel) are left to their weak defaults (zeldaVR/gfx/vr_defaults.c):
   no sites in a Switch 2 game, so VR and the camera row stay off. */
float runtime_aspect_ratio(void) {
#if defined(GAME_DKB) || defined(GAME_POKOPIA)
    unsigned w,h;if(!atomic_load(&output_page)||!runtime_output_res(&w,&h))return 0;
    uint64_t s=runtime_output_size_current();return (unsigned)(s>>32)?(float)(unsigned)s/(unsigned)(s>>32):0;
#elif defined(GAME_OOT) && !TARGET_OS_IPHONE
    if(!oot_aspect_cell)return 0;float r=runtime_aspect_now();return r>0?r:16.0f/9;
#else
    return 0;
#endif
}
float runtime_aspect_start(void) {
#if defined(GAME_DKB) || defined(GAME_POKOPIA)
    return output_start_h?(float)output_start_w/output_start_h:0;
#elif defined(GAME_OOT) && !TARGET_OS_IPHONE
    const char *e=getenv("OOT_ASPECT");return e&&*e?strtof(e,NULL):0;
#else
    return 0;
#endif
}

/* Exclusive loads with acquire (TOTK_EXCL_ACQUIRE=0: as shipped): every plain ldxr / ldxp that opens a real exclusive window
   (a store-exclusive on the same base register within 16 instructions) gets the acquire bit. Why: game_patches_wonder.c. */
static int excl_ld(uint32_t w) { return (w & 0x3ffffc00u) == 0x085f7c00u || (w & 0xbfff8000u) == 0x887f0000u; }   /* ldxr{b,h} / ldxp */
static int excl_st(uint32_t w) { return (w & 0x3fe07c00u) == 0x08007c00u || (w & 0xbfe00000u) == 0x88200000u; }   /* st{l}xr{b,h} / st{l}xp */
static unsigned excl_acquire(uint8_t *code, size_t n, int write) {
    unsigned done = 0; uint32_t *w = (uint32_t *)(void *)code; size_t words = n / 4;
    for (size_t i = 0; i < words; i++) {
        if (!excl_ld(w[i])) continue;
        uint32_t rn = (w[i] >> 5) & 31;
        for (size_t j = i + 1; j < words && j <= i + 16; j++) if (excl_st(w[j]) && ((w[j] >> 5) & 31) == rn) { if (write) w[i] |= 0x8000u; done++; break; }
    }
    return done;
}

#if defined(GAME_DKB) || defined(GAME_POKOPIA)
/* Shared PC-relative instruction encoder. */
/* The game's maximum framebuffer getters and its gsys creation argument are
   distinct inputs. The former allocates the canvas, the latter selects the
   active mode's extent and output scale. Both must describe the requested size. */
static uint32_t output_adrp(unsigned reg, uintptr_t pc, uintptr_t value) {
    int64_t delta=((int64_t)(value&~4095ULL)-(int64_t)(pc&~4095ULL))/4096;
    uint32_t imm=(uint32_t)delta&0x1fffff;
    return 0x90000000u|((imm&3)<<29)|((imm>>2)<<5)|reg;
}
#endif
#ifdef GAME_DKB
static uintptr_t output_main;
static void patch_output_parameters(Module *m) {
    output_main=(uintptr_t)m->base;
    const uint64_t page=(m->file_size+0x3fff)&~0x3fffULL;
    if(page+0x4000!=m->size)runtime_fail("Bananza resolution parameter page");
    unsigned w=2880,h=1620;int custom=runtime_output_res(&w,&h);
    uint32_t sizes[2]={w,h};memcpy(m->base+page,sizes,sizeof sizes);
    uint64_t *parameters=(void *)(m->base+page);
    parameters[1]=custom?((uint64_t)output_start_h<<32|output_start_w):0;
    parameters[2]=parameters[3]=0;
    atomic_store(&output_page,parameters);
    const uint64_t sites[2]={0x5bf770,0x5bf780};
    const uint32_t initial[2][3]={{0x52816800,0xd65f03c0,0},{0x5280ca80,0xd65f03c0,0}};
    for(unsigned k=0;k<2;k++){
        uint32_t want[3]={output_adrp(8,(uintptr_t)m->base+sites[k],(uintptr_t)m->base+page),0xb9400100u|(k<<10),0xd65f03c0};
#if TARGET_OS_IPHONE
        if(memcmp(m->base+sites[k],want,sizeof want))runtime_fail("Bananza resolution getters not exported at build time");
#else
        if(memcmp(m->base+sites[k],initial[k],sizeof initial[k]))runtime_fail("Bananza framebuffer getter differs from verified v458752");
        memcpy(m->base+sites[k],want,sizeof want);
#endif
    }
    uint32_t call;memcpy(&call,m->base+0x5399ec,4);
    if(call!=0x94590609)runtime_fail("Bananza gsys creation argument copy differs from verified v458752");
    memcpy(&call,m->base+0x53a068,4);
    if(call!=0x9459046a)runtime_fail("Bananza graphics memory argument copy differs from verified v458752");
    const uint64_t stub=(m->text_end+15)&~15ULL,site=0x30f85c;
    uint32_t code[BANANZA_DISPLAY_WORDS];
    if(stub+sizeof code>m->ro_off)runtime_fail("Bananza display veneer outside text padding");
    bananza_display_code(code,(uintptr_t)m->base+stub,(uintptr_t)m->base+page,(uintptr_t)m->base+site+4);
    uint32_t jump=bananza_display_branch(site,stub),word;memcpy(&word,m->base+site,4);
#if TARGET_OS_IPHONE
    if(word!=jump||memcmp(m->base+stub,code,sizeof code))runtime_fail("Bananza live display veneer not exported");
#else
    if(word!=0x2941200b)runtime_fail("Bananza native display update differs from verified v458752");
    for(unsigned k=0;k<sizeof code;k++)if(m->base[stub+k])runtime_fail("Bananza display text padding in use");
    memcpy(m->base+stub,code,sizeof code);memcpy(m->base+site,&jump,4);
#endif
    if(custom){
        const uint64_t camera_site=0x31f3bc,camera_stub=stub+96;
        uint32_t camera[BANANZA_CAMERA_WORDS];
        if(camera_stub+sizeof camera>m->ro_off)runtime_fail("Bananza camera veneer outside text padding");
        bananza_camera_code(camera,(uintptr_t)m->base+camera_stub,(uintptr_t)m->base+page,(uintptr_t)m->base+camera_site+4,(uintptr_t)m->base+0xb1358);
        jump=bananza_display_branch(camera_site,camera_stub);memcpy(&word,m->base+camera_site,4);
#if TARGET_OS_IPHONE
        if(word!=jump||memcmp(m->base+camera_stub,camera,sizeof camera))runtime_fail("Bananza camera aspect veneer not exported");
#else
        if(word!=0xf9400901)runtime_fail("Bananza registered camera transfer differs from verified v458752");
        for(unsigned k=0;k<sizeof camera;k++)if(m->base[camera_stub+k])runtime_fail("Bananza camera text padding in use");
        memcpy(m->base+camera_stub,camera,sizeof camera);memcpy(m->base+camera_site,&jump,4);
#endif
    }
    fprintf(stderr,"[totk] Bananza native live display: start %ux%u, capacity %ux%u; game-thread resize path\n",output_start_w,output_start_h,w,h);
    fprintf(stderr,"[totk] Bananza framebuffer maximum %ux%u; gsys mode sizes %s (verified v458752 source parameters)\n",w,h,custom?"custom":"original");
}
/* The verified main+0x5399ec memcpy publishes the native gsys creation
   argument: 24 Vector2i extents followed by six Vector2f output scales. Other
   copies, and the original-size mode, retain the ordinary memcpy semantics. */
__attribute__((noinline)) void *runtime_bananza_memcpy(void *dst,const void *src,size_t n) {
    void *result=memcpy(dst,src,n);
    uintptr_t caller=(uintptr_t)__builtin_return_address(0);
    if(n==0x11d && caller==output_main+0x53a06c){
        unsigned w,h;if(!runtime_output_res(&w,&h))return result;
        if(!bananza_set_render_pool(dst,n,w,h))runtime_fail("Bananza dynamic render pool differs from verified v458752");
        fprintf(stderr,"[totk] Bananza dynamic render pool: 200 -> %.2f MiB for %ux%u (native graphics creation parameter)\n",bananza_render_pool_bytes(w,h)/1048576.0,w,h);
        return result;
    }
    if(n!=0x108 || caller!=output_main+0x5399f0)return result;
    unsigned w,h;if(!runtime_output_res(&w,&h))return result;
    if(!bananza_set_gsys_sizes(dst,n,w,h))runtime_fail("Bananza gsys maximum does not match its framebuffer getters");
    fprintf(stderr,"[totk] Bananza gsys creation argument: all 24 native extents %ux%u, six output scales 1:1\n",w,h);
    return result;
}
#endif

#ifdef GAME_POKOPIA
static uintptr_t pokopia_main;
__attribute__((noinline)) void *runtime_pokopia_memcpy(void *dst,const void *src,size_t n) {
    void *result=memcpy(dst,src,n);
    if(n==0xa0&&(uintptr_t)__builtin_return_address(0)==pokopia_main+0x17953f8){
        unsigned w,h;if(!runtime_output_res(&w,&h))return result;
        if(!pokopia_window_capacity(dst,n,w,h))runtime_fail("Pokopia native bounded window creation argument mismatch");
        fprintf(stderr,"[totk] Pokopia native bounded resize enabled: swapchain capacity %ux%u\n",w,h);
    }
    return result;
}
static void patch_pokopia_display(Module *m) {
    pokopia_main=(uintptr_t)m->base;
    unsigned mw=1920,mh=1080;if(!runtime_output_res(&mw,&mh))return;
    uint64_t page=(m->file_size+0x3fff)&~0x3fffULL,stub=(m->text_end+15)&~15ULL,site=0x22d598;
    if(page+0x4000!=m->size||stub+128+POKOPIA_CAMERA_WORDS*4>m->ro_off)runtime_fail("Pokopia display patch layout");
    uint64_t *p=(void *)(m->base+page);p[0]=(uint64_t)mh<<32|mw;
    p[1]=p[2]=(uint64_t)output_start_h<<32|output_start_w;p[3]=p[4]=0;
    atomic_store(&output_page,p);
    const uint64_t sites[]={0x1129980,0x1129990,0x11299c0,0x11299d0};
#if !TARGET_OS_IPHONE
    const uint32_t original[][3]={{0x5280f000,0xd65f03c0,0},{0x12001c08,0x7100851f,0x540000c8},
        {0x52808700,0xd65f03c0,0},{0x12001c08,0x7100851f,0x540000c8}};
#endif
    for(unsigned k=0;k<4;k++){
        uintptr_t pc=(uintptr_t)m->base+sites[k],data=(uintptr_t)m->base+page;
        int64_t delta=((int64_t)(data&~4095ULL)-(int64_t)(pc&~4095ULL))/4096;
        uint32_t imm=(uint32_t)delta&0x1fffff;
        uint32_t code[]={0x90000008|((imm&3)<<29)|((imm>>2)<<5),0xb9401100u|((k/2)<<10),0xd65f03c0};
#if TARGET_OS_IPHONE
        if(memcmp(m->base+sites[k],code,sizeof code))runtime_fail("Pokopia display getter not exported");
#else
        if(memcmp(m->base+sites[k],original[k],sizeof code))runtime_fail("Pokopia display getter differs from verified 2.0.1");
        memcpy(m->base+sites[k],code,sizeof code);
#endif
    }
    uint32_t code[POKOPIA_DISPLAY_WORDS];pokopia_display_code(code,(uintptr_t)m->base+stub,(uintptr_t)m->base+page,(uintptr_t)m->base+site+4);
    uint32_t word,jump=bananza_display_branch(site,stub);memcpy(&word,m->base+site,4);
#if TARGET_OS_IPHONE
    if(word!=jump||memcmp(m->base+stub,code,sizeof code))runtime_fail("Pokopia live display veneer not exported");
#else
    if(word!=0xaa0003f3)runtime_fail("Pokopia display controller differs from verified 2.0.1");
    for(unsigned k=0;k<sizeof code;k++)if(m->base[stub+k])runtime_fail("Pokopia display text padding in use");
    memcpy(m->base+stub,code,sizeof code);memcpy(m->base+site,&jump,4);
#endif
    uint32_t resize[POKOPIA_RESIZE_WORDS];const uint64_t resize_site=0x22d648,resize_stub=stub+64;
    pokopia_resize_code(resize,(uintptr_t)m->base+resize_stub,(uintptr_t)m->base+page,(uintptr_t)m->base+resize_site+4);
    jump=bananza_display_branch(resize_site,resize_stub);memcpy(&word,m->base+resize_site,4);
#if TARGET_OS_IPHONE
    if(word!=jump||memcmp(m->base+resize_stub,resize,sizeof resize))runtime_fail("Pokopia native resize veneer not exported");
#else
    if(word!=0x39420008)runtime_fail("Pokopia committed display update differs from verified 2.0.1");
    for(unsigned k=0;k<sizeof resize;k++)if(m->base[resize_stub+k])runtime_fail("Pokopia resize text padding in use");
    memcpy(m->base+resize_stub,resize,sizeof resize);memcpy(m->base+resize_site,&jump,4);
#endif
    uint32_t camera[POKOPIA_CAMERA_WORDS];const uint64_t camera_site=0x8cd884,camera_stub=stub+128;
    pokopia_camera_code(camera,(uintptr_t)m->base+camera_stub,(uintptr_t)m->base+page,(uintptr_t)m->base+camera_site+4,(uintptr_t)m->base+0x1f11e78);
    jump=bananza_display_branch(camera_site,camera_stub);memcpy(&word,m->base+camera_site,4);
#if TARGET_OS_IPHONE
    if(word!=jump||memcmp(m->base+camera_stub,camera,sizeof camera))runtime_fail("Pokopia camera aspect veneer not exported");
#else
    if(word!=0xaa0003f3)runtime_fail("Pokopia gameplay camera transfer differs from verified 2.0.1");
    for(unsigned k=0;k<sizeof camera;k++)if(m->base[camera_stub+k])runtime_fail("Pokopia camera text padding in use");
    memcpy(m->base+camera_stub,camera,sizeof camera);memcpy(m->base+camera_site,&jump,4);
#endif
    uint32_t ui[POKOPIA_UI_WORDS];const uint64_t ui_site=0x1f9bcc8,ui_stub=stub+224;
    if(ui_stub+sizeof ui>m->ro_off)runtime_fail("Pokopia UI camera veneer outside text padding");
    pokopia_ui_code(ui,(uintptr_t)m->base+ui_stub,(uintptr_t)m->base+page,(uintptr_t)m->base+ui_site+4,(uintptr_t)m->base+0x1fa6c68);
    jump=bananza_display_branch(ui_site,ui_stub);memcpy(&word,m->base+ui_site,4);
#if TARGET_OS_IPHONE
    if(word!=jump||memcmp(m->base+ui_stub,ui,sizeof ui))runtime_fail("Pokopia UI camera veneer not exported");
#else
    if(word!=0x91008001)runtime_fail("Pokopia UI camera snapshot differs from verified 2.0.1");
    for(unsigned k=0;k<sizeof ui;k++)if(m->base[ui_stub+k])runtime_fail("Pokopia UI camera text padding in use");
    memcpy(m->base+ui_stub,ui,sizeof ui);memcpy(m->base+ui_site,&jump,4);
#endif
    /* Native Resize(width,height,maxWidth,maxHeight) must reserve the depth
       target as well as the color swapchain. FSR/view updates retain the old
       color extent during transition; shrinking depth to the requested extent
       makes native RT-stack Push fail (old 1080p color, new 720p depth).
       The API explicitly supports depth >= color. Keep both capacities stable,
       and let the game's own view/target rebuilds consume the new active size. */
    const uint64_t capacity_sites[]={0x22d684,0x22d68c};
#if !TARGET_OS_IPHONE
    const uint32_t capacity_original[]={0x2a1f03e3,0x2a1f03e4};
#endif
    uint32_t capacity_code[]={output_adrp(16,(uintptr_t)m->base+capacity_sites[0],(uintptr_t)m->base+page),0x29401203};
    for(unsigned k=0;k<2;k++){
        memcpy(&word,m->base+capacity_sites[k],4);
#if TARGET_OS_IPHONE
        if(word!=capacity_code[k])runtime_fail("Pokopia depth capacity not exported");
#else
        if(word!=capacity_original[k])runtime_fail("Pokopia depth capacity arguments differ from verified 2.0.1");
        memcpy(m->base+capacity_sites[k],&capacity_code[k],4);
#endif
    }
    memcpy(&word,m->base+0x17953f4,4);if(word!=0x943ec29b)runtime_fail("Pokopia window creation copy differs from verified 2.0.1");
    fprintf(stderr,"[totk] Pokopia native live display: start %ux%u, capacity %ux%u; native resize, scene and UI cameras\n",output_start_w,output_start_h,mw,mh);
}
#endif

#if defined(GAME_OOT) && !TARGET_OS_IPHONE
/* OOT (2026-11-08, the user's choice): the 3D renders at a fixed size, docked 1536x864 (0.75 of the 2048x1152 canvas, the
   game's own top size) and handheld 1280x720 (2/3 of 1920x1080), through the game's own "dynamic resolution disabled" path.
   Why it is needed: with correct GPU timestamps (guest queue ordering, 159a2fee) the game's controller measures our real GPU
   time and settles at 683x384 docked; the former 1536x864 came from a measurement of 0.2 ms.
   main+0x805bd0 tbz w8, #0, 0x80640c becomes a branch to that path, which returns scale 1.0 (main+0x806420 mov w9, #1.0, then
   a branch back). 2/3 needs two instructions, so main+0x806420 branches to a veneer after the time-step veneers.
   OOT_FIXED_RES=0: the game's dynamic resolution. OOT_FIXED_RES=<scale>, 0 < scale <= 1, overrides the scale. */
/* Settings panel "Resolution" (height; 0 = the default fixed size), in force at once: patch_oot_scale_live. Scales measured:
   handheld is linear; docked moves in steps, so 720/900/1080 are the nearest sizes reached (1275x717, 1586x892, 1906x1072). */
enum { OOT_SCALE_CELL = 48, OOT_SCALE_APPLY_CELL = 52 };
static _Atomic uint32_t *oot_scale_cell, *oot_scale_apply_cell; static float oot_scale_base, oot_scale_listed; static int oot_internal_h;
static float oot_internal_scale(int h) {
    int hh = runtime_handheld_mode();
    switch (h) {
        case 720: return hh ? 2.0f / 3.0f : 0.6125f;
        case 900: return hh ? 5.0f / 6.0f : 0.7695f;
        case 1080: return hh ? 1.0f : 0.9267f;
        case 1152: return 1.0f;
    }
    return oot_scale_base;
}
int runtime_oot_internal_res(void) { return oot_internal_h; }
/* Any render scale, applied at once (also "flag oot_scale <scale * 10000>", to measure which scale gives which size). */
static void oot_scale_write(float s) {
    uint32_t bits; memcpy(&bits, &s, sizeof bits); atomic_store(oot_scale_cell, bits);
    if (oot_scale_apply_cell) atomic_store(oot_scale_apply_cell, 1);   /* the game's frame code applies it (patch_oot_scale_live) */
    fprintf(stderr, "[oot] render scale %g\n", s);
}
void runtime_oot_scale_set(float s) {   /* before the patch: the scale it starts with */
    if (!(s > 0 && s <= 1)) return;
    oot_scale_listed = s;
    if (oot_scale_cell) oot_scale_write(s);
}
void runtime_oot_internal_res_set(int h) {
    oot_internal_h = h; oot_scale_listed = 0;
    if (!oot_scale_cell) return;
    fprintf(stderr, "[oot] internal resolution setting %d\n", h);
    oot_scale_write(oot_internal_scale(h));
}
/* Changing the resolution while the game runs (2026-11-09). With its dynamic resolution off the game takes the scale only when
   the controller reports a change: main+0x31d3ee0 calls the controller (0x805aa0) and, on the report, writes the scale to the
   render view's field +0xe5c, which is read every frame (found with mscan watch; writing it moved the main pass from 1536x864
   to 1275x717 and 1906x1072 at once, claude/oot-depth-unify g15). So the instruction after that call branches here: when the
   settings panel asks (the apply cell), the veneer stores the cell's scale there the way the game does, scale * +0xe58 / +0xe50. */
/* The output canvas (2026-11-09). When the game lays out its display (an operation mode change: docking) it computes the canvas
   size and stores it at main+0x6505fc, then 0x1087f40 keeps it; the window crop and the render targets read that. The veneer
   puts the size of the data-page cell there instead, while the cell is not zero. */
static _Atomic uint64_t *oot_output_cell;
void runtime_oot_output_size_set(unsigned w, unsigned h) {
    if (!oot_output_cell) return;
    atomic_store(oot_output_cell, (uint64_t)h << 32 | w);
    fprintf(stderr, "[oot] output canvas requested: %ux%u\n", w, h);
}
/* The resolution list of the settings panel: the canvas, and MetalFX to the window for the "+ MetalFX" entries (index 0: the
   game's own canvas). A change takes effect when the game lays out its display again, which it does when the operation mode
   changes: the mode is reported as the other one for a moment and then back (the notification alone is not enough, g28). */
static uint64_t oot_output_pending; static _Atomic int oot_flip_busy, oot_flip_again;
extern void runtime_operation_mode_report(int docked);
static void *oot_canvas_flip(void *arg) {
    (void)arg; int docked = !runtime_handheld_mode();
    do { atomic_store(&oot_flip_again, 0); runtime_operation_mode_report(!docked); usleep(400000); runtime_operation_mode_report(docked); usleep(100000); } while (atomic_exchange(&oot_flip_again, 0));
    atomic_store(&oot_flip_busy, 0); return NULL;
}
/* The resolution row (user 2026-11-09): the canvas is the picture, the 3D sits inside it at the listed scale, and the one
   upscale between them is the game's network or MetalFX, as the "Upscaler" row says. Docked scales move in steps; these were
   measured (claude/oot-depth-unify g34, g36): 0.6667 gives 854x480 in 1280x720 and 1280x720 in 1920x1080, 0.75 gives 1920x1080
   in 2560x1440. 6 and 7 need a 4K canvas, which crashes at scale 1 (issues_lessons 2026-11-09): not in the list.
   0 is 854x480 in 1280x720 and 8 the same (saved by an earlier list); with no choice saved the settings panel starts 2,
   1280x720 in 1920x1080 (user 2026-11-09: the Mac default, with MetalFX). 9 keeps the canvas the
   game computes (docked 2048x1152) with patch_oot_fixed_res's scale (1536x864): not the game's own picture, which is dynamic
   resolution; not listed (user 2026-11-09), "flag oot_canvas 9" only. */
void runtime_oot_canvas_set(int index, int live) {
    static const struct { unsigned w, h; float scale; } list[] = {{1280,720,0.6667f},{1280,720,1},{1920,1080,0.6667f},{1920,1080,1},{2560,1440,0.75f},{2560,1440,1},{3840,2160,0.6667f},{3840,2160,1},{1280,720,0.6667f},{0,0,0}};
    if (index < 0 || index >= (int)(sizeof list / sizeof *list)) return;
    extern void dm_gfx_set_flag(const char *name, int value);
    uint64_t size = (uint64_t)list[index].h << 32 | list[index].w;
    if (oot_output_cell) atomic_store(oot_output_cell, size); else oot_output_pending = size;
    if (list[index].scale > 0) runtime_oot_scale_set(list[index].scale); else runtime_oot_internal_res_set(0);   /* 9: patch_oot_fixed_res's scale */
    dm_gfx_set_flag("mfx_upscale", 0); dm_gfx_set_flag("mfx_output", 0);   /* no second upscale after the canvas */
    fprintf(stderr, "[oot] resolution %d: canvas %ux%u, 3D scale %g\n", index, list[index].w, list[index].h, (double)list[index].scale);
    if (!live || !oot_output_cell) return;
    if (atomic_exchange(&oot_flip_busy, 1)) { atomic_store(&oot_flip_again, 1); return; }
    pthread_t t; if (pthread_create(&t, NULL, oot_canvas_flip, NULL)) atomic_store(&oot_flip_busy, 0); else pthread_detach(t);
}
enum { OOT_OUTPUT_CELL = 64 };
static void patch_oot_output_size(Module *m, uint64_t page) {
    const uint64_t site = 0x6505fc; uint32_t have; memcpy(&have, m->base + site, 4);
    if (have != 0x290227e8) runtime_fail("OOT output size site differs from verified update 131072");
    uint64_t stub = ((m->text_end + 15) & ~15ULL) + 248 + 8 + 16 + 24 + 48, rx_end = (m->text_end + 0x3fff) & ~0x3fffULL;
    int64_t pages = ((int64_t)page - (int64_t)(stub & ~4095ULL)) / 4096;
    uint32_t cell = (OOT_OUTPUT_CELL + (uint32_t)(page & 4095)) / 8;
    uint32_t code[7] = {
        0x90000010 | (((uint32_t)pages & 3) << 29) | ((((uint32_t)pages >> 2) & 0x7ffff) << 5),    /* adrp x16, page */
        0xf9400211 | cell << 10,    /* ldr x17, [x16, #cell] */
        0xb4000071,                 /* cbz x17, the original instruction */
        0x2a1103e8,                 /* mov w8, w17 */
        0xd360fe29,                 /* lsr x9, x17, #32 */
        have,
        oot_step_branch(stub + 24, site + 4) };
    if (stub + sizeof code > rx_end || page + OOT_OUTPUT_CELL + 8 > m->size) runtime_fail("OOT output size: no room left");
    for (uint64_t k = stub; k < stub + sizeof code; k++) if (m->base[k]) runtime_fail("OOT output size: text padding already in use");
    for (uint64_t k = page + OOT_OUTPUT_CELL; k < page + OOT_OUTPUT_CELL + 8; k++) if (m->base[k]) runtime_fail("OOT output size: data page already in use");
    oot_output_cell = (_Atomic uint64_t *)(void *)(m->base + page + OOT_OUTPUT_CELL);
    if (oot_output_pending) atomic_store(oot_output_cell, oot_output_pending);
    memcpy(m->base + stub, code, sizeof code);
    uint32_t b = oot_step_branch(site, stub); memcpy(m->base + site, &b, 4);
}
static void patch_oot_scale_live(Module *m, uint64_t page) {
    const uint64_t site = 0x31d3ee4; uint32_t have; memcpy(&have, m->base + site, 4);
    if (have != 0xd3609c08) runtime_fail("OOT resolution apply site differs from verified update 131072");   /* ubfx x8, x0, #32, #8 */
    uint64_t stub = ((m->text_end + 15) & ~15ULL) + 248 + 8 + 16 + 24, rx_end = (m->text_end + 0x3fff) & ~0x3fffULL;
    int64_t pages = ((int64_t)page - (int64_t)(stub & ~4095ULL)) / 4096;
    uint32_t off = (uint32_t)(page & 4095), scale = (OOT_SCALE_CELL + off) / 4, apply = (OOT_SCALE_APPLY_CELL + off) / 4;
    uint32_t code[12] = {
        0x90000010 | (((uint32_t)pages & 3) << 29) | ((((uint32_t)pages >> 2) & 0x7ffff) << 5),    /* adrp x16, page */
        0xb9400211 | apply << 10,   /* ldr w17, [x16, #apply] */
        0x34000111,                 /* cbz w17, the original instruction */
        0xb900021f | apply << 10,   /* str wzr, [x16, #apply] */
        0xbd400200 | scale << 10,   /* ldr s0, [x16, #scale] */
        0xbd4e5a81,                 /* ldr s1, [x20, #0xe58] */
        0x1e210800,                 /* fmul s0, s0, s1 */
        0xbd4e5281,                 /* ldr s1, [x20, #0xe50] */
        0x1e211800,                 /* fdiv s0, s0, s1 */
        0xbd0e5e80,                 /* str s0, [x20, #0xe5c] */
        have,
        oot_step_branch(stub + 44, site + 4) };
    if (stub + sizeof code > rx_end || page + OOT_SCALE_APPLY_CELL + 4 > m->size) runtime_fail("OOT live resolution: no room left");
    for (uint64_t k = stub; k < stub + sizeof code; k++) if (m->base[k]) runtime_fail("OOT live resolution: text padding already in use");
    for (uint64_t k = page + OOT_SCALE_APPLY_CELL; k < page + OOT_SCALE_APPLY_CELL + 4; k++) if (m->base[k]) runtime_fail("OOT live resolution: data page already in use");
    oot_scale_apply_cell = (_Atomic uint32_t *)(void *)(m->base + page + OOT_SCALE_APPLY_CELL);
    memcpy(m->base + stub, code, sizeof code);
    uint32_t b = oot_step_branch(site, stub); memcpy(m->base + site, &b, 4);
}
static void patch_oot_fixed_res(Module *m) {
    if (getenv("TOTK_EXPORT_IOS")) return;
    const char *e = getenv("OOT_FIXED_RES");
    if (e && !strcmp(e, "0")) { fprintf(stderr, "[oot] dynamic resolution left to the game (OOT_FIXED_RES=0)\n"); return; }
    float s = runtime_handheld_mode() ? 2.0f / 3.0f : 0.75f;
    if (e && *e) { char *end = NULL; s = strtof(e, &end); if (!end || *end || !(s > 0 && s <= 1)) runtime_fail("OOT_FIXED_RES must be 0 or a scale in (0, 1]"); }
    uint32_t bits; memcpy(&bits, &s, sizeof bits);
    const uint64_t site = 0x805bd0, disabled = 0x80640c, scale_site = 0x806420;
    uint32_t have, have_mov; memcpy(&have, m->base + site, 4); memcpy(&have_mov, m->base + scale_site, 4);
    if (have != 0x360041e8 || have_mov != 0x52a7f009) runtime_fail("OOT resolution sites differ from verified update 131072");
    /* After the 248 bytes of time-step veneers (oot_timestep.h), which stay reserved even with OOT_REALTIME=0. */
    uint64_t stub = ((m->text_end + 15) & ~15ULL) + 248 + 8, rx_end = (m->text_end + 0x3fff) & ~0x3fffULL;
    /* The scale is read from the data page (+48, after the time-step fields) so the settings panel can change it live. */
    uint64_t page = (m->file_size + 0x3fff) & ~0x3fffULL;
    int64_t pages = ((int64_t)page - (int64_t)(stub & ~4095ULL)) / 4096;
    uint32_t veneer[3] = { 0x90000009 | (((uint32_t)pages & 3) << 29) | ((((uint32_t)pages >> 2) & 0x7ffff) << 5), 0xb9400129 | (((OOT_SCALE_CELL + (uint32_t)(page & 4095)) / 4) << 10), oot_step_branch(stub + 8, scale_site + 4) };
    if (stub + sizeof veneer > rx_end || page + OOT_SCALE_CELL + 4 > m->size) runtime_fail("OOT fixed resolution: no room left");
    for (uint64_t k = stub; k < stub + sizeof veneer; k++) if (m->base[k]) runtime_fail("OOT fixed resolution: text padding already in use");
    for (uint64_t k = page + OOT_SCALE_CELL; k < page + OOT_SCALE_CELL + 4; k++) if (m->base[k]) runtime_fail("OOT fixed resolution: data page already in use");
    oot_scale_base = s; oot_scale_cell = (_Atomic uint32_t *)(void *)(m->base + page + OOT_SCALE_CELL);
    if (oot_scale_listed > 0) memcpy(&bits, &oot_scale_listed, sizeof bits);
    else if (oot_internal_h) { float c = oot_internal_scale(oot_internal_h); memcpy(&bits, &c, sizeof bits); }
    atomic_store(oot_scale_cell, bits);
    uint32_t b = oot_step_branch(site, disabled), to_veneer = oot_step_branch(scale_site, stub);
    memcpy(m->base + stub, veneer, sizeof veneer); memcpy(m->base + site, &b, 4); memcpy(m->base + scale_site, &to_veneer, 4);
    fprintf(stderr, "[oot] fixed render scale %g (%s; OOT_FIXED_RES=0: the game's dynamic resolution)\n", s, runtime_handheld_mode() ? "handheld" : "docked");
    patch_oot_scale_live(m, page);
    patch_oot_output_size(m, page);
}
/* The iOS export only (TOTK_EXPORT_IOS; user 2026-11-09: the phone's resolution pick, Ocarina of Time only). patch_oot_fixed_res
   stays the Mac's and skips the export, so this writes the same code into the exported text: main+0x805bd0 branches to the
   game's "dynamic resolution disabled" path, main+0x806420 to a veneer that loads the scale from the data page (+48). The
   data page is not exported (runtime/loader.c export-ios); the app writes the scale at start (oot_ios_fixed_res). A Mac run
   never calls this. */
static void export_oot_fixed_res_ios(Module *m) {
    const uint64_t site = 0x805bd0, disabled = 0x80640c, scale_site = 0x806420;
    uint32_t have, have_mov; memcpy(&have, m->base + site, 4); memcpy(&have_mov, m->base + scale_site, 4);
    if (have != 0x360041e8 || have_mov != 0x52a7f009) runtime_fail("OOT resolution sites differ from verified update 131072");
    uint64_t stub = ((m->text_end + 15) & ~15ULL) + 248 + 8, rx_end = (m->text_end + 0x3fff) & ~0x3fffULL;
    uint64_t page = (m->file_size + 0x3fff) & ~0x3fffULL;
    int64_t pages = ((int64_t)page - (int64_t)(stub & ~4095ULL)) / 4096;
    uint32_t veneer[3] = { 0x90000009 | (((uint32_t)pages & 3) << 29) | ((((uint32_t)pages >> 2) & 0x7ffff) << 5), 0xb9400129 | (((OOT_SCALE_CELL + (uint32_t)(page & 4095)) / 4) << 10), oot_step_branch(stub + 8, scale_site + 4) };
    if (stub + sizeof veneer > rx_end || page + OOT_SCALE_CELL + 4 > m->size) runtime_fail("OOT fixed resolution (iOS export): no room left");
    for (uint64_t k = stub; k < stub + sizeof veneer; k++) if (m->base[k]) runtime_fail("OOT fixed resolution (iOS export): text padding already in use");
    uint32_t b = oot_step_branch(site, disabled), to_veneer = oot_step_branch(scale_site, stub);
    memcpy(m->base + stub, veneer, sizeof veneer); memcpy(m->base + site, &b, 4); memcpy(m->base + scale_site, &to_veneer, 4);
    fprintf(stderr, "[oot] fixed render scale: code exported for iOS; the app writes its scale at start\n");
}
#endif
#if defined(GAME_OOT) && !TARGET_OS_IPHONE
/* OOT aspect ratio (2026-11-09). The game camera's parameters (near +0x134, far +0x138, fovy +0x13c, aspect +0x140) are
   copied into its sead perspective projection every frame at main+0x4c52d0; main+0x4c5334 ldr s0, [x19, #0x140] reads
   the aspect for the store at main+0x4c533c (the projection's +0xb0; found with mscan bp / watch, claude/oot-gpu-ts w1, w2).
   That load branches to a veneer that runs it and, while a ratio sits in the data-page cell (+56), uses that instead.
   x16 and s1 are free there (not read before the bl at main+0x4c534c). OOT_ASPECT=<ratio> starts with that ratio. */
enum { OOT_ASPECT_CELL = 56 };
static void patch_oot_aspect(Module *m) {
    if (getenv("TOTK_EXPORT_IOS")) return;
    const uint64_t site = 0x4c5334; uint32_t have; memcpy(&have, m->base + site, 4);
    if (have != 0xbd414260) runtime_fail("OOT aspect site differs from verified update 131072");
    uint64_t page = (m->file_size + 0x3fff) & ~0x3fffULL, rx_end = (m->text_end + 0x3fff) & ~0x3fffULL;
    uint64_t stub = ((m->text_end + 15) & ~15ULL) + 248 + 8 + 16;
    int64_t pages = ((int64_t)page - (int64_t)((stub + 4) & ~4095ULL)) / 4096;
    uint32_t code[6] = {
        0xbd414260,                                                                                   /* ldr s0, [x19, #0x140] */
        0x90000010 | (((uint32_t)pages & 3) << 29) | ((((uint32_t)pages >> 2) & 0x7ffff) << 5),    /* adrp x16, page */
        0xbd400201 | (((OOT_ASPECT_CELL + (uint32_t)(page & 4095)) / 4) << 10),                     /* ldr s1, [x16, #cell] */
        0x1e202028,                                                                                   /* fcmp s1, #0.0 */
        0x1e201c20,                                                                                   /* fcsel s0, s1, s0, ne */
        oot_step_branch(stub + 20, site + 4) };
    if (stub + sizeof code > rx_end || page + OOT_ASPECT_CELL + 4 > m->size) runtime_fail("OOT aspect: no room left");
    for (uint64_t k = stub; k < stub + sizeof code; k++) if (m->base[k]) runtime_fail("OOT aspect: text padding already in use");
    for (uint64_t k = page + OOT_ASPECT_CELL; k < page + OOT_ASPECT_CELL + 4; k++) if (m->base[k]) runtime_fail("OOT aspect: data page already in use");
    oot_aspect_cell = (_Atomic uint32_t *)(void *)(m->base + page + OOT_ASPECT_CELL);
    const char *e = getenv("OOT_ASPECT"); float r = e && *e ? strtof(e, NULL) : 0;
    if (r && !(r >= 1 && r <= 4)) runtime_fail("OOT_ASPECT must be a ratio between 1 and 4");
    uint32_t bits; memcpy(&bits, &r, 4); atomic_store(oot_aspect_cell, bits);
    uint32_t jump = oot_step_branch(site, stub);
    memcpy(m->base + stub, code, sizeof code); memcpy(m->base + site, &jump, 4);
    fprintf(stderr, "[oot] aspect veneer at main+0x%llx; ratio %g (0 = the game's 16:9)\n", (unsigned long long)site, r);
}
#endif
#if defined(GAME_OOT) && TARGET_OS_IPHONE
/* iOS (user 2026-11-09): the phone's resolution pick. The export wrote export_oot_fixed_res_ios's code in; the data page cell it
   reads (+48, OOT_SCALE_CELL on the Mac) is not exported, so the scale main.m chose (OOT_FIXED_RES, settings_map.c
   oot_picture_build) is written here, before the game first asks for it. */
static void oot_ios_fixed_res(Module *m) {
    const uint64_t site = 0x805bd0, disabled = 0x80640c, scale_site = 0x806420, cell = 48;
    uint32_t have, have_mov; memcpy(&have, m->base + site, 4); memcpy(&have_mov, m->base + scale_site, 4);
    if (have != (0x14000000u | (uint32_t)((disabled - site) / 4)) || (have_mov & 0xfc000000u) != 0x14000000u)
        runtime_fail("embedded OOT code has no fixed render scale (export-ios from before 2026-11-09)");
    const char *e = getenv("OOT_FIXED_RES"); char *end = NULL;
    float s = e && *e ? strtof(e, &end) : 2.0f / 3.0f;
    if ((e && *e && (!end || *end)) || !(s > 0 && s <= 1)) runtime_fail("OOT_FIXED_RES must be a scale in (0, 1]");
    uint64_t page = (m->file_size + 0x3fff) & ~0x3fffULL;
    if (page + cell + 4 > m->size) runtime_fail("OOT fixed resolution: no data page");
    _Atomic uint32_t *slot = (_Atomic uint32_t *)(void *)(m->base + page + cell);
    if (atomic_load(slot)) runtime_fail("OOT fixed resolution: data page already in use");
    uint32_t bits; memcpy(&bits, &s, sizeof bits); atomic_store(slot, bits);
    fprintf(stderr, "[oot] fixed render scale %g (iOS, OOT_FIXED_RES)\n", s);
}
#endif
void runtime_game_patches(Module *m) {
    if (strcmp(m->name, "main")) runtime_fail("game patches expect the main module");
    if (getenv("TOTK_PCHTXT") || getenv("TOTK_ASPECT"))
        fprintf(stderr, "[totk] %s: TOTK_PCHTXT and TOTK_ASPECT are not supported; ignored\n", GAME_NAME);
#ifdef GAME_DKB
    patch_output_parameters(m);
#elif defined(GAME_POKOPIA)
    patch_pokopia_display(m);
#elif defined(GAME_OOT) && !TARGET_OS_IPHONE
    patch_oot_timestep(m);
    patch_oot_fixed_res(m);
    patch_oot_aspect(m);
    if (getenv("TOTK_EXPORT_IOS")) export_oot_fixed_res_ios(m);   /* only when exporting for the phone */
#elif defined(GAME_OOT)
    oot_ios_fixed_res(m);
#endif
    const char *a = getenv("TOTK_EXCL_ACQUIRE");
    if (TARGET_OS_IPHONE || getenv("TOTK_EXPORT_IOS") || !a || strcmp(a, "0")) {
        const int write = !TARGET_OS_IPHONE;
        unsigned t = excl_acquire(m->base, m->text_end, write), p = excl_acquire(m->base + m->patch_off, m->file_size - m->patch_off, write);
        if (!write && (t || p)) runtime_fail("embedded NS2 code was not patched at build time (export-ios)");
        fprintf(stderr, "[totk] exclusive loads with acquire: %u in the code, %u in the rewriter's patch area (TOTK_EXCL_ACQUIRE=0: as shipped)\n", t, p);
    } else fprintf(stderr, "[totk] exclusive loads as shipped (TOTK_EXCL_ACQUIRE=0)\n");
}

/* The game paces itself (its own present interval and frame-rate controller): no host frame limit. */
int runtime_game_fps_limit(void) { return 0; }
