/* Horizon services reached by the current TOTK startup. Unknown operations fail. */
#include "runtime.h"
#include <TargetConditionals.h>
#include "ipc.h"
#include <mach/mach.h>
#include "mach_vm_compat.h"
#include <stdio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <ctype.h>
#include "game_language.h"
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
#ifdef GAME_BOTW
#define GAME_PROGRAM_ID 0x01007ef00011e000ULL   /* GetInfo 18 (ProgramId): Breath of the Wild */
#elif defined(GAME_WONDER)
#define GAME_PROGRAM_ID 0x010015100b514000ULL   /* Super Mario Bros. Wonder */
#else
#define GAME_PROGRAM_ID 0x0100f2c0115b6000ULL
#endif
#define HEAP_CAP (4ULL<<30)   /* 1.4.3 sets a 2.2 GiB heap during nninitStartup */
#define APP_POOL app_pool()
static uint64_t app_pool(void){static uint64_t v;if(!v){const char *e=getenv("TOTK_APP_POOL");uint64_t extra=(runtime_output_res_extra_bytes()+0x1fffff)&~0x1fffffULL;
 v=e?strtoull(e,NULL,0):0xcd500000ULL+extra;if(!e&&extra)fprintf(stderr,"[totk] application pool %#llx: +%llu MiB for the bigger texture heaps (TOTK_OUTPUT_RES, TOTK_RENDER_RES)\n",(unsigned long long)v,(unsigned long long)(extra>>20));}return v;}
/* TOTK: application memory pool reported to the game (default: retail 3.2 GiB; TOTK_OUTPUT_RES adds what its HeapDef overlay gives the texture heaps) */
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
/* Reserve size bytes in a free gap (2 MiB aligned). The end of the map is
   not reported, so the last gap is only tried; FIXED fails there with KERN_NO_SPACE when it reaches past the end. */
static uint8_t *reserve_in_gap_from(uint64_t size,int log,uint64_t prev){
 for(;;){
  mach_vm_address_t r=prev;mach_vm_size_t rsize=0;vm_region_basic_info_data_64_t info;mach_msg_type_number_t n=VM_REGION_BASIC_INFO_COUNT_64;mach_port_t obj=MACH_PORT_NULL;
  int more=mach_vm_region(mach_task_self(),&r,&rsize,VM_REGION_BASIC_INFO_64,(vm_region_info_t)&info,&n,&obj)==KERN_SUCCESS;
  if(obj!=MACH_PORT_NULL)mach_port_deallocate(mach_task_self(),obj);
  uint64_t start=(prev+GUEST_ALIGN-1)&~(GUEST_ALIGN-1),end=more&&r<GUEST_ADDRESS_LIMIT?r:GUEST_ADDRESS_LIMIT;
  if(end>start&&end-start>=(256ULL<<20)&&log)fprintf(stderr,"[totk-mem] free %#llx..%s%#llx\n",(unsigned long long)start,more?"":"(end of map, at most) ",(unsigned long long)end);
  if(end>start&&end-start>=size){uint8_t *p=reserve_fixed(start,size);if(p)return p;}
  if(!more||r>=GUEST_ADDRESS_LIMIT||!rsize||r+rsize<=prev)return NULL;
  prev=r+rsize;
 }
}
static uint8_t *reserve_in_gap(uint64_t size,int log){
#if TARGET_OS_IPHONE
 return reserve_in_gap_from(size,log,1ULL<<32);
#else
 return reserve_in_gap_from(size,log,LOW_SEARCH_FROM);
#endif
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
  alias_space=NULL;
  uint64_t want=alias_requested(),least=getenv("TOTK_ALIAS_GIB")?want:(8ULL<<30);
  for(uint64_t g=want;!low_space;g-=4ULL<<30){
   for(uint64_t at=LOW_SEARCH_FROM;at+LOW_SIZE+g<=GUEST_ADDRESS_LIMIT&&!low_space;at+=1ULL<<30)
    if((low_space=reserve_fixed(at,LOW_SIZE+g)))alias_len=g;
   if(g<least+(4ULL<<30))break;
  }
  if(!low_space){
   /* ASLR can leave only ~13 GiB holes, or a 14 GiB hole missed by the old
      1 GiB stride. Preserve successful layouts; only a previously failing
      startup queries exact gaps, then tries separate stack/heap and alias
      reservations (already supported by the original fixed layout). */
   for(uint64_t g=want;!low_space;g-=4ULL<<30){
    if((low_space=reserve_in_gap(LOW_SIZE+g,0))){alias_space=low_space+LOW_SIZE;alias_len=g;break;}
    /* Keep low below alias: GetInfo's ASLR span and QueryMemory use that order,
       including an explicit TOTK_ALIAS_GIB smaller than LOW_SIZE. */
    uint8_t *separate_low=reserve_in_gap(LOW_SIZE,0);
    if(separate_low){
     if((alias_space=reserve_in_gap_from(g,0,(uintptr_t)separate_low+LOW_SIZE))){low_space=separate_low;alias_len=g;break;}
     mach_vm_deallocate(mach_task_self(),(mach_vm_address_t)(uintptr_t)separate_low,LOW_SIZE);
    }
    if(g<least+(4ULL<<30))break;
   }
  }
  if(!low_space)runtime_fail(getenv("TOTK_ALIAS_GIB")?"reserve guest stack, heap and the TOTK_ALIAS_GIB alias region below 2^39":"reserve guest stack, heap and alias regions below 2^39");
  if(!alias_space)alias_space=low_space+LOW_SIZE;
 }
