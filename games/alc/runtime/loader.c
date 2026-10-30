/* Native ARM64 module loader. Guest images and generated metadata stay local. */
#include "runtime.h"
#include "totk_ui_lang.h"
#include <TargetConditionals.h>
#include <dlfcn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <libkern/OSCacheControl.h>
#include <CommonCrypto/CommonDigest.h>
#include <mach/mach_time.h>
#include <signal.h>
#include <mach/mach.h>
#include "mach_vm_compat.h"
#include <sys/ucontext.h>
#include "runtime_tables.h"
#include "game_patches.h"
#include "game_version.h"
#define NM (sizeof(modules)/sizeof(modules[0]))
pthread_key_t runtime_keys[8];
static uint64_t timer_calls, svc_calls;
_Noreturn void runtime_fail(const char *message) { fprintf(stderr,"[totk] FAIL: %s\n",message); exit(1); }
static void need(bool c,const char *m) { if(!c) runtime_fail(m); }
static size_t page_up(size_t x) { return (x+0x3fff)&~(size_t)0x3fff; }
static unsigned hash_name(const char *s) { unsigned h=2166136261u; for(;*s;s++)h=(h^(uint8_t)*s)*16777619u;return h; }
static void make_hash(Module *m) {
 unsigned cap=16;while(cap<m->nsym*2)cap<<=1;
 m->hash=calloc(cap,sizeof(unsigned));need(m->hash!=NULL,"symbol hash allocation");m->hash_mask=cap-1;
 for(unsigned i=1;i<m->nsym;i++)if(m->symbols[i].defined) {
  const char *s=m->symbols[i].name;unsigned p=hash_name(s)&m->hash_mask;
  while(m->hash[p]&&strcmp(s,m->symbols[m->hash[p]].name))p=(p+1)&m->hash_mask;
  if(!m->hash[p])m->hash[p]=i;
 }
}
static void *module_symbol(Module *m,const char *name) {
 unsigned p=hash_name(name)&m->hash_mask;
 while(m->hash[p]) {
  const Symbol *s=&m->symbols[m->hash[p]];
  if(!strcmp(name,s->name))return s->value<m->key_off?m->base+s->value:NULL;
  p=(p+1)&m->hash_mask;
 }
 return NULL;
}
void *runtime_symbol(const char *name) {
 for(unsigned i=0;i<NM;i++){void *p=module_symbol(&modules[i],name);if(p)return p;}
 return NULL;
}
uint8_t *runtime_module_base(const char *name){for(unsigned i=0;i<NM;i++)if(!strcmp(modules[i].name,name))return modules[i].base;return NULL;}
/* RELRO in newer SDKs protects relocated data after linking. Keep permissions at
 * the guest's 4 KiB granularity. A Darwin 16 KiB page receives the union of its
 * four guest permissions, so protecting an edge never removes access from an
 * adjacent writable object. Whole interior pages are actually made read-only. */
static pthread_mutex_t module_permission_lock=PTHREAD_MUTEX_INITIALIZER;
static unsigned char *module_permissions[NM];
static unsigned module_page_permission(unsigned i,uint64_t off){
 Module *m=&modules[i];
 if(off<m->data_off)return off<page_up(m->text_end)?5:1;
 if(off>=m->key_off)return off>=(uint64_t)m->patch_off?5:1;   /* a negative patch_off is never a guest offset */
 return module_permissions[i]?module_permissions[i][(off-m->data_off)/4096]:3;
}
uint64_t runtime_module_permission(uint64_t address,uint64_t size,unsigned permission){
 if(address&4095)return 0xcc01;
 if(!size||(size&4095))return 0xca01;
 if(permission!=0&&permission!=1&&permission!=3)return 0xd801;
 for(unsigned i=0;i<NM;i++){
  Module *m=&modules[i];uint64_t base=(uintptr_t)m->base;
  if(address<base||address-base<m->data_off||address-base>=m->key_off||size>m->key_off-(address-base))continue;
  uint64_t off=address-base;
  pthread_mutex_lock(&module_permission_lock);
  if(!module_permissions[i]){size_t count=(m->key_off-m->data_off+4095)/4096;module_permissions[i]=malloc(count);need(module_permissions[i]!=NULL,"module permission table");memset(module_permissions[i],3,count);}
  for(uint64_t p=0;p<size/4096;p++)if(module_permissions[i][(off-m->data_off)/4096+p]&16){pthread_mutex_unlock(&module_permission_lock);return 0xd401;}
  memset(module_permissions[i]+(off-m->data_off)/4096,permission,size/4096);
  uint64_t lo=off&~0x3fffULL,hi=page_up(off+size);
  for(uint64_t page=lo;page<hi;page+=0x4000){
   unsigned combined=0;for(unsigned p=0;p<4;p++)combined|=module_page_permission(i,page+p*4096);
   int prot=(combined&1?PROT_READ:0)|(combined&2?PROT_WRITE:0);
   need(!(combined&4),"module permission overlaps executable page");
   need(mprotect(m->base+page,0x4000,prot)==0,"module data permission");
  }
  pthread_mutex_unlock(&module_permission_lock);
  fprintf(stderr,"[totk] %s data permission +%#llx size=%#llx perm=%u\n",m->name,(unsigned long long)off,(unsigned long long)size,permission);
  return 0;
 }
 return 0xd401;
}
uint64_t runtime_module_lock_permission(uint64_t address,uint64_t size,unsigned mask,unsigned attribute){
 if(address&4095)return 0xcc01;
 if(!size||(size&4095))return 0xca01;
 /* The only module attribute used by the SDK is the irreversible RELRO lock. */
 if(mask!=16||attribute!=16)return 0xd801;
 for(unsigned i=0;i<NM;i++){
  Module *m=&modules[i];uint64_t start=(uintptr_t)m->base;
  if(address<start||address-start<m->data_off||address-start>=m->key_off||size>m->key_off-(address-start))continue;
  pthread_mutex_lock(&module_permission_lock);
  if(!module_permissions[i]){size_t count=(m->key_off-m->data_off+4095)/4096;module_permissions[i]=malloc(count);need(module_permissions[i]!=NULL,"module attribute table");memset(module_permissions[i],3,count);}
  size_t first=(address-start-m->data_off)/4096;
  for(uint64_t p=0;p<size/4096;p++)module_permissions[i][first+p]|=16;
  pthread_mutex_unlock(&module_permission_lock);return 0;
 }
 return 0xd401;
}
int runtime_module_query(uint64_t address,uint64_t *base,uint64_t *size,unsigned *state,unsigned *permission,unsigned *attribute){
 for(unsigned i=0;i<NM;i++){
  Module *m=&modules[i];uint64_t start=(uintptr_t)m->base;
  if(address<start||address-start>=m->key_off)continue;
  uint64_t off=(address-start)&~4095ULL,lo=off,hi=off+4096;
  pthread_mutex_lock(&module_permission_lock);
  unsigned perm=module_page_permission(i,off);int data=off>=m->data_off;
  uint64_t limit=data?m->data_off:0,end=data?m->key_off:m->data_off;
  while(lo>limit&&module_page_permission(i,lo-4096)==perm)lo-=4096;
  while(hi<end&&module_page_permission(i,hi)==perm)hi+=4096;
  pthread_mutex_unlock(&module_permission_lock);
  *base=start+lo;*size=hi-lo;*state=data?4:3;*permission=perm&7;*attribute=perm&16;return 1;
 }
 return 0;
}
void runtime_describe_address(uint64_t a,char *out,size_t n){for(unsigned i=0;i<NM;i++)if(a>=(uintptr_t)modules[i].base&&a<(uintptr_t)modules[i].base+modules[i].size){snprintf(out,n,"%s+0x%llx",modules[i].name,(unsigned long long)(a-(uintptr_t)modules[i].base));return;}Dl_info di;if(dladdr((void *)(uintptr_t)a,&di)&&di.dli_sname){snprintf(out,n,"%s+0x%llx",di.dli_sname,(unsigned long long)(a-(uintptr_t)di.dli_saddr));return;}snprintf(out,n,"0x%llx",(unsigned long long)a);}
/* Guest frame-pointer chain. Guest threads run on guest stacks (not the host pthread stack), so frames are read with
   mach_vm_read_overwrite (a bad pointer ends the walk instead of faulting); the chain must grow upwards. */
