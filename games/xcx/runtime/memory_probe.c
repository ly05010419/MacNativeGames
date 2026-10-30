/* Validate stack moves and logical accounting using the production SVC path. */
#include "runtime.h"
#include <stdio.h>
#include <string.h>
#include <mach/mach.h>
#include "mach_vm_compat.h"
static void call(unsigned n,uint64_t *r){runtime_svc(n,r,"memory-probe",0);}
static void check(int ok,const char *s){if(!ok)runtime_fail(s);}
static unsigned host_permission(uint64_t address){
 mach_vm_address_t a=address;mach_vm_size_t size=0;vm_region_basic_info_data_64_t info;
 mach_msg_type_number_t count=VM_REGION_BASIC_INFO_COUNT_64;mach_port_t object=0;
 check(mach_vm_region(mach_task_self(),&a,&size,VM_REGION_BASIC_INFO_64,(vm_region_info_t)&info,&count,&object)==KERN_SUCCESS&&a<=address,"probe host region");
 if(object)mach_port_deallocate(mach_task_self(),object);
 return (unsigned)info.protection;
}
void runtime_memory_probe(void){
 runtime_thread_init();svc_thread_main_init();
 uint64_t module=(uintptr_t)runtime_module_base("main"),mb=0,ms=0;unsigned state=0,perm=0,attr=0;
 for(unsigned i=0;i<8;i++){check(runtime_module_query(module,&mb,&ms,&state,&perm,&attr),"probe module query");if(state==4)break;module=mb+ms;}
 check(state==4&&ms>=0x14000,"probe module writable data");
 uint64_t relro=(mb+0x3fff)&~0x3fffULL;relro+=0x4000;
 check(!runtime_module_permission(relro+4096,0x9000,1),"probe RELRO permission");
 check(host_permission(relro)&VM_PROT_WRITE,"RELRO leading edge preserves neighbor");
 check(host_permission(relro+0x4000)==VM_PROT_READ,"RELRO full interior host page read-only");
 check(host_permission(relro+0x8000)&VM_PROT_WRITE,"RELRO trailing edge preserves neighbor");
 check(runtime_module_query(relro+0x2000,&mb,&ms,&state,&perm,&attr)&&mb==relro+4096&&ms==0x9000&&perm==1&&!attr,"RELRO logical 4 KiB boundaries");
 check(runtime_module_permission(relro+1,4096,1)==0xcc01,"RELRO misaligned address rejected");
 check(runtime_module_permission(relro,1,1)==0xca01,"RELRO misaligned size rejected");
 check(runtime_module_permission(relro,4096,5)==0xd801,"RELRO executable permission rejected");
 check(runtime_module_permission((uintptr_t)runtime_module_base("main"),4096,3)==0xd401,"RELRO text modification rejected");
 check(!runtime_module_permission(relro+4096,0x9000,3),"RELRO writable restoration");
 check(host_permission(relro+0x4000)==(VM_PROT_READ|VM_PROT_WRITE),"RELRO host writable restoration");
 check(!runtime_module_permission(relro+4096,4096,1)&&!runtime_module_lock_permission(relro+4096,4096,16,16),"RELRO permanent permission lock");
 check(runtime_module_permission(relro+4096,4096,3)==0xd401,"RELRO locked reprotection rejected");
 check(runtime_module_query(relro+4096,&mb,&ms,&state,&perm,&attr)&&perm==1&&attr==16&&ms==4096,"RELRO locked query");
 uint64_t r[31]={0};r[1]=14;call(0x29,r);uint64_t dst=r[1]+0x13000;
 /* XCX supplies an 8 KiB static thread stack in writable module data.
    Read-only/locked data and executable pages must remain ineligible. */
 uint64_t data=relro+0xc000;
 check(runtime_module_query(data,&mb,&ms,&state,&perm,&attr)&&state==4&&perm==3&&!attr&&data-mb<=ms&&0x8000<=ms-(data-mb),"probe module stack source");
 uint8_t saved[0x8000];memcpy(saved,(void *)(uintptr_t)data,sizeof saved);
 for(unsigned pass=0;pass<2;pass++){
  uint64_t n=pass?0x8000:0x2000;memset((void *)(uintptr_t)data,0x62,n);
  r[0]=dst;r[1]=data;r[2]=n;call(4,r);check(!r[0],"map writable module stack");
  for(uint64_t i=0;i<n;i++)check(((uint8_t *)(uintptr_t)dst)[i]==0x62,"module stack copy");
  memset((void *)(uintptr_t)dst,0x47,n);
  r[0]=dst;r[1]=data;r[2]=n;call(5,r);check(!r[0],"unmap writable module stack");
  for(uint64_t i=0;i<n;i++)check(((uint8_t *)(uintptr_t)data)[i]==0x47,"module stack copyback");
 }
 memcpy((void *)(uintptr_t)data,saved,sizeof saved);
 r[0]=dst;r[1]=relro+4096;r[2]=4096;call(4,r);check(r[0]!=0,"reject locked read-only module stack");
 r[0]=dst;r[1]=(uintptr_t)runtime_module_base("main");r[2]=4096;call(4,r);check(r[0]!=0,"reject executable module stack");
 r[1]=0x200000;call(1,r);check(!r[0],"probe heap");uint8_t *src=(void *)(uintptr_t)(r[1]+0x13000);
 {
  unsigned char object[0x100]={0};
  uint32_t (*create)(void *,void *,uint64_t,unsigned)=runtime_symbol("_ZN2nn2os20CreateTransferMemoryEPNS0_18TransferMemoryTypeEPvmNS0_16MemoryPermissionE");
  void (*destroy)(void *)=runtime_symbol("_ZN2nn2os21DestroyTransferMemoryEPNS0_18TransferMemoryTypeE");
  check(create&&destroy,"transfer-memory SDK exports");
  uint32_t result=create(object,(void *)((uintptr_t)src&~0x3fffULL),0x10000,0);
  fprintf(stderr,"[totk-memory] SDK transfer create result=%#x object=%02x handle=%#x\n",result,object[0],*(uint32_t *)(void *)(object+0x18));
  check(result==0,"SDK transfer-memory create result");destroy(object);
 }
 for(unsigned i=0;i<0x10000;i++)src[i]=(unsigned char)(i*13+7);
 r[0]=dst;r[1]=(uintptr_t)src;r[2]=0x10000;call(4,r);check(!r[0],"map stack");
 for(unsigned i=0;i<0x10000;i++)check(((unsigned char *)(uintptr_t)dst)[i]==(unsigned char)(i*13+7),"mapped stack content");
 struct{uint64_t base,size;uint32_t state,attr,perm,ipc,device,pad;} info;
 r[0]=(uintptr_t)&info;r[2]=dst+0x5000;call(6,r);check(info.base==dst&&info.size==0x10000&&info.state==0xb&&info.perm==3,"mapped stack query");
 r[0]=(uintptr_t)&info;r[2]=(uintptr_t)src+0x5000;call(6,r);check(info.base==(uintptr_t)src&&info.size==0x10000&&info.state==5&&info.perm==0&&info.attr==4,"moved source accounting");
 r[0]=dst;r[1]=(uintptr_t)src;r[2]=0x10000;call(4,r);check(r[0]!=0,"duplicate mapping rejected");
 memset((void *)(uintptr_t)dst,0x5a,0x10000);r[0]=dst;r[1]=(uintptr_t)src;r[2]=0x10000;call(5,r);check(!r[0],"unmap stack");
 for(unsigned i=0;i<0x10000;i++)check(src[i]==0x5a,"unmap copy back");
 r[0]=(uintptr_t)&info;r[2]=dst;call(6,r);check(info.state==0&&info.perm==0,"unmapped stack query");
 r[0]=dst;r[1]=(uintptr_t)src;r[2]=0x10000;call(5,r);check(r[0]!=0,"second unmap rejected");
 r[1]=2;call(0x29,r);uint64_t alias=r[1]+0x1000;
 r[0]=alias;r[1]=0x1000;call(0x2c,r);check(!r[0],"alias page allocation");memset((void *)(uintptr_t)alias,0x37,4096);
 r[0]=alias+4096;r[1]=4096;call(0x2c,r);check(!r[0],"neighbor alias allocation");
 for(unsigned i=0;i<4096;i++)check(((unsigned char *)(uintptr_t)alias)[i]==0x37,"neighbor mapping preserves bytes");
 r[0]=alias;r[1]=4096;call(0x2d,r);check(!r[0],"alias partial unmap");
 r[0]=alias;r[1]=4096;call(0x2c,r);check(!r[0],"alias remap");for(unsigned i=0;i<4096;i++)check(((unsigned char *)(uintptr_t)alias)[i]==0,"reallocated alias zero fill");
 r[0]=(uintptr_t)&info;r[2]=alias;call(6,r);check(info.state==5&&info.perm==3,"alias allocation query");
 /* Horizon: MapPhysicalMemory over an already-mapped page succeeds and leaves that page untouched */
 ((unsigned char *)(uintptr_t)alias)[7]=0x5a;r[0]=alias;r[1]=4096;call(0x2c,r);check(!r[0],"duplicate physical map accepted");check(((unsigned char *)(uintptr_t)alias)[7]==0x5a,"duplicate physical map keeps contents");
 /* Reject an over-budget request before changing any page/accounting state,
    then show a smaller allocation can still succeed at the rejected address. */
 r[1]=6;call(0x29,r);uint64_t pool=r[1];r[1]=7;call(0x29,r);uint64_t used=r[1];
 check(pool>used,"memory probe requires free application pool");
 uint64_t rejected=alias+0x1000000,too_large=((pool-used)&~4095ULL)+4096;
 r[0]=rejected;r[1]=too_large;call(0x2c,r);check(r[0]==0x10801,"physical memory exhaustion must return LimitReached");
 r[1]=7;call(0x29,r);check(r[1]==used,"failed physical map changed accounting");
 r[0]=(uintptr_t)&info;r[2]=rejected;call(6,r);check(info.state==0&&info.perm==0,"failed physical map changed page state");
 check(((unsigned char *)(uintptr_t)alias)[7]==0x5a,"failed physical map changed existing contents");
 r[0]=rejected;r[1]=4096;call(0x2c,r);check(!r[0],"smaller allocation after exhausted request");
 r[0]=rejected;r[1]=4096;call(0x2d,r);check(!r[0],"release recovery allocation");
 r[1]=7;call(0x29,r);check(r[1]==used,"recovered physical map accounting");
 r[0]=alias+4096;r[1]=4096;call(0x2d,r);check(!r[0],"alias free tail");
 r[0]=alias;r[1]=8192;call(0x2d,r);check(!r[0],"alias complete unmap covering free hole");
 puts("{\"status\":\"pass\",\"scope\":\"RELRO host and logical permissions, permanent permission locks and invalid requests; 8/32 KiB writable-module and 64 KiB heap stack move/copyback, text/RELRO rejection, alias neighbor preservation and zero-fill remap, logical states, duplicate map/unmap rejection; host protection remains 16 KiB granular\"}");
}

