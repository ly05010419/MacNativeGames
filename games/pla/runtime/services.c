/* Horizon services reached by the current TOTK startup. Unknown operations fail. */
#include "runtime.h"
#include <TargetConditionals.h>
#include "ipc.h"
#include "game_version.h"
#include <mach/mach.h>
#include "mach_vm_compat.h"
#include <stdio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <ctype.h>
#include <string.h>
#include <sys/mman.h>

#include <time.h>
#include <unistd.h>
#include <mach/mach_time.h>
/* Guest address layout. The SDK encodes IPC buffer addresses with 39 bits (HIPC descriptor bits 36..38),
   so all guest memory must lie below 2^39. Darwin arm64 keeps 0x10_0000_0000..0x70_0000_0000 reserved,
   leaving exactly 64 GiB at 0x70_0000_0000 for the alias region (the real 39-bit Horizon size, which the
   game's allocators depend on); the stack and heap regions are reserved separately below it. */
#define GUEST_ADDRESS_LIMIT (1ULL<<39)
#define ALIAS_SPACE_AT 0x7000000000ULL
#define ALIAS_SIZE_MAX (64ULL<<30)
/* TOTK_ALIAS_GIB: alias region size reported to the game (default 64, the real 39-bit Horizon size). An iOS app signed
   without extended virtual addressing (unavailable to personal teams) gets a much smaller map: the old 6 + 16 GiB
   reservation failed on an iPhone, while 4.75 + 16 GiB fit on an iPad (A16, iPadOS 26.7). So the largest size from 16
   down to 4 GiB that fits is chosen at startup; the game maps about 3.3 GiB of it. */
#if TARGET_OS_IPHONE
#define ALIAS_DEFAULT_GIB 16
#define ALIAS_MIN_GIB 4ULL
#else
#define ALIAS_DEFAULT_GIB 64
#endif
static uint64_t alias_len;
static uint64_t alias_requested(void){const char *e=getenv("TOTK_ALIAS_GIB");uint64_t g=e?strtoull(e,NULL,0):ALIAS_DEFAULT_GIB;if(g<1||g>64)runtime_fail("TOTK_ALIAS_GIB must be 1..64");return g<<30;}
static uint64_t alias_size(void){if(!alias_len)alias_len=alias_requested();return alias_len;}
#define ALIAS_SIZE alias_size()
/* PLA's nninitStartup carves one AllocateAddressRegion(32 GiB) out of the VAMM region reported by GetInfo(2/3),
   and macOS 27 cannot fit that below 2^39: the shared cache covers everything up to 0xF_C000_0000 and the system
   drops a ~24 GiB reservation at a random place inside the 64 GiB window at 0x70_0000_0000, so no 38 GiB free hole
   exists there (measured over several boots; the TOTK shrink-to-fit path ends at 12 GiB). The VAMM region is real
   RAM on the Switch (the VammManager keeps its books in user mode, AllocateMemoryPages only tags attributes), so it
   is hosted as a plain lazy-commit mapping above the 39-bit line, where address space is plentiful. Nothing on
   those addresses should ever travel through the guest's 39-bit HIPC buffer encoding; if one ever does, the guest's
   sf layer truncates it and the HLE fails loudly on the wrong address. */
#define VAMM_SPACE_AT 0x10000000000ULL
#define VAMM_SIZE (64ULL<<30)
static uint8_t *vamm_space;
#define VAMM_PAGES (VAMM_SIZE/4096)
static unsigned char vamm_pages[VAMM_PAGES];   /* per-4-KB-page physical-commit state */
static uint64_t vamm_bytes;
#define LOW_SEARCH_FROM 0x300000000ULL
#if TARGET_OS_IPHONE
#define LOW_SIZE (HEAP_OFF+HEAP_CAP)   /* iOS: no slack after the heap cap; address space is scarce */
#else
#define LOW_SIZE (6ULL<<30)
#endif
#define STACK_OFF 0x10000000ULL
#define STACK_SIZE 0x10000000ULL
#define HEAP_OFF (STACK_OFF+STACK_SIZE+0x10000000ULL)
static uint8_t *alias_space,*low_space,*heap;
static size_t heap_size;
static unsigned char alias_pages[ALIAS_SIZE_MAX/4096];
/* 4 KiB guest pages that are unmapped but whose host 16 KiB page is still committed with old contents: only these are
   zeroed when mapped again. Every other free page is untouched anonymous memory (zero on first touch), so mapping does
   not make it resident; a fully free 16 KiB host page is handed back to the system (fresh anonymous mapping). On the
   iPhone, where the app may use about 4 GB in total, memory the game maps but never touches must stay unbacked. */
static unsigned char alias_stale[ALIAS_SIZE_MAX/4096];
static uint64_t alias_bytes;
static uint8_t user_exception_context[0x1000] __attribute__((aligned(0x1000)));
#define SYSTEM_RESOURCE_BUDGET (32ULL<<20)
#define HEAP_CAP (4ULL<<30)   /* 1.4.3 sets a 2.2 GiB heap during nninitStartup */
#define APP_POOL app_pool()
static uint64_t app_pool(void){static uint64_t v;if(!v){const char *e=getenv("TOTK_APP_POOL");uint64_t extra=(runtime_output_res_extra_bytes()+0x1fffff)&~0x1fffffULL;
 v=e?strtoull(e,NULL,0):0xcd500000ULL+extra;if(!e&&extra)fprintf(stderr,"[totk] application pool %#llx: +%llu MiB for the true-2K texture heaps (TOTK_OUTPUT_RES)\n",(unsigned long long)v,(unsigned long long)(extra>>20));}return v;}
/* TOTK: application memory pool reported to the game (default: retail 3.2 GiB; TOTK_OUTPUT_RES adds what its HeapDef overlay gives the texture heaps) */
#ifdef GAME_PLA
/* PLA plans its commits from the pool GetInfo(6/21) reports — it submitted 5.9 GiB against a 6 GiB pool and 2.95 GiB
   against the 3.2 GiB retail one, always leaving itself just over the physical budget, which then fails here. Report
   the retail pool to the game and check the physical budget against a wide value instead: macOS anonymous memory is
   lazily committed, so the 3.1 GiB the game books is resident only in the fraction it actually touches. */
#define APP_POOL_CHECK (0x180000000ULL)
#else
#define APP_POOL_CHECK APP_POOL
#endif
#define ALIAS_START ((uintptr_t)alias_space)
#define STACK_START ((uintptr_t)low_space+STACK_OFF)
#define ASLR_START ((uintptr_t)low_space)
#if TARGET_OS_IPHONE
#define ASLR_SIZE (ALIAS_START+ALIAS_SIZE-(uintptr_t)low_space)   /* iOS: everything the process can map ends far below 2^39 */
#else
#define ASLR_SIZE (GUEST_ADDRESS_LIMIT-(uintptr_t)low_space)
#endif
static uint8_t *reserve_fixed(uint64_t at,uint64_t size){
 mach_vm_address_t a=at;   /* FIXED without OVERWRITE: fails instead of replacing an existing mapping */
 return mach_vm_map(mach_task_self(),&a,size,0,VM_FLAGS_FIXED,MEMORY_OBJECT_NULL,0,FALSE,VM_PROT_NONE,VM_PROT_ALL,VM_INHERIT_DEFAULT)==KERN_SUCCESS?(uint8_t *)(uintptr_t)a:NULL;
}
#define GUEST_ALIGN (2ULL<<20)
/* Reserve size bytes in the lowest free gap of this process's map above 4 GiB (2 MiB aligned). The end of the map is
   not reported, so the last gap is only tried; FIXED fails there with KERN_NO_SPACE when it reaches past the end. */
