/* Single-contact mouse gestures. ABI follows switchbrew/libnx hid.h. */
#include "runtime.h"
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
static int trace_input(void){static int on=-1;if(on<0)on=getenv("TOMODACHI_INPUT_TRACE")!=NULL;return on;}
typedef struct {
 uint64_t sample,context;uint32_t type,direction,x,y;int32_t dx,dy;
 float vx,vy;uint32_t attributes;float scale,rotation;int32_t count;
 struct {uint32_t x,y;} points[4];
} Gesture;
_Static_assert(sizeof(Gesture)==0x60,"GestureState ABI");
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static Gesture queue[64],current;
static unsigned head,length;
static int initialized,down,moved,start_x,start_y;
static uint64_t sample,context,last_ns;
typedef struct {uint64_t delta_ns;uint32_t attributes,id,x,y,width,height,angle,pad;} Touch;
_Static_assert(sizeof(Touch)==40,"TouchState ABI");
static Touch touches[64],touch_current;
static unsigned touch_head,touch_length;
static uint64_t touch_sample;
static void initialize_touch(void) {if(trace_input())fprintf(stderr,"[tomodachi-input] InitializeTouchScreen\n");pthread_mutex_lock(&mutex);initialized=1;pthread_mutex_unlock(&mutex);}
static void initialize_gesture(void) {if(trace_input())fprintf(stderr,"[tomodachi-input] InitializeGesture\n");pthread_mutex_lock(&mutex);initialized=1;pthread_mutex_unlock(&mutex);}
void runtime_title_pointer(int phase,double nx,double ny) {
 if(trace_input())fprintf(stderr,"[tomodachi-input] pointer phase=%d xy=%.4f,%.4f initialized=%d\n",phase,nx,ny,initialized);
 pthread_mutex_lock(&mutex);
 if(!initialized){pthread_mutex_unlock(&mutex);return;}
 int x=(int)llround(fmax(0,fmin(1,nx))*1279),y=(int)llround(fmax(0,fmin(1,ny))*719);
 uint64_t now=runtime_now_ns();Gesture state={0};state.scale=1;state.x=x;state.y=y;
 if(phase==0){down=1;moved=0;start_x=x;start_y=y;++context;state.type=3;state.attributes=16;}
 else if(!down){pthread_mutex_unlock(&mutex);return;}
 else if(phase==1){state.type=6;state.dx=x-(int)current.x;state.dy=y-(int)current.y;
  if(abs(x-start_x)+abs(y-start_y)>6)moved=1;
  double dt=last_ns?(now-last_ns)/1e9:0;if(dt>0){state.vx=state.dx/dt;state.vy=state.dy/dt;}
 }else{down=0;state.type=phase==3?2:(moved?1:5);}
 state.sample=++sample;state.context=context;state.count=(down||state.type==5)?1:0;
 state.points[0].x=x;state.points[0].y=y;current=state;last_ns=now;
 touch_current=(Touch){0,phase==0?1u:phase>=2?2u:0u,0,(uint32_t)x,(uint32_t)y,8,8,0,0};
 if(touch_length==64){touch_head=(touch_head+1)%64;--touch_length;}
 touches[(touch_head+touch_length)%64]=touch_current;++touch_length;
 if(length==64){head=(head+1)%64;--length;}
 queue[(head+length)%64]=state;++length;pthread_mutex_unlock(&mutex);
}
static int gesture_states(Gesture *out,int capacity) {
 if(!out||capacity<=0)return 0;
 pthread_mutex_lock(&mutex);
 if(!initialized){pthread_mutex_unlock(&mutex);return 0;}
 static unsigned calls;if(trace_input()&&calls++%600==0)fprintf(stderr,"[tomodachi-input] GetGestureStates capacity=%d queued=%u\n",capacity,length);
 unsigned n=length<(unsigned)capacity?length:(unsigned)capacity;
 if(n){for(unsigned i=0;i<n;i++)out[n-i-1]=queue[(head+i)%64];head=(head+n)%64;length-=n;}
 else{current.sample=++sample;current.attributes=0;current.dx=current.dy=0;current.vx=current.vy=0;
  current.type=down?4:0;current.count=down?1:0;out[0]=current;n=1;}
 pthread_mutex_unlock(&mutex);return (int)n;
}
static int touch_states(void *output,int capacity,int contacts) {
 if(!output||capacity<=0)return 0;
 pthread_mutex_lock(&mutex);
 unsigned n=touch_length<(unsigned)capacity?touch_length:(unsigned)capacity;
 if(!n)n=1;
 size_t stride=16+(size_t)contacts*40;
 for(unsigned i=0;i<n;i++){
  uint8_t *out=(uint8_t*)output+(n-i-1)*stride;memset(out,0,stride);
  uint64_t seq=++touch_sample;memcpy(out,&seq,8);
  Touch touch=touch_length?touches[(touch_head+i)%64]:touch_current;
  int32_t count=touch_length||down?1:0;memcpy(out+8,&count,4);
  if(count)memcpy(out+16,&touch,40);
 }
 if(touch_length){touch_head=(touch_head+n)%64;touch_length-=n;}
 touch_current.attributes=0;pthread_mutex_unlock(&mutex);return (int)n;
}
static int touch_states16(void *out,int n){return touch_states(out,n,16);}
static int touch_states1(void *out,int n){return touch_states(out,n,1);}
static void touch_state16(void *out){(void)touch_states(out,1,16);}
static void touch_state1(void *out){(void)touch_states(out,1,1);}
static unsigned touch_detail16(int *count,void *out,int n){if(!count)return 0x2ee202;*count=touch_states16(out,n);return 0;}
static unsigned touch_detail1(int *count,void *out,int n){if(!count)return 0x2ee202;*count=touch_states1(out,n);return 0;}
void *runtime_title_input_override(const char *name) {
 if(!strcmp(name,"_ZN2nn3hid21InitializeTouchScreenEv"))return initialize_touch;
 if(!strcmp(name,"_ZN2nn3hid17InitializeGestureEv"))return initialize_gesture;
 if(!strcmp(name,"_ZN2nn3hid16GetGestureStatesEPNS0_12GestureStateEi"))return gesture_states;
 if(!strcmp(name,"_ZN2nn3hid20GetTouchScreenStatesILm16EEEiPNS0_16TouchScreenStateIXT_EEEi"))return touch_states16;
 if(!strcmp(name,"_ZN2nn3hid20GetTouchScreenStatesILm1EEEiPNS0_16TouchScreenStateIXT_EEEi"))return touch_states1;
 if(!strcmp(name,"_ZN2nn3hid19GetTouchScreenStateILm16EEEvPNS0_16TouchScreenStateIXT_EEE"))return touch_state16;
 if(!strcmp(name,"_ZN2nn3hid19GetTouchScreenStateILm1EEEvPNS0_16TouchScreenStateIXT_EEE"))return touch_state1;
 if(!strcmp(name,"_ZN2nn3hid6detail20GetTouchScreenStatesILm16EEENS_6ResultEPiPNS0_16TouchScreenStateIXT_EEEi"))return touch_detail16;
 if(!strcmp(name,"_ZN2nn3hid6detail20GetTouchScreenStatesILm1EEENS_6ResultEPiPNS0_16TouchScreenStateIXT_EEEi"))return touch_detail1;
 return NULL;
}
