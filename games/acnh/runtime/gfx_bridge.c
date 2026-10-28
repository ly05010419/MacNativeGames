/* Bind the donor graphics interface to this native runtime, without donor game hooks. */
#include "runtime.h"
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../third_party/d2r_gfx/gfx/nvn/nvn_mtl.h"
typedef struct{const char *name;void *fn;const char *why;} GfxEntry;
uint64_t g_t0_ns;

uint64_t now_ns(void){return runtime_now_ns();}
uint64_t guest_ns(void){return runtime_now_ns();}
void *module_of(const void *p){(void)p;return NULL;}
static int (*fault_hook)(void *,int,void *);
static void (*prepare_hook)(void *,size_t);
static void (*report_hook)(uint32_t,const char *);
void dm_fault_hook_register(int (*fn)(void *,int,void *)){if(fault_hook)runtime_fail("second gfx fault hook");fault_hook=fn;}
void dm_guest_write_prep_register(void (*fn)(void *,size_t)){if(prepare_hook)runtime_fail("second gfx prepare hook");prepare_hook=fn;}
void dm_res_report_register(void (*fn)(uint32_t,const char *)){report_hook=fn;(void)report_hook;}
extern uint64_t dm_host_write_pc;   /* nvn_dirty.c: writer of a host write, for its DM_NVN_TILELOG diagnostic */
void dm_guest_write_prepare(void *addr,size_t n){dm_host_write_pc=(uint64_t)__builtin_return_address(0);if(prepare_hook)prepare_hook(addr,n);}
/* TOTK diagnostic write watchpoint: TOTK_WATCH_ADDR=<hex guest address> [TOTK_WATCH_FRAME=<first frame>, default 300].
   From that frame on, the 16 KiB page holding the address is made read-only after every present; a guest write faults,
   the writer (pc / lr / frame chain as module+offset) is logged, the page is unprotected and the write re-executes. */
#include <sys/mman.h>
#include <stdatomic.h>
#include <pthread.h>
#include <unistd.h>
#include <unistd.h>
#include <mach/mach.h>
#include "mach_vm_compat.h"
#include <sys/ucontext.h>
static uintptr_t watch_lo,watch_hi,watch_addr,watch_end;static long watch_from=300;static int watch_hits,watch_init,watch_read;
static unsigned char watch_snap[32];static int watch_snap_valid;static _Atomic int watch_armed;   /* TOTK: content poll at every present + periodic re-arm (an unrelated write in the page unprotects it; without re-arm the block's own write in the same frame is missed) */
static void *watch_rearm_thread(void *arg){(void)arg;for(;;){usleep(100);if(watch_lo&&atomic_load(&watch_armed))mprotect((void *)watch_lo,watch_hi-watch_lo,watch_read?PROT_NONE:PROT_READ);}return NULL;}
int runtime_module_offset(uint64_t pc,const char **name,uint64_t *off);
static void watch_hex(char *b,size_t *n,const char *label,uint64_t v){while(*label)b[(*n)++]=*label++;b[(*n)++]='0';b[(*n)++]='x';for(int i=15;i>=0;i--)b[(*n)++]="0123456789abcdef"[(v>>(i*4))&15];}
static void watch_sym(char *b,size_t *n,const char *label,uint64_t pc){const char *nm;uint64_t off;while(*label)b[(*n)++]=*label++;if(runtime_module_offset(pc,&nm,&off)){while(*nm)b[(*n)++]=*nm++;watch_hex(b,n,"+",off);}else watch_hex(b,n,"host:",pc);}
/* TOTK: watch a guest range [addr, addr+len); TOTK_WATCH_READ=1 also catches reads (pages become inaccessible after each present).
   Set from the environment (TOTK_WATCH_ADDR) or at runtime by the graphics layer (DM_NVN_WATCH_SB: a storage buffer bound to a given shader). */
