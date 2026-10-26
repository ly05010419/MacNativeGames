/* 1.4.3 action layer: install into a synthetic module and execute the actual
   gate, operation translator, fused raw requests, Bow load and query veneer. */
#include "action_probe_support.h"
#include "../games/totk/runtime/semantic_action_patch_v143.h"
void runtime_diag_frm_watch(double s,unsigned o,int x){(void)s;(void)o;(void)x;abort();}
void runtime_diag_xpoke(unsigned o,float v){(void)o;(void)v;abort();}
#include <sys/mman.h>
#include <libkern/OSCacheControl.h>
void probe_core_gate(void *entry,const void *device,uint32_t gate,uint32_t out[4]);
void probe_raw_dual(void *site,const void *device,uint32_t out[3]);
void probe_raw_bow(void *site,const void *device,uint32_t out[2]);
void probe_query(void *site,void *node,uint64_t out[4]);
static const uint32_t rawMasks[]={1,2,8,16,0x2000,0x4000,4,32,0x10000,0x20000,0x40000,0x80000,0x80,0x40,0xc00,0x1200};
static uint32_t raw_map(const RuntimeActionBindings *b,uint32_t in){
 uint32_t out=in&~0xf7effu;
 for(unsigned i=0;i<ACTION_BINDING_COUNT;i++)if(in&rawMasks[i]){int n=action_binding_code_index(b->target[i]);if(n)out|=rawMasks[n-1];}
 return out;
}
static void check_core(Module *m,const RuntimeActionBindings *b,uint32_t edge,uint32_t held,uint32_t release){
 unsigned char device[0x118]={0};memcpy(device+8,&edge,4);memcpy(device+12,&release,4);memcpy(device+0x114,&held,4);
 uint32_t out[4];probe_core_gate(m->base+0xf23e28,device,1,out);
 uint32_t ee=0,eh=0,er=0;
 for(unsigned i=0;i<ACTION_BINDING_COUNT;i++){
  if(edge&rawMasks[i])ee|=b->target[i]&0x3fff;
  if(held&rawMasks[i])eh|=b->target[i]&0xfff;
  if(release&rawMasks[i]){if(b->target[i]==ACTION_ATTACK)er|=1;if(b->target[i]==ACTION_ABILITY)er|=2;}
 }
 if(!(out[0]==ee&&out[1]==eh&&out[2]==er))fprintf(stderr,"core mismatch edge=%#x held=%#x rel=%#x got %u %u %u want %u %u %u\n",edge,held,release,out[0],out[1],out[2],ee,eh,er);
 assert(out[0]==ee&&out[1]==eh&&out[2]==er);
 probe_core_gate(m->base+0xf23e28,device,0,out);
 assert(out[0]==0&&out[1]==0&&out[2]==0);
}
static void check_raw(Module *m,const RuntimeActionBindings *b,uint32_t edge,uint32_t release,uint32_t held){
 unsigned char device[0x118]={0};memcpy(device+8,&edge,4);memcpy(device+12,&release,4);memcpy(device+0x114,&held,4);
 uint32_t out[3];probe_raw_dual(m->base+0xecbe18,device,out);
 assert(out[0]==raw_map(b,edge)&&out[1]==raw_map(b,release));
 probe_raw_bow(m->base+0xecc07c,device,out);
 assert(out[0]==raw_map(b,held));
}
static void check_query(Module *m,const RuntimeActionBindings *b,const char *name,uint32_t inputs[5]){
 uintptr_t resource[2]={0,(uintptr_t)name};uintptr_t owner[5]={0};owner[4]=(uintptr_t)resource;
 unsigned char node[0x78]={0};uintptr_t ptr=(uintptr_t)owner;memcpy(node+8,&ptr,8);
 memcpy(node+0x20,inputs,5*4);uintptr_t linked=0x12345678;memcpy(node+0x50,&linked,8);
 uint64_t out[4];probe_query(m->base+0x6c767c,node,out);
 assert(out[0]==0x12345678&&out[1]==(uintptr_t)node+0x30&&out[2]==(uintptr_t)node);
 uint32_t mapped[5];for(unsigned k=0;k<5;k++)mapped[k]=raw_map(b,inputs[k]);
 for(unsigned k=0;k<5;k++){
  uint32_t wanted=mapped[k],actual;memcpy(&actual,node+0x20+k*4,4);
  if(strcmp(name,"_LocalModule_31bc5067250e.module")){
   uint32_t trigger=!strcmp(name,"SpecialPowerShortcut.module")?0x2000:
    !strcmp(name,"MaterialShortcut.module")?0x10000:!strcmp(name,"WeaponShortcut.module")?0x80000:
    !strcmp(name,"ShieldShortcut.module")?0x40000:0x80020;
   wanted=inputs[k]|(mapped[k]&trigger);
   if(k==2)wanted&=~((inputs[1]|mapped[1])&trigger);
  }
  assert(actual==wanted);
 }
}
int main(void){
 enum { TEXT_END=0xf25000, FILE_SIZE=0xf28000, SIZE=0xf30000, DATA=FILE_SIZE };
 Module m={.text_end=TEXT_END,.ro_off=TEXT_END,.file_size=FILE_SIZE,.size=SIZE};
 m.base=mmap(NULL,SIZE,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
 assert(m.base&&m.base!=MAP_FAILED&&!((uintptr_t)m.base&0x3fff));
 for(unsigned i=0;i<sizeof semantic_action_gates_v143/sizeof *semantic_action_gates_v143;i++)memcpy(m.base+semantic_action_gates_v143[i].offset,&semantic_action_gates_v143[i].original,4);
 for(unsigned i=0;i<sizeof semantic_action_sites_v143/sizeof *semantic_action_sites_v143;i++)memcpy(m.base+semantic_action_sites_v143[i].offset,&semantic_action_sites_v143[i].original,4);
 for(unsigned i=0;i<sizeof semantic_raw_sites_v143/sizeof *semantic_raw_sites_v143;i++)memcpy(m.base+semantic_raw_sites_v143[i].offset,&semantic_raw_sites_v143[i].original,4);
 for(unsigned i=0;i<sizeof semantic_query_sites_v143/sizeof *semantic_query_sites_v143;i++)memcpy(m.base+semantic_query_sites_v143[i].offset,&semantic_query_sites_v143[i].original,4);
 uint32_t word=0x37001bca;memcpy(m.base+SEMANTIC_ACTION_V143_ENTRY,&word,4);
 word=0xf9400668;memcpy(m.base+SEMANTIC_ACTION_V143_RETURN,&word,4);
 word=0;memcpy(m.base+semantic_action_sites_v143[sizeof semantic_action_sites_v143/sizeof *semantic_action_sites_v143-1].offset,&word,4);
 assert(!semantic_action_install_v143(&m));
 memcpy(m.base+semantic_action_sites_v143[sizeof semantic_action_sites_v143/sizeof *semantic_action_sites_v143-1].offset,&semantic_action_sites_v143[sizeof semantic_action_sites_v143/sizeof *semantic_action_sites_v143-1].original,4);
 assert(semantic_action_install_v143(&m)&&runtime_action_bindings_available());
 word=0xd65f03c0;
 memcpy(m.base+SEMANTIC_ACTION_V143_RETURN,&word,4);
 memcpy(m.base+SEMANTIC_RAW_V143_DUAL+4,&word,4);
 memcpy(m.base+SEMANTIC_RAW_V143_BOW+4,&word,4);
 memcpy(m.base+SEMANTIC_QUERY_V143_RESUME,&word,4);
 assert(mprotect(m.base,TEXT_END,PROT_READ|PROT_EXEC)==0);
 assert(mprotect(m.base+DATA+RUNTIME_ACTION_CODE_OFFSET,RUNTIME_ACTION_CODE_SIZE,PROT_READ|PROT_EXEC)==0);
 sys_icache_invalidate(m.base,TEXT_END);sys_icache_invalidate(m.base+DATA+RUNTIME_ACTION_CODE_OFFSET,RUNTIME_ACTION_CODE_SIZE);
 RuntimeActionBindings b;action_bindings_default(&b);assert(runtime_action_bindings_set(&b));
 check_core(&m,&b,1,2,16);check_core(&m,&b,0xf7eff,0xf7eff,0xf7eff);
 check_raw(&m,&b,0xf7eff,0xf7eff,0xf7eff);
 for(unsigned k=0;k<ACTION_BINDING_CODE_COUNT;k++){
  for(unsigned i=0;i<ACTION_BINDING_COUNT;i++)b.target[i]=action_binding_code_at((i+k)%ACTION_BINDING_CODE_COUNT);
  assert(runtime_action_bindings_set(&b));
  check_core(&m,&b,1,2,16);check_core(&m,&b,0x10000,0x80,0x10);check_raw(&m,&b,1,0x10,0x20);
 }
 action_bindings_default(&b);b.target[0]=ACTION_JUMP;b.target[3]=ACTION_INTERACT;b.target[2]=ACTION_ATTACK;assert(runtime_action_bindings_set(&b));
 check_core(&m,&b,1,2,16);check_raw(&m,&b,0xf7eff,0xf7eff,0xf7eff);
 b.target[8]=ACTION_BOW;b.target[15]=ACTION_JUMP;assert(runtime_action_bindings_set(&b));
 check_raw(&m,&b,1,0x10,0x10000);
 uint32_t inputs[5]={0xf7eff,0xf7eff,0,0x2000,0x80};
 check_query(&m,&b,"_LocalModule_31bc5067250e.module",inputs);
 check_query(&m,&b,"SpecialPowerShortcut.module",inputs);
 assert(runtime_action_bindings_set(&b));
 runtime_action_bindings_attach(NULL);assert(!runtime_action_bindings_available());
 munmap(m.base,SIZE);
 puts("PASS: 1.4.3 gate, operation masks, fused raw requests, Bow gate and query veneer");return 0;
}