static uint8_t *reserve_in_gap(uint64_t size,int log){
 uint64_t prev=1ULL<<32;
 for(;;){
  mach_vm_address_t r=prev;mach_vm_size_t rsize=0;vm_region_basic_info_data_64_t info;mach_msg_type_number_t n=VM_REGION_BASIC_INFO_COUNT_64;mach_port_t obj=MACH_PORT_NULL;
  int more=mach_vm_region(mach_task_self(),&r,&rsize,VM_REGION_BASIC_INFO_64,(vm_region_info_t)&info,&n,&obj)==KERN_SUCCESS;
  uint64_t start=(prev+GUEST_ALIGN-1)&~(GUEST_ALIGN-1),end=more?r:GUEST_ADDRESS_LIMIT;
  if(end>start&&end-start>=(256ULL<<20)&&log)fprintf(stderr,"[totk-mem] free %#llx..%s%#llx\n",(unsigned long long)start,more?"":"(end of map, at most) ",(unsigned long long)end);
  if(end>start&&end-start>=size){uint8_t *p=reserve_fixed(start,size);if(p)return p;}
  if(!more)return NULL;
  prev=r+rsize;
 }
}
static void memory_init(void) {
 if(alias_space)return;
#if TARGET_OS_IPHONE
 /* iOS: stack+heap and the alias region go side by side in one reservation (ASLR_SIZE spans both), placed in the lowest
    free gap that holds them. The free space depends on the device, so the alias region shrinks from the requested size
    to ALIAS_MIN_GIB before giving up (logging the free gaps); TOTK_ALIAS_GIB pins one size. */
 uint64_t want=alias_requested(),least=getenv("TOTK_ALIAS_GIB")?want:(ALIAS_MIN_GIB<<30);
 for(uint64_t a=want;a>=least&&!low_space;a-=1ULL<<30)if((low_space=reserve_in_gap(LOW_SIZE+a,0)))alias_len=a;
 if(!low_space){reserve_in_gap(UINT64_MAX,1);runtime_fail("reserve guest stack, heap and alias regions");}
 alias_space=low_space+LOW_SIZE;
#else
 alias_space=reserve_fixed(ALIAS_SPACE_AT,ALIAS_SIZE);
 if(alias_space)for(uint64_t at=LOW_SEARCH_FROM;at+LOW_SIZE<=ALIAS_SPACE_AT&&!low_space;at+=1ULL<<30)low_space=reserve_fixed(at,LOW_SIZE);
 if(!low_space){
  /* macOS 27: the shared cache fills 0x1_8000_0000..0xF_C000_0000 and the system reserves ~24 GiB at a random place in
     0x70_0000_0000..2^39, so neither fixed region fits. As on iOS, put stack+heap and the alias region side by side in one
     free hole, shrinking the alias region (the game uses < 3 GiB of it) until it fits. TOTK_ALIAS_GIB pins one size. */
  if(alias_space)mach_vm_deallocate(mach_task_self(),(mach_vm_address_t)(uintptr_t)alias_space,ALIAS_SIZE);
  uint64_t want=alias_requested(),least=getenv("TOTK_ALIAS_GIB")?want:(8ULL<<30);
  reserve_in_gap(UINT64_MAX,getenv("TOTK_MEM_LOG")?1:0);   /* print this process's free gaps below 2^39 */
  for(uint64_t g=want;!low_space;g-=4ULL<<30){
   for(uint64_t at=LOW_SEARCH_FROM;at+LOW_SIZE+g<=GUEST_ADDRESS_LIMIT&&!low_space;at+=1ULL<<30)
    if((low_space=reserve_fixed(at,LOW_SIZE+g)))alias_len=g;
   if(g<least+(4ULL<<30))break;
  }
  if(!low_space)runtime_fail(getenv("TOTK_ALIAS_GIB")?"reserve guest stack, heap and the TOTK_ALIAS_GIB alias region below 2^39":"reserve guest stack, heap and alias regions below 2^39");
  alias_space=low_space+LOW_SIZE;
 }
#endif
 heap=low_space+HEAP_OFF;
 if(!vamm_space){
  mach_vm_address_t a=VAMM_SPACE_AT;
  vamm_space=mach_vm_map(mach_task_self(),&a,VAMM_SIZE,0,VM_FLAGS_FIXED,MEMORY_OBJECT_NULL,0,FALSE,VM_PROT_READ|VM_PROT_WRITE,VM_PROT_ALL,VM_INHERIT_DEFAULT)==KERN_SUCCESS?(uint8_t *)(uintptr_t)a:NULL;
 }
 if(!vamm_space)runtime_fail("reserve the 64 GiB VAMM region at 0x10000000000");
 fprintf(stderr,"[totk] guest layout: alias %#llx+%#llx stack %#llx+%#llx heap %#llx (cap %#llx) vamm %#llx+%#llx\n",(unsigned long long)ALIAS_START,ALIAS_SIZE,(unsigned long long)STACK_START,STACK_SIZE,(unsigned long long)(uintptr_t)heap,HEAP_CAP,(unsigned long long)VAMM_SPACE_AT,VAMM_SIZE);
}
/* Scoped stack moves: independent host backing, copy back on unmap.
 * Logical accounting is 4 KiB; Darwin protections are rounded to 16 KiB.
 * The leading/trailing partial source pages cannot be isolated on this host.
 */
