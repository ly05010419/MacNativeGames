/* 1.4.3 has a new compiler and frame-controller layout. Every adapted game-code
   site below is checked before any write; older versions use game_patches.c. */
#include "aspect_patch.h"
#include <time.h>
#include "game_patches.h"
#include "game_version.h"
#include "world_survey.h"
#include "../zeldaVR/runtime/vr_camera.h"   /* zeldaVR: the VR camera hook */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>   /* access: the VR swapchain sizes present (vr_big_swapchain) */
#include <stdatomic.h>
#include <math.h>
#include <TargetConditionals.h>
#if !TARGET_OS_IPHONE
#include <mach/mach.h>
#include <mach/mach_vm.h>   /* the VR camera search (Mac diagnostic) */
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#endif

typedef struct { uint64_t off; uint32_t original, replacement; } Patch;
static const Patch fps60[] = {
    {0x2db204, 0x6b08015f, 0x7100055f},
    {0x2db208, 0x1a89b10a, 0x1a9fa52a},
    {0x2db22c, 0xb940468a, 0xd503201f},
    {0x2db360, 0x6b0801bf, 0x710005bf},
    {0x2db36c, 0x1a88c1ac, 0x1a9fc5ac},
    {0x2db340, 0xb9400288, 0x52800028},
    {0x2db34c, 0x2a0a03f5, 0x1a9fa555},
};


static _Atomic uint64_t *frame_step_data;
static _Atomic uint32_t *frame_interval_data;
static _Atomic uint32_t pinned_step_us;
static uint64_t last_present_ns;
static double frame_samples[5];
static unsigned frame_sample_count,frame_sample_next;
static void frame_step_store(float seconds){
    float values[3]={seconds*30.0f,seconds,seconds*60.0f};uint64_t bits;uint32_t interval;
    memcpy(&bits,values,8);memcpy(&interval,values+2,4);
    atomic_store_explicit(frame_step_data,bits,memory_order_release);
    atomic_store_explicit(frame_interval_data,interval,memory_order_release);
}
void runtime_frame_step_pin(double ms){
    uint32_t us=ms>0?(uint32_t)(ms*1000.0+0.5):0;atomic_store(&pinned_step_us,us);
    if(frame_step_data&&us)frame_step_store((float)(us/1e6));
    fprintf(stderr,"[totk] 1.4.3 frame step %s (pin %.3f ms)\n",us?"pinned":"real time",us/1000.0);
}
void runtime_frame_step_update(uint64_t now_ns){
    if(!frame_step_data||!now_ns)return;
    if(atomic_load_explicit(&pinned_step_us,memory_order_relaxed)){last_present_ns=now_ns;return;}
    if(!last_present_ns){last_present_ns=now_ns;return;}
    double dt=(double)(now_ns-last_present_ns)*1e-9;last_present_ns=now_ns;
    if(dt>0.25||dt<0.001){frame_sample_count=frame_sample_next=0;return;}
    /* No 1/120 s floor, as in TOTK 1.0-1.2.1 (game_patches.c) and BotW since 2338bb0: a present shorter than 1/120 s
       counted as 1/120 s made the game run fast above 120 fps (1.2x at 144, 2x at 240) and with jitter at the 120 cap
       (each short interval raised, each long one kept). A hitch is still cut at 1/15 s per interval, as before. */
    if(dt>1.0/15.0)dt=1.0/15.0;
    frame_samples[frame_sample_next++%5]=dt;if(frame_sample_count<5)frame_sample_count++;
    double sum=0;for(unsigned i=0;i<frame_sample_count;i++)sum+=frame_samples[i];
    frame_step_store((float)(sum/frame_sample_count));
}
static uint32_t adrp(unsigned reg,uint64_t pc,uint64_t target){
    int64_t pages=(int64_t)(target>>12)-(int64_t)(pc>>12);
    if(pages<-(1<<20)||pages>=(1<<20))runtime_fail("1.4.3 data outside ADRP range");
    uint32_t imm=(uint32_t)pages&0x1fffff;
    return 0x90000000|((imm&3)<<29)|((imm>>2)<<5)|reg;
}
static uint32_t branch(uint64_t pc,uint64_t target){
    int64_t delta=(int64_t)target-(int64_t)pc;
    if((delta&3)||delta<-(1<<27)||delta>=(1<<27))runtime_fail("1.4.3 patch branch outside range");
    return 0x14000000|(((uint32_t)(delta>>2))&0x3ffffff);
}
/* The actor animation update has a second, unscaled clock for cloth/hair
 * (ordinary animation already receives FrameRateMgr's real-time step). Its
 * explicit time-factor path still constructs {factor / 30, factor} every
 * rendered frame. At 120 FPS that advances four seconds per real second.
 * Correct only that clock, retaining pauses, slow motion and the fallback to
 * the ordinary clock. 1.4.3 inlines the update in two actor paths; two other
 * callers share the out-of-line update at +0x2a0e40. Its third caller already
 * passes measured time and MUST NOT be scaled a second time.
 *
 * The inline veneers use only the original destination s register and x8,
 * which the next original load overwrites. At the two call boundaries x8/s4
 * are caller-saved scratch; the veneer tail-calls the original function with
 * its original LR. No stack, flags, ordinary-clock arguments or callee-saved
 * registers are changed. Stubs follow the four 32-byte aspect veneers. */
static void patch_secondary_animation(Module *m,uint64_t data,uint64_t stub){
    const char *enabled=getenv("TOTK_CLOTH_REALTIME");
    if(enabled&&!strcmp(enabled,"0"))return;   /* diagnostic comparison */
    const Patch sites[]={
        {0x246eac,0x1e200940,0}, /* fmul s0,s10,s0: inline clock */
        {0x345904,0x1e210801,0}, /* fmul s1,s0,s1: inline clock */
        {0x24944c,0x94015e7d,0}, /* bl 0x2a0e40: actor with transform override */
        {0x28bc98,0x9400546a,0}, /* bl 0x2a0e40: includes both transform branches */
    };
    enum { WORDS=19 };
    if(stub+WORDS*4>((m->text_end+0x3fff)&~0x3fffULL)||data+8>m->size)
        runtime_fail("1.4.3 secondary animation patch layout");
    uint32_t code[WORDS]={
        adrp(8,(uintptr_t)m->base+stub,(uintptr_t)m->base+data),
        0xbd400100,0x1e200940,0xbd0367e0, /* ratio * factor -> sp+0x364 */
        0xbd400500,0x1e200940,branch(stub+24,0x246eb0), /* seconds * factor */
        adrp(8,(uintptr_t)m->base+stub+28,(uintptr_t)m->base+data),
        0xbd400101,0x1e210801,0xbd0117e1, /* ratio * factor -> sp+0x114 */
        0xbd400501,0x1e210801,branch(stub+52,0x345908),
        adrp(8,(uintptr_t)m->base+stub+56,(uintptr_t)m->base+data),
        0xbd400104,0x1e240842,0x1e240863, /* s2/s3 *= measured 30 Hz ratio */
        branch(stub+72,0x2a0e40),
    };
    uint32_t jumps[]={branch(sites[0].off,stub),branch(sites[1].off,stub+28),
        branch(sites[2].off,stub+56)|0x80000000u,branch(sites[3].off,stub+56)|0x80000000u};
    for(unsigned i=0;i<sizeof sites/sizeof *sites;i++){
        if(sites[i].off+4>m->text_end)runtime_fail("1.4.3 secondary animation site outside text");
        uint32_t word;memcpy(&word,m->base+sites[i].off,4);
        if(word!=(TARGET_OS_IPHONE?jumps[i]:sites[i].original))
            runtime_fail("1.4.3 secondary animation instruction mismatch");
    }
#if TARGET_OS_IPHONE
    if(memcmp(m->base+stub,code,sizeof code))runtime_fail("1.4.3 embedded secondary animation veneer mismatch");
#else
    for(unsigned i=0;i<sizeof code;i++)if(m->base[stub+i])runtime_fail("1.4.3 secondary animation padding in use");
    memcpy(m->base+stub,code,sizeof code);
    for(unsigned i=0;i<sizeof sites/sizeof *sites;i++)memcpy(m->base+sites[i].off,jumps+i,4);
#endif
    fprintf(stderr,"[totk] 1.4.3 cloth/hair clock: real-time seconds and ratio at 4 guarded actor sites; ordinary animation and time factors retained\n");
}
static void patch_realtime(Module *m){
    const char *enabled=getenv("TOTK_REALTIME");if(enabled&&!strcmp(enabled,"0"))return;
    uint64_t data=(m->file_size+0x3fff)&~0x3fffULL;
    /* Keep executable veneers inside the text segment. iOS embeds x0 as a
     * signed RX segment, so these bytes must be emitted by export-ios rather
     * than written after the app starts. */
    uint64_t stub=(m->text_end+15)&~15ULL;
    if(data+0x4000!=m->size||stub+16>((m->text_end+0x3fff)&~0x3fffULL))runtime_fail("1.4.3 frame-step patch layout");
    const Patch guards[]={
        {0x3e5658,0x1e210808,0},{0x2daa54,0xbd425260,0},{0x2daa58,0x52a84e08,0},
        {0x2daa6c,0x5e21d800,0},{0x2daa70,0x1e201820,0},{0x2daa74,0x1e2c1001,0},{0x2daa78,0x1e210800,0}};
    for(unsigned i=0;i<sizeof guards/sizeof *guards;i++){
#if !TARGET_OS_IPHONE
        uint32_t word;memcpy(&word,m->base+guards[i].off,4);
        if(word!=guards[i].original)runtime_fail("1.4.3 real-time original instruction mismatch");
#endif
    }
#if !TARGET_OS_IPHONE
    for(unsigned i=0;i<16;i++)if(m->base[stub+i])runtime_fail("1.4.3 frame-step veneer padding not empty");
#endif
    /* Replace the nominal 60/FPS interval BEFORE the original producer applies
     * its 0.5/1.0 selector (sys+0x1da bit 0). Both FrameRateMgr call sites use
     * this s8 value. Hooking the callee entry instead would discard that scale
     * and affect any other intentional interval setter. Keep the original fmul
     * and all FrameRateMgr ratio/inverse/seconds/FPS arithmetic untouched.
     * x9 contains live task flags here: use x8, which the next ldxr overwrites.
     * s0 is dead until the call sites copy s8 into it again. */
    uint32_t veneer[]={adrp(8,(uintptr_t)m->base+stub,(uintptr_t)m->base+data),0xbd400900,0x1e210808,branch(stub+12,0x3e565c)};
 #if TARGET_OS_IPHONE
    if(memcmp(m->base+stub,veneer,sizeof veneer))runtime_fail("1.4.3 embedded frame-step veneer mismatch");
 #else
    memcpy(m->base+stub,veneer,sizeof veneer);
 #endif
    uint32_t words[]={branch(0x3e5658,stub),adrp(9,(uintptr_t)m->base+0x2daa54,(uintptr_t)m->base+data),0xbd400120,0xd503201f,0xd503201f,0xd503201f,0xd503201f};
 #if TARGET_OS_IPHONE
    for(unsigned i=0;i<sizeof guards/sizeof *guards;i++){uint32_t got;memcpy(&got,m->base+guards[i].off,4);if(got!=words[i])runtime_fail("1.4.3 embedded real-time patch mismatch");}
 #else
    for(unsigned i=0;i<sizeof guards/sizeof *guards;i++)memcpy(m->base+guards[i].off,words+i,4);
 #endif
    frame_step_data=(_Atomic uint64_t *)(void *)(m->base+data);
    frame_interval_data=(_Atomic uint32_t *)(void *)(m->base+data+8);frame_step_store(1.0f/60.0f);
    patch_secondary_animation(m,data,stub+176);
    fprintf(stderr,"[totk] 1.4.3 real-time frame step: 7 guarded sites, producer veneer +%#llx; original half-step selector and ratio/inverse/seconds/FPS calculations retained\n",(unsigned long long)stub);
}
/* High frame rates (#61, sky-island trampolines): the contact code at main+0x562a64 scales the bounce vector by 0.1
   when X+0x28 > 100, X being the time object 0x3e58a4 feeds (X+0x28 = 30 / (30 Hz ratio * time scale), X+0x38 the
   ratio). The shipping game steps at 30 Hz (ratio 1), so the check means "time slowed below 0.3"; above 100 FPS it also
   fires with time running normally. The veneer compares X+0x28 * X+0x38 = 30 / time scale instead: the same value at
   30 Hz, independent of the frame rate. TOTK_HIGHFPS_FIX=0 keeps the original check. */
static void patch_slowmo_bounce(Module *m){
    const char *e=getenv("TOTK_HIGHFPS_FIX");if(e&&!strcmp(e,"0"))return;
    /* after the ultrawide veneers (+48, 4 x 32 bytes) and the cloth/hair clock (+176, 19 words); read-only data starts
       at the next 4 KiB page */
    uint64_t stub=((m->text_end+15)&~15ULL)+256;
    if(stub+20>((m->text_end+0xfff)&~0xfffULL))runtime_fail("1.4.3 bounce veneer layout");
    uint32_t veneer[]={0xbd402920,   /* ldr s0, [x9, #0x28]: X+0x28 */
        0xbd403921,                  /* ldr s1, [x9, #0x38]: the 30 Hz ratio (s1 is set again by the next original instruction) */
        0x1e210800,                  /* fmul s0, s0, s1 */
        0x52a85909,                  /* mov w9, #0x42c80000: the original 100.0 */
        branch(stub+16,0x562a6c)};
    uint32_t word=branch(0x562a64,stub);
#if TARGET_OS_IPHONE
    uint32_t got;memcpy(&got,m->base+0x562a64,4);
    if(got!=word||memcmp(m->base+stub,veneer,sizeof veneer))runtime_fail("1.4.3 embedded bounce patch mismatch");
#else
    const Patch guards[]={{0x562a64,0xbd402920,0},{0x562a68,0x52a85909,0},{0x562a6c,0x1e270121,0}};
    for(unsigned i=0;i<sizeof guards/sizeof *guards;i++){uint32_t w;memcpy(&w,m->base+guards[i].off,4);if(w!=guards[i].original)runtime_fail("1.4.3 bounce check: original instruction mismatch");}
    for(unsigned i=0;i<sizeof veneer;i++)if(m->base[stub+i])runtime_fail("1.4.3 bounce veneer padding not empty");
    memcpy(m->base+stub,veneer,sizeof veneer);memcpy(m->base+0x562a64,&word,4);
#endif
    fprintf(stderr,"[totk] 1.4.3 high-FPS fix: trampoline slow-motion check uses the time scale, veneer +%#llx\n",(unsigned long long)stub);
}
/* High frame rates (#466, #556, #538: translucent rocks, stone lanterns and walls above 60 FPS):
   TOTK advances distance/LOD mesh fades (0x0a7ac4, 0x95dd24) and pre-actor fades (0x6155e4, 0x615660)
   using a byte counter divided by 20. The increment added to the counter each frame is
   trunc(2.1 * ratio30). At 30 FPS ratio30 is 1.0 (increment 2), at 60 FPS ratio30 is 0.5 (increment 1).
   Above ~63.5 FPS (ratio30 < 1/2.1 ≈ 0.476), including 90, 100, 120 and 144 FPS, trunc(2.1 * ratio30)
   evaluates to ZERO. The counter stalls indefinitely, leaving objects either completely invisible (stuck at 0)
   or translucent (stuck midway).
   Clamp the increment to at least 1 at all four sites: max(trunc(2.1 * ratio30), 1).
   Text padding stub at +928 (after Vert+ stubs, 20 words = 80 bytes). TOTK_FADE_FIX=0 or TOTK_HIGHFPS_FIX=0
   disables this patch. */
static void patch_fade_increment(Module *m){
    const char *e=getenv("TOTK_HIGHFPS_FIX");if(e&&!strcmp(e,"0"))return;
    const char *f=getenv("TOTK_FADE_FIX");if(f&&!strcmp(f,"0"))return;
    uint64_t stub=((m->text_end+15)&~15ULL)+928;
    if(stub+80>((m->text_end+0xfff)&~0xfffULL))runtime_fail("1.4.3 fade veneer layout");
    uint32_t veneer[]={
        /* stub + 0 (28 bytes): site 1 (mesh LOD fade 0x0a7ac4) */
        0x1e380008,   /* fcvtzs w8, s0 */
        0x7100011f,   /* cmp w8, #0 */
        0x1a9f1508,   /* csinc w8, w8, wzr, ne */
        0x3942792a,   /* ldrb w10, [x9, #0x9e] */
        0x0b080148,   /* add w8, w10, w8 */
        0x39027928,   /* strb w8, [x9, #0x9e] */
        branch(stub+24,0x0a7ad4),

        /* stub + 28 (20 bytes): site 2 (mesh LOD fade 0x95dd24) */
        0x1e380008,   /* fcvtzs w8, s0 */
        0x7100011f,   /* cmp w8, #0 */
        0x1a9f1508,   /* csinc w8, w8, wzr, ne */
        0x0b080128,   /* add w8, w9, w8 */
        branch(stub+28+16,0x95dd2c),

        /* stub + 48 (16 bytes): site 3 (pre-actor fade 0x6155e4) */
        0x1e38000a,   /* fcvtzs w10, s0 */
        0x7100015f,   /* cmp w10, #0 */
        0x1a9f154a,   /* csinc w10, w10, wzr, ne */
        branch(stub+48+12,0x6155e8),

        /* stub + 64 (16 bytes): site 4 (pre-actor fade 0x615660) */
        0x1e38000a,   /* fcvtzs w10, s0 */
        0x7100015f,   /* cmp w10, #0 */
        0x1a9f154a,   /* csinc w10, w10, wzr, ne */
        branch(stub+64+12,0x615664),
    };
    const Patch guards[]={
        {0x0a7ac4,0x1e380008,0},{0x0a7ac8,0x3942792a,0},
        {0x0a7acc,0x0b080148,0},{0x0a7ad0,0x39027928,0},
        {0x95dd24,0x1e380008,0},{0x95dd28,0x0b080128,0},
        {0x6155e4,0x1e38000a,0},
        {0x615660,0x1e38000a,0},
    };
    uint32_t jump_s1[]={branch(0x0a7ac4,stub),0xd503201f,0xd503201f,0xd503201f};
    uint32_t jump_s2[]={branch(0x95dd24,stub+28),0xd503201f};
    uint32_t jump_s3=branch(0x6155e4,stub+48);
    uint32_t jump_s4=branch(0x615660,stub+64);
#if TARGET_OS_IPHONE
    uint32_t got[8];
    for(unsigned i=0;i<4;i++)memcpy(got+i,m->base+0x0a7ac4+4*i,4);
    for(unsigned i=0;i<2;i++)memcpy(got+4+i,m->base+0x95dd24+4*i,4);
    memcpy(got+6,m->base+0x6155e4,4);
    memcpy(got+7,m->base+0x615660,4);
    if(memcmp(got,jump_s1,16)||memcmp(got+4,jump_s2,8)||got[6]!=jump_s3||got[7]!=jump_s4||
       memcmp(m->base+stub,veneer,sizeof veneer))
        runtime_fail("1.4.3 embedded fade patch mismatch");
#else
    for(unsigned i=0;i<sizeof guards/sizeof *guards;i++){
        uint32_t w;memcpy(&w,m->base+guards[i].off,4);
        if(w!=guards[i].original)runtime_fail("1.4.3 fade check: original instruction mismatch");
    }
    for(unsigned i=0;i<sizeof veneer;i++)if(m->base[stub+i])runtime_fail("1.4.3 fade veneer padding not empty");
    memcpy(m->base+stub,veneer,sizeof veneer);
    memcpy(m->base+0x0a7ac4,jump_s1,sizeof jump_s1);
    memcpy(m->base+0x95dd24,jump_s2,sizeof jump_s2);
    memcpy(m->base+0x6155e4,&jump_s3,4);
    memcpy(m->base+0x615660,&jump_s4,4);
#endif
    fprintf(stderr,"[totk] 1.4.3 high-FPS fix: LOD/actor fade counters clamped to >= 1 at 4 sites (prevents translucent objects above 60 FPS), veneer +%#llx\n",(unsigned long long)stub);
}
/* Benchmark camera (survey sessions only, 2026-10-20). The look-at camera update stores the frame's eye (doubles +0x60,
   floats +0x38), target (doubles +0x78, floats +0x44) and normalised up (+0x50) into the camera in x20 at
   main+0x6287b4..0x628824, then builds the view matrix from d1,d2,d0 / d3,d4,d5 / s7,s16,s17. The adrp at main+0x628828
   (the point UltraCam's "Camera Controller Universal" signature finds) branches to a stub that, while the block at
   data+0x100 is active, loads the benchmark camera into those registers and the camera's fields; the game's camera
   controller keeps its own state, so the view returns to the player when the block goes inactive. Block: u32 active,
   eye f64[3] +8, target f64[3] +32, up f32[3] +56, then the float copies of eye and target as at +0x38 (+72..+95), the
   vertical FOV in radians +96 (stored into the controller's +0x13c, which main+0x628c3c copies to the projection's
   fovy, as UltraCam's "Camera Controller Hook Fix 1.4.0+" does). UltraCam's time-of-day hook ("TimeHook") matches an
   unrelated function in 1.4.3, so the benchmark sets no time: the save's time runs on as in citron.
   The stub sits after the rewriter's patch area (main+file_size up to the data page, executable like the rest of that
   area and never written by the rewriter): main's text padding is taken by the veneers above, the aspect and Vert+ stubs. */
#define BENCH_CAM_DATA 0x100u
static void patch_bench_cam(Module *m){
#if TARGET_OS_IPHONE
    (void)m;
#else
    const char *survey=getenv("TOTK_SURVEY");if(!survey||strcmp(survey,"1"))return;
    uint64_t data=(m->file_size+0x3fff)&~0x3fffULL,stub=(m->file_size+15)&~15ULL;   /* the patch area's tail */
    static const uint32_t body[]={
        0x91040129, /* add x9, x9, #0x100 */
        0xb940012a, /* ldr w10, [x9]: active? */
        0x3400028a, /* cbz w10, restore (20 instructions on) */
        0x6d408921,0xfd400d20,0x6d421123,0xfd401925,   /* ldp d1,d2 / ldr d0 / ldp d3,d4 / ldr d5: eye and target */
        0x2d474127,0xbd404131,                         /* ldp s7,s16 / ldr s17: up */
        0x6d060a81,0x6d070e80,0x6d081684,              /* stp into the camera's doubles +0x60..+0x88 */
        0x2d0a4287,0xbd005a91,                         /* stp s7,s16,[x20,#0x50] / str s17,[x20,#0x58] */
        0xf940252a,0xf9001e8a,0xf940292a,0xf900228a,0xf9402d2a,0xf900268a,   /* float eye and target +0x38..+0x4f */
        0xb940612a,0xb9013e6a,                         /* ldr w10, [x9, #96] / str w10, [x19, #0x13c]: FOV */
        0xf84107ea}; /* restore: ldr x10, [sp], #16 */
    enum { N = 2 + sizeof body / 4 + 2 };
    uint32_t words[N];
    words[0]=0xf81f0fea;   /* str x10, [sp, #-16]! */
    words[1]=adrp(9,(uintptr_t)m->base+stub+4,(uintptr_t)m->base+data);
    memcpy(words+2,body,sizeof body);
    words[N-2]=adrp(9,(uintptr_t)m->base+stub+(N-2)*4,(uintptr_t)m->base+0x396f000);   /* the instruction it replaces */
    words[N-1]=branch(stub+(N-1)*4,0x62882c);
    uint32_t got;memcpy(&got,m->base+0x628828,4);
    if(got!=0xf0019a29)runtime_fail("1.4.3 benchmark camera: the camera code does not match");
    if(stub<m->patch_off||stub+sizeof words>data||data+BENCH_CAM_DATA+112>m->size)runtime_fail("1.4.3 benchmark camera layout");
    for(unsigned i=0;i<sizeof words;i++)if(m->base[stub+i])runtime_fail("1.4.3 benchmark camera padding not empty");
    memset(m->base+data+BENCH_CAM_DATA,0,112);
    memcpy(m->base+stub,words,sizeof words);
    uint32_t jump=branch(0x628828,stub);memcpy(m->base+0x628828,&jump,4);
    runtime_bench_cam=m->base+data+BENCH_CAM_DATA;
    fprintf(stderr,"[totk] 1.4.3 benchmark camera: look-at and FOV override at main+0x628828 (HUD not hidden yet), stub +%#llx\n",
            (unsigned long long)stub);
#endif
}
/* Model LOD distance (Mac, the settings panel's effects page or TOTK_LOD_SCALE=<K>, 2026-10-30). The renderer picks a model's level of detail
   from the view-space depth z of its bounding sphere (negative in front; the caller stores it at the entry's +0x10 next to
   the projected radius): v = z + bias, the first level while v >= renderer+0xc50, beyond that one level per 1/k of depth
   (bias and k from the model's LOD table). main+0x1ee8424 is that function; callers that see its vtable slot inline it,
   so the sum appears at 12 sites. Each `fadd v, z, bias` branches to a stub computing z * c + bias, c = 1/K read from the
   data page at every call, so the level at depth d is the one the game picks at d / K (K = 1: the same bits as fadd).
   "lodscale <K>" changes K live. The community "LOD Improvement" pchtxt patches only the function, which the inlined
   copies never call. Always installed on the Mac: at K = 1 the level is the game's own, bit for bit. */
#define LOD_DATA_SLOT 0x180u
#if !TARGET_OS_IPHONE
/* The LOD, render distance, field of view, shadow resolution and depth of field patches read their factors from the
   data page, which only the Mac's settings panel and input commands write. The iOS export (TOTK_EXPORT_IOS) runs these
   patches on the Mac to make the phone's code, and on the phone that page would read 0 (no LOD distance, no far plane,
   no field of view, the smallest shadow map), so the export leaves them out: the phone runs the game's own code there. */
static int live_tuned(void){return !getenv("TOTK_EXPORT_IOS");}
static _Atomic uint32_t *lod_factor;
static double lod_k=1,far_k=1;   /* the factors asked for (the settings panel may ask before the patches are installed) */
static void factor_store(_Atomic uint32_t *slot,double k){float f=(float)k;uint32_t bits;memcpy(&bits,&f,4);atomic_store(slot,bits);}
static double env_factor(const char *name,double lo){
    const char *e=getenv(name);if(!e||!*e)return 0;
    double k=strtod(e,NULL);if(!(k>=lo&&k<=100)){fprintf(stderr,"[totk] %s takes %g to 100\n",name,lo);runtime_fail("factor out of range");}
    return k;
}
#endif
static void patch_lod_distance(Module *m){
#if TARGET_OS_IPHONE
    (void)m;
#else
    if(!live_tuned())return;
    double k=env_factor("TOTK_LOD_SCALE",0.1);if(k)lod_k=k;else k=lod_k;
    static const uint64_t sites[]={0x7ad1c,0x7c00c,0x7c4b0,0x7d4a8,0x80f30,0x1ee8474,
                                   0x2870578,0x287200c,0x2872a30,0x2874924,0x2874cfc,0x287f53c};
    enum{N=sizeof sites/sizeof *sites,W=5};
    uint64_t data=(m->file_size+0x3fff)&~0x3fffULL,stub=((m->file_size+15)&~15ULL)+128;   /* after the benchmark camera */
    if(stub+N*W*4>data||data+LOD_DATA_SLOT+4>m->size)runtime_fail("1.4.3 LOD distance layout");
    for(unsigned i=0;i<N*W*4;i++)if(m->base[stub+i])runtime_fail("1.4.3 LOD distance padding not empty");
    for(unsigned i=0;i<N;i++){
        uint32_t add,next;memcpy(&add,m->base+sites[i],4);memcpy(&next,m->base+sites[i]+4,4);
        if((add&0xffe0fc00)!=0x1e202800||(next&0xfffffc00)!=0xbd4c5000)runtime_fail("1.4.3 LOD distance: the LOD code does not match");
        unsigned d=add&31,n=(add>>5)&31,s=(add>>16)&31;   /* fadd sd, sn (depth), ss (bias) */
        uint64_t at=stub+i*W*4,lit=data+LOD_DATA_SLOT-(at+4);
        uint32_t words[W]={
            0xfc1f0fff,                                   /* str d31, [sp, #-16]! */
            0x1c000000|(((uint32_t)(lit>>2)&0x7ffff)<<5)|31,   /* ldr s31, <data page factor> */
            0x1f000000|(31u<<16)|(s<<10)|(n<<5)|d,        /* fmadd sd, sn, s31, ss */
            0xfc4107ff,                                   /* ldr d31, [sp], #16 */
            branch(at+16,sites[i]+4)};
        if(d==31||n==31||s==31||lit>=(1u<<20))runtime_fail("1.4.3 LOD distance stub");
        memcpy(m->base+at,words,sizeof words);
    }
    lod_factor=(_Atomic uint32_t *)(void *)(m->base+data+LOD_DATA_SLOT);float c=(float)(1.0/k);factor_store(lod_factor,1.0/k);
    for(unsigned i=0;i<N;i++){uint32_t j=branch(sites[i],stub+i*W*4);memcpy(m->base+sites[i],&j,4);}
    fprintf(stderr,"[totk] 1.4.3 LOD distance x%g at %u sites (depth scaled by %g for the level choice), stub +%#llx\n",
            k,(unsigned)N,c,(unsigned long long)stub);
#endif
}
/* Render distance (Mac, the settings panel's effects page or TOTK_FAR_SCALE=<K>, 2026-10-30): the camera update copies the controller's near / far
   (+0x134 / +0x138) and FOV (+0x13c) into the projection (+0x98 / +0x9c / +0xa0, sead's PerspectiveProjection order) at
   main+0x628c20..0x628c44. The far load branches to a stub that keeps the game's value on the data page (+0x188, for
   "farscale" to print) and multiplies it by the factor at +0x184. x8 is reloaded at 0x628c6c and s2 is caller-saved across
   the sin / cos / tan calls that follow, so the stub may use both. The game's far is 25000 (measured 2026-10-30 in the
   field and in Lookout Landing), more than the whole map: a smaller factor cuts the distance (fewer distant draws); the
   panel offers 1x to 10x (user 2026-10-30), past what the map needs. Always installed on the Mac: at 1 the far plane is the game's own. */
