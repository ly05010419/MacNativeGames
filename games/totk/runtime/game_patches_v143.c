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
    if(dt<1.0/120.0)dt=1.0/120.0;if(dt>1.0/15.0)dt=1.0/15.0;
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
void runtime_game_patches(Module *m) {
    runtime_survey_init(m);
    if (strcmp(m->name, "main")) runtime_fail("1.4.3 game patches expect main");
    patch_realtime(m);
    patch_output_res(m);
    patch_render_res(m);
    patch_slowmo_bounce(m);
#if !TARGET_OS_IPHONE
    diag_main=m;   /* frmwatch / xpoke */
#endif
    { static const uint64_t sites[] = {0x628c74, 0xb43460, 0x1ff04f4, 0x253fbb0};   /* camera update and setup, viewport x2 */
      aspect_install(m, "1.4.3", sites, 4, ((m->text_end + 15) & ~15ULL) + 48,   /* after the two veneers above */
                     (m->file_size + 0x3fff) & ~0x3fffULL); }
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
/* Settings picture v2 (runtime/settings.h): the output resolution and the upscaler. The runtime keeps its own copies of
   the schema constants so this translation unit does not include the settings one; the values are part of the v2
   contract (TOTK_RES_* / TOTK_UPSCALE_*). */
#ifndef TOTK_RES_1080
#define TOTK_RES_1080 1080
#define TOTK_RES_1440 1440
#define TOTK_RES_2160 2160
#endif
#ifndef TOTK_UPSCALE_NATIVE
#define TOTK_UPSCALE_NATIVE 0
#define TOTK_UPSCALE_METALFX 1
#endif
static int output_full_at_start;
static unsigned output_w,output_h;
static _Atomic(uint8_t *) output_page;
static uint64_t pack_size(unsigned w,unsigned h){return (uint64_t)h<<32|w;}
uint64_t runtime_output_size_current(void){
    uint8_t *page=atomic_load(&output_page);
    return page?atomic_load((_Atomic uint64_t *)(void *)(page+32)):pack_size(1920,1080);
}
/* The largest picture this process can present: 4K only after a 3840x2160 start, 2K when the swapchain has at least
   2560x1440. The drawable, not the physical screen, is what counts (a 4K canvas on a smaller window is a downscale). */
int runtime_output_max_res(void){
    if(!output_page)return TOTK_RES_1080;
    if(output_w==3840&&output_h==2160)return TOTK_RES_2160;
    if(output_w>=2560&&output_h>=1440)return TOTK_RES_1440;
    return TOTK_RES_1080;
}
/* A MetalFX output start keeps the game at the 1080p canvas and lets the scaler reach the swapchain; only a native start
   draws the requested size itself. TOTK_MFX_OUTPUT is what the settings layer exports for the MetalFX upscaler. */
static int output_mfx_env(void){
    const char *e=getenv("TOTK_MFX_OUTPUT");
    return e&&*e&&strcmp(e,"0")!=0;
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
            const char *dir=getenv("TOTK_ROMFS_PATCHES_OUT");char path[4096];
            snprintf(path,sizeof path,"%s/2560x1440/manifest.tsv",dir?dir:"extracted/v143/romfs-patches-out");
            FILE *f=fopen(path,"r");if(f){fclose(f);on=1;cw=2560;ch=1440;}
            output_full_at_start=0;
        }else{
            char tail;
            if(sscanf(e,"%ux%u%c",&cw,&ch,&tail)!=2||cw<640||ch<360||cw>7680||ch>4320)runtime_fail("TOTK_OUTPUT_RES must be auto, 0 or widthxheight");
            on=1;output_full_at_start=output_mfx_env()?0:1;
        }
        decided=1;
    }
    if(on){*w=cw;*h=ch;}return on;
}
uint64_t runtime_output_res_extra_bytes(void){
    unsigned w,h;if(!runtime_output_res(&w,&h))return 0;
    double ratio=(double)w*h/(1920.0*1080.0);if(ratio<=1)return 0;
    uint64_t kib=(uint64_t)ceil(113664.0*ratio/1024.0)*1024-113664;return kib<<10;
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
    atomic_store(&output_page,page);   /* Publish initialized state to the HUD on the main thread. */
    fprintf(stderr,"[totk] 1.4.3 output: %ux%u swapchain; starts %s, key 3 cycles MetalFX / native / off; display-config veneer +%#llx\n",w,h,output_full_at_start?"at requested resolution":"at 1920x1080",(unsigned long long)stub);
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
static int render_res(unsigned *w,unsigned *h){
    const char *e=getenv("TOTK_RENDER_RES");
    if(!e||!*e||!strcmp(e,"0"))return 0;
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
    float v[2]={(float)w,(float)h};
    memcpy(m->base+data+16,v,sizeof v);
    fprintf(stderr,"[totk] 1.4.3 render resolution patch: scene render size %ux%u%s (4 instructions at main+0x585e48, value on the data page)\n",
            w,h,set?"":" (the game's handheld size; TOTK_RENDER_RES not set)");
}
/* Request a new canvas: store it, set the display config's pending bit and post the operation-mode notification. Without
   a published display config the size is taken when the game first lays out. Returns 1. */
static int output_request_canvas(unsigned cw,unsigned ch,int full){
    _Atomic uint64_t *size=(_Atomic uint64_t *)(void *)(output_page+32);
    atomic_store(size,pack_size(cw,ch));
    uint64_t config=atomic_load((_Atomic uint64_t *)(void *)(output_page+40));
    const char *before="";
    if(config){uint32_t old=__atomic_fetch_or((uint32_t *)(uintptr_t)(config+8),1u<<21,__ATOMIC_SEQ_CST);before=old&(1u<<21)?" (the previous switch was still pending)":"";runtime_oe_push_message(30);}
    int maxres=runtime_output_max_res();
    fprintf(stderr,"[totk] 1.4.3 output requested %ux%u (%s)%s%s\n",cw,ch,
            full?(maxres==TOTK_RES_2160?"native 4K: the canvas and the UI at that size":"true 2K: the canvas and the UI at that size")
                 :(maxres==TOTK_RES_2160?"MetalFX 4K: 1080p canvas, MetalFX scales it at present":"the 2K output: 1080p canvas, MetalFX scales it at present"),
            config?" from the next layout":"; the game has not laid out its displays yet, it takes this size when it does",before);
    return 1;
}
int runtime_output_res_toggle(void){
    if(!output_page)return 0;
    _Atomic uint64_t *size=(_Atomic uint64_t *)(void *)(output_page+32);
    int full=atomic_load(size)!=pack_size(output_w,output_h);
    return output_request_canvas(full?output_w:1920,full?output_h:1080,full);
}
int runtime_output_mode_cycle(void){
    if(!output_page)return 0;
    extern int mfxup_output_enabled(void);extern void dm_gfx_set_flag(const char *,int);
    _Atomic uint64_t *size=(_Atomic uint64_t *)(void *)(output_page+32);
    uint64_t current=atomic_load(size);
    int full=current==pack_size(output_w,output_h),canvas_1080=current==pack_size(1920,1080);
    int maxres=runtime_output_max_res();
    const char *off_name=maxres==TOTK_RES_2160?"关闭 4K":"关闭 2K";
    const char *native_name=maxres==TOTK_RES_2160?"原生 4K":"原生 2K";
    const char *mfx_name=maxres==TOTK_RES_2160?"MetalFX 4K":"MetalFX 2K";
    int next_full,next_mfx;const char *name;
    if(full){next_full=0;next_mfx=0;name=off_name;}
    else if(mfxup_output_enabled()){next_full=1;next_mfx=0;name=native_name;}
    else if(!canvas_1080){next_full=0;next_mfx=0;name=off_name;}
    else{next_full=0;next_mfx=1;name=mfx_name;}
    if(full!=next_full||!canvas_1080)output_request_canvas(next_full?output_w:1920,next_full?output_h:1080,next_full);
    dm_gfx_set_flag("mfx_output",next_mfx);dm_gfx_set_flag("mfx_upscale",next_mfx);
    fprintf(stderr,"[totk] key 3: %s\n",name);return 1;
}
/* Settings picture mode (runtime/settings.h): deterministic OFF / MetalFX / NATIVE instead of key 3's relative cycle.
   -1: no 2K-capable start; -2: the display config or a previous switch is still pending. Key 3 keeps its own path. */
static const char *output_mode_name(int mode){
    int maxres=runtime_output_max_res();
    return mode==2?(maxres==TOTK_RES_2160?"native 4K":"native 2K"):mode==1?(maxres==TOTK_RES_2160?"MetalFX 4K":"MetalFX 2K"):"off (1080p)";
}
int runtime_output_available(void){return atomic_load(&output_page)!=NULL;}
int runtime_output_mode_get(void){
    uint8_t *page=atomic_load(&output_page);if(!page)return -1;
    if(atomic_load((_Atomic uint64_t *)(void *)(page+32))==pack_size(output_w,output_h))return 2;
    extern int mfxup_output_enabled(void);return mfxup_output_enabled()?1:0;
}
int runtime_output_mode_set(int mode){
    if(mode<0||mode>2){fprintf(stderr,"[totk] output mode %d: expected 0 (off), 1 (MetalFX 2K) or 2 (native 2K)\n",mode);return -1;}
    uint8_t *page=atomic_load(&output_page);
    if(!page){fprintf(stderr,"[totk] output mode %s: 2K needs a 2K-capable start (TOTK_OUTPUT_RES=auto with the HeapDef overlay, or <w>x<h>)\n",output_mode_name(mode));return -1;}
    extern void dm_gfx_set_flag(const char *,int);int mfx=mode==1;
    int current=runtime_output_mode_get();
    if(current==mode){dm_gfx_set_flag("mfx_output",mfx);dm_gfx_set_flag("mfx_upscale",mfx);return 0;}
    /* Only a resolution change re-runs the game's display handler; OFF <-> MetalFX is just the upscaler flags, as key 3 does. */
    int full=mode==2,was_full=current==2;
    if(full!=was_full){
        uint64_t config=atomic_load((_Atomic uint64_t *)(void *)(page+40));
        if(!config){fprintf(stderr,"[totk] output mode %s: the game has not laid out its displays yet (pending)\n",output_mode_name(mode));return -2;}
        uint32_t old=__atomic_fetch_or((uint32_t *)(uintptr_t)(config+8),1u<<21,__ATOMIC_SEQ_CST);
        if(old&(1u<<21)){fprintf(stderr,"[totk] output mode %s: the previous 2K switch is still pending\n",output_mode_name(mode));return -2;}
    }
    atomic_store((_Atomic uint64_t *)(void *)(page+32),full?pack_size(output_w,output_h):pack_size(1920,1080));
    dm_gfx_set_flag("mfx_output",mfx);dm_gfx_set_flag("mfx_upscale",mfx);
    if(full!=was_full)runtime_oe_push_message(30);
    fprintf(stderr,"[totk] output mode: %s\n",output_mode_name(mode));return 0;
}

/* Picture v2 (runtime/settings.h): choose the output resolution and the upscaler by value. res names the presented tier
   (the startup drawable); the canvas is what changes at run time. MetalFX upscales the 1080p canvas to the drawable, so
   MetalFX 2K needs a 2K start and MetalFX 4K a 3840x2160 one; native draws the canvas itself (2K native works on any
   2K/4K start, 4K native needs the 3840x2160 swapchain). A mode this process did not start big enough for returns
   RUNTIME_OUTPUT_RESTART so settings can persist it and start again. */
static const char *output_name_ex(int res,int upscaler){
    if(res==TOTK_RES_1080)return "off (1080p)";
    if(res==TOTK_RES_2160)return upscaler==TOTK_UPSCALE_NATIVE?"native 4K":"MetalFX 4K";
    return upscaler==TOTK_UPSCALE_NATIVE?"native 2K":"MetalFX 2K";
}
static int output_mode_plan(int res,int upscaler,unsigned *cw,unsigned *ch,int *mfx){
    if(upscaler!=TOTK_UPSCALE_NATIVE&&upscaler!=TOTK_UPSCALE_METALFX)return RUNTIME_OUTPUT_INVALID;
    int maxres=runtime_output_max_res();
    switch(res){
        case TOTK_RES_1080:
            /* Off is the 1080p canvas with the output scaler disabled. 1080p with MetalFX would be the same 1080p canvas
               as MetalFX 2K (the scaler targets the drawable), so it is not a separate mode. */
            if(upscaler!=TOTK_UPSCALE_NATIVE)return RUNTIME_OUTPUT_INVALID;
            *cw=1920;*ch=1080;*mfx=0;return RUNTIME_OUTPUT_OK;
        case TOTK_RES_1440:
            if(maxres<TOTK_RES_1440)return RUNTIME_OUTPUT_RESTART;
            if(upscaler==TOTK_UPSCALE_NATIVE){*cw=2560;*ch=1440;*mfx=0;}
            else if(maxres==TOTK_RES_2160)return RUNTIME_OUTPUT_RESTART;   /* a 4K drawable presents MetalFX at 4K: that is 2160 MetalFX */
            else{*cw=1920;*ch=1080;*mfx=1;}
            return RUNTIME_OUTPUT_OK;
        case TOTK_RES_2160:
            if(maxres<TOTK_RES_2160)return RUNTIME_OUTPUT_RESTART;
            if(upscaler==TOTK_UPSCALE_NATIVE){*cw=output_w;*ch=output_h;*mfx=0;}
            else{*cw=1920;*ch=1080;*mfx=1;}
            return RUNTIME_OUTPUT_OK;
    }
    return RUNTIME_OUTPUT_INVALID;
}
int runtime_output_mode_available_ex(int res,int upscaler){
    unsigned cw,ch;int mfx;
    return output_mode_plan(res,upscaler,&cw,&ch,&mfx)==RUNTIME_OUTPUT_OK;
}
int runtime_output_mode_restart_required(int res,int upscaler){
    unsigned cw,ch;int mfx;
    return output_mode_plan(res,upscaler,&cw,&ch,&mfx)==RUNTIME_OUTPUT_RESTART;
}
int runtime_output_mode_get_ex(int *res,int *upscaler){
    uint8_t *page=atomic_load(&output_page);
    int r=TOTK_RES_1080,u=TOTK_UPSCALE_NATIVE;
    if(page){
        extern int mfxup_output_enabled(void);
        uint64_t size=atomic_load((_Atomic uint64_t *)(void *)(page+32));
        int mfx=mfxup_output_enabled();
        int unknown=0;
        if(size==pack_size(1920,1080)){
            if(mfx){r=runtime_output_max_res()==TOTK_RES_2160?TOTK_RES_2160:TOTK_RES_1440;u=TOTK_UPSCALE_METALFX;}
        }else if(!mfx&&size==pack_size(2560,1440)){r=TOTK_RES_1440;}
        else if(!mfx&&size==pack_size(3840,2160)){r=TOTK_RES_2160;}
        else if(!mfx&&size==pack_size(output_w,output_h)){
            /* An explicit startup size that is neither 2K nor 4K: native at the tier the drawable reaches. */
            if(output_w==3840&&output_h==2160)r=TOTK_RES_2160;
            else if(output_w>=2560&&output_h>=1440)r=TOTK_RES_1440;
        }else unknown=1;
        if(res)*res=r;if(upscaler)*upscaler=u;
        if(unknown){
            fprintf(stderr,"[totk] output mode: the canvas %ux%u (MetalFX %s) is not a picture-v2 mode\n",(unsigned)(size&0xffffffffu),(unsigned)(size>>32),mfx?"on":"off");
            return -1;
        }
        return 0;
    }
    if(res)*res=r;if(upscaler)*upscaler=u;
    return 0;
}
int runtime_output_mode_set_ex(int res,int upscaler){
    const char *name=output_name_ex(res,upscaler);
    unsigned cw,ch;int mfx;
    int plan=output_mode_plan(res,upscaler,&cw,&ch,&mfx);
    if(plan==RUNTIME_OUTPUT_INVALID){
        fprintf(stderr,"[totk] output mode %d/%d: not a presentation mode (1080 native, 1440 native or MetalFX, 2160 native or MetalFX)\n",res,upscaler);
        return RUNTIME_OUTPUT_INVALID;
    }
    if(plan==RUNTIME_OUTPUT_RESTART){
        fprintf(stderr,"[totk] output mode %s: needs the %s start (TOTK_OUTPUT_RES=%s with the HeapDef overlay); restart required\n",
                name,res==TOTK_RES_2160?"3840x2160":"2560x1440",res==TOTK_RES_2160?"3840x2160":"2560x1440");
        return RUNTIME_OUTPUT_RESTART;
    }
    extern int mfxup_output_enabled(void);extern void dm_gfx_set_flag(const char *,int);
    uint8_t *page=atomic_load(&output_page);
    uint64_t wanted=pack_size(cw,ch);
    uint64_t current=page?atomic_load((_Atomic uint64_t *)(void *)(page+32)):pack_size(1920,1080);
    if(current==wanted){
        dm_gfx_set_flag("mfx_output",mfx);dm_gfx_set_flag("mfx_upscale",mfx);
        fprintf(stderr,"[totk] output mode: %s\n",name);return RUNTIME_OUTPUT_OK;
    }
    /* A canvas change re-runs the game's display handler. Claim it first: when a previous switch is still in flight this
       request changes nothing (settings reports "pending" and keeps the old target). */
    uint64_t config=page?atomic_load((_Atomic uint64_t *)(void *)(page+40)):0;
    if(!config){
        fprintf(stderr,"[totk] output mode %s: the game has not laid out its displays yet (pending)\n",name);
        return RUNTIME_OUTPUT_PENDING;
    }
    uint32_t old=__atomic_fetch_or((uint32_t *)(uintptr_t)(config+8),1u<<21,__ATOMIC_SEQ_CST);
    if(old&(1u<<21)){
        fprintf(stderr,"[totk] output mode %s: the previous output switch is still pending\n",name);
        return RUNTIME_OUTPUT_PENDING;
    }
    atomic_store((_Atomic uint64_t *)(void *)(page+32),wanted);
    dm_gfx_set_flag("mfx_output",mfx);dm_gfx_set_flag("mfx_upscale",mfx);
    runtime_oe_push_message(30);   /* nn::oe "operation mode changed": the game runs its display handler */
    fprintf(stderr,"[totk] output mode: %s (%ux%u)\n",name,cw,ch);return RUNTIME_OUTPUT_OK;
}
