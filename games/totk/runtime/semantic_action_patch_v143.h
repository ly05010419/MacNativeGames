/* 1.4.3 Mac player-operation patch. The original entry at main+0xf23e28
   gate-filters the edge, held and release masks into w10/w9/w8; the translator
   starts after that gate (main+0xf23e44) and resumes at main+0xf23ebc. The
   screen-request router fused its edge and release loads into one ldp
   (main+0xecbe18) and reads the Bow held bit at main+0xecc07c; the generic
   query node publishes its five masks at main+0x6c767c. All words are checked
   before any write. Code lives in the private executable page after main's
   image (loader.c), the binding table in its writable first page. */
#pragma once
#include <TargetConditionals.h>
#if !TARGET_OS_IPHONE
#include "runtime.h"
#include "action_bindings.h"
#include <string.h>

typedef struct { uint32_t offset,original; } SemanticActionSiteV143;
static const SemanticActionSiteV143 semantic_action_gates_v143[]={
 {0xf23e28,0xb9411688},{0xf23e2c,0x7200001f},{0xf23e30,0x2a1f03f5},
 {0xf23e34,0x29412e8a},{0xf23e38,0x1a9f1109},{0xf23e3c,0x1a9f1168},
 {0xf23e40,0x1a9f114a},
};
static const SemanticActionSiteV143 semantic_action_sites_v143[]={
 /* A, B, X, Y, L, R, ZL, ZR, Up, Down, Left, Right, L-stick, R-stick (edge: w21). */
 {0xf241bc,0x52800035},{0xf241c4,0x321f02b5},{0xf240dc,0x321e02b5},
 {0xf241cc,0x321d02b5},{0xf241d4,0x321c02b5},{0xf241dc,0x321b02b5},
 {0xf241e4,0x321a02b5},{0xf241ec,0x321902b5},{0xf241f4,0x321802b5},
 {0xf241fc,0x321702b5},{0xf24204,0x321602b5},{0xf2420c,0x321502b5},
 {0xf24214,0x321402b5},{0xf2421c,0x321302b5},
 /* Held (w20); the stick clicks have no held site. */
 {0xf24224,0x52800034},{0xf2422c,0x321f0294},{0xf240e4,0x321e0294},
 {0xf24234,0x321d0294},{0xf2423c,0x321c0294},{0xf24244,0x321b0294},
 {0xf2424c,0x321a0294},{0xf24254,0x32190294},{0xf2425c,0x32180294},
 {0xf24264,0x32170294},{0xf2426c,0x32160294},{0xf24274,0x32150294},
};
static const SemanticActionSiteV143 semantic_raw_sites_v143[]={
 {0xecbe18,0x29416aa8},   /* ldp w8,w26,[x21,#8]: edge + release */
 {0xecc07c,0xb9411408},   /* ldr w8,[x0,#0x114]: held Bow gate */
};
static const SemanticActionSiteV143 semantic_query_sites_v143[]={
 {0x6c75f8,0xaa0003f3},{0x6c764c,0xb8024d88},{0x6c7654,0xb8020d68},
 {0x6c765c,0xb802cd28},{0x6c7668,0xb8028d0a},{0x6c7678,0xb8030c2a},
 {0x6c767c,0xf940102a},{0x6c7680,0x3600016a},
};
enum { SEMANTIC_ACTION_V143_ENTRY=0xf23e44, SEMANTIC_ACTION_V143_RETURN=0xf23ebc,
       SEMANTIC_QUERY_V143_SITE=0x6c767c, SEMANTIC_QUERY_V143_RESUME=0x6c7680,
       SEMANTIC_RAW_V143_DUAL=0xecbe18, SEMANTIC_RAW_V143_BOW=0xecc07c,
       SEMANTIC_ACTION_V143_DATA=64, SEMANTIC_ACTION_V143_CODE_OFFSET=RUNTIME_ACTION_CODE_OFFSET,
       SEMANTIC_RAW_V143_TRAMP=48 };