#define FAR_DATA_SLOT 0x184u
#if !TARGET_OS_IPHONE
static _Atomic uint32_t *far_factor;
static const float *far_game;
#endif
static void patch_far_distance(Module *m){
#if TARGET_OS_IPHONE
    (void)m;
#else
    if(!live_tuned())return;
    double k=env_factor("TOTK_FAR_SCALE",0.001);if(k)far_k=k;else k=far_k;
    uint64_t data=(m->file_size+0x3fff)&~0x3fffULL,stub=((m->file_size+15)&~15ULL)+128+12*5*4;   /* after the LOD stubs */
    if(stub+24>data||data+FAR_DATA_SLOT+8>m->size)runtime_fail("1.4.3 render distance layout");
    for(unsigned i=0;i<24;i++)if(m->base[stub+i])runtime_fail("1.4.3 render distance padding not empty");
    static const Patch guards[]={{0x628c30,0xbd413a60,0},{0x628c38,0xbd009e80,0},{0x628c6c,0x91051268,0}};
    for(unsigned i=0;i<3;i++){uint32_t w;memcpy(&w,m->base+guards[i].off,4);if(w!=guards[i].original)runtime_fail("1.4.3 render distance: the camera code does not match");}
    uint32_t words[6]={
        0xbd413a60,                                              /* ldr s0, [x19, #0x138]: the game's far */
        adrp(8,(uintptr_t)m->base+stub+4,(uintptr_t)m->base+data),
        0xbd000100|(((FAR_DATA_SLOT+4)/4)<<10),                  /* str s0, [x8, #+0x188] */
        0xbd400102|((FAR_DATA_SLOT/4)<<10),                      /* ldr s2, [x8, #+0x184] */
        0x1e220800,                                              /* fmul s0, s0, s2 */
        branch(stub+20,0x628c34)};
    memcpy(m->base+stub,words,sizeof words);
    far_factor=(_Atomic uint32_t *)(void *)(m->base+data+FAR_DATA_SLOT);far_game=(const float *)(const void *)(m->base+data+FAR_DATA_SLOT+4);
    factor_store(far_factor,k);
    uint32_t j=branch(0x628c30,stub);memcpy(m->base+0x628c30,&j,4);
    fprintf(stderr,"[totk] 1.4.3 render distance x%g (camera far plane), stub +%#llx\n",k,(unsigned long long)stub);
#endif
}
/* Field of view (Mac, the settings panel's effects page or TOTK_FOV_SCALE=<K>, 2026-10-30): the same camera update loads the
   controller's vertical FOV (+0x13c, radians) at main+0x628c3c, stores it at the projection's +0xa0 and derives the sine,
   cosine and tangent of half of it (sead's setFovy, inlined). The load branches to a stub that keeps the game's value
   (+0x194, for the log) and multiplies it by the factor at +0x190, so every value sead derives agrees (aspect_math.h:
   widening only the tangent left the lighting wrong). The game's own zooms (bow, scope) scale with it. x8 and s2 are free
   there as at the far load; s1 holds the 0.5 the next instruction uses. Vert+ (TOTK_ASPECT_VERT=1) takes that load
   first; the FOV then stays the game's. The event camera setup (main+0xb43420..) keeps its own. */
#define FOV_DATA_SLOT 0x190u
#if !TARGET_OS_IPHONE
static _Atomic uint32_t *fov_factor;
static double fov_k=1;
static int fov_taken;   /* Vert+ holds the FOV load: the FOV stays the game's */
#endif
static void patch_fov(Module *m){
#if TARGET_OS_IPHONE
    (void)m;
#else
    if(!live_tuned())return;
    double k=env_factor("TOTK_FOV_SCALE",0.5);if(k)fov_k=k;else k=fov_k;
    if(k>2)runtime_fail("TOTK_FOV_SCALE takes 0.5 to 2");
    uint64_t data=(m->file_size+0x3fff)&~0x3fffULL,stub=((m->file_size+15)&~15ULL)+128+12*5*4+24;   /* after the far stub */
    if(stub+24>data||data+FOV_DATA_SLOT+8>m->size)runtime_fail("1.4.3 field of view layout");
    for(unsigned i=0;i<24;i++)if(m->base[stub+i])runtime_fail("1.4.3 field of view padding not empty");
    uint32_t w;memcpy(&w,m->base+0x628c3c,4);
    if(w!=0xbd413e60){fov_taken=1;fprintf(stderr,"[totk] 1.4.3 field of view: the FOV load is taken (Vert+, TOTK_ASPECT_VERT=1); the game's FOV\n");return;}
    static const Patch guards[]={{0x628c40,0x1e210808,0},{0x628c44,0xbd00a280,0},{0x628c6c,0x91051268,0}};
    for(unsigned i=0;i<3;i++){memcpy(&w,m->base+guards[i].off,4);if(w!=guards[i].original)runtime_fail("1.4.3 field of view: the camera code does not match");}
    uint32_t words[6]={
        0xbd413e60,                                              /* ldr s0, [x19, #0x13c]: the game's vertical FOV */
        adrp(8,(uintptr_t)m->base+stub+4,(uintptr_t)m->base+data),
        0xbd000100|(((FOV_DATA_SLOT+4)/4)<<10),                  /* str s0, [x8, #+0x194] */
        0xbd400102|((FOV_DATA_SLOT/4)<<10),                      /* ldr s2, [x8, #+0x190] */
        0x1e220800,                                              /* fmul s0, s0, s2 */
        branch(stub+20,0x628c40)};
    memcpy(m->base+stub,words,sizeof words);
    fov_factor=(_Atomic uint32_t *)(void *)(m->base+data+FOV_DATA_SLOT);factor_store(fov_factor,k);
    uint32_t j=branch(0x628c3c,stub);memcpy(m->base+0x628c3c,&j,4);
    fprintf(stderr,"[totk] 1.4.3 field of view x%g (the game camera's vertical FOV), stub +%#llx\n",k,(unsigned long long)stub);
#endif
}
/* Shadow resolution (Mac, the settings panel's effects page or TOTK_SHADOW_SCALE=<K>, 2026-10-30): the shadow setup works
   out each cascade's size as align8(max(32, s0 x base)) from the settings' base size (+0x190 / +0x194, 800 x 800) and a
   scale the game clamps (main+0xf5998 width, +0xf59b8 height), stores it (+0x3d8 / +0x3f8) and makes or updates the
   cascade's textures from it at once (main+0xf5a1c -> 0xfca60; found with TOTK_SIZE_CHAIN=800x800). Each multiply
   branches to a stub that multiplies by K from the data page (+0x198), so the textures, viewports and everything sized
   from those fields follow; K = 1 is the game's own (an exact multiply by 1). s3 was last read at 0xf5960. */
#define SHADOW_DATA_SLOT 0x198u
#if !TARGET_OS_IPHONE
static _Atomic uint32_t *shadow_factor;
static double shadow_k=1;
#endif
static void patch_shadow_res(Module *m){
#if TARGET_OS_IPHONE
    (void)m;
#else
    if(!live_tuned())return;
    double k=env_factor("TOTK_SHADOW_SCALE",0.25);if(k)shadow_k=k;else k=shadow_k;
    if(k>8)runtime_fail("TOTK_SHADOW_SCALE takes 0.25 to 8");
    static const Patch sites[]={{0xf5998,0x1e220801,0x1e230821},{0xf59b8,0x1e210800,0x1e230800}};   /* fmul s1,s0,s2 / s0,s0,s1; then x K in s3 */
    static const Patch next[]={{0xf599c,0x1e380029,0},{0xf59bc,0x1e38000a,0}};   /* fcvtzs w9,s1 / w10,s0 */
    uint64_t data=(m->file_size+0x3fff)&~0x3fffULL,stub=((m->file_size+15)&~15ULL)+128+12*5*4+24+24;   /* after the FOV stub */
    if(stub+32>data||data+SHADOW_DATA_SLOT+4>m->size)runtime_fail("1.4.3 shadow resolution layout");
    for(unsigned i=0;i<32;i++)if(m->base[stub+i])runtime_fail("1.4.3 shadow resolution padding not empty");
    for(unsigned i=0;i<2;i++){uint32_t w,n;memcpy(&w,m->base+sites[i].off,4);memcpy(&n,m->base+next[i].off,4);
        if(w!=sites[i].original||n!=next[i].original)runtime_fail("1.4.3 shadow resolution: the shadow setup does not match");}
    for(unsigned i=0;i<2;i++){
        uint64_t at=stub+i*16,lit=data+SHADOW_DATA_SLOT-(at+4);
        uint32_t words[4]={sites[i].original,0x1c000000|(((uint32_t)(lit>>2)&0x7ffff)<<5)|3,sites[i].replacement,branch(at+12,sites[i].off+4)};
        memcpy(m->base+at,words,sizeof words);
    }
    shadow_factor=(_Atomic uint32_t *)(void *)(m->base+data+SHADOW_DATA_SLOT);factor_store(shadow_factor,k);
    for(unsigned i=0;i<2;i++){uint32_t j=branch(sites[i].off,stub+i*16);memcpy(m->base+sites[i].off,&j,4);}
    fprintf(stderr,"[totk] 1.4.3 shadow resolution x%g (cascades %gx%g), stub +%#llx\n",k,800*k,800*k,(unsigned long long)stub);
#endif
}
/* Depth of field off (Mac, the settings panel's effects page, 2026-10-31; UltraCam turns it off through the game's DOF
   state rather than a shader, and so does this). The game runs its DOF passes (dof_near_mask .. dof_coc_composite) only
   while a DOF request is stored: the camera's DOF requester takes them at main+0x1ec606c (focus distance, f-number, blur
   level per requester type; callers: the event commands ExecuteCameraSetDof / ExecuteCameraSetCoCDof at main+0x224c560
   and main+0x6a0cd8) and the final blur at main+0x1ec6180 (the event command only). With DOF off both entries return
   without storing, so no DOF is requested; an event command still marks itself done (main+0x224c5b0), so cutscenes go on.
   The flag sits on the data page (+0x19c); x9 is free at both entries. */
#define DOF_DATA_SLOT 0x19cu
#if !TARGET_OS_IPHONE
static _Atomic uint32_t *dof_flag;
static int dof_off;
#endif
static void patch_dof(Module *m){
#if TARGET_OS_IPHONE
    (void)m;
#else
    if(!live_tuned())return;
    static const Patch sites[]={{0x1ec606c,0xb9400808,0},{0x1ec6180,0xa9ba7bfd,0}};   /* ldr w8,[x0,#8] / stp x29,x30,[sp,#-0x60]! */
    static const Patch next[]={{0x1ec6070,0x7100007f,0},{0x1ec6184,0xf9000bfb,0}};
    uint64_t data=(m->file_size+0x3fff)&~0x3fffULL,stub=((m->file_size+15)&~15ULL)+128+12*5*4+24+24+32;   /* after the shadow stubs */
    if(stub+48>data||data+DOF_DATA_SLOT+4>m->size)runtime_fail("1.4.3 depth of field layout");
    for(unsigned i=0;i<48;i++)if(m->base[stub+i])runtime_fail("1.4.3 depth of field padding not empty");
    for(unsigned i=0;i<2;i++){uint32_t w,n;memcpy(&w,m->base+sites[i].off,4);memcpy(&n,m->base+next[i].off,4);
        if(w!=sites[i].original||n!=next[i].original)runtime_fail("1.4.3 depth of field: the DOF requester does not match");}
    for(unsigned i=0;i<2;i++){
        uint64_t at=stub+i*24;
        uint32_t words[6]={
            adrp(9,(uintptr_t)m->base+at,(uintptr_t)m->base+data),
            0xb9400129|((DOF_DATA_SLOT/4)<<10),     /* ldr w9, [x9, #+0x19c]: DOF off? */
            0x35000069,                             /* cbnz w9, ret (3 words on) */
            sites[i].original,                      /* the first instruction */
            branch(at+16,sites[i].off+4),
            0xd65f03c0};                            /* ret: no request stored */
        memcpy(m->base+at,words,sizeof words);
    }
    dof_flag=(_Atomic uint32_t *)(void *)(m->base+data+DOF_DATA_SLOT);atomic_store(dof_flag,(uint32_t)dof_off);
    for(unsigned i=0;i<2;i++){uint32_t j=branch(sites[i].off,stub+i*24);memcpy(m->base+sites[i].off,&j,4);}
    fprintf(stderr,"[totk] 1.4.3 depth of field %s (DOF requests at main+0x1ec606c / 0x1ec6180), stub +%#llx\n",dof_off?"off":"on",(unsigned long long)stub);
#endif
}
#if !TARGET_OS_IPHONE
void runtime_dof_set(int on){dof_off=!on;if(dof_flag)atomic_store(dof_flag,(uint32_t)dof_off);fprintf(stderr,"[totk] depth of field %s\n",on?"on":"off");}
int runtime_dof_on(void){return !dof_off;}
void runtime_shadow_scale_set(double k){
    shadow_k=k;if(shadow_factor)factor_store(shadow_factor,k);
    fprintf(stderr,"[totk] shadowscale: shadow resolution x%g\n",k);
}
double runtime_shadow_scale(void){return shadow_k;}
int runtime_shadow_scale_locked(void){const char *e=getenv("TOTK_SHADOW_SCALE");return e&&*e;}
void runtime_fov_scale_set(double k){
    fov_k=k;if(fov_factor)factor_store(fov_factor,k);
    float g=fov_factor?((const volatile float *)fov_factor)[1]:0;
    fprintf(stderr,"[totk] fovscale: field of view x%g (the game's vertical FOV now %.1f degrees)\n",k,g*57.29578f);
}
double runtime_fov_scale(void){return fov_k;}
int runtime_fov_scale_locked(void){const char *e=getenv("TOTK_FOV_SCALE");return e&&*e;}
int runtime_fov_available(void){return !fov_taken;}
#endif
#if !TARGET_OS_IPHONE
/* The settings panel and the input file's "farscale" / "lodscale": in force at once, or from the patch's installation. */
void runtime_far_scale_set(double k){
    far_k=k;if(far_factor)factor_store(far_factor,k);
    fprintf(stderr,"[totk] farscale: render distance x%g (the game's far plane %.1f)\n",k,far_game?*(const volatile float *)far_game:0.0f);
}
void runtime_lod_scale_set(double k){
    lod_k=k;if(lod_factor)factor_store(lod_factor,1.0/k);
    fprintf(stderr,"[totk] lodscale: LOD distance x%g\n",k);
}
double runtime_far_scale(void){return far_k;}
double runtime_lod_scale(void){return lod_k;}
int runtime_far_scale_locked(void){const char *e=getenv("TOTK_FAR_SCALE");return e&&*e;}
int runtime_lod_scale_locked(void){const char *e=getenv("TOTK_LOD_SCALE");return e&&*e;}
#endif
#if !TARGET_OS_IPHONE
/* High-frame-rate diagnostics (Mac, TOTK_INPUT_FILE commands). Such bugs sit in game code that compares the game's time
   values with constants tuned for 30 Hz (#61: X+0x28 > 100 above). The time values: the FrameRateMgr singleton
   (main+0x3ada848; +0x10 30 Hz ratio, +0x18 seconds, +0x1c integer FPS) and the time object X = [[main+0x3ad27c8]+0xc8]
   (+0x24 seconds, +0x28 FPS, +0x38 ratio), which 0x3e58a4 updates only when the ratio changes.
   - "frmwatch <seconds> [hex offset]" (FrameRateMgr, default +0x10) / "frmwatchx <seconds> <hex offset>" (X): a hardware
     read watchpoint in every thread, re-armed every 10 ms. A hit (loader.c crash() hands SIGTRAP to
     runtime_diag_watch_trap) records the reading instruction and its return address and disarms that thread, so the
     load runs again; the readers are printed at the end. A frequent reader hides the later ones on its thread until the
     next re-arm, and threshold code often runs only at the moment of contact: watch while the case happens.
   - "xpoke <hex offset> <value>": writes a float into X. With the frame step pinned (framestep) the value stays until
     the ratio changes, which tells which field a behaviour depends on. */
static Module *diag_main;
static uint64_t time_object(void){uint64_t x;memcpy(&x,diag_main->base+0x3ad27c8,8);memcpy(&x,(const void *)(uintptr_t)(x+0xc8),8);return x;}
static uint64_t watch_addr;
static int watch_store;   /* the VR camera search watches stores, frmwatch loads */
enum{WATCH_SLOTS=512};
static struct{_Atomic uint64_t pc;_Atomic uint64_t lr;_Atomic uint64_t up[3];_Atomic uint64_t x1;_Atomic uint64_t xr[6];_Atomic uint32_t n;_Atomic uint32_t ph[3];} watch_hits[WATCH_SLOTS];   /* xr: x0 x19 x20 x21 x22 x8; ph: hits by stereo B2 run (b2_in_second_run) */
extern void vr_stage_hook(void);
uint64_t g_b2_stage_return;
static int b2_in_second_run(void);
static void watch_set(thread_act_t t,int on){
    arm_debug_state64_t d;mach_msg_type_number_t c=ARM_DEBUG_STATE64_COUNT;
    if(thread_get_state(t,ARM_DEBUG_STATE64,(thread_state_t)&d,&c)!=KERN_SUCCESS)return;
    /* WCR: enable, EL0, load (or store), byte address select for the 4 bytes at watch_addr */
    d.__wvr[0]=on?(watch_addr&~7ULL):0;d.__wcr[0]=on?((0xfULL<<((watch_addr&7)+5))|((watch_store?2u:1u)<<3)|(2u<<1)|1u):0;
    thread_set_state(t,ARM_DEBUG_STATE64,(thread_state_t)&d,ARM_DEBUG_STATE64_COUNT);
}
/* VR camera search: a hardware breakpoint (BVR/BCR 0) on one instruction; a hit records x1 and disarms that thread. */
static uint64_t bp_addr;static int bp_reg=1;static _Atomic uint64_t bp_x1;static _Atomic uint32_t bp_n;
static int bp_collect;static struct{_Atomic uint64_t v;_Atomic uint32_t n,raw;} bp_seen[8];   /* "camsrc"; raw: copies of a matrix the hook did not write */
extern vr_cam_state *g_vr_headlook;
static int bp_hooked(uint64_t a){   /* the 48 bytes at a are one of the hook's ring matrices */
    const vr_cam_state *s=g_vr_headlook;if(!s||!a)return 1;
    for(int i=0;i<16;i++)if(!memcmp(s->ring[i].m,(const void *)(uintptr_t)a,48))return 1;
    return 0;
}
static void bp_set(thread_act_t t,int on){
    arm_debug_state64_t d;mach_msg_type_number_t c=ARM_DEBUG_STATE64_COUNT;
    if(thread_get_state(t,ARM_DEBUG_STATE64,(thread_state_t)&d,&c)!=KERN_SUCCESS)return;
    d.__bvr[0]=on?(bp_addr&~3ULL):0;d.__bcr[0]=on?((0xfULL<<5)|(2u<<1)|1u):0;   /* enable, EL0, all byte lanes */
    thread_set_state(t,ARM_DEBUG_STATE64,(thread_state_t)&d,ARM_DEBUG_STATE64_COUNT);
}
static void bp_all(int on){
    thread_act_array_t list;mach_msg_type_number_t n;thread_act_t self=mach_thread_self();
    if(task_threads(mach_task_self(),&list,&n)!=KERN_SUCCESS)return;
    for(unsigned i=0;i<n;i++){if(list[i]!=self){thread_suspend(list[i]);bp_set(list[i],on);thread_resume(list[i]);}mach_port_deallocate(mach_task_self(),list[i]);}
    vm_deallocate(mach_task_self(),(vm_address_t)list,n*sizeof *list);mach_port_deallocate(mach_task_self(),self);
}
static void watch_all(int on){
    thread_act_array_t list;mach_msg_type_number_t n;thread_act_t self=mach_thread_self();
    if(task_threads(mach_task_self(),&list,&n)!=KERN_SUCCESS)return;
    for(unsigned i=0;i<n;i++){if(list[i]!=self){thread_suspend(list[i]);watch_set(list[i],on);thread_resume(list[i]);}mach_port_deallocate(mach_task_self(),list[i]);}
    vm_deallocate(mach_task_self(),(vm_address_t)list,n*sizeof *list);mach_port_deallocate(mach_task_self(),self);
}
static int ft_trap(ucontext_t *u,unsigned ec);   /* "frametrace" (below) */
static int br_trap(ucontext_t *u,unsigned ec);   /* "bpregs" (below) */
int runtime_diag_watch_trap(void *context){
    ucontext_t *u=context;uint64_t pc=u->uc_mcontext->__ss.__pc,lr=u->uc_mcontext->__ss.__lr;
    unsigned ec=(unsigned)(u->uc_mcontext->__es.__esr>>26);
    if(ft_trap(u,ec))return 1;
    if(br_trap(u,ec))return 1;
    if(bp_addr&&(ec==0x30||ec==0x31)&&pc==bp_addr){   /* the VR camera search's breakpoint */
        atomic_store(&bp_x1,u->uc_mcontext->__ss.__x[bp_reg]);atomic_fetch_add(&bp_n,1);
        if(bp_collect){const uint64_t v=u->uc_mcontext->__ss.__x[bp_reg];   /* "camsrc": every distinct value, counted */
            for(unsigned i=0;i<8;i++){uint64_t z=0;if(atomic_compare_exchange_strong(&bp_seen[i].v,&z,v)||z==v){atomic_fetch_add(&bp_seen[i].n,1);if(!bp_hooked(v))atomic_fetch_add(&bp_seen[i].raw,1);break;}}}
        thread_act_t self=mach_thread_self();bp_set(self,0);mach_port_deallocate(mach_task_self(),self);return 1;}
    if(!watch_addr||(ec!=0x34&&ec!=0x35))return 0;   /* watchpoint */
    for(unsigned i=(unsigned)(pc>>2)%WATCH_SLOTS,k=0;k<WATCH_SLOTS;k++,i=(i+1)%WATCH_SLOTS){
        uint64_t z=0;if(atomic_compare_exchange_strong(&watch_hits[i].pc,&z,pc)||z==pc){atomic_store(&watch_hits[i].lr,lr);
            uint64_t fp=u->uc_mcontext->__ss.__fp;for(int d=0;d<3;d++){uint64_t fr[2]={0,0};if(!fp||(fp&15))break;memcpy(fr,(const void *)(uintptr_t)fp,16);atomic_store(&watch_hits[i].up[d],fr[1]);fp=fr[0];}
            atomic_store(&watch_hits[i].x1,u->uc_mcontext->__ss.__x[1]);
            {static const int rn[6]={0,19,20,21,22,8};for(int r=0;r<6;r++)atomic_store(&watch_hits[i].xr[r],u->uc_mcontext->__ss.__x[rn[r]]);}
            atomic_fetch_add(&watch_hits[i].ph[b2_in_second_run()],1);
            atomic_fetch_add(&watch_hits[i].n,1);break;}
    }
    thread_act_t self=mach_thread_self();watch_set(self,0);mach_port_deallocate(mach_task_self(),self);
    return 1;
}
static void *watch_run(void *arg){
    double secs=((double *)arg)[0];unsigned off=(unsigned)((double *)arg)[1];int in_x=((double *)arg)[2]!=0;free(arg);
    uint64_t obj;if(in_x)obj=time_object();else memcpy(&obj,diag_main->base+0x3ada848,8);
    for(unsigned i=0;i<WATCH_SLOTS;i++){atomic_store(&watch_hits[i].pc,0);atomic_store(&watch_hits[i].n,0);}
    watch_addr=obj+off;
    fprintf(stderr,"[totk] frmwatch: %s %#llx, watching +%#x for %.0f s\n",in_x?"X":"FrameRateMgr",(unsigned long long)obj,off,secs);
    for(double t=0;t<secs;t+=0.01){watch_all(1);usleep(10000);}
    watch_all(0);uint64_t base=(uintptr_t)diag_main->base;unsigned total=0;
    for(unsigned i=0;i<WATCH_SLOTS;i++){uint64_t pc=atomic_load(&watch_hits[i].pc);if(!pc)continue;uint32_t n=atomic_load(&watch_hits[i].n);total+=n;
        fprintf(stderr,"[totk] frmwatch hit main%+lld x%u lr main%+lld\n",(long long)(pc-base),n,(long long)(atomic_load(&watch_hits[i].lr)-base));}
    fprintf(stderr,"[totk] frmwatch done: %u hits\n",total);watch_addr=0;return NULL;
}
void runtime_diag_frm_watch(double secs,unsigned off,int in_x){
    if(watch_addr)runtime_fail("frmwatch: a watch is already running");
    double *a=malloc(3*sizeof *a);a[0]=secs;a[1]=off;a[2]=in_x;pthread_t t;pthread_create(&t,NULL,watch_run,a);pthread_detach(t);
}
/* VR stage 2 (diagnostic, TOTK_VR_FINDCAM=1, armed by the replay's camera scan, nvn_mtl.m vr_camscan): the 3x4 view
   matrix of the main camera block is a copy of the game's camera (sead LookAtCamera): search every writable region for
   its 48 bytes, print the matches, then put a hardware store watchpoint on the first one outside the GPU blocks for
   15 s and print who writes it (pc, return address and three more frames, module offsets). That writer is where the
   head's rotation goes in. */
static void vr_desc(uint64_t a,char *b,size_t n){extern void runtime_describe_address(uint64_t,char *,size_t);runtime_describe_address(a,b,n);}
/* One store-watch phase of 15 s on addr: prints every writer; returns x1 of the most frequent one (its source). */
static double vr_watch_secs=15;   /* "camwatch <seconds>" sets its own */
static uint64_t vr_watch_phase(uint64_t addr,int stage){
    for(unsigned i=0;i<WATCH_SLOTS;i++){atomic_store(&watch_hits[i].pc,0);atomic_store(&watch_hits[i].n,0);for(int k=0;k<3;k++)atomic_store(&watch_hits[i].ph[k],0);}
    watch_store=1;watch_addr=addr;
    fprintf(stderr,"[vr] findcam stage %d: store watchpoint on %#llx for %.0f s\n",stage,(unsigned long long)addr,vr_watch_secs);
    for(double t=0;t<vr_watch_secs;t+=0.01){watch_all(1);usleep(10000);}
    watch_all(0);unsigned total=0,best=0;uint64_t best_x1=0;
    for(unsigned i=0;i<WATCH_SLOTS;i++){uint64_t pc=atomic_load(&watch_hits[i].pc);if(!pc)continue;uint32_t n=atomic_load(&watch_hits[i].n);total+=n;
        char d0[96],d1[96],d2[96],d3[96],d4[96];vr_desc(pc,d0,96);vr_desc(atomic_load(&watch_hits[i].lr),d1,96);
        vr_desc(atomic_load(&watch_hits[i].up[0]),d2,96);vr_desc(atomic_load(&watch_hits[i].up[1]),d3,96);vr_desc(atomic_load(&watch_hits[i].up[2]),d4,96);
        uint64_t x1=atomic_load(&watch_hits[i].x1);
        fprintf(stderr,"[vr] findcam stage %d write x%u (B2 runs: first %u, second %u, after %u) pc %s lr %s < %s < %s < %s  x1 %#llx x0 %#llx x19 %#llx x20 %#llx x21 %#llx x22 %#llx x8 %#llx\n",stage,n,atomic_load(&watch_hits[i].ph[0]),atomic_load(&watch_hits[i].ph[1]),atomic_load(&watch_hits[i].ph[2]),d0,d1,d2,d3,d4,(unsigned long long)x1,
                (unsigned long long)atomic_load(&watch_hits[i].xr[0]),(unsigned long long)atomic_load(&watch_hits[i].xr[1]),(unsigned long long)atomic_load(&watch_hits[i].xr[2]),
                (unsigned long long)atomic_load(&watch_hits[i].xr[3]),(unsigned long long)atomic_load(&watch_hits[i].xr[4]),(unsigned long long)atomic_load(&watch_hits[i].xr[5]));
        if(n>best){best=n;best_x1=x1;}}
    fprintf(stderr,"[vr] findcam stage %d done: %u writes\n",stage,total);watch_addr=0;watch_store=0;
    return best_x1;
}
static void *vr_findcam_run(void *arg){
    float v[12];memcpy(v,arg,48);free(arg);
    extern uint64_t g_vr_cam_blocks[8];uint64_t hits[64];int nh=0;
    mach_vm_address_t a=0;mach_vm_size_t sz=0;vm_region_basic_info_data_64_t info;mach_msg_type_number_t cnt;mach_port_t obj;
    uint64_t scanned=0;
    for(;;){cnt=VM_REGION_BASIC_INFO_COUNT_64;
        if(mach_vm_region(mach_task_self(),&a,&sz,VM_REGION_BASIC_INFO_64,(vm_region_info_t)&info,&cnt,&obj)!=KERN_SUCCESS)break;
        if((info.protection&(VM_PROT_READ|VM_PROT_WRITE))==(VM_PROT_READ|VM_PROT_WRITE)&&sz<(8ULL<<30)){
            const uint8_t *p=(const uint8_t *)(uintptr_t)a,*end=p+sz;scanned+=sz;
            while(p+48<=end&&nh<64){const uint8_t *q=memmem(p,(size_t)(end-p),v,48);if(!q)break;
                if(q!=(const uint8_t *)v)hits[nh++]=(uintptr_t)q;p=q+4;}
        }
        a+=sz;
    }
    fprintf(stderr,"[vr] findcam: %llu MB scanned, %d copies of the view matrix\n",(unsigned long long)(scanned>>20),nh);
    /* The camera position from the view matrix (rows R | t): -R^T t. sead's LookAtCamera keeps its vtable just before
       the matrix and its position (then look-at point and up) just after it: such a copy is the camera object itself. */
    float cp[3];for(int c=0;c<3;c++)cp[c]=-(v[0*4+c]*v[3]+v[1*4+c]*v[7]+v[2*4+c]*v[11]);
    uint64_t target=0,first=0;
    for(int i=0;i<nh;i++){int gpu=0;for(int k=0;k<8;k++)gpu|=g_vr_cam_blocks[k]&&hits[i]>=g_vr_cam_blocks[k]&&hits[i]<g_vr_cam_blocks[k]+2368;
        uint64_t vt=0;float after[9]={0};mach_vm_size_t got=0;   /* safe reads: the copy may start or end a region */
        mach_vm_read_overwrite(mach_task_self(),hits[i]-8,8,(mach_vm_address_t)(uintptr_t)&vt,&got);mach_vm_read_overwrite(mach_task_self(),hits[i]+48,36,(mach_vm_address_t)(uintptr_t)after,&got);
        char d[96],dv[96];vr_desc(hits[i],d,sizeof d);vr_desc(vt,dv,sizeof dv);
        int is_vt=!strncmp(dv,"main+",5),at_pos=fabsf(after[0]-cp[0])<0.05f&&fabsf(after[1]-cp[1])<0.05f&&fabsf(after[2]-cp[2])<0.05f;
        fprintf(stderr,"[vr] findcam copy %#llx%s before %s after %.2f %.2f %.2f | %.2f %.2f %.2f | %.3f %.3f %.3f%s\n",(unsigned long long)hits[i],gpu?" (camera block)":"",dv,
                after[0],after[1],after[2],after[3],after[4],after[5],after[6],after[7],after[8],at_pos?(is_vt?"  <- camera object":"  <- position follows"):"");
        if(!gpu&&!first)first=hits[i];
        if(!gpu&&at_pos&&(!target||is_vt))target=hits[i];}   /* the position right after the matrix; a vtable before it wins */
    fprintf(stderr,"[vr] findcam: camera position %.2f %.2f %.2f\n",cp[0],cp[1],cp[2]);
    if(!target)target=first;
    if(!target||watch_addr){fprintf(stderr,"[vr] findcam: %s\n",target?"a watch is already running":"no copy outside the GPU blocks");return NULL;}
    /* Stage 1 watches the copy; its writer usually copies from the camera's own matrix, whose address it holds in x1
       (main+0x56c8ac: ldr x1,[x15,#0x730]; ldr q2,[x1]; stur q2,[x0,#0x48]). Stage 2 watches that source. */
    uint64_t src=vr_watch_phase(target,1);
    if(src&&src!=target)vr_watch_phase(src,2);
    return NULL;
}
/* Found 2026-10-26: main+0x56c8ac (1.4.3) copies the main camera's view matrix into a per-view record: ldr x1,[x15,#0x730];
   ldr q2,[x1]; stur q2,[x0,#0x48]. A breakpoint there gives x1, the camera's own matrix; a store watch on it, its writer. */
