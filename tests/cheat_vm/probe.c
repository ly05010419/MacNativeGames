#include "cheat_vm.h"
#include <assert.h>
static unsigned char memory[4096];
static int rd(void *p,uint64_t a,unsigned n,uint64_t *v){(void)p;if(a<0x10000||a+n< a||a+n>0x11000)return 0;*v=0;memcpy(v,memory+a-0x10000,n);return 1;}
static int wr(void *p,uint64_t a,unsigned n,uint64_t v){(void)p;if(a<0x10000||a+n<a||a+n>0x11000)return 0;memcpy(memory+a-0x10000,&v,n);return 1;}
static int run(const uint32_t *w,size_t n,uint64_t keys,unsigned budget){CvProgram p;assert(cv_compile(&p,w,n));CvHost h={.read=rd,.write=wr,.bases={0x10000,0,0,0,0},.keys=keys,.budget=budget};int r=cv_execute(&p,&h);cv_free(&p);return r;}
int main(void){
 uint32_t simple[]={0x04000000,0x20,0x12345678};assert(run(simple,3,0,0)==CV_OK&&memory[0x20]==0x78);
 uint64_t ptr=0x10080;memcpy(memory+0x40,&ptr,8);uint32_t pointer[]={0x580F0000,0x40,0x780F0000,4,0x640F0000,0,999};assert(run(pointer,7,0,0)==CV_OK);uint32_t value;memcpy(&value,memory+0x84,4);assert(value==999);
 uint32_t conditional[]={0x80000104,0x04000000,0x24,7,0x21000000,0x04000000,0x24,8,0x20000000};assert(run(conditional,9,0,0)==CV_OK&&memory[0x24]==8);assert(run(conditional,9,0x104,0)==CV_OK&&memory[0x24]==7);
 uint32_t loop[]={0x400F0000,0,0x100c0,0x300C0000,3,0x640F1000,0,123,0x310C0000};assert(run(loop,9,0,0)==CV_OK);memcpy(&value,memory+0xc8,4);assert(value==123);
 uint32_t regs[]={0x400F0000,0,0x100e0,0x400A0000,0,42,0xA4AF0200,4};assert(run(regs,8,0,0)==CV_OK);memcpy(&value,memory+0xe4,4);assert(value==42);
 uint32_t badptr[]={0x580F0000,0x48,0x640F0000,0,1};assert(run(badptr,5,0,0)==CV_MEMORY);
 uint32_t bounded[]={0x300C0000,0xffffffff,0x310C0000};assert(run(bounded,3,0,10)==CV_BUDGET);
 CvProgram p;uint32_t malformed[]={0x80000001};assert(!cv_compile(&p,malformed,1));uint32_t unknown[]={0xff000000};assert(!cv_compile(&p,unknown,1));
 puts("cheat VM synthetic checks passed");return 0;
}