static pthread_mutex_t memory_lock=PTHREAD_MUTEX_INITIALIZER;
typedef struct{uint64_t dst,src,size;} StackMap;static StackMap stack_maps[4096];static unsigned nstack_maps;
static int overlaps(uint64_t a,uint64_t n,uint64_t b,uint64_t m){return a<b+m&&b<a+n;}
static uint64_t stack_move(int unmap,uint64_t dst,uint64_t src,uint64_t size){
 if(!size||((dst|src|size)&4095))return 0xcc01;
 if(dst<STACK_START||size>STACK_SIZE||dst-STACK_START>STACK_SIZE-size)return 0xdc01;
 int inHeap=src>=(uintptr_t)heap&&size<=heap_size&&src-(uintptr_t)heap<=heap_size-size;
 uint64_t aliasStart=ALIAS_START;
 int inAlias=src>=aliasStart&&size<=ALIAS_SIZE&&src-aliasStart<=ALIAS_SIZE-size;
 /* PLA: thread stacks live in the VAMM region (the 32 GiB carveout the game commits with AllocateMemoryPages),
    and nn::os's CreateAliasStackUnsafe maps them into the alias region with MapMemory — a source the TotK-era
    check never saw. Same semantics as an alias source: pages must be committed; mapping moves them to dst
    (source goes PROT_NONE, the bitmap marks them uncommitted) and unmapping restores them. */
 uint64_t vammStart=VAMM_SPACE_AT;
 int inVamm=src>=vammStart&&size<=VAMM_SIZE&&src-vammStart<=VAMM_SIZE-size;
 if(!inHeap&&!inAlias&&!inVamm)return 0xdc01;
 pthread_mutex_lock(&memory_lock);
 if(inAlias){unsigned first=(unsigned)((src-aliasStart)/4096);for(uint64_t i=0;i<size/4096;i++)if(!alias_pages[first+i]){pthread_mutex_unlock(&memory_lock);return 0xdc01;}}
 if(inVamm){unsigned first=(unsigned)((src-vammStart)/4096);for(uint64_t i=0;i<size/4096;i++)if(!vamm_pages[first+i]){pthread_mutex_unlock(&memory_lock);return 0xdc01;}}

 unsigned found=nstack_maps;for(unsigned i=0;i<nstack_maps;i++)if(stack_maps[i].dst==dst&&stack_maps[i].src==src&&stack_maps[i].size==size)found=i;
 uint64_t lo=dst&~0x3fffULL,hi=(dst+size+0x3fff)&~0x3fffULL;
 uint64_t slo=(src+0x3fff)&~0x3fffULL,shi=(src+size)&~0x3fffULL;
 if(unmap){
  if(found==nstack_maps){pthread_mutex_unlock(&memory_lock);return 0xd401;}
  if(shi>slo&&mprotect((void *)slo,shi-slo,PROT_READ|PROT_WRITE))runtime_fail("restore moved stack source");
  if(inVamm){unsigned first=(unsigned)((src-vammStart)/4096);for(uint64_t i=0;i<size/4096;i++)if(!vamm_pages[first+i]){vamm_pages[first+i]=1;vamm_bytes+=4096;}}
  memcpy((void *)src,(const void *)dst,size);stack_maps[found]=stack_maps[--nstack_maps];
  for(uint64_t page=lo;page<hi;page+=0x4000){int used=0;for(unsigned i=0;i<nstack_maps;i++)if(overlaps(page,0x4000,stack_maps[i].dst,stack_maps[i].size))used=1;
   if(!used&&mprotect((void *)page,0x4000,PROT_NONE))runtime_fail("decommit moved stack");}
 }else{
  if(nstack_maps==4096){pthread_mutex_unlock(&memory_lock);return 0xd401;}
  for(unsigned i=0;i<nstack_maps;i++)if(overlaps(dst,size,stack_maps[i].dst,stack_maps[i].size)||overlaps(src,size,stack_maps[i].src,stack_maps[i].size)){pthread_mutex_unlock(&memory_lock);return 0xdc01;}
  if(mprotect((void *)lo,hi-lo,PROT_READ|PROT_WRITE))runtime_fail("commit moved stack");memcpy((void *)dst,(const void *)src,size);
  /* keep the VAMM source committed (RW): the game reuses moved-from stack pages through its own allocator
     without a fresh MapPhysicalMemory, and a PROT_NONE source page faults that reuse. */
  if(shi>slo&&!inVamm&&mprotect((void *)slo,shi-slo,PROT_NONE))runtime_fail("protect moved stack source");
  stack_maps[nstack_maps++]=(StackMap){dst,src,size};
 }
 pthread_mutex_unlock(&memory_lock);return 0;
}
static uint64_t alias_map(int unmap,uint64_t addr,uint64_t size){
 uint64_t start=ALIAS_START;
 if(!size||((addr|size)&4095))return 0xcc01;
 if(addr<start||size>ALIAS_SIZE||addr-start>ALIAS_SIZE-size)return 0xdc01;
 pthread_mutex_lock(&memory_lock);unsigned first=(unsigned)((addr-start)/4096),count=(unsigned)(size/4096);
 /* Horizon MapPhysicalMemory maps only the pages of the range that are still unmapped and leaves mapped pages (and their
    contents) untouched; TOTK's physical-memory heaps grow by re-mapping ranges that overlap what they already own. */
 uint64_t fresh=0;if(!unmap)for(unsigned i=0;i<count;i++)if(!alias_pages[first+i])fresh+=4096;
 /* Physical-memory resource reservation failure is LimitReached (132), not
    InvalidCurrentMemory (106). The original SDK maps LimitReached to its
    recoverable out-of-memory result; 0xd401 makes AllocateMemoryPages abort. */
 if(!unmap&&fresh+alias_bytes+heap_size+runtime_image_bytes()>APP_POOL_CHECK){
  pthread_mutex_unlock(&memory_lock);
  const char *negative=getenv("TOTK_PROBE_NEGATIVE");
  return negative&&atoi(negative)==14?0xd401:0x10801;
 }
 if(unmap){
  for(unsigned i=0;i<nstack_maps;i++)if(overlaps(addr,size,stack_maps[i].src,stack_maps[i].size)){pthread_mutex_unlock(&memory_lock);return 0xdc01;}
  uint64_t released=0;for(unsigned i=0;i<count;i++)if(alias_pages[first+i]){alias_pages[first+i]=0;alias_stale[first+i]=1;released+=4096;}alias_bytes-=released;
  uint64_t low=addr&~0x3fffULL,high=(addr+size+0x3fff)&~0x3fffULL;
  for(uint64_t page=low;page<high;page+=0x4000){unsigned p=(unsigned)((page-start)/4096);if(!alias_pages[p]&&!alias_pages[p+1]&&!alias_pages[p+2]&&!alias_pages[p+3]){
   if(mmap((void *)page,0x4000,PROT_NONE,MAP_FIXED|MAP_PRIVATE|MAP_ANON,-1,0)!=(void *)page)runtime_fail("decommit physical alias page");
   memset(alias_stale+p,0,4);}}
 }else if(fresh){
  uint64_t low=addr&~0x3fffULL,high=(addr+size+0x3fff)&~0x3fffULL;
  if(mprotect((void *)low,high-low,PROT_READ|PROT_WRITE))runtime_fail("commit physical alias pages");
  for(unsigned i=0;i<count;i++)if(!alias_pages[first+i]){   /* new pages only: zero-filled like fresh pool pages */
   if(alias_stale[first+i]){memset((void *)(addr+(uint64_t)i*4096),0,4096);alias_stale[first+i]=0;}
   alias_pages[first+i]=1;}
  alias_bytes+=fresh;
 }
 pthread_mutex_unlock(&memory_lock);return 0;
}
uint64_t runtime_heap_base(void){return (uintptr_t)heap;}
/* MapPhysicalMemory/UnmapPhysicalMemory over the VAMM region (the 64 GiB above the 39-bit line): the reservation
   is PROT_NONE and committing is mprotect, like the alias path; unmap tears protection down and marks the pages
   stale so the next map hands them out zeroed, exactly what Horizon's pool does. */
