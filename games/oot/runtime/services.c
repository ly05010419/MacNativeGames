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
#include "game_data.h"
#include "game_version.h"
#include <string.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include "output_memory.h"   /* HEAP_CAP / LOW_SIZE from the application pool */

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
/* TOTK_DRAM_8GB (Mac, docs/mods.md): the 8 GB mode for big texture packs. Unset or "0" leaves the retail layout. */
static int dram_8gb(void){
#if TARGET_OS_IPHONE
 return 0;
#else
 static int v=-1;if(v<0){const char *e=getenv("TOTK_DRAM_8GB");v=e?(*e&&strcmp(e,"0")):runtime_texture_pack_active();
  if(!v&&runtime_texture_pack_active())runtime_fail("texture pack requires the 8 GB layout; remove TOTK_DRAM_8GB=0");}return v;
#endif
}
#if TARGET_OS_IPHONE
#define LOW_SIZE (HEAP_OFF+HEAP_CAP)   /* iOS: no slack after the heap cap; address space is scarce */
#else
#define LOW_SIZE output_low_size(dram_8gb()?8ULL<<30:6ULL<<30, HEAP_OFF, HEAP_CAP)   /* Preserve the old reservation unless the expanded heap needs more address space. */
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
#ifdef GAME_NS2
/* GetInfo 16: the system resource size the NPDM declares (process.txt, scripts/prepare_runtime.py), as Horizon answers it.
   Donkey Kong Bananza declares 0: virtual address memory is off and the game sets a heap. A non-zero answer sends the
   Switch 2 SDK's VammManager into mapping its memory up front (GetInfo 33) - found by seedds/nxnative (8dd0c98a, MIT).
   TOTK and BotW keep the 32 MiB they were brought up with (open: whether their NPDMs declare the same). */
#define SYSTEM_RESOURCE_BUDGET runtime_game_number("process.txt","SYSTEM_RESOURCE_SIZE")
#else
#define SYSTEM_RESOURCE_BUDGET (32ULL<<20)
#endif
#ifdef GAME_BOTW
#define GAME_PROGRAM_ID 0x01007ef00011e000ULL   /* GetInfo 18 (ProgramId): Breath of the Wild */
#elif defined(GAME_NS2)
#define GAME_PROGRAM_ID GAME_TITLE_ID   /* a Switch 2 game (game_version.h) */
#else
#define GAME_PROGRAM_ID 0x0100f2c0115b6000ULL
#endif
/* The heap region (GetInfo 5) holds at least the application pool (GetInfo 6): no heap can be bigger than all the
   memory, and a pool the output / scene overlays grew (runtime_output_res_extra_bytes) needs its heap to grow with it -
   the 8 GB mode with the scene at a 4K canvas broke nninitStartup at the fixed 7 GiB (PR #1470, after PR #1515). The
   region is address space only (PROT_NONE until the game maps it), so no host memory size decides it; LOW_SIZE grows
   with it. iOS grows no pool: its cap stays the retail one. */
#define HEAP_CAP output_heap_capacity(dram_8gb()?7ULL<<30:4ULL<<30, APP_POOL)
#define APP_POOL app_pool()
#ifdef GAME_NS2
#define APP_CORE_MASK 0x3f   /* GetInfo 0: the Switch 2's six application cores (threads.c NCORE) */
#else
#define APP_CORE_MASK 7
#endif
#ifdef GAME_NS2
/* The Switch 2 leaves a game 9-10 GiB of its 12 GiB (seedds/nxnative's Bananza profile: 9; xFractals/pokopia-native's Pokopia
   profile: 10, which is taken; neither measured on the console): with a
   Switch 1 pool (3.2 GiB) Bananza's heaps did not fit (AudioSystemHeap creation returned null). Address space only until
   the game maps it. */
#define APP_POOL_DEFAULT 0x280000000ULL
#else
#define APP_POOL_DEFAULT (dram_8gb()?0x1b3400000ULL:0xcd500000ULL)
#endif
static uint64_t app_pool(void){static uint64_t v;if(!v){const char *e=getenv("TOTK_APP_POOL");uint64_t extra=(runtime_output_res_extra_bytes()+0x1fffff)&~0x1fffffULL;
 v=e?strtoull(e,NULL,0):APP_POOL_DEFAULT+extra;if(!e&&extra)fprintf(stderr,"[totk] application pool %#llx: +%llu MiB for the bigger texture heaps (TOTK_OUTPUT_RES, TOTK_RENDER_RES)\n",(unsigned long long)v,(unsigned long long)(extra>>20));}return v;}
/* TOTK: application memory pool reported to the game (default: retail 3.2 GiB, or 6.8 GiB as in an emulator's 8 GB DRAM mode with TOTK_DRAM_8GB; TOTK_OUTPUT_RES adds what its HeapDef overlay gives the texture heaps) */
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
/* An iOS process cannot swap anonymous guest pages to disk. NS2's retail heap
   grows beyond a 6 GB phone's RAM even before its first world draw. A shared
   file mapping preserves the guest's addresses and bytes while letting Darwin
   write back and reclaim idle pages. It is scratch memory, never save data.
   Enabled for Bananza on iOS after CPU/Metal alias/page-in and real-world
   device checks; other games and the Mac retain anonymous backing.
   Do not enable file backing for OOT on the Mac: guest memory becomes NVN memory
   pools, which Metal residency wires (IOGPUSysMemory::wire). For a file-backed
   page that wire is a page-in through APFS while the system's periodic sync()
   msyncs the same mapped file. 2026-11-06, with an uncommitted default that backed
   the heap and MapPhysicalMemory with a file: a game thread sat in
   IOGPUDevice::group_add_resources -> wireVirtual -> apfs_vnop_pagein waiting for
   a krwlock held by launchd's sync (apfs_vfsop_sync -> ubc_msync), which waited
   for the pages being wired. Both uninterruptible: kill -9 and shutdown hung until
   a forced restart (spindump OcarinaOfTime_2026-11-06-214913.hang, pid 92339,
   about 51 s after launch). Frame rate was equal with and without it. */