/* TOTK_VR_FINDCAM=src: breakpoint main+0x56c8ac, x1. =cam: one step further up, the call main+0xbea74 -> main+0xb33e8
   (sets view 0 of a list from a camera): x2 = x9 + 8 = the camera's matrix, the camera object 8 bytes before it. The
   callee itself serves other callers too (a camera on a thread's stack hit first). */
static void *vr_findsrc_run(void *arg){
    int cam=arg!=NULL;bp_reg=cam?2:1;
    bp_addr=(uintptr_t)diag_main->base+(cam?0xbea74:0x56c8ac);atomic_store(&bp_x1,0);atomic_store(&bp_n,0);
    for(int t=0;t<300&&!atomic_load(&bp_n);t++){bp_all(1);usleep(10000);}
    bp_all(0);uint64_t src=atomic_load(&bp_x1);bp_addr=0;
    float m[12]={0};if(src)memcpy(m,(const void *)(uintptr_t)src,48);
    fprintf(stderr,"[vr] findcam breakpoint main+%#x: %u hits, x%d %#llx: %.3f %.3f %.3f %.2f | %.3f %.3f %.3f %.2f | %.3f %.3f %.3f %.2f\n",
            cam?0xbea74:0x56c8ac,atomic_load(&bp_n),bp_reg,(unsigned long long)src,m[0],m[1],m[2],m[3],m[4],m[5],m[6],m[7],m[8],m[9],m[10],m[11]);
    if(src&&cam){   /* the camera object: its vtable, and the 64 bytes after the matrix (sead LookAtCamera: position, look-at, up) */
        uint64_t vt=0;float f[16]={0};memcpy(&vt,(const void *)(uintptr_t)(src-8),8);memcpy(f,(const void *)(uintptr_t)(src+48),64);
        char d[96];vr_desc(vt,d,sizeof d);
        fprintf(stderr,"[vr] findcam camera object %#llx vtable %s; after the matrix:",(unsigned long long)(src-8),d);
        for(int i=0;i<16;i++)fprintf(stderr," %.3f",f[i]);fprintf(stderr,"\n");}
    if(src)vr_watch_phase(src,cam?3:2);
    return NULL;
}
/* VR first person, stage 1 of hiding the head (user 2026-10-26: "第一人称 我希望 去掉头 或者不显示头！ 其他部位显示"),
   diagnostic "flag vr_findhead 1" then (after the player crouches) "flag vr_findhead 2": candidate bone matrices near
   the player's head, in world space (translation within 0.7 m of position + 1.5 m up) or model space (|x|,|z| < 0.4,
   1.0 < y < 1.9), as 3x4 rows (sead Matrix34) or 4x4 columns (GPU), with orthonormal-ish rotation, 16-byte aligned;
   stage 2 lists those that moved down by 0.2-0.9 m (the head and neck), with their memory region. */
enum{HEAD_MAX=8192};
static struct{uint64_t a;float t[3];uint8_t layout,space;} head_c[HEAD_MAX];static int head_n;
static int head_rows_ok(const float *r,int stride){   /* three rotation vectors of length about 1, roughly orthogonal */
    for(int i=0;i<3;i++){float l=r[i*stride]*r[i*stride]+r[i*stride+1]*r[i*stride+1]+r[i*stride+2]*r[i*stride+2];if(!(l>0.81f&&l<1.21f))return 0;}
    float d=r[0]*r[stride]+r[1]*r[stride+1]+r[2]*r[stride+2];return fabsf(d)<0.15f;
}
static void head_tr(const float *f,int layout,float t[3]){if(layout==0){t[0]=f[3];t[1]=f[7];t[2]=f[11];}else{t[0]=f[12];t[1]=f[13];t[2]=f[14];}}
static void *vr_findhead_run(void *arg){
    int stage=(int)(intptr_t)arg;
    extern uintptr_t runtime_player_pos_address(void);uintptr_t pa=runtime_player_pos_address();float pos[3]={0};
    if(!pa){fprintf(stderr,"[vr] findhead: no player\n");return NULL;}memcpy(pos,(const void *)pa,12);
    float head[3]={pos[0],pos[1]+1.5f,pos[2]};
    if(stage==1){
        head_n=0;mach_vm_address_t a=0;mach_vm_size_t sz=0;vm_region_basic_info_data_64_t info;mach_msg_type_number_t cnt;mach_port_t obj;uint64_t scanned=0;
        for(;;){cnt=VM_REGION_BASIC_INFO_COUNT_64;
            if(mach_vm_region(mach_task_self(),&a,&sz,VM_REGION_BASIC_INFO_64,(vm_region_info_t)&info,&cnt,&obj)!=KERN_SUCCESS)break;
            if((info.protection&(VM_PROT_READ|VM_PROT_WRITE))==(VM_PROT_READ|VM_PROT_WRITE)&&sz<(8ULL<<30)){scanned+=sz;
                const float *f=(const float *)(uintptr_t)a,*end=(const float *)(uintptr_t)(a+sz)-16;
                for(;f<=end&&head_n<HEAD_MAX;f+=4)for(int layout=0;layout<2;layout++){
                    float t[3];head_tr(f,layout,t);
                    int world=fabsf(t[0]-head[0])<0.7f&&fabsf(t[1]-head[1])<0.7f&&fabsf(t[2]-head[2])<0.7f;
                    int model=!world&&fabsf(t[0])<0.4f&&t[1]>1.0f&&t[1]<1.9f&&fabsf(t[2])<0.4f;
                    if(!world&&!model)continue;
                    if(!(layout==0?head_rows_ok(f,4):head_rows_ok(f,4)))continue;   /* rows at 0,4,8 (3x4) or columns at 0,4,8 (4x4) */
                    if(layout==1&&!(f[3]==0&&f[7]==0&&f[11]==0&&fabsf(f[15]-1)<1e-4f))continue;
                    head_c[head_n].a=(uint64_t)(uintptr_t)f;memcpy(head_c[head_n].t,t,12);head_c[head_n].layout=(uint8_t)layout;head_c[head_n].space=(uint8_t)!world;head_n++;
                }}
            a+=sz;}
        fprintf(stderr,"[vr] findhead stage 1: player %.2f %.2f %.2f, %llu MB scanned, %d candidates (world / model space bone matrices near the head)\n",
                pos[0],pos[1],pos[2],(unsigned long long)(scanned>>20),head_n);
        return NULL;
    }
    int shown=0;
    for(int i=0;i<head_n;i++){
        float f[16];mach_vm_size_t got=0;if(mach_vm_read_overwrite(mach_task_self(),head_c[i].a,64,(mach_vm_address_t)(uintptr_t)f,&got)||got!=64)continue;
        float t[3];head_tr(f,head_c[i].layout,t);float dy=t[1]-head_c[i].t[1],dx=t[0]-head_c[i].t[0],dz=t[2]-head_c[i].t[2];
        if(!(dy<-0.2f&&dy>-0.9f))continue;
        char d[96];vr_desc(head_c[i].a,d,sizeof d);
        if(shown++<60)fprintf(stderr,"[vr] findhead moved %#llx %s %s: y %.3f -> %.3f (dx %.2f dz %.2f) %s\n",(unsigned long long)head_c[i].a,
            head_c[i].layout?"4x4":"3x4",head_c[i].space?"model":"world",head_c[i].t[1],t[1],dx,dz,d);
    }
    fprintf(stderr,"[vr] findhead stage 2: player %.2f %.2f %.2f, %d of %d candidates moved down 0.2-0.9 m\n",pos[0],pos[1],pos[2],shown,head_n);
    /* TOTK_VR_HEADDUMP=<folder>: the 16 KB around each moved model-space 4x4 candidate (the skinning palettes), raw */
    const char *dir=getenv("TOTK_VR_HEADDUMP");uint64_t last=0;
    for(int i=0;dir&&i<head_n;i++){if(!(head_c[i].layout==1&&head_c[i].space==1))continue;
        uint64_t base=(head_c[i].a-0x2000)&~0xfffULL;if(base==last)continue;last=base;
        static uint8_t buf[0x4000];mach_vm_size_t got=0;if(mach_vm_read_overwrite(mach_task_self(),base,sizeof buf,(mach_vm_address_t)(uintptr_t)buf,&got)||got!=sizeof buf)continue;
        char path[1024];snprintf(path,sizeof path,"%s/pal-%llx.bin",dir,(unsigned long long)base);FILE *o=fopen(path,"wb");if(o){fwrite(buf,1,sizeof buf,o);fclose(o);}
        fprintf(stderr,"[vr] findhead dumped %s\n",path);}
    return NULL;
}
void runtime_vr_find_head(int stage){pthread_t t;pthread_create(&t,NULL,vr_findhead_run,(void *)(intptr_t)stage);pthread_detach(t);}
static void runtime_vr_find_camera(const float *view){
    pthread_t t;
    const char *mode=getenv("TOTK_VR_FINDCAM");
    if(mode&&(!strcmp(mode,"src")||!strcmp(mode,"cam"))){pthread_create(&t,NULL,vr_findsrc_run,!strcmp(mode,"cam")?(void *)1:NULL);pthread_detach(t);return;}
    float *a=malloc(48);memcpy(a,view,48);pthread_create(&t,NULL,vr_findcam_run,a);pthread_detach(t);
}
/* "camwatch <seconds>" (diagnostic, user 2026-11-01: the screen shakes while the Zonai device thumbnails load; the frame log
   showed the G-buffer camera flipping between the hook's turned camera and the game's own, 1 m apart, on frames with and
   without a hook call): a store watchpoint on the main camera's matrix (g_vr_cam.cam + 8, recorded at main+0xbea74) for
   that long; every writer's pc and callers. The hook's own store shows outside main. */
static void *vr_camwatch_run(void *arg){
    extern vr_cam_state g_vr_cam;vr_watch_secs=((double *)arg)[0];const int src=((double *)arg)[1]!=0;free(arg);
    /* src: the matrix the per-view record copies (the busiest x1 of the last "camsrc") instead of the main camera's */
    uint64_t a=g_vr_cam.cam?g_vr_cam.cam+8:0;
    if(src){a=0;uint32_t best=0;for(unsigned i=0;i<8;i++)if(atomic_load(&bp_seen[i].n)>best){best=atomic_load(&bp_seen[i].n);a=atomic_load(&bp_seen[i].v);}}
    if(!a){fprintf(stderr,"[vr] camwatch: %s\n",src?"no camsrc result yet":"no main camera seen yet");return NULL;}
    vr_watch_phase(a,src?5:4);vr_watch_secs=15;return NULL;
}
/* "camsrc <seconds>" (diagnostic, same shaking): a breakpoint on main+0x56c8ac (ldr q2,[x1]: the per-view record copies
   a camera's view matrix from x1) for that long; every distinct x1 with its count, and the main camera's matrix
   (g_vr_cam.cam + 8) for comparison. A second source during the thumbnails is a second camera feeding the picture. */
static void *vr_camsrc_run(void *arg){
    extern vr_cam_state g_vr_cam;const double secs=*(double *)arg;free(arg);
    for(unsigned i=0;i<8;i++){atomic_store(&bp_seen[i].v,0);atomic_store(&bp_seen[i].n,0);atomic_store(&bp_seen[i].raw,0);}
    bp_reg=1;bp_collect=1;bp_addr=(uintptr_t)diag_main->base+0x56c8ac;
    fprintf(stderr,"[vr] camsrc: breakpoint main+0x56c8ac for %.0f s; main camera matrix at %#llx\n",secs,(unsigned long long)(g_vr_cam.cam?g_vr_cam.cam+8:0));
    for(double t=0;t<secs;t+=0.01){bp_all(1);usleep(10000);}
    bp_all(0);bp_addr=0;bp_collect=0;
    for(unsigned i=0;i<8;i++){const uint64_t v=atomic_load(&bp_seen[i].v);if(!v)continue;char d[96];vr_desc(v,d,sizeof d);
        float m[12]={0};mach_vm_size_t got=0;mach_vm_read_overwrite(mach_task_self(),v,48,(mach_vm_address_t)(uintptr_t)m,&got);
        float e[3];for(int j=0;j<3;j++)e[j]=-(m[j]*m[3]+m[4+j]*m[7]+m[8+j]*m[11]);
        fprintf(stderr,"[vr] camsrc x1 %#llx (%s) x%u (not the hook's: %u)%s: camera %.2f %.2f %.2f fwd %.3f %.3f %.3f\n",(unsigned long long)v,d,atomic_load(&bp_seen[i].n),atomic_load(&bp_seen[i].raw),
                v==g_vr_cam.cam+8?" = the main camera":"",e[0],e[1],e[2],m[8],m[9],m[10]);}
    fprintf(stderr,"[vr] camsrc done\n");return NULL;
}
void runtime_diag_cam_src(double secs){
    if(bp_addr){fprintf(stderr,"[vr] camsrc: a breakpoint is already set\n");return;}
    double *a=malloc(sizeof *a);*a=secs;pthread_t t;pthread_create(&t,NULL,vr_camsrc_run,a);pthread_detach(t);
}
/* "frametrace <seconds> <hex offset> [<hex offset>...]" (diagnostic, stereo B2 survey 2026-11-02: where one game update
   ends and its drawing starts, so that the scene can be drawn twice per update): hardware breakpoints (BVR 1-4) on up to
   four main instructions in every thread, re-armed every 10 ms; each hit records its site, thread, time and up to 20
   return addresses up the frame chain. The game's submits, presents and command-buffer recordings (hle_nvn.c,
   runtime_frametrace_event) go into the same log. At the end: each distinct (site, thread, call chain) with its count,
   then the first 600 events in time
   order (which thread computes the camera, prepares the views, submits, presents, and in what order within a frame). */
enum{FT_SITES=4,FT_MAX=16384,FT_DEPTH=20};
static uint64_t ft_site[FT_SITES];static int ft_nsite;static _Atomic int ft_on;static _Atomic uint32_t ft_n;
static struct{uint64_t t,tid,ret[FT_DEPTH];uint8_t site,depth;} ft_ev[FT_MAX];
static void ft_record(int site,const uint64_t *ret,int depth){
    if(!atomic_load_explicit(&ft_on,memory_order_relaxed))return;
    const uint32_t i=atomic_fetch_add(&ft_n,1);if(i>=FT_MAX)return;
    uint64_t tid=0;pthread_threadid_np(NULL,&tid);
    ft_ev[i].t=clock_gettime_nsec_np(CLOCK_UPTIME_RAW);ft_ev[i].tid=tid;ft_ev[i].site=(uint8_t)site;ft_ev[i].depth=(uint8_t)depth;
    for(int d=0;d<depth;d++)ft_ev[i].ret[d]=ret[d];
}
/* 8 submit, 9 present, 10/11 begin/end recording (hle_nvn.c). Submits and recording starts also get the guest's call
   chain: the host frames lead through the HLE stack switch's frame record (hle_stack.S) to the guest caller; frames are
   read with mach_vm_read_overwrite since the walk crosses from the host stack to the guest's. */
static void b2_calls_present(void);
void runtime_frametrace_event(int kind){
    if(kind==9)b2_calls_present();   /* "b2calls" frames are counted between presents */
    if(!atomic_load_explicit(&ft_on,memory_order_relaxed))return;
    uint64_t ret[FT_DEPTH];int depth=0;
    if(kind==8||kind==10){const uint64_t mlo=(uintptr_t)diag_main->base,mhi=mlo+diag_main->size;uint64_t fp=(uintptr_t)__builtin_frame_address(0);int guest=0;
        for(int k=0;k<64&&depth<FT_DEPTH&&fp&&!(fp&15);k++){uint64_t fr[2];mach_vm_size_t got=0;
            if(mach_vm_read_overwrite(mach_task_self(),fp,16,(mach_vm_address_t)(uintptr_t)fr,&got)!=KERN_SUCCESS||got!=16)break;
            if(!guest&&fr[1]>=mlo&&fr[1]<mhi)guest=1;
            if(guest)ret[depth++]=fr[1];
            fp=fr[0];}}
    ft_record(kind,ret,depth);
}
static void ft_set(thread_act_t t,int slot,int on){   /* slot -1: all sites */
    arm_debug_state64_t d;mach_msg_type_number_t c=ARM_DEBUG_STATE64_COUNT;
    if(thread_get_state(t,ARM_DEBUG_STATE64,(thread_state_t)&d,&c)!=KERN_SUCCESS)return;
    for(int k=0;k<ft_nsite;k++)if(slot<0||slot==k){d.__bvr[k+1]=on?(ft_site[k]&~3ULL):0;d.__bcr[k+1]=on?((0xfULL<<5)|(2u<<1)|1u):0;}
    thread_set_state(t,ARM_DEBUG_STATE64,(thread_state_t)&d,ARM_DEBUG_STATE64_COUNT);
}
static void ft_all(int on){
    thread_act_array_t list;mach_msg_type_number_t n;thread_act_t self=mach_thread_self();
    if(task_threads(mach_task_self(),&list,&n)!=KERN_SUCCESS)return;
    for(unsigned i=0;i<n;i++){if(list[i]!=self){thread_suspend(list[i]);ft_set(list[i],-1,on);thread_resume(list[i]);}mach_port_deallocate(mach_task_self(),list[i]);}
    vm_deallocate(mach_task_self(),(vm_address_t)list,n*sizeof *list);mach_port_deallocate(mach_task_self(),self);
}
static int ft_trap(ucontext_t *u,unsigned ec){   /* from runtime_diag_watch_trap: 1 when the breakpoint was one of ours */
    if(!ft_nsite||(ec!=0x30&&ec!=0x31))return 0;   /* by address, not ft_on: a thread can hit a site while the end clears them */
    const uint64_t pc=u->uc_mcontext->__ss.__pc;int k=0;while(k<ft_nsite&&ft_site[k]!=pc)k++;
    if(k==ft_nsite)return 0;
    uint64_t ret[FT_DEPTH];int depth=0;ret[depth++]=u->uc_mcontext->__ss.__lr;
    const uint64_t sp=u->uc_mcontext->__ss.__sp;uint64_t fp=u->uc_mcontext->__ss.__fp;
    while(depth<FT_DEPTH&&fp&&!(fp&15)&&fp>=sp&&fp<sp+(1u<<20)){uint64_t fr[2];memcpy(fr,(const void *)(uintptr_t)fp,16);
        ret[depth++]=fr[1];if(fr[0]<=fp)break;fp=fr[0];}   /* callers' frames sit higher on the stack */
    ft_record(k,ret,depth);
    thread_act_t self=mach_thread_self();ft_set(self,k,0);mach_port_deallocate(mach_task_self(),self);return 1;
}
static int ft_cmp(const void *a,const void *b){const uint64_t x=*(const uint64_t *)a,y=*(const uint64_t *)b;return x<y?-1:x>y;}
static void *ft_run(void *arg){
    const double secs=*(double *)arg;free(arg);const uint64_t base=(uintptr_t)diag_main->base;
    atomic_store(&ft_n,0);atomic_store(&ft_on,1);
    fprintf(stderr,"[vr] frametrace: %d breakpoints for %.0f s:",ft_nsite,secs);for(int k=0;k<ft_nsite;k++)fprintf(stderr," %d=main+%#llx",k,(unsigned long long)(ft_site[k]-base));fprintf(stderr," 8=submit 9=present 10=begin recording 11=end recording\n");
    for(double t=0;t<secs;t+=0.01){ft_all(1);usleep(10000);}
    ft_all(0);atomic_store(&ft_on,0);usleep(20000);   /* the sites stay known until the end (ft_trap) */
    uint32_t n=atomic_load(&ft_n);if(n>FT_MAX)n=FT_MAX;
    static uint64_t order[FT_MAX];   /* time order: (time - first) << 14 | index */
    uint64_t t0=~0ULL;for(uint32_t i=0;i<n;i++)if(ft_ev[i].t<t0)t0=ft_ev[i].t;
    for(uint32_t i=0;i<n;i++)order[i]=((ft_ev[i].t-t0)/1000)<<14|i;qsort(order,n,8,ft_cmp);
    uint64_t tids[32];int nt=0;   /* threads, numbered in order of first event */
    #define FT_TNUM(x) ({int _j=0;while(_j<nt&&tids[_j]!=(x))_j++;if(_j==nt&&nt<32)tids[nt++]=(x);_j;})
    for(uint32_t j=0;j<n;j++)(void)FT_TNUM(ft_ev[order[j]&16383].tid);
    /* distinct chains */
    static uint8_t done[FT_MAX];memset(done,0,n);
    for(uint32_t j=0;j<n;j++){const uint32_t i=order[j]&16383;if(done[i])continue;unsigned cnt=0;
        for(uint32_t q=j;q<n;q++){const uint32_t m=order[q]&16383;if(done[m]||ft_ev[m].site!=ft_ev[i].site||ft_ev[m].tid!=ft_ev[i].tid||ft_ev[m].depth!=ft_ev[i].depth||memcmp(ft_ev[m].ret,ft_ev[i].ret,8*ft_ev[i].depth))continue;done[m]=1;cnt++;}
        char line[2400];size_t u=(size_t)snprintf(line,sizeof line,"[vr] frametrace chain site %d thread T%d x%u:",ft_ev[i].site,FT_TNUM(ft_ev[i].tid),cnt);
        for(int d=0;d<ft_ev[i].depth&&u<sizeof line;d++){char s[96];vr_desc(ft_ev[i].ret[d],s,sizeof s);u+=(size_t)snprintf(line+u,sizeof line-u," < %s",s);}
        fprintf(stderr,"%s\n",line);}
    for(uint32_t j=0;j<n&&j<600;j++){const uint32_t i=order[j]&16383;
        fprintf(stderr,"[vr] frametrace %8.3f ms T%d site %d\n",(ft_ev[i].t-t0)/1e6,FT_TNUM(ft_ev[i].tid),ft_ev[i].site);}
    #undef FT_TNUM
    fprintf(stderr,"[vr] frametrace done: %u events, %d threads\n",n,nt);ft_nsite=0;return NULL;
}
void runtime_diag_frame_trace(double secs,const char *offs){
    if(ft_nsite||atomic_load(&ft_on)){fprintf(stderr,"[vr] frametrace: already running\n");return;}
    int n=0;for(const char *p=offs;*p&&n<FT_SITES;){char *e;unsigned long long o=strtoull(p,&e,16);if(e==p)break;
        if(o>=diag_main->size){fprintf(stderr,"[vr] frametrace: main+%#llx is outside main\n",o);return;}ft_site[n++]=(uintptr_t)diag_main->base+o;p=e;while(*p==' ')p++;}
    if(!n){fprintf(stderr,"[vr] frametrace: no addresses\n");return;}
    ft_nsite=n;double *a=malloc(sizeof *a);*a=secs;pthread_t t;pthread_create(&t,NULL,ft_run,a);pthread_detach(t);
}
/* "jobdump" (diagnostic, stereo B2 2026-11-02): the frame's calc runs its render work as job lists on one queue: at
   main+0x3e5abc (bl main+0x3e60b0: queue [x19+0x4b8], x1 [x19+0x4c0], x2 the list [x19+0x4d8]) a breakpoint takes x19;
   then the lists at x19+0x4c8, +0x4d0, +0x4d8 (count at +8, array at +0x10 of pointers to entries: tag, job) are printed: per job its
   first 12 words (main addresses as main+off, pointers to text as the text), its vtable's first 6 entries. Read with
   mach_vm_read_overwrite: nothing is assumed about the objects. */
static int jd_read(uint64_t a,void *b,size_t n){mach_vm_size_t got=0;return a&&mach_vm_read_overwrite(mach_task_self(),a,n,(mach_vm_address_t)(uintptr_t)b,&got)==KERN_SUCCESS&&got==n;}
static void jd_word(uint64_t v,char *o,size_t n){
    const uint64_t lo=(uintptr_t)diag_main->base,hi=lo+diag_main->size;
    if(v>=lo&&v<hi){snprintf(o,n,"main+%#llx",(unsigned long long)(v-lo));return;}
    char s[33]={0};int k=0;
    if(v>0x100000000ULL&&v<0x0001000000000000ULL&&jd_read(v,s,32))while(k<32&&s[k]>=0x20&&s[k]<0x7f)k++;
    if(k>=4&&(k==32||!s[k])){s[k]=0;snprintf(o,n,"\"%s\"",s);return;}
    snprintf(o,n,"%#llx",(unsigned long long)v);
}
static void *jd_run(void *arg){
    (void)arg;bp_reg=19;bp_addr=(uintptr_t)diag_main->base+0x3e5abc;atomic_store(&bp_x1,0);atomic_store(&bp_n,0);
    for(int t=0;t<300&&!atomic_load(&bp_n);t++){bp_all(1);usleep(10000);}
    bp_all(0);const uint64_t x19=atomic_load(&bp_x1);bp_addr=0;bp_reg=1;
    if(!x19){fprintf(stderr,"[vr] jobdump: the breakpoint at main+0x3e5abc was not hit in 3 s\n");return NULL;}
    uint64_t f[8];if(jd_read(x19+0x4b8,f,0x28))fprintf(stderr,"[vr] jobdump: x19 %#llx, queue %#llx, +0x4c0 %#llx\n",(unsigned long long)x19,(unsigned long long)f[0],(unsigned long long)f[1]);
    for(unsigned off=0x4c8;off<=0x4d8;off+=8){
        uint64_t list=0;int32_t cnt=0;uint64_t arr=0;
        if(!jd_read(x19+off,&list,8)||!jd_read(list+8,&cnt,4)||!jd_read(list+0x10,&arr,8)){fprintf(stderr,"[vr] jobdump list +%#x: unreadable\n",off);continue;}
        fprintf(stderr,"[vr] jobdump list +%#x @%#llx: %d jobs\n",off,(unsigned long long)list,cnt);
        for(int i=0;i<cnt&&i<256;i++){
            /* the array holds pointers to 24-byte entries: a tag (priority, group, index), the job object, -1 */
            uint64_t ent=0,e2[2],job=0,w[12],vt[6];
            if(!jd_read(arr+8*(uint64_t)i,&ent,8)||!jd_read(ent,e2,16)||!(job=e2[1])||!jd_read(job,w,sizeof w)){fprintf(stderr,"[vr] jobdump  %d: unreadable\n",i);continue;}
            char line[1600];size_t u=(size_t)snprintf(line,sizeof line,"[vr] jobdump  +%#x %d tag %#llx job %#llx:",off,i,(unsigned long long)e2[0],(unsigned long long)job);
            for(int k=0;k<12&&u<sizeof line;k++){char d[64];jd_word(w[k],d,sizeof d);u+=(size_t)snprintf(line+u,sizeof line-u," %s",d);}
            if(jd_read(w[0],vt,sizeof vt)){u+=(size_t)snprintf(line+u,sizeof line-u," | vt");for(int k=0;k<6&&u<sizeof line;k++){char d[64];jd_word(vt[k],d,sizeof d);u+=(size_t)snprintf(line+u,sizeof line-u," %s",d);}}
            fprintf(stderr,"%s\n",line);
        }
    }
    fprintf(stderr,"[vr] jobdump done\n");return NULL;
}
/* "calctree" (diagnostic, stereo B2): the method tree the frame's calc walks (procCalc main+0x2dad80 calls callRec
   main+0x8384 on its root at main+0x2dadd8, x0). Walked as callRec does: flags byte +0x7c (bit 0/1: skip the node's
   method / its children), the delegate at +0x50 (vtable, object +0x58, method +0x60, adjustment +0x68: odd = virtual,
   the method is then a vtable offset), children: link = [node+0x28], child = [link+0x20], next link = [child+0x30].
   Each node: depth, flags, the method (main+off, or the object's vtable entry for a virtual one), the object's first
   words (names show as text). */
static int ct_n;
static void ct_visit(uint64_t node,int depth){
    if(ct_n++>600||depth>24)return;
    uint8_t fl=0;uint64_t d[4]={0},ow[6]={0};jd_read(node+0x7c,&fl,1);jd_read(node+0x50,d,32);
    uint64_t fn=d[2];const uint64_t adj=d[3],obj=d[1]+(uint64_t)((int64_t)adj>>1);
    if((adj&1)&&obj){uint64_t vt=0;if(jd_read(obj,&vt,8))jd_read(vt+fn,&fn,8);}
    jd_read(obj,ow,sizeof ow);
    char line[1200];size_t u=(size_t)snprintf(line,sizeof line,"[vr] calctree %*s%#llx fl %x",depth*2,"",(unsigned long long)node,fl);
    char s[64];jd_word(d[0],s,sizeof s);u+=(size_t)snprintf(line+u,sizeof line-u," dvt %s",s);
    jd_word(fn,s,sizeof s);u+=(size_t)snprintf(line+u,sizeof line-u," fn %s%s obj %#llx:",s,(adj&1)?" (virtual)":"",(unsigned long long)obj);
    for(int k=0;k<6&&u<sizeof line;k++){jd_word(ow[k],s,sizeof s);u+=(size_t)snprintf(line+u,sizeof line-u," %s",s);}
    fprintf(stderr,"%s\n",line);
    uint64_t link=0;jd_read(node+0x28,&link,8);
    for(int i=0;link&&i<200;i++){uint64_t child=0;if(!jd_read(link+0x20,&child,8)||!child)break;ct_visit(child,depth+1);if(!jd_read(child+0x30,&link,8))break;}
}
static void *ct_run(void *arg){
    (void)arg;bp_reg=0;bp_addr=(uintptr_t)diag_main->base+0x2dadd8;atomic_store(&bp_x1,0);atomic_store(&bp_n,0);
    for(int t=0;t<300&&!atomic_load(&bp_n);t++){bp_all(1);usleep(10000);}
    bp_all(0);const uint64_t root=atomic_load(&bp_x1);bp_addr=0;bp_reg=1;
    if(!root){fprintf(stderr,"[vr] calctree: the breakpoint at main+0x2dadd8 was not hit in 3 s\n");return NULL;}
    ct_n=0;ct_visit(root,0);fprintf(stderr,"[vr] calctree done: %d nodes\n",ct_n);return NULL;
}
void runtime_diag_calc_tree(void){
    if(bp_addr){fprintf(stderr,"[vr] calctree: a breakpoint is already set\n");return;}
    pthread_t t;pthread_create(&t,NULL,ct_run,NULL);pthread_detach(t);
}
/* "dumpmain <file>" (diagnostic, stereo B2): main's whole loaded image as it is now (relocated: GOT slots and vtables hold
   real addresses), with its base in a ".base" file beside it, for static analysis outside the game. Kept outside the
   repository (the user's own game code). "peek <hex address> [n]" / "peekm <hex main offset> [n]": n (default 8, at
   most 64) words from memory, main addresses as main+off, text as text. */
