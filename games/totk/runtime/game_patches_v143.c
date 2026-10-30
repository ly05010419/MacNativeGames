/* 1.4.3 has a new compiler and frame-controller layout. Every adapted game-code
   site below is checked before any write; older versions use game_patches.c. */
#include "aspect_patch.h"
#include "game_patches.h"
#include "game_version.h"
#include "world_survey.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <math.h>
#include <TargetConditionals.h>
#if !TARGET_OS_IPHONE
#include <mach/mach.h>
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
enum{WATCH_SLOTS=512};
static struct{_Atomic uint64_t pc;_Atomic uint64_t lr;_Atomic uint32_t n;} watch_hits[WATCH_SLOTS];
static void watch_set(thread_act_t t,int on){
    arm_debug_state64_t d;mach_msg_type_number_t c=ARM_DEBUG_STATE64_COUNT;
    if(thread_get_state(t,ARM_DEBUG_STATE64,(thread_state_t)&d,&c)!=KERN_SUCCESS)return;
    /* WCR: enable, EL0, load, byte address select for the 4 bytes at watch_addr */
    d.__wvr[0]=on?(watch_addr&~7ULL):0;d.__wcr[0]=on?((0xfULL<<((watch_addr&7)+5))|(1u<<3)|(2u<<1)|1u):0;
    thread_set_state(t,ARM_DEBUG_STATE64,(thread_state_t)&d,ARM_DEBUG_STATE64_COUNT);
}
static void watch_all(int on){
    thread_act_array_t list;mach_msg_type_number_t n;thread_act_t self=mach_thread_self();
    if(task_threads(mach_task_self(),&list,&n)!=KERN_SUCCESS)return;
    for(unsigned i=0;i<n;i++){if(list[i]!=self){thread_suspend(list[i]);watch_set(list[i],on);thread_resume(list[i]);}mach_port_deallocate(mach_task_self(),list[i]);}
    vm_deallocate(mach_task_self(),(vm_address_t)list,n*sizeof *list);mach_port_deallocate(mach_task_self(),self);
}
int runtime_diag_watch_trap(void *context){
    ucontext_t *u=context;uint64_t pc=u->uc_mcontext->__ss.__pc,lr=u->uc_mcontext->__ss.__lr;
    unsigned ec=(unsigned)(u->uc_mcontext->__es.__esr>>26);if(!watch_addr||(ec!=0x34&&ec!=0x35))return 0;   /* watchpoint */
    for(unsigned i=(unsigned)(pc>>2)%WATCH_SLOTS,k=0;k<WATCH_SLOTS;k++,i=(i+1)%WATCH_SLOTS){
        uint64_t z=0;if(atomic_compare_exchange_strong(&watch_hits[i].pc,&z,pc)||z==pc){atomic_store(&watch_hits[i].lr,lr);atomic_fetch_add(&watch_hits[i].n,1);break;}
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
void runtime_diag_xpoke(unsigned off,float value){
    uint64_t x=time_object();float old;
    memcpy(&old,(const void *)(uintptr_t)(x+off),4);memcpy((void *)(uintptr_t)(x+off),&value,4);
    fprintf(stderr,"[totk] xpoke X+%#x: %g -> %g\n",off,old,value);
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
#endif
    { static const uint64_t sites[] = {0x628c74, 0xb43460, 0x1ff04f4, 0x253fbb0};   /* camera update and setup, viewport x2 */
      aspect_install(m, "1.4.3", sites, 4, 2, ((m->text_end + 15) & ~15ULL) + 48,   /* after the two veneers above */
                     ((m->text_end + 15) & ~15ULL) + 320,   /* Vert+ stubs (2 x 304 bytes): after the bounce veneer at +256, before the rewriter's code at +1024 */
                     (m->file_size + 0x3fff) & ~0x3fffULL); }
    patch_bench_cam(m);
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
uint64_t runtime_output_size_current(void){
    uint8_t *page=atomic_load(&output_page);
    return page?atomic_load((_Atomic uint64_t *)(void *)(page+32)):pack_size(1920,1080);
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
        }else if(e&&!strcmp(e,"0"))on=0;
        else if(!e||!*e||!strcmp(e,"auto")){
            /* The largest canvas whose heap overlay the app carries (romfs.c loads <canvas>-scene-<canvas>, the scene
               following the canvas): 2K. The start stays 1080p; only the swapchain and the heaps are sized for 2K.
               Not 4K (2026-10-25, user "添加 4k 支持"): its heaps make the game create one 1490 MiB memory pool at the
               start, which the graphics layer's 1 GiB zero-copy limit refuses (nvn_mtl.m POOL_BUFFER_MAX, set after two
               macOS kernel panics in IOGPUFamily); the game stopped at once. 4K needs that pool split first. */
            const char *dir=getenv("TOTK_ROMFS_PATCHES_OUT");char path[4096];
            static const unsigned sizes[][2]={{2560,1440}};
            for(unsigned i=0;i<sizeof sizes/sizeof *sizes&&!on;i++){
                snprintf(path,sizeof path,"%s/%ux%u-scene-%ux%u/manifest.tsv",dir?dir:"extracted/v143/romfs-patches-out",sizes[i][0],sizes[i][1],sizes[i][0],sizes[i][1]);
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
   unset: nothing is kept, as before). It holds one word: off, metalfx, native-2k, native-2k-metalfx, native-4k or 720p. The next start with
   automatic output sizes begins in that mode; a size larger than this app's swapchain falls back to the largest one it
   has (native-4k on a 2K app starts at native 2K). An explicit TOTK_OUTPUT_RES=<w>x<h> starts as it says. */
static void picture_mode_save(uint64_t size){
    const char *path=getenv("TOTK_PICTURE_MODE");if(!path||!*path)return;
    extern int mfxup_output_enabled(void);
    const char *mode=mfxup_output_enabled()?(size==pack_size(2560,1440)?"native-2k-metalfx":"metalfx"):size==pack_size(2560,1440)?"native-2k":
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
    if(!strcmp(mode,"native-4k")&&output_w>=3840&&output_h>=2160){w=3840;h=2160;}
    else if((!strcmp(mode,"native-4k")||!strcmp(mode,"native-2k"))&&output_w>=2560&&output_h>=1440){w=2560;h=1440;}
    else if(!strcmp(mode,"native-2k-metalfx")&&output_w>=2560&&output_h>=1440){extern void dm_gfx_set_flag(const char *,int);
        w=2560;h=1440;dm_gfx_set_flag("mfx_output",1);dm_gfx_set_flag("mfx_upscale",1);}
    else if(!strcmp(mode,"metalfx")){extern void dm_gfx_set_flag(const char *,int);dm_gfx_set_flag("mfx_output",1);dm_gfx_set_flag("mfx_upscale",1);}
    else if(!strcmp(mode,"720p")){w=1280;h=720;}
    else if(strcmp(mode,"off")){fprintf(stderr,"[totk] picture mode: %s holds \"%s\", not off, metalfx, native-2k, native-2k-metalfx, native-4k or 720p; starting off\n",path,mode);return;}
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
    picture_mode_restore(page);
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
static void render_res_publish(unsigned canvas_w){
    if(!render_page)return;
    float k=canvas_w/1920.0f,v[2]={render_w/k,render_h/k};
    if(render_canvas){v[0]=1920;v[1]=1080;render_w=(unsigned)(1920*k);render_h=(unsigned)(1080*k);}   /* the game's scaling makes it the canvas */
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
    if(sscanf(e,"%ux%u%c",w,h,&rest)!=2||*w<64||*h<64||*w>4096||*h>4096)runtime_fail("TOTK_RENDER_RES must be <width>x<height>");
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
    render_res_publish(output_w?(unsigned)runtime_output_size_current():1920);
    fprintf(stderr,"[totk] 1.4.3 render resolution patch: scene render size %ux%u%s (4 instructions at main+0x585e48, value on the data page)\n",
            w,h,set?"":" (the game's handheld size; TOTK_RENDER_RES not set)");
}
/* TOTK_FIXED_RES=1: no dynamic resolution. The game's controller (main+0x584300, called from the renderer's per-frame update at
   main+0x58196c) keeps a window of 24 frame-time samples, predicts the next frame's cost and steps through resolution tiers
   (the scene came out at 3024x1888, 2712x1696, 2416x1512 on a 3024x1890 canvas: 100 %, 90 %, 80 %). It returns the tier's scale
   as a float in the low word of x0 (bit 33 set: the tier did not change); the caller stores cc4 * scale / cc0 as the renderer's
   scale at +0xccc. Every path leaves through one epilogue, so the single instruction that moves the scale into the result
   (main+0x5848a8, mov w9, w23) is replaced by a move of 1.0f: the controller still runs and picks tiers, but every tier is 100 %.
   The size the render-resolution patch above chooses (TOTK_RENDER_RES) stays; this only stops the dynamic part. */
static void patch_fixed_res(Module *m){
    const char *e=getenv("TOTK_FIXED_RES");
    if(!e||!*e||!strcmp(e,"0"))return;
#if TARGET_OS_IPHONE
    fprintf(stderr,"[totk] TOTK_FIXED_RES: not available in the embedded build\n");(void)m;
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
    if(atomic_load(size)==want)return;
    atomic_store(size,want);render_res_publish((unsigned)want);
    uint64_t config=atomic_load((_Atomic uint64_t *)(void *)(output_page+40));
    if(config){__atomic_fetch_or((uint32_t *)(uintptr_t)(config+8),1u<<21,__ATOMIC_SEQ_CST);runtime_oe_push_message(30);}
    fprintf(stderr,"[totk] 1.4.3 output requested %ux%u (display config %s)\n",(unsigned)want,(unsigned)(want>>32),config?"ready":"not published yet");
}
/* The settings panel's resolution (runtime.h OUTPUT_MODE_*), kept for the next start (picture_mode_save). Native is
   the swapchain's size (2K); with MetalFX the output stage then scales the 2K canvas up to the window (a 4K screen). */
int runtime_output_native(int mode,unsigned *w,unsigned *h){
    if(!output_page||(mode!=OUTPUT_MODE_NATIVE&&mode!=OUTPUT_MODE_NATIVE_METALFX))return 0;
    *w=output_w;*h=output_h;return 1;
}
int runtime_output_modes(void){
    if(!output_page)return 0;
    return 1<<OUTPUT_MODE_OFF|1<<OUTPUT_MODE_METALFX|1<<OUTPUT_MODE_720P|1<<OUTPUT_MODE_NATIVE|1<<OUTPUT_MODE_NATIVE_METALFX;
}
int runtime_output_mode(void){
    extern int mfxup_output_enabled(void);
    if(!output_page)return OUTPUT_MODE_OFF;
    uint64_t now=atomic_load((_Atomic uint64_t *)(void *)(output_page+32));
    if(now==pack_size(1280,720))return OUTPUT_MODE_720P;
    if((unsigned)now>1920)return mfxup_output_enabled()?OUTPUT_MODE_NATIVE_METALFX:OUTPUT_MODE_NATIVE;
    return mfxup_output_enabled()?OUTPUT_MODE_METALFX:OUTPUT_MODE_OFF;
}
int runtime_output_mode_set(int mode){
    extern void dm_gfx_set_flag(const char *,int);
    if(!output_page||mode<OUTPUT_MODE_OFF||mode>OUTPUT_MODE_720P||!(runtime_output_modes()&1<<mode))return 0;
    unsigned w=1920,h=1080;
    if(mode==OUTPUT_MODE_720P){w=1280;h=720;}else if(mode==OUTPUT_MODE_NATIVE||mode==OUTPUT_MODE_NATIVE_METALFX)runtime_output_native(mode,&w,&h);
    output_size_set(pack_size(w,h));
    int mfx=mode==OUTPUT_MODE_METALFX||mode==OUTPUT_MODE_NATIVE_METALFX;
    dm_gfx_set_flag("mfx_output",mfx);dm_gfx_set_flag("mfx_upscale",mfx);
    fprintf(stderr,"[totk] picture: %s\n",mode==OUTPUT_MODE_720P?"720p":mode==OUTPUT_MODE_NATIVE_METALFX?"native 2K, MetalFX to the window":
            mode==OUTPUT_MODE_NATIVE?"native":mode==OUTPUT_MODE_METALFX?"MetalFX 2K":"off (1080p)");
    picture_mode_save(atomic_load((_Atomic uint64_t *)(void *)(output_page+32)));
    return 1;
}
