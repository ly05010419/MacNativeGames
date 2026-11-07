#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <math.h>
#include <string.h>
extern unsigned char step_state[4096],step_check[64];
extern float call_input(void *,float,float),call_logic(void *,float,float);
static void near(float a,float b){
 assert(fabsf(a-b)<0.00001f);
 uint64_t x16,x17,flags;memcpy(&x16,step_check,8);memcpy(&x17,step_check+8,8);memcpy(&flags,step_check+16,8);
 assert(x16==0x1234&&x17==0x5678&&flags==0x60000000);
 for(unsigned k=32;k<48;k++)assert(step_check[k]==0x5a);
 for(unsigned k=48;k<64;k++)assert(step_check[k]==0xa5);
}
int main(void){
 _Alignas(16) unsigned char fw[0x400]={0};uint64_t ticks;double before;uint64_t count;
 const uint64_t durations[]={520833,1041667,1562500,3125000};
 for(unsigned k=0;k<4;k++){
  ticks=durations[k];memcpy(fw+0x100,&ticks,8);
  near(call_input(fw,60,60),(float)(ticks*60.0/31250000.0));
  near(call_logic(fw,60,60),(float)(ticks*60.0/31250000.0));
  near(call_logic(fw,60,30),(float)(ticks*30.0/31250000.0));
 }
 ticks=0;memcpy(fw+0x100,&ticks,8);near(call_input(fw,30,60),2);near(call_logic(fw,30,60),2);
 float pin=1.0f/60;memcpy(step_state,&pin,4);
 ticks=1562500;memcpy(fw+0x100,&ticks,8);near(call_input(fw,60,60),1);near(call_logic(fw,60,60),1);near(call_logic(fw,60,30),.5);
 pin=0;memcpy(step_state,&pin,4);memcpy(&before,step_state+24,8);memcpy(&count,step_state+16,8);
 for(int k=0;k<20;k++)near(call_logic(fw,60,60),3);
 double after;uint64_t n;memcpy(&after,step_state+24,8);memcpy(&n,step_state+16,8);assert(fabs(after-before-1)<1e-6);assert(n-count==20);
 puts("native ARM64 time-step tests: 16.7/33.3/50/100 ms, 30/60 selectors, startup fallback, live pin, accumulated time and register/flag preservation passed");return 0;
}
