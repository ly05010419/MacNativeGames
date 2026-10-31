/* Synthetic mappings and constants, independent of any game. */
#include "runtime.h"
#include <unistd.h>
#include <time.h>
#include <assert.h>
uint64_t runtime_now_ns(void){struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);return (uint64_t)ts.tv_sec*1000000000+ts.tv_nsec;}
#include "cheat_native.h"
Module *runtime_cheat_module(void){return NULL;}
void runtime_cheat_region_bases(uint64_t *p){(void)p;}
int runtime_cheat_guest_range(uint64_t a,size_t n){(void)a;(void)n;return 0;}
unsigned runtime_cheat_thread_ports(uint32_t *p,unsigned n){(void)p;(void)n;return 0;}
uint64_t runtime_gamepad_peek_buttons(void){return 0;}
int main(void){
 mach_vm_address_t a=0;assert(!mach_vm_allocate(mach_task_self(),&a,CN_PAGE*2,VM_FLAGS_ANYWHERE));
 uint32_t *data=(void *)(uintptr_t)a;data[0]=0x12345678;
 Module module={.base=(void *)(uintptr_t)a,.key_off=CN_PAGE*2};
 CnMemory *c=calloc(1,sizeof *c);assert(c);c->module=&module;c->deadline=UINT64_MAX;
 CnGuard guards[]={{.offset=0,.original=0x12345678},{.offset=4,.original=0}};c->guards=guards;c->nguards=2;
 assert(!mach_vm_protect(mach_task_self(),a,CN_PAGE,FALSE,VM_PROT_READ|VM_PROT_EXECUTE));
 assert(cn_write(c,a,4,0x87654321));assert(data[0]==0x12345678);assert(cn_commit(c));assert(data[0]==0x87654321);
 assert(cn_release(c,0));assert(data[0]==0x12345678);
 c->npages=c->nwrites=0;c->entry=0;assert(cn_write(c,a,4,0x87654321));assert(cn_commit(c));
 c->npages=c->nwrites=0;c->entry=1;assert(!cn_write(c,a,4,17));
 assert(cn_write(c,a,4,0x87654321));assert(cn_commit(c));assert(cn_release(c,0));assert(data[0]==0x87654321);assert(cn_release(c,1));assert(data[0]==0x12345678);
 c->npages=c->nwrites=0;assert(!cn_write(c,a+8,4,17));assert(!cn_write(c,(uintptr_t)c,4,17));
 assert(cn_write(c,a+CN_PAGE,4,123));assert(cn_commit(c));assert(data[CN_PAGE/4]==123);
 c->npages=c->nwrites=0;
 uint32_t failure[]={0x04000000,CN_PAGE,456,0x300C0000,0xffffffff,0x310C0000};CvProgram program;assert(cv_compile(&program,failure,6));
 CvHost host={.context=c,.read=cn_read,.write=cn_write,.bases={a,0,0,0,0},.budget=10};assert(cv_execute(&program,&host)==CV_BUDGET);assert(data[CN_PAGE/4]==123);cv_free(&program);
 c->npages=c->nwrites=0;c->entry=0;assert(cn_write(c,a+4,4,17));assert(cn_commit(c));assert(cn_release(c,0));assert(data[1]==17);
 c->npages=c->nwrites=0;c->entry=1;assert(!cn_write(c,a+4,4,18)&&c->issue==3);assert(data[1]==17);
 c->deadline=0;c->npages=c->nwrites=0;assert(!cn_write(c,a+CN_PAGE,4,7));assert(data[CN_PAGE/4]==123);
 mach_vm_deallocate(mach_task_self(),a,CN_PAGE*2);free(c);puts("native guarded transactions passed");return 0;
}
