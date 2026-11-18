#include <assert.h>
#include <stdio.h>
#include "switch_left_fallback.h"
int main(void) {
 SwitchLeftFallback s={0};float x=0,y=0;
 uint64_t t=1000000000ULL;
 /* native input first: a working controller never switches */
 switch_left_feed(&s,2048,4095,t);
 assert(!switch_left_get(&s,0,0.9f,t,&x,&y));
 switch_left_feed(&s,2048,4095,t+100000000);
 assert(!switch_left_get(&s,0,0.9f,t+100000000,&x,&y) && !s.confirmed);
 s=(SwitchLeftFallback){0};
 /* drift and a light touch (under 0.3) are no evidence */
 switch_left_feed(&s,2100,2070,t);
 assert(!switch_left_get(&s,0,0,t,&x,&y));
 switch_left_feed(&s,2560,2048,t+300000000);
 assert(!switch_left_get(&s,0,0,t+300000000,&x,&y));
 switch_left_feed(&s,2560,2048,t+400000000);
 assert(!switch_left_get(&s,0,0,t+400000000,&x,&y) && !s.confirmed);
 /* activation after 80 ms past 0.3 */
 switch_left_feed(&s,2336,4064,t+500000000);
 assert(!switch_left_get(&s,0,0,t+500000000,&x,&y));
 switch_left_feed(&s,2336,4064,t+579000000);
 assert(!switch_left_get(&s,0,0,t+579000000,&x,&y));
 switch_left_feed(&s,2336,4064,t+580000000);
 assert(switch_left_get(&s,0,0,t+580000000,&x,&y) && s.confirmed);
 assert(y>0.98f && x>0.13f && x<0.15f);
 /* centering */
 switch_left_feed(&s,2048,2048,t+600000000);
 assert(switch_left_get(&s,0,0,t+600000000,&x,&y) && x==0 && y==0);
 /* confirmed: a stray native value (the D-pad) does not drop it, a short flick counts at once */
 switch_left_feed(&s,0,2048,t+620000000);
 assert(switch_left_get(&s,0,0.9f,t+620000000,&x,&y) && x==-1 && y==0);
 /* stale reports release it, fresh ones take it back without a new delay */
 assert(!switch_left_get(&s,0,0,t+871000000,&x,&y) && !s.active && s.confirmed);
 switch_left_feed(&s,4095,2048,t+900000000);
 assert(switch_left_get(&s,0,0,t+900000000,&x,&y) && x==1 && y==0);
 /* malformed data */
 switch_left_feed(&s,4096,2048,t+910000000);
 assert(!switch_left_get(&s,0,0,t+910000000,&x,&y));
 puts("PASS: native priority before evidence, drift and light-touch rejection, 80 ms activation, centering, latched raw input, stale release and resume, malformed data");
}
