/* Execute the production translator on native ARM64 with synthetic device
   masks and guarded words only. No game image, saves, controller or GPU. */
#include "action_probe_support.h"
#include "../games/totk/runtime/semantic_action_patch.h"
#include <sys/mman.h>
#include <libkern/OSCacheControl.h>
void probe_action_masks(void *entry,const void *device,uint64_t out[14]);
void probe_gameplay_raw(void *entry,const void *device,uint64_t out[4],const void *node);
void probe_query_masks(void *entry,void *node,uint64_t out[19]);
static const uint32_t rawMasks[]={1,2,8,16,0x2000,0x4000,4,32,0x10000,0x20000,0x40000,0x80000,0x80,0x40,0xc00,0x1200};
static void check_translation(Module *m,const RuntimeActionBindings *b,uint32_t edge,uint32_t held,uint32_t released){
 unsigned char device[0x118]={0};memcpy(device+8,&edge,4);memcpy(device+12,&released,4);memcpy(device+0x114,&held,4);
 unsigned char before[sizeof device];memcpy(before,device,sizeof device);uint64_t out[14];
 probe_action_masks(m->base+SEMANTIC_ACTION_ENTRY,device,out);
 uint32_t expectedEdge=0,expectedHeld=0,expectedReleased=0;
 for(unsigned i=0;i<ACTION_BINDING_COUNT;i++){
  if(edge&rawMasks[i])expectedEdge|=b->target[i]&0x3fff;
  if(held&rawMasks[i])expectedHeld|=b->target[i]&0xfff;
  if(released&rawMasks[i]){if(b->target[i]==ACTION_ATTACK)expectedReleased|=1;if(b->target[i]==ACTION_ABILITY)expectedReleased|=2;}
 }
 assert(out[0]==expectedEdge&&out[1]==expectedHeld&&out[2]==expectedReleased);
 assert(out[3]==0x60000000);for(unsigned i=0;i<10;i++)assert(out[4+i]==8+i);
 // The screen-request reads remap gameplay bits but preserve Plus/Minus and
 // ordinary UI/device state. In particular D-pad -> Jump cannot also open a shortcut.
 uint32_t inputs[]={edge,released,held,held,edge};
 for(unsigned k=0;k<3;k++){
  uint32_t expected=inputs[k]&~0xf7effu;
  for(unsigned i=0;i<ACTION_BINDING_COUNT;i++)if(inputs[k]&rawMasks[i]){
   int target=action_binding_code_index(b->target[i]);if(target)expected|=rawMasks[target-1];
  }
  for(unsigned context=0;context<(k>=3?2u:1u);context++){
   const char *label=context?"UI":"Camera";uintptr_t property=(uintptr_t)&label|1;
   unsigned char node[0x78]={0};memcpy(node+0x70,&property,8);
   uint32_t wanted=context?inputs[k]:expected;
   uint64_t ui[4];probe_gameplay_raw(m->base+semantic_raw_sites[k].offset,device,ui,node);
   assert(ui[0]==(k==0||k>=3?wanted:8)&&ui[1]==(k==2?wanted:9)&&ui[2]==(k==1?wanted:27)&&ui[3]==0x60000000);
  }
 }
 assert(!memcmp(before,device,sizeof device)); // UI/raw device masks never change
}
static void check_query(Module *m,const RuntimeActionBindings *b){
 const char *names[]={"_LocalModule_31bc5067250e.module","SpecialPowerShortcut.module","MaterialShortcut.module","WeaponShortcut.module","ShieldShortcut.module","BowShortcut.module","PauseMenu.module"};
 for(unsigned context=0;context<7;context++){
  uintptr_t resource[2]={0,(uintptr_t)names[context]},owner[5]={0};owner[4]=(uintptr_t)resource;
  unsigned char node[0x78]={0};uintptr_t ptr=(uintptr_t)owner;memcpy(node+8,&ptr,8);
  uint32_t inputs[]={0xf7eff,0xf7eff,0,0x2000,0x80};memcpy(node+0x20,inputs,sizeof inputs);
  ptr=0x12345678;memcpy(node+0x50,&ptr,8);uint64_t out[19];
  probe_query_masks(m->base+0xc7a0dc,node,out);
  assert(out[0]==(uintptr_t)(m->base+0xc7a0dc)&&out[18]==0x60000000);
  for(unsigned i=1;i<18;i++)assert(out[i]==(i==9?0x12345678:i));
  for(unsigned k=0;k<5;k++){
   uint32_t mapped=inputs[k]&~0xf7effu;
   for(unsigned i=0;i<ACTION_BINDING_COUNT;i++)if(inputs[k]&rawMasks[i]){int n=action_binding_code_index(b->target[i]);if(n)mapped|=rawMasks[n-1];}
   const uint32_t triggers[]={0,0x2000,0x10000,0x80000,0x40000,0x80020,0};
   uint32_t wanted=context==0?mapped:inputs[k]|(mapped&triggers[context]),actual;
   if(context&&k==2){uint32_t held=inputs[1]&~0xf7effu;for(unsigned i=0;i<ACTION_BINDING_COUNT;i++)if(inputs[1]&rawMasks[i]){int n=action_binding_code_index(b->target[i]);if(n)held|=rawMasks[n-1];}wanted&=~((inputs[1]|held)&triggers[context]);}
   memcpy(&actual,node+0x20+k*4,4);assert(actual==wanted);
  }
 }
}
typedef struct { ActionLive *page; RuntimeActionBindings a,b; } ActionRace;
static void *action_race_writer(void *arg){ActionRace *r=arg;for(unsigned i=0;i<5000;i++){action_publish(r->page,&r->a);action_publish(r->page,&r->b);}return NULL;}
static void check_concurrent_apply(Module *m){
 ActionRace r={.page=atomic_load(&actionPage)};for(unsigned i=0;i<ACTION_BINDING_COUNT;i++){r.a.target[i]=ACTION_JUMP;r.b.target[i]=ACTION_ATTACK;}
 action_publish(r.page,&r.a);pthread_t thread;assert(!pthread_create(&thread,NULL,action_race_writer,&r));
 unsigned char device[0x118]={0};uint32_t mask=0xf7eff;memcpy(device+8,&mask,4);memcpy(device+12,&mask,4);memcpy(device+0x114,&mask,4);
 for(unsigned i=0;i<2000;i++){uint64_t out[14];probe_action_masks(m->base+SEMANTIC_ACTION_ENTRY,device,out);assert((out[0]==4&&out[1]==4&&out[2]==0)||(out[0]==8&&out[1]==8&&out[2]==1));}
 assert(!pthread_join(thread,NULL));
}
int main(void){
 RuntimeActionBindings b;action_bindings_default(&b);
 if(!action_version_supported()){assert(!runtime_action_bindings_set(&b));assert(!runtime_action_bindings_available());puts("PASS: unsupported game version refuses player action changes");return 0;}
 Module m={.text_end=0x2070d00,.ro_off=0x2072000,.file_size=0x2078100,.size=0x2080000};
 m.base=mmap(NULL,m.size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);assert(m.base!=MAP_FAILED);
 const unsigned sites=sizeof semantic_action_sites/sizeof *semantic_action_sites;
 for(unsigned i=0;i<sites;i++)memcpy(m.base+semantic_action_sites[i].offset,&semantic_action_sites[i].original,4);
 for(unsigned i=0;i<3;i++)memcpy(m.base+semantic_raw_sites[i].offset,&semantic_raw_sites[i].original,4);
 uint32_t word=0xb9400a88;memcpy(m.base+SEMANTIC_ACTION_ENTRY,&word,4);word=0xf9400668;memcpy(m.base+SEMANTIC_ACTION_RETURN,&word,4);
 word=0xf94012a9;memcpy(m.base+0xc7a0dc,&word,4);
 // A mismatch must leave the entry, original operation words and padding intact.
 word=0;memcpy(m.base+semantic_action_sites[sites-1].offset,&word,4);assert(!semantic_action_install(&m));
 memcpy(&word,m.base+SEMANTIC_ACTION_ENTRY,4);assert(word==0xb9400a88);
 for(unsigned i=0;i<sites-1;i++){memcpy(&word,m.base+semantic_action_sites[i].offset,4);assert(word==semantic_action_sites[i].original);}
 memcpy(m.base+semantic_action_sites[sites-1].offset,&semantic_action_sites[sites-1].original,4);
 assert(semantic_action_install(&m)&&runtime_action_bindings_available());
 for(unsigned i=0;i<sites;i++){memcpy(&word,m.base+semantic_action_sites[i].offset,4);assert(word==semantic_action_sites[i].original);}
 // The synthetic continuation returns to the probe rather than running a game.
 word=0xd65f03c0;memcpy(m.base+SEMANTIC_ACTION_RETURN,&word,4);memcpy(m.base+0xc7a0e0,&word,4);
 for(unsigned i=0;i<3;i++)memcpy(m.base+semantic_raw_sites[i].offset+4,&word,4);
 assert(mprotect(m.base,(m.text_end+0x3fff)&~0x3fffULL,PROT_READ|PROT_EXEC)==0);sys_icache_invalidate(m.base,(m.text_end+0x3fff)&~0x3fffULL);
 // Every source and target, including stick-held and remapped attack/ability release.
 for(unsigned k=0;k<ACTION_BINDING_CODE_COUNT;k++){
  for(unsigned i=0;i<ACTION_BINDING_COUNT;i++)b.target[i]=action_binding_code_at((i+k)%ACTION_BINDING_CODE_COUNT);
  assert(runtime_action_bindings_set(&b));
  for(unsigned i=0;i<ACTION_BINDING_COUNT;i++){
   check_translation(&m,&b,rawMasks[i],rawMasks[i],0);
   check_translation(&m,&b,0,0,rawMasks[i]);
  }
  check_translation(&m,&b,0xf7eff,0xf7eff,0xf7eff);check_query(&m,&b);
 }
 check_concurrent_apply(&m);
 action_bindings_xbox(&b);assert(runtime_action_bindings_set(&b));
 initialize();uint32_t id=0;NpadState state;static const unsigned keysToPress[]={38,40,34,32};static const unsigned raw[]={1,2,4,8};
 for(unsigned i=0;i<4;i++){runtime_input_key_synthetic(keysToPress[i],1);full_states(&state,1,&id);assert(state.buttons==raw[i]);runtime_input_key_synthetic(keysToPress[i],0);}
 RuntimeActionBindings bad,got;action_bindings_default(&bad);bad.target[0]=3;assert(!runtime_action_bindings_set(&bad));runtime_action_bindings_get(&got);assert(action_bindings_equal(&got,&b));
 action_bindings_default(&b);assert(runtime_action_bindings_set(&b));check_translation(&m,&b,0xf7eff,0xf7eff,0xf7eff);
 runtime_action_bindings_attach(NULL);assert(!runtime_action_bindings_available());assert(!runtime_action_bindings_set(&b));runtime_action_bindings_get(&got);assert(action_bindings_equal(&b,&got));
 munmap(m.base,m.size);puts("PASS: 16-source edge/held/release, gameplay UI requests, coherent RX-safe live actions, registers/flags, guards and unchanged UI masks");return 0;
}