#endif
 heap=low_space+HEAP_OFF;
 fprintf(stderr,"[totk] guest layout: alias %#llx+%#llx stack %#llx+%#llx heap %#llx (cap %#llx)\n",(unsigned long long)ALIAS_START,ALIAS_SIZE,(unsigned long long)STACK_START,STACK_SIZE,(unsigned long long)(uintptr_t)heap,HEAP_CAP);
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
 if(!inHeap&&!inAlias)return 0xdc01;
 pthread_mutex_lock(&memory_lock);
 if(inAlias){unsigned first=(unsigned)((src-aliasStart)/4096);for(uint64_t i=0;i<size/4096;i++)if(!alias_pages[first+i]){pthread_mutex_unlock(&memory_lock);return 0xdc01;}}

 unsigned found=nstack_maps;for(unsigned i=0;i<nstack_maps;i++)if(stack_maps[i].dst==dst&&stack_maps[i].src==src&&stack_maps[i].size==size)found=i;
 uint64_t lo=dst&~0x3fffULL,hi=(dst+size+0x3fff)&~0x3fffULL;
 uint64_t slo=(src+0x3fff)&~0x3fffULL,shi=(src+size)&~0x3fffULL;
 if(unmap){
  if(found==nstack_maps){pthread_mutex_unlock(&memory_lock);return 0xd401;}
  if(shi>slo&&mprotect((void *)slo,shi-slo,PROT_READ|PROT_WRITE))runtime_fail("restore moved stack source");
  memcpy((void *)src,(const void *)dst,size);stack_maps[found]=stack_maps[--nstack_maps];
  for(uint64_t page=lo;page<hi;page+=0x4000){int used=0;for(unsigned i=0;i<nstack_maps;i++)if(overlaps(page,0x4000,stack_maps[i].dst,stack_maps[i].size))used=1;
   if(!used&&mprotect((void *)page,0x4000,PROT_NONE))runtime_fail("decommit moved stack");}
 }else{
  if(nstack_maps==4096){pthread_mutex_unlock(&memory_lock);return 0xd401;}
  for(unsigned i=0;i<nstack_maps;i++)if(overlaps(dst,size,stack_maps[i].dst,stack_maps[i].size)||overlaps(src,size,stack_maps[i].src,stack_maps[i].size)){pthread_mutex_unlock(&memory_lock);return 0xdc01;}
  if(mprotect((void *)lo,hi-lo,PROT_READ|PROT_WRITE))runtime_fail("commit moved stack");memcpy((void *)dst,(const void *)src,size);
  if(shi>slo&&mprotect((void *)slo,shi-slo,PROT_NONE))runtime_fail("protect moved stack source");
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
 if(!unmap&&fresh+alias_bytes+heap_size+runtime_image_bytes()>APP_POOL){
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
#ifdef GAME_BOTW
void runtime_aoc_announce(void); int runtime_aoc_count(void); int runtime_aoc_list(int *out,int offset,int count);   /* aoc_romfs.c */
static void host_aoc_init(void) { runtime_aoc_announce(); }   /* aoc_romfs.c: the DLC volumes (botw.md) */
static int host_aoc_count(void) { return runtime_aoc_count(); }
static int host_aoc_list(int *out,int offset,int count) {
 if(offset<0||count<0)runtime_fail("invalid add-on content list range");return runtime_aoc_list(out,offset,count);
}
#else
static void host_aoc_init(void) { fprintf(stderr,"[totk] add-on content: no DLC\n"); }
static int host_aoc_count(void) { return 0; }
static int host_aoc_list(int *out,int offset,int count) {
 (void)out;if(offset<0||count<0)runtime_fail("invalid add-on content list range");return 0;
}
#endif
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
   the Mac's current locale, mapped by game_language.h (regional variants as on the console: en-GB, fr-CA, es-419, and pt-BR
   only in the 1.4.3 build and in Breath of the Wild 1.9.0, whose RomFS have the official USpt text). */
static uint64_t desired_language(void) {
 const char *explicit=getenv("TOTK_LANG");char code[8]={0};
 if(explicit){if(strlen(explicit)>7)runtime_fail("language code too long");memcpy(code,explicit,strlen(explicit));}
 else {
  char language[16]={0},script[8]={0},region[4]={0};const char *tag=getenv("TOTK_LOCALE");
  if(tag&&*tag){
   size_t n=strcspn(tag,"-_");if(n<sizeof(language))memcpy(language,tag,n);
   for(const char *p=tag+n;*p;){p+=strspn(p,"-_");size_t k=strcspn(p,"-_");
    if(k==2&&isalpha((unsigned char)p[0])&&isalpha((unsigned char)p[1])){region[0]=(char)toupper((unsigned char)p[0]);region[1]=(char)toupper((unsigned char)p[1]);region[2]=0;}
    if(k==3&&isdigit((unsigned char)p[0])&&isdigit((unsigned char)p[1])&&isdigit((unsigned char)p[2]))memcpy(region,p,3);   /* UN M.49, e.g. es-419 */
    if(k==4&&!script[0]&&isalpha((unsigned char)p[0])&&isalpha((unsigned char)p[3])){for(size_t i=0;i<4;i++)script[i]=(char)(i?tolower((unsigned char)p[i]):toupper((unsigned char)p[i]));}
    p+=k;}
  } else {
   CFLocaleRef locale=CFLocaleCopyCurrent();
   CFStringRef value=CFLocaleGetValue(locale,kCFLocaleLanguageCode);
   if(value)CFStringGetCString(value,language,sizeof(language),kCFStringEncodingUTF8);
   value=CFLocaleGetValue(locale,kCFLocaleScriptCode);
   if(value)CFStringGetCString(value,script,sizeof(script),kCFStringEncodingUTF8);
   value=CFLocaleGetValue(locale,kCFLocaleCountryCode);
   if(value)CFStringGetCString(value,region,sizeof(region),kCFStringEncodingUTF8);CFRelease(locale);
  }
#if defined(TOTK_GAME_V143) || defined(GAME_BOTW190)
  const int brazil=1;
#else
  const int brazil=0;
#endif
  const char *selected=game_language(language,script,region,brazil);
  memcpy(code,selected,strlen(selected));
 }
 uint64_t result;memcpy(&result,code,8);
 fprintf(stderr,"[totk] language: %s (TOTK_LANG=%s TOTK_LOCALE=%s)\n",code,explicit?explicit:"-",getenv("TOTK_LOCALE")?getenv("TOTK_LOCALE"):"-");
 return result;
}
/* One line per game allocation came in bursts of 1000-3000 while the world streams (each a write to the log),
   so only failures are logged unless TOTK_TRACE_ALLOC=1. */
static int trace_alloc(void){static int v=-1;if(v<0){const char *e=getenv("TOTK_TRACE_ALLOC");v=e&&*e&&strcmp(e,"0");}return v;}
static uint32_t observed_region(uint64_t *out,uint64_t size){
 uint8_t *fn=runtime_symbol("_ZN2nn2os21AllocateAddressRegionEPmm");uint32_t r=((uint32_t(*)(uint64_t *,uint64_t))fn)(out,size);
 if(r||trace_alloc())fprintf(stderr,"[totk] AllocateAddressRegion size=%llx result=%x address=%llx\n",(unsigned long long)size,r,(unsigned long long)(r?0:*out));
 if(r&&getenv("TOTK_TRACE_VAMM")){
  uint8_t *sdk=fn-0x12ca60;uint64_t root;memcpy(&root,sdk+0xd16bb8,8);uint64_t *v=(void *)(uintptr_t)(root+0x418);
  for(unsigned i=0;i<10;i++)fprintf(stderr,"[totk] VAMM +%x=%llx\n",i*8,(unsigned long long)v[i]);
  if(v[8]){uint64_t *a=(void *)(uintptr_t)v[8];for(unsigned i=0;i<12;i++)fprintf(stderr,"[totk] allocator +%x=%llx\n",i*8,(unsigned long long)a[i]);if(a[2]){uint64_t *node=(void *)(uintptr_t)a[2];for(unsigned i=0;i<12;i++)fprintf(stderr,"[totk] node +%x=%llx\n",i*8,(unsigned long long)node[i]);}}
 }
 return r;
}
static uint32_t observed_pages(uint64_t addr,uint64_t size){uint32_t r=((uint32_t(*)(uint64_t,uint64_t))runtime_symbol("_ZN2nn2os19AllocateMemoryPagesEmm"))(addr,size);if(r||trace_alloc())fprintf(stderr,"[totk] AllocateMemoryPages address=%llx size=%llx result=%x\n",(unsigned long long)addr,(unsigned long long)size,r);return r;}
void *runtime_observer(const char *name) {
 static const struct {const char *name;void *fn;} entries[]={
 {"_ZN2nn2os21AllocateAddressRegionEPmm",observed_region},{"_ZN2nn2os19AllocateMemoryPagesEmm",observed_pages},{"nnosInitialize",observed_os_init},{"nninitInitializeSdkModule",observed_sdk_init},{"nninitStartup",observed_startup},{"nnMain",observed_main}};
 for(size_t i=0;i<sizeof(entries)/sizeof(entries[0]);i++)if(!strcmp(name,entries[i].name))return entries[i].fn;return NULL;
}
void *runtime_override(const char *name) {
 static const struct {const char *name;void *fn;} entries[]={
  {"_ZN2nn2oe18GetDesiredLanguageEv",desired_language},
  {"_ZN2nn2oe27SetPerformanceConfigurationENS0_15PerformanceModeEi",performance_config},
  {"_ZN2nn2oe15SetCpuBoostModeENS0_12CpuBoostModeE",cpu_boost},{"_ZN2nn2oe16GetOperationModeEv",operation_mode},{"_ZN2nn2oe18GetPerformanceModeEv",performance_mode},
  {"_ZN2nn2oe20SetFocusHandlingModeENS0_17FocusHandlingModeE",focus_mode},{"_ZN2nn2oe20GetCurrentFocusStateEv",focus_state},
  {"_ZN2nn2oe28SetResumeNotificationEnabledEb",resume_enabled},{"_ZN2nn2oe42SetOperationModeChangedNotificationEnabledEb",operation_enabled},{"_ZN2nn2oe44SetPerformanceModeChangedNotificationEnabledEb",performance_enabled},
  {"nndiagStartup",host_diag_init},{"_ZN2nn2oe10InitializeEv",host_oe_init},{"_ZN2nn3aoc10InitializeEv",host_aoc_init},
  {"_ZN2nn3aoc17CountAddOnContentEv",host_aoc_count},{"_ZN2nn3aoc16ListAddOnContentEPiii",host_aoc_list},
  {"_ZN2nn3err26InitializeContextWriterApiEv",host_error_init},{"_ZN2nn2fs24InitializeForApplicationEv",host_fs_init},
  {"_ZN2nn4pctl10InitializeEv",host_pctl_init},{"_ZN2nn4diag34InitializeApplicationAbortObserverEv",host_abort_init}};
 for(size_t i=0;i<sizeof(entries)/sizeof(entries[0]);i++)if(!strcmp(name,entries[i].name))return entries[i].fn;
 void *input=runtime_input_override(name);if(input)return input;void *font=runtime_font_override(name);if(font)return font;void *codec=runtime_codec_override(name);if(codec)return codec;void *movie=runtime_movie_override(name);if(movie)return movie;void *clock=runtime_time_override(name);if(clock)return clock;void *applet=runtime_applet_override(name);if(applet)return applet;
#ifdef GAME_WONDER   /* the offline network and console-identity services (games/wonder/runtime/offline_host.c) */
 void *offline=runtime_offline_override(name);if(offline)return offline;
#endif
 void *swkbd=runtime_swkbd_override(name);if(swkbd)return swkbd;void *account=runtime_account_override(name);if(account)return account;void *fs=runtime_fs_override(name);if(fs)return fs;void *front=runtime_front_override(name);return front?front:runtime_gfx_override(name);
}
void runtime_core_checkpoint(void);
#ifdef GAME_BOTW
/* Breath of the Wild (botw.md): the preemption point is also after every SVC. A thread the SVC woke competes for a core at
   once (threads.c wake_queue); when it beats this one on this core, Horizon runs it before the SVC returns. */
uint64_t runtime_svc_inner(unsigned num,uint64_t *r,const char *module,uint64_t offset);
uint64_t runtime_svc(unsigned num,uint64_t *r,const char *module,uint64_t offset){uint64_t v=runtime_svc_inner(num,r,module,offset);runtime_core_checkpoint();return v;}
#define runtime_svc runtime_svc_inner
#endif
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
 case 0x03:r[0]=runtime_module_lock_permission(r[0],r[1],(unsigned)r[2],(unsigned)r[3]);return r[0];
 case 0x29: {
  uint64_t value=0;unsigned id=(unsigned)r[1];
  switch(id) {
  case 0:value=7;break;case 1:value=0x0ffffffff0000000ULL;break;
  case 2:value=ALIAS_START;break;case 3:value=ALIAS_SIZE;break;
  case 4:value=(uintptr_t)heap;break;case 5:value=HEAP_CAP;break;
  case 6:case 21:value=APP_POOL;break;case 7:case 22:value=runtime_image_bytes()+heap_size+alias_bytes;
   { static unsigned polls; if(getenv("TOTK_MEM_LOG")&&(polls++%600)==0) fprintf(stderr,"[totk-mem] GetInfo(%u) total=%#llx used=%#llx image=%#llx heap=%#llx alias=%#llx\n",id,(unsigned long long)APP_POOL,(unsigned long long)value,(unsigned long long)runtime_image_bytes(),(unsigned long long)heap_size,(unsigned long long)alias_bytes); }
   break;
  case 8:value=0;break;case 10:value=0;break;
  case 11:arc4random_buf(&value,8);break;
  case 12:value=ASLR_START;break;case 13:value=ASLR_SIZE;break;
  case 14:value=STACK_START;break;case 15:value=STACK_SIZE;break;
  case 16:value=SYSTEM_RESOURCE_BUDGET;break;case 17:value=sizeof(stack_maps)+sizeof(alias_pages);break;case 18:value=GAME_PROGRAM_ID;break;
  case 20:value=(uintptr_t)user_exception_context;break;   /* UserExceptionContextAddress: SDK stores e.g. the dying-message region at +0x1c0 */case 23:value=1;break;
  /* AliasRegionExtraSize (18.0.0+): our alias reservation contains no
   * kernel-owned extension outside the range reported by ids 2 and 3. */
  case 28:value=0;break;
  default:fprintf(stderr,"unknown GetInfo id %u\n",id);runtime_fail("GetInfo id not implemented");
  }
  r[0]=0;r[1]=value;return 0;
 }
 case 0x2c:case 0x2d:{uint64_t a=r[0],n=r[1];r[0]=alias_map(num==0x2d,a,n);
  if(getenv("TOTK_MEM_LOG"))fprintf(stderr,"[totk-mem] %sMapPhysicalMemory %#llx+%#llx -> %#llx alias=%#llx remaining=%#llx\n",num==0x2d?"Un":"",(unsigned long long)a,(unsigned long long)n,(unsigned long long)r[0],(unsigned long long)alias_bytes,(unsigned long long)(APP_POOL-runtime_image_bytes()-heap_size-alias_bytes));
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
  if(size>HEAP_CAP||size%0x200000||size+runtime_image_bytes()>APP_POOL){r[0]=0xd401;return r[0];}
  if(size>heap_size&&mprotect(heap+heap_size,size-heap_size,PROT_READ|PROT_WRITE))runtime_fail("commit heap");
  if(size<heap_size&&mprotect(heap+size,heap_size-size,PROT_NONE))runtime_fail("decommit heap");heap_size=size;r[0]=0;r[1]=(uintptr_t)heap;return 0;}
 case 0x0c: r[0]=0;r[1]=44;return 0;
 case 0x0e: r[0]=0;r[1]=0;r[2]=15;return 0;
 case 0x25: {uint64_t id;pthread_threadid_np(NULL,&id);r[0]=0;r[1]=id;return 0;}
 case 0x10:r[0]=0;return 0;
 /* SynchronizePreemptionState: the SDK calls it after clearing ThreadLocalRegion disable_count (+0x100) when interrupt_flag
    (+0x102) was set by a preemption that had to wait (threads.c totk_preempt_signal). The switch itself happened at this
    SVC's entry (runtime_core_checkpoint); here the flag is cleared, as Horizon does. */
 case 0x36:{volatile uint16_t *tlr=pthread_getspecific(runtime_keys[2]);if(tlr)tlr[0x102/2]=0;r[0]=0;return 0;}
 case 0x26: {uint32_t result=0;if(r[1]&&r[2]>=4)memcpy(&result,(void *)(uintptr_t)r[1],4);fprintf(stderr,"[totk] guest Break reason=%llx result=%x\n",(unsigned long long)r[0],result);runtime_trace_guest(r[29],r[30]);if(r[0]&0x80000000ULL)return 0;runtime_fail("original SDK Break");}
 case 0x27: {size_t n=r[1];if(n>65536)runtime_fail("debug string too long");fwrite((void *)(uintptr_t)r[0],1,n,stderr);r[0]=0;return 0;}
 case 0x0b:{int64_t ns=(int64_t)r[0];if(ns>0){struct timespec t={ns/1000000000,ns%1000000000};nanosleep(&t,NULL);}r[0]=0;return 0;}
 default:fprintf(stderr,"[totk] unimplemented SVC 0x%x at %s+%#llx\n",num,module,(unsigned long long)offset);if(num==0x1f)fprintf(stderr,"[totk] named port %.12s\n",(const char *)(uintptr_t)r[1]);runtime_trace_guest(r[29],r[30]);runtime_fail("system service not implemented");return 0;
 }
}
#ifdef GAME_BOTW
#undef runtime_svc
#endif