void runtime_diag_dump_main(const char *path){
    FILE *f=fopen(path,"wb");if(!f){fprintf(stderr,"[vr] dumpmain: cannot write %s\n",path);return;}
    static uint8_t buf[1<<20];size_t done=0;const uint64_t base=(uintptr_t)diag_main->base;
    while(done<diag_main->size){size_t n=diag_main->size-done<sizeof buf?diag_main->size-done:sizeof buf;
        if(!jd_read(base+done,buf,n))memset(buf,0,n);fwrite(buf,1,n,f);done+=n;}
    fclose(f);char p2[1100];snprintf(p2,sizeof p2,"%s.base",path);f=fopen(p2,"w");if(f){fprintf(f,"%#llx %#llx\n",(unsigned long long)base,(unsigned long long)diag_main->size);fclose(f);}
    fprintf(stderr,"[vr] dumpmain: %zu bytes from %#llx to %s\n",done,(unsigned long long)base,path);
}
void runtime_diag_peek(uint64_t a,int n,int rel){
    if(rel)a+=(uintptr_t)diag_main->base;if(n<1)n=8;if(n>64)n=64;
    for(int i=0;i<n;i+=4){char line[600];size_t u=(size_t)snprintf(line,sizeof line,"[vr] peek %#llx:",(unsigned long long)(a+8*(uint64_t)i));
        for(int k=i;k<n&&k<i+4;k++){uint64_t v=0;char d[64];if(!jd_read(a+8*(uint64_t)k,&v,8)){u+=(size_t)snprintf(line+u,sizeof line-u," ??");continue;}jd_word(v,d,sizeof d);u+=(size_t)snprintf(line+u,sizeof line-u," %s",d);}
        fprintf(stderr,"%s\n",line);}
}
/* "watchw <hex address> <seconds>" (diagnostic, stereo B2): a store watchpoint on the 4 bytes at the address for that
   long, every writer printed (vr_watch_phase: pc, return addresses, registers). "bpregs <hex main offset>": a one-shot
   breakpoint (BVR 5) in every thread; the first hit prints x0-x28, sp, the return address and 3 callers. */
/* "watchr <hex address> <seconds>": the same for loads. */
static void *ww_run(void *arg){uint64_t *a=arg;const uint64_t addr=a[0];vr_watch_secs=(double)a[1];const int load=(int)a[2];free(a);
    if(load)watch_store=0;else watch_store=1;
    for(unsigned i=0;i<WATCH_SLOTS;i++){atomic_store(&watch_hits[i].pc,0);atomic_store(&watch_hits[i].n,0);}
    if(load){watch_addr=addr;fprintf(stderr,"[vr] findcam stage 9: load watchpoint on %#llx for %.0f s\n",(unsigned long long)addr,vr_watch_secs);
        for(double t=0;t<vr_watch_secs;t+=0.01){watch_all(1);usleep(10000);}
        watch_all(0);unsigned total=0;
        for(unsigned i=0;i<WATCH_SLOTS;i++){uint64_t pc=atomic_load(&watch_hits[i].pc);if(!pc)continue;uint32_t n=atomic_load(&watch_hits[i].n);total+=n;
            char d0[96],d1[96],d2[96],d3[96];vr_desc(pc,d0,96);vr_desc(atomic_load(&watch_hits[i].lr),d1,96);vr_desc(atomic_load(&watch_hits[i].up[0]),d2,96);vr_desc(atomic_load(&watch_hits[i].up[1]),d3,96);
            fprintf(stderr,"[vr] findcam stage 9 read x%u pc %s lr %s < %s < %s  x1 %#llx x0 %#llx x19 %#llx x20 %#llx x21 %#llx x22 %#llx x8 %#llx\n",n,d0,d1,d2,d3,(unsigned long long)atomic_load(&watch_hits[i].x1),
                (unsigned long long)atomic_load(&watch_hits[i].xr[0]),(unsigned long long)atomic_load(&watch_hits[i].xr[1]),(unsigned long long)atomic_load(&watch_hits[i].xr[2]),
                (unsigned long long)atomic_load(&watch_hits[i].xr[3]),(unsigned long long)atomic_load(&watch_hits[i].xr[4]),(unsigned long long)atomic_load(&watch_hits[i].xr[5]));}
        fprintf(stderr,"[vr] findcam stage 9 done: %u reads\n",total);watch_addr=0;watch_store=0;}
    else vr_watch_phase(addr,9);
    vr_watch_secs=15;return NULL;}
void runtime_diag_watch_write(uint64_t addr,int secs,int load){
    if(watch_addr){fprintf(stderr,"[vr] watchw: a watch is already running\n");return;}
    uint64_t *a=malloc(24);a[0]=addr;a[1]=(uint64_t)(secs<1?1:secs>60?60:secs);a[2]=(uint64_t)load;pthread_t t;pthread_create(&t,NULL,ww_run,a);pthread_detach(t);
}
static uint64_t br_addr;static _Atomic int br_hit;static uint64_t br_x[34];static int br_creg=-1;static uint64_t br_clo,br_chi;   /* "bpregs" with a condition: x[creg] in [clo, chi) */
static void br_set(thread_act_t t,int on){
    arm_debug_state64_t d;mach_msg_type_number_t c=ARM_DEBUG_STATE64_COUNT;
    if(thread_get_state(t,ARM_DEBUG_STATE64,(thread_state_t)&d,&c)!=KERN_SUCCESS)return;
    d.__bvr[5]=on?(br_addr&~3ULL):0;d.__bcr[5]=on?((0xfULL<<5)|(2u<<1)|1u):0;
    thread_set_state(t,ARM_DEBUG_STATE64,(thread_state_t)&d,ARM_DEBUG_STATE64_COUNT);
}
static void br_all(int on){
    thread_act_array_t list;mach_msg_type_number_t n;thread_act_t self=mach_thread_self();
    if(task_threads(mach_task_self(),&list,&n)!=KERN_SUCCESS)return;
    for(unsigned i=0;i<n;i++){if(list[i]!=self){thread_suspend(list[i]);br_set(list[i],on);thread_resume(list[i]);}mach_port_deallocate(mach_task_self(),list[i]);}
    vm_deallocate(mach_task_self(),(vm_address_t)list,n*sizeof *list);mach_port_deallocate(mach_task_self(),self);
}
/* "bpvals <hex main offset> <register 0-28> <seconds>": the same breakpoint kept armed (every 10 ms), counting each
   distinct value of the register, split by the stereo B2 run it was seen in (outside / in the second run). */
static int bv_on,bv_reg;static struct{_Atomic uint64_t v;_Atomic uint32_t n[3];} bv_seen[32];
static int b2_in_second_run(void);   /* below g_vr_b2: 0 the first run (or before stereo), 1 the second, 2 after it */
static int br_trap(ucontext_t *u,unsigned ec){
    if(!br_addr||(ec!=0x30&&ec!=0x31)||u->uc_mcontext->__ss.__pc!=br_addr)return 0;
    if(bv_on){const uint64_t v=u->uc_mcontext->__ss.__x[bv_reg];const int ph=b2_in_second_run();
        for(unsigned i=0;i<32;i++){uint64_t z=0;if(atomic_compare_exchange_strong(&bv_seen[i].v,&z,v)||z==v){atomic_fetch_add(&bv_seen[i].n[ph],1);break;}}
        thread_act_t self=mach_thread_self();br_set(self,0);mach_port_deallocate(mach_task_self(),self);return 1;}
    if(br_creg>=0){const uint64_t cv=u->uc_mcontext->__ss.__x[br_creg];if(cv<br_clo||cv>=br_chi){thread_act_t self=mach_thread_self();br_set(self,0);mach_port_deallocate(mach_task_self(),self);return 1;}}   /* condition not met: re-armed later */
    int z=0;if(atomic_compare_exchange_strong(&br_hit,&z,1)){for(int i=0;i<29;i++)br_x[i]=u->uc_mcontext->__ss.__x[i];
        br_x[29]=u->uc_mcontext->__ss.__fp;br_x[30]=u->uc_mcontext->__ss.__lr;br_x[31]=u->uc_mcontext->__ss.__sp;
        uint64_t fp=br_x[29];for(int d=0;d<2;d++){uint64_t fr[2]={0,0};if(!fp||(fp&15)||!jd_read(fp,fr,16))break;br_x[32+d]=fr[1];fp=fr[0];}
        atomic_store(&br_hit,2);}
    thread_act_t self=mach_thread_self();br_set(self,0);mach_port_deallocate(mach_task_self(),self);return 1;
}
static void *br_run(void *arg){
    (void)arg;atomic_store(&br_hit,0);
    for(int t=0;t<500&&atomic_load(&br_hit)!=2;t++){br_all(1);usleep(10000);}
    br_all(0);usleep(20000);const uint64_t a=br_addr;br_addr=0;br_creg=-1;
    if(atomic_load(&br_hit)!=2){fprintf(stderr,"[vr] bpregs main+%#llx: not hit in 5 s\n",(unsigned long long)(a-(uintptr_t)diag_main->base));return NULL;}
    char line[1600];size_t u=(size_t)snprintf(line,sizeof line,"[vr] bpregs main+%#llx:",(unsigned long long)(a-(uintptr_t)diag_main->base));
    static const char *nm[34]={"x0","x1","x2","x3","x4","x5","x6","x7","x8","x9","x10","x11","x12","x13","x14","x15","x16","x17","x18","x19","x20","x21","x22","x23","x24","x25","x26","x27","x28","fp","lr","sp","up1","up2"};
    for(int i=0;i<34&&u<sizeof line;i++){if(i>=9&&i<19)continue;char d[64];jd_word(br_x[i],d,sizeof d);u+=(size_t)snprintf(line+u,sizeof line-u," %s=%s",nm[i],d);}
    fprintf(stderr,"%s\n",line);return NULL;
}
static void *bv_run(void *arg){
    const double secs=*(double *)arg;free(arg);
    for(unsigned i=0;i<32;i++){atomic_store(&bv_seen[i].v,0);for(int k=0;k<3;k++)atomic_store(&bv_seen[i].n[k],0);}
    bv_on=1;for(double t=0;t<secs;t+=0.01){br_all(1);usleep(10000);}
    br_all(0);usleep(20000);const uint64_t a=br_addr;br_addr=0;bv_on=0;
    fprintf(stderr,"[vr] bpvals main+%#llx x%d over %.0f s:\n",(unsigned long long)(a-(uintptr_t)diag_main->base),bv_reg,secs);
    for(unsigned i=0;i<32;i++){const uint64_t v=atomic_load(&bv_seen[i].v);if(!v)continue;char d[64];jd_word(v,d,sizeof d);
        fprintf(stderr,"[vr] bpvals   %s: %u in the first run (or without stereo), %u in the second, %u after it\n",d,atomic_load(&bv_seen[i].n[0]),atomic_load(&bv_seen[i].n[1]),atomic_load(&bv_seen[i].n[2]));}
    return NULL;
}
void runtime_diag_bp_vals(uint64_t off,int reg,double secs){
    if(br_addr){fprintf(stderr,"[vr] bpvals: the breakpoint is in use\n");return;}
    if(off>=diag_main->size||reg<0||reg>28){fprintf(stderr,"[vr] bpvals: bad arguments\n");return;}
    bv_reg=reg;br_creg=-1;br_addr=(uintptr_t)diag_main->base+off;double *a=malloc(sizeof *a);*a=secs<1?1:secs>30?30:secs;pthread_t t;pthread_create(&t,NULL,bv_run,a);pthread_detach(t);
}
void runtime_diag_bp_regs_if(uint64_t off,int reg,uint64_t lo,uint64_t hi){
    if(br_addr){fprintf(stderr,"[vr] bpregs: already armed\n");return;}
    br_creg=reg>=0&&reg<=28?reg:-1;br_clo=lo;br_chi=hi;
    extern void runtime_diag_bp_regs(uint64_t off);runtime_diag_bp_regs(off);
}
void runtime_diag_bp_regs(uint64_t off){
    if(br_addr&&br_creg<0){fprintf(stderr,"[vr] bpregs: already armed\n");return;}
    if(off>=diag_main->size){fprintf(stderr,"[vr] bpregs: outside main\n");return;}
    br_addr=(uintptr_t)diag_main->base+off;pthread_t t;pthread_create(&t,NULL,br_run,NULL);pthread_detach(t);
}
void runtime_diag_job_dump(void){
    if(bp_addr){fprintf(stderr,"[vr] jobdump: a breakpoint is already set\n");return;}
    pthread_t t;pthread_create(&t,NULL,jd_run,NULL);pthread_detach(t);
}
/* "linkscan" (diagnostic, user 2026-11-01 "根本 修复！": his meshes by the game's own data, not by where their bones are):
   from his actor (world_survey.c, the scene's "Player"), a breadth-first walk over the pointers in each object (0x1000
   bytes, at most 6 steps, 400000 objects) looking for the addresses of the palette buffers the renderer took as his
   (nvn_mtl.m vr_link_palettes: the body by its draw, the head and hair by his head bone). Each hit prints its path:
   actor +off -> +off ... = the buffer (exact) or "+d into it" (a pointer up to 64 KB before it). */
enum{LS_MAXN=400000,LS_HASH=1<<21,LS_READ=0x1000,LS_DEPTH=6};
static struct ls_node{uint64_t a;int32_t parent;uint16_t off;uint8_t depth;} *ls_q;
static struct{uint64_t obj,pool,buf;} ls_d[64];static int ls_nd;   /* where each buffer address was found (the field's address), and the qword before it */
static uint64_t *ls_seen;
static int ls_visit(uint64_t a){uint64_t h=(a>>3)*0x9E3779B97F4A7C15ull;for(uint32_t i=(uint32_t)(h>>43)&(LS_HASH-1);;i=(i+1)&(LS_HASH-1)){if(ls_seen[i]==a)return 0;if(!ls_seen[i]){ls_seen[i]=a;return 1;}}}
static void ls_path(int n,char *b,size_t sz){int chain[LS_DEPTH+2],k=0;for(int i=n;i>0&&k<LS_DEPTH+1;i=ls_q[i].parent)chain[k++]=i;
    size_t u=(size_t)snprintf(b,sz,"actor");for(int j=k-1;j>=0&&u<sz;j--)u+=(size_t)snprintf(b+u,sz-u," +%#x ->",ls_q[chain[j]].off);}
static void *vr_linkscan_run(void *arg){
    (void)arg;extern uintptr_t runtime_player_pos_address(void);extern int vr_link_palettes(uint64_t *,int *,int) __attribute__((weak_import));
    uint64_t tg[16];int tb[16],nt=vr_link_palettes?vr_link_palettes(tg,tb,16):0;
    const uintptr_t pa=runtime_player_pos_address();
    if(!pa||!nt){fprintf(stderr,"[vr] linkscan: %s\n",!pa?"no player":"no palette taken as his yet");return NULL;}
    const uint64_t actor=pa-GAME_ACTOR_POS,mlo=(uintptr_t)diag_main->base,mhi=mlo+diag_main->size;
    fprintf(stderr,"[vr] linkscan: actor %#llx, %d palettes:",(unsigned long long)actor,nt);for(int i=0;i<nt;i++)fprintf(stderr," %#llx (%d bones)",(unsigned long long)tg[i],tb[i]);fprintf(stderr,"\n");
    ls_q=calloc(LS_MAXN,sizeof *ls_q);ls_seen=calloc(LS_HASH,8);if(!ls_q||!ls_seen){free(ls_q);free(ls_seen);return NULL;}
    int head=0,tail=1,hits=0;ls_nd=0;ls_q[0]=(struct ls_node){actor,-1,0,0};ls_visit(actor);
    static uint64_t buf[LS_READ/8];
    while(head<tail&&hits<600){
        const struct ls_node nd=ls_q[head];mach_vm_size_t got=0;
        if(mach_vm_read_overwrite(mach_task_self(),nd.a,LS_READ,(mach_vm_address_t)(uintptr_t)buf,&got)||got!=LS_READ){
            got=0;if(mach_vm_read_overwrite(mach_task_self(),nd.a,0x100,(mach_vm_address_t)(uintptr_t)buf,&got)||!got){head++;continue;}}
        const int nw=(int)(got/8);
        for(int w=0;w<nw;w++){const uint64_t v=buf[w];
            for(int t=0;t<nt;t++)if(v&&v==tg[t]){char pb[400];ls_path(head,pb,sizeof pb);
                if(w>=1&&ls_nd<64){ls_d[ls_nd].obj=nd.a+(uint64_t)w*8;ls_d[ls_nd].pool=buf[w-1];ls_d[ls_nd].buf=v;ls_nd++;}   /* exact only; and the fields around it (the other buffer of a pair?) */
                fprintf(stderr,"[vr] linkscan hit %#llx (%d bones): %s +%#x (object %#llx) | around:",(unsigned long long)tg[t],tb[t],pb,w*8,(unsigned long long)nd.a);
                for(int k=w-4;k<=w+4;k++)if(k>=0&&k<nw)fprintf(stderr," %+d:%#llx",(k-w)*8,(unsigned long long)buf[k]);fprintf(stderr,"\n");hits++;}
            if(nd.depth<LS_DEPTH&&tail<LS_MAXN&&!(v&7)&&v>0x100000000ull&&v<0x100000000000ull&&!(v>=mlo&&v<mhi)&&ls_visit(v))
                ls_q[tail++]=(struct ls_node){v,head,(uint16_t)(w*8),(uint8_t)(nd.depth+1)};
        }
        head++;
    }
    fprintf(stderr,"[vr] linkscan done: %d objects read, %d queued, %d hits\n",head,tail,hits);
    free(ls_q);free(ls_seen);ls_q=NULL;ls_seen=NULL;return NULL;
}
/* "linkheap" (after "linkscan"): which object reachable from his actor in two steps holds an address range (two qwords
   lo < hi, at most 512 MB apart) holding his actor and the fields linkscan found the buffers in (his actor's heap?); for
   the best range, every "pool pointer, buffer address" pair inside it like the ones found (the same pool pointer), and
   the buffers they name. */
static void *vr_linkheap_run(void *arg){
    (void)arg;extern uintptr_t runtime_player_pos_address(void);const uintptr_t pa=runtime_player_pos_address();
    if(!pa||!ls_nd){fprintf(stderr,"[vr] linkheap: %s\n",!pa?"no player":"run linkscan first");return NULL;}
    const uint64_t actor=pa-GAME_ACTOR_POS;static uint64_t b0[0x1000/8],b1[0x400/8];mach_vm_size_t got;
    uint64_t best_lo=0,best_hi=0;int best_in=-1;
    if(mach_vm_read_overwrite(mach_task_self(),actor,sizeof b0,(mach_vm_address_t)(uintptr_t)b0,&got)||got!=sizeof b0)return NULL;
    for(int i=-1;i<(int)(sizeof b0/8);i++){
        const uint64_t p=i<0?actor:b0[i];if(p&7||p<0x100000000ull||p>0x100000000000ull)continue;
        if(mach_vm_read_overwrite(mach_task_self(),p,sizeof b1,(mach_vm_address_t)(uintptr_t)b1,&got)||got!=sizeof b1)continue;
        for(int a=0;a<(int)(sizeof b1/8);a++)for(int c=a+1;c<a+4&&c<(int)(sizeof b1/8);c++){
            const uint64_t lo=b1[a],hi=b1[c];if(!(lo<=actor&&actor<hi&&hi-lo<(512ull<<20)))continue;
            int in=0;for(int k=0;k<ls_nd;k++)in+=ls_d[k].obj>=lo&&ls_d[k].obj<hi;
            fprintf(stderr,"[vr] linkheap actor +%#x -> %#llx +%#x/+%#x: %#llx..%#llx (%llu KB), holds %d of the %d fields\n",i<0?0:i*8,(unsigned long long)p,a*8,c*8,
                    (unsigned long long)lo,(unsigned long long)hi,(unsigned long long)((hi-lo)>>10),in,ls_nd);
            if(in>best_in||(in==best_in&&hi-lo<best_hi-best_lo)){best_in=in;best_lo=lo;best_hi=hi;}
        }
    }
    if(best_in<=0){fprintf(stderr,"[vr] linkheap: no range holds the fields\n");return NULL;}
    /* every (pool, buffer) pair in the range with a pool pointer seen in linkscan */
    uint64_t pools[8];int np=0;for(int k=0;k<ls_nd;k++){int s2=0;for(int j=0;j<np;j++)s2|=pools[j]==ls_d[k].pool;if(!s2&&np<8)pools[np++]=ls_d[k].pool;}
    fprintf(stderr,"[vr] linkheap: best %#llx..%#llx, pools",(unsigned long long)best_lo,(unsigned long long)best_hi);for(int j=0;j<np;j++)fprintf(stderr," %#llx",(unsigned long long)pools[j]);fprintf(stderr,"\n");
    static uint64_t chunk[0x10000/8];uint64_t bufs[64];int nb=0,pairs=0;
    for(uint64_t a=best_lo&~7ull;a<best_hi;a+=sizeof chunk){
        if(mach_vm_read_overwrite(mach_task_self(),a,sizeof chunk,(mach_vm_address_t)(uintptr_t)chunk,&got)||!got)continue;
        for(int w=0;w+1<(int)(got/8);w++){int isp=0;for(int j=0;j<np;j++)isp|=chunk[w]==pools[j];if(!isp)continue;
            const uint64_t v=chunk[w+1];if(v<0x100000000ull||v>0x100000000000ull)continue;pairs++;
            int s2=0;for(int j=0;j<nb;j++)s2|=bufs[j]==v;if(!s2&&nb<64)bufs[nb++]=v;}
    }
    fprintf(stderr,"[vr] linkheap: %d pool pairs in it, %d buffers:",pairs,nb);for(int j=0;j<nb;j++)fprintf(stderr," %#llx",(unsigned long long)bufs[j]);fprintf(stderr,"\n");
    return NULL;
}
/* "linkwatch <seconds>" (after "linkscan"): a store watchpoint on the field linkscan found his body's palette buffer in
   (the one after the common pool pointer): who writes it each frame (the double buffer's flip), with x0 x19 x20 x21 x22 x8
   (the model object it belongs to), then, for each of those registers, whether his actor is within two steps of it. */
static void *vr_linkwatch_run(void *arg){
    vr_watch_secs=*(double *)arg;free(arg);
    /* the pool pointer: one into main's image (the descriptors had a static object there; the recorded command buffers'
       0x200000000 is not one) */
    const uint64_t mlo=(uintptr_t)diag_main->base,mhi=mlo+diag_main->size;
    uint64_t pool=0;int bestc=0;for(int k=0;k<ls_nd;k++){if(ls_d[k].pool<mlo||ls_d[k].pool>=mhi)continue;int c=0;for(int j=0;j<ls_nd;j++)c+=ls_d[j].pool==ls_d[k].pool;if(c>bestc){bestc=c;pool=ls_d[k].pool;}}
    uint64_t f=0;for(int k=0;k<ls_nd&&!f;k++)if(ls_d[k].pool==pool)f=ls_d[k].obj;
    if(!f){fprintf(stderr,"[vr] linkwatch: run linkscan first\n");vr_watch_secs=15;return NULL;}
    fprintf(stderr,"[vr] linkwatch: field %#llx (pool %#llx, %d fields like it)\n",(unsigned long long)f,(unsigned long long)pool,bestc);
    vr_watch_phase(f,7);vr_watch_secs=15;return NULL;
}
void runtime_diag_link_watch(double secs){if(watch_addr)return;double *a=malloc(sizeof *a);*a=secs;pthread_t t;pthread_create(&t,NULL,vr_linkwatch_run,a);pthread_detach(t);}
/* "linkup" (after "linkscan"; the root identity, user 2026-11-01): who owns his palette buffers' descriptors. From one
   descriptor field per buffer (a pool pointer into main's image before it), a reverse search over all writable memory
   for pointers into the 0x400 bytes up to that field (the object holding it), then into the 0x1000 bytes up to each
   holder found, for 4 levels (at most 12 holders a level); a holder inside his actor (0x2000 bytes) ends the chain,
   printed from the actor down. */
typedef struct{uint64_t at,val;int parent;} lu_node;
static int lu_scan(const uint64_t *lo,const uint64_t *hi,int nw,lu_node *out,int max,int parent_base,const uint64_t mlo,const uint64_t mhi){
    int n=0;mach_vm_address_t a=0;mach_vm_size_t sz=0;vm_region_basic_info_data_64_t info;mach_msg_type_number_t cnt;mach_port_t obj;
    static uint64_t chunk[1<<17];
    for(;n<max;a+=sz){cnt=VM_REGION_BASIC_INFO_COUNT_64;
        if(mach_vm_region(mach_task_self(),&a,&sz,VM_REGION_BASIC_INFO_64,(vm_region_info_t)&info,&cnt,&obj)!=KERN_SUCCESS)break;
        if((info.protection&(VM_PROT_READ|VM_PROT_WRITE))!=(VM_PROT_READ|VM_PROT_WRITE)||sz>(16ull<<30)||(a>=mlo&&a<mhi))continue;
        for(mach_vm_address_t c=a;c<a+sz&&n<max;c+=sizeof chunk){mach_vm_size_t got=0;const mach_vm_size_t want=a+sz-c<sizeof chunk?a+sz-c:sizeof chunk;
            if(mach_vm_read_overwrite(mach_task_self(),c,want,(mach_vm_address_t)(uintptr_t)chunk,&got)||!got)continue;
            for(size_t w=0;w<got/8&&n<max;w++){const uint64_t v=chunk[w];if(v&7)continue;
                for(int k=0;k<nw;k++)if(v>=lo[k]&&v<=hi[k]){out[n].at=c+w*8;out[n].val=v;out[n].parent=parent_base+k;n++;break;}}}
    }
    return n;
}
static void *vr_linkup_run(void *arg){
    (void)arg;extern uintptr_t runtime_player_pos_address(void);const uintptr_t pa=runtime_player_pos_address();
    if(!pa||!ls_nd){fprintf(stderr,"[vr] linkup: %s\n",!pa?"no player":"run linkscan first");return NULL;}
    const uint64_t actor=pa-GAME_ACTOR_POS,mlo=(uintptr_t)diag_main->base,mhi=mlo+diag_main->size;
    static lu_node L[5][64];int nl[5]={0};
    /* level 0: one descriptor field per distinct buffer */
    for(int k=0;k<ls_nd&&nl[0]<12;k++){if(ls_d[k].pool<mlo||ls_d[k].pool>=mhi)continue;int dup=0;for(int j=0;j<nl[0];j++)dup|=L[0][j].val==ls_d[k].buf;
        if(!dup){L[0][nl[0]].at=ls_d[k].obj;L[0][nl[0]].val=ls_d[k].buf;L[0][nl[0]].parent=-1;nl[0]++;}}
    fprintf(stderr,"[vr] linkup: actor %#llx, %d descriptor fields\n",(unsigned long long)actor,nl[0]);
    for(int lv=1;lv<5;lv++){
        uint64_t lo[64],hi[64];const uint64_t win=lv==1?0x400:0x1000;
        for(int k=0;k<nl[lv-1];k++){hi[k]=L[lv-1][k].at;lo[k]=hi[k]-win;}
        nl[lv]=lu_scan(lo,hi,nl[lv-1],L[lv],12*nl[lv-1]<64?12*nl[lv-1]:64,0,mlo,mhi);
        fprintf(stderr,"[vr] linkup level %d: %d holders\n",lv,nl[lv]);
        int ended=0;
        for(int k=0;k<nl[lv];k++){const uint64_t h=L[lv][k].at;
            if(h>=actor&&h<actor+0x2000){ended++;char b[600];size_t u=(size_t)snprintf(b,sizeof b,"actor +%#llx",(unsigned long long)(h-actor));
                int p=L[lv][k].parent;uint64_t val=L[lv][k].val;
                for(int l2=lv-1;l2>=0&&p>=0;l2--){const lu_node *q=&L[l2][p];u+=(size_t)snprintf(b+u,sizeof b-u," -> obj %#llx +%#llx",(unsigned long long)val,(unsigned long long)(q->at-val));val=q->val;p=q->parent;}
                fprintf(stderr,"[vr] linkup chain: %s = buffer %#llx\n",b,(unsigned long long)val);}
            else if(lv<=2){fprintf(stderr,"[vr] linkup   holder %#llx -> %#llx (+%#llx into the level %d object)\n",(unsigned long long)h,(unsigned long long)L[lv][k].val,(unsigned long long)(L[lv-1][L[lv][k].parent].at-L[lv][k].val),lv-1);}
        }
        if(ended)fprintf(stderr,"[vr] linkup level %d: %d chains reach his actor\n",lv,ended);
        if(!nl[lv])break;
    }
    fprintf(stderr,"[vr] linkup done\n");return NULL;
}
void runtime_diag_link_up(void){pthread_t t;pthread_create(&t,NULL,vr_linkup_run,NULL);pthread_detach(t);}
void runtime_diag_link_heap(void){pthread_t t;pthread_create(&t,NULL,vr_linkheap_run,NULL);pthread_detach(t);}
void runtime_diag_link_scan(void){pthread_t t;pthread_create(&t,NULL,vr_linkscan_run,NULL);pthread_detach(t);}
void runtime_diag_cam_watch(double secs,int src){
    if(watch_addr){fprintf(stderr,"[vr] camwatch: a watch is already running\n");return;}
    double *a=malloc(2*sizeof *a);a[0]=secs;a[1]=src;pthread_t t;pthread_create(&t,NULL,vr_camwatch_run,a);pthread_detach(t);
}
void runtime_diag_xpoke(unsigned off,float value){
    uint64_t x=time_object();float old;
    memcpy(&old,(const void *)(uintptr_t)(x+off),4);memcpy((void *)(uintptr_t)(x+off),&value,4);
    fprintf(stderr,"[totk] xpoke X+%#x: %g -> %g\n",off,old,value);
}
#endif
/* VR camera control (vr_camera.h), Mac only. TOTK_VR_HEADLOOK=0 disables it.
   Two veneers live in the checked tail of the rewriter's patch area:
   - main+0x6289a4 (ldr x8,[x19,#0x20], right after the inlined LookAtCamera matrix update stored x20+8..+0x38) goes
     to a veneer that jumps (x16, dead there, as are x17 and the flags: checked to the function's end) to vr_cam_hook;
   - main+0xb33e8 sets every native view of its actual camera/projection owner;
     view 0 also publishes its camera identity and matrix to v0ring. */