/* OOT_MEM_CLASSES=<seconds> (diagnostic, off by default; user 2026-11-10: why the phone needs this much memory with the
   shaders on disk). One bounded line every <seconds> s, and one more each time the remaining allowance falls under another
   100 MB step below 600 MB, so the last line before a memory kill is in the log. It splits what the limit counts: the
   footprint and its graphics ledger (Metal's memory, mapped or not), the guest regions' dirty + compressed bytes (alias = the
   physical memory the game maps, heap, stack), the five largest VM tags of the process, malloc in use, and Metal's
   allocated size with the part the storage textures hold (host copies of guest textures, mtl_mem_classes). The numbers
   overlap (Metal buffers over guest memory count in Metal's size and in the guest regions; the ledger and the tags too):
   they are not to be added up. */
#include <malloc/malloc.h>
#include <pthread.h>
void mtl_mem_classes(uint64_t *allocated, uint64_t *storage, uint64_t *storage_n);
typedef struct { uint64_t dirty, compressed, resident; } mc_bytes;
static void mc_walk(uint64_t lo, uint64_t hi, mc_bytes *range, mc_bytes *tags) {
 mach_vm_address_t addr=lo;natural_t depth=0;uint64_t page=vm_page_size;
 while(addr<hi){
  mach_vm_address_t at=addr;mach_vm_size_t size=0;vm_region_submap_info_data_64_t info;mach_msg_type_number_t n=VM_REGION_SUBMAP_INFO_COUNT_64;
  if(mach_vm_region_recurse(mach_task_self(),&at,&size,&depth,(vm_region_recurse_info_t)&info,&n)!=KERN_SUCCESS||at>=hi)break;
  if(info.is_submap){depth++;continue;}
  mc_bytes b={(uint64_t)info.pages_dirtied*page,(uint64_t)info.pages_swapped_out*page,(uint64_t)info.pages_resident*page};
  if(range){range->dirty+=b.dirty;range->compressed+=b.compressed;range->resident+=b.resident;}
  if(tags){mc_bytes *t=&tags[info.user_tag&255];t->dirty+=b.dirty;t->compressed+=b.compressed;t->resident+=b.resident;}
  addr=at+size;
 }
}
#define MC_MB(x) ((unsigned long long)(x)>>20)
static void mc_report(const char *why, long long left) {
 task_vm_info_data_t vm;memset(&vm,0,sizeof vm);mach_msg_type_number_t n=TASK_VM_INFO_COUNT;
 task_info(mach_task_self(),TASK_VM_INFO,(task_info_t)&vm,&n);
 mc_bytes alias={0},hp={0},stack={0},tags[256];memset(tags,0,sizeof tags);
 mc_walk(ALIAS_START,ALIAS_START+ALIAS_SIZE,&alias,NULL);
 mc_walk((uintptr_t)heap,(uintptr_t)heap+HEAP_CAP,&hp,NULL);
 mc_walk(STACK_START,STACK_START+STACK_SIZE,&stack,NULL);
 mc_walk(0,~0ULL,NULL,tags);
 int top[5]={-1,-1,-1,-1,-1};
 for(int t=0;t<256;t++){uint64_t v=tags[t].dirty+tags[t].compressed;if(!v)continue;
  for(int k=0;k<5;k++)if(top[k]<0||v>tags[top[k]].dirty+tags[top[k]].compressed){memmove(&top[k+1],&top[k],(size_t)(4-k)*sizeof top[0]);top[k]=t;break;}}
 char tagtxt[160];int o=0;tagtxt[0]=0;
 for(int k=0;k<5&&top[k]>=0&&o<(int)sizeof tagtxt;k++)o+=snprintf(tagtxt+o,sizeof tagtxt-(size_t)o,"%s%d:%llu",k?" ":"",top[k],MC_MB(tags[top[k]].dirty+tags[top[k]].compressed));
 malloc_statistics_t ms;memset(&ms,0,sizeof ms);malloc_zone_statistics(NULL,&ms);
 uint64_t metal=0,storage=0,storage_n=0;mtl_mem_classes(&metal,&storage,&storage_n);
 fprintf(stderr,"[mem-classes] %s: footprint %llu MB (peak %llu), left %lld MB | graphics ledger %llu MB (+%llu compressed) | guest dirty+compressed MB: alias %llu, heap %llu, stack %llu (resident %llu / %llu / %llu) | VM tags dirty+compressed MB %s | malloc in use %llu MB | Metal allocated %llu MB, storage textures %llu MB in %llu | internal %llu, compressed %llu MB\n",
  why,MC_MB(vm.phys_footprint),MC_MB(vm.ledger_phys_footprint_peak),left<0?-1LL:(long long)(left>>20),MC_MB(vm.ledger_tag_graphics_footprint),MC_MB(vm.ledger_tag_graphics_footprint_compressed),
  MC_MB(alias.dirty+alias.compressed),MC_MB(hp.dirty+hp.compressed),MC_MB(stack.dirty+stack.compressed),MC_MB(alias.resident),MC_MB(hp.resident),MC_MB(stack.resident),
  tagtxt,MC_MB(ms.size_in_use),MC_MB(metal),MC_MB(storage),(unsigned long long)storage_n,MC_MB(vm.internal),MC_MB(vm.compressed));
}
static void *mc_thread(void *arg) {
 unsigned every=(unsigned)(uintptr_t)arg;long long last_step=1LL<<40;time_t last=0;int lines=0;
 for(;;){
  sleep(1);
  task_vm_info_data_t vm;memset(&vm,0,sizeof vm);mach_msg_type_number_t n=TASK_VM_INFO_COUNT;
  long long left=task_info(mach_task_self(),TASK_VM_INFO,(task_info_t)&vm,&n)==KERN_SUCCESS&&vm.limit_bytes_remaining?(long long)vm.limit_bytes_remaining:-1;
  long long step=left>=0&&left<(600LL<<20)?left/(100LL<<20):1LL<<40;time_t now=time(NULL);
  int low=step<last_step;
  if(low||now-last>=(time_t)every){mc_report(low?"low":"every",left);last=now;if(low)last_step=step;if(++lines>=400)return NULL;}
 }
}
static void mem_classes_start(void) {
 const char *e=getenv("OOT_MEM_CLASSES");if(!e||!*e||!strcmp(e,"0"))return;
 unsigned every=(unsigned)atoi(e);if(every<1)every=5;
 pthread_t t;if(!pthread_create(&t,NULL,mc_thread,(void *)(uintptr_t)every))pthread_detach(t);
 fprintf(stderr,"[mem-classes] on: a line every %u s and at each 100 MB step under 600 MB left (OOT_MEM_CLASSES)\n",every);
}
static void heap_file_backing(void) {
 const char *e=getenv("TOTK_FILE_HEAP");
#if TARGET_OS_IPHONE && defined(GAME_DKB)
 if(e&&!atoi(e))return;
#else
 if(!e||!atoi(e))return;
#endif
 const char *tmp=getenv("TMPDIR");if(!tmp||!*tmp)tmp="/tmp";
 char path[PATH_MAX];int len=snprintf(path,sizeof path,"%s/totk-heap-XXXXXX",tmp);
 if(len<0||(size_t)len>=sizeof path)runtime_fail("guest heap backing path");
 int fd=mkstemp(path);if(fd<0)runtime_fail("create guest heap backing");
 /* The mapping holds the vnode after close. Unlink now so a crash cannot
    leave a multi-GB cache behind, and a later launch always starts at zero. */
 if(unlink(path)||ftruncate(fd,(off_t)HEAP_CAP)){
  int saved=errno;close(fd);errno=saved;runtime_fail("size guest heap backing");
 }
 void *p=mmap(heap,HEAP_CAP,PROT_NONE,MAP_SHARED|MAP_FIXED,fd,0);
 int saved=errno;close(fd);errno=saved;
 if(p!=heap)runtime_fail("map guest heap backing");
 fprintf(stderr,"[totk-mem] guest heap: shared scratch-file backing, %llu MiB address space; bytes retained on page-out, file removed on exit\n",(unsigned long long)(HEAP_CAP>>20));
}
static int guest_files_init(void);   /* iOS: the guest block in files (below memory_lock) */
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
 heap=low_space+HEAP_OFF;if(!guest_files_init())heap_file_backing();
 fprintf(stderr,"[totk] guest layout: alias %#llx+%#llx stack %#llx+%#llx heap %#llx (cap %#llx)\n",(unsigned long long)ALIAS_START,ALIAS_SIZE,(unsigned long long)STACK_START,STACK_SIZE,(unsigned long long)(uintptr_t)heap,HEAP_CAP);
 mem_classes_start();
}
/* Scoped stack moves: independent host backing, copy back on unmap.
 * Logical accounting is 4 KiB; Darwin protections are rounded to 16 KiB.
 * The leading/trailing partial source pages cannot be isolated on this host.
 */