int totk_vpool_backing(uintptr_t va,uint64_t len,uintptr_t *cpu);
void runtime_watch_set(uintptr_t addr,size_t len){
 { uintptr_t cpu=0; if(totk_vpool_backing(addr,len?len:256,&cpu)&&cpu){char b[160];size_t n=0;watch_hex(b,&n,"[watch] virtual pool address ",addr);watch_hex(b,&n," -> CPU backing ",cpu);b[n++]=10;write(2,b,n);addr=cpu;} }
 watch_addr=addr;watch_end=addr+(len?len:256);watch_lo=addr&~(uintptr_t)0x3fff;watch_hi=(watch_end+0x3fff)&~(uintptr_t)0x3fff;watch_hits=0;
 char b[160];size_t n=0;watch_hex(b,&n,"[watch] range set addr=",addr);watch_hex(b,&n," end=",watch_end);watch_hex(b,&n," pages=",(watch_hi-watch_lo)>>14);b[n++]=10;write(2,b,n);
}
void runtime_watch_tick(uint64_t frame){
 if(!watch_init){watch_init=1;const char *e=getenv("TOTK_WATCH_ADDR");if(e){uintptr_t a=(uintptr_t)strtoull(e,NULL,16);const char *l=getenv("TOTK_WATCH_LEN");runtime_watch_set(a,l?(size_t)strtoull(l,NULL,0):256);}const char *f=getenv("TOTK_WATCH_FRAME");if(f)watch_from=atol(f);watch_read=getenv("TOTK_WATCH_READ")!=NULL;}
 if(watch_lo&&(long)frame>=watch_from){ unsigned char cur[32];memcpy(cur,(const void *)watch_addr,32);
  if(!watch_snap_valid||memcmp(cur,watch_snap,32)){char b[256];size_t n=0;watch_hex(b,&n,"[watch] content at present frame=",frame);for(int i=0;i<32;i+=4){uint32_t w;memcpy(&w,cur+i,4);b[n++]=' ';for(int k=7;k>=0;k--)b[n++]="0123456789abcdef"[(w>>(k*4))&15];}b[n++]=10;write(2,b,n);memcpy(watch_snap,cur,32);watch_snap_valid=1;} }
 if(!watch_lo||(long)frame<watch_from||watch_hits>=200)return;
 { static int started;if(!started){started=1;pthread_t t;pthread_create(&t,NULL,watch_rearm_thread,NULL);} atomic_store(&watch_armed,1); }
 if(mprotect((void *)watch_lo,watch_hi-watch_lo,watch_read?PROT_NONE:PROT_READ)!=0){char b[96];size_t n=0;watch_hex(b,&n,"[watch] mprotect failed page=",watch_lo);b[n++]=10;write(2,b,n);}
}
static int watch_fault(void *addr,int is_write,void *context){
 uintptr_t a=(uintptr_t)addr;if(!watch_lo||a<watch_lo||a>=watch_hi)return 0;
 if(a<watch_addr||a>=watch_end){mprotect((void *)watch_lo,watch_hi-watch_lo,PROT_READ|PROT_WRITE);return 1;}   /* other data in the pages: silently unprotect (re-armed at the next present) */
 ucontext_t *u=context;char b[1024];size_t n=0;
 { extern uint64_t g_frame_pub; extern _Atomic int dm_submit_busy; uint64_t th=0; pthread_threadid_np(NULL,&th);
   watch_hex(b,&n,"[watch] F",g_frame_pub);watch_hex(b,&n," t_us=",clock_gettime_nsec_np(CLOCK_UPTIME_RAW)/1000);watch_hex(b,&n," th=",th);watch_hex(b,&n," submit_busy=",(uint64_t)atomic_load(&dm_submit_busy)); }
 watch_hex(b,&n,is_write?" write hit addr=":" read hit addr=",a);watch_hex(b,&n," off=",a-watch_addr);
 watch_sym(b,&n," pc=",u->uc_mcontext->__ss.__pc);watch_sym(b,&n," lr=",u->uc_mcontext->__ss.__lr);
 uint64_t fp=u->uc_mcontext->__ss.__fp;for(int d=0;d<8&&fp&&!(fp&15);d++){uint64_t fr[2];mach_vm_size_t got=0;if(mach_vm_read_overwrite(mach_task_self(),fp,16,(uintptr_t)fr,&got)!=KERN_SUCCESS||got!=16)break;watch_sym(b,&n," <",fr[1]);fp=fr[0];}
 b[n++]='\n';write(2,b,n);watch_hits++;
 mprotect((void *)watch_lo,watch_hi-watch_lo,PROT_READ|PROT_WRITE);return 1;
}
int runtime_gfx_fault(void *addr,int is_write,void *context){if(watch_fault(addr,is_write,context))return 1;return fault_hook?fault_hook(addr,is_write,context):0;}
void crash_set_wait_dump(void (*fn)(void)){(void)fn;}
void *runtime_gfx_override(const char *name){
 unsigned long size=0;const GfxEntry *entries=(void *)getsectiondata(&_mh_execute_header,"__DATA","totkgfx",&size);
 for(unsigned i=0;entries&&i<size/sizeof(*entries);i++)if(!strcmp(entries[i].name,name))return entries[i].fn;return NULL;
}
typedef void *(*Alloc)(unsigned long,unsigned long,void *);
typedef void (*Free)(void *,void *);
typedef void *(*Realloc)(void *,unsigned long,void *);
struct Allocator{Alloc alloc;Free free;Realloc realloc;void *user;};
struct Allocator g_nv_alloc,g_nv_devtools_alloc;
static void graphics_allocator(Alloc a,Free f,Realloc r,void *u){g_nv_alloc=(struct Allocator){a,f,r,u};}
static void graphics_dev_allocator(Alloc a,Free f,Realloc r,void *u){g_nv_devtools_alloc=(struct Allocator){a,f,r,u};}
static void graphics_initialize(void *mem,unsigned long size){
 if(getenv("ACNH_CPU_ONLY"))runtime_fail("CPU-only diagnostic reached graphics initialization boundary");
 if(!mem||!size)runtime_fail("invalid graphics work memory");g_t0_ns=runtime_now_ns();mtl_init();fprintf(stderr,"[totk] native Metal graphics context initialized\n");
}
__attribute__((used,section("__DATA,totkgfx"))) static const GfxEntry init_entry={"_ZN2nv18InitializeGraphicsEPvm",graphics_initialize,"native Metal context"};
__attribute__((used,section("__DATA,totkgfx"))) static const GfxEntry alloc_entry={"_ZN2nv20SetGraphicsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_",graphics_allocator,"game allocator callbacks"};
__attribute__((used,section("__DATA,totkgfx"))) static const GfxEntry dev_alloc_entry={"_ZN2nv28SetGraphicsDevtoolsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_",graphics_dev_allocator,"game diagnostic allocator callbacks"};