/* Defined by the VR compositor (totk_vr.m); weak here so tests that build this file alone still link. */
__attribute__((weak)) vr_cam_state *g_vr_headlook;
#if !TARGET_OS_IPHONE
vr_cam_state g_vr_cam __attribute__((aligned(16)));
#include "totk_vr_camera_owner.h"
_Static_assert(offsetof(vr_cam_state,cam)==8&&offsetof(vr_cam_state,h)==16&&offsetof(vr_cam_state,used)==64&&offsetof(vr_cam_state,fp_lag)==68&&offsetof(vr_cam_state,ret)==72&&offsetof(vr_cam_state,ring)==80&&sizeof g_vr_cam.ring[0]==64&&offsetof(vr_cam_state,fp_pos)==1104&&offsetof(vr_cam_state,fp_blend)==1112&&offsetof(vr_cam_state,eye_half)==1116&&offsetof(vr_cam_state,fp_up)==1120&&offsetof(vr_cam_state,fp_fwd)==1136&&offsetof(vr_cam_state,cam_back)==1140&&offsetof(vr_cam_state,level)==1144&&offsetof(vr_cam_state,calls)==1148&&offsetof(vr_cam_state,off)==1152&&offsetof(vr_cam_state,game_right)==1168&&offsetof(vr_cam_state,callee)==1184&&offsetof(vr_cam_state,main_vs)==1192&&offsetof(vr_cam_state,v0n)==1200&&offsetof(vr_cam_state,v0ring)==1216&&sizeof g_vr_cam.v0ring[0]==64,"vr_cam_hook offsets");
__asm__(".text\n.p2align 4\n.globl _vr_cam_hook\n_vr_cam_hook:\n"
        "  adrp x16, _g_vr_cam@PAGE\n  add x16, x16, _g_vr_cam@PAGEOFF\n"
        "  sub sp, sp, #224\n  mrs x17, nzcv\n  str x17, [sp, #112]\n"
        /* Diagnostic only: several native controllers can update in one run.
           Ownership comes from controller -> holder -> camera, not this last pointer. */
        "  add x17, x16, #3320\n  stlr x20, [x17]\n"
        "  ldr x17, [x16, #8]\n  cmp x17, x20\n  b.ne 1f\n"   /* not the main camera */
        "  ldr w17, [x16]\n  cbz w17, 1f\n"                     /* no head rotation yet */
        "  stp q0, q1, [sp]\n  stp q2, q3, [sp, #32]\n  stp q4, q5, [sp, #64]\n  str q6, [sp, #96]\n"
        "  stp q7, q8, [sp, #128]\n  stp q9, q10, [sp, #160]\n  stp q11, q12, [sp, #192]\n"
        "  ldr w17, [x16, #1148]\n  add w17, w17, #1\n  str w17, [x16, #1148]\n"   /* calls: the world camera runs */
        /* level: the game's pitch out (the camera has no roll, so its right axis r0 is level): r0' = r0 flattened and
           normalised, r1' = up, r2' = (-r0'.z, 0, r0'.x); the position kept: eye = -(t0 r0 + t1 r1 + t2 r2), t' = -R' eye */
        "  ldr w17, [x16, #1144]\n  cbz w17, 3f\n"
        "  ldur q0, [x20, #8]\n  ldur q1, [x20, #24]\n  ldur q2, [x20, #40]\n"
        "  fmul v3.4s, v0.4s, v0.s[3]\n  fmla v3.4s, v1.4s, v1.s[3]\n  fmla v3.4s, v2.4s, v2.s[3]\n"   /* -eye in lanes 0..2 */
        "  mov v4.16b, v0.16b\n  mov v4.s[1], wzr\n  mov v4.s[3], wzr\n"
        "  fmul v5.4s, v4.4s, v4.4s\n  faddp v5.4s, v5.4s, v5.4s\n  faddp s5, v5.2s\n  fsqrt s5, s5\n"
        "  fmov s6, #0.125\n  fcmp s5, s6\n  b.lt 3f\n"                                   /* not a usable right axis */
        "  dup v5.4s, v5.s[0]\n  fdiv v4.4s, v4.4s, v5.4s\n"
        "  movi v6.16b, #0\n  fmov s7, #1.0\n  mov v6.s[1], v7.s[0]\n"
        "  movi v7.16b, #0\n  mov v7.s[2], v4.s[0]\n  mov v8.s[0], v4.s[2]\n  fneg s8, s8\n  mov v7.s[0], v8.s[0]\n"
        "  fmul v8.4s, v4.4s, v3.4s\n  faddp v8.4s, v8.4s, v8.4s\n  faddp s8, v8.2s\n  mov v4.s[3], v8.s[0]\n"
        "  fmul v8.4s, v6.4s, v3.4s\n  faddp v8.4s, v8.4s, v8.4s\n  faddp s8, v8.2s\n  mov v6.s[3], v8.s[0]\n"
        "  fmul v8.4s, v7.4s, v3.4s\n  faddp v8.4s, v8.4s, v8.4s\n  faddp s8, v8.2s\n  mov v7.s[3], v8.s[0]\n"
        "  stur q4, [x20, #8]\n  stur q6, [x20, #24]\n  stur q7, [x20, #40]\n"
        "3:\n"
        /* the settings' offset (off, world axes; the VR thread makes it from game_right): the eye moves by d, t_i -= r_i . d */
        "  ldur q0, [x20, #8]\n  ldur q1, [x20, #24]\n  ldur q2, [x20, #40]\n  str q0, [x16, #1168]\n"
        "  ldr q9, [x16, #1152]\n  mov v9.s[3], wzr\n"
        "  fmul v8.4s, v0.4s, v9.4s\n  faddp v8.4s, v8.4s, v8.4s\n  faddp s8, v8.2s\n  mov s10, v0.s[3]\n  fsub s10, s10, s8\n  mov v0.s[3], v10.s[0]\n"
        "  fmul v8.4s, v1.4s, v9.4s\n  faddp v8.4s, v8.4s, v8.4s\n  faddp s8, v8.2s\n  mov s10, v1.s[3]\n  fsub s10, s10, s8\n  mov v1.s[3], v10.s[0]\n"
        "  fmul v8.4s, v2.4s, v9.4s\n  faddp v8.4s, v8.4s, v8.4s\n  faddp s8, v8.2s\n  mov s10, v2.s[3]\n  fsub s10, s10, s8\n  mov v2.s[3], v10.s[0]\n"
        "  stur q0, [x20, #8]\n  stur q1, [x20, #24]\n  stur q2, [x20, #40]\n"
        /* Publish the actual orbit/cutscene base before multiplying by H. */
        "  ldr w17, [x16, #3312]\n  add w17, w17, #1\n  str w17, [x16, #3312]\n  dmb ish\n"
        "  str q0, [x16, #3264]\n  str q1, [x16, #3280]\n  str q2, [x16, #3296]\n"
        "  dmb ish\n  add w17, w17, #1\n  str w17, [x16, #3312]\n"
        "  ldr w17, [x16, #4]\n"                                  /* seq before h: a newer h than its seq at worst */
        "  ldp q4, q5, [x16, #16]\n  ldr q6, [x16, #48]\n"
        "  ldr q0, [x20, #8]\n  ldr q1, [x20, #24]\n  ldr q2, [x20, #40]\n"
        /* third-person distance: back along the view = everything cam_back further down -z: row 2's translation - cam_back */
        "  ldr s11, [x16, #1140]\n  mov s12, v2.s[3]\n  fsub s12, s12, s11\n  mov v2.s[3], v12.s[0]\n"
        "  fmul v3.4s, v0.4s, v4.s[0]\n  fmla v3.4s, v1.4s, v4.s[1]\n  fmla v3.4s, v2.4s, v4.s[2]\n  str q3, [x20, #8]\n"
        "  fmul v3.4s, v0.4s, v5.s[0]\n  fmla v3.4s, v1.4s, v5.s[1]\n  fmla v3.4s, v2.4s, v5.s[2]\n  str q3, [x20, #24]\n"
        "  fmul v3.4s, v0.4s, v6.s[0]\n  fmla v3.4s, v1.4s, v6.s[1]\n  fmla v3.4s, v2.4s, v6.s[2]\n  str q3, [x20, #40]\n"
        "  str w17, [x16, #64]\n"                                 /* this frame took head rotation seq */
        "  fmov s0, w17\n"                                        /* kept in s0 for the ring */
        /* first person: eye = player position + fp_up - fp_fwd * row 2 (the view looks down -z); t_i = -row_i . eye */
        "  movi v11.2d, #0\n"                                     /* the position read, for the ring (0: third person) */
        "  ldr x17, [x16, #1104]\n  cbz x17, 2f\n"
        "  ldur q7, [x17]\n  mov v11.16b, v7.16b\n"
        /* fp_lag: the position fp_lag updates ago (ring entry (calls - fp_lag) & 15, written by an earlier call of this
           thread), the one the drawn body is at; an entry without one (0: third person then) keeps the position read */
        "  ldr w17, [x16, #68]\n  cbz w17, 4f\n"
        "  ldr s8, [x16, #1148]\n  fmov s12, w17\n  sub v8.2s, v8.2s, v12.2s\n  fmov w17, s8\n"
        "  and x17, x17, #15\n  add x17, x16, x17, lsl #6\n  ldur q8, [x17, #132]\n"
        "  fmov w17, s8\n  cbz w17, 4f\n  mov v7.16b, v8.16b\n"
        "4:\n  ldr q9, [x16, #1120]\n  ldr s10, [x16, #1136]\n"
        "  ldur q1, [x20, #8]\n  ldur q2, [x20, #24]\n  ldur q3, [x20, #40]\n"
        "  fadd v7.4s, v7.4s, v9.4s\n  fmls v7.4s, v3.4s, v10.s[0]\n  mov v7.s[3], wzr\n"
        /* fp_blend (s12): t = t_third + (t_eyes - t_third) * blend, per row (s10 the third person's t) */
        "  ldr s12, [x16, #1112]\n"
        "  fmul v8.4s, v1.4s, v7.4s\n  faddp v8.4s, v8.4s, v8.4s\n  faddp s8, v8.2s\n  fneg s8, s8\n"
        "  mov s10, v1.s[3]\n  fsub s8, s8, s10\n  fmadd s8, s8, s12, s10\n  mov v1.s[3], v8.s[0]\n  stur q1, [x20, #8]\n"
        "  fmul v8.4s, v2.4s, v7.4s\n  faddp v8.4s, v8.4s, v8.4s\n  faddp s8, v8.2s\n  fneg s8, s8\n"
        "  mov s10, v2.s[3]\n  fsub s8, s8, s10\n  fmadd s8, s8, s12, s10\n  mov v2.s[3], v8.s[0]\n  stur q2, [x20, #24]\n"
        "  fmul v8.4s, v3.4s, v7.4s\n  faddp v8.4s, v8.4s, v8.4s\n  faddp s8, v8.2s\n  fneg s8, s8\n"
        "  mov s10, v3.s[3]\n  fsub s8, s8, s10\n  fmadd s8, s8, s12, s10\n  mov v3.s[3], v8.s[0]\n  stur q3, [x20, #40]\n"
        /* B1 stereo (eye_half, 1116): the eye along the final right axis, left on odd calls, right on even: t0 -= +-e */
        "2:\n  ldr s9, [x16, #1116]\n  fcmp s9, #0.0\n  b.eq 5f\n"
        "  ldr w17, [x16, #1148]\n  tst w17, #1\n  b.eq 6f\n  fneg s9, s9\n6:\n"
        "  ldur q1, [x20, #8]\n  mov s10, v1.s[3]\n  fsub s10, s10, s9\n  mov v1.s[3], v10.s[0]\n  stur q1, [x20, #8]\n"
        "5:\n  ldr w17, [x16, #1148]\n  and x17, x17, #15\n  add x17, x16, x17, lsl #6\n"   /* its ring entry (by calls: one per camera update; by seq a still head kept rewriting one entry): the final rows, seq */
        "  ldur q1, [x20, #8]\n  str q1, [x17, #80]\n  ldur q1, [x20, #24]\n  str q1, [x17, #96]\n"
        "  ldur q1, [x20, #40]\n  str q1, [x17, #112]\n  str s0, [x17, #128]\n"
        "  stur d11, [x17, #132]\n  mov v12.s[0], v11.s[2]\n  stur s12, [x17, #140]\n"   /* and the position this frame's camera was put at */
        "  ldp q0, q1, [sp]\n  ldp q2, q3, [sp, #32]\n  ldp q4, q5, [sp, #64]\n  ldr q6, [sp, #96]\n"
        "  ldp q7, q8, [sp, #128]\n  ldp q9, q10, [sp, #160]\n  ldp q11, q12, [sp, #192]\n"
        "1:\n  ldr x17, [sp, #112]\n  msr nzcv, x17\n  add sp, sp, #224\n"
        "  ldr x8, [x19, #0x20]\n"                               /* the instruction the branch replaced */
        "  ldr x16, [x16, #72]\n  br x16\n");
extern void vr_cam_hook(void);
_Static_assert(offsetof(vr_cam_state,base_view)==3264&&offsetof(vr_cam_state,base_sequence)==3312&&offsetof(vr_cam_state,native_writer)==3320,"VR base publication offsets");
int runtime_vr_base_view(float out[12]) {
    if(!g_vr_headlook)return 0;
    for(unsigned attempt=0;attempt<8;attempt++){
        uint32_t sequence=__atomic_load_n(&g_vr_cam.base_sequence,__ATOMIC_ACQUIRE);
        if(!sequence||(sequence&1))continue;
        for(unsigned i=0;i<12;i++){uint32_t bits=__atomic_load_n((uint32_t *)(void *)&g_vr_cam.base_view[i],__ATOMIC_RELAXED);memcpy(out+i,&bits,4);}
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if(sequence==__atomic_load_n(&g_vr_cam.base_sequence,__ATOMIC_ACQUIRE))return 1;
    }
    return 0;
}
/* The native view-set setter receives x0=view set, w1=index, x2=camera+8,
   x3=projection and x4=extra. All views of the owned gameplay camera need the
   same eye projection: patching only the view-0 caller left auxiliary views
   symmetric (observed in the actual GPU bindings). Only view 0 enters v0ring;
   other callers are rejected by the real camera/projection owner relationship. */
__asm__(".text\n.p2align 4\n.globl _vr_view0_hook\n_vr_view0_hook:\n"
        "  sub x9, x2, #8\n  cbnz w1, 9f\n"
        "  adrp x16, _g_vr_cam@PAGE\n  add x16, x16, _g_vr_cam@PAGEOFF\n"
        "  sub sp, sp, #16\n  str q0, [sp]\n"
        "  ldr w17, [x16, #1200]\n  add w17, w17, #1\n  str w17, [x16, #1200]\n"
        "  and x17, x17, #31\n  add x17, x16, x17, lsl #6\n  add x17, x17, #1216\n"
        "  str x0, [x17]\n  str x9, [x17, #8]\n  ldr q0, [x2]\n  str q0, [x17, #16]\n  ldr q0, [x2, #16]\n  str q0, [x17, #32]\n"
        "  ldr q0, [x2, #32]\n  str q0, [x17, #48]\n"
        "  ldr q0, [sp]\n  add sp, sp, #16\n"
        "9:\n  mov x5, x9\n  adrp x16, _totk_vr_view_set@PAGE\n  add x16, x16, _totk_vr_view_set@PAGEOFF\n  br x16\n");
extern void vr_view0_hook(void);
/* Third-person camera distance (settings panel, also without VR): metres further back (negative: closer), kept in
   TOTK_CAMERA_SETTINGS (the launcher: <data>/camera.txt). Without VR nothing else turns the hook on, so it is switched
   on here with the identity rotation; with VR the VR thread writes the head's rotation every frame. Not checked
   against walls: the game's own camera collision does not see the extra distance. */
static int vr_active(void){const char *c=getenv("TOTK_VR_CONTROL");return c&&*c;}
static void camera_distance_apply(float m){
    g_vr_cam.cam_back=m;
    if(!vr_active()){static const float id[12]={1,0,0,0,0,1,0,0,0,0,1,0};memcpy(g_vr_cam.h,id,sizeof id);__atomic_store_n(&g_vr_cam.on,1,__ATOMIC_RELEASE);}
}
int runtime_camera_distance_cm(void){return g_vr_headlook?(int)lroundf(g_vr_cam.cam_back*100):0;}
int runtime_camera_distance_available(void){return g_vr_headlook!=NULL;}
/* The right stick's up / down is the game's pitch: with the pitch locked to the headset it does nothing in the world (user
   2026-10-29: "手柄 只能左右改变摄像头 不能上下"); in menus and the map (the world camera stops: no hook call for 0.2 s)
   it works as before. runtime/input.c (its weak default returns 0). */
int runtime_vr_stick_pitch_locked(void){
    if(!g_vr_headlook||!__atomic_load_n(&g_vr_cam.level,__ATOMIC_RELAXED))return 0;
    static uint32_t last;static uint64_t at;uint32_t c=__atomic_load_n(&g_vr_cam.calls,__ATOMIC_RELAXED);
    struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);uint64_t now=(uint64_t)ts.tv_sec*1000000000ull+(uint64_t)ts.tv_nsec;
    if(c!=last){last=c;at=now;}
    return now-at<200000000ull;
}
void runtime_camera_distance_set(int cm){
    if(!g_vr_headlook||cm<-500||cm>1000)return;
    camera_distance_apply(cm/100.0f);
    const char *p=getenv("TOTK_CAMERA_SETTINGS");FILE *f=p?fopen(p,"w"):NULL;if(f){fprintf(f,"%d\n",cm);fclose(f);}
    fprintf(stderr,"[totk] camera distance (third person): %+d cm\n",cm);
}
static void camera_distance_load(void){
    const char *p=getenv("TOTK_CAMERA_SETTINGS");FILE *f=p?fopen(p,"r"):NULL;int cm=0;
    if(f){if(fscanf(f,"%d",&cm)!=1||cm<-500||cm>1000)cm=0;fclose(f);}
    if(cm){camera_distance_apply(cm/100.0f);fprintf(stderr,"[totk] camera distance (third person): %+d cm (kept in %s)\n",cm,p);}
}
/* Stereo B2, step 1 (2026-11-02, user: "开始 最好的办法": both eyes of one game update): the frame's last calc stage
   (main+0x3e55e0 sets the job list [x19+0x4d8] and tail-calls the queue runner main+0x3e5dfc at main+0x3e5adc) runs the
   game's 35 render jobs (vtable entry 2 = main+0x2887b80, each with a mask of the render work it does: views, culling,
   command recording) together with logic jobs (actors, physics, events, sound) in one dependency graph. The tail call
   goes to vr_b2_hook instead, which runs the stage as before and then, with "b2 1", runs a second list through the
   game's own queue: main+0x3e60b0 (queue [x19+0x4b8], [x19+0x4c0], list) then the runner, as the stage does. The second
   list holds only the render jobs and the dependencies among them (the second eye's picture, once the camera changes
   between the runs: later steps). Called directly, outside the queue, the view job waited forever for the sub-jobs it
   hands to the queue's workers (b2a, 2026-11-02). Off (the default) the hook only calls the runner and returns.
   Before the second list every view's per-frame start runs again (main+0x3dbf28 (view, [render system+0x9a4] != 0, 1)
   for each view of the view manager [main+0x3acda18] (+0x10 count, +0x18 views), as the render system's per-frame start
   main+0x3db120 does): it clears the view's work counters and masks (+0x3ba8...) and prepares it. Without it the view
   job found its work items all taken and waited forever at its barrier (b2b). The whole per-frame start is not run:
   it also empties the frame's model lists, which only the logic jobs fill (b2c: the second picture was sky only).
   Work in progress (2026-11-02), off by default, Mac VR only. Modes ("b2 <mode> [mask]"):
   1 each view's per-frame start before the second list: no hang, but the view's model requests (+0x3350..+0x3380, filled
     by the logic jobs after the render system's per-frame start) are emptied: sky only (b2d, 99 draws a frame);
   2 only the view's job counters, no per-frame start: hangs at the view barrier (b2f; the start does more that the jobs
     need);
   3 the per-frame start with the fields chosen by the mask saved before it and put back after it: bit 0 +0x3350..+0x3380,
     bit 1 +0x3398..+0x33a8, bit 2 +0x33e0..+0x3420, bit 3 each render step's +0x5d0 word and +0x2914 half-word. Mask 1
     (b2g): the second picture has the scene (1021 draws a frame, G-buffer 566 + 191 against 691 + 598) but almost no
     shadows (44 draws against about 1300): the shadow casters' requests are kept elsewhere (next step). Mask 9 untested.
   Eyes: with "b2eye <mm>" (half the eye distance) the main camera's view matrix moves left for the first run and right
   for the second (t.x += / -= in view space, as the B1 hook), back afterwards; the camera is final before this stage.
   The first eye's constants and both eyes' submission are later steps.
   g_vr_b2: +0 the runner, +8 the list setter, +16 the mode (0 off), +20 the mask, +24 second runs done, +28 1 during the second run, +32 the stage
   object (x19), +40 the second list, +48 the view's per-frame start, +56 the render system, +64 the view manager's slot,
   +72 the saved fields (27 words), then the render steps' saved words and half-words. */
typedef struct { uint64_t run, setlist; uint32_t on, mask, runs, phase; uint64_t stage, list, begin, begin_obj, vmgr; uint64_t save[27]; uint32_t step_flags[32]; uint16_t step_bits[32]; uint64_t camp; float eh; uint32_t pad3; } vr_b2_state;
vr_b2_state g_vr_b2 __attribute__((aligned(16)));
#include "totk_vr_projection.h"
static uint64_t b2_s1_deleg;   /* render stage 1's view renderer (b2_stage1_run1) */
static uint64_t b2_slot_obj[8];static int b2_slot_n;   /* the owners of the render stages' summary slots (runtime_b2_twin) */
static _Atomic uint64_t b2_map_n; /* exact renderer/layer pairs transferred to right-copy ownership */
static int b2_in_second_run(void){const uint32_t p=__atomic_load_n(&g_vr_b2.phase,__ATOMIC_RELAXED);return p==1?1:p==3?2:0;}
__asm__(".text\n.p2align 4\n.globl _vr_b2_hook\n_vr_b2_hook:\n"
        "  stp x29, x30, [sp, #-64]!\n  mov x29, sp\n  stp x19, x20, [sp, #16]\n  stp x21, x22, [sp, #32]\n  str x23, [sp, #48]\n"
        "  adrp x19, _g_vr_b2@PAGE\n  add x19, x19, _g_vr_b2@PAGEOFF\n"
        /* on: the main camera's view matrix (the camera object's +8, rows R | t) to the left eye for the first run:
           t.x += half the eye distance (as the B1 hook does), x0 (the queue) kept */
        "  ldr w8, [x19, #16]\n  cbz w8, 20f\n  str wzr, [x19, #28]\n"
        "  ldr x9, [x19, #480]\n  ldr x9, [x9]\n  cbz x9, 20f\n"
        "  ldr s0, [x19, #488]\n  ldr s1, [x9, #20]\n  fadd s1, s1, s0\n  str s1, [x9, #20]\n"
        "20:\n  ldr x16, [x19]\n  blr x16\n"                       /* the stage, as the tail call did */
        "  ldr w8, [x19, #16]\n  cbz w8, 9f\n"
        "  adrp x16, _b2_between_runs@PAGE\n  add x16, x16, _b2_between_runs@PAGEOFF\n  blr x16\n"
        "  mov w8, #1\n  str w8, [x19, #28]\n"   /* recordings from here on are the second run's (runtime_b2_rec) */
        "  ldr x9, [x19, #480]\n  ldr x9, [x9]\n  cbz x9, 21f\n"   /* the right eye: t.x -= the eye distance */
        "  ldr s0, [x19, #488]\n  ldr s1, [x9, #20]\n  fsub s1, s1, s0\n  fsub s1, s1, s0\n  str s1, [x9, #20]\n"
        "21:\n"
        "  ldr x9, [x19, #56]\n  ldr w10, [x9, #0x9a4]\n  cmp w10, #0\n  cset w20, ne\n"
        "  ldr x9, [x19, #64]\n  ldr x21, [x9]\n  cbz x21, 3f\n  ldrsw x22, [x21, #0x10]\n  ldr x23, [x21, #0x18]\n"
        "2:\n  cbz x22, 3f\n  ldr x0, [x23], #8\n"
        "  ldr w8, [x19, #16]\n  cmp w8, #2\n  b.eq 4f\n"
        "  cmp w8, #3\n  b.ne 8f\n"   /* "b2 3 <mask>": keep the view's lists at +0x3350..+0x3428 the per-frame start would clear */
        "  mov x10, #0x3350\n  add x10, x0, x10\n  add x11, x19, #72\n  mov w12, #27\n"
        "10:\n  ldr x13, [x10], #8\n  str x13, [x11], #8\n  subs w12, w12, #1\n  b.ne 10b\n"
        /* the render steps (+0x130 of them at +0x138, 0x2920 bytes each): their +0x5d0 word and +0x2914 half-word */
        "  ldr w12, [x0, #0x130]\n  cmp w12, #32\n  mov w13, #32\n  csel w12, w12, w13, ls\n  ldr x10, [x0, #0x138]\n"
        "  add x11, x19, #72+27*8\n  add x15, x19, #72+27*8+32*4\n  mov x16, #0x2914\n"
        "16:\n  cbz w12, 17f\n  ldr w13, [x10, #0x5d0]\n  str w13, [x11], #4\n  ldrh w13, [x10, x16]\n  strh w13, [x15], #2\n"
        "  mov x13, #0x2920\n  add x10, x10, x13\n  sub w12, w12, #1\n  b 16b\n"
        "17:\n  str x0, [sp, #56]\n"
        "  mov w1, w20\n  mov w2, #1\n  ldr x16, [x19, #48]\n  blr x16\n"
        "  ldr x0, [sp, #56]\n  ldr w14, [x19, #20]\n"
        "  tbz w14, #3, 18f\n"
        "  ldr w12, [x0, #0x130]\n  cmp w12, #32\n  mov w13, #32\n  csel w12, w12, w13, ls\n  ldr x10, [x0, #0x138]\n"
        "  add x11, x19, #72+27*8\n  add x15, x19, #72+27*8+32*4\n  mov x16, #0x2914\n"
        "19:\n  cbz w12, 18f\n  ldr w13, [x11], #4\n  str w13, [x10, #0x5d0]\n  ldrh w13, [x15], #2\n  strh w13, [x10, x16]\n"
        "  mov x13, #0x2920\n  add x10, x10, x13\n  sub w12, w12, #1\n  b 19b\n"
        "18:\n"
        "  tbz w14, #0, 11f\n  mov x10, #0x3350\n  add x10, x0, x10\n  add x11, x19, #72\n  mov w12, #6\n"
        "12:\n  ldr x13, [x11], #8\n  str x13, [x10], #8\n  subs w12, w12, #1\n  b.ne 12b\n"
        "11:\n  tbz w14, #1, 13f\n  mov x10, #0x3398\n  add x10, x0, x10\n  add x11, x19, #72+0x48\n  mov w12, #3\n"
        "14:\n  ldr x13, [x11], #8\n  str x13, [x10], #8\n  subs w12, w12, #1\n  b.ne 14b\n"
        "13:\n  tbz w14, #2, 5f\n  mov x10, #0x33e0\n  add x10, x0, x10\n  add x11, x19, #72+0x90\n  mov w12, #9\n"
        "15:\n  ldr x13, [x11], #8\n  str x13, [x10], #8\n  subs w12, w12, #1\n  b.ne 15b\n"
        "  b 5f\n"
        "8:\n  mov w1, w20\n  mov w2, #1\n  ldr x16, [x19, #48]\n  blr x16\n"   /* "b2 1": each view's per-frame start */
        "  b 5f\n"
        /* "b2 2": only the view's job state: the record batches (+0x3b78, +0x3b80), the pairs at +0x3ba0 (+0x3b98 of them),
           the work counters +0x3ba8..+0x3bd4, and the flags the per-frame start sets (+0x3340 |= 0x1001, &= ~0x400;
           +0x3a80 &= ~2). The lists the logic jobs fill after the per-frame start stay. */
        "4:\n  str wzr, [x0, #0x3b78]\n  str wzr, [x0, #0x3b80]\n"
        "  ldr w8, [x0, #0x3b98]\n  ldr x9, [x0, #0x3ba0]\n"
        "6:\n  cbz w8, 7f\n  str xzr, [x9], #8\n  sub w8, w8, #1\n  b 6b\n"
        "7:\n  mov x10, #0x3ba8\n  add x10, x0, x10\n  stp xzr, xzr, [x10]\n  stp xzr, xzr, [x10, #16]\n  stp xzr, xzr, [x10, #32]\n  str wzr, [x10, #44]\n"
        "  ldr w8, [x0, #0x3340]\n  and w8, w8, #0xfffffbff\n  orr w8, w8, #0x1\n  orr w8, w8, #0x1000\n  str w8, [x0, #0x3340]\n"
        "  ldr w8, [x0, #0x3a80]\n  and w8, w8, #0xfffffffd\n  str w8, [x0, #0x3a80]\n"
        "5:\n  sub x22, x22, #1\n  b 2b\n"
        "3:\n"
        "  ldr x9, [x19, #32]\n  ldr x0, [x9, #0x4b8]\n  ldr x1, [x9, #0x4c0]\n  ldr x2, [x19, #40]\n"
        "  ldr x16, [x19, #8]\n  blr x16\n"                   /* main+0x3e60b0: the render-only list onto the queue */
        "  ldr x9, [x19, #32]\n  ldr x0, [x9, #0x4b8]\n  ldr x16, [x19]\n  blr x16\n"   /* and run it */
        "  adrp x16, _b2_after_runs@PAGE\n  add x16, x16, _b2_after_runs@PAGEOFF\n  blr x16\n"   /* the stage slots of both runs (runtime_b2_twin) */
        "  mov w8, #3\n  str w8, [x19, #28]\n"   /* after the second run (the post-calc pass, the final assembly): phase 3 */
        "  ldr x9, [x19, #480]\n  ldr x9, [x9]\n  cbz x9, 22f\n"   /* back to the game's camera */
        "  ldr s0, [x19, #488]\n  ldr s1, [x9, #20]\n  fadd s1, s1, s0\n  str s1, [x9, #20]\n"
        "22:\n  ldr w8, [x19, #24]\n  add w8, w8, #1\n  str w8, [x19, #24]\n"
        "9:\n  ldr x23, [sp, #48]\n  ldp x21, x22, [sp, #32]\n  ldp x19, x20, [sp, #16]\n  ldp x29, x30, [sp], #64\n  ret\n");