void runtime_trace_guest(uint64_t fp,uint64_t lr) {
 for(unsigned n=0;n<16;n++) {
  for(unsigned i=0;i<NM;i++)if(lr>=(uintptr_t)modules[i].base&&lr<(uintptr_t)modules[i].base+modules[i].size)fprintf(stderr,"[totk] caller %s+0x%llx\n",modules[i].name,(unsigned long long)(lr-(uintptr_t)modules[i].base));
  if(!fp||fp%8)break;uint64_t f[2];mach_vm_size_t got=0;
  if(mach_vm_read_overwrite(mach_task_self(),fp,16,(uintptr_t)f,&got)!=KERN_SUCCESS||got!=16)break;
  lr=f[1];if(f[0]<=fp||f[0]-fp>(64u<<20))break;fp=f[0];
 }
}
static void *required(const char *name) { void *p=runtime_symbol(name);if(!p){fprintf(stderr,"missing %s\n",name);runtime_fail("required symbol");}return p; }
#if TARGET_OS_IPHONE
/* iOS: code written into memory at run time cannot execute, so ios/tools/gen_embed.py compiles each rewritten image into
   this executable: the text (with the game patches already applied on the Mac, TOTK_EXPORT_IOS) and the patch area go
   into the r-x segment __TOTKX, everything between them (rodata, data, bss, key page) into the rw segment __TOTK<name>W.
   Here one contiguous range is reserved and the three parts are vm_remap'ed back to their original relative offsets:
   code pages shared with the signed executable (copy=FALSE, r-x), the data part copy-on-write. Main's extra private
   page after the image (real-time frame step) stays plain zero-filled memory from the reservation. */
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
#include "ios_embed.h"
static void remap_at(Module *m,uintptr_t to,const void *from,size_t size,boolean_t copy,vm_prot_t need_prot) {
 mach_vm_address_t a=to;vm_prot_t cur=0,max=0;
 kern_return_t kr=mach_vm_remap(mach_task_self(),&a,size,0,VM_FLAGS_FIXED|VM_FLAGS_OVERWRITE,mach_task_self(),(uintptr_t)from,copy,&cur,&max,VM_INHERIT_NONE);
 if(kr!=KERN_SUCCESS||a!=to){fprintf(stderr,"[totk] %s vm_remap %p -> %#lx (%#zx bytes) failed: %d\n",m->name,from,(unsigned long)to,size,kr);runtime_fail("remap embedded module");}
 need((cur&need_prot)==need_prot,"embedded module permissions after remap");
}
static void digest_hex(const void *p,size_t n,char hex[65]){unsigned char d[32];CC_SHA256(p,(CC_LONG)n,d);for(unsigned i=0;i<32;i++)snprintf(hex+i*2,3,"%02x",d[i]);}
static void map_module(Module *m,const char *dir) {
 (void)dir;
 const IosEmbed *e=NULL;for(unsigned i=0;i<sizeof(ios_embed)/sizeof(*ios_embed);i++)if(!strcmp(ios_embed[i].name,m->name))e=&ios_embed[i];
 need(e!=NULL,"module missing from ios_embed.h");
 char sx0[17],sx1[17],sw[17];snprintf(sx0,sizeof sx0,"__%sx0",m->name);snprintf(sx1,sizeof sx1,"__%sx1",m->name);snprintf(sw,sizeof sw,"__TOTK%sW",m->name);
 unsigned long z0=0,zw=0,z1=0;
 const uint8_t *x0=getsectiondata(&_mh_execute_header,"__TOTKX",sx0,&z0),*w=getsectiondata(&_mh_execute_header,sw,"__img",&zw),*x1=getsectiondata(&_mh_execute_header,"__TOTKX",sx1,&z1);
 need(x0&&w&&x1,"embedded module sections missing");
 need(z0==page_up(m->text_end)&&(uint64_t)m->patch_off==z0+zw&&(uint64_t)m->patch_off+z1==page_up(m->file_size),"embedded module layout");
 need(!((uintptr_t)x0&0x3fff)&&!((uintptr_t)w&0x3fff)&&!((uintptr_t)x1&0x3fff),"embedded sections not 16 KiB aligned");
 char hex[65];
 digest_hex(x0,z0,hex);need(!strcmp(hex,e->x0),"embedded text SHA256 differs from the exported patched text");
 digest_hex(w,zw,hex);need(!strcmp(hex,e->w),"embedded data SHA256 differs from verified rewrite");
 digest_hex(x1,z1,hex);need(!strcmp(hex,e->x1),"embedded patch area SHA256 differs from verified rewrite");
 m->size=page_up(m->file_size)+(!strcmp(m->name,"main")?0x4000:0);
 mach_vm_address_t base=0;need(mach_vm_allocate(mach_task_self(),&base,m->size,VM_FLAGS_ANYWHERE)==KERN_SUCCESS,"reserve module range");
 remap_at(m,base,x0,z0,FALSE,VM_PROT_READ|VM_PROT_EXECUTE);
 remap_at(m,base+z0,w,zw,TRUE,VM_PROT_READ|VM_PROT_WRITE);
 remap_at(m,base+m->patch_off,x1,z1,FALSE,VM_PROT_READ|VM_PROT_EXECUTE);
 m->base=(uint8_t *)(uintptr_t)base;
 need(m->key_off%0x4000==0&&(uint64_t)m->patch_off>m->key_off&&(uint64_t)m->patch_off<m->size,"key and patch layout");
 uint64_t *key=(void *)(m->base+m->key_off);
 for(unsigned i=0;i<8;i++)key[i]=runtime_keys[i]*8;
 make_hash(m);
 fprintf(stderr,"[totk] mapped embedded %s at %p size=%zu symbols=%u\n",m->name,m->base,m->size,m->nsym);
}
#else
static void map_module(Module *m,const char *dir) {
 char path[4096];need(snprintf(path,sizeof(path),"%s/%s.patched.bin",dir,m->name)<(int)sizeof(path),"image path");
 FILE *f=fopen(path,"rb");need(f!=NULL,"open patched module");
 struct stat st;need(fstat(fileno(f),&st)==0&&(size_t)st.st_size==m->file_size,"patched module size");
 /* The patch area is its own file, mapped at the signed patch_off relative to the module base: below the
    base when the image span exceeds a B's reach (main), appended after the image otherwise (sdk). */
 char ppath[4096];need(snprintf(ppath,sizeof(ppath),"%s/%s.patcharea.bin",dir,m->name)<(int)sizeof(ppath),"patch area path");
 FILE *pf=fopen(ppath,"rb");need(pf!=NULL,"open patch area");
 struct stat pst;need(fstat(fileno(pf),&pst)==0&&(size_t)pst.st_size==m->patch_size,"patch area size");
 /* The main module gets one private, writable page after its rewritten image. The
    real-time frame-step patch reads its two floats from this page via ADRP. */
 size_t arena=m->patch_off<0?(size_t)(-(intptr_t)m->patch_off):0;
 intptr_t patch_end_s=m->patch_off+(intptr_t)m->patch_size;   /* guest-relative, negative inside the arena */
 size_t patch_end=patch_end_s>0?(size_t)patch_end_s:0;
 if(patch_end<m->file_size)patch_end=m->file_size;
 m->size=page_up(patch_end)+(!strcmp(m->name,"main")?0x4000:0);
 void *region=mmap(NULL,arena+m->size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
 need(region!=MAP_FAILED&&region!=NULL,"map module");
 m->base=(uint8_t*)region+arena;
 need(fread(m->base,1,m->file_size,f)==m->file_size,"read image");fclose(f);
 unsigned char digest[32];CC_SHA256(m->base,(CC_LONG)m->file_size,digest);char hex[65];
 for(unsigned i=0;i<32;i++)snprintf(hex+i*2,3,"%02x",digest[i]);
 need(!strcmp(hex,m->digest),"image SHA256 differs from verified rewrite");
 need(fread(m->base+m->patch_off,1,m->patch_size,pf)==m->patch_size,"read patch area");fclose(pf);
 CC_SHA256(m->base+m->patch_off,(CC_LONG)m->patch_size,digest);
 for(unsigned i=0;i<32;i++)snprintf(hex+i*2,3,"%02x",digest[i]);
 need(!strcmp(hex,m->patch_digest),"patch area SHA256 differs from verified rewrite");
 need(m->key_off%0x4000==0&&m->key_off+0x4000<=m->file_size,"key page layout");
 uint64_t *key=(void *)(m->base+m->key_off);
 for(unsigned i=0;i<8;i++)key[i]=runtime_keys[i]*8;
 make_hash(m);
 fprintf(stderr,"[totk] mapped %s size=%zu patch=%lld+%zu symbols=%u\n",m->name,m->size,(long long)m->patch_off,m->patch_size,m->nsym);
}
/* TOTK_EXPORT_IOS=<dir>: after the game patches, write each module's text [0, page_up(TEXT_END)) for ios/tools/gen_embed.py.
   Relocations never reach the text (they start at DATA_OFF), so the exported bytes differ from the verified image only at
   the patch sites; the count is printed and checked by the generator. */
static void export_ios_text(const char *rewritten,const char *out) {
 for(unsigned i=0;i<NM;i++){Module *m=&modules[i];char path[4096];size_t n=page_up(m->text_end);
  need(snprintf(path,sizeof path,"%s/%s.patched.bin",rewritten,m->name)<(int)sizeof path,"image path");
  FILE *f=fopen(path,"rb");need(f!=NULL,"open patched module");uint8_t *orig=malloc(n);need(orig&&fread(orig,1,n,f)==n,"read original text");fclose(f);
  unsigned diff=0;for(size_t k=0;k<n;k+=4)if(memcmp(orig+k,m->base+k,4))diff++;free(orig);
  need(snprintf(path,sizeof path,"%s/%s.x0.bin",out,m->name)<(int)sizeof path,"export path");
  f=fopen(path,"wb");need(f&&fwrite(m->base,1,n,f)==n&&fclose(f)==0,"write exported text");
  printf("%s text_bytes=%zu patched_words=%u\n",m->name,n,diff);}
}
#endif
static void relocate(Module *m) {
 for(unsigned i=0;i<m->nreloc;i++) {
  const Reloc *r=&m->relocs[i];
  need(r->where>=m->data_off&&r->where+8<=m->key_off&&r->symbol<m->nsym,"relocation bounds");
  uintptr_t v;
  if(r->kind==1027) { need(r->addend>=0&&(uint64_t)r->addend<m->key_off,"relative addend");v=(uintptr_t)m->base+r->addend;m->relative++; }
  else {
   need(r->kind==257||r->kind==1025||r->kind==1026,"relocation kind");
   const Symbol *s=&m->symbols[r->symbol];void *p=r->kind==1026?runtime_observer(s->name):NULL;
   if(!p)p=runtime_override(s->name);
   if(p)p=runtime_hle_wrap_named(p,s->name);   /* host replacements run on the thread's host stack */
   if(!p)p=s->defined?(s->value<m->key_off?m->base+s->value:NULL):runtime_symbol(s->name);
   if(!p&&s->binding!=2){fprintf(stderr,"%s unresolved %s\n",m->name,s->name);runtime_fail("strong import unresolved");}
   if(!p)m->weak++;
   if(p&&!s->defined)m->cross++;
   v=(uintptr_t)p+(r->kind==1026?0:r->addend);m->symbolic++;
  }
  memcpy(m->base+r->where,&v,8);
 }
}
static void verify_identity(int negative) {
 unsigned checked=0;void *original=required("nnMain");
 for(unsigned i=0;i<NM;i++)for(unsigned j=0;j<modules[i].nreloc;j++) {
  Module *m=&modules[i];const Reloc *r=&m->relocs[j];
  if(r->kind==1027||r->kind==1026||strcmp(m->symbols[r->symbol].name,"nnMain"))continue;
  uintptr_t actual;memcpy(&actual,m->base+r->where,8);
  if(negative){actual=(uintptr_t)runtime_observer("nnMain");memcpy(m->base+r->where,&actual,8);}
  need(actual==(uintptr_t)original,"guest nnMain address identity changed");checked++;
 }
 need(checked>0,"nnMain identity reference missing");
}
static void seal(Module *m) {
 size_t rx=page_up(m->text_end),ro_end=m->data_off&~(size_t)0x3fff;
 need(rx<=ro_end,"text/data overlap");
#if TARGET_OS_IPHONE
 if(ro_end>rx)need(mprotect(m->base+rx,ro_end-rx,PROT_READ)==0,"rodata R");   /* code parts are already r-x shared pages */
 need(mprotect(m->base+m->key_off,0x4000,PROT_READ)==0,"key page R");
 return;
#endif
 need(mprotect(m->base,rx,PROT_READ|PROT_EXEC)==0,"text RX");
 if(ro_end>rx)need(mprotect(m->base+rx,ro_end-rx,PROT_READ)==0,"rodata R");
 need(mprotect(m->base+m->key_off,0x4000,PROT_READ)==0,"key page R");
 /* The patch area (signed offset) becomes executable; the icache flush covers it. */
 uint8_t *plo=m->base+m->patch_off,*phi=(uint8_t*)(((uintptr_t)m->base+m->patch_off+m->patch_size+0x3fff)&~(uintptr_t)0x3fff);
 need(mprotect(plo,phi-plo,PROT_READ|PROT_EXEC)==0,"patch RX");
 sys_icache_invalidate(m->base,rx);sys_icache_invalidate(plo,phi-plo);
}
static void check_permissions(Module *m) {
 mach_vm_address_t a=(uintptr_t)m->base,end=a+m->size;
 while(a<end) {
  mach_vm_size_t size=0;vm_region_basic_info_data_64_t info;mach_msg_type_number_t count=VM_REGION_BASIC_INFO_COUNT_64;mach_port_t obj=0;
  mach_vm_address_t requested=a;
  need(mach_vm_region(mach_task_self(),&a,&size,VM_REGION_BASIC_INFO_64,(vm_region_info_t)&info,&count,&obj)==KERN_SUCCESS&&a==requested&&size>0,"mapped region query");
  if(obj)mach_port_deallocate(mach_task_self(),obj);
  uint64_t off=a-(uintptr_t)m->base;
  vm_prot_t expected=(off>=(uint64_t)m->patch_off&&off<(uint64_t)m->patch_off+m->patch_size)?(VM_PROT_READ|VM_PROT_EXECUTE):
   off>=page_up(m->file_size)?(VM_PROT_READ|VM_PROT_WRITE):
   off<page_up(m->text_end)?(VM_PROT_READ|VM_PROT_EXECUTE):
   ((off>=page_up(m->text_end)&&off<(m->data_off&~0x3fffULL))||off==m->key_off)?VM_PROT_READ:(VM_PROT_READ|VM_PROT_WRITE);
  if(info.protection!=expected){fprintf(stderr,"[totk] %s: off=%#llx prot=%x expected=%llx\n",m->name,(unsigned long long)off,info.protection,(unsigned long long)expected);runtime_fail("module permission mismatch");}a+=size;
 }
}
typedef struct {void *tls,*tlr,*dummy,*signal_stack;} ThreadEnvironment;
static pthread_key_t environment_key;
#define SIGNAL_STACK_SIZE (256u<<10)
static void environment_destroy(void *p) {
 ThreadEnvironment *e=p;if(!e)return;
 if(e->signal_stack){stack_t off={.ss_flags=SS_DISABLE};sigaltstack(&off,NULL);free(e->signal_stack);}
 free(e->dummy);free(e->tlr);free(e->tls);free(e);
}
void runtime_thread_init(void) {
 need(!pthread_getspecific(environment_key),"duplicate guest thread environment");
 ThreadEnvironment *e=calloc(1,sizeof(*e));need(e!=NULL,"thread environment");
 e->tls=calloc(1,8192);e->tlr=calloc(1,0x200);e->dummy=calloc(1,0x400);
 need(e->tls&&e->tlr&&e->dummy,"thread environment allocation");
 /* Faults and emulated instructions are handled off the guest stack, which can be as small as 4 KiB. */
 e->signal_stack=malloc(SIGNAL_STACK_SIZE);need(e->signal_stack!=NULL,"signal stack allocation");
 stack_t ss={.ss_sp=e->signal_stack,.ss_size=SIGNAL_STACK_SIZE,.ss_flags=0};need(sigaltstack(&ss,NULL)==0,"install signal stack");memcpy((uint8_t *)e->tlr+0x1f8,&e->dummy,8);
 need(pthread_setspecific(environment_key,e)==0&&pthread_setspecific(runtime_keys[0],NULL)==0,"thread ownership");
 need(pthread_setspecific(runtime_keys[1],e->tls)==0&&pthread_setspecific(runtime_keys[2],e->tlr)==0&&pthread_setspecific(runtime_keys[3],(void *)runtime_gate)==0,"thread slots");
 uintptr_t tp;__asm__ volatile("mrs %0,tpidrro_el0":"=r"(tp));
 need((tp&7)==0,"unaligned raw Darwin TSD incompatible with patches");
 for(unsigned i=0;i<4;i++){void *raw;memcpy(&raw,(void *)(tp+runtime_keys[i]*8),8);need(raw==pthread_getspecific(runtime_keys[i]),"Darwin TSD layout");}
}
static void thread_cleanup(void) {
 ThreadEnvironment *e=pthread_getspecific(environment_key);pthread_setspecific(environment_key,NULL);
 for(unsigned i=0;i<4;i++)pthread_setspecific(runtime_keys[i],NULL);environment_destroy(e);
}
uint64_t runtime_now_ns(void) {mach_timebase_info_data_t tb;mach_timebase_info(&tb);return (uint64_t)((__uint128_t)mach_absolute_time()*tb.numer/tb.denom);}
void runtime_dispatch(uint64_t ret,uint64_t *regs) {
 for(unsigned i=0;i<NM;i++)for(unsigned j=0;j<modules[i].ngate;j++) {
  const Gate *g=&modules[i].gates[j];if((uintptr_t)modules[i].base+g->ret!=ret)continue;
  if(g->timer){mach_timebase_info_data_t tb;mach_timebase_info(&tb);__uint128_t ns=(__uint128_t)mach_absolute_time()*tb.numer/tb.denom;
   need(g->arg<31&&g->arg!=18,"timer target register");regs[g->arg]=(uint64_t)(ns*12/625);__atomic_fetch_add(&timer_calls,1,__ATOMIC_RELAXED);return;}
  __atomic_fetch_add(&svc_calls,1,__ATOMIC_RELAXED);runtime_svc(g->arg,regs,modules[i].name,g->original);return;
 }
 runtime_fail("unknown gate return address");
}
/* Signal diagnostics use only stack buffers + write; no guest continuation. */
__thread sigjmp_buf *runtime_fault_guard;
static void signal_hex(const char *label,uint64_t value) {
 char b[128];size_t n=0;while(*label)b[n++]=*label++;b[n++]='0';b[n++]='x';
 for(int i=15;i>=0;i--)b[n++]="0123456789abcdef"[(value>>(i*4))&15];b[n++]='\n';write(2,b,n);
}
/* CTR_EL0 for 128-byte host cache lines: DminLine=IminLine=5 (32 words), ERG=CWG=5, PIPT I-cache, IDC/DIC clear. */
#define HOST_CTR_EL0 0x8555c005ULL
static void crash(int num,siginfo_t *info,void *context) {
 ucontext_t *u=context;
 /* (The TotK fault workaround for an invalid optional menu reference is TotK-only and not carried over.) */
 /* EL0 cache maintenance that Darwin traps but Horizon allows: emulate exactly two encodings (SDK only). */
 if(num==SIGILL){uint32_t insn=*(const uint32_t *)(uintptr_t)u->uc_mcontext->__ss.__pc;unsigned rt=insn&31;
  if((insn&~31u)==0xd53b0020u){if(rt!=31)u->uc_mcontext->__ss.__x[rt]=HOST_CTR_EL0;u->uc_mcontext->__ss.__pc+=4;return;}   /* mrs Xt, CTR_EL0 */
  if((insn&~31u)==0xd50b7e20u){uintptr_t a=rt==31?0:(uintptr_t)u->uc_mcontext->__ss.__x[rt];sys_dcache_flush((void *)(a&~(uintptr_t)127),128);u->uc_mcontext->__ss.__pc+=4;return;}   /* dc civac, Xt */
 }
 if((num==SIGBUS||num==SIGSEGV)&&runtime_fault_guard){sigjmp_buf *g=runtime_fault_guard;runtime_fault_guard=NULL;siglongjmp(*g,1);}   /* guarded host read of guest memory (uniform snapshot copies) */
 if(runtime_gfx_fault(info->si_addr,(u->uc_mcontext->__es.__esr>>6)&1,context))return;uint64_t pc=u->uc_mcontext->__ss.__pc,fp=u->uc_mcontext->__ss.__fp;
 signal_hex("[totk] signal=",num);signal_hex("fault=",(uintptr_t)info->si_addr);signal_hex("esr=",u->uc_mcontext->__es.__esr);
 {mach_vm_address_t ra=(uintptr_t)info->si_addr;mach_vm_size_t rs=0;vm_region_basic_info_data_64_t ri;mach_msg_type_number_t rc=VM_REGION_BASIC_INFO_COUNT_64;mach_port_t ro=0;
  if(mach_vm_region(mach_task_self(),&ra,&rs,VM_REGION_BASIC_INFO_64,(vm_region_info_t)&ri,&rc,&ro)==KERN_SUCCESS){signal_hex("region=",ra);signal_hex("region size=",rs);signal_hex("region prot=",(uint64_t)ri.protection|((uint64_t)ri.max_protection<<8));}}signal_hex("pc=",pc);signal_hex("lr=",u->uc_mcontext->__ss.__lr);signal_hex("fp=",fp);
 for(unsigned i=0;i<NM;i++)if(pc>=(uintptr_t)modules[i].base&&pc<(uintptr_t)modules[i].base+modules[i].size){write(2,modules[i].name,strlen(modules[i].name));signal_hex(" offset=",pc-(uintptr_t)modules[i].base);}
 for(unsigned i=0;i<29;i++)signal_hex("x=",u->uc_mcontext->__ss.__x[i]);signal_hex("sp=",u->uc_mcontext->__ss.__sp);
 for(unsigned depth=0;depth<24&&fp&&!(fp&15);depth++){uint64_t frame[2];mach_vm_size_t got=0;if(mach_vm_read_overwrite(mach_task_self(),fp,16,(uintptr_t)frame,&got)!=KERN_SUCCESS||got!=16)break;signal_hex("frame lr=",frame[1]);for(unsigned i=0;i<NM;i++)if(frame[1]>=(uintptr_t)modules[i].base&&frame[1]<(uintptr_t)modules[i].base+modules[i].size){write(2,modules[i].name,strlen(modules[i].name));signal_hex(" offset=",frame[1]-(uintptr_t)modules[i].base);}if(frame[0]==fp)break;fp=frame[0];   /* host-stack switch: frames may move between stacks */}
 /* TOTK_CRASH_MEM=<reg>[:<signed offset>[:<bytes>]]: dump words at register+offset; TOTK_CRASH_MEM_FOLLOW=k1,k2,...: for each
    listed word index, dump 0x100 bytes at that pointer and the ASCII of every readable pointer inside it (data-structure faults). */
 {const char *e=getenv("TOTK_CRASH_MEM");if(e){unsigned r=(unsigned)strtoul(e,NULL,0);const char *c=strchr(e,':');int64_t off=c?strtoll(c+1,NULL,0):0;const char *c2=c?strchr(c+1,':'):NULL;
  uint64_t len=c2?strtoull(c2+1,NULL,0):0x100;if(len>0x1000)len=0x1000;if(r<29){uint64_t a=u->uc_mcontext->__ss.__x[r]+(uint64_t)off,w[512];mach_vm_size_t got=0;signal_hex("mem at=",a);
  if(mach_vm_read_overwrite(mach_task_self(),a,len,(uintptr_t)w,&got)==KERN_SUCCESS&&got==len){unsigned nw=(unsigned)(len/8);for(unsigned i=0;i<nw;i++){signal_hex("m=",w[i]);}
   const char *f=getenv("TOTK_CRASH_MEM_FOLLOW");while(f&&*f){unsigned k=(unsigned)strtoul(f,NULL,0);uint64_t v[32];if(k<nw&&mach_vm_read_overwrite(mach_task_self(),w[k],sizeof(v),(uintptr_t)v,&got)==KERN_SUCCESS&&got==sizeof(v)){
     signal_hex("follow word=",k);signal_hex("follow at=",w[k]);for(unsigned i=0;i<32;i++){signal_hex("f=",v[i]);
      if(v[i]>=0x100000000ULL&&v[i]<0x800000000000ULL){char t[128];mach_vm_size_t g2=0;if(mach_vm_read_overwrite(mach_task_self(),v[i],120,(uintptr_t)t,&g2)==KERN_SUCCESS&&g2==120){
       unsigned printable=0;for(unsigned j=0;j<120&&t[j];j++)if(t[j]>=0x20&&t[j]<0x7f)printable++;else break;
       if(printable>=3){char b[160];size_t n=0;const char *lab="  str=";while(*lab)b[n++]=*lab++;for(unsigned j=0;j<printable&&j<120;j++)b[n++]=t[j];b[n++]='\n';write(2,b,n);}}}}}
    const char *nx=strchr(f,',');f=nx?nx+1:NULL;}}}}}
 {const char *hw=getenv("TOTK_CRASH_HEAPWALK");if(hw){unsigned r=(unsigned)strtoul(hw,NULL,0);const char *c=strchr(hw,':');int64_t off=c?strtoll(c+1,NULL,0):0;mach_vm_size_t got=0;uint64_t hp=0,hh[32];
  if(r<29&&mach_vm_read_overwrite(mach_task_self(),u->uc_mcontext->__ss.__x[r]+(uint64_t)off,8,(uintptr_t)&hp,&got)==KERN_SUCCESS&&got==8&&mach_vm_read_overwrite(mach_task_self(),hp,sizeof(hh),(uintptr_t)hh,&got)==KERN_SUCCESS&&got==sizeof(hh)){
   uint64_t start=hh[5],size=hh[6];signal_hex("heapwalk heap=",hp);signal_hex("heapwalk start=",start);signal_hex("heapwalk size=",size);
   if(start==hp&&size<0x40000000ULL){uint64_t sentinel=hp+0xc8,p=0;unsigned count=0;   /* sead::ExpHeap use list: OffsetList node at +0xc8, blocks linked by their 16-byte ListNode */
    if(mach_vm_read_overwrite(mach_task_self(),sentinel+8,8,(uintptr_t)&p,&got)!=KERN_SUCCESS)p=0;
    while(p&&p!=sentinel&&count<20000){uint64_t h[4],d0=0;if(mach_vm_read_overwrite(mach_task_self(),p,32,(uintptr_t)h,&got)!=KERN_SUCCESS||got!=32)break;
     mach_vm_read_overwrite(mach_task_self(),p+0x20,8,(uintptr_t)&d0,&got);
     char b[200];size_t n=0;const char *lab="blk off=";while(*lab)b[n++]=*lab++;
     uint64_t vals[4]={p-start,h[3],h[2],d0};const char *labs[4]={""," size="," attr="," data0="};
     for(unsigned k=0;k<4;k++){const char *l=labs[k];while(*l)b[n++]=*l++;b[n++]='0';b[n++]='x';for(int i=15;i>=0;i--)b[n++]="0123456789abcdef"[(vals[k]>>(i*4))&15];}
     b[n++]='\n';write(2,b,n);p=h[1];count++;}}}}}
 /* TOTK_CRASH_HEAPFIND=<reg>: locate the sead::Heap (self pointer at +0x28, size at +0x30) whose range contains the register's
    address by scanning backwards page by page (resource heaps are page-aligned in the alias region), then walk and dump it. */
 {const char *hf=getenv("TOTK_CRASH_HEAPFIND");if(hf){unsigned r=(unsigned)strtoul(hf,NULL,0);mach_vm_size_t got=0;uint64_t a=r<29?u->uc_mcontext->__ss.__x[r]:0,found=0;
  for(uint64_t pg=a&~0xfffULL;pg&&a-pg<(256ULL<<20);pg-=0x1000){uint64_t hh[8];if(mach_vm_read_overwrite(mach_task_self(),pg,sizeof hh,(uintptr_t)hh,&got)!=KERN_SUCCESS||got!=sizeof hh)continue;
   if(hh[5]==pg&&hh[6]>=0x1000&&hh[6]<0x40000000ULL&&a<pg+hh[6]){found=pg;break;}}
  signal_hex("heapfind reg value=",a);signal_hex("heapfind heap=",found);
  if(found){uint64_t hh[32];if(mach_vm_read_overwrite(mach_task_self(),found,sizeof hh,(uintptr_t)hh,&got)==KERN_SUCCESS&&got==sizeof hh){
   char t[128];mach_vm_size_t g2=0;if(hh[4]>=0x100000000ULL&&mach_vm_read_overwrite(mach_task_self(),hh[4],120,(uintptr_t)t,&g2)==KERN_SUCCESS){unsigned pr=0;for(unsigned j=0;j<120&&t[j];j++)if(t[j]>=0x20&&t[j]<0x7f)pr++;else break;char b[160];size_t n=0;const char *lab="heapfind name=";while(*lab)b[n++]=*lab++;for(unsigned j=0;j<pr;j++)b[n++]=t[j];b[n++]='\n';write(2,b,n);}
   signal_hex("heapfind size=",hh[6]);signal_hex("heapfind freelist head=",hh[22]);signal_hex("heapfind free count=",hh[24]&0xffffffff);
   uint64_t sentinel=found+0xc8,p=0;unsigned count=0;if(mach_vm_read_overwrite(mach_task_self(),sentinel+8,8,(uintptr_t)&p,&got)!=KERN_SUCCESS)p=0;
   while(p&&p!=sentinel&&count<20000){uint64_t h[4],d0=0;if(mach_vm_read_overwrite(mach_task_self(),p,32,(uintptr_t)h,&got)!=KERN_SUCCESS||got!=32)break;mach_vm_read_overwrite(mach_task_self(),p+0x20,8,(uintptr_t)&d0,&got);
    char b[200];size_t n=0;const char *lab="blk off=";while(*lab)b[n++]=*lab++;uint64_t vals[4]={p-found,h[3],h[2],d0};const char *labs[4]={""," size="," attr="," data0="};
    for(unsigned k=0;k<4;k++){const char *l=labs[k];while(*l)b[n++]=*l++;b[n++]='0';b[n++]='x';for(int i=15;i>=0;i--)b[n++]="0123456789abcdef"[(vals[k]>>(i*4))&15];}b[n++]='\n';write(2,b,n);p=h[1];count++;}
   const char *dp=getenv("TOTK_CRASH_HEAPDUMP");if(dp){int fd=open(dp,O_WRONLY|O_CREAT|O_TRUNC,0644);if(fd>=0){uint64_t done=0;static char buf2[65536];while(done<hh[6]){uint64_t n=hh[6]-done;if(n>sizeof buf2)n=sizeof buf2;
    if(mach_vm_read_overwrite(mach_task_self(),found+done,n,(uintptr_t)buf2,&got)!=KERN_SUCCESS||got!=n)break;if(write(fd,buf2,(size_t)n)!=(ssize_t)n)break;done+=n;}close(fd);signal_hex("heapdump bytes=",done);}}}}}}
 /* TOTK_CRASH_HEAPDUMP=<path>: also write the walked heap's bytes (same heap as TOTK_CRASH_HEAPWALK) to a file for offline comparison. */
 {const char *hw=getenv("TOTK_CRASH_HEAPWALK"),*dp=getenv("TOTK_CRASH_HEAPDUMP");if(hw&&dp){unsigned r=(unsigned)strtoul(hw,NULL,0);const char *c=strchr(hw,':');int64_t off=c?strtoll(c+1,NULL,0):0;mach_vm_size_t got=0;uint64_t hp=0,hh[8];
  if(r<29&&mach_vm_read_overwrite(mach_task_self(),u->uc_mcontext->__ss.__x[r]+(uint64_t)off,8,(uintptr_t)&hp,&got)==KERN_SUCCESS&&got==8&&mach_vm_read_overwrite(mach_task_self(),hp,sizeof(hh),(uintptr_t)hh,&got)==KERN_SUCCESS&&got==sizeof(hh)&&hh[5]==hp&&hh[6]<0x40000000ULL){
   int fd=open(dp,O_WRONLY|O_CREAT|O_TRUNC,0644);if(fd>=0){uint64_t done=0;static char buf[65536];while(done<hh[6]){uint64_t n=hh[6]-done;if(n>sizeof buf)n=sizeof buf;
    if(mach_vm_read_overwrite(mach_task_self(),hp+done,n,(uintptr_t)buf,&got)!=KERN_SUCCESS||got!=n)break;if(write(fd,buf,(size_t)n)!=(ssize_t)n)break;done+=n;}close(fd);signal_hex("heapdump bytes=",done);}}}}
 _exit(128+num);
}
static void *probe_thread(void *unused) {
 (void)unused;runtime_thread_init();
 typedef uint64_t (*Fn)(void);Fn fn=(Fn)(void *)(modules[0].base+PROBE_PLT);uint64_t previous=0;
 for(unsigned i=0;i<1000;i++){uint64_t current=fn();need(current>=previous&&current>0,"cross-module clock result");previous=current;}
 thread_cleanup();return NULL;
}
static void probe(void) {
 uintptr_t slot;memcpy(&slot,modules[0].base+PROBE_SLOT,8);
 need(slot==(uintptr_t)required("_ZN2nn2os13GetSystemTickEv"),"main PLT GOT resolution");
 pthread_t threads[4];for(unsigned i=0;i<4;i++)need(pthread_create(&threads[i],NULL,probe_thread,NULL)==0,"probe thread");
 for(unsigned i=0;i<4;i++)need(pthread_join(threads[i],NULL)==0,"join probe");need(timer_calls==4000,"timer gate count");
 printf("{\"status\":\"pass\",\"execution\":\"native arm64 main PLT -> sdk -> host timer -> return\",\"calls\":4000,\"threads\":4,\"modules\":[");
 for(unsigned i=0;i<NM;i++){Module *m=&modules[i];printf("%s{\"name\":\"%s\",\"relative\":%u,\"symbolic\":%u,\"cross\":%u,\"weak_zero\":%u}",i?",":"",m->name,m->relative,m->symbolic,m->cross,m->weak);}
 puts("],\"scope\":\"cross-module calls; constructors and game entry not run\"}");
}
int runtime_module_offset(uint64_t pc,const char **name,uint64_t *off) {
 for(unsigned i=0;i<NM;i++)if(pc>=(uintptr_t)modules[i].base&&pc<(uintptr_t)modules[i].base+modules[i].size){*name=modules[i].name;*off=pc-(uintptr_t)modules[i].base;return 1;}
 return 0;
}
uintptr_t runtime_frame_controller_return_pc(void) {
 return 0; /* The graphics diagnostic reads TotK's frame controller; Astral Chain has none. */
}
uint64_t runtime_image_bytes(void) { uint64_t total=0;for(unsigned i=0;i<NM;i++)total+=modules[i].key_off;return page_up(total); }
static void loader_ready(void) { fprintf(stderr,"[totk] rtld callback: modules linked\n"); }
static void module_inits(void) {
 fprintf(stderr,"[totk] SDK startup returned; running module constructors\n");
 for(unsigned i=0;i<NM;i++){char detail[128];snprintf(detail,sizeof detail,TOTK_UI("运行 %s 游戏模块初始化代码（%u/%u）","Running the %s game module initializers (%u/%u)"),modules[i].name,i+1,(unsigned)NM);runtime_boot_status(TOTK_UI("初始化游戏模块","Initializing game modules"),detail);fprintf(stderr,"[totk] ENTER %s DT_INIT\n",modules[i].name);((void(*)(void))(modules[i].base+modules[i].init))();uint32_t state;memcpy(&state,modules[i].base+modules[i].ctor_flag,4);need(state==2,"DT_INIT deferred or skipped constructors");
 fprintf(stderr,"[totk] RETURN %s DT_INIT state=%u init_array_count=%llu\n",modules[i].name,state,(unsigned long long)(modules[i].init_size/8));}
}
void runtime_startup_checkpoint(uintptr_t caller) {
 if(!getenv("TOTK_VERIFY_STARTUP"))return;

 for(unsigned i=0;i<NM;i++){uint32_t state;memcpy(&state,modules[i].base+modules[i].ctor_flag,4);need(state==2,"constructor checkpoint state");}
 printf("{\"status\":\"pass\",\"phase\":4,\"caller_main_offset\":\"0x%llx\",\"scope\":\"all DT_INIT constructor loops completed; original nnMain prefix executed; stops at focus setup\"}\n",(unsigned long long)(caller-(uintptr_t)modules[0].base));fflush(stdout);_exit(0);
}
static void module_finis(void) { runtime_fail("guest exited: module finalizers not yet implemented"); }
static void boot(void) {
 fprintf(stderr,"[totk] game version %s\n", TOTK_GAME_VERSION);
 runtime_boot_status(TOTK_UI("启动游戏运行环境","Starting the game runtime"),TOTK_UI("连接游戏代码与 macOS 系统服务","Connecting the game code to macOS system services"));
 runtime_thread_init();uint32_t main_handle=svc_thread_main_init();
 fprintf(stderr,"[totk] ENTER original nn::init::Start sdk+0xfa060\n");
 /* TotK/by3's SDK (13+) passes three callbacks; Astral Chain's older SDK passes two (no finalizer). */
 {void *s4=runtime_symbol("_ZN2nn4init5StartEmmPFvvES2_");
  if(s4)((void(*)(uint64_t,uint64_t,void(*)(void),void(*)(void)))s4)(main_handle,0,loader_ready,module_inits);
  else ((void(*)(uint64_t,uint64_t,void(*)(void),void(*)(void),void(*)(void)))required("_ZN2nn4init5StartEmmPFvvES2_S2_"))(main_handle,0,loader_ready,module_inits,module_finis);}
 runtime_fail("SDK Start unexpectedly returned");
}
/* Host aborts (e.g. Metal validation assertions) print the host call stack before the process ends. */
#include <execinfo.h>
static void abort_report(int num) {
 (void)num;void *frames[64];int n=backtrace(frames,64);write(2,"[totk] host abort, call stack:\n",31);backtrace_symbols_fd(frames,n,2);
 signal(SIGABRT,SIG_DFL);raise(SIGABRT);
}
int main(int argc,char **argv) {
 need(argc==3,"usage: totk-native rewritten-dir probe|boot");
 /* One game or GPU probe at a time (memory, GPU time): wait for the slot before anything is loaded. */
 if(!strcmp(argv[2],"boot")||!strncmp(argv[2],"probe-gpu",9))runtime_game_slot_acquire(argv[2]);
 if(strcmp(argv[2],"boot"))setenv("TOTK_PROBE_GPU","1",1);   /* standalone probes submit GPU work; the game does not unless TOTK_GPU_ALLOW_SUBMIT=1 */
 else unsetenv("TOTK_PROBE_GPU");need(sysconf(_SC_PAGESIZE)==0x4000,"expected 16KB Apple pages");
 struct sigaction a={0};a.sa_sigaction=crash;a.sa_flags=SA_SIGINFO|SA_ONSTACK;sigemptyset(&a.sa_mask);
 sigaction(SIGSEGV,&a,NULL);sigaction(SIGBUS,&a,NULL);sigaction(SIGILL,&a,NULL);sigaction(SIGTRAP,&a,NULL);
 {struct sigaction ab={0};ab.sa_handler=abort_report;ab.sa_flags=SA_ONSTACK;sigemptyset(&ab.sa_mask);sigaction(SIGABRT,&ab,NULL);}
 for(unsigned i=0;i<8;i++)need(pthread_key_create(&runtime_keys[i],NULL)==0&&runtime_keys[i]!=0,"allocate Darwin key");
 runtime_hle_stack_init((uint64_t)runtime_keys[4]*8);
 need(pthread_key_create(&environment_key,environment_destroy)==0,"thread environment key");
 for(unsigned i=0;i<NM;i++)map_module(&modules[i],argv[1]);
 for(unsigned i=0;i<NM;i++)relocate(&modules[i]);
 for(unsigned i=0;i<NM;i++)if(!strcmp(modules[i].name,"main"))runtime_game_patches(&modules[i]);
#if !TARGET_OS_IPHONE
 if(!strcmp(argv[2],"export-ios")){const char *out=getenv("TOTK_EXPORT_IOS");need(out!=NULL,"export-ios needs TOTK_EXPORT_IOS=<dir>");export_ios_text(argv[1],out);return 0;}
#endif
 verify_identity(!strcmp(argv[2],"probe-identity-negative"));
 for(unsigned i=0;i<NM;i++){seal(&modules[i]);check_permissions(&modules[i]);}
 if(!strcmp(argv[2],"probe-gpu-pool-retire")||!strcmp(argv[2],"probe-gpu-tex-slots")||!strcmp(argv[2],"probe-gpu-arg-epoch")||!strcmp(argv[2],"probe-gpu-anomaly")||!strcmp(argv[2],"probe-gpu-vertex-streams")||!strcmp(argv[2],"probe-gpu-layer-merge")||!strcmp(argv[2],"probe-gpu-cube-merge")||!strcmp(argv[2],"probe-gpu-texture-retire")||!strcmp(argv[2],"probe-gpu-linear-rt")||!strcmp(argv[2],"probe-gpu-storage-view")||!strcmp(argv[2],"probe-gpu-compressed-alias")||!strcmp(argv[2],"probe-gpu-upload-batch")||!strcmp(argv[2],"probe-gpu-ds-clear")||!strcmp(argv[2],"probe-gpu-ub-rename")||!strcmp(argv[2],"probe-gpu-ubo-snapshot")||!strcmp(argv[2],"probe-gpu-drv-c0")||!strcmp(argv[2],"probe-gpu-dispatch-chain")||!strcmp(argv[2],"probe-gpu-copy-alias")||!strcmp(argv[2],"probe-gpu-srgb-view")||!strcmp(argv[2],"probe-gpu-depth-output"))runtime_gpu_path_probe(argv[2]);else if(!strcmp(argv[2],"probe-memory"))runtime_memory_probe();else if(!strcmp(argv[2],"probe-gfx"))runtime_gfx_probe(0);else if(!strcmp(argv[2],"probe-vpool"))runtime_vpool_probe();else if(!strcmp(argv[2],"probe-gpu-widen")||!strcmp(argv[2],"probe-gpu-blockrt")||!strcmp(argv[2],"probe-gpu-midclear")||!strcmp(argv[2],"probe-gpu-winding")||!strcmp(argv[2],"probe-gpu-tile"))runtime_gpu_path_probe(argv[2]);else if(!strcmp(argv[2],"probe-audio"))runtime_audio_probe();else if(!strcmp(argv[2],"probe-gfx-negative"))runtime_gfx_probe(1);else if(!strcmp(argv[2],"probe"))probe();else if(!strcmp(argv[2],"probe-fs"))runtime_fs_probe();else if(!strcmp(argv[2],"probe-swkbd"))runtime_swkbd_probe();else if(!strcmp(argv[2],"probe-save"))runtime_save_probe();else if(!strcmp(argv[2],"boot")){if(getenv("TOTK_VERIFY_STARTUP")||getenv("TOTK_HEADLESS"))boot();else runtime_front_run(boot);}else runtime_fail("unknown mode");return 0;
}
