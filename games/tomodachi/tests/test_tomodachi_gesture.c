#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
void *runtime_title_input_override(const char *);
void runtime_title_pointer(int,double,double);
uint64_t runtime_now_ns(void){static uint64_t now;return now+=10000000;}
static uint32_t u32(const uint8_t *p,unsigned offset){uint32_t v;memcpy(&v,p+offset,4);return v;}
int main(void){
 void(*initialize)(void)=runtime_title_input_override("_ZN2nn3hid17InitializeGestureEv");
 int(*read_states)(void*,int)=runtime_title_input_override("_ZN2nn3hid16GetGestureStatesEPNS0_12GestureStateEi");
 uint8_t states[2][0x60];memset(states,0xcc,sizeof states);
 assert(read_states(states,1)==0);initialize();
 /* A complete click between polls must preserve both press and release. */
 runtime_title_pointer(0,0.5,0.5);runtime_title_pointer(2,0.5,0.5);
 assert(read_states(states,1)==1);assert(u32(states[0],16)==3);assert(u32(states[0],48)==16);
 assert(u32(states[0],24)==640);assert(u32(states[0],28)==360);
 assert(read_states(states,1)==1);assert(u32(states[0],16)==5);assert(u32(states[0],60)==1);
 assert(read_states(states,1)==1);assert(u32(states[0],16)==0);
 /* Drag outside the view clamps coordinates and ends with Complete, not Tap. */
 runtime_title_pointer(0,0,0);runtime_title_pointer(1,2,2);
 assert(read_states(states,2)==2);assert(u32(states[0],16)==6);
 assert(u32(states[0],24)==1279);assert(u32(states[0],28)==719);
 assert(u32(states[0],32)==1279);assert(u32(states[0],36)==719);
 runtime_title_pointer(2,2,2);assert(read_states(states,1)==1);assert(u32(states[0],16)==1);
 /* End without an active contact produces no new event. */
 runtime_title_pointer(2,0,0);assert(read_states(states,1)==1);assert(u32(states[0],16)==0);
 assert(read_states(NULL,1)==0);assert(read_states(states,0)==0);
 puts("Tomodachi gesture event ordering, ABI, bounds and release tests passed");
}