extern void vr_b2_hook(void);
_Static_assert(offsetof(vr_b2_state,on)==16&&offsetof(vr_b2_state,runs)==24&&offsetof(vr_b2_state,phase)==28&&offsetof(vr_b2_state,stage)==32&&offsetof(vr_b2_state,list)==40&&offsetof(vr_b2_state,begin)==48&&offsetof(vr_b2_state,begin_obj)==56&&offsetof(vr_b2_state,vmgr)==64&&offsetof(vr_b2_state,mask)==20&&offsetof(vr_b2_state,save)==72&&offsetof(vr_b2_state,step_flags)==72+27*8&&offsetof(vr_b2_state,step_bits)==72+27*8+32*4&&offsetof(vr_b2_state,camp)==480&&offsetof(vr_b2_state,eh)==488,"vr_b2_hook offsets");
static uint64_t vr_b2_job_fn;   /* the render jobs' run function (main+0x2887b80) */
static void vr_b2_install(Module *m){
    const uint64_t site=0x3e5adc,runner=0x3e5dfc,setlist=0x3e60b0,job_fn=0x2887b80,data=(m->file_size+0x3fff)&~0x3fffULL,v0=((data-64)&~15ULL)-32;
    uint32_t got;memcpy(&got,m->base+site,4);
    const uint32_t want=0x14000000u|((uint32_t)((int64_t)(runner-site)>>2)&0x3ffffff);
    if(got!=want){fprintf(stderr,"[vr] stereo B2: main+%#llx is %08x, not 1.4.3's tail call (%08x); off\n",(unsigned long long)site,got,want);return;}
    if(v0<m->patch_off){fprintf(stderr,"[vr] stereo B2: no room in main's patch area tail; off\n");return;}
    for(unsigned i=0;i<16;i++)if(m->base[v0+i]){fprintf(stderr,"[vr] stereo B2: patch area tail at +%#llx in use; off\n",(unsigned long long)v0);return;}
    uint32_t c[4]={0x58000050u /* ldr x16, #8 */,0xd61f0200u /* br x16 */,0,0};uint64_t hook=(uint64_t)(uintptr_t)vr_b2_hook;memcpy(&c[2],&hook,8);
    const uint32_t b=aspect_b(site,v0);if(!b){fprintf(stderr,"[vr] stereo B2: branch out of range; off\n");return;}
    memset(&g_vr_b2,0,sizeof g_vr_b2);g_vr_b2.run=(uint64_t)(uintptr_t)(m->base+runner);g_vr_b2.setlist=(uint64_t)(uintptr_t)(m->base+setlist);
    vr_b2_job_fn=(uint64_t)(uintptr_t)(m->base+job_fn);g_vr_b2.camp=(uint64_t)(uintptr_t)&g_vr_cam.cam;g_vr_b2.begin=(uint64_t)(uintptr_t)(m->base+0x3dbf28);g_vr_b2.vmgr=(uint64_t)(uintptr_t)(m->base+0x3acda18);
    uint32_t stage_word;memcpy(&stage_word,m->base+0x97aa4,4);
    if(stage_word!=0xf9002b00u)runtime_fail("VR summary owner: original instruction mismatch");
    for(unsigned i=0;i<16;i++)if(m->base[v0-16+i])runtime_fail("VR summary veneer is occupied");
    uint32_t stage_stub[4]={0x58000050u,0xd61f0200u,0,0};uint64_t stage_hook=(uint64_t)(uintptr_t)vr_stage_hook;
    memcpy(stage_stub+2,&stage_hook,8);g_b2_stage_return=(uint64_t)(uintptr_t)(m->base+0x97aa8);
    uint32_t stage_branch=aspect_b(0x97aa4,v0-16);if(!stage_branch)runtime_fail("VR summary veneer out of range");
    memcpy(m->base+v0-16,stage_stub,sizeof stage_stub);memcpy(m->base+0x97aa4,&stage_branch,4);
    memcpy(m->base+v0,c,sizeof c);memcpy(m->base+site,&b,4);
    fprintf(stderr,"[vr] stereo B2: last calc stage through the hook at main+%#llx (veneer +%#llx); second render run off (\"b2 1\")\n",(unsigned long long)site,(unsigned long long)v0);
}
/* The object of the calc-tree node (from node, as calctree walks it) whose method is fn; 0 when none. */
static uint64_t vr_b2_node_obj(uint64_t node,uint64_t fn,int depth){
    if(depth>24)return 0;
    uint64_t d[4]={0};jd_read(node+0x50,d,32);uint64_t f=d[2];const uint64_t obj=d[1]+(uint64_t)((int64_t)d[3]>>1);
    if((d[3]&1)&&obj){uint64_t vt=0;if(jd_read(obj,&vt,8))jd_read(vt+f,&f,8);}
    if(f==fn&&obj)return obj;
    uint64_t link=0;jd_read(node+0x28,&link,8);
    for(int i=0;link&&i<200;i++){uint64_t child=0;if(!jd_read(link+0x20,&child,8)||!child)break;
        const uint64_t r=vr_b2_node_obj(child,fn,depth+1);if(r)return r;if(!jd_read(child+0x30,&link,8))break;}
    return 0;
}
/* "b2 <0|1|2|3> [mask]": the second render run (modes above). The first "b2" that turns it on builds the render-only list: a breakpoint at main+0x3e5abc gives the
   stage object x19; its list [x19+0x4d8] (+8 entry count, +0x10 pointers to entries {.., +8 the job}, +0x30 dependency
   count, +0x38 pointers to dependencies {u16 before, u16 after}: indices in the list) gives every entry whose job's
   vtable entry 2 is main+0x2887b80, and every dependency between two of them, renumbered. The list object's other
   fields are copied. The memory is the runtime's own and lives for the session. */
static void *vr_b2_find(void *arg){
    const int on=(int)(intptr_t)arg;
    if(on&&!g_vr_b2.list){
        bp_reg=19;bp_addr=(uintptr_t)diag_main->base+0x3e5abc;atomic_store(&bp_x1,0);atomic_store(&bp_n,0);
        for(int t=0;t<300&&!atomic_load(&bp_n);t++){bp_all(1);usleep(10000);}
        bp_all(0);const uint64_t x19=atomic_load(&bp_x1);bp_addr=0;bp_reg=1;
        uint64_t L=0;uint8_t head[0x48];
        if(!x19||!jd_read(x19+0x4d8,&L,8)||!jd_read(L,head,sizeof head)){fprintf(stderr,"[vr] b2: the job list was not found\n");return NULL;}
        int32_t cnt,ecnt;uint64_t arr,earr;memcpy(&cnt,head+8,4);memcpy(&arr,head+0x10,8);memcpy(&ecnt,head+0x30,4);memcpy(&earr,head+0x38,8);
        if(cnt<=0||cnt>256||ecnt<0||ecnt>4096){fprintf(stderr,"[vr] b2: list sizes %d / %d out of range\n",cnt,ecnt);return NULL;}
        int map[256];uint64_t *na=calloc((size_t)cnt,8);uint32_t n=0;
        for(int i=0;i<cnt;i++){uint64_t ent=0,job=0,vt=0,fn=0;map[i]=-1;
            if(jd_read(arr+8*(uint64_t)i,&ent,8)&&jd_read(ent+8,&job,8)&&jd_read(job,&vt,8)&&jd_read(vt+16,&fn,8)&&fn==vr_b2_job_fn){map[i]=(int)n;na[n++]=ent;}}
        uint64_t *ne=calloc((size_t)ecnt+1,8);uint16_t *edges=calloc((size_t)ecnt+1,4);uint32_t m=0,dropped=0;
        for(int i=0;i<ecnt;i++){uint64_t e=0;uint16_t ab[2];if(!jd_read(earr+8*(uint64_t)i,&e,8)||!jd_read(e,ab,4))continue;
            if(ab[0]>=cnt||ab[1]>=cnt||map[ab[0]]<0||map[ab[1]]<0){dropped++;continue;}
            edges[2*m]=(uint16_t)map[ab[0]];edges[2*m+1]=(uint16_t)map[ab[1]];ne[m]=(uint64_t)(uintptr_t)&edges[2*m];m++;}
        uint8_t *nl=calloc(1,sizeof head);memcpy(nl,head,sizeof head);
        int32_t n32=(int32_t)n,m32=(int32_t)m;uint64_t pa=(uint64_t)(uintptr_t)na,pe=(uint64_t)(uintptr_t)ne;
        memcpy(nl+8,&n32,4);memcpy(nl+0x10,&pa,8);memcpy(nl+0x30,&m32,4);memcpy(nl+0x38,&pe,8);
        /* the render system's per-frame start: the calc-tree node whose method is main+0x3db120 (calctree's walk) */
        bp_reg=0;bp_addr=(uintptr_t)diag_main->base+0x2dadd8;atomic_store(&bp_x1,0);atomic_store(&bp_n,0);
        for(int t=0;t<300&&!atomic_load(&bp_n);t++){bp_all(1);usleep(10000);}
        bp_all(0);const uint64_t root=atomic_load(&bp_x1);bp_addr=0;bp_reg=1;
        const uint64_t bobj=root?vr_b2_node_obj(root,(uintptr_t)diag_main->base+0x3db120,0):0;
        if(!bobj){fprintf(stderr,"[vr] b2: the render system's per-frame start was not found in the calc tree\n");return NULL;}
        g_vr_b2.begin_obj=bobj;
        /* the view handler of the render stage 1 (the camera constants' upload, main+0x97fb0 case 1 at main+0x984e4: x0 its
           view renderer, [x0+0x8a8] the view): b2_stage1_run1 runs it for the first run's render step */
        bp_reg=0;bp_addr=(uintptr_t)diag_main->base+0x984e4;atomic_store(&bp_x1,0);atomic_store(&bp_n,0);
        for(int t=0;t<300&&!atomic_load(&bp_n);t++){bp_all(1);usleep(10000);}
        bp_all(0);b2_s1_deleg=atomic_load(&bp_x1);bp_addr=0;bp_reg=1;
        {uint64_t v=0;if(b2_s1_deleg)jd_read(b2_s1_deleg+0x8a8,&v,8);fprintf(stderr,"[vr] b2: the camera upload (render stage 1) of view renderer %#llx, view %#llx\n",(unsigned long long)b2_s1_deleg,(unsigned long long)v);}
        /* the summary slots' owners: x25 where the render stage's summary recording ends (main+0x97aa4, after EndRecording;
           the handle goes to [x24+0x50], x24 = [x25+0xd8] + 0x98 * (byte [x25+0xe1] * half [x25+0xea] + stage slot)) */
        for(unsigned i=0;i<8;i++){atomic_store(&bp_seen[i].v,0);atomic_store(&bp_seen[i].n,0);atomic_store(&bp_seen[i].raw,0);}
        bp_reg=25;bp_collect=1;bp_addr=(uintptr_t)diag_main->base+0x97aa4;
        for(int t=0;t<50;t++){bp_all(1);usleep(10000);}
        bp_all(0);bp_addr=0;bp_collect=0;bp_reg=1;b2_slot_n=0;
        for(unsigned i=0;i<8;i++){const uint64_t o=atomic_load(&bp_seen[i].v);if(!o)continue;b2_slot_obj[b2_slot_n++]=o;
            int32_t n=0;uint8_t half=0;uint16_t stride=0;uint64_t arr=0;jd_read(o+0xd0,&n,4);jd_read(o+0xe1,&half,1);jd_read(o+0xea,&stride,2);jd_read(o+0xd8,&arr,8);
            fprintf(stderr,"[vr] b2: render stage summaries of %#llx: %d slots of 0x98 at %#llx, %u a half, current half %u (%u recordings in 0.5 s)\n",(unsigned long long)o,n,(unsigned long long)arr,stride,half,atomic_load(&bp_seen[i].n));}
        g_vr_b2.stage=x19;__atomic_store_n(&g_vr_b2.list,(uint64_t)(uintptr_t)nl,__ATOMIC_RELEASE);
        fprintf(stderr,"[vr] b2: render-only list: %u of %d jobs, %u of %d dependencies (%u to other jobs dropped); per-frame start on %#llx\n",n,cnt,m,ecnt,dropped,(unsigned long long)bobj);
        if(!n)return NULL;
    }
    __atomic_store_n(&g_vr_b2.on,(uint32_t)on,__ATOMIC_RELEASE);
    fprintf(stderr,"[vr] b2: second render run %s (%u done so far)\n",!on?"off":on==2?"on, views' job state only":"on, views' per-frame start",g_vr_b2.runs);
    return NULL;
}
/* Draws recorded per run (hle_nvn.c at each recording's end): the first run's (with the frame's few other recordings) and
   the second run's, averaged per frame every 60 second runs: the second run is complete when the two agree.
   Each recording's handle (the runtime's copy of it, hle_nvn.c) is kept with its run and draws (b2_tab, the last 8192),
   and the handles the frame calls (CallCommands: the frame's primary command buffers call the recordings) are counted
   by run: which run's pictures the game's single submission holds. */
static void *_Atomic b2_left_cap;
static _Atomic uint32_t b2_cap_frame;static _Atomic uint64_t b2_cap_taken,b2_cap_mismatch,b2_why[4],b2_slot_hits;static _Atomic uint32_t b2_twin_frame;   /* the left capture's frame against the twins' (runtime_b2_left_cap) */
/* Called by vr_b2_hook between the runs (the game's thread, the first run complete): the uniforms the first run's
   recordings bind, captured now (mtl_sub_capture_ubos, as the translation thread mode does at the submit call), for the
   left replay of the stereo submission (hle_nvn.c submit_exec): the second run writes the same blocks. */
/* Render stage 1 for the first run (mask bit 8, stereo B2 step 2): the game uploads each view's camera constants (the
   camera blocks: view, projection, the frames before, the shadow cascades) only after the calc stage, in render stage 1
   (main+0x97fb0 with stage 1, run by the post-calc pass), from the view's current render step: the views' per-frame
   start flips between two render steps, each with its own blocks (entries of 0x480 at [step+0x18], block pointer at
   +0x3e8). With two runs the first run's step is never uploaded: its recordings bound blocks that kept the camera of
   the frame stereo began (b2ah / b2aj: the left eye's camera fixed while the right one turned with the head). So between
   the runs the view handler of stage 1 runs here for the first run's step, as the game does it (main+0x984e4..0x982bc):
   the view's +0x170 list (vt+0x48 (object, step), the default no-op left out), main+0xe31d4 (step, [view+0x3a80] bit 2),
   then [view+0x3ab0] vt+0x88 (object, step, view). The game's state is the one stage 1 sees after a frame's run. */
static void b2_stage1_run1(void){
    const uint64_t d=b2_s1_deleg,mb=(uintptr_t)diag_main->base;if(!d)return;
    uint64_t view=0,sa=0,dflt=0;int8_t idx=0;int32_t ns=0;uint8_t fl909=0;uint16_t fl8a4=0;uint64_t f228=1;
    if(!jd_read(d+0x8a8,&view,8)||!view||!jd_read(d+0x908,&idx,1)||!jd_read(d+0x909,&fl909,1)||!jd_read(d+0x8a4,&fl8a4,2)||!jd_read(d+0x228,&f228,8))return;
    if(f228||!(fl8a4&0x10)||(fl909&8))return;   /* the conditions of the game's inline view handler */
    jd_read(view+0x130,&ns,4);jd_read(view+0x138,&sa,8);jd_read(mb+0x3970540,&dflt,8);
    const int32_t k=(uint32_t)ns>(uint32_t)(int32_t)idx?idx:0;const uint64_t step=sa+(uint64_t)k*0x2920;
    uint16_t sf=0;jd_read(step+0x2914,&sf,2);if(!(sf&1))return;
    int32_t nl=0;uint64_t la=0;jd_read(view+0x170,&nl,4);jd_read(view+0x178,&la,8);
    for(int32_t i=0;i<nl&&i<64;i++){uint64_t o=0,vt=0,fn=0;if(!jd_read(la+8*(uint64_t)i,&o,8)||!o||!jd_read(o,&vt,8)||!jd_read(vt+0x48,&fn,8)||fn==dflt)continue;
        ((void(*)(uint64_t,uint64_t))(uintptr_t)fn)(o,step);}
    uint32_t f3a80=0;jd_read(view+0x3a80,&f3a80,4);
    ((void(*)(uint64_t,uint32_t))(mb+0xe31d4))(step,(f3a80>>2)&1);
    uint64_t o=0,vt=0,fn=0;if(jd_read(view+0x3ab0,&o,8)&&o&&jd_read(o,&vt,8)&&jd_read(vt+0x88,&fn,8)&&fn)((void(*)(uint64_t,uint64_t,uint64_t))(uintptr_t)fn)(o,step,view);
    static int said;if(said<3){said++;uint64_t ents=0,blk=0;int32_t ne=0;jd_read(step+0x10,&ne,4);jd_read(step+0x18,&ents,8);if(ents)jd_read(ents+0x3e8,&blk,8);
        fprintf(stderr,"[vr] b2: render stage 1 for the first run: view %#llx, render step %d of %d (%#llx, %d cameras, the first one's block %#llx)\n",(unsigned long long)view,k,ns,(unsigned long long)step,ne,(unsigned long long)blk);}
}
void b2_between_runs(void);
void *runtime_b2_left_cap(void){
    void *c=atomic_exchange(&b2_left_cap,NULL);
    if(c){atomic_fetch_add(&b2_cap_taken,1);if(atomic_load(&b2_cap_frame)!=atomic_load_explicit(&b2_twin_frame,memory_order_relaxed))atomic_fetch_add(&b2_cap_mismatch,1);}
    return c;}
static _Atomic uint64_t b2_draws[3],b2_calls[4],b2_cdraws[4];static _Atomic uint64_t b2_twin_hits,b2_twin_miss;static _Atomic uint32_t b2_printed;
enum{B2_TAB=8192};static struct{_Atomic uint64_t h;uint64_t key;uint32_t draws,frame;uint16_t occ;uint8_t phase;uint32_t ra[6];} b2_tab[B2_TAB];   /* ra: the innermost main return addresses (diagnostic) */
/* A recording's identity: the guest code that recorded it (its 3 innermost return addresses in main, from the host frames
   through the HLE stack switch's frame record) and how many recordings with that key the same run made before it. */
/* The summary's arguments identify the renderer and render-layer object.
   Recording order and call stacks vary with worker scheduling. */
static pthread_mutex_t b2_stage_mu=PTHREAD_MUTEX_INITIALIZER;
static struct { uint64_t owner,layer,handle;uint32_t frame;int phase; } b2_stage_records[2048];
static unsigned b2_stage_next;
void b2_stage_record(uint64_t handle,uint64_t owner,uint64_t layer) {
    if(!g_vr_b2.on||!owner||!layer)return;
    extern uint64_t hle_vr_copy_record(uint64_t);
    const uint64_t identity=hle_vr_copy_record(handle);unsigned phase=(unsigned)identity;uint32_t frame=(uint32_t)(identity>>32);
    if(phase>1)return;
    pthread_mutex_lock(&b2_stage_mu);
    extern void hle_vr_copy_retain(uint64_t);
    unsigned at=2048;for(unsigned i=0;i<2048;i++)if(!b2_stage_records[i].handle){at=i;break;}
    if(at==2048)runtime_fail("VR stage ownership table is full");
    hle_vr_copy_retain(handle);b2_stage_next++;
    b2_stage_records[at].owner=owner;b2_stage_records[at].layer=layer;b2_stage_records[at].handle=handle;
    b2_stage_records[at].frame=frame;b2_stage_records[at].phase=(int)phase;
    if(b2_stage_next<=32)fprintf(stderr,"[vr] stage identity frame %u eye %d owner %#llx layer %#llx handle %#llx\n",frame,(int)phase,(unsigned long long)owner,(unsigned long long)layer,(unsigned long long)handle);
    pthread_mutex_unlock(&b2_stage_mu);
}
__asm__(".text\n.p2align 4\n.globl _vr_stage_hook\n_vr_stage_hook:\n"
        "str x0,[x24,#0x50]\nsub sp,sp,#320\nstp x0,x1,[sp]\nstp x2,x3,[sp,#16]\n"
        "stp x4,x5,[sp,#32]\nstp x6,x7,[sp,#48]\nstp x8,x9,[sp,#64]\nstp x10,x11,[sp,#80]\n"
        "stp x12,x13,[sp,#96]\nstp x14,x15,[sp,#112]\nstp x18,x30,[sp,#128]\n"
        "mrs x17,nzcv\nstr x17,[sp,#144]\nstp q0,q1,[sp,#160]\nstp q2,q3,[sp,#192]\nstp q4,q5,[sp,#224]\nstp q6,q7,[sp,#256]\n"
        "ldr x1,[sp,#400]\nldr x2,[sp,#408]\nbl _b2_stage_record\n"
        "ldp q0,q1,[sp,#160]\nldp q2,q3,[sp,#192]\nldp q4,q5,[sp,#224]\nldp q6,q7,[sp,#256]\n"
        "ldr x17,[sp,#144]\nmsr nzcv,x17\nldp x18,x30,[sp,#128]\nldp x14,x15,[sp,#112]\nldp x12,x13,[sp,#96]\n"
        "ldp x10,x11,[sp,#80]\nldp x8,x9,[sp,#64]\nldp x6,x7,[sp,#48]\nldp x4,x5,[sp,#32]\nldp x2,x3,[sp,#16]\nldp x0,x1,[sp]\nadd sp,sp,#320\n"
        "adrp x16,_g_b2_stage_return@PAGE\nldr x16,[x16,_g_b2_stage_return@PAGEOFF]\nbr x16\n");
extern void vr_stage_hook(void);
void b2_between_runs(void){
    if(g_vr_b2.mask&0x100)b2_stage1_run1();
    extern void *mtl_sub_capture_ubos(const uint64_t *,int);extern void mtl_ucap_free(void *);
    const uint32_t frame=__atomic_load_n(&g_vr_b2.runs,__ATOMIC_RELAXED);uint64_t handles[2048];int count=0;
    pthread_mutex_lock(&b2_stage_mu);
    for(unsigned i=0;i<2048;i++)if(b2_stage_records[i].handle&&b2_stage_records[i].frame==frame&&b2_stage_records[i].phase==0)handles[count++]=b2_stage_records[i].handle;
    void *capture=count&&(g_vr_b2.mask&0x80)?mtl_sub_capture_ubos(handles,count):NULL;
    pthread_mutex_unlock(&b2_stage_mu);
    atomic_store(&b2_cap_frame,frame);void *old=atomic_exchange(&b2_left_cap,capture);if(old)mtl_ucap_free(old);
}
static uint64_t b2_rec_key(uint32_t ra[6]){
    const uint64_t mlo=(uintptr_t)diag_main->base,mhi=mlo+diag_main->size;uint64_t fp=(uintptr_t)__builtin_frame_address(0),k=0xcbf29ce484222325ull;int got=0;
    for(int i=0;i<6;i++)ra[i]=0;
    for(int q=0;q<64&&got<6&&fp&&!(fp&15);q++){uint64_t fr[2];if(!jd_read(fp,fr,16))break;if(fr[1]>=mlo&&fr[1]<mhi){if(got<3)k=(k^(fr[1]-mlo))*0x100000001b3ull;ra[got++]=(uint32_t)(fr[1]-mlo);}fp=fr[0];}
    return k;
}
/* The first run's recordings by identity (key, occurrence, frame): the twin of a second-run recording. */
static struct{_Atomic uint64_t h;uint64_t key;uint32_t frame;uint16_t occ;} b2_lidx[8192];
static struct{uint64_t key;uint32_t frame;uint16_t n;uint8_t phase;} b2_occ[1024];static pthread_mutex_t b2_occ_mu=PTHREAD_MUTEX_INITIALIZER;
static uint16_t b2_occ_next(uint64_t key,uint32_t frame,uint8_t phase){
    pthread_mutex_lock(&b2_occ_mu);unsigned i=(unsigned)(key*0x9E3779B97F4A7C15ull>>54)&1023;uint16_t r=0;
    for(int t=0;t<1024;t++,i=(i+1)&1023){if(b2_occ[i].frame!=frame||b2_occ[i].phase!=phase||!b2_occ[i].key){b2_occ[i].key=key;b2_occ[i].frame=frame;b2_occ[i].phase=phase;b2_occ[i].n=1;r=0;break;}
        if(b2_occ[i].key==key){r=b2_occ[i].n++;break;}}
    pthread_mutex_unlock(&b2_occ_mu);return r;
}
void runtime_b2_rec(uint32_t draws,uint64_t handle){
    if(!g_vr_b2.on)return;
    const uint32_t ph0=__atomic_load_n(&g_vr_b2.phase,__ATOMIC_RELAXED);const int ph=ph0==1?1:ph0==3?2:0;   /* 0 first run, 1 second run, 2 after it (the post-calc pass, the final assembly) */
    atomic_fetch_add_explicit(&b2_draws[ph],draws,memory_order_relaxed);
    const unsigned k=(unsigned)((handle>>4)*0x9E3779B97F4A7C15ull>>51)&(B2_TAB-1);
    uint32_t ra[6];const uint64_t key=b2_rec_key(ra);const uint32_t frame=__atomic_load_n(&g_vr_b2.runs,__ATOMIC_RELAXED)-(ph==2?1u:0u);   /* after the second run the counter has moved on */
    {extern void hle_vr_copy_tag(uint64_t,uint32_t,unsigned);hle_vr_copy_tag(handle,frame,(unsigned)ph);}
    memcpy(b2_tab[k].ra,ra,sizeof ra);b2_tab[k].draws=draws;b2_tab[k].phase=(uint8_t)ph;b2_tab[k].key=key;b2_tab[k].frame=frame;b2_tab[k].occ=b2_occ_next(key,frame,(uint8_t)ph);
    atomic_store_explicit(&b2_tab[k].h,handle,memory_order_release);

    if(ph==0){const unsigned j=(unsigned)((key^((uint64_t)b2_tab[k].occ*0x9E3779B97F4A7C15ull)^((uint64_t)frame<<32))*0xD6E8FEB86659FD93ull>>51)&(B2_TAB-1);
        b2_lidx[j].key=key;b2_lidx[j].occ=b2_tab[k].occ;b2_lidx[j].frame=frame;atomic_store_explicit(&b2_lidx[j].h,handle,memory_order_release);}
    const uint32_t runs=__atomic_load_n(&g_vr_b2.runs,__ATOMIC_RELAXED);uint32_t last=atomic_load(&b2_printed);
    if(runs>=last+60&&atomic_compare_exchange_strong(&b2_printed,&last,runs)){
        const uint64_t a=atomic_exchange(&b2_draws[0],0),b=atomic_exchange(&b2_draws[1],0),f=atomic_exchange(&b2_draws[2],0);const uint32_t n=last?runs-last:60;
        uint64_t c[4],d[4];for(int i=0;i<4;i++){c[i]=atomic_exchange(&b2_calls[i],0);d[i]=atomic_exchange(&b2_cdraws[i],0);}
        {const uint64_t th=atomic_exchange(&b2_twin_hits,0),tm=atomic_exchange(&b2_twin_miss,0);if(th||tm)fprintf(stderr,"[vr] b2 twins: %.1f second-run recordings replaced by their first-run twin a frame, %.1f without a twin\n",(double)th/n,(double)tm/n);
         uint64_t w[4];for(int i=0;i<4;i++)w[i]=atomic_exchange(&b2_why[i],0);
         fprintf(stderr,"[vr] b2 twins: calls not paired a frame: %.1f not second-run recordings, %.1f of the frame in progress, %.1f too old, %.1f without a twin; left captures of another frame than their twins %llu of %llu\n",
                 (double)w[0]/n,(double)w[1]/n,(double)w[2]/n,(double)w[3]/n,(unsigned long long)atomic_exchange(&b2_cap_mismatch,0),(unsigned long long)atomic_exchange(&b2_cap_taken,0));
         fprintf(stderr,"[vr] b2 twins: by renderer/layer identities %.1f a frame (%.1f stage pairs owned a frame)\n",(double)atomic_exchange(&b2_slot_hits,0)/n,(double)atomic_exchange(&b2_map_n,0)/n);}
        fprintf(stderr,"[vr] b2 draws a frame: first run %.0f, second run %.0f, after it %.0f; calls of first-run recordings %.1f (%.0f draws), second-run %.1f (%.0f), after-run %.1f (%.0f), unknown %.1f (%u frames, mode %u mask %#x)\n",
                (double)a/n,(double)b/n,(double)f/n,(double)c[0]/n,(double)d[0]/n,(double)c[1]/n,(double)d[1]/n,(double)c[2]/n,(double)d[2]/n,(double)c[3]/n,n,g_vr_b2.on,g_vr_b2.mask);
        {const uint64_t cam=g_vr_cam.cam,writer=__atomic_load_n(&g_vr_cam.native_writer,__ATOMIC_ACQUIRE);float tx=0;if(cam)memcpy(&tx,(const void *)(uintptr_t)(cam+20),4);fprintf(stderr,"[vr] b2 eyes: half the eye distance %.4f m; the main camera %#llx, native writer %#llx, owner match %d, its view t.x now %.4f\n",g_vr_b2.eh,(unsigned long long)cam,(unsigned long long)writer,runtime_vr_camera_owned(cam,0),tx);}}
}
/* "b2calls": the calls of known recordings (first / second run) between the next two presents, in order, by caller
   (the command buffer object, and the run it was in when it called): how the game's frame strings the runs' recordings
   together. Calls of recordings the table does not know (made before, e.g. the materials' small command lists) are
   only counted. */
enum{B2_CL=4096};static struct{uint64_t caller,where;uint32_t draws;uint8_t callee,inrun;} b2_cl[B2_CL];
static _Atomic uint32_t b2_cln,b2_clunk;static _Atomic int b2_clstate;   /* 1 armed, 2 recording */
static uint64_t b2_clchain[12];static _Atomic int b2_clchain_n;   /* the guest's call chain at the first call from a stack array (the final assembly) */
void runtime_vr_b2_calls(void){atomic_store(&b2_cln,0);atomic_store(&b2_clunk,0);atomic_store(&b2_clstate,1);fprintf(stderr,"[vr] b2calls: the next frame's calls\n");}
static void b2_calls_present(void){
    int st=atomic_load(&b2_clstate);
    if(st==1){atomic_store(&b2_cln,0);atomic_store(&b2_clunk,0);atomic_store(&b2_clstate,2);return;}
    if(st!=2)return;
    atomic_store(&b2_clstate,0);uint32_t n=atomic_load(&b2_cln);if(n>B2_CL)n=B2_CL;
    fprintf(stderr,"[vr] b2calls: %u calls of known recordings, %u of others\n",n,atomic_load(&b2_clunk));
    {int cn=atomic_load(&b2_clchain_n);char cl[900];size_t cu=(size_t)snprintf(cl,sizeof cl,"[vr] b2calls the first call from a stack array came from:");
     for(int i=0;i<cn&&cu<sizeof cl-64;i++){char d[64];vr_desc(b2_clchain[i],d,sizeof d);cu+=(size_t)snprintf(cl+cu,sizeof cl-cu," < %s",d);}
     fprintf(stderr,"%s\n",cl);atomic_store(&b2_clchain_n,0);}
    char line[1200];size_t u=0;uint64_t cur=~0ull;int curin=-1;
    for(uint32_t i=0;i<=n;i++){
        if(i==n||b2_cl[i].caller!=cur||b2_cl[i].inrun!=curin){if(u)fprintf(stderr,"%s\n",line);if(i==n)break;
            cur=b2_cl[i].caller;curin=b2_cl[i].inrun;u=(size_t)snprintf(line,sizeof line,"[vr] b2calls caller %#llx (%s):",(unsigned long long)cur,curin==1?"in the second run":curin==3?"after the second run":"in the first run");}
        if(b2_cl[i].callee==4){if(u<sizeof line-48)u+=(size_t)snprintf(line+u,sizeof line-u," ?%#x@%#llx",b2_cl[i].draws,(unsigned long long)b2_cl[i].where);continue;}   /* unknown, low 32 bits of the handle */
        if(u<sizeof line-40)u+=(size_t)snprintf(line+u,sizeof line-u," %c%u","LRP"[b2_cl[i].callee<3?b2_cl[i].callee:0],b2_cl[i].draws);
        if(!b2_cl[i].inrun&&b2_cl[i].callee&&u<sizeof line-40)u+=(size_t)snprintf(line+u,sizeof line-u,"@%#llx",(unsigned long long)b2_cl[i].where);}   /* where the game passed the handle from */
}
/* "b2slots" (stereo B2 diagnostic): where the game keeps the runs' recording handles between frames. Scans the view
   manager [main+0x3acda18] (0x400 bytes), each of its views (+0x10 count, +0x18 views: 0x3c00 bytes each, and the
   view's render steps: +0x130 of them at +0x138, 0x2920 bytes each) and the render system [main+0x3acdf90] (0x1000
   bytes) for words that are handles of the table (b2_tab): first-run (L) or second-run (R) recordings. */
