/* Host-side transactional memory access for the optional cheat DATA VM.
   No allocations or Objective-C calls are permitted between guest suspend/resume.
   All addresses stay within this process's guest mappings. Disk images are untouched. */
#ifndef TOTK_CHEAT_NATIVE_H
#define TOTK_CHEAT_NATIVE_H
#include "cheat_vm.h"
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <libkern/OSCacheControl.h>
#define CN_PAGE 16384u
#define CN_CACHE 128u
#define CN_WRITES 8192u
#define CN_PATCHES 4096u
#define CN_ENTRIES 256u
extern Module *runtime_cheat_module(void) __attribute__((weak_import));
extern void runtime_cheat_region_bases(uint64_t *) __attribute__((weak_import));
extern int runtime_cheat_guest_range(uint64_t,size_t) __attribute__((weak_import));
extern unsigned runtime_cheat_suspend_threads(uint32_t *,unsigned) __attribute__((weak_import));
extern uint64_t runtime_gamepad_peek_buttons(void) __attribute__((weak_import));
typedef struct {uint64_t offset;uint32_t original;} CnGuard;
typedef struct {uint64_t address,value,old;unsigned width;} CnWrite;
typedef struct {uint64_t address;vm_prot_t protection;unsigned char bytes[CN_PAGE];} CnPage;
typedef struct {uint64_t address;uint32_t original,last;uint64_t owners[4];} CnPatch;
typedef struct {
 Module *module;CnGuard *guards;unsigned nguards;
 CnPage pages[CN_CACHE];unsigned npages;
 CnWrite writes[CN_WRITES];unsigned nwrites;
 CnPatch patches[CN_PATCHES];unsigned npatches,entry;
 uint64_t deadline;int timedout,issue;unsigned changed;
} CnMemory;
static int cn_allowed(CnMemory *c,uint64_t a,unsigned n){
 if(!n||a+n<a)return 0;
 uint64_t b=(uintptr_t)c->module->base;
 if(a>=b&&a+n<=b+c->module->key_off)return 1;
 return runtime_cheat_guest_range&&runtime_cheat_guest_range(a,n);
}
static CnPage *cn_page(CnMemory *c,uint64_t a){
 uint64_t p=a&~(uint64_t)(CN_PAGE-1);
 for(unsigned i=0;i<c->npages;i++)if(c->pages[i].address==p)return &c->pages[i];
 if(c->npages==CN_CACHE||!cn_allowed(c,p,CN_PAGE))return NULL;
 mach_vm_address_t r=p;mach_vm_size_t size=0;vm_region_basic_info_data_64_t info;
 mach_msg_type_number_t count=VM_REGION_BASIC_INFO_COUNT_64;mach_port_t obj=0;
 kern_return_t k=mach_vm_region(mach_task_self(),&r,&size,VM_REGION_BASIC_INFO_64,(vm_region_info_t)&info,&count,&obj);
 if(obj)mach_port_deallocate(mach_task_self(),obj);
 if(k||r>p||size<CN_PAGE||!(info.protection&VM_PROT_READ))return NULL;
 CnPage *page=&c->pages[c->npages];mach_vm_size_t got=0;
 if(mach_vm_read_overwrite(mach_task_self(),p,CN_PAGE,(mach_vm_address_t)page->bytes,&got)||got!=CN_PAGE)return NULL;
 page->address=p;page->protection=info.protection;c->npages++;return page;
}
static int cn_read(void *context,uint64_t a,unsigned n,uint64_t *v){
 CnMemory *c=context;if(runtime_now_ns()>c->deadline){c->timedout=1;return 0;}
 if(!cn_allowed(c,a,n))return 0;*v=0;
 for(unsigned i=0;i<n;i++){CnPage *p=cn_page(c,a+i);if(!p)return 0;*v|=(uint64_t)p->bytes[(a+i)&(CN_PAGE-1)]<<(i*8);}
 return 1;
}
static CnPatch *cn_patch(CnMemory *c,uint64_t a){for(unsigned i=0;i<c->npatches;i++)if(c->patches[i].address==a)return &c->patches[i];return NULL;}
static int cn_guard(CnMemory *c,uint64_t a,uint32_t *original){
 uint64_t b=(uintptr_t)c->module->base;if(a<b)return 0;
 for(unsigned i=0;i<c->nguards;i++)if(c->guards[i].offset==a-b){*original=c->guards[i].original;return 1;}return 0;
}
static int cn_write(void *context,uint64_t a,unsigned n,uint64_t value){
 CnMemory *c=context;uint64_t old;if(!cn_read(c,a,n,&old))return 0;
 if(n==8){CnPage *p=cn_page(c,a);if(!p)return 0;if(!(p->protection&VM_PROT_WRITE))return cn_write(c,a,4,(uint32_t)value)&&cn_write(c,a+4,4,value>>32);}
 CnPage *page=cn_page(c,a);if(!page)return 0;
 if(!(page->protection&VM_PROT_WRITE)){
  uint32_t original;if(n!=4||(a&3)||!cn_guard(c,a,&original)){c->issue=1;return 0;}
  CnPatch *p=cn_patch(c,a);uint64_t owners=p?(p->owners[c->entry/64]&~(UINT64_C(1)<<(c->entry%64))):0;
  if(p)for(unsigned i=0;i<4;i++)if(i!=c->entry/64)owners|=p->owners[i];
  if(old!=original&&(!p||old!=p->last)){c->issue=1;return 0;}
  if(owners&&p->last!=(uint32_t)value){c->issue=2;return 0;}
  if(p&&!original&&p->last!=(uint32_t)value){c->issue=3;return 0;}
  if(!p&&c->npatches+c->nwrites>=CN_PATCHES)return 0;
 }
 unsigned i;for(i=0;i<c->nwrites;i++)if(c->writes[i].address==a&&c->writes[i].width==n)break;
 if(i==c->nwrites){if(i==CN_WRITES)return 0;c->writes[c->nwrites++]=(CnWrite){a,value,old,n};}else c->writes[i].value=value;
 for(unsigned j=0;j<n;j++){CnPage *p=cn_page(c,a+j);if(!p)return 0;p->bytes[(a+j)&(CN_PAGE-1)]=(value>>(8*j))&255;}
 return 1;
}
/* Commit the validated entry while guest threads are stopped. Never make a page RWX.
   Restore page permissions before any guest resumes; a failed protection restore is fatal. */