extern const unsigned char semantic_action_v143_code[],semantic_action_v143_code_end[];
extern const unsigned char semantic_action_v143_data_adrp[],semantic_action_v143_resume[];
extern const unsigned char semantic_raw_code[],semantic_raw_code_end[],semantic_raw_data_adrp[];
extern const unsigned char semantic_query_code[],semantic_query_code_end[],semantic_query_callback[];
extern const unsigned char semantic_query_resume[],semantic_query_original_load[];

static inline uint32_t semantic_action_branch_v143(uint64_t from,uint64_t to){
 return 0x14000000u|((uint32_t)(((int64_t)to-(int64_t)from)/4)&0x3ffffffu);
}
static inline uint32_t semantic_action_adrp_v143(uint64_t from,uint64_t data){
 uint32_t p=(uint32_t)((int64_t)(data>>12)-(int64_t)(from>>12));
 return 0x90000010u|((p&3)<<29)|(((p>>2)&0x7ffff)<<5);
}
static uint32_t semantic_query_mask_v143(uint32_t raw,const RuntimeActionBindings *b){
 static const uint32_t masks[]={1,2,8,16,0x2000,0x4000,4,32,0x10000,0x20000,0x40000,0x80000,0x80,0x40,0xc00,0x1200};
 uint32_t out=raw&~0xf7effu;
 for(unsigned i=0;i<ACTION_BINDING_COUNT;i++)if(raw&masks[i]){int n=action_binding_code_index(b->target[i]);if(n)out|=masks[n-1];}
 return out;
}
static void semantic_query_update_v143(void *node){
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
 uint32_t mapped[5];for(unsigned i=0;i<5;i++)mapped[i]=semantic_query_mask_v143(raw[i],&b);
 for(unsigned i=0;i<5;i++){
  uint32_t out=gameplay?mapped[i]:raw[i]|(mapped[i]&trigger);
  if(!gameplay&&i==2)out&=~((raw[1]|mapped[1])&trigger);
  memcpy((char*)node+0x20+i*4,&out,4);
 }
}
static const char *semantic_action_v143_failure;
static uint32_t semantic_action_v143_failure_offset;
#define V143_FAIL(s) do{semantic_action_v143_failure=(s);return 0;}while(0)
static inline int semantic_action_install_v143(Module *m){
 const uint64_t data=(m->file_size+0x3fff)&~0x3fffULL,code=data+SEMANTIC_ACTION_V143_CODE_OFFSET,codeEnd=data+0x8000;
 const size_t coreBytes=(uintptr_t)semantic_action_v143_code_end-(uintptr_t)semantic_action_v143_code;
 const size_t coreAdr=(uintptr_t)semantic_action_v143_data_adrp-(uintptr_t)semantic_action_v143_code;
 const size_t coreResume=(uintptr_t)semantic_action_v143_resume-(uintptr_t)semantic_action_v143_code;
 const size_t rawBytes=(uintptr_t)semantic_raw_code_end-(uintptr_t)semantic_raw_code;
 const size_t rawAdr=(uintptr_t)semantic_raw_data_adrp-(uintptr_t)semantic_raw_code;
 const size_t queryBytes=(uintptr_t)semantic_query_code_end-(uintptr_t)semantic_query_code;
 const size_t queryCallback=(uintptr_t)semantic_query_callback-(uintptr_t)semantic_query_code;
 const size_t queryResume=(uintptr_t)semantic_query_resume-(uintptr_t)semantic_query_code;
 const size_t queryLoad=(uintptr_t)semantic_query_original_load-(uintptr_t)semantic_query_code;
 const uint64_t stub=code,raw=stub+coreBytes;
 const uint64_t tramps=(raw+rawBytes+15)&~15ULL;
 const uint64_t query=(tramps+2*SEMANTIC_RAW_V143_TRAMP+7)&~7ULL;
 const uint64_t end=query+queryBytes;
 if(((uintptr_t)m->base&0x3fff)||data+0x8000!=m->size)V143_FAIL("layout: base alignment or private-page size");
 if(codeEnd!=data+0x8000||end>codeEnd)V143_FAIL("layout: code page bounds");
 if(coreBytes<4||coreAdr+4>coreBytes||coreResume+4>coreBytes)V143_FAIL("layout: translator symbol offsets");
 if(rawAdr+4>rawBytes||queryCallback+8>queryBytes||queryResume+4>queryBytes||queryLoad+4>queryBytes)V143_FAIL("layout: adapter symbol offsets");
 int64_t d=(int64_t)stub-SEMANTIC_ACTION_V143_ENTRY;if(d<=-(1LL<<27)||d>=(1LL<<27))V143_FAIL("layout: core branch range");
 d=(int64_t)query-SEMANTIC_QUERY_V143_SITE;if(d<=-(1LL<<27)||d>=(1LL<<27))V143_FAIL("layout: query hook range");
 d=(int64_t)query-SEMANTIC_QUERY_V143_RESUME;if(d<=-(1LL<<27)||d>=(1LL<<27))V143_FAIL("layout: query resume range");
 d=(int64_t)tramps-SEMANTIC_RAW_V143_DUAL;if(d<=-(1LL<<27)||d>=(1LL<<27))V143_FAIL("layout: raw dual range");
 d=(int64_t)(tramps+SEMANTIC_RAW_V143_TRAMP)-SEMANTIC_RAW_V143_BOW;if(d<=-(1LL<<27)||d>=(1LL<<27))V143_FAIL("layout: raw bow range");
 int64_t pages=(int64_t)(data>>12)-(int64_t)((stub+coreAdr)>>12);if(pages<-(1LL<<20)||pages>=(1LL<<20))V143_FAIL("layout: core ADRP range");
 pages=(int64_t)(data>>12)-(int64_t)((raw+rawAdr)>>12);if(pages<-(1LL<<20)||pages>=(1LL<<20))V143_FAIL("layout: raw ADRP range");
 uint32_t word;
 for(unsigned i=0;i<sizeof semantic_action_gates_v143/sizeof *semantic_action_gates_v143;i++){
  const SemanticActionSiteV143 *q=&semantic_action_gates_v143[i];
  if((uint64_t)q->offset+4>m->text_end){semantic_action_v143_failure_offset=q->offset;V143_FAIL("guard: site outside text");}
  memcpy(&word,m->base+q->offset,4);if(word!=q->original){semantic_action_v143_failure_offset=q->offset;V143_FAIL("guard: gate word mismatch");}
 }
 memcpy(&word,m->base+SEMANTIC_ACTION_V143_ENTRY,4);if(word!=0x37001bca){semantic_action_v143_failure_offset=SEMANTIC_ACTION_V143_ENTRY;V143_FAIL("guard: core entry");}
 memcpy(&word,m->base+SEMANTIC_ACTION_V143_RETURN,4);if(word!=0xf9400668){semantic_action_v143_failure_offset=SEMANTIC_ACTION_V143_RETURN;V143_FAIL("guard: core return");}
 for(unsigned i=0;i<sizeof semantic_action_sites_v143/sizeof *semantic_action_sites_v143;i++){
  const SemanticActionSiteV143 *q=&semantic_action_sites_v143[i];
  if((uint64_t)q->offset+4>m->text_end){semantic_action_v143_failure_offset=q->offset;V143_FAIL("guard: site outside text");}
  memcpy(&word,m->base+q->offset,4);if(word!=q->original){semantic_action_v143_failure_offset=q->offset;V143_FAIL("guard: operation handler word mismatch");}
 }
 for(unsigned i=0;i<sizeof semantic_raw_sites_v143/sizeof *semantic_raw_sites_v143;i++){
  const SemanticActionSiteV143 *q=&semantic_raw_sites_v143[i];
  if((uint64_t)q->offset+4>m->text_end){semantic_action_v143_failure_offset=q->offset;V143_FAIL("guard: site outside text");}
  memcpy(&word,m->base+q->offset,4);if(word!=q->original){semantic_action_v143_failure_offset=q->offset;V143_FAIL("guard: raw request word mismatch");}
 }
 for(unsigned i=0;i<sizeof semantic_query_sites_v143/sizeof *semantic_query_sites_v143;i++){
  const SemanticActionSiteV143 *q=&semantic_query_sites_v143[i];
  if((uint64_t)q->offset+4>m->text_end){semantic_action_v143_failure_offset=q->offset;V143_FAIL("guard: site outside text");}
  memcpy(&word,m->base+q->offset,4);if(word!=q->original){semantic_action_v143_failure_offset=q->offset;V143_FAIL("guard: query publication word mismatch");}
 }
 for(uint64_t i=stub;i<end;i++)if(m->base[i])V143_FAIL("padding: translator code page not empty");
 for(unsigned i=0;i<40;i++)if(m->base[data+SEMANTIC_ACTION_V143_DATA+i])V143_FAIL("padding: binding table not empty");
 memcpy(m->base+stub,semantic_action_v143_code,coreBytes);
 word=semantic_action_adrp_v143(stub+coreAdr,data);memcpy(m->base+stub+coreAdr,&word,4);
 word=semantic_action_branch_v143(stub+coreResume,SEMANTIC_ACTION_V143_RETURN);memcpy(m->base+stub+coreResume,&word,4);
 memcpy(m->base+raw,semantic_raw_code,rawBytes);
 word=semantic_action_adrp_v143(raw+rawAdr,data);memcpy(m->base+raw+rawAdr,&word,4);
 memcpy(m->base+query,semantic_query_code,queryBytes);
 word=0xf940102a;memcpy(m->base+query+queryLoad,&word,4);
 uintptr_t callback=(uintptr_t)semantic_query_update_v143;memcpy(m->base+query+queryCallback,&callback,8);
 word=semantic_action_branch_v143(query+queryResume,SEMANTIC_QUERY_V143_RESUME);memcpy(m->base+query+queryResume,&word,4);
 uint32_t dual[12]={0xa9be7be8,0x29416aa8,0xb90017fa,0,
  0xb90013e8,0xb94017e8,0,0x2a0803fa,0xb94013e8,0xf94007fe,0x910083ff,0};
 dual[3]=semantic_action_branch_v143(tramps+12,raw)|0x80000000u;
 dual[6]=semantic_action_branch_v143(tramps+24,raw)|0x80000000u;
 dual[11]=semantic_action_branch_v143(tramps+44,SEMANTIC_RAW_V143_DUAL+4);
 memcpy(m->base+tramps,dual,sizeof dual);
 const uint64_t at=tramps+SEMANTIC_RAW_V143_TRAMP;
 uint32_t single[12]={0xa9bf7be8,0xb9411408,0x2a0803e8,0,
  0x2a0803e8,0xd503201f,0xf94007fe,0x910043ff,0,0xd503201f,0xd503201f,0xd503201f};
 single[3]=semantic_action_branch_v143(at+12,raw)|0x80000000u;
 single[8]=semantic_action_branch_v143(at+32,SEMANTIC_RAW_V143_BOW+4);
 memcpy(m->base+at,single,sizeof single);
 word=semantic_action_branch_v143(SEMANTIC_ACTION_V143_ENTRY,stub);memcpy(m->base+SEMANTIC_ACTION_V143_ENTRY,&word,4);
 word=semantic_action_branch_v143(SEMANTIC_RAW_V143_DUAL,tramps);memcpy(m->base+SEMANTIC_RAW_V143_DUAL,&word,4);
 word=semantic_action_branch_v143(SEMANTIC_RAW_V143_BOW,at);memcpy(m->base+SEMANTIC_RAW_V143_BOW,&word,4);
 word=semantic_action_branch_v143(SEMANTIC_QUERY_V143_SITE,query);memcpy(m->base+SEMANTIC_QUERY_V143_SITE,&word,4);
 runtime_action_bindings_attach(m->base+data+SEMANTIC_ACTION_V143_DATA);
 return 1;
}
#endif