static pthread_mutex_t memory_lock=PTHREAD_MUTEX_INITIALIZER;
/* The game's memory in files on iOS (user 2026-11-10: "照 nxnative 做，只限 iOS"), after seedds/nxnative hle_svc.c and its
   docs/reference/memory.md "iOS ends the app once the guest's touched pages reach its memory limit" (b54968e7). Every page
   the game touches is anonymous memory, which phys_footprint charges and iOS cannot write out: on an iPhone 14 the guest
   regions held 3,357 MB of a 4,095 MB limit at the title and the app was ended at 114 s (res-mem-4), and a smaller pool
   reported to the game crashes it (7 and 5 GiB). In sparse files, unlinked at once and mapped shared over the reservation at
   the same addresses, the pages are out of the footprint and the kernel writes them out when it needs the room. On when the
   game's pool is larger than the room to the memory limit at start; OOT_GUEST_FILE=1 / 0 forces it on / off. 1 GB files: the
   stall at the system's periodic flush grows with the file (nxnative: 148 ms with one 20 GB file, 4 ms with 1 GB files).
   What the game gives back goes PROT_NONE and its blocks are punched out: the heap's at once, the alias region's by a sweep
   every 10 s of the host pages given back over a whole interval (the game maps and unmaps about 0.5 GB of the same ranges a
   second). A page that keeps its bytes (wired under a Metal no-copy buffer) is zeroed when it is opened again; one that reads
   0 is left clean. A thread stops the game (exit 87) before the disk fills: under OOT_GUEST_FILE_MIN_FREE (2 GB) of free space
   less what the dirty pages may still take, as a full disk leaves dirty pages that can never be written out. The Mac keeps
   anonymous memory: there file pages the GPU had wired deadlocked against APFS's sync (heap_file_backing's note). */