static uint64_t vamm_map(int unmap,uint64_t addr,uint64_t size){
 uint64_t start=VAMM_SPACE_AT;
 if(!size||((addr|size)&4095))return 0xcc01;
 if(addr<start||size>VAMM_SIZE||addr-start>VAMM_SIZE-size)return 0xdc01;
 pthread_mutex_lock(&memory_lock);unsigned first=(unsigned)((addr-start)/4096),count=(unsigned)(size/4096);
 uint64_t fresh=0;if(!unmap)for(unsigned i=0;i<count;i++)if(!vamm_pages[first+i])fresh+=4096;
 if(!unmap&&fresh+alias_bytes+heap_size+runtime_image_bytes()+vamm_bytes>APP_POOL_CHECK){
  pthread_mutex_unlock(&memory_lock);return 0x10801;
 }
 uint64_t low=addr&~0x3fffULL,high=(addr+size+0x3fff)&~0x3fffULL;
 /* PLA: its allocator unmaps ranges it later writes again through stale bookkeeping (the post-language-
    confirm burst memcpy'd into unmapped-then-reused pages). Horizon physical memory is a real resource the
    game tracks precisely, but on the host the VAMM region is lazily-committed anonymous memory: keep every
    page committed once mapped and treat map/unmap as bookkeeping only. Physical-memory pressure accounting
    (vamm_bytes) still follows the svc calls for the pool reports. */
 if(!unmap){
  if(mprotect((void *)low,high-low,PROT_READ|PROT_WRITE))runtime_fail("commit VAMM pages");
  for(unsigned i=0;i<count;i++)if(!vamm_pages[first+i]){vamm_pages[first+i]=1;vamm_bytes+=4096;}
 }
 pthread_mutex_unlock(&memory_lock);return 0;
}
/* Guest memory the game has committed (heap size + mapped alias pages), for memory reports (iOS app monitor). */
void runtime_guest_committed(uint64_t *heap_bytes,uint64_t *alias_mapped){*heap_bytes=heap_size;*alias_mapped=alias_bytes;}
/* Physical memory behind the guest regions (stack+heap and alias): resident pages, of which dirty, and pages the system
   compressed. Committed sizes overstate what the game uses; these are what count against the iOS memory limit. */
static void region_pages(uint64_t a,uint64_t end,uint64_t *resident,uint64_t *dirty,uint64_t *compressed){
 while(a<end){vm_address_t va=(vm_address_t)a;vm_size_t vs=0;natural_t depth=0;vm_region_submap_info_data_64_t info;mach_msg_type_number_t cnt=VM_REGION_SUBMAP_INFO_COUNT_64;
  if(vm_region_recurse_64(mach_task_self(),&va,&vs,&depth,(vm_region_recurse_info_t)&info,&cnt)!=KERN_SUCCESS||va>=end)break;
  *resident+=(uint64_t)info.pages_resident*vm_page_size;*dirty+=(uint64_t)info.pages_dirtied*vm_page_size;*compressed+=(uint64_t)info.pages_swapped_out*vm_page_size;
  a=(uint64_t)va+vs;}
}
void runtime_guest_resident(uint64_t *resident,uint64_t *dirty,uint64_t *compressed){
 *resident=*dirty=*compressed=0;if(!low_space)return;
 region_pages((uintptr_t)low_space,(uintptr_t)low_space+LOW_SIZE,resident,dirty,compressed);
 region_pages(ALIAS_START,ALIAS_START+ALIAS_SIZE,resident,dirty,compressed);
}
static void host_diag_init(void) { fprintf(stderr,"[totk] native diagnostics initialized\n"); }
static void host_oe_init(void) { fprintf(stderr,"[totk] application environment: local macOS process\n"); }
static void host_aoc_init(void) { fprintf(stderr,"[totk] add-on content: no DLC\n"); }
static int host_aoc_count(void) { return 0; }
static int host_aoc_list(int *out,int offset,int count) {
 (void)out;if(offset<0||count<0)runtime_fail("invalid add-on content list range");return 0;
}
static void host_error_init(void) { fprintf(stderr,"[totk] error contexts: local diagnostics\n"); }
static void host_fs_init(void) { runtime_fs_init(); }
static void host_pctl_init(void) { fprintf(stderr,"[totk] local application parental controls initialized\n"); }
static void host_abort_init(void) { fprintf(stderr,"[totk] native crash observer initialized\n"); }
static void observed_os_init(uint64_t h,uint64_t args) {
 fprintf(stderr,"[totk] ENTER nnosInitialize\n");((void(*)(uint64_t,uint64_t))runtime_symbol("nnosInitialize"))(h,args);fprintf(stderr,"[totk] RETURN nnosInitialize\n");
}
static void observed_sdk_init(void) { fprintf(stderr,"[totk] ENTER nninitInitializeSdkModule\n");((void(*)(void))runtime_symbol("nninitInitializeSdkModule"))();fprintf(stderr,"[totk] RETURN nninitInitializeSdkModule\n"); }
static void observed_startup(void) { fprintf(stderr,"[totk] ENTER nninitStartup\n");((void(*)(void))runtime_symbol("nninitStartup"))();fprintf(stderr,"[totk] RETURN nninitStartup\n"); }
static void observed_main(void) { fprintf(stderr,"[totk] ENTER nnMain\n");((void(*)(void))runtime_symbol("nnMain"))();fprintf(stderr,"[totk] RETURN nnMain\n"); }
static void cpu_boost(int mode) { if(mode<0||mode>2)runtime_fail("unknown CPU boost mode");fprintf(stderr,"[totk] CPU boost request %d: macOS manages CPU frequency\n",mode); }
/* TOTK_OPERATION_MODE=handheld reports the Switch handheld mode (oe OperationMode 0, PerformanceMode Normal 0) instead of
   docked (1, Boost 1): the game then uses its own handheld settings (1280x720 output, lower internal resolution), which
   Nintendo made for half the docked GPU clock. Read once; any other value stops the start. */
static int handheld_mode(void) {static int v=-1;if(v<0){const char *e=getenv("TOTK_OPERATION_MODE");int h;if(!e||!strcmp(e,"docked"))h=0;else if(!strcmp(e,"handheld"))h=1;else runtime_fail("TOTK_OPERATION_MODE must be docked or handheld");
 fprintf(stderr,"[totk] operation mode reported to the game: %s\n",h?"handheld (performance normal)":"docked (performance boost)");v=h;}return v;}
int runtime_handheld_mode(void){return handheld_mode();}   /* runtime/input.c: the pad is the handheld controller then */
static int operation_mode(void) {return handheld_mode()?0:1;}
static int performance_mode(void) {return handheld_mode()?0:1;}
static int focus_policy;
static void focus_mode(int mode) {if(mode<0||mode>3)runtime_fail("unknown focus handling mode");focus_policy=mode;runtime_startup_checkpoint((uintptr_t)__builtin_return_address(0));fprintf(stderr,"[totk] focus policy stored: %d (window lifecycle pending)\n",focus_policy);}
static int focus_state(void) {return runtime_front_focus();}
static unsigned resume_notification,operation_notification,performance_notification;
static void resume_enabled(unsigned value) {resume_notification=value&255;}
static void operation_enabled(unsigned value) {operation_notification=value&255;}
static void performance_enabled(unsigned value) {performance_notification=value&255;}
static uint32_t performance_configuration[2];
static void performance_config(int mode,int cfg) {if(mode<0||mode>1)runtime_fail("invalid performance mode");performance_configuration[mode]=(uint32_t)cfg;fprintf(stderr,"[totk] performance request mode=%d cfg=%x: native macOS scheduling\n",mode,(uint32_t)cfg);}
/* The game's language: TOTK_LANG as given (a Switch language code), otherwise from a locale: TOTK_LOCALE, which the
   launcher sets to the app's first preferred language (BCP 47, e.g. "pt-BR"; it follows a per-app language), or else
   the Mac's current locale. Brazil maps to "pt-BR" only in the 1.4.3 build, whose RomFS has the official USpt text. */
