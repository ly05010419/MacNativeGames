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

/* No frame-step patch: these are what the frontend and the frame lock call. */
void runtime_frame_step_pin(double ms) { (void)ms; }
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
float runtime_aspect_now(void) { return 0; }
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
#else
    return 0;
#endif
}
float runtime_aspect_start(void) {
#if defined(GAME_DKB) || defined(GAME_POKOPIA)
    return output_start_h?(float)output_start_w/output_start_h:0;
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

void runtime_game_patches(Module *m) {
    if (strcmp(m->name, "main")) runtime_fail("game patches expect the main module");
    if (getenv("TOTK_PCHTXT") || getenv("TOTK_ASPECT"))
        fprintf(stderr, "[totk] %s: TOTK_PCHTXT and TOTK_ASPECT are not supported; ignored\n", GAME_NAME);
#ifdef GAME_DKB
    patch_output_parameters(m);
#elif defined(GAME_POKOPIA)
    patch_pokopia_display(m);
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
