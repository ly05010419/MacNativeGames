/* 1.0.0 Mac player-operation patch. Offsets/guards were established by the local
   TOTK input analysis (2026-10-22); no raw Npad/UI masks or game resources change.
   Code is installed once before RX sealing. A single guarded entry redirects the edge/held/release translation. The stub
   snapshots one sequence-checked mapping and preserves scratch registers, SP, LR and NZCV. */
#pragma once
#include "runtime.h"
typedef struct { uint32_t offset,original;unsigned char source,reg,set; } SemanticActionSite;
static const SemanticActionSite semantic_action_sites[]={
 /* A, B, X, Y, L, R, ZL, ZR, Up, Down, Left, Right, L-stick, R-stick (edge mask w22). */
 {0x1ccfe28,0x52800036,0,22,1},{0x1ccfe30,0x321f02d6,1,22,0},{0x1ccfc60,0x321e02d6,2,22,0},
 {0x1ccfe38,0x321d02d6,3,22,0},{0x1ccfe40,0x321c02d6,4,22,0},{0x1ccfe48,0x321b02d6,5,22,0},
 {0x1ccfe50,0x321a02d6,6,22,0},{0x1ccfe58,0x321902d6,7,22,0},{0x1ccfe60,0x321802d6,8,22,0},
 {0x1ccfe68,0x321702d6,9,22,0},{0x1ccfe70,0x321602d6,10,22,0},{0x1ccfe78,0x321502d6,11,22,0},
 {0x1ccfe80,0x321402d6,12,22,0},{0x1ccfe88,0x321302d6,13,22,0},
 /* Held mask w21; the stick clicks have no held site. */
 {0x1ccfe90,0x52800035,0,21,1},{0x1ccfe98,0x321f02b5,1,21,0},{0x1ccfc68,0x321e02b5,2,21,0},
 {0x1ccfea0,0x321d02b5,3,21,0},{0x1ccfea8,0x321c02b5,4,21,0},{0x1ccfeb0,0x321b02b5,5,21,0},
 {0x1ccfeb8,0x321a02b5,6,21,0},{0x1ccfec0,0x321902b5,7,21,0},{0x1ccfec8,0x321802b5,8,21,0},
 {0x1ccfed0,0x321702b5,9,21,0},{0x1ccfed8,0x321602b5,10,21,0},{0x1ccfee0,0x321502b5,11,21,0}
};
/* These are reads in the gameplay screen-request router, not menu navigation.
   The byte-sized bow gate needs the full held word before source remapping. */
typedef struct {uint32_t offset,original,load;unsigned reg;} SemanticRawSite;
static const SemanticRawSite semantic_raw_sites[]={
 {0x10635bc,0xb9400aa8,0xb9400aa8,8},
 {0x10635dc,0xb9400ebb,0xb9400ebb,27},
 {0x1063b64,0x39445009,0xb9411409,9},

};
enum { SEMANTIC_ACTION_PADDING=512, SEMANTIC_ACTION_DATA=64,
       SEMANTIC_ACTION_ENTRY=0x1ccf96c, SEMANTIC_ACTION_RETURN=0x1ccf9f4,
       SEMANTIC_RAW_TRAMP_BYTES=48 };
extern const unsigned char semantic_action_code[],semantic_action_code_end[];
extern const unsigned char semantic_action_data_adrp[],semantic_action_resume[];
extern const unsigned char semantic_raw_code[],semantic_raw_code_end[],semantic_raw_data_adrp[];
extern const unsigned char semantic_query_code[],semantic_query_code_end[],semantic_query_callback[],semantic_query_resume[];
static inline uint32_t semantic_action_branch(uint64_t from,uint64_t to){return 0x14000000u|((uint32_t)(((int64_t)to-(int64_t)from)/4)&0x3ffffffu);}
static inline uint32_t semantic_action_adrp(uint64_t from,uint64_t data){
 uint32_t p=(uint32_t)((int64_t)(data>>12)-(int64_t)(from>>12));
 return 0x90000010u|((p&3)<<29)|(((p>>2)&0x7ffff)<<5);
}
/* UI graphs read a separate raw-button query. Only the gameplay trigger graph
   is translated here. Its stable resource name was established in the local
   1.0.0 analysis. Shortcut/menu graphs retain their original navigation masks. */