uint64_t runtime_desired_language(void) {
 const char *explicit=getenv("TOTK_LANG");char code[8]={0};
 if(explicit){if(strlen(explicit)>7)runtime_fail("language code too long");memcpy(code,explicit,strlen(explicit));}
 else {
  char language[16]={0},region[4]={0};const char *tag=getenv("TOTK_LOCALE");
  if(tag&&*tag){
   size_t n=strcspn(tag,"-_");if(n<sizeof(language))memcpy(language,tag,n);
   for(const char *p=tag+n;*p;){p+=strspn(p,"-_");size_t k=strcspn(p,"-_");
    if(k==2&&isalpha((unsigned char)p[0])&&isalpha((unsigned char)p[1])){region[0]=(char)toupper((unsigned char)p[0]);region[1]=(char)toupper((unsigned char)p[1]);}
    p+=k;}
  } else {
   CFLocaleRef locale=CFLocaleCopyCurrent();
   CFStringRef value=CFLocaleGetValue(locale,kCFLocaleLanguageCode);
   if(value)CFStringGetCString(value,language,sizeof(language),kCFStringEncodingUTF8);
   value=CFLocaleGetValue(locale,kCFLocaleCountryCode);
   if(value)CFStringGetCString(value,region,sizeof(region),kCFStringEncodingUTF8);CFRelease(locale);
  }
#ifdef TOTK_GAME_V143
  const char *portuguese=!strcmp(region,"BR")?"pt-BR":"pt";
#else
  const char *portuguese="pt";
#endif
  const char *selected=!strcmp(language,"ja")?"ja":!strcmp(language,"de")?"de":!strcmp(language,"fr")?"fr":!strcmp(language,"es")?"es":!strcmp(language,"it")?"it":!strcmp(language,"nl")?"nl":!strcmp(language,"ru")?"ru":!strcmp(language,"ko")?"ko":!strcmp(language,"zh")?"zh-Hans":!strcmp(language,"pt")?portuguese:"en-US";
  memcpy(code,selected,strlen(selected));
 }
 uint64_t result;memcpy(&result,code,8);return result;
}
static uint32_t observed_region(uint64_t *out,uint64_t size){
 uint8_t *fn=runtime_symbol("_ZN2nn2os21AllocateAddressRegionEPmm");uint32_t r=((uint32_t(*)(uint64_t *,uint64_t))fn)(out,size);
 fprintf(stderr,"[totk] AllocateAddressRegion size=%llx result=%x address=%llx\n",(unsigned long long)size,r,(unsigned long long)(r?0:*out));
 if(r&&getenv("TOTK_TRACE_VAMM")){
  /* PLA layout (disassembly): VammManager global at AllocateAddressRegion-0x34cad0+0xcff430; its +0/+8 are the
     reserved base/size from GetInfo(2/3), +0x40 the AddressRegionManager whose +0/+8/+0x18/+0x28 mirror
     (base, size, base, base+0x3fe00000). */
  uint8_t *vamm=fn-0x34cad0+0xcff430;
  for(unsigned i=0;i<10;i++){uint64_t v;memcpy(&v,vamm+i*8,8);fprintf(stderr,"[totk] VAMM +%02x = %llx\n",i*8,(unsigned long long)v);}
 }
 return r;
}
static uint32_t observed_pages(uint64_t addr,uint64_t size){uint32_t r=((uint32_t(*)(uint64_t,uint64_t))runtime_symbol("_ZN2nn2os19AllocateMemoryPagesEmm"))(addr,size);fprintf(stderr,"[totk] AllocateMemoryPages address=%llx size=%llx result=%x\n",(unsigned long long)addr,(unsigned long long)size,r);return r;}
static int observed_thread_create(void *tt,void *entry,void *arg,void *stack,uint64_t size,int prio,int core){
 fprintf(stderr,"[totk] CreateThread CALL entry=%p arg=%p stack=%p size=%#llx prio=%d core=%d\n",entry,arg,stack,(unsigned long long)size,prio,core);
 int r=((int(*)(void*,void*,void*,void*,uint64_t,int,int))runtime_symbol("_ZN2nn2os12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mii"))(tt,entry,arg,stack,size,prio,core);
 fprintf(stderr,"[totk] CreateThread -> %x\n",r);
 return r;
}
/* PLA: the names the game gives its threads. Wwise names its engine threads (AK::IOThread, AK::OpusDecodingThread,
   ...), so this shows whether its sound engine runs at all. Enabled by PLA_THREAD_NAMES=1. */
