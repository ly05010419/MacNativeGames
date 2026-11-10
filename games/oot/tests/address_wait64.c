/* Native SDK22 address arbitration: use the production SVC and real guest
   thread entry, including a signal after only the high 32 bits change. */
#include "../runtime/runtime.h"
#include <mach/mach_time.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

_Noreturn void runtime_fail(const char *s){fprintf(stderr,"FAIL %s\n",s);exit(1);}
pthread_key_t runtime_keys[8];
uint64_t runtime_hle_tsd_off;
int runtime_guest_code_pc(uint64_t pc){(void)pc;return 0;}
void runtime_thread_init(void){void *p=calloc(1,0x200);if(!p)runtime_fail("TLS allocation");pthread_setspecific(runtime_keys[2],p);}
void *runtime_symbol(const char *s){(void)s;return NULL;}
uint64_t runtime_now_ns(void){mach_timebase_info_data_t t;mach_timebase_info(&t);return (uint64_t)((__uint128_t)mach_absolute_time()*t.numer/t.denom);}
static void need(int ok,const char *s){if(!ok)runtime_fail(s);}
static void kernel(int svc,uint64_t *r){need(svc_thread(svc,r)==1,"SVC dispatch");}
static _Atomic uint64_t value;
static _Atomic int entered,done;
static uint64_t result;
static void waiter(uint64_t unused){
    (void)unused;uint64_t r[31]={(uintptr_t)&value,3,UINT64_C(0x1234567800000042),UINT64_C(2000000000)};
    atomic_store(&entered,1);kernel(0x34,r);result=r[0];atomic_store(&done,1);kernel(0x0a,r);runtime_fail("ExitThread returned");
}
static uint64_t wait_value(int type,uint64_t expected,int64_t timeout){uint64_t r[31]={(uintptr_t)&value,(uint64_t)type,expected,(uint64_t)timeout};kernel(0x34,r);return r[0];}
static void sleep_guest(uint64_t ns){uint64_t r[31]={ns};kernel(0x0b,r);}
int main(void){
    for(int i=0;i<8;i++)pthread_key_create(&runtime_keys[i],free);runtime_thread_init();svc_thread_main_init();int checked=0;
    atomic_store(&value,UINT64_C(0x1234567800000042));
    need(wait_value(3,atomic_load(&value),0)==0xea01u,"equal64 poll must time out, not reject equal counter");checked++;
    need(wait_value(3,UINT64_C(0x2234567800000042),0)==0xfa01u,"different high word must not wait");checked++;
    need(wait_value(3,UINT64_C(0x1234567800000043),0)==0xfa01u,"different low word must not wait");checked++;
    need(wait_value(2,UINT64_C(0x9999999900000042),0)==0xea01u,"equal32 must ignore high word");checked++;
    need(wait_value(0,0x43,0)==0xea01u,"less32 compatibility");checked++;
    uint64_t start=runtime_now_ns();need(wait_value(3,atomic_load(&value),20000000)==0xea01u,"equal64 timed wait");
    need(runtime_now_ns()-start>=15000000,"equal64 wait returned without blocking");checked+=2;
    size_t bytes=1<<20;void *stack=mmap(NULL,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);need(stack!=MAP_FAILED,"stack allocation");
    uint64_t r[31]={0,(uintptr_t)waiter,0,(uintptr_t)stack+bytes,44,1};kernel(0x08,r);need(!r[0],"CreateThread");uint32_t handle=(uint32_t)r[1];r[0]=handle;kernel(0x09,r);need(!r[0],"StartThread");
    for(int i=0;i<100&&!atomic_load(&entered);i++)sleep_guest(1000000);
    need(atomic_load(&entered),"waiter entered");sleep_guest(30000000);need(!atomic_load(&done),"equal64 waiter stayed blocked");checked++;
    atomic_fetch_add(&value,UINT64_C(1)<<32);r[0]=(uintptr_t)&value;r[1]=0;r[2]=0;r[3]=1;kernel(0x35,r);need(!r[0],"SignalToAddress");
    r[1]=(uintptr_t)&handle;r[2]=1;r[3]=2000000000;kernel(0x18,r);need(!r[0]&&atomic_load(&done)&&!result,"signal released equal64 waiter");checked++;
    r[0]=handle;kernel(0x16,r);munmap(stack,bytes);
    printf("{\"status\":\"pass\",\"probe\":\"address-wait64\",\"checked\":%d}\n",checked);return 0;
}
