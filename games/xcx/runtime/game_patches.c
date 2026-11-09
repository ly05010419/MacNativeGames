#include "game_patches.h"
#include "game_version.h"
#include "movie_host.h"
#include <TargetConditionals.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Independent native adaptation of jmars125/xcx-recomp (MIT),
   feb125c975924331da8ca5365f39d7a6548d6eed: recomp/patches-upd.txt and
   runtime/frame_rate.c. See third_party/COPIES.md. These are 2.0.0 sites,
   checked against the already hash-verified loaded image before ANY write.
   No generated image on disk is modified. The unsafe getter/mode patches
   from the reference are deliberately excluded. */
typedef struct { uint32_t offset, original; unsigned count; uint32_t words[4]; } Site;
/* A word below 32 encodes ADRP xN to the private timing page. */
static const Site sites[] = {
 {0x13db540,0x52800040,1,{0x52800020}},
 {0x13baa34,0x72a79108,2,{8,0xb9404108}},
 {0x13a9af4,0x1e210800,2,{8,0xbd404900}},
 {0xa1f05c,0x1e27d001,2,{9,0xbd404521}},
 {0x56d40,0x72a7a108,2,{8,0xb9404108}},
 {0x1e28e0,0x1e204100,3,{9,0xbd404d20,0x1e200900}},
 {0x1e291c,0x1e204100,3,{9,0xbd404d20,0x1e200900}},
 {0xeb408,0x1e204100,3,{9,0xbd404d20,0x1e200900}},
 {0x5c6284,0x1e213800,4,{8,0xbd404d02,0x1e220821,0x1e213800}},
 {0xab700,0xbd40b6a8,4,{8,0xbd40b6a8,0xbd404d10,0x1e300908}},
 {0xe5e8c,0x1e280800,4,{0,0xbd404c01,0x1e280800,0x1e210800}},
 {0xe5860,0x1e214000,4,{8,0xbd404d01,0x1e214000,0x1e210800}},
 {0xe5eec,0x1e3e1001,2,{8,0xbd404d01}},
 {0xe5ef0,0x1e212800,1,{0x1e213800}},
 {0x23efe8,0x5286f7a8,1,{8}},
 {0x23efec,0x72b6b0c8,1,{0xd503201f}},
 {0x23eff8,0x1e270101,1,{0xbd404101}},
 {0x23effc,0x1e212800,1,{0x1e213800}},
 {0x927e4c,0x1e201821,4,{9,0xbd404121,0x1e220003,0x1e230821}},
 {0x927e50,0x1e201840,1,{0x1e210840}},
 {0xa1116c,0xbd489501,4,{0xbd489501,11,0xbd404d65,0x1e250821}},
 {0xa11180,0xbd489501,4,{0xbd489501,11,0xbd404d65,0x1e250821}},
 {0xa1126c,0xbd489500,4,{0xbd489500,8,0xbd404d02,0x1e220800}}
};
static _Atomic uint32_t *timing;
static uint8_t *game;
static unsigned limit, sample_count, sample_next;
static int was_cut, trace;
static uint64_t last_ns, trace_start, trace_frames;
static double samples[5], trace_steps;