static int b2_known(uint64_t v,int *ph,uint32_t *dr){const unsigned k=(unsigned)((v>>4)*0x9E3779B97F4A7C15ull>>51)&(B2_TAB-1);
    if(!v||atomic_load_explicit(&b2_tab[k].h,memory_order_acquire)!=v)return 0;*ph=b2_tab[k].phase;*dr=b2_tab[k].draws;return 1;}
static int b2_scan(uint64_t base,uint64_t len,const char *what,int shown){
    static uint64_t buf[0x800];
    for(uint64_t off=0;off<len&&shown<96;off+=sizeof buf){const uint64_t n=len-off<sizeof buf?len-off:sizeof buf;if(!jd_read(base+off,buf,n))continue;
        for(uint64_t i=0;i<n/8&&shown<96;i++){int ph;uint32_t dr;if(!b2_known(buf[i],&ph,&dr))continue;
            fprintf(stderr,"[vr] b2slots %s+%#llx (%#llx) = %c handle, %u draws\n",what,(unsigned long long)(off+8*i),(unsigned long long)(base+off+8*i),ph?'R':'L',dr);shown++;}}
    return shown;
}
static void *b2_slots_run(void *arg){
    (void)arg;int shown=0;const uint64_t mb=(uintptr_t)diag_main->base;uint64_t V=0,C=0;jd_read(mb+0x3acda18,&V,8);jd_read(mb+0x3acdf90,&C,8);
    if(V){shown=b2_scan(V,0x400,"viewmgr",shown);int32_t nv=0;uint64_t va=0;jd_read(V+0x10,&nv,4);jd_read(V+0x18,&va,8);
        for(int i=0;i<nv&&i<8;i++){uint64_t v=0;if(!jd_read(va+8*(uint64_t)i,&v,8)||!v)continue;char w[32];snprintf(w,sizeof w,"view%d",i);shown=b2_scan(v,0x3c00,w,shown);
            int32_t ns=0;uint64_t sa=0;jd_read(v+0x130,&ns,4);jd_read(v+0x138,&sa,8);
            if(ns>0&&ns<64&&sa){snprintf(w,sizeof w,"view%d.steps",i);fprintf(stderr,"[vr] b2slots view%d: %d render steps at %#llx\n",i,ns,(unsigned long long)sa);shown=b2_scan(sa,(uint64_t)ns*0x2920,w,shown);}}}
    if(C)shown=b2_scan(C,0x1000,"rendersys",shown);
    fprintf(stderr,"[vr] b2slots done: %d handles found\n",shown);return NULL;
}
/* "b2pair": the last complete frame's recordings by identity: first run (L) against second run (R), (key, occurrence)
   pairs matched, and the unmatched ones with their keys and draws. */
static void *b2_pair_run(void *arg){
    (void)arg;const uint32_t f=__atomic_load_n(&g_vr_b2.runs,__ATOMIC_RELAXED)-2;
    static struct{uint64_t key;uint32_t draws;uint16_t occ;uint8_t ph,used;} e[2048];int n=0;
    for(unsigned i=0;i<B2_TAB&&n<2048;i++){if(!atomic_load(&b2_tab[i].h)||b2_tab[i].frame!=f||b2_tab[i].phase>1)continue;e[n].key=b2_tab[i].key;e[n].draws=b2_tab[i].draws;e[n].occ=b2_tab[i].occ;e[n].ph=b2_tab[i].phase;e[n].used=0;n++;}
    {int hist[7][3]={{0}};int tot=0;for(unsigned i=0;i<B2_TAB;i++){if(!atomic_load(&b2_tab[i].h))continue;tot++;const int d=(int)(b2_tab[i].frame-f);if(d>=-3&&d<=3&&b2_tab[i].phase<3)hist[d+3][b2_tab[i].phase]++;}
     fprintf(stderr,"[vr] b2pair table: %d handles; by frame - %u (first / second / after):",tot,f);for(int d=0;d<7;d++)fprintf(stderr," %+d: %d/%d/%d",d-3,hist[d][0],hist[d][1],hist[d][2]);fprintf(stderr,"\n");}
    {int hist[7][3]={{0}};int tot=0;for(unsigned i=0;i<B2_TAB;i++){if(!atomic_load(&b2_tab[i].h))continue;tot++;const int d=(int)(b2_tab[i].frame-f);if(d>=-3&&d<=3&&b2_tab[i].phase<3)hist[d+3][b2_tab[i].phase]++;}
     fprintf(stderr,"[vr] b2pair table: %d handles; by frame - %u (first / second / after):",tot,f);for(int d=0;d<7;d++)fprintf(stderr," %+d: %d/%d/%d",d-3,hist[d][0],hist[d][1],hist[d][2]);fprintf(stderr,"\n");}
    int nl=0,nr=0,m=0,dd=0;for(int i=0;i<n;i++){if(e[i].ph)nr++;else nl++;}
    for(int i=0;i<n;i++){if(e[i].ph!=1)continue;for(int j=0;j<n;j++)if(!e[j].used&&e[j].ph==0&&e[j].key==e[i].key&&e[j].occ==e[i].occ){e[j].used=e[i].used=1;m++;if(e[j].draws!=e[i].draws)dd++;break;}}
    fprintf(stderr,"[vr] b2pair frame %u: %d first-run recordings, %d second-run, %d pairs by (code, order) (%d with different draws)\n",f,nl,nr,m,dd);
    int shown=0;for(int i=0;i<n&&shown<24;i++)if(!e[i].used){fprintf(stderr,"[vr] b2pair   unmatched %c key %#llx #%u, %u draws\n",e[i].ph?'R':'L',(unsigned long long)e[i].key,e[i].occ,e[i].draws);shown++;}
    return NULL;
}
void runtime_vr_b2_pair(void){pthread_t t;pthread_create(&t,NULL,b2_pair_run,NULL);pthread_detach(t);}
void runtime_vr_b2_slots(void){pthread_t t;pthread_create(&t,NULL,b2_slots_run,NULL);pthread_detach(t);}
/* hle_nvn.c CallCommands: the handle to inline instead of h, or 0. Mask bit 6 (a check of the pairing and of the first
   run's completeness): after the second run (the post-calc pass, the frame's final assembly), a second-run recording is
   replaced by its first-run twin, so the screen shows the first run (the left eye). */
/* Mask bit 7: stereo frames (the twin goes behind the second-run recording between OP_EYE marks, hle_nvn.c), counted
   in b2_marks so the submission that carries them is replayed once per eye (hle_nvn.c submit_exec). */
static _Atomic uint64_t b2_marks;
int runtime_b2_stereo(void){return g_vr_b2.on&&(g_vr_b2.mask&0x80);}
int runtime_b2_keep(void){return g_vr_b2.on&&(g_vr_b2.mask&0xc0)&&__atomic_load_n(&g_vr_b2.phase,__ATOMIC_RELAXED)==0;}   /* hle_nvn.c: first-run copies stay two submits */
uint64_t runtime_b2_stereo_marks(void){return atomic_load_explicit(&b2_marks,memory_order_relaxed);}
/* Outside the second run, a second-run recording of a frame already done: the final assembly (the post-calc pass runs
   asynchronously and sometimes still records after the next frame's stage has begun, phase 0; b2ae: half the stereo
   frames had no twins when only phase 3 counted). */
/* Stereo B2: the twins by the game's own data. A render stage's summary recording (main+0x96ca0, ending at main+0x97aa4)
   is stored in its owner's slot [owner+0xd8] + 0x98 * (half * per-half count + stage) at +0x50, the half ([owner+0xe1])
   flipping with each views' per-frame start: the first run fills one half, the second the other. When the second run
   ends (vr_b2_hook) both halves hold this frame's summaries, before the final assembly takes the second run's: each
   second-run summary is mapped to the first-run one of the same stage (b2_map). The call chain of a recording is no
   identity: the same job runs on the calc thread or a worker, inline or not (b2ar: every other frame's summaries unpaired). */
void b2_after_runs(void){
    const uint32_t frame=__atomic_load_n(&g_vr_b2.runs,__ATOMIC_RELAXED);
    extern void hle_vr_copy_bind(uint64_t,uint64_t);extern void hle_vr_copy_release(uint64_t);
    pthread_mutex_lock(&b2_stage_mu);
    for(unsigned i=0;i<2048;i++){
        __typeof__(b2_stage_records[0]) *right=&b2_stage_records[i];
        if(!right->handle||right->phase!=1||right->frame!=frame)continue;
        unsigned left_count=0,right_count=0;uint64_t left=0;
        for(unsigned j=0;j<2048;j++){
            __typeof__(b2_stage_records[0]) *candidate=&b2_stage_records[j];
            if(!candidate->handle||candidate->frame!=frame||candidate->owner!=right->owner||candidate->layer!=right->layer)continue;
            if(candidate->phase==0){left=candidate->handle;left_count++;}else right_count++;
        }
        if(left_count==1&&right_count==1){hle_vr_copy_bind(right->handle,left);atomic_fetch_add(&b2_map_n,1);}
    }
    for(unsigned i=0;i<2048;i++)if(b2_stage_records[i].handle&&b2_stage_records[i].frame==frame){hle_vr_copy_release(b2_stage_records[i].handle);b2_stage_records[i].handle=0;}
    pthread_mutex_unlock(&b2_stage_mu);
}
/* By the recording's own frame, not by the run in progress: the final assembly of a frame (asynchronous) sometimes runs
   while the next frame's second run is recording (b2am: every other stereo frame had no twins while calls during the
   second run were never paired). That run's own recordings (calls nested in its summaries) are of the frame in progress
   (frame == runs) and stay as they are. A recording more than two frames old is not paired: its twin's copy is kept for
   two submits only (hle_nvn.c keep_until). b2_why counts the calls that were not paired: [0] not a known second-run
   recording, [1] of the frame in progress, [2] too old, [3] no twin. */
uint64_t runtime_b2_twin(uint64_t h){
    if(!g_vr_b2.on||!(g_vr_b2.mask&0xc0))return 0;
    extern uint64_t hle_vr_copy_twin(uint64_t);
    uint64_t twin=hle_vr_copy_twin(h);
    if(twin){atomic_fetch_add_explicit(&b2_twin_hits,1,memory_order_relaxed);atomic_fetch_add(&b2_slot_hits,1);
        extern uint64_t hle_vr_copy_record(uint64_t);atomic_store_explicit(&b2_twin_frame,(uint32_t)(hle_vr_copy_record(h)>>32),memory_order_relaxed);
        if(g_vr_b2.mask&0x80)atomic_fetch_add_explicit(&b2_marks,1,memory_order_relaxed);return twin;}
    atomic_fetch_add_explicit(&b2_twin_miss,1,memory_order_relaxed);return 0;
}
void runtime_b2_called(int n,const uint64_t *h,uint64_t caller){
    if(!g_vr_b2.on)return;
    const int st=atomic_load_explicit(&b2_clstate,memory_order_relaxed);
    for(int i=0;i<n;i++){const unsigned k=(unsigned)((h[i]>>4)*0x9E3779B97F4A7C15ull>>51)&(B2_TAB-1);
        const int known=atomic_load_explicit(&b2_tab[k].h,memory_order_acquire)==h[i];const int ph=known?b2_tab[k].phase:3;
        atomic_fetch_add_explicit(&b2_calls[ph],1,memory_order_relaxed);if(known)atomic_fetch_add_explicit(&b2_cdraws[ph],b2_tab[k].draws,memory_order_relaxed);
        if(st==2){if(!known){atomic_fetch_add_explicit(&b2_clunk,1,memory_order_relaxed);
                /* an unknown handle passed from a stack array (the final assembly passes its handles that way): logged */
                const uint64_t w=(uint64_t)(uintptr_t)&h[i];
                if(w>=0x100000000ull&&w<0x200000000ull){const uint32_t j=atomic_fetch_add(&b2_cln,1);if(j<B2_CL){b2_cl[j].caller=caller;b2_cl[j].where=w;b2_cl[j].draws=(uint32_t)(h[i]&0xffffffffu);b2_cl[j].callee=4;b2_cl[j].inrun=(uint8_t)__atomic_load_n(&g_vr_b2.phase,__ATOMIC_RELAXED);}}
                continue;}
            {const uint64_t w=(uint64_t)(uintptr_t)&h[i];if(w>=0x160000000ull&&w<0x180000000ull&&!atomic_load(&b2_clchain_n)){   /* the first call from a stack array (the final assembly): the guest code that made it */
                const uint64_t mlo=(uintptr_t)diag_main->base,mhi=mlo+diag_main->size;uint64_t fp=(uintptr_t)__builtin_frame_address(0);int guest=0,cn=0;
                for(int q=0;q<64&&cn<12&&fp&&!(fp&15);q++){uint64_t fr[2];if(!jd_read(fp,fr,16))break;if(!guest&&fr[1]>=mlo&&fr[1]<mhi)guest=1;if(guest)b2_clchain[cn++]=fr[1];fp=fr[0];}
                atomic_store(&b2_clchain_n,cn);}}
            const uint32_t j=atomic_fetch_add(&b2_cln,1);if(j<B2_CL){b2_cl[j].caller=caller;b2_cl[j].where=(uint64_t)(uintptr_t)&h[i];b2_cl[j].draws=b2_tab[k].draws;b2_cl[j].callee=(uint8_t)ph;b2_cl[j].inrun=(uint8_t)__atomic_load_n(&g_vr_b2.phase,__ATOMIC_RELAXED);}}}
}
void runtime_vr_b2_set_eye(float m){if(m>0&&m<0.2f)g_vr_b2.eh=m;}   /* a valid eye distance only: "b2eye" stays otherwise */   /* the VR thread, every headset frame: half the eye distance (metres) */
void runtime_vr_b2_eye(float mm){g_vr_b2.eh=mm/1000.0f;fprintf(stderr,"[vr] b2: half the eye distance %.1f mm (the first run left of the game's camera, the second right)\n",mm);}
void runtime_vr_b2_mask(unsigned m){g_vr_b2.mask=m;fprintf(stderr,"[vr] b2: lists kept over the views' per-frame start: mask %#x\n",m);}
void runtime_vr_b2(int on){
    if(!g_vr_b2.run){fprintf(stderr,"[vr] b2: not installed\n");return;}
    if(bp_addr){fprintf(stderr,"[vr] b2: a breakpoint is in use\n");return;}
    pthread_t t;pthread_create(&t,NULL,vr_b2_find,(void *)(intptr_t)on);pthread_detach(t);
}
static void vr_headlook_install(Module *m){
    /* zeldaVR: also without VR, for the third-person camera distance (runtime_camera_distance_set) */
    const char *hl=getenv("TOTK_VR_HEADLOOK");
    if(hl&&!strcmp(hl,"0"))return;
    /* The two branches sit in the rewriter's patch area tail (main+file_size up to the data page, executable, never written
       by the rewriter), at its top end: main's own stubs fill it from the bottom up (benchmark camera, LOD, render distance:
       +0..+392 on 2026-10-30, about 6.4 KB free on 1.4.3), so the two only meet when the tail is full, and both check.
       Before: a fixed +256, which main's LOD stubs (+128..+368, the effects page) took over. Main's text padding (+944 / +968
       before 2026-10-29) is taken since main's high-FPS fade fix (+928..+1008, #466). */
    const uint64_t site=0x6289a4,call=0xbea74,callee=0xb33e8,data=(m->file_size+0x3fff)&~0x3fffULL,v1=(data-64)&~15ULL,v2=v1+32;
    uint32_t got_site,got_call,got_entry;memcpy(&got_site,m->base+site,4);memcpy(&got_call,m->base+call,4);memcpy(&got_entry,m->base+callee,4);
    uint32_t want_call=0x94000000u|((uint32_t)((int64_t)(callee-call)>>2)&0x3ffffff);
    if(got_site!=0xf9401268u||got_call!=want_call||got_entry!=0xa9bd7bfdu){fprintf(stderr,"[vr] head look: the camera code is not 1.4.3's (%08x %08x %08x); off\n",got_site,got_call,got_entry);return;}
    if(vr_active()){
        uint32_t arg,calc;uint64_t type,method;
        memcpy(&arg,m->base+0xb3438,4);memcpy(&calc,m->base+0x27d5438,4);
        memcpy(&type,m->base+0x39023e0,8);memcpy(&method,m->base+0x3902438,8);
        if(arg!=0xaa0303e2||calc!=0xbd409800||type!=(uintptr_t)m->base+0xc9350||method!=(uintptr_t)m->base+0x27d5438)
            runtime_fail("TotK native source projection owner/ABI does not match 1.4.3");
        const struct{uint64_t off;uint32_t word;} owners[]={{0x6286d4,0xf9400e68},{0x6286e4,0xf9400514},{0x628c14,0xf9400e68},{0x628c24,0xf9400914}};
        for(unsigned i=0;i<sizeof owners/sizeof *owners;i++){uint32_t word;memcpy(&word,m->base+owners[i].off,4);if(word!=owners[i].word)runtime_fail("TotK native camera holder relationship does not match 1.4.3");}
    }
    if(v1<m->patch_off||v2+24>data){fprintf(stderr,"[vr] head look: no room in main's patch area tail; off\n");return;}
    for(unsigned i=0;i<56;i++)if(m->base[v1+i]){fprintf(stderr,"[vr] head look: patch area tail at +%#llx in use; off\n",(unsigned long long)v1);return;}
    uint32_t c1[4]={0x58000050u /* ldr x16, #8 */,0xd61f0200u /* br x16 */,0,0};uint64_t hook=(uint64_t)(uintptr_t)vr_cam_owner_hook;memcpy(&c1[2],&hook,8);
    uint32_t c2[6]={0x58000050u /* ldr x16, #8 */,0xd61f0200u /* br x16 */,0,0,got_entry,aspect_b(v2+20,callee+4)};
    uint64_t v0h=(uint64_t)(uintptr_t)vr_view0_hook;memcpy(&c2[2],&v0h,8);
    uint32_t b1=aspect_b(site,v1),b2=aspect_b(callee,v2);
    if(!b1||!b2||!c2[5]){fprintf(stderr,"[vr] head look: branch out of range; off\n");return;}
    memset(&g_vr_cam,0,sizeof g_vr_cam);g_vr_cam.ret=(uint64_t)(uintptr_t)(m->base+site+4);
    g_vr_cam.callee=(uint64_t)(uintptr_t)(m->base+v2+16); /* original prologue, then original setter+4 */
    memcpy(m->base+v1,c1,sizeof c1);memcpy(m->base+v2,c2,sizeof c2);
    memcpy(m->base+site,&b1,4);memcpy(m->base+callee,&b2,4);
    g_vr_headlook=&g_vr_cam;
    runtime_player_init(m);   /* first person: the player's position (world_survey.c) */
    camera_distance_load();
    fprintf(stderr,"[vr] head look: camera hook at main+%#llx, all native views at main+%#llx, branches +%#llx (TOTK_VR_HEADLOOK=0 turns it off)\n",(unsigned long long)site,(unsigned long long)callee,(unsigned long long)v1);
}
#endif
static void patch_output_res(Module *m);
static void patch_render_res(Module *m);
static void patch_fixed_res(Module *m);
void runtime_game_patches(Module *m) {
    runtime_survey_init(m);
    if (strcmp(m->name, "main")) runtime_fail("1.4.3 game patches expect main");
    patch_realtime(m);
    patch_output_res(m);
    patch_render_res(m);
    patch_fixed_res(m);
    patch_slowmo_bounce(m);
    patch_fade_increment(m);
#if !TARGET_OS_IPHONE
    diag_main=m;   /* frmwatch / xpoke */
    { extern void (*g_vr_find_camera)(const float *); g_vr_find_camera=runtime_vr_find_camera; }   /* VR camera search */
#endif
#if !TARGET_OS_IPHONE
    /* VR head look widens the game's field of view to the headset's through the Vert+ stubs (runtime_aspect_vr) */
    { const char *ctl=getenv("TOTK_VR_CONTROL"),*hl=getenv("TOTK_VR_HEADLOOK"); if(ctl&&*ctl&&!(hl&&!strcmp(hl,"0")))setenv("TOTK_ASPECT_VERT","1",0); }
#endif
    { static const uint64_t sites[] = {0x628c74, 0xb43460, 0x1ff04f4, 0x253fbb0};   /* camera update and setup, viewport x2 */
      aspect_install(m, "1.4.3", sites, 4, 2, ((m->text_end + 15) & ~15ULL) + 48,   /* after the two veneers above */
                     ((m->text_end + 15) & ~15ULL) + 320,   /* Vert+ stubs (2 x 304 bytes): after the bounce veneer at +256, before the rewriter's code at +1024 */
                     (m->file_size + 0x3fff) & ~0x3fffULL); }
#if !TARGET_OS_IPHONE
    /* Not into the iOS export either (TOTK_EXPORT_IOS runs these patches on the Mac and embeds the code they leave): the
       hook branches into this Mac runtime, which the iPhone / iPad app does not have, so the exported game jumped to a
       Mac address and the iPad stopped at the first camera update ("Invalid Page", 2026-11-03, every 1.4.3 iOS build
       since zeldaVR came in, d1509b4b). */
    if (!getenv("TOTK_EXPORT_IOS")) vr_headlook_install(m);
    if (!getenv("TOTK_EXPORT_IOS") && vr_active()) vr_b2_install(m);   /* zeldaVR stereo B2 (step 1: the second render run, "b2 1") */
#endif
    patch_bench_cam(m);
    patch_lod_distance(m);
    patch_far_distance(m);
    patch_fov(m);   /* after aspect_install: Vert+ may hold the same load */
    patch_shadow_res(m);
    patch_dof(m);
    const char *enabled = getenv("TOTK_FPS60");
    if (enabled && !strcmp(enabled, "0")) {
        fprintf(stderr, "[totk] 1.4.3 shipping frame controller (TOTK_FPS60=0)\n");
        return;
    }
    for (unsigned i = 0; i < sizeof fps60 / sizeof *fps60; i++) {
#if !TARGET_OS_IPHONE
        uint32_t word;
        if (fps60[i].off + 4 > m->text_end) runtime_fail("1.4.3 FPS patch outside text");
        memcpy(&word, m->base + fps60[i].off, 4);
        if (word != fps60[i].original) runtime_fail("1.4.3 FPS patch original instruction mismatch");
#endif
    }
 #if TARGET_OS_IPHONE
    for (unsigned i = 0; i < sizeof fps60 / sizeof *fps60; i++) {
        uint32_t word; memcpy(&word,m->base+fps60[i].off,4);
        if(word!=fps60[i].replacement)runtime_fail("1.4.3 embedded FPS patch mismatch");
    }
 #else
    for (unsigned i = 0; i < sizeof fps60 / sizeof *fps60; i++)
        memcpy(m->base + fps60[i].off, &fps60[i].replacement, 4);
 #endif
    fprintf(stderr, "[totk] 1.4.3 60 FPS: seven verified controller instructions; original game frame-time calculation\n");
}

/* New display configuration object: +0x10 is display 0's canvas size, +0x508
 * its active crop, and +0x528 the floating display scale. The allocation path
 * multiplies the canvas by that scale (8d860..8d894). Publishing only +0x508
 * changes the final crop but leaves every UI/composition target at 1080p.
 * Pending output changes are bit 21 of +8. Keep the original rebuild path. */
static int output_full_at_start;
static unsigned output_w,output_h;
static _Atomic(uint8_t *) output_page;
static uint64_t pack_size(unsigned w,unsigned h){return (uint64_t)h<<32|w;}
/* zeldaVR: the canvas in the headset's shape (user 2026-10-30: "根本的改法": the game drew the headset's ~111 x 113 degree
   view into its 16:9 canvas, squeezed across, so a pixel covered 1.6 times more vertically than across and edges stepped
   badly). With VR on at the start the canvas (and the scene, which follows it) is 1.12 : 1: square pixels, as many as the 16:9 size has (vr_shape, below); std_shape gives the 16:9 size back (the modes and the
   saved picture mode are named by their 16:9 sizes).
   TOTK_VR_CANVAS=0 keeps 16:9; =<ratio> another shape. */
static float vr_canvas_aspect=-1;
#if TARGET_OS_IPHONE
static int vr_active(void){return 0;}
#endif
static float vr_canvas_a(void){
    if(vr_active())return 0; /* VR quality uses exact per-eye dimensions. */
    if(vr_canvas_aspect<0){
        const char *c=getenv("TOTK_VR_CONTROL"),*e=getenv("TOTK_VR_CANVAS");
        float a=c&&*c?(e&&*e?(float)atof(e):1.12f):0;
        vr_canvas_aspect=a>=0.5f&&a<=1.78f?a:0;
    }
    return vr_canvas_aspect;
}
/* The 16:9 size's pixels in the headset's shape (user 2026-10-31: "1616×1440 ... 不能按 2k渲染吗"; before, the height was
   kept and the width cut, 2560x1440 -> 1616x1440, a third of the pixels gone): 2560x1440 -> 2048x1816, 1920x1080 ->
   1536x1360, 1280x720 -> 1024x904; no taller than the swapchain (3840x2160 -> 2432x2160, as before), width in 16s. */
static uint64_t vr_shape_cap(uint64_t s,unsigned cap_w,unsigned cap_h){
    float a=vr_canvas_a();if(!(a>0))return s;
    double px=(double)(unsigned)s*(double)(unsigned)(s>>32);
    unsigned h=((unsigned)sqrt(px/a)+4)&~7u;
    if(cap_h&&h>cap_h)h=cap_h;
    unsigned w=((unsigned)(h*a+0.5f)+15)&~15u;
    if(cap_w&&w>cap_w)w=cap_w&~15u;
    return pack_size(w,h);
}
static uint64_t vr_shape(uint64_t s){return vr_shape_cap(s,output_w,output_h);}
/* zeldaVR, VR only (user 2026-10-31: "vr 4k + metalfx8k 和 原生8k", then "改成 6k": native 8K, 6096x5440 with 6.6 GB more
   texture heap, stopped before the title, the game's own allocation failing at main+0x8ac158): native 4K, 4K + MetalFX and
   native 6K keep their pixels in the headset's shape (3840x2160 -> 3056x2720, 5760x3240 -> 4576x4080), taller than the usual 3840x2160
   swapchain: in VR the swapchain and the heaps are sized for the largest of them at the start (vr_big_start), so all are
   picked while playing; without that size's heap overlay a pick that does not fit waits for a start that has it. 4K + MetalFX: the
   headset's eyes take the 3056x2720 picture MetalFX made twice as large (nvn_mtl.m vr_picture), 6112x5440. */
static int vr_big_mode(int mode,unsigned *w,unsigned *h){
    if(mode==OUTPUT_MODE_NATIVE_4K||mode==OUTPUT_MODE_NATIVE_4K_METALFX){*w=3840;*h=2160;return 1;}
    if(mode==OUTPUT_MODE_NATIVE_6K){*w=5760;*h=3240;return 1;}
    return 0;
}
static int vr_big_fits(int mode){   /* the running swapchain holds this mode's headset-shaped canvas */
    unsigned w,h;if(!vr_big_mode(mode,&w,&h))return 0;
    uint64_t s=vr_shape_cap(pack_size(w,h),0,0);return (unsigned)s<=output_w&&(unsigned)(s>>32)<=output_h;
}
/* Back to the 16:9 size it came from: the modes' sizes and the swapchain's are matched exactly, anything else by its pixels. */
static uint64_t std_shape(uint64_t s){
    if(!(vr_canvas_a()>0))return s;
    const uint64_t known[]={pack_size(1280,720),pack_size(1920,1080),pack_size(2560,1440),pack_size(3840,2160),pack_size(5760,3240),pack_size(output_w,output_h)};
    for(unsigned i=0;i<sizeof known/sizeof *known;i++)if(known[i]&&vr_shape(known[i])==s)return known[i];
    unsigned h=(unsigned)(sqrt((double)(unsigned)s*(double)(unsigned)(s>>32)*9.0/16.0)+0.5);return pack_size(h*16/9,h);
}
uint64_t runtime_output_size_current(void){
    uint8_t *page=atomic_load(&output_page);
    return page?atomic_load((_Atomic uint64_t *)(void *)(page+32)):pack_size(1920,1080);
}
/* The swapchain and the heaps in VR: the largest headset-shaped size this app carries a heap overlay for (6K, else 4K;
   user 2026-10-31: "原生 6k 为什么 要重启？ 不要重启！"), so every resolution, 6K included, is picked while playing. The
   heaps are reserved for it from the start whichever is picked (as a 4K app's are for 4K). The start's canvas is the
   saved one (picture_mode_restore, then the settings panel). */