#if TARGET_OS_IPHONE
#include <os/proc.h>
#include <sys/mount.h>
#include <sys/stat.h>
enum { GF_MAX = 64 };
static int gf_tab[GF_MAX],gf_n;static uint64_t gf_chunk,gf_block,gf_min_free=2ULL<<30;static _Atomic uint64_t gf_punched;
static uint64_t alias_rel[ALIAS_SIZE_MAX/0x4000/64+1],alias_aged[ALIAS_SIZE_MAX/0x4000/64+1];   /* host pages given back since / before the last sweep */
static _Atomic uint64_t alias_hw,heap_hw;   /* the highest alias offset mapped and the largest heap: what the dirty count reads */
static char gf_dir[1024];
static void gf_punch(uint64_t lo,uint64_t hi){
 while(lo<hi){
  uint64_t off=lo-(uintptr_t)low_space,k=off/gf_chunk,in=off-k*gf_chunk,fsz=(int)k==gf_n-1?gf_block-k*gf_chunk:gf_chunk,len=hi-lo<fsz-in?hi-lo:fsz-in;
  fpunchhole_t ph;memset(&ph,0,sizeof ph);ph.fp_offset=(off_t)in;ph.fp_length=(off_t)len;
  if(!fcntl(gf_tab[k],F_PUNCHHOLE,&ph))atomic_fetch_add(&gf_punched,len);
  else{static _Atomic int nred;if(atomic_fetch_add(&nred,1)<4)fprintf(stderr,"[totk-mem] 【红】guest file: F_PUNCHHOLE file %llu +%#llx+%#llx failed (errno %d): the range keeps its disk blocks\n",(unsigned long long)k,(unsigned long long)in,(unsigned long long)len,errno);}
  lo+=len;
 }
}
/* Zero the 4 KiB pages of [a, a+n) that do not read 0: a hole reads 0 and stays clean, where a memset would dirty it */
static void gf_zero(uint8_t *a,uint64_t n){
 for(uint8_t *e=a+n;a<e;a+=4096){uint64_t any=0;
  for(size_t i=0;!any&&i<4096;i+=64){uint64_t w[8];memcpy(w,a+i,sizeof w);any=w[0]|w[1]|w[2]|w[3]|w[4]|w[5]|w[6]|w[7];}
  if(any)memset(a,0,4096);}
}
/* Punch the alias host pages given back before the last sweep and not mapped again since, then age the ones given back since.
   Under memory_lock, so no MapPhysicalMemory opens a page while it is punched */
static void gf_sweep(void){
 pthread_mutex_lock(&memory_lock);
 for(size_t w=0;w<sizeof alias_aged/sizeof alias_aged[0];w++){uint64_t a=alias_aged[w];
  while(a){unsigned b=(unsigned)__builtin_ctzll(a),e=b;while(e<64&&(a>>e&1))e++;
   gf_punch(ALIAS_START+((uint64_t)w*64+b)*0x4000,ALIAS_START+((uint64_t)w*64+e)*0x4000);a&=e==64?0:~0ULL<<e;}
  alias_rel[w]&=~alias_aged[w];alias_aged[w]=alias_rel[w];}
 pthread_mutex_unlock(&memory_lock);
}
/* The disk's free space with what the system can purge (caches, offloadable data): -1 when the query fails */
static double gf_free_important(void){
 CFURLRef u=CFURLCreateFromFileSystemRepresentation(NULL,(const UInt8 *)gf_dir,(CFIndex)strlen(gf_dir),true);double r=-1;CFTypeRef v=NULL;int64_t x;
 if(!u)return r;
 if(CFURLCopyResourcePropertyForKey(u,kCFURLVolumeAvailableCapacityForImportantUsageKey,&v,NULL)&&v){
  if(CFGetTypeID(v)==CFNumberGetTypeID()&&CFNumberGetValue((CFNumberRef)v,kCFNumberSInt64Type,&x)&&x>=0)r=(double)x;CFRelease(v);}
 CFRelease(u);return r;
}
/* The files' modified pages (mincore over what the game has opened): what the disk may still have to take; -1 on failure */
static double gf_dirty(char *vec){
 uint64_t all=0,range[2][2]={{(uintptr_t)low_space,HEAP_OFF+atomic_load(&heap_hw)},{ALIAS_START,atomic_load(&alias_hw)}};
 for(int i=0;i<2;i++){uint64_t n=(range[i][1]+0x3fff)/0x4000;if(!n)continue;
  if(mincore((void *)(uintptr_t)range[i][0],(size_t)(n*0x4000),vec))return -1;
  for(uint64_t j=0;j<n;j++)all+=(vec[j]&MINCORE_MODIFIED)!=0;}
 return (double)all*0x4000;
}
/* Every second the disk's room less the dirty pages (counted every 10 s), as nxnative's guest_file_thread: under the minimum
   the free space with what the system can purge is asked (at most every 10 s), and the game goes on on that while the disk's
   own room stays above half the minimum. Every 10 s the alias sweep and one line, with the page-ins (thrashing shows there) */