static void observed_thread_name(void *tt, const char *name) {
    if (getenv("PLA_THREAD_NAMES")) {
        fprintf(stderr, "[totk-thread] name %s (h=%#x)\n", name ? name : "(null)", runtime_thread_current_handle());
        if (atoi(getenv("PLA_THREAD_NAMES")) > 1) {   /* the guest chain that named it (Wwise's own callers included) */
            uint64_t fp; __asm__ volatile("mov %0, x29" : "=r"(fp));
            uint64_t *thr = fp ? (uint64_t *)(uintptr_t)((uint64_t *)fp)[0] : NULL;
            if (thr) runtime_trace_guest(thr[0], thr[1]);
        }
    }
    void (*real)(void *, const char *) = runtime_symbol("_ZN2nn2os13SetThreadNameEPNS0_10ThreadTypeEPKc");
    if (real) real(tt, name);
}
void *runtime_observer(const char *name) {
 static const struct {const char *name;void *fn;} entries[]={
 {"_ZN2nn2os21AllocateAddressRegionEPmm",observed_region},{"_ZN2nn2os19AllocateMemoryPagesEmm",observed_pages},{"_ZN2nn2os12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mii",observed_thread_create},{"nnosInitialize",observed_os_init},{"nninitInitializeSdkModule",observed_sdk_init},{"nninitStartup",observed_startup},{"nnMain",observed_main}};
 for(size_t i=0;i<sizeof(entries)/sizeof(entries[0]);i++)if(!strcmp(name,entries[i].name))return entries[i].fn;return NULL;
}
/* PLA: the SDK-level AudioOut probes (runtime/audio_out.c). */
uint32_t probe_open_default_audio_out(void *ao, const void *param);
void *runtime_audio_device_override(const char *name);   /* runtime/audio_device_sdk.c */
void *runtime_audio_sdk_override(const char *name);      /* runtime/audio_sdk.c */
uint32_t probe_open_audio_out(void *ao, const char *name, const void *param, void *event);
void *runtime_override(const char *name) {
 static const struct {const char *name;void *fn;} entries[]={
  {"_ZN2nn2oe18GetDesiredLanguageEv",runtime_desired_language},
  {"_ZN2nn2oe27SetPerformanceConfigurationENS0_15PerformanceModeEi",performance_config},
  {"_ZN2nn2oe15SetCpuBoostModeENS0_12CpuBoostModeE",cpu_boost},{"_ZN2nn2oe16GetOperationModeEv",operation_mode},{"_ZN2nn2oe18GetPerformanceModeEv",performance_mode},
  {"_ZN2nn2oe20SetFocusHandlingModeENS0_17FocusHandlingModeE",focus_mode},{"_ZN2nn2oe20GetCurrentFocusStateEv",focus_state},
  {"_ZN2nn2oe28SetResumeNotificationEnabledEb",resume_enabled},{"_ZN2nn2oe42SetOperationModeChangedNotificationEnabledEb",operation_enabled},{"_ZN2nn2oe44SetPerformanceModeChangedNotificationEnabledEb",performance_enabled},
  {"nndiagStartup",host_diag_init},{"_ZN2nn2oe10InitializeEv",host_oe_init},{"_ZN2nn3aoc10InitializeEv",host_aoc_init},
  {"_ZN2nn3aoc17CountAddOnContentEv",host_aoc_count},{"_ZN2nn3aoc16ListAddOnContentEPiii",host_aoc_list},
  {"_ZN2nn3err26InitializeContextWriterApiEv",host_error_init},{"_ZN2nn2fs24InitializeForApplicationEv",host_fs_init},
  {"_ZN2nn4pctl10InitializeEv",host_pctl_init},{"_ZN2nn4diag34InitializeApplicationAbortObserverEv",host_abort_init},
 {"_ZN2nn2os12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mii",observed_thread_create},
 {"_ZN2nn2os13SetThreadNameEPNS0_10ThreadTypeEPKc",observed_thread_name},
 };
 for(size_t i=0;i<sizeof(entries)/sizeof(entries[0]);i++)if(!strcmp(name,entries[i].name))return entries[i].fn;
 void *pla=runtime_pla_override(name);if(pla)return pla;
 /* PLA_NO_AUDIO_OVERRIDE=1 (diagnostic A/B): let the SDK's own nn::audio implementations run, i.e. the audout:u /
    audren IPC path this host also emulates (that is the path by3 plays Wwise through). */
 {void *ad=runtime_audio_device_override(name);if(ad)return ad;}
 /* PLA_NO_AUDIO_SDK_OVERRIDE=1 (diagnostic A/B): skip only the nn::audio *function* overrides, so the SDK module's own
    implementation runs and drives audout:u / audren:u IPC — the path by3 plays Wwise through. */
 if(!getenv("PLA_NO_AUDIO_SDK_OVERRIDE")){void *aa=runtime_audio_sdk_override(name);if(aa)return aa;}
 void *input=runtime_input_override(name);if(input)return input;void *font=runtime_font_override(name);
 if(font){static unsigned n;if(n++<24&&getenv("PLA_FONT_TRACE"))fprintf(stderr,"[totk] font override hit: %s\n",name);}
 if(font)return font;void *codec=runtime_codec_override(name);if(codec)return codec;void *movie=runtime_movie_override(name);if(movie)return movie;void *clock=runtime_time_override(name);if(clock)return clock;void *applet=runtime_applet_override(name);if(applet)return applet;void *swkbd=runtime_swkbd_override(name);if(swkbd)return swkbd;void *account=runtime_account_override(name);if(account)return account;void *friends=runtime_friends_override(name);if(friends)return friends;void *fs=runtime_fs_override(name);if(fs)return fs;void *front=runtime_front_override(name);return front?front:runtime_gfx_override(name);
}
void runtime_core_checkpoint(void);
uint64_t runtime_svc(unsigned num,uint64_t *r,const char *module,uint64_t offset) {
 runtime_core_checkpoint();
 // Successful SVCs occur several times per frame. Keep the detailed trace available
 // for diagnostics without formatting and writing every call during normal play.
 static _Atomic int trace=-1;int enabled=atomic_load_explicit(&trace,memory_order_relaxed);
 if(enabled<0){enabled=getenv("TOTK_TRACE_SVC")!=NULL;atomic_store_explicit(&trace,enabled,memory_order_relaxed);}
 if(enabled)fprintf(stderr,"[totk] SVC 0x%x at %s+0x%llx x0=0x%llx x1=0x%llx x2=0x%llx x3=0x%llx\n",num,module,(unsigned long long)offset,(unsigned long long)r[0],(unsigned long long)r[1],(unsigned long long)r[2],(unsigned long long)r[3]);
 if(svc_thread((int)num,r))return r[0];
 if(runtime_ipc_svc(num,r))return r[0];
 memory_init();
 switch(num) {
 case 0x02:r[0]=runtime_module_permission(r[0],r[1],(unsigned)r[2]);return r[0];
 case 0x03:{
  uint64_t a=r[0],s=r[1];
  /* VAMM pages live in the 64 GiB region above the 39-bit line; attribute ops there are bookkeeping only. */
  if(vamm_space&&s&&a>=(uintptr_t)vamm_space&&s<=VAMM_SIZE&&a+s<=(uintptr_t)vamm_space+VAMM_SIZE)return r[0]=0;
  r[0]=runtime_module_lock_permission(r[0],r[1],(unsigned)r[2],(unsigned)r[3]);return r[0];
}
 case 0x29: {
  uint64_t value=0;unsigned id=(unsigned)r[1];
  switch(id) {
  case 0:value=7;break;case 1:value=0x0ffffffff0000000ULL;break;
  case 2:value=(uintptr_t)vamm_space;break;case 3:value=VAMM_SIZE;break;
  case 4:value=(uintptr_t)heap;break;case 5:value=HEAP_CAP;break;
  case 6:case 21:value=APP_POOL;break;case 7:case 22:value=runtime_image_bytes()+heap_size+alias_bytes;
   { static unsigned polls; if(getenv("TOTK_MEM_LOG")&&(polls++%600)==0) fprintf(stderr,"[totk-mem] GetInfo(%u) total=%#llx used=%#llx image=%#llx heap=%#llx alias=%#llx\n",id,(unsigned long long)APP_POOL,(unsigned long long)value,(unsigned long long)runtime_image_bytes(),(unsigned long long)heap_size,(unsigned long long)alias_bytes); }
   break;
  case 8:value=0;break;case 10:value=0;break;
  case 11:arc4random_buf(&value,8);break;
  case 12:value=ASLR_START;break;case 13:value=ASLR_SIZE;break;
  case 14:value=STACK_START;break;case 15:value=STACK_SIZE;break;
  case 16:value=SYSTEM_RESOURCE_BUDGET;break;case 17:value=sizeof(stack_maps)+sizeof(alias_pages);break;case 18:value=GAME_TITLE_ID;break;
  case 20:value=(uintptr_t)user_exception_context;break;   /* UserExceptionContextAddress: SDK stores e.g. the dying-message region at +0x1c0 */case 23:value=1;break;
  /* AliasRegionExtraSize (18.0.0+): our alias reservation contains no
   * kernel-owned extension outside the range reported by ids 2 and 3. */
  case 28:value=0;break;
  default:fprintf(stderr,"unknown GetInfo id %u\n",id);runtime_fail("GetInfo id not implemented");
  }
  r[0]=0;r[1]=value;return 0;
 }
 case 0x2c:case 0x2d:{uint64_t a=r[0],n=r[1];
  uint64_t start=VAMM_SPACE_AT;
  r[0]=(vamm_space&&a>=start&&a<start+VAMM_SIZE)?vamm_map(num==0x2d,a,n):alias_map(num==0x2d,a,n);
  if(getenv("TOTK_MEM_LOG"))fprintf(stderr,"[totk-mem] %sMapPhysicalMemory %#llx+%#llx -> %#llx alias=%#llx vamm=%#llx remaining=%#llx\n",num==0x2d?"Un":"",(unsigned long long)a,(unsigned long long)n,(unsigned long long)r[0],(unsigned long long)alias_bytes,(unsigned long long)vamm_bytes,(unsigned long long)(APP_POOL_CHECK-runtime_image_bytes()-heap_size-alias_bytes-vamm_bytes));
  if(r[0])fprintf(stderr,"[totk] %sMapPhysicalMemory %#llx+%#llx failed %#llx (alias %#llx used, heap %#llx)\n",num==0x2d?"Un":"",(unsigned long long)a,(unsigned long long)n,(unsigned long long)r[0],(unsigned long long)alias_bytes,(unsigned long long)heap_size);return r[0];}
 case 0x04:case 0x05:r[0]=stack_move(num==5,r[0],r[1],r[2]);return r[0];
 case 0x06: {
  uint64_t addr=r[2],base=addr&~0xfffULL,size=0x1000;unsigned state=0,perm=0,attr=0;
  uintptr_t top=(uintptr_t)pthread_get_stackaddr_np(pthread_self());size_t stack=pthread_get_stacksize_np(pthread_self());
  if(addr>=top-stack&&addr<top){base=top-stack;size=stack;state=0xb;perm=3;}
  else if(addr>=(uintptr_t)heap&&addr<(uintptr_t)heap+heap_size){base=(uintptr_t)heap;size=heap_size;state=5;perm=3;}
  else if(addr>=(uintptr_t)low_space&&addr<(uintptr_t)low_space+LOW_SIZE){base=(uintptr_t)low_space;size=LOW_SIZE;if(addr<(uintptr_t)heap){size=(uintptr_t)heap-base;}else{base=(uintptr_t)heap+heap_size;size=(uintptr_t)low_space+LOW_SIZE-base;}}
  else if(addr>=(uintptr_t)low_space+LOW_SIZE&&addr<ALIAS_START){base=(uintptr_t)low_space+LOW_SIZE;size=ALIAS_START-base;state=0x10;perm=0;}   /* host-owned: Inaccessible */
  else if(!addr){base=0;size=ASLR_START;state=0;perm=0;}
  else if(runtime_module_query(addr,&base,&size,&state,&perm,&attr)){}
  else {state=3;perm=5;}
  pthread_mutex_lock(&memory_lock);
  if(addr>=ALIAS_START&&addr<ALIAS_START+ALIAS_SIZE){
   unsigned p=(unsigned)((addr-ALIAS_START)/4096),low=p,high=p+1;unsigned allocated=alias_pages[p];
   while(low>0&&alias_pages[low-1]==allocated)low--;while(high<sizeof(alias_pages)&&alias_pages[high]==allocated)high++;
   base=ALIAS_START+(uint64_t)low*4096;size=(uint64_t)(high-low)*4096;state=allocated?5:0;perm=allocated?3:0;
  }
  for(unsigned i=0;i<nstack_maps;i++){
   StackMap *m=&stack_maps[i];uint64_t end=m->dst+m->size,send=m->src+m->size;
   if(addr>=m->dst&&addr<end){base=m->dst;size=m->size;state=0xb;perm=3;break;}
   if(addr>=m->src&&addr<send){base=m->src;size=m->size;state=5;perm=0;attr=4;break;}
   if(state==0){uint64_t oldEnd=base+size;if(end<=addr&&end>base)base=end;if(m->dst>addr&&m->dst<oldEnd)oldEnd=m->dst;size=oldEnd-base;}
   else if(state==5){uint64_t oldEnd=base+size;if(send<=addr&&send>base)base=send;if(m->src>addr&&m->src<oldEnd)oldEnd=m->src;size=oldEnd-base;}
  }pthread_mutex_unlock(&memory_lock);
  struct {uint64_t base,size;uint32_t state,attr,perm,ipc,device,pad;} info={base,size,state,attr,perm,0,0,0};
  if(r[0])memcpy((void *)(uintptr_t)r[0],&info,sizeof(info));r[0]=0;r[1]=0;return 0;
 }
 case 0x01:{size_t size=r[1];if(getenv("TOTK_MEM_LOG"))fprintf(stderr,"[totk-mem] SetHeapSize %#zx (image=%#llx alias=%#llx pool=%#llx)\n",size,(unsigned long long)runtime_image_bytes(),(unsigned long long)alias_bytes,(unsigned long long)APP_POOL);
  if(size>HEAP_CAP||size%0x200000||size+runtime_image_bytes()>APP_POOL_CHECK){r[0]=0xd401;return r[0];}
  if(size>heap_size&&mprotect(heap+heap_size,size-heap_size,PROT_READ|PROT_WRITE))runtime_fail("commit heap");
  if(size<heap_size&&mprotect(heap+size,heap_size-size,PROT_NONE))runtime_fail("decommit heap");heap_size=size;r[0]=0;r[1]=(uintptr_t)heap;return 0;}
 case 0x0c: r[0]=0;r[1]=44;return 0;
 case 0x0e: r[0]=0;r[1]=0;r[2]=15;return 0;
 case 0x25: {uint64_t id;pthread_threadid_np(NULL,&id);r[0]=0;r[1]=id;return 0;}
 case 0x10:r[0]=0;return 0;
 case 0x82:case 0x83:case 0x84:case 0x85:case 0x86:case 0x87:case 0x88:case 0x89:case 0x8a:case 0x8b:
 case 0x8c:case 0x8d:case 0x8e:case 0x8f:case 0x90:case 0x91:case 0x92:{
  /* SVC_HOST_GLYPH_LOOKUP_<i>: a glyph-lookup call site (main+0x11f88c..+0x11ff9c, bl 0x123270).
     The host calls the ORIGINAL lookup first — it holds the game's own map walk plus the virtual fallback
     resolver at [this+0x1bd8] that builds glyph records — and substitutes a writable zero block only when
     the lookup really returns NULL (nothing found anywhere), which used to crash the caller. */
  static __thread uint8_t *blank;
  if(!blank){blank=calloc(1,0x100);if(!blank)runtime_fail("glyph blank block");}
  typedef void *(*lookup_fn)(uint64_t,uint64_t,uint64_t,uint64_t);
  lookup_fn fn = (lookup_fn)(void *)runtime_guest_function(0x123270);
  void *r0 = fn ? fn(r[0],r[1],r[2],r[3]) : NULL;
  /* PLA_GLYPH_TRACE=1: every requested character code and glyph index, one line each (glyph-lookups.txt);
     the first ten go to stderr as before. The dialogue text renders as '?'/blank, so this records whether the
     game asks for real CJK codes or already substitutes 0x3f before the lookup. */
  { static int ton=-1; static FILE *tf; static pthread_mutex_t tmu=PTHREAD_MUTEX_INITIALIZER;
    if(ton<0){ ton = getenv("PLA_GLYPH_TRACE")!=NULL; if(ton){ tf=fopen("glyph-lookups.txt","w"); if(tf) setvbuf(tf,NULL,_IOLBF,0); } }
    if(ton&&tf){ pthread_mutex_lock(&tmu); fprintf(tf,"%llx\t%llx\t%p\n",(unsigned long long)r[1],(unsigned long long)r[2],r0); pthread_mutex_unlock(&tmu); } }
  static unsigned nlog;
  if(nlog++<10) fprintf(stderr,"[totk] glyph lookup forwarded char=%#llx idx=%#llx -> %p%s\n",
      (unsigned long long)r[1],(unsigned long long)r[2],r0,r0?"":" (NULL, blank block)");
  if(r0 && getenv("PLA_GLYPH_REC")){ static unsigned nd; if(nd++<4){
      uint8_t rec[0xc0]; mach_vm_size_t got=0;
      if(mach_vm_read_overwrite(mach_task_self(),(uintptr_t)r0,sizeof rec,(uintptr_t)rec,&got)==KERN_SUCCESS&&got==sizeof rec){
        fprintf(stderr,"[totk] glyphrec char=%#llx:",(unsigned long long)r[1]);
        for(int i=0;i<0x60;i+=8){ uint64_t v; memcpy(&v,rec+i,8); fprintf(stderr," %llx",(unsigned long long)v); }
        fprintf(stderr,"\n[totk] glyphrec floats:"); 
        for(int i=0;i<0x30;i+=4){ float f; memcpy(&f,rec+i,4); fprintf(stderr," %.3f",f); }
        fprintf(stderr,"\n"); } } }
  /* PLA_GLYPH_REC_CJK=1: the same record dump for a CJK character and the ellipsis, plus the 0x40 bytes a
     record's two pointers lead to — the dialogue's CJK glyphs never reach the atlas, so this compares what the
     game returns for a char it rasterises (0x2026) and one it does not. */
  if(r0 && getenv("PLA_GLYPH_REC_CJK") && (r[1]>=0x3000 || r[1]==0x2026)){ static unsigned nd; if(nd++<8){
      uint8_t rec[0xc0]; mach_vm_size_t got=0;
      if(mach_vm_read_overwrite(mach_task_self(),(uintptr_t)r0,sizeof rec,(uintptr_t)rec,&got)==KERN_SUCCESS&&got==sizeof rec){
        fprintf(stderr,"[totk] cjkrec char=%#llx idx=%#llx:",(unsigned long long)r[1],(unsigned long long)r[2]);
        for(int i=0;i<0xb8;i+=8){ uint64_t v; memcpy(&v,rec+i,8); fprintf(stderr," %llx",(unsigned long long)v); }
        fprintf(stderr,"\n[totk] cjkrec floats:");
        for(int i=0;i<0x30;i+=4){ float f; memcpy(&f,rec+i,4); fprintf(stderr," %.3f",f); }
        fprintf(stderr,"\n");
        for(int k=0;k<2;k++){ uint64_t p; memcpy(&p,rec+(k?0x58:0x10),8);
          if(p>0x1000){ uint8_t d[0x40]; mach_vm_size_t g2=0;
            if(mach_vm_read_overwrite(mach_task_self(),(uintptr_t)p,sizeof d,(uintptr_t)d,&g2)==KERN_SUCCESS&&g2==sizeof d){
              fprintf(stderr,"[totk] cjkrec +%#x -> %#llx:",k?0x58:0x10,(unsigned long long)p);
              for(int i=0;i<0x40;i+=8){ uint64_t v; memcpy(&v,d+i,8); fprintf(stderr," %llx",(unsigned long long)v); }
              fprintf(stderr,"\n"); } } } } } }
  r[0] = r0 ? (uintptr_t)r0 : (uintptr_t)blank;
  return 0; }
 case 0x26: {uint32_t result=0;if(r[1]&&r[2]>=4)memcpy(&result,(void *)(uintptr_t)r[1],4);fprintf(stderr,"[totk] guest Break reason=%llx result=%x\n",(unsigned long long)r[0],result);
  if(r[1]&&r[2]>0&&r[2]<4096){char buf[2048];mach_vm_size_t got=0;if(mach_vm_read_overwrite(mach_task_self(),r[1],r[2]<2047?r[2]:2047,(uintptr_t)buf,&got)==KERN_SUCCESS&&got){buf[got]=0;fprintf(stderr,"[totk] Break message: %s\n",buf);}}
  {char buf[512];mach_vm_size_t got=0;uint64_t msg=(uintptr_t)user_exception_context+0x1c0;   /* nn::os::UserExceptionInfo's dying-message slot */
   if(mach_vm_read_overwrite(mach_task_self(),msg,sizeof buf-1,(uintptr_t)buf,&got)==KERN_SUCCESS&&got){buf[sizeof buf-1]=0;if(buf[0])fprintf(stderr,"[totk] exception context message: %s\n",buf);}}
  for(unsigned q=0;q<8;q++){if(r[q]>0x1000){char buf[120];mach_vm_size_t got=0;if(mach_vm_read_overwrite(mach_task_self(),r[q],sizeof buf-1,(uintptr_t)buf,&got)==KERN_SUCCESS&&got){buf[sizeof buf-1]=0;int pr=1;for(int k=0;k<60&&buf[k];k++){unsigned char c=buf[k];if(c<0x20||c>0x7e){if(c)pr=0;break;}}if(pr&&buf[0])fprintf(stderr,"[totk]   x%u = \"%s\"\n",q,buf);}}}
  runtime_trace_guest(r[29],r[30]);if(r[0]&0x80000000ULL)return 0;
  if(getenv("PLA_SKIP_SDK_BREAK")){fprintf(stderr,"[totk] PLA_SKIP_SDK_BREAK: continuing past this assert\n");return 0;}
  runtime_fail("original SDK Break");}
 case 0x27: {size_t n=r[1];if(n>65536)runtime_fail("debug string too long");fwrite((void *)(uintptr_t)r[0],1,n,stderr);r[0]=0;return 0;}
 case 0x0b:{int64_t ns=(int64_t)r[0];if(ns>0){struct timespec t={ns/1000000000,ns%1000000000};nanosleep(&t,NULL);}r[0]=0;return 0;}
 default:fprintf(stderr,"[totk] unimplemented SVC 0x%x at %s+%#llx\n",num,module,(unsigned long long)offset);if(num==0x1f)fprintf(stderr,"[totk] named port %.12s\n",(const char *)(uintptr_t)r[1]);runtime_trace_guest(r[29],r[30]);runtime_fail("system service not implemented");return 0;
 }
}