/* XCX needs a real 60-GiB virtual reservation, but commits only sparse pages.
   Exercise the production SVC and IPC address paths, not a size-report stub. */
void runtime_address_probe(void){
 runtime_thread_init();svc_thread_main_init();
 uint64_t r[31]={0};r[1]=2;call(0x29,r);uint64_t base=r[1];
 r[1]=3;call(0x29,r);uint64_t size=r[1];
 check(size>=(60ULL<<30),"XCX requires at least 60 GiB of real alias VA");
 r[1]=12;call(0x29,r);uint64_t aslr=r[1];
 r[1]=13;call(0x29,r);check(aslr<=base&&base-aslr<=r[1]&&size<=r[1]-(base-aslr),"ASLR range contains full alias region");
 uint64_t addresses[]={base,base+(60ULL<<30)-0x4000};
 for(unsigned i=0;i<2;i++){
  uint64_t a=addresses[i];runtime_ipc_address_probe(a+0x123);
  r[0]=a;r[1]=0x4000;call(0x2c,r);check(!r[0],"sparse alias commit");
  memset((void *)(uintptr_t)a,0x31+i,0x4000);
  struct{uint64_t base,size;uint32_t state,attr,perm,ipc,device,pad;} info;
  r[0]=(uintptr_t)&info;r[2]=a;call(6,r);
  check(info.base<=a&&info.size>a-info.base&&info.state==5&&info.perm==3,"sparse alias query");
 }
 check(*(unsigned char *)(uintptr_t)addresses[0]==0x31,"far commit preserves first alias page");
 for(unsigned i=0;i<2;i++){r[0]=addresses[i];r[1]=0x4000;call(0x2d,r);check(!r[0],"sparse alias release");}
 printf("{\"status\":\"pass\",\"scope\":\"real 60 GiB VA, sparse first/last pages, ASLR bounds, full-width HIPC addresses\",\"alias_base\":\"%llx\",\"alias_bytes\":%llu}\n",(unsigned long long)base,(unsigned long long)size);
}