static void *gf_thread(void *arg){
 (void)arg;const double G=1073741824.0;char *vec=malloc((size_t)(gf_block/0x4000)+1);double dirty=-1,imp=-1,count_ms=0;unsigned imp_at=0;int lines=0,purge_told=0;
 pthread_set_qos_class_self_np(QOS_CLASS_UTILITY,0);
 for(unsigned sec=1;;sec++){
  sleep(1);
  if(sec%10==0)gf_sweep();
  if(sec%10==1&&vec){uint64_t t0=clock_gettime_nsec_np(CLOCK_MONOTONIC);dirty=gf_dirty(vec);count_ms=(double)(clock_gettime_nsec_np(CLOCK_MONOTONIC)-t0)/1e6;}
  struct statfs fs;int ok=!fstatfs(gf_tab[0],&fs);double avail=ok?(double)fs.f_bavail*fs.f_bsize:-1,need=dirty>0?dirty:0,room=avail-need;
  if(ok&&gf_min_free&&room<(double)gf_min_free){
   if(imp<0||sec-imp_at>=10){imp=gf_free_important();imp_at=sec;}
   if(room<gf_min_free/2.0||imp-need<(double)gf_min_free){
    fprintf(stderr,"[totk-mem] 【红】guest file: %.1f GB free on its disk (%.1f with what the system can purge), %.1f GB of it still to be taken by dirty pages: under %.1f GB left, stopping the game\n",avail/G,imp/G,need/G,gf_min_free/G);
    _exit(87);}
   if(!purge_told){purge_told=1;fprintf(stderr,"[totk-mem] guest file: %.1f GB free on its disk, less %.1f GB for dirty pages, is under %.1f GB, but %.1f GB with what the system can purge: going on (stopping under %.1f GB of it)\n",avail/G,need/G,gf_min_free/G,imp/G,gf_min_free/2.0/G);}
  }
  if(sec%10||lines>=360)continue;
  double held=0;struct stat st;for(int i=0;i<gf_n;i++)if(!fstat(gf_tab[i],&st))held+=(double)st.st_blocks*512.0;
  task_vm_info_data_t vi;memset(&vi,0,sizeof vi);mach_msg_type_number_t n=TASK_VM_INFO_COUNT;if(task_info(mach_task_self(),TASK_VM_INFO,(task_info_t)&vi,&n)!=KERN_SUCCESS)memset(&vi,0,sizeof vi);
  struct rusage ru;memset(&ru,0,sizeof ru);getrusage(RUSAGE_SELF,&ru);
  lines++;
  fprintf(stderr,"[totk-mem] %sguest file: %.2f GB on disk, %.2f GB dirty in memory (%.0f ms to count), %.2f GB given back as holes; page-ins %ld, block reads %ld; footprint %.2f GB (internal %.2f, compressed %.2f, peak %.2f), file pages mapped %.2f GB; %.1f GB free on its disk\n",
   gf_min_free&&ok&&avail-need<gf_min_free+G?"【红】":"",held/G,need/G,count_ms,atomic_load(&gf_punched)/G,(long)ru.ru_majflt,(long)ru.ru_inblock,
   vi.phys_footprint/G,vi.internal/G,vi.compressed/G,vi.ledger_phys_footprint_peak/G,vi.external/G,avail/G);
 }
}
/* The iOS reservation (stack, heap, then the alias region, memory_init) in GF files: 1 when the guest is in them */
static int guest_files_init(void){
 const double G=1073741824.0;const char *e=getenv("OOT_GUEST_FILE"),*mf=getenv("OOT_GUEST_FILE_MIN_FREE");
 uint64_t room=os_proc_available_memory();int want=e&&*e?atoi(e)>0:room&&APP_POOL>room;
 fprintf(stderr,"[totk-mem] guest memory: pool %.2f GB, room to the memory limit %.2f GB, OOT_GUEST_FILE %s: %s\n",APP_POOL/G,room/G,e&&*e?e:"unset",want?"in files":"anonymous");
 if(!want)return 0;
 if(mf&&*mf)gf_min_free=strtoull(mf,NULL,0);
 if(ALIAS_START!=(uintptr_t)low_space+LOW_SIZE){fprintf(stderr,"[totk-mem] 【红】guest memory: anonymous (the alias region does not follow the heap region)\n");return 0;}
 gf_block=LOW_SIZE+ALIAS_SIZE;gf_chunk=1ULL<<30;
 if((gf_block+gf_chunk-1)/gf_chunk>GF_MAX)gf_chunk=((gf_block+GF_MAX-1)/GF_MAX+0x3fff)&~0x3fffULL;
 int n=(int)((gf_block+gf_chunk-1)/gf_chunk),made=0,ok=1;char why[600]="";struct statfs fs;
 /* the app's own temporary folder (in its data container, on the phone's one APFS volume): what nxnative uses */
 if(!confstr(_CS_DARWIN_USER_TEMP_DIR,gf_dir,sizeof gf_dir)||!gf_dir[0]){const char *tmp=getenv("TMPDIR");snprintf(gf_dir,sizeof gf_dir,"%s",tmp&&*tmp?tmp:"/tmp");}
 const char *sep=gf_dir[strlen(gf_dir)-1]=='/'?"":"/";
 for(int k=0;ok&&k<n;k++){
  char path[PATH_MAX];snprintf(path,sizeof path,"%s%sguest-%d-%d.mem",gf_dir,sep,(int)getpid(),k);unlink(path);
  const int fl=O_RDWR|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC;
  int fd=open_dprotected_np(path,fl,3,0,0600);   /* protection class C: readable and writable from the first unlock on, also while locked */
  if(fd<0&&errno!=EEXIST)fd=open(path,fl,0600);
  if(fd<0){snprintf(why,sizeof why,"cannot create %s, errno %d",path,errno);ok=0;break;}
  gf_tab[made++]=fd;
  if(unlink(path)){snprintf(why,sizeof why,"cannot unlink %s, errno %d",path,errno);ok=0;break;}
  int cls=fcntl(fd,F_GETPROTECTIONCLASS);if(cls!=3&&cls!=4){fcntl(fd,F_SETPROTECTIONCLASS,3);cls=fcntl(fd,F_GETPROTECTIONCLASS);}
  if(cls!=3&&cls!=4){snprintf(why,sizeof why,"protection class %d, not C (3) or D (4)",cls);ok=0;}
 }
 if(ok&&fstatfs(gf_tab[0],&fs)){snprintf(why,sizeof why,"cannot read the disk's free space, errno %d",errno);ok=0;}
 if(ok){double avail=(double)fs.f_bavail*fs.f_bsize,imp=avail<gf_min_free?gf_free_important():-1;
  if(avail<gf_min_free&&imp<(double)gf_min_free){snprintf(why,sizeof why,"%.1f GB free on the disk (%.1f with what the system can purge), under %.1f",avail/G,imp/G,gf_min_free/G);ok=0;}}
 for(int k=0;ok&&k<n;k++){uint64_t sz=k==n-1?gf_block-(uint64_t)k*gf_chunk:gf_chunk;uint8_t *at=low_space+(uint64_t)k*gf_chunk;
  if(ftruncate(gf_tab[k],(off_t)sz)){snprintf(why,sizeof why,"cannot size file %d, errno %d",k,errno);ok=0;}
  else if(mmap(at,(size_t)sz,PROT_NONE,MAP_SHARED|MAP_FIXED,gf_tab[k],0)!=at){snprintf(why,sizeof why,"cannot map file %d, errno %d",k,errno);ok=0;}}
 if(!ok){
  for(int k=0;k<made;k++)close(gf_tab[k]);
  if(mmap(low_space,(size_t)gf_block,PROT_NONE,MAP_PRIVATE|MAP_ANON|MAP_FIXED,-1,0)!=(void *)low_space)runtime_fail("reserve the guest block again after the guest files failed");
  fprintf(stderr,"[totk-mem] 【红】guest memory: anonymous (guest files: %s)\n",why);return 0;}
 gf_n=n;
 double avail=(double)fs.f_bavail*fs.f_bsize,imp=gf_free_important(),most=avail>imp?avail:imp;
 fprintf(stderr,"[totk-mem] guest memory: %d files of %.2f GB in %s (unlinked, sparse, shared, protection class C), %.2f GB in all, on the %s volume, %.1f GB free on it (%.1f with what the system can purge); holes punched where the game gives memory back\n",
  n,gf_chunk/G,gf_dir,gf_block/G,fs.f_fstypename,avail/G,imp/G);
 /* the files take up to what the game touches: at most about its pool while what it gives back is punched out */
 if(gf_min_free&&most<(double)(APP_POOL+gf_min_free))fprintf(stderr,"[totk-mem] 【红】guest file: the files may take up to the memory pool, %.1f GB, and %.1f GB is free: the game is stopped if under %.1f GB is left\n",APP_POOL/G,most/G,gf_min_free/G);
 if(strcmp(fs.f_fstypename,"apfs")||!(fs.f_flags&MNT_LOCAL))fprintf(stderr,"[totk-mem] 【红】guest file: not on a local APFS volume, where it may not be sparse, or be slow\n");
 pthread_t t;if(!pthread_create(&t,NULL,gf_thread,NULL))pthread_detach(t);else fprintf(stderr,"[totk-mem] 【红】guest file: no thread to watch the disk (nothing stops the game when its disk fills)\n");
 return 1;
}
/* alias_map gives a whole host page back: with the files it goes PROT_NONE and is marked for the sweep, and keeps its bytes
   until it is punched, so alias_stale stays set for its pages; an anonymous mapping over it would turn it back into memory
   the footprint charges as soon as the game maps it again. 1 when handled here */
