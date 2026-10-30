// Native execution of generated timing instructions against synthetic objects.
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../runtime/gfx/nvn/aoc_timing.h"
extern unsigned char timing_image[];
extern void timing_device(void), timing_task(void), timing_step(void), timing_scene(void);
typedef struct {
    uint64_t time, previous, object, frequency, system, now, x8, x9;
    float work, s0, s9;
} Registers;
_Static_assert(offsetof(Registers,work)==64 && offsetof(Registers,s9)==72,"assembly layout");
extern void timing_call(void (*stub)(void), Registers *registers);
static Registers r;
enum { STATE=0x3000000, TIME=0x3500000, OBJECT=0x3502000, DEVICE=0x3503000, FPS=0x269e150 };
static void put(unsigned off, const void *v, size_t n) { memcpy(timing_image+off,v,n); }
static void fput(unsigned off,float v) { put(off,&v,4); }
static void wput(unsigned off,uint32_t v) { put(off,&v,4); }
static void xput(unsigned off,uint64_t v) { put(off,&v,8); }
static float fget(unsigned off) { float v;memcpy(&v,timing_image+off,4);return v; }
static uint32_t wget(unsigned off) { uint32_t v;memcpy(&v,timing_image+off,4);return v; }
static uint64_t xget(unsigned off) { uint64_t v;memcpy(&v,timing_image+off,8);return v; }
static double dget(unsigned off) { double v;memcpy(&v,timing_image+off,8);return v; }
static uint64_t tick(void) { uint64_t v;__asm__ volatile("mrs %0,cntvct_el0":"=r"(v));return v; }
static uint64_t frequency(void) { uint64_t v;__asm__ volatile("mrs %0,cntfrq_el0":"=r"(v));return v; }
static unsigned checks;
#define CHECK(x) do { checks++; if(!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x);exit(1); } } while(0)
static void near(double a,double b,double epsilon) { CHECK(fabs(a-b)<epsilon); }
static void configure(int target,int dynamic) { fput(STATE,target);wput(STATE+AOC_DYNAMIC,dynamic); }
static float update(double dt) {
    timing_call(timing_task,&r);
    r.work=.0001f; // pre-wait work must never substitute for elapsed time
    r.previous=10000000;r.frequency=19200000;r.now=r.previous+llround(dt*r.frequency);
    timing_call(timing_step,&r);
    return fget(TIME+0x18);
}
static void initialize(void) {
    r=(Registers){.time=(uintptr_t)(timing_image+TIME),.system=(uintptr_t)(timing_image+0x3501000),
        .object=(uintptr_t)(timing_image+OBJECT)};
    fput(0x1ef5f3c,59.94f);xput(0x2bac798,(uintptr_t)(timing_image+DEVICE));
    wput(OBJECT+0x48,30);fput(OBJECT+0x34,1.f/30);fput(OBJECT+0x44,1.f/30);
    fput(STATE+AOC_MAX_DT,.1f);fput(STATE+AOC_MIN_DT,.001f);
    fput(STATE+AOC_COUNTER_SCALE,1.f/frequency());
}
static void targets(void) {
    for(int target=30;target<=60;target+=30) {
        configure(target,1);double before=dget(STATE+AOC_SECONDS);
        for(int i=0;i<target*2;i++) near(update(1./target),1./target,1e-7);
        near(dget(STATE+AOC_SECONDS)-before,2,1e-5);
        CHECK(wget(TIME+0x30)==(unsigned)target);near(fget(FPS+12),target,.001);
        near(fget(DEVICE+0x32d0),target*59.94/60,.001);
        CHECK(wget(FPS+4)==(target==30?2u:1u));
    }
}
static void variable(void) {
    configure(60,1);double total=0,route[]={1./60,1./30,1./45,.040,1./50};
    for(int i=0;i<300;i++) { double dt=route[i%5];near(update(dt),dt,1e-7);total+=dt; }
    near(dget(STATE+AOC_SECONDS),total,1e-5);
    timing_call(timing_task,&r);near(fget(FPS+12),50,.001);
}
static void switches(void) {
    int route[]={60,30,60,0,30,0};
    for(unsigned i=0;i<sizeof(route)/sizeof(*route);i++) {
        int target=route[i];configure(target,1);update(1./(target?target:30));
        CHECK(wget(TIME+0x30)==(unsigned)(target?target:30));
        near(fget(FPS+12),target?target:30,.001);
        CHECK(wget(DEVICE+0x2aa4)==(target<=30?2u:1u));
    }
    near(fget(TIME+0x18),1./30,1e-7);
}
static void bounds(void) {
    configure(60,0);near(update(.025),1./60,1e-7);
    configure(60,1);near(update(5),.1,1e-6);near(update(0),.001,1e-7);
}
static void timescale(void) {
    configure(60,1);fput(OBJECT+0x34,.5f/30);fput(OBJECT+0x44,.25f/30);
    near(update(.02),.01,1e-7);near(r.s0,.005,1e-7);
}
static void scene(void) {
    configure(60,1);update(1./60);timing_call(timing_scene,&r);
    double before=dget(STATE+AOC_SCENE_SECONDS),total=0;
    for(int i=0;i<96;i++) {
        update(1./60);
        xput(STATE+AOC_SCENE_TICKS,tick()-frequency()/48);
        r.x8=0x12345678;timing_call(timing_scene,&r);
        CHECK(r.x8==0x12345678);
        float dt=fget(STATE+AOC_SCENE_DT);CHECK(dt>=1.f/48-1e-6f && dt<=.1f);total+=dt;
        near(fget(FPS+12),1./dt,.001);near(r.s9,1./dt,.001);
    }
    near(dget(STATE+AOC_SCENE_SECONDS)-before,total,1e-5);
    update(1./60);near(fget(FPS+12),1./fget(STATE+AOC_SCENE_DT),.001);
    CHECK(wget(TIME+0x30)==60);CHECK(xget(STATE+AOC_SCENE_UPDATES)==97);
}
static void scene_bounds(void) {
    configure(0,1);fput(FPS+12,30);r.x9=(uintptr_t)(timing_image+0x269e000);
    timing_call(timing_scene,&r);near(r.s9,30,.001);
    configure(60,1);timing_call(timing_scene,&r);
    xput(STATE+AOC_SCENE_TICKS,tick()-frequency()*5);timing_call(timing_scene,&r);
    near(fget(STATE+AOC_SCENE_DT),.1,1e-6);
}
static void device(void) {
    int route[]={0,30,60};
    for(unsigned i=0;i<sizeof(route)/sizeof(*route);i++) {
        configure(route[i],1);r.x9=0;timing_call(timing_device,&r);
        CHECK(r.x9>>32==(route[i]<=30?2u:1u));
    }
    xput(0x2bac798,0);configure(60,1);timing_call(timing_task,&r);
}
int main(int argc,char **argv) {
    if(argc!=2)return 2;
    initialize();
    struct { const char *name; void (*run)(void); } cases[]={
        {"targets",targets},{"variable",variable},{"switches",switches},{"bounds",bounds},
        {"timescale",timescale},{"scene",scene},{"scene-bounds",scene_bounds},{"device",device}};
    for(unsigned i=0;i<sizeof(cases)/sizeof(*cases);i++) if(!strcmp(argv[1],cases[i].name)) {
        cases[i].run();printf("native timing %s: %u checks passed\n",argv[1],checks);return 0;
    }
    return 2;
}
