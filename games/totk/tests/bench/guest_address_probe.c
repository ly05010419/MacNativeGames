/* Exercise the actual production memory_init against captured Darwin VM gaps. */
#include "mach_vm_compat.h"
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <setjmp.h>
#define LIMIT (1ULL<<39)
#define GIB (1ULL<<30)
typedef struct {uint64_t at,size;int owned;} Range;
static Range ranges[64];static unsigned nr,queries,releases;static jmp_buf failed;
static int order(const void *a,const void *b){uint64_t x=((const Range*)a)->at,y=((const Range*)b)->at;return (x>y)-(x<y);}
static void add(uint64_t at,uint64_t size,int owned){assert(nr<64&&size&&at<LIMIT&&size<=LIMIT-at);ranges[nr++]=(Range){at,size,owned};qsort(ranges,nr,sizeof(*ranges),order);}
static kern_return_t probe_map(vm_map_t task,mach_vm_address_t *at,mach_vm_size_t size,mach_vm_offset_t mask,int flags,mem_entry_name_port_t object,memory_object_offset_t offset,boolean_t copy,vm_prot_t cur,vm_prot_t max,vm_inherit_t inherit){
 (void)task;(void)mask;(void)object;(void)offset;(void)copy;(void)max;(void)inherit;
 assert(!(flags&VM_FLAGS_OVERWRITE)&&cur==VM_PROT_NONE);
 assert(*at<LIMIT&&size<=LIMIT-*at);
 for(unsigned i=0;i<nr;i++)if(*at<ranges[i].at+ranges[i].size&&ranges[i].at<*at+size)return KERN_NO_SPACE;
 add(*at,size,1);return KERN_SUCCESS;
}
static kern_return_t probe_region(vm_map_t task,mach_vm_address_t *at,mach_vm_size_t *size,vm_region_flavor_t flavor,vm_region_info_t info,mach_msg_type_number_t *n,mach_port_t *obj){
 (void)task;(void)flavor;(void)info;(void)n;queries++;*obj=MACH_PORT_NULL;
 for(unsigned i=0;i<nr;i++)if(ranges[i].at+ranges[i].size>*at){*at=ranges[i].at;*size=ranges[i].size;return KERN_SUCCESS;}
 return KERN_INVALID_ADDRESS;
}
static kern_return_t probe_deallocate(vm_map_t task,mach_vm_address_t at,mach_vm_size_t size){
 (void)task;
 for(unsigned i=0;i<nr;i++)if(ranges[i].owned&&ranges[i].at==at&&ranges[i].size==size){ranges[i]=ranges[--nr];qsort(ranges,nr,sizeof(*ranges),order);releases++;return KERN_SUCCESS;}
 assert(!"release must refer to an owned exact reservation");return KERN_INVALID_ADDRESS;
}
#define mach_vm_map probe_map
#define mach_vm_region probe_region
#define mach_vm_deallocate probe_deallocate
#ifndef PRODUCTION_SERVICES
#define PRODUCTION_SERVICES "../../runtime/services.c"
#endif
#include PRODUCTION_SERVICES
#undef mach_vm_map
#undef mach_vm_region
#undef mach_vm_deallocate
_Noreturn void runtime_fail(const char *message){(void)message;longjmp(failed,1);}
static void reset(const uint64_t gaps[][2],unsigned count){
 nr=queries=releases=0;alias_space=low_space=heap=NULL;alias_len=heap_size=0;
 uint64_t end=0;for(unsigned i=0;i<count;i++){assert(gaps[i][0]>=end);if(gaps[i][0]>end)add(end,gaps[i][0]-end,0);end=gaps[i][1];}if(end<LIMIT)add(end,LIMIT-end,0);
}
static void success(int adjacent){
 assert(alias_len==alias_requested()&&alias_space&&low_space);
 uint64_t a=(uintptr_t)alias_space,l=(uintptr_t)low_space;
 assert(a+alias_len<=LIMIT&&l+LOW_SIZE<=LIMIT);
 assert(l+LOW_SIZE<=a);
 assert(ASLR_START<=l&&ASLR_START<=a&&ASLR_START+ASLR_SIZE>=a+alias_len);
 if(adjacent>=0)assert((a==l+LOW_SIZE)==adjacent);
 unsigned owned=0;for(unsigned i=0;i<nr;i++)owned+=ranges[i].owned;
 assert(owned==(adjacent==1?1u:2u));
}
int main(void){
 setenv("TOTK_ALIAS_GIB","8",1);
 /* Captured failed process #64: all three holes are <14 GiB. */
 const uint64_t split[][2]={{481036337152ULL,495229861888ULL},{495234056192ULL,509725376512ULL},{535495180288ULL,LIMIT}};
 reset(split,3);if(setjmp(failed))abort();memory_init();success(0);assert(queries);
 /* Captured #68: a 14.15 GiB hole was missed by the original 1 GiB stride. */
 const uint64_t exact[][2]={{481036337152ULL,493921239040ULL},{493925433344ULL,509121396736ULL},{534891200512ULL,LIMIT}};
 reset(exact,3);if(setjmp(failed))abort();memory_init();success(1);assert(queries);
 /* Insufficient aggregate space: discard the partial low reservation. */
 const uint64_t insufficient[][2]={{0x7000000000ULL,0x7000000000ULL+10*GIB}};
 reset(insufficient,1);if(!setjmp(failed)){memory_init();assert(!"insufficient space must fail");}
 assert(!low_space&&!alias_space&&releases>=1);for(unsigned i=0;i<nr;i++)assert(!ranges[i].owned);
 /* Preserve both existing successful layout paths without gap enumeration. */
 const uint64_t combined[][2]={{0x7000000000ULL,0x7000000000ULL+14*GIB}};
 reset(combined,1);if(setjmp(failed))abort();memory_init();success(1);assert(!queries);
 const uint64_t original[][2]={{LOW_SEARCH_FROM,LOW_SEARCH_FROM+LOW_SIZE},{ALIAS_SPACE_AT,ALIAS_SPACE_AT+8*GIB}};
 reset(original,2);if(setjmp(failed))abort();memory_init();success(0);assert(!queries);
 /* Explicit small alias must not be picked from an earlier small hole. */
 setenv("TOTK_ALIAS_GIB","1",1);
 const uint64_t small[][2]={{ALIAS_SPACE_AT,ALIAS_SPACE_AT+2*GIB},{ALIAS_SPACE_AT+3*GIB,ALIAS_SPACE_AT+9*GIB},{ALIAS_SPACE_AT+10*GIB,ALIAS_SPACE_AT+11*GIB}};
 reset(small,3);if(setjmp(failed))abort();memory_init();success(0);assert(alias_len==GIB&&queries);
 puts("{\"captured_split_gap\":true,\"captured_stride_gap\":true,\"failure_cleanup\":true,\"combined_fast_path\":true,\"separate_fast_path\":true,\"pinned_8gib\":true,\"small_alias_order\":true,\"nonoverwriting_below_39bit\":true}");
 return 0;
}