static uint32_t branch(uint64_t from, uint64_t to) {
 int64_t delta=(int64_t)to-(int64_t)from;
 if((delta&3)||delta<-(1LL<<27)||delta>=(1LL<<27))runtime_fail("XCX timing branch range");
 return 0x14000000|((uint32_t)(delta>>2)&0x3ffffff);
}
static uint32_t page(unsigned reg, uint64_t from, uint64_t to) {
 int64_t delta=(int64_t)(to>>12)-(int64_t)(from>>12);
 if(delta<-(1LL<<20)||delta>=(1LL<<20))runtime_fail("XCX timing page range");
 uint32_t imm=(uint32_t)delta&0x1fffff;
 return 0x90000000|((imm&3)<<29)|((imm>>2)<<5)|reg;
}
static void store_float(unsigned i, float f) {
 uint32_t bits;memcpy(&bits,&f,4);atomic_store_explicit(timing+i,bits,memory_order_release);
}
static void clock_store(double seconds, int cut) {
 store_float(0,(float)(cut?1.0/60:seconds));
 store_float(1,(float)(cut?60:1/seconds));
 store_float(2,(float)(cut?1.0/30:1.0/limit));
 store_float(3,(float)(cut?1:30*seconds));
}
void runtime_game_patches(Module *main_module) {
 const char *e=getenv("XCX_FPS_LIMIT");
 if(!e||!strcmp(e,"30")) { fprintf(stderr,"[xcx] timing: original 30 FPS\n");return; }
 if(strcmp(e,"60")&&strcmp(e,"120"))runtime_fail("XCX_FPS_LIMIT: use 30, 60 or 120");
#if TARGET_OS_IPHONE
 runtime_fail("XCX timing experiment needs macOS");
#endif
 if(strcmp(TOTK_GAME_VERSION,"2.0.0"))runtime_fail("XCX timing experiment needs verified 2.0.0");
 const char *hz=getenv("DM_PRESENT_HZ"),*itv=getenv("DM_PRESENT_INTERVAL"),*mfx=getenv("TOTK_MFX");
 if((hz&&strcmp(hz,e))||(itv&&strcmp(itv,"game"))||(mfx&&strcmp(mfx,"0")))
  runtime_fail("XCX timing experiment conflicts with present pacing or interpolation");
 Module *m=main_module;
 if(strcmp(m->name,"main")||m->text_end!=0x1735050||m->ro_off!=0x1736000||
    m->file_size>SIZE_MAX-0x7fff)runtime_fail("XCX timing image layout");
 size_t data=(m->file_size+0x3fff)&~(size_t)0x3fff;
 if(m->file_size<m->ro_off||m->size<data+0x4000||((uintptr_t)m->base&0x3fff))runtime_fail("XCX timing private page");
 size_t cave=(m->text_end+15)&~(size_t)15,used=0;
 /* Prepare every word and branch in a local plan first. A late mismatch must
    leave all earlier sites, the cave and the timing page untouched. */
 uint32_t replacement[sizeof sites/sizeof *sites], cave_words[128];
 for(size_t i=m->text_end;i<m->ro_off;i++)if(m->base[i])runtime_fail("XCX timing cave is occupied");
 for(size_t i=0;i<sizeof sites/sizeof *sites;i++) {
  const Site *s=sites+i;uint32_t original;memcpy(&original,m->base+s->offset,4);
  if(original!=s->original)runtime_fail("XCX timing original instruction mismatch");
  int trampoline=s->count>1;
  uint64_t pc=(uintptr_t)m->base+(trampoline?cave+used*4:s->offset);
  replacement[i]=trampoline?branch((uintptr_t)m->base+s->offset,pc):s->words[0];
  for(unsigned j=0;j<s->count;j++) {
   uint32_t w=s->words[j];if(w<32)w=page(w,pc+j*4,(uintptr_t)m->base+data);
   if(trampoline) { if(used>=127)runtime_fail("XCX timing cave plan full");cave_words[used++]=w; }
   else replacement[i]=w;
  }
  if(trampoline)cave_words[used++]=branch(pc+s->count*4,(uintptr_t)m->base+s->offset+4);
 }
 if(cave+used*4>m->ro_off)runtime_fail("XCX timing cave too small");
 if(setenv("DM_PRESENT_HZ",e,1)||setenv("TOTK_UNCAPPED","0",1))runtime_fail("XCX timing pacing environment");
 memcpy(m->base+cave,cave_words,used*4);
 for(size_t i=0;i<sizeof sites/sizeof *sites;i++)memcpy(m->base+sites[i].offset,replacement+i,4);
 limit=(unsigned)strtoul(e,NULL,10);game=m->base;
 timing=(_Atomic uint32_t *)(m->base+data+0x40);clock_store(1.0/limit,0);
 const char *t=getenv("XCX_FPS_TRACE");trace=t&&!strcmp(t,"1");
 fprintf(stderr,"[xcx] experimental %u FPS: 23 checked timing sites, camera/battle/menu/sky steps, cutscenes 30 FPS\n",limit);
}

/* The existing present hook runs on the guest's presenting thread. Aligned
   whole-word stores match the game's scalar loads; the private page remains
   writable after text is sealed. Long loading gaps restart the average. */
void runtime_frame_step_pin(double ms) { (void)ms; }
void runtime_frame_step_update(uint64_t now_ns) {
 if(!timing)return;
 uint32_t event=__atomic_load_n((uint32_t *)(game+0x1fefaf4),__ATOMIC_ACQUIRE);
 int cut=event>=2||movie_host_playing();
 double seconds=1.0/limit;
 if(last_ns&&now_ns>last_ns) {
  double delta=(now_ns-last_ns)/1e9;
  if(delta<0.25&&cut==was_cut) {
   if(delta>1.0/15)delta=1.0/15;
   if(delta<0.001)delta=0.001;
   samples[sample_next]=delta;sample_next=(sample_next+1)%5;
   if(sample_count<5)sample_count++;
   seconds=0;for(unsigned i=0;i<sample_count;i++)seconds+=samples[i];seconds/=sample_count;
  } else sample_count=sample_next=0;
 }
 __atomic_store_n((uint32_t *)(game+0x1d943a0),cut?2:1,__ATOMIC_RELEASE);
 clock_store(seconds,cut);was_cut=cut;last_ns=now_ns;
 if(trace) {
  if(!trace_start)trace_start=now_ns;
  else { trace_frames++;trace_steps+=cut?1.0/30:seconds; }
  if(trace_frames>=120) {
   double wall=(now_ns-trace_start)/1e9;
   fprintf(stderr,"[xcx-fps] cap=%u rendered=%.2f step/wall=%.3f cut=%d\n",limit,trace_frames/wall,trace_steps/wall,cut);
   trace_start=now_ns;trace_frames=0;trace_steps=0;
  }
 }
}
int runtime_output_res(unsigned *w, unsigned *h) { (void)w; (void)h; return 0; }
uint64_t runtime_output_res_extra_bytes(void) { return 0; }
uint64_t runtime_output_size_current(void) { return (uint64_t)1080 << 32 | 1920; }
int runtime_output_res_toggle(void) { return 0; }
int runtime_output_mode_cycle(void) { return 0; }
int runtime_aspect_toggle(void) { return 0; }
float runtime_aspect_now(void) { return 0; }