static uint32_t semantic_query_mask(uint32_t raw,const RuntimeActionBindings *b){
 static const uint32_t masks[]={1,2,8,16,0x2000,0x4000,4,32,0x10000,0x20000,0x40000,0x80000,0x80,0x40,0xc00,0x1200};
 uint32_t out=raw&~0xf7effu;
 for(unsigned i=0;i<ACTION_BINDING_COUNT;i++)if(raw&masks[i]){int n=action_binding_code_index(b->target[i]);if(n)out|=masks[n-1];}
 return out;
}
static void semantic_query_update(void *node){
 uintptr_t owner,resource,name;
 memcpy(&owner,(char*)node+8,8);if(!owner)return;
 memcpy(&resource,(void*)(owner+0x20),8);if(!resource)return;
 memcpy(&name,(void*)(resource+8),8);if(!name)return;
 uint32_t trigger=0;
 int gameplay=!strcmp((const char*)name,"_LocalModule_31bc5067250e.module");
 if(!gameplay){
  if(!strcmp((const char*)name,"SpecialPowerShortcut.module"))trigger=0x2000;
  else if(!strcmp((const char*)name,"MaterialShortcut.module"))trigger=0x10000;
  else if(!strcmp((const char*)name,"WeaponShortcut.module"))trigger=0x80000;
  else if(!strcmp((const char*)name,"ShieldShortcut.module"))trigger=0x40000;
  else if(!strcmp((const char*)name,"BowShortcut.module"))trigger=0x80020;
  else return;
 }
 RuntimeActionBindings b;runtime_action_bindings_get(&b);
 uint32_t raw[5];memcpy(raw,(char*)node+0x20,sizeof raw);
 uint32_t mapped[5];for(unsigned i=0;i<5;i++)mapped[i]=semantic_query_mask(raw[i],&b);
 for(unsigned i=0;i<5;i++){
  /* Inside a shortcut, keep the original UI controls and also recognize the
     custom opening key. A release cannot close it while either key is held. */
  uint32_t out=gameplay?mapped[i]:raw[i]|(mapped[i]&trigger);
  if(!gameplay&&i==2)out&=~((raw[1]|mapped[1])&trigger);
  memcpy((char*)node+0x20+i*4,&out,4);
 }

}
static inline int semantic_action_install(Module *m){
 const uint64_t stub=((m->text_end+15)&~15ULL)+SEMANTIC_ACTION_PADDING;
 const size_t bytes=(uintptr_t)semantic_action_code_end-(uintptr_t)semantic_action_code;
 const size_t adr=(uintptr_t)semantic_action_data_adrp-(uintptr_t)semantic_action_code;
 const size_t resume=(uintptr_t)semantic_action_resume-(uintptr_t)semantic_action_code;
 const size_t rawBytes=(uintptr_t)semantic_raw_code_end-(uintptr_t)semantic_raw_code;
 const size_t rawAdr=(uintptr_t)semantic_raw_data_adrp-(uintptr_t)semantic_raw_code;
 const size_t queryBytes=(uintptr_t)semantic_query_code_end-(uintptr_t)semantic_query_code;
 const size_t queryCallback=(uintptr_t)semantic_query_callback-(uintptr_t)semantic_query_code;
 const size_t queryResume=(uintptr_t)semantic_query_resume-(uintptr_t)semantic_query_code;
 const uint64_t raw=stub+bytes,tramps=(raw+rawBytes+15)&~15ULL;
 const unsigned rawCount=sizeof semantic_raw_sites/sizeof *semantic_raw_sites;
 const uint64_t query=(tramps+rawCount*SEMANTIC_RAW_TRAMP_BYTES+7)&~7ULL;
 const uint64_t end=query+queryBytes,data=(m->file_size+0x3fff)&~0x3fffULL;
 if(((uintptr_t)m->base&0x3fff)||data+0x4000!=m->size||end>m->ro_off||end>((m->text_end+0x3fff)&~0x3fffULL))return 0;
 if(SEMANTIC_ACTION_RETURN+4>m->text_end||bytes<4||adr+4>bytes||resume+4>bytes||rawAdr+4>rawBytes||queryCallback+8>queryBytes||queryResume+4>queryBytes)return 0;
 uint32_t word;memcpy(&word,m->base+SEMANTIC_ACTION_ENTRY,4);if(word!=0xb9400a88)return 0;
 memcpy(&word,m->base+SEMANTIC_ACTION_RETURN,4);if(word!=0xf9400668)return 0;
 memcpy(&word,m->base+0xc7a0dc,4);if(word!=0xf94012a9u)return 0;
 /* All 32 guards and all padding are checked before any byte is changed. */
 for(unsigned i=0;i<sizeof semantic_action_sites/sizeof *semantic_action_sites;i++){
  const SemanticActionSite *q=&semantic_action_sites[i];
  if((uint64_t)q->offset+4>m->text_end)return 0;
  memcpy(&word,m->base+q->offset,4);if(word!=q->original)return 0;
 }
 for(unsigned i=0;i<rawCount;i++){
  const SemanticRawSite *q=&semantic_raw_sites[i];if((uint64_t)q->offset+4>m->text_end)return 0;
  memcpy(&word,m->base+q->offset,4);if(word!=q->original)return 0;
  int64_t d=(int64_t)(tramps+i*SEMANTIC_RAW_TRAMP_BYTES)-q->offset;if(d<=-(1LL<<27)||d>=(1LL<<27))return 0;
 }
 for(uint64_t i=stub;i<end;i++)if(m->base[i])return 0;
 int64_t distance=(int64_t)stub-SEMANTIC_ACTION_ENTRY;
 if(distance<=-(1LL<<27)||distance>=(1LL<<27))return 0;
 int64_t pages=(int64_t)(data>>12)-(int64_t)((stub+adr)>>12);
 int64_t rawPages=(int64_t)(data>>12)-(int64_t)((raw+rawAdr)>>12);
 if(pages<-(1LL<<20)||pages>=(1LL<<20)||rawPages<-(1LL<<20)||rawPages>=(1LL<<20))return 0;
 memcpy(m->base+stub,semantic_action_code,bytes);
 word=semantic_action_adrp(stub+adr,data);memcpy(m->base+stub+adr,&word,4);
 word=semantic_action_branch(stub+resume,SEMANTIC_ACTION_RETURN);memcpy(m->base+stub+resume,&word,4);
 memcpy(m->base+raw,semantic_raw_code,rawBytes);
 word=semantic_action_adrp(raw+rawAdr,data);memcpy(m->base+raw+rawAdr,&word,4);
 for(unsigned i=0;i<rawCount;i++){
  const SemanticRawSite *q=&semantic_raw_sites[i];uint64_t at=tramps+i*SEMANTIC_RAW_TRAMP_BYTES;
  uint32_t code[12]={0xa9bf7be8,q->load,0x2a0003e8u|(q->reg<<16),
   semantic_action_branch(at+12,raw)|0x80000000u,0x2a0803e0u|q->reg,
   q->reg==8?0xd503201fu:0xf94003e8u,0xf94007fe,0x910043ff,
   semantic_action_branch(at+32,q->offset+4),0xd503201f,0xd503201f,0xd503201f};
  memcpy(m->base+at,code,sizeof code);
 }
 memcpy(m->base+query,semantic_query_code,queryBytes);
 uintptr_t callback=(uintptr_t)semantic_query_update;memcpy(m->base+query+queryCallback,&callback,8);
 word=semantic_action_branch(query+queryResume,0xc7a0e0);memcpy(m->base+query+queryResume,&word,4);
 word=semantic_action_branch(0xc7a0dc,query);memcpy(m->base+0xc7a0dc,&word,4);
 runtime_action_bindings_attach(m->base+data+SEMANTIC_ACTION_DATA);
 word=semantic_action_branch(SEMANTIC_ACTION_ENTRY,stub);memcpy(m->base+SEMANTIC_ACTION_ENTRY,&word,4);
 for(unsigned i=0;i<rawCount;i++){
  word=semantic_action_branch(semantic_raw_sites[i].offset,tramps+i*SEMANTIC_RAW_TRAMP_BYTES);
  memcpy(m->base+semantic_raw_sites[i].offset,&word,4);
 }
 return 1;
}