static int vr_big_start(unsigned *w,unsigned *h){
    static const int modes[]={OUTPUT_MODE_NATIVE_6K,OUTPUT_MODE_NATIVE_4K};
    if(!(vr_canvas_a()>0))return 0;
    const char *dir=getenv("TOTK_ROMFS_PATCHES_OUT");char path[4096];
    for(unsigned i=0;i<sizeof modes/sizeof *modes;i++){
        unsigned fw,fh;vr_big_mode(modes[i],&fw,&fh);
        uint64_t s=vr_shape_cap(pack_size(fw,fh),0,0);unsigned sw=(unsigned)s,sh=(unsigned)(s>>32);
        snprintf(path,sizeof path,"%s/%ux%u-scene-%ux%u/manifest.tsv",dir?dir:"extracted/v143/romfs-patches-out",sw,sh,sw,sh);
        if(access(path,R_OK))continue;
        *w=sw;*h=sh;fprintf(stderr,"[totk] VR: swapchain %ux%u, as large as native %s in the headset's shape\n",sw,sh,i?"4K":"6K");
        return 1;
    }
    fprintf(stderr,"[totk] VR: this app has no headset-shaped 4K / 6K heap overlay (build_app.py VR_BIG_SIZES); the usual sizes\n");
    return 0;
}
int runtime_output_res(unsigned *w,unsigned *h){
    static int decided,on;static unsigned cw,ch;
    if(!decided){
        const char *e=getenv("TOTK_OUTPUT_RES");
        /* iOS shows 720p only (the user, 2026-10-18): no larger swapchain, heaps or output there, whatever
           TOTK_OUTPUT_RES says. The iOS export runs these patches on the Mac to make the phone's embedded code
           (TOTK_EXPORT_IOS), so it follows the same rule: before, auto put the 2560x1440 swapchain into the phone's code,
           and the phone, which carries the heap overlay in its bundle, reserved 88 MiB more for a picture it never shows. */
        if(TARGET_OS_IPHONE||getenv("TOTK_EXPORT_IOS")){
            on=0;
            if(e&&*e&&strcmp(e,"0")&&strcmp(e,"auto"))fprintf(stderr,"[totk] TOTK_OUTPUT_RES=%s ignored: iOS shows 720p only\n",e);
        }else if(vr_active()){
            on=1;cw=4096;ch=4416;output_full_at_start=0;
        }else if(e&&!strcmp(e,"0"))on=0;
        else if((!e||!*e||!strcmp(e,"auto"))&&vr_big_start(&cw,&ch)){
            on=1;output_full_at_start=0;   /* zeldaVR: room for every resolution; the start's canvas is the saved one */
        }else if(!e||!*e||!strcmp(e,"auto")){
            /* The largest canvas whose heap overlay the app carries (romfs.c loads <canvas>-scene-<canvas>, the scene
               following the canvas): 4K when the build made it (build_app.py native 4K: on by default on a Mac with
               16 GB or more, user 2026-10-30), else 2K. The start stays 1080p; only the swapchain and the heaps are
               sized for the largest. A 4K scene's heaps make the game create one 1,490 MiB memory pool at the start
               (+1,380 MiB for the application pool), which the graphics layer takes since it has no size limit on a
               pool's zero-copy buffer (nvn_mtl.m pool_range_check, 2026-10-30). */
            const char *dir=getenv("TOTK_ROMFS_PATCHES_OUT");char path[4096];
            static const unsigned sizes[][4]={{5120,2880,5120,2880},{3840,2160,3840,2160},{2560,1440,2560,1440}};   /* canvas, scene */
            for(unsigned i=0;i<sizeof sizes/sizeof *sizes&&!on;i++){
                snprintf(path,sizeof path,"%s/%ux%u-scene-%ux%u/manifest.tsv",dir?dir:"extracted/v143/romfs-patches-out",sizes[i][0],sizes[i][1],sizes[i][2],sizes[i][3]);
                FILE *f=fopen(path,"r");if(f){fclose(f);on=1;cw=sizes[i][0];ch=sizes[i][1];}
            }
            output_full_at_start=0;
        }else{
            char tail;
            if(sscanf(e,"%ux%u%c",&cw,&ch,&tail)!=2||cw<640||ch<360||cw>7680||ch>4320)runtime_fail("TOTK_OUTPUT_RES must be auto, 0 or widthxheight");
            on=1;output_full_at_start=1;
        }
        decided=1;
    }
    if(on){*w=cw;*h=ch;}return on;
}
/* The canvas-sized and the scene-sized render targets share DynamicTextureAllocatorMem (113664 KiB at a 1080p canvas
 * and the 1600x900 scene). Each grows the heap by its own pixel ratio above the shipped size, a bigger scene with a 1.6
 * margin: measured 2026-10-19, a 3440x1440 canvas with a 2580x1080 scene crashed in the allocator (main+0x1c2d20) at
 * +258 MiB and loaded at +400 MiB. That was before render_res_publish divided out the game's own canvas scaling (the
 * scene was then about 4620x1935), so for the scene asked for the heaps are now larger than needed: safe, not tight.
 * The HeapDef overlay (scripts/make_output_res_overlay.py --scene) uses the same formula. Mac only: iOS keeps the
 * game's own heaps. */
static int render_res(unsigned *w,unsigned *h);
int runtime_render_res(unsigned *w,unsigned *h){
#if TARGET_OS_IPHONE
    (void)w;(void)h;return 0;
#else
    return !getenv("TOTK_EXPORT_IOS")&&render_res(w,h);
#endif
}
uint64_t runtime_output_res_extra_bytes(void){
    unsigned w,h,sw,sh;double grow=0;
    if(runtime_output_res(&w,&h)&&(double)w*h>1920.0*1080.0)grow+=(double)w*h/(1920.0*1080.0)-1;
    int scene=runtime_render_res(&sw,&sh)&&(double)sw*sh>1600.0*900.0;
    if(scene)grow+=((double)sw*sh/(1600.0*900.0)-1);
    if(grow<=0)return 0;
    if(scene)grow*=1.6;
    uint64_t kib=(uint64_t)ceil(113664.0*(1+grow)/1024.0)*1024-113664;return kib<<10;
}
/* The settings panel's picture mode is kept between runs in the file TOTK_PICTURE_MODE names (the launcher: <data>/picture-mode.txt;
   unset: nothing is kept, as before). It holds one word: off, metalfx, native-2k, native-2k-metalfx, native-4k, native-5k, 720p or 720p-metalfx. The next start with
   automatic output sizes begins in that mode; a size larger than this app's swapchain falls back to the largest one it
   has (native-4k on a 2K app starts at native 2K). An explicit TOTK_OUTPUT_RES=<w>x<h> starts as it says. */
static void picture_mode_save(uint64_t size){
    const char *path=getenv("TOTK_PICTURE_MODE");if(!path||!*path)return;
    size=std_shape(size);   /* zeldaVR: the headset-shaped canvas is named by its 16:9 size */
    extern int mfxup_output_enabled(void);
    const char *mode=mfxup_output_enabled()?(size==pack_size(2560,1440)?"native-2k-metalfx":size==pack_size(3840,2160)?"native-4k-metalfx":size==pack_size(1280,720)?"720p-metalfx":"metalfx"):
                     size==pack_size(5760,3240)?"native-6k":size==pack_size(5120,2880)?"native-5k":size==pack_size(2560,1440)?"native-2k":
                     size==pack_size(3840,2160)?"native-4k":size==pack_size(1280,720)?"720p":"off";
    FILE *f=fopen(path,"w");
    if(!f||fprintf(f,"%s\n",mode)<0||fclose(f)){fprintf(stderr,"[totk] picture mode: cannot write %s\n",path);return;}
    fprintf(stderr,"[totk] picture mode %s kept for the next start\n",mode);
}
static void picture_mode_restore(uint8_t *page){
    const char *path=getenv("TOTK_PICTURE_MODE");if(!path||!*path||output_full_at_start)return;
    char mode[32]="";FILE *f=fopen(path,"r");if(!f)return;
    if(!fgets(mode,sizeof mode,f))mode[0]=0;
    fclose(f);mode[strcspn(mode,"\r\n")]=0;
    unsigned w=1920,h=1080;
    if(!strcmp(mode,"native-4k-metalfx")){extern void dm_gfx_set_flag(const char *,int);dm_gfx_set_flag("mfx_output",1);dm_gfx_set_flag("mfx_upscale",1);strcpy(mode,"native-4k");}
    if(!strcmp(mode,"native-6k")&&!vr_big_fits(OUTPUT_MODE_NATIVE_6K))strcpy(mode,"native-4k");   /* zeldaVR: a swapchain too small for 6K starts in 4K */
    if(!strcmp(mode,"native-6k")){w=5760;h=3240;}
    else if(!strcmp(mode,"native-5k")&&output_w>=5120&&output_h>=2880){w=5120;h=2880;}
    else if((!strcmp(mode,"native-5k")||!strcmp(mode,"native-4k"))&&output_w>=3840&&output_h>=2160){w=3840;h=2160;}
    else if((!strcmp(mode,"native-5k")||!strcmp(mode,"native-4k")||!strcmp(mode,"native-2k"))&&output_w>=2560&&output_h>=1440){w=2560;h=1440;}
    else if(!strcmp(mode,"native-2k-metalfx")&&output_w>=2560&&output_h>=1440){extern void dm_gfx_set_flag(const char *,int);
        w=2560;h=1440;dm_gfx_set_flag("mfx_output",1);dm_gfx_set_flag("mfx_upscale",1);}
    else if(!strcmp(mode,"metalfx")){extern void dm_gfx_set_flag(const char *,int);dm_gfx_set_flag("mfx_output",1);dm_gfx_set_flag("mfx_upscale",1);}
    else if(!strcmp(mode,"720p")){w=1280;h=720;}
    else if(!strcmp(mode,"720p-metalfx")){extern void dm_gfx_set_flag(const char *,int);w=1280;h=720;dm_gfx_set_flag("mfx_output",1);dm_gfx_set_flag("mfx_upscale",1);}
    else if(strcmp(mode,"off")){fprintf(stderr,"[totk] picture mode: %s holds \"%s\", not off, metalfx, native-2k, native-2k-metalfx, native-4k, native-4k-metalfx, native-5k, native-6k, 720p or 720p-metalfx; starting off\n",path,mode);return;}
    atomic_store((_Atomic uint64_t *)(void *)(page+32),pack_size(w,h));
    fprintf(stderr,"[totk] picture mode: starts in %s (kept from the last run, %s)\n",mode,path);
}
static void patch_output_res(Module *m){
    unsigned w,h;if(!runtime_output_res(&w,&h))return;
    uint64_t data=(m->file_size+0x3fff)&~0x3fffULL,stub=((m->text_end+15)&~15ULL)+16;
    if(data+0x4000!=m->size||stub+28>((m->text_end+0x3fff)&~0x3fffULL))runtime_fail("1.4.3 output patch layout");
    const Patch guards[]={
        {0xbdb578,0xd280f008,0},{0xbdb584,0xf2c08708,0},
        {0x3dcb28,0xd360fc64,0},{0x3dcb2c,0xf9028663,0},{0x3dcb30,0xd001ac08,0}};
    for(unsigned i=0;i<sizeof guards/sizeof *guards;i++){
#if !TARGET_OS_IPHONE
        uint32_t word;memcpy(&word,m->base+guards[i].off,4);
        if(word!=guards[i].original)runtime_fail("1.4.3 output original instruction mismatch");
#endif
    }
#if !TARGET_OS_IPHONE
    for(unsigned i=0;i<28;i++)if(m->base[stub+i])runtime_fail("1.4.3 output veneer padding not empty");
#endif
    /* x9 is dead at this publication point; the next original use initializes
     * it from the active display selector. x19 remains the display config. */
    uint32_t veneer[]={adrp(9,(uintptr_t)m->base+stub,(uintptr_t)m->base+data),
        0xf9001533, /* str x19,[x9,#40]: live display config */
        0xf9401123, /* ldr x3,[x9,#32]: requested output */
        0xf9000a63, /* str x3,[x19,#0x10]: display 0 canvas */
        0xd360fc64, /* lsr x4,x3,#32: crop height */
        0xf9028663, /* str x3,[x19,#0x508]: original active output write */
        branch(stub+24,0x3dcb30)};
 #if TARGET_OS_IPHONE
    if(memcmp(m->base+stub,veneer,sizeof veneer))runtime_fail("1.4.3 embedded output veneer mismatch");
 #else
    memcpy(m->base+stub,veneer,sizeof veneer);
 #endif
    uint32_t words[]={0xd2800008|(w<<5),0xf2c00008|(h<<5),branch(0x3dcb28,stub)};
 #if TARGET_OS_IPHONE
    for(unsigned i=0;i<3;i++){uint32_t got;memcpy(&got,m->base+guards[i].off,4);if(got!=words[i])runtime_fail("1.4.3 embedded output patch mismatch");}
 #else
    for(unsigned i=0;i<3;i++)memcpy(m->base+guards[i].off,words+i,4);
 #endif
    uint8_t *page=m->base+data;output_w=w;output_h=h;
    atomic_store((_Atomic uint64_t *)(void *)(page+32),output_full_at_start?pack_size(w,h):pack_size(1920,1080));
    atomic_store((_Atomic uint64_t *)(void *)(page+40),0);
    if(vr_active()&&!getenv("TOTK_EXPORT_IOS")){
        extern void vr_resolution(int *,int *);int eye_w=0,eye_h=0;vr_resolution(&eye_w,&eye_h);
        atomic_store((_Atomic uint64_t *)(void *)(page+32),pack_size((unsigned)eye_w,(unsigned)eye_h));
        fprintf(stderr,"[vr] TotK starts with exact %dx%d eye targets; fixed stream 2048x2208\n",eye_w,eye_h);
    }else picture_mode_restore(page);
    if(vr_canvas_a()>0){   /* zeldaVR: the headset's shape at the height picked */
        uint64_t was=atomic_load((_Atomic uint64_t *)(void *)(page+32)),now=vr_shape(was);
        atomic_store((_Atomic uint64_t *)(void *)(page+32),now);
        fprintf(stderr,"[totk] VR canvas: %ux%u instead of %ux%u (the headset's shape, %.2f : 1; TOTK_VR_CANVAS=0 keeps 16:9)\n",
                (unsigned)now,(unsigned)(now>>32),(unsigned)was,(unsigned)(was>>32),vr_canvas_a());
    }
    atomic_store(&output_page,page);   /* Publish initialized state to the HUD on the main thread. */
    uint64_t start=atomic_load((_Atomic uint64_t *)(void *)(page+32));
    fprintf(stderr,"[totk] 1.4.3 output: %ux%u swapchain; starts at %ux%u, the settings panel picks the resolution up to the swapchain; display-config veneer +%#llx\n",w,h,(unsigned)start,(unsigned)(start>>32),(unsigned long long)stub);
}
/* Render resolution (TOTK_RENDER_RES=<w>x<h>, e.g. 960x540) - the 1.4.3 port of game_patches.c's patch.
   1.4.3 keeps the same graphics-parameter entry (+0x30 of the mode entry: handheld 1280x720, docked 1600x900) and the
   same renderer fields (+0xcc4 width, +0xcc8 height), but converts both through s0 and keeps the output size in w9:
     main+0x585e48  scvtf s0, w11             -> adrp x3, <data page>
     main+0x585e50  str   s0, [x19, #0xcc4]   -> ldp  s0, s1, [x3, #16]
     main+0x585e54  scvtf s0, w10             -> str  s0, [x19, #0xcc4]
     main+0x585e60  str   s0, [x19, #0xcc8]   -> str  s1, [x19, #0xcc8]
   Every path that picks the render size converges through main+0x585e48 (the branch at 0x585f50 only reloads w11/w10
   and jumps back), so the four sites cover all of them. x3 is scratch and untouched by this function; x9 must NOT be
   used here because w9 still holds the output size for main+0x585e64. The size comes from the loader's data page +16
   (the page patch_realtime reserves); TOTK_RENDER_RES unset = the game's own size and nothing is patched.
   iOS cannot write code at run time: the iOS export (TOTK_EXPORT_IOS, runtime/loader.c) runs this patch on the Mac, so
   the phone's embedded code carries it only when TOTK_RENDER_RES was set for the export. The phone checks the embedded
   words and writes the size on the data page itself: its own TOTK_RENDER_RES, or the game's handheld 1280x720 when unset
   (the phone always reports handheld mode). Embedded code without the patch keeps the game's own size. */
/* The game scales its scene with the canvas: the size it renders is the mode entry's (or the patched) size times the
   output width over 1920, on both axes (measured: 2560x1440 asked on a 2560x1440 canvas rendered 3408x1920, 3440x1440 on
   the 3440x1440 canvas 6160x2576). The data page therefore holds the asked size divided by that factor, rewritten when
   the settings panel changes the canvas, so the scene is the size asked for on every canvas. Below 1920 the same factor
   holds (2026-10-25: 1920x1080 on the 720p canvas rendered 1280x720). The game reads this size only at the start: after
   that it scales the size it read there with the canvas (a 1600x900 written on the 1080p canvas was never taken). */
static uint8_t *render_page;static unsigned render_w,render_h;static int render_canvas;
static void render_res_publish(uint64_t canvas){
    if(!render_page)return;
    const unsigned canvas_w=(unsigned)canvas,canvas_h=(unsigned)(canvas>>32);
    float k=canvas_w/1920.0f,v[2]={render_w/k,render_h/k};
    /* the game's scaling makes it the canvas: its height over its width (16:9, or the VR canvas's shape) */
    if(render_canvas){float r=canvas_w&&canvas_h?(float)canvas_h/canvas_w:1080.0f/1920;v[0]=1920;v[1]=1920*r;render_w=canvas_w;render_h=(unsigned)(1920*r*k+0.5f);}
    memcpy(render_page+16,v,sizeof v);
    fprintf(stderr,"[totk] 1.4.3 render resolution: %ux%u on a %u-wide canvas (game size %.0fx%.0f, scaled by %.4f)\n",render_w,render_h,canvas_w,v[0],v[1],k);
}
/* On by default on both (user 2026-10-23: "两边默认都打开"; before, only with TOTK_RENDER_RES, as PR #19 brought it):
   - Mac: the scene is the canvas's size, 1920x1080 on the 1080p canvas (the docked mode's own is 1600x900) and 2560x1440
     on the 2K one (the settings panel), only ever up (the Mac never lowers resolution). The heaps grow with it (romfs.c picks the
     <canvas>-scene-<size> HeapDef overlay build_app.py makes; the application pool follows).
   - iPhone / iPad: 960x540 under the 1280x720 canvas, the game's own upscale (FSR 1 EASU, a3a570eb / ac58fc18) drawing it
     there (user 2026-10-23: no MetalFX on the phone, as Breath of the Wild): the phone's GPU and memory are at their
     limit; the same size as on the old port (2026-10-16, GPU 24.3 -> 17.3 ms, measured with MetalFX upscaling).
   TOTK_RENDER_RES=<w>x<h> sets a size, =0 the game's own (no patch on the Mac; 1280x720 on the phone). */
static int render_res(unsigned *w,unsigned *h){
    const char *e=getenv("TOTK_RENDER_RES");
    if(e&&!strcmp(e,"0"))return 0;
    if(!e||!*e){
#if TARGET_OS_IPHONE
        *w=960;*h=540;
#else
        render_canvas=1;unsigned ow,oh;if(runtime_output_res(&ow,&oh)){*w=ow;*h=oh;}else{*w=1920;*h=1080;}   /* the largest scene, for the heaps */
#endif
        return 1;
    }
    char rest;
    if(sscanf(e,"%ux%u%c",w,h,&rest)!=2||*w<64||*h<64||*w>5120||*h>4096)runtime_fail("TOTK_RENDER_RES must be <width>x<height>");
    return 1;
}
static void patch_render_res(Module *m){
    unsigned w=1280,h=720;int set=render_res(&w,&h);
#if !TARGET_OS_IPHONE
    if(!set)return;   /* Mac (and the iOS export): the game's own size unless asked */
#endif
    uint64_t data=(m->file_size+0x3fff)&~0x3fffULL;
    if(data+24>m->size)runtime_fail("1.4.3 render-resolution patch layout");
    const Patch sites[]={
        {0x585e48,0x1e220160,0},
        {0x585e50,0xbd0cc660,0x2d420460},
        {0x585e54,0x1e220140,0xbd0cc660},
        {0x585e60,0xbd0cca60,0xbd0cca61},
    };
    uint32_t want[4]={adrp(3,(uintptr_t)m->base+sites[0].off,(uintptr_t)m->base+data),
                      sites[1].replacement,sites[2].replacement,sites[3].replacement};
#if TARGET_OS_IPHONE
    uint32_t first;memcpy(&first,m->base+sites[0].off,4);
    if(first==sites[0].original){fprintf(stderr,"[totk] 1.4.3 render resolution: the game's own (not patched at build)\n");return;}
    for(unsigned i=0;i<sizeof sites/sizeof *sites;i++){
        uint32_t got;memcpy(&got,m->base+sites[i].off,4);
        if(got!=want[i])runtime_fail("1.4.3 embedded render-resolution patch mismatch");
    }
#else
    for(unsigned i=0;i<sizeof sites/sizeof *sites;i++){
        uint32_t word;memcpy(&word,m->base+sites[i].off,4);
        if(word!=sites[i].original){
            fprintf(stderr,"[totk] render resolution patch main+%#llx: found %08x, expected %08x\n",
                    (unsigned long long)sites[i].off,word,sites[i].original);
            runtime_fail("render resolution patch: main does not match 1.4.3");
        }
    }
    for(unsigned i=0;i<sizeof sites/sizeof *sites;i++)memcpy(m->base+sites[i].off,&want[i],4);
#endif
    render_page=m->base+data;render_w=w;render_h=h;
    render_res_publish(output_w?runtime_output_size_current():pack_size(1920,1080));
    fprintf(stderr,"[totk] 1.4.3 render resolution patch: scene render size %ux%u%s (4 instructions at main+0x585e48, value on the data page)\n",
            w,h,set?"":" (the game's handheld size; TOTK_RENDER_RES not set)");
}
/* No dynamic resolution by default (zeldaVR, user 2026-11-01: "默认关闭 动态分辨率": the Zonai device menu flickered as the
   resolution stepped between 3056x2720 and 2288x2040); TOTK_FIXED_RES=0 lets the game step again. */
/* TOTK_FIXED_RES=1: no dynamic resolution. The game's controller (main+0x584300, called from the renderer's per-frame update at
   main+0x58196c) keeps a window of 24 frame-time samples, predicts the next frame's cost and steps through resolution tiers
   (the scene came out at 3024x1888, 2712x1696, 2416x1512 on a 3024x1890 canvas: 100 %, 90 %, 80 %). It returns the tier's scale
   as a float in the low word of x0 (bit 33 set: the tier did not change); the caller stores cc4 * scale / cc0 as the renderer's
   scale at +0xccc. Every path leaves through one epilogue, so the single instruction that moves the scale into the result
   (main+0x5848a8, mov w9, w23) is replaced by a move of 1.0f: the controller still runs and picks tiers, but every tier is 100 %.
   The size the render-resolution patch above chooses (TOTK_RENDER_RES) stays; this only stops the dynamic part. */
static void patch_fixed_res(Module *m){
    const char *e=getenv("TOTK_FIXED_RES");
    if(e&&!strcmp(e,"0"))return;   /* default: fixed (zeldaVR) */
#if TARGET_OS_IPHONE
    if(e&&!strcmp(e,"1"))fprintf(stderr,"[totk] TOTK_FIXED_RES: not available in the embedded build\n");(void)m;
#else
    const uint64_t site=0x5848a8;const uint32_t original=0x2a1703e9,replacement=0x52a7f009;   /* mov w9, w23 -> mov w9, #0x3f800000 */
    uint32_t word;memcpy(&word,m->base+site,4);
    if(word!=original){
        fprintf(stderr,"[totk] fixed resolution patch main+%#llx: found %08x, expected %08x\n",(unsigned long long)site,word,original);
        runtime_fail("fixed resolution patch: main does not match 1.4.3");
    }
    memcpy(m->base+site,&replacement,4);
    fprintf(stderr,"[totk] 1.4.3 fixed resolution (TOTK_FIXED_RES): the dynamic resolution controller's scale is held at 100 %% (1 instruction at main+%#llx)\n",(unsigned long long)site);
#endif
}
/* The game's canvas: any size up to the swapchain's. The scene size goes first (the game rebuilds when told). */
static void output_size_set(uint64_t want){
    _Atomic uint64_t *size=(_Atomic uint64_t *)(void *)(output_page+32);
    want=vr_shape(want);   /* zeldaVR: the headset's shape */
    if(atomic_load(size)==want)return;
    atomic_store(size,want);render_res_publish(want);
    uint64_t config=atomic_load((_Atomic uint64_t *)(void *)(output_page+40));
    if(config){__atomic_fetch_or((uint32_t *)(uintptr_t)(config+8),1u<<21,__ATOMIC_SEQ_CST);runtime_oe_push_message(30);}
    fprintf(stderr,"[totk] 1.4.3 output requested %ux%u (display config %s)\n",(unsigned)want,(unsigned)(want>>32),config?"ready":"not published yet");
}
int runtime_vr_render_size_set(unsigned w,unsigned h){
    if(!vr_active()||getenv("TOTK_EXPORT_IOS")||!atomic_load(&output_page)||w>output_w||h>output_h)return 0;
    output_size_set(pack_size(w,h));return 1;
}

/* The settings panel's resolution (runtime.h OUTPUT_MODE_*), kept for the next start (picture_mode_save). Native is
   the swapchain's size (2K); with MetalFX the output stage then scales the 2K canvas up to the window (a 4K screen). */
int runtime_output_native(int mode,unsigned *w,unsigned *h){
    if(!output_page)return 0;
    if(vr_canvas_a()>0&&vr_big_mode(mode,w,h))return 1;   /* zeldaVR: offered in VR, maybe from the next start (vr_big_fits) */
    if(mode==OUTPUT_MODE_NATIVE_5K){if(output_w<5120||output_h<2880)return 0;*w=5120;*h=2880;return 1;}
    if(mode==OUTPUT_MODE_NATIVE_4K){if(output_w<3840||output_h<2160)return 0;*w=3840;*h=2160;return 1;}   /* a native 4K app */
    if(mode!=OUTPUT_MODE_NATIVE&&mode!=OUTPUT_MODE_NATIVE_METALFX)return 0;
    if(output_w>2560){*w=2560;*h=1440;return 1;}   /* a 4K app's 2K stays 2560x1440 */
    *w=output_w;*h=output_h;return 1;
}
int runtime_output_modes(void){
    if(vr_active()&&!getenv("TOTK_EXPORT_IOS"))return 1<<OUTPUT_MODE_NATIVE;
    if(!output_page)return 0;
    return 1<<OUTPUT_MODE_OFF|1<<OUTPUT_MODE_METALFX|1<<OUTPUT_MODE_720P|1<<OUTPUT_MODE_720P_METALFX|1<<OUTPUT_MODE_NATIVE|1<<OUTPUT_MODE_NATIVE_METALFX|
           (output_w>=3840&&output_h>=2160?1<<OUTPUT_MODE_NATIVE_4K:0)|
           (output_w>=5120&&output_h>=2880?1<<OUTPUT_MODE_NATIVE_5K:0)|
           (vr_canvas_a()>0?1<<OUTPUT_MODE_NATIVE_4K|1<<OUTPUT_MODE_NATIVE_4K_METALFX|1<<OUTPUT_MODE_NATIVE_6K:0);
}
int runtime_output_mode(void){
    if(vr_active()&&!getenv("TOTK_EXPORT_IOS"))return OUTPUT_MODE_NATIVE;
    extern int mfxup_output_enabled(void);
    if(!output_page)return OUTPUT_MODE_OFF;
    uint64_t now=std_shape(atomic_load((_Atomic uint64_t *)(void *)(output_page+32)));   /* zeldaVR: by its 16:9 size */
    if(now==pack_size(1280,720))return mfxup_output_enabled()?OUTPUT_MODE_720P_METALFX:OUTPUT_MODE_720P;
    if(now==pack_size(5760,3240))return OUTPUT_MODE_NATIVE_6K;
    if(now==pack_size(3840,2160)&&vr_canvas_a()>0&&mfxup_output_enabled())return OUTPUT_MODE_NATIVE_4K_METALFX;
    if(now==pack_size(5120,2880)&&!mfxup_output_enabled())return OUTPUT_MODE_NATIVE_5K;
    if(now==pack_size(3840,2160)&&!mfxup_output_enabled())return OUTPUT_MODE_NATIVE_4K;
    if((unsigned)now>1920)return mfxup_output_enabled()?OUTPUT_MODE_NATIVE_METALFX:OUTPUT_MODE_NATIVE;
    return mfxup_output_enabled()?OUTPUT_MODE_METALFX:OUTPUT_MODE_OFF;
}
int runtime_output_mode_set(int mode){
    extern void dm_gfx_set_flag(const char *,int);
    if(!output_page||mode<OUTPUT_MODE_OFF||mode>OUTPUT_MODE_LAST||!(runtime_output_modes()&1<<mode))return 0;
    unsigned w=1920,h=1080;
    if(vr_canvas_a()>0&&vr_big_mode(mode,&w,&h)&&!vr_big_fits(mode)){   /* zeldaVR: the swapchain is too small; the settings keep it for the next start */
        fprintf(stderr,"[totk] picture: %s %s from the next start (the swapchain %ux%u is smaller than its headset-shaped canvas)\n",
                mode==OUTPUT_MODE_NATIVE_6K?"native 6K":mode==OUTPUT_MODE_NATIVE_4K?"native 4K":"4K + MetalFX",mode==OUTPUT_MODE_NATIVE_6K?"only":"in full",output_w,output_h);
        if(mode==OUTPUT_MODE_NATIVE_6K)return 1;   /* 4K: until then as large as the swapchain allows, as before */
    }
    if(mode==OUTPUT_MODE_720P||mode==OUTPUT_MODE_720P_METALFX){w=1280;h=720;}else if(mode==OUTPUT_MODE_NATIVE||mode==OUTPUT_MODE_NATIVE_METALFX||mode==OUTPUT_MODE_NATIVE_4K||mode==OUTPUT_MODE_NATIVE_5K||mode==OUTPUT_MODE_NATIVE_4K_METALFX||mode==OUTPUT_MODE_NATIVE_6K)runtime_output_native(mode,&w,&h);
    output_size_set(pack_size(w,h));
    int mfx=mode==OUTPUT_MODE_METALFX||mode==OUTPUT_MODE_NATIVE_METALFX||mode==OUTPUT_MODE_NATIVE_4K_METALFX||mode==OUTPUT_MODE_720P_METALFX;
    dm_gfx_set_flag("mfx_output",mfx);dm_gfx_set_flag("mfx_upscale",mfx);
    fprintf(stderr,"[totk] picture: %s\n",mode==OUTPUT_MODE_NATIVE_6K?"native 6K":mode==OUTPUT_MODE_NATIVE_4K_METALFX?"native 4K, MetalFX to the headset":mode==OUTPUT_MODE_NATIVE_5K?"native 5K":mode==OUTPUT_MODE_NATIVE_4K?"native 4K":mode==OUTPUT_MODE_720P?"720p":mode==OUTPUT_MODE_720P_METALFX?"720p, MetalFX to the window":mode==OUTPUT_MODE_NATIVE_METALFX?"native 2K, MetalFX to the window":
            mode==OUTPUT_MODE_NATIVE?"native":mode==OUTPUT_MODE_METALFX?"MetalFX 2K":"off (1080p)");
    picture_mode_save(atomic_load((_Atomic uint64_t *)(void *)(output_page+32)));
    return 1;
}