static int gf_alias_release(uint64_t page){
 if(!gf_n)return 0;
 if(mprotect((void *)(uintptr_t)page,0x4000,PROT_NONE))runtime_fail("decommit physical alias page");
 uint64_t h=(page-ALIAS_START)/0x4000;alias_rel[h>>6]|=1ULL<<(h&63);return 1;
}
/* alias_map opens host pages [lo, hi) again: in use, not to be punched; and the dirty count reads up to them */
static void gf_alias_open(uint64_t lo,uint64_t hi){
 if(!gf_n)return;
 for(uint64_t h=(lo-ALIAS_START)/0x4000,e=(hi-ALIAS_START)/0x4000;h<e;h++){uint64_t m=~(1ULL<<(h&63));alias_rel[h>>6]&=m;alias_aged[h>>6]&=m;}
 if(hi-ALIAS_START>atomic_load(&alias_hw))atomic_store(&alias_hw,hi-ALIAS_START);
}
/* A 4 KiB alias page mapped again over old bytes reads 0, as a fresh pool page: over the files only when it does not already */
static void gf_alias_zero(void *a){if(gf_n)gf_zero(a,4096);else memset(a,0,4096);}
/* SetHeapSize: what the heap gives back is punched out at once; what it grows back over reads 0 again (a wired page keeps its
   bytes through the punch) */