static int cn_commit(CnMemory *c){
 uint64_t opened[CN_CACHE];vm_prot_t protection[CN_CACHE];unsigned count=0,done=0;int ok=1;
 for(unsigned i=0;i<c->nwrites;i++){
  CnWrite *w=&c->writes[i];if(w->old==w->value)continue;
  for(unsigned j=0;j<w->width;j++){
   CnPage *p=cn_page(c,w->address+j);if(!p){ok=0;goto close;}
   if(p->protection&VM_PROT_WRITE)continue;
   unsigned k;for(k=0;k<count;k++)if(opened[k]==p->address)break;if(k<count)continue;
   if(count==CN_CACHE||mach_vm_protect(mach_task_self(),p->address,CN_PAGE,FALSE,VM_PROT_READ|VM_PROT_WRITE)){ok=0;goto close;}
   opened[count]=p->address;protection[count++]=p->protection;
  }
 }
 for(;done<c->nwrites;done++){
  CnWrite *w=&c->writes[done];if(w->old==w->value)continue;
  if(mach_vm_write(mach_task_self(),w->address,(vm_offset_t)&w->value,w->width)){ok=0;break;}
 }
 if(!ok)for(unsigned i=0;i<done;i++){CnWrite *w=&c->writes[i];if(w->old!=w->value&&mach_vm_write(mach_task_self(),w->address,(vm_offset_t)&w->old,w->width))_exit(75);}
close:
 for(unsigned i=0;i<count;i++){
  sys_icache_invalidate((void *)(uintptr_t)opened[i],CN_PAGE);
  if(mach_vm_protect(mach_task_self(),opened[i],CN_PAGE,FALSE,protection[i]))_exit(75);
 }
 if(!ok)return 0;
 for(unsigned i=0;i<c->nwrites;i++){
  CnWrite *w=&c->writes[i];CnPage *page=cn_page(c,w->address);if(!page)return 0;
  if(!(page->protection&VM_PROT_WRITE)){
   CnPatch *p=cn_patch(c,w->address);
   if(!p){p=&c->patches[c->npatches++];memset(p,0,sizeof *p);p->address=w->address;cn_guard(c,w->address,&p->original);}
   p->last=(uint32_t)w->value;p->owners[c->entry/64]|=UINT64_C(1)<<(c->entry%64);
  }
  if(w->old!=w->value)c->changed++;
 }
 return 1;
}
/* Unchecking stops data writes. Restore guarded code/constant sites when the last
   owner leaves. Dormant zero-filled code caves remain until the next clean launch:
   a suspended guest might already have its PC in that cave. */
static int cn_release(CnMemory *c,unsigned entry){
 c->entry=entry;c->npages=c->nwrites=0;uint64_t bit=UINT64_C(1)<<(entry%64);
 for(unsigned i=0;i<c->npatches;i++){
  CnPatch *p=&c->patches[i];if(!(p->owners[entry/64]&bit))continue;
  uint64_t other=0;for(unsigned j=0;j<4;j++)other|=p->owners[j]&(j==entry/64?~bit:UINT64_MAX);
  if(!other&&p->original){uint64_t current;if(!cn_read(c,p->address,4,&current))return 0;
   if(current==p->last&&!cn_write(c,p->address,4,p->original))return 0;
  }
 }
 if(!cn_commit(c))return 0;
 for(unsigned i=0;i<c->npatches;i++)c->patches[i].owners[entry/64]&=~bit;
 return 1;
}
#endif
