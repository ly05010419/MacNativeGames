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
#include <TargetConditionals.h>
#include "ns2_resolution.h"

/* No frame-step patch: these are what the frontend and the frame lock call. */
void runtime_frame_step_pin(double ms) { (void)ms; }
void runtime_frame_step_update(uint64_t now_ns) { (void)now_ns; }

#ifdef GAME_DKB
static pthread_once_t output_once=PTHREAD_ONCE_INIT;
static unsigned output_w,output_h;
static int output_has_size;
static void output_initialize(void) {
    const char *e = getenv("TOTK_OUTPUT_RES");
#if !TARGET_OS_IPHONE
    /* Creation parameters are immutable for this run. A panel choice is read
       before module patching, and takes effect on the following launch. */
    char saved[32];
    if (!e || !*e) {
        if(runtime_setting_word("graphics-output-size.txt",saved,sizeof saved)){
            if(ns2_parse_saved_resolution(saved,&output_w,&output_h)){output_has_size=1;return;}
            fprintf(stderr,"[totk] invalid saved Bananza resolution; using 1920x1080\n");
        }
        e="1920x1080";
    }
#endif
    if (!e || !*e || !strcmp(e, "0")) return;
    if (!ns2_parse_resolution(e,&output_w,&output_h))
        runtime_fail("TOTK_OUTPUT_RES must be <width>x<height>, each 1..16384 (Metal texture extent)");
    output_has_size=1;
}
#endif
int runtime_output_res(unsigned *w, unsigned *h) {
#ifdef GAME_DKB
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
uint64_t runtime_output_size_current(void) { unsigned w=1920,h=1080;runtime_output_res(&w,&h);return (uint64_t)h << 32 | w; }
int runtime_output_res_toggle(void) { return 0; }
int runtime_output_mode_cycle(void) { return 0; }
int runtime_aspect_toggle(void) { return 0; }
float runtime_aspect_now(void) { return 0; }
int runtime_aspect_step(void) { return -1; }   /* no aspect setting: the shortcut list leaves it out */
void runtime_aspect_view(double w, double h) { (void)w; (void)h; }
int runtime_output_modes(void) { return 0; }
int runtime_output_mode(void) { return OUTPUT_MODE_OFF; }
int runtime_output_mode_set(int mode) { (void)mode; return 0; }
int runtime_output_native(int mode, unsigned *w, unsigned *h) { (void)mode; (void)w; (void)h; return 0; }
int runtime_aspect_set_ratio(float r) { (void)r; return 0; }
/* TOTK's VR and camera-distance hooks (zeldaVR, the settings panel) are left to their weak defaults (zeldaVR/gfx/vr_defaults.c):
   no sites in a Switch 2 game, so VR and the camera row stay off. */
float runtime_aspect_ratio(void) { return 0; }
float runtime_aspect_start(void) { return 0; }

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

#ifdef GAME_DKB
static uintptr_t output_main;
/* The game's maximum framebuffer getters and its gsys creation argument are
   distinct inputs. The former allocates the canvas, the latter selects the
   active mode's extent and output scale. Both must describe the requested size. */
static uint32_t output_adrp(unsigned reg, uintptr_t pc, uintptr_t value) {
    int64_t delta=((int64_t)(value&~4095ULL)-(int64_t)(pc&~4095ULL))/4096;
    uint32_t imm=(uint32_t)delta&0x1fffff;
    return 0x90000000u|((imm&3)<<29)|((imm>>2)<<5)|reg;
}
static void patch_output_parameters(Module *m) {
    output_main=(uintptr_t)m->base;
    const uint64_t page=(m->file_size+0x3fff)&~0x3fffULL;
    if(page+0x4000!=m->size)runtime_fail("Bananza resolution parameter page");
    unsigned w=2880,h=1620;int custom=runtime_output_res(&w,&h);
    uint32_t sizes[2]={w,h};memcpy(m->base+page,sizes,sizeof sizes);
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

void runtime_game_patches(Module *m) {
    if (strcmp(m->name, "main")) runtime_fail("game patches expect the main module");
    if (getenv("TOTK_PCHTXT") || getenv("TOTK_ASPECT"))
        fprintf(stderr, "[totk] %s: TOTK_PCHTXT and TOTK_ASPECT are not supported; ignored\n", GAME_NAME);
#ifdef GAME_DKB
    patch_output_parameters(m);
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