static void gf_heap_resized(uint64_t was,uint64_t now){
 if(!gf_n)return;
 if(now<was){gf_punch((uintptr_t)heap+now,(uintptr_t)heap+was);return;}
 uint64_t hw=atomic_load(&heap_hw);
 if(now>was&&hw>was)gf_zero(heap+was,(now<hw?now:hw)-was);
 if(now>hw)atomic_store(&heap_hw,now);
}
#else
static int guest_files_init(void){return 0;}   /* the Mac keeps anonymous guest memory */
static int gf_alias_release(uint64_t page){(void)page;return 0;}
static void gf_alias_open(uint64_t lo,uint64_t hi){(void)lo;(void)hi;}
static void gf_alias_zero(void *a){memset(a,0,4096);}
static void gf_heap_resized(uint64_t was,uint64_t now){(void)was;(void)now;}
#endif
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
   if(gf_alias_release(page))continue;   /* iOS, the game in files: PROT_NONE, punched by the sweep */
   if(mmap((void *)page,0x4000,PROT_NONE,MAP_FIXED|MAP_PRIVATE|MAP_ANON,-1,0)!=(void *)page)runtime_fail("decommit physical alias page");
   memset(alias_stale+p,0,4);}}
 }else if(fresh){
  uint64_t low=addr&~0x3fffULL,high=(addr+size+0x3fff)&~0x3fffULL;
  if(mprotect((void *)low,high-low,PROT_READ|PROT_WRITE))runtime_fail("commit physical alias pages");
  gf_alias_open(low,high);
  for(unsigned i=0;i<count;i++)if(!alias_pages[first+i]){   /* new pages only: zero-filled like fresh pool pages */
   if(alias_stale[first+i]){gf_alias_zero((void *)(addr+(uint64_t)i*4096));alias_stale[first+i]=0;}
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
/* Diagnostic "flag oot_opmode <0|1>": what the game is told from then on (0 handheld, 1 docked), with the "operation mode
   changed" notification a Switch sends when it is docked or undocked. -1: the mode of the start. */
static _Atomic int reported_mode_override=-1;
void runtime_operation_mode_report(int docked){atomic_store(&reported_mode_override,docked?1:0);runtime_oe_push_message(30);fprintf(stderr,"[oot] operation mode reported as %s (notification sent)\n",docked?"docked":"handheld");}
static int operation_mode(void) {int o=atomic_load(&reported_mode_override);return o>=0?o:handheld_mode()?0:1;}
static int performance_mode(void) {int o=atomic_load(&reported_mode_override);return o>=0?o:handheld_mode()?0:1;}
/* The settings panel's quality profile (user 2026-11-10): the same report, kept as the mode in force. notify 0 before the
   guest starts (it reads the mode at its start), 1 while it runs. runtime_operation_mode_docked: the mode in force now. */
void runtime_operation_mode_choose(int docked,int notify){atomic_store(&reported_mode_override,docked?1:0);if(notify)runtime_oe_push_message(30);fprintf(stderr,"[oot] quality profile: %s%s\n",docked?"docked":"handheld",notify?" (notification sent)":" (from the start)");}
int runtime_operation_mode_docked(void){return operation_mode();}
/* nn::oe::GetDisplayVersion: control.nacp's DisplayVersion (char[16]) through IApplicationFunctions, which the host does not
   serve, so the SDK read a null proxy (Donkey Kong Bananza at its start; from games/xc3's copy of this file). The build's
   version (game_version.h); a dump folder has no control data to read the real one from. */
static void display_version(char *out) {if(!out)runtime_fail("display version output pointer");memset(out,0,16);snprintf(out,16,"%s",TOTK_GAME_VERSION);}
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
#ifdef GAME_DKB
void *runtime_bananza_memcpy(void *,const void *,size_t);
#elif defined(GAME_POKOPIA)
void *runtime_pokopia_memcpy(void *,const void *,size_t);
#endif
void *runtime_observer(const char *name) {
#ifdef GAME_DKB
 if(!strcmp(name,"memcpy"))return runtime_bananza_memcpy;
#elif defined(GAME_POKOPIA)
 if(!strcmp(name,"memcpy"))return runtime_pokopia_memcpy;
#endif
 static const struct {const char *name;void *fn;} entries[]={
 {"_ZN2nn2os21AllocateAddressRegionEPmm",observed_region},{"_ZN2nn2os19AllocateMemoryPagesEmm",observed_pages},{"nnosInitialize",observed_os_init},{"nninitInitializeSdkModule",observed_sdk_init},{"nninitStartup",observed_startup},{"nnMain",observed_main}};
 for(size_t i=0;i<sizeof(entries)/sizeof(entries[0]);i++)if(!strcmp(name,entries[i].name))return entries[i].fn;return NULL;
}
static int observed_pthread_mutex_lock(void *mutex) {
 static unsigned n;uint64_t words[6];memcpy(words,mutex,sizeof words);
 if(n++<10)fprintf(stderr,"[oot-mutex] %p words=%llx %llx %llx %llx %llx %llx thread=%p\n",mutex,(unsigned long long)words[0],(unsigned long long)words[1],(unsigned long long)words[2],(unsigned long long)words[3],(unsigned long long)words[4],(unsigned long long)words[5],((void *(*)(void))runtime_symbol("_ZN2nn2os16GetCurrentThreadEv"))());
 return ((int(*)(void *))runtime_symbol("pthread_mutex_lock"))(mutex);
}
static void host_abort_message(const char *condition,const char *file,const char *function,int line,const uint32_t *result,const void *exception,const char *format,void *args) {
 (void)exception;(void)args;
 fprintf(stderr,"[oot-abort] condition=%s file=%s function=%s line=%d result=%#x format=%s\n",condition?condition:"-",file?file:"-",function?function:"-",line,result?*result:0,format?format:"-");
 runtime_trace_guest((uintptr_t)__builtin_frame_address(0),(uintptr_t)__builtin_return_address(0));
 runtime_fail("SDK abort (original diagnostic above)");
}
void *runtime_override(const char *name) {
 if(getenv("TOTK_TRACE_MUTEX")&&!strcmp(name,"pthread_mutex_lock"))return observed_pthread_mutex_lock;
 if(!strcmp(name,"_ZN2nn4diag6detail10VAbortImplEPKcS3_S3_iPKNS_6ResultEPKNS_2os17UserExceptionInfoES3_RSt9__va_list"))return host_abort_message;
 static const struct {const char *name;void *fn;} entries[]={
  {"_ZN2nn2oe18GetDesiredLanguageEv",desired_language},
  {"_ZN2nn2oe17GetDisplayVersionEPNS0_14DisplayVersionE",display_version},
  {"_ZN2nn2oe27SetPerformanceConfigurationENS0_15PerformanceModeEi",performance_config},
  {"_ZN2nn2oe15SetCpuBoostModeENS0_12CpuBoostModeE",cpu_boost},{"_ZN2nn2oe16GetOperationModeEv",operation_mode},{"_ZN2nn2oe18GetPerformanceModeEv",performance_mode},
  {"_ZN2nn2oe20SetFocusHandlingModeENS0_17FocusHandlingModeE",focus_mode},{"_ZN2nn2oe20GetCurrentFocusStateEv",focus_state},
  {"_ZN2nn2oe28SetResumeNotificationEnabledEb",resume_enabled},{"_ZN2nn2oe42SetOperationModeChangedNotificationEnabledEb",operation_enabled},{"_ZN2nn2oe44SetPerformanceModeChangedNotificationEnabledEb",performance_enabled},
  {"nndiagStartup",host_diag_init},{"_ZN2nn2oe10InitializeEv",host_oe_init},{"_ZN2nn3aoc10InitializeEv",host_aoc_init},
  {"_ZN2nn3aoc17CountAddOnContentEv",host_aoc_count},{"_ZN2nn3aoc16ListAddOnContentEPiii",host_aoc_list},
  {"_ZN2nn3err26InitializeContextWriterApiEv",host_error_init},{"_ZN2nn2fs24InitializeForApplicationEv",host_fs_init},
  {"_ZN2nn4pctl10InitializeEv",host_pctl_init},{"_ZN2nn4diag34InitializeApplicationAbortObserverEv",host_abort_init}};
 for(size_t i=0;i<sizeof(entries)/sizeof(entries[0]);i++)if(!strcmp(name,entries[i].name))return entries[i].fn;
 #ifdef GAME_NS2
 void *ns2=runtime_ns2_override(name);if(ns2)return ns2;
 void *codec2=runtime_ns2_codec_override(name);if(codec2)return codec2;
 void *aout=runtime_ns2_audio_out_override(name);if(aout)return aout;
 void *opusms=runtime_ns2_opus_ms_override(name);if(opusms)return opusms;
 void *offline=runtime_offline_override(name);if(offline)return offline;
 #endif
 void *input=runtime_input_override(name);if(input)return input;void *font=runtime_font_override(name);if(font)return font;void *codec=runtime_codec_override(name);if(codec)return codec;void *movie=runtime_movie_override(name);if(movie)return movie;void *clock=runtime_time_override(name);if(clock)return clock;void *applet=runtime_applet_override(name);if(applet)return applet;void *swkbd=runtime_swkbd_override(name);if(swkbd)return swkbd;void *account=runtime_account_override(name);if(account)return account;void *fs=runtime_fs_override(name);if(fs)return fs;void *front=runtime_front_override(name);return front?front:runtime_gfx_override(name);
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
  case 0:value=APP_CORE_MASK;break;case 1:value=0x0ffffffff0000000ULL;break;
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
  /* Horizon's other types (Atmosphere kern_svc_info.cpp) answer 0 here: no thread tick count (25), no SVC permission bits
     (26), no I/O region (27). Switch 2 SDKs ask two more (from seedds/nxnative 8dd0c98a and c4b0618c, MIT): 33 is the
     kernel's page size, which nn::ro::ProtectRelro takes as the alignment of the RELRO bounds (value - 1 as a mask, 0
     aborts); 29-32 are the remote region (nnosInitialize builds nn::os::detail::RemoteRegionManager over [29, 29 + 30),
     GetRemoteMemoryUsage returns 31 and 32), whose only user is the GPU driver's boot through nv::InitializeGraphics,
     which the host replaces: no remote region, no remote memory. */
  case 25:case 26:case 27:value=0;break;
  case 29:case 30:case 31:case 32:value=0;break;
  case 33:value=0x1000;break;
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
  if(size<heap_size&&mprotect(heap+size,heap_size-size,PROT_NONE))runtime_fail("decommit heap");gf_heap_resized(heap_size,size);heap_size=size;r[0]=0;r[1]=(uintptr_t)heap;return 0;}
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
#include "mem_scan.h"   /* Mac diagnostics: mscan commands (memory scan, write watch) */
