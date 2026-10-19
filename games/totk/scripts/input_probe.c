#include "../runtime/runtime.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* Interactive diagnostics are not exercised by this service ABI probe. */
/* A fake clock: real user input reads it to rate-limit its log line. */
/* Motion checks pin it (probe_clock_fixed) to the times of the samples they feed. */
static uint64_t probe_now;static int probe_clock_fixed;
uint64_t runtime_now_ns(void){return probe_clock_fixed?probe_now:(probe_now+=1000000);}
void dm_gfx_shot_at(long frames,const char *path){(void)frames;(void)path;runtime_fail("unexpected screenshot in unit probe");}
void dm_gfx_passdump_next(long n){(void)n;runtime_fail("unexpected pass dump in unit probe");}
void dm_gfx_texdump_next(const char *path){(void)path;runtime_fail("unexpected texture dump in unit probe");}
static int help_toggles,interp_toggles,output_cycles,aspect_toggles;   // keys 1 / 2 / 3 / 4 (see the key checks)
void runtime_swkbd_selftest(void){runtime_fail("unexpected keyboard self-test in unit probe");}
void runtime_dump_threads(void){runtime_fail("unexpected thread dump in unit probe");}
void runtime_hle_trace_set(int mode){(void)mode;runtime_fail("unexpected host-call trace in unit probe");}
void runtime_frame_step_pin(double ms){(void)ms;runtime_fail("unexpected frame step pin in unit probe");}
void runtime_front_text_test(const char *op,const char *text){(void)op;(void)text;runtime_fail("unexpected keyboard test in unit probe");}
int runtime_output_res_toggle(void){runtime_fail("unexpected output size switch in unit probe");return 0;}
void runtime_shortcut_help_toggle(void){help_toggles++;}
int runtime_output_mode_cycle(void){output_cycles++;return 1;}
int runtime_aspect_toggle(void){aspect_toggles++;return 1;}
void dm_gfx_set_flag(const char *name,int v){if(!strcmp(name,"mfx_interp")&&v==-1){interp_toggles++;return;}runtime_fail("unexpected runtime flag in unit probe");}
void dm_gfx_querylog_next(long n){(void)n;runtime_fail("unexpected query log in unit probe");}
void dm_gfx_anomdump(void){runtime_fail("unexpected anomaly dump in unit probe");}
void dm_gfx_window_shot(const char *p){(void)p;runtime_fail("unexpected window shot in unit probe");}
int runtime_survey_command(const char *line){(void)line;return 0;}
void runtime_survey_cancel_user(void){}
void runtime_front_request_close(void){runtime_fail("unexpected window close in unit probe");}
/* Test double for the native controller layer (runtime/gamepad.m is not linked into this probe). */
static RuntimeGamepadState fake_pad;
void runtime_gamepad_read(RuntimeGamepadState *out){*out=fake_pad;}
void runtime_gamepad_clear(void){fake_pad=(RuntimeGamepadState){0};}
/* Test double for the Mac controller's haptics (runtime/gamepad_feedback.m): the last value per side. */
static int rumble_calls;static float rumble_seen[2][4];
void runtime_rumble_set(unsigned side,float al,float fl,float ah,float fh){if(side>1)runtime_fail("rumble side out of range");rumble_calls++;rumble_seen[side][0]=al;rumble_seen[side][1]=fl;rumble_seen[side][2]=ah;rumble_seen[side][3]=fh;}
static int handheld;
int runtime_handheld_mode(void){return handheld;}
_Noreturn void runtime_fail(const char *s){fprintf(stderr,"%s\n",s);exit(1);}
typedef struct{int64_t sample;uint64_t buttons;int32_t lx,ly,rx,ry;uint32_t attr,pad;} State;
static void need(int c,const char *s){if(!c)runtime_fail(s);}

/* Rumble and motion (runtime/hid_feedback.c, runtime/motion_fusion.c): off unless TOTK_RUMBLE / TOTK_GYRO; the SDK's
   handles; the hid detail answers; newest-first six-axis states; the orientation integrated in the controller's frame. */
typedef struct{float al,fl,ah,fh;} Vib;
typedef struct{uint32_t type,position;} VibInfo;
typedef struct{int64_t dt,sampling;float acc[3],gyro[3],angle[3],dir[9];uint32_t attr,res;} Six;
static int near(double a,double b,double tol){return fabs(a-b)<=tol;}
static int near3(const float *v,double x,double y,double z,double tol){return near(v[0],x,tol)&&near(v[1],y,tol)&&near(v[2],z,tol);}
static void feed(int n,double ax,double ay,double az,double gx,double gy,double gz){
 for(int i=0;i<n;i++){probe_now+=4000000;const double a[3]={ax,ay,az},g[3]={gx,gy,gz};runtime_motion_feed(a,g,probe_now);}
}
static void feedback_checks(void){
 const char *getV="_ZN2nn3hid25GetVibrationDeviceHandlesEPNS0_21VibrationDeviceHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE";
 const char *getS="_ZN2nn3hid23GetSixAxisSensorHandlesEPNS0_19SixAxisSensorHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE";
 const char *sendN="_ZN2nn3hid6detail18SendVibrationValueERKNS0_21VibrationDeviceHandleERKNS0_14VibrationValueE";
 setenv("TOTK_RUMBLE","0",1);setenv("TOTK_GYRO","0",1);
 int(*none)(void *,int,const uint32_t *,uint32_t)=runtime_input_override(getV);uint32_t hs[2]={0xdead,0xbeef},pid=0;
 need(none&&none(hs,2,&pid,1)==0&&hs[0]==0xdead,"rumble =0: no vibration devices (as before)");
 none=runtime_input_override(getS);need(none&&none(hs,2,&pid,1)==0&&hs[0]==0xdead,"gyro =0: no six-axis sensor (as before)");
 need(!runtime_input_override(sendN)&&!runtime_input_override("_ZN2nn3hid6detail18StartSixAxisSensorERKNS0_19SixAxisSensorHandleE"),"=0: the hid detail functions stay the SDK's");
 unsetenv("TOTK_RUMBLE");unsetenv("TOTK_GYRO");setenv("TOTK_SLOT_OWNER","claude",1);
 need(runtime_input_override(getV)&&runtime_input_override(getS),"an agent's test session: off unless set");
 setenv("TOTK_SLOT_OWNER","user",1);
 need(!runtime_input_override(getV)&&!runtime_input_override(getS),"default (the user's game): on, the SDK's own handle helpers run");
 unsetenv("TOTK_SLOT_OWNER");setenv("TOTK_RUMBLE","1",1);setenv("TOTK_GYRO","1",1);
 need(!runtime_input_override(getV)&&!runtime_input_override(getS),"=1: on");
 uint32_t(*vinit)(const uint32_t *)=runtime_input_override("_ZN2nn3hid6detail25InitializeVibrationDeviceERKNS0_21VibrationDeviceHandleE");
 uint32_t(*vsend)(const uint32_t *,const Vib *)=runtime_input_override(sendN);
 uint32_t(*vmany)(const uint32_t *,const Vib *,int)=runtime_input_override("_ZN2nn3hid6detail19SendVibrationValuesEPKNS0_21VibrationDeviceHandleEPKNS0_14VibrationValueEi");
 uint32_t(*vactual)(Vib *,const uint32_t *)=runtime_input_override("_ZN2nn3hid6detail23GetActualVibrationValueEPNS0_14VibrationValueERKNS0_21VibrationDeviceHandleE");
 uint32_t(*vinfo)(VibInfo *,const uint32_t *)=runtime_input_override("_ZN2nn3hid6detail22GetVibrationDeviceInfoEPNS0_19VibrationDeviceInfoERKNS0_21VibrationDeviceHandleE");
 uint32_t(*vmounted)(_Bool *,const uint32_t *)=runtime_input_override("_ZN2nn3hid6detail24IsVibrationDeviceMountedEPbRKNS0_21VibrationDeviceHandleE");
 uint32_t(*vperm)(_Bool *)=runtime_input_override("_ZN2nn3hid6detail20IsVibrationPermittedEPb");
 need(vinit&&vsend&&vmany&&vactual&&vinfo&&vmounted&&vperm,"rumble on: vibration detail functions answered here");
 /* The SDK's handles for a full-key controller 0 (nn::hid::utility::GetVibrationDeviceHandles): left 0x3, right 0x10003. */
 const uint32_t left=0x3,right=0x10003,joydual=0x5;
 need(!vinit(&left)&&!vinit(&right),"vibration device init");
 need(!vsend(&left,&(Vib){0.5f,160,0.25f,320})&&rumble_calls==1&&rumble_seen[0][0]==0.5f&&rumble_seen[0][1]==160&&rumble_seen[0][2]==0.25f&&rumble_seen[0][3]==320,"left value reaches the left handle");
 need(!vsend(&right,&(Vib){0.1f,90,0.3f,600})&&rumble_seen[1][0]==0.1f&&rumble_seen[1][3]==600&&rumble_seen[0][0]==0.5f,"right value reaches the right handle only");
 const uint32_t pair[2]={left,right};const Vib two[2]={{0.2f,100,0,320},{0,160,0.6f,400}};
 need(!vmany(pair,two,2)&&rumble_seen[0][0]==0.2f&&rumble_seen[1][2]==0.6f&&rumble_calls==4,"a batch (VibrationTarget's path) reaches both handles");
 Vib got;need(!vactual(&got,&right)&&got.ah==0.6f&&got.fh==400,"actual value = the last one sent");
 VibInfo info;need(!vinfo(&info,&left)&&info.type==1&&info.position==1&&!vinfo(&info,&right)&&info.position==2,"device info: actuator, left / right");
 _Bool b=0;need(!vmounted(&b,&left)&&b&&!vperm(&b)&&b,"mounted and permitted");
 int before=rumble_calls;need(!vsend(&joydual,&(Vib){0.9f,160,0.9f,320})&&rumble_calls==before&&!vmounted(&b,&joydual)&&!b&&!vinfo(&info,&joydual)&&!info.type,"a handle this controller does not have plays nothing");
 need(!vsend(&left,&(Vib){NAN,160,0.4f,320})&&rumble_seen[0][0]==0&&rumble_seen[0][2]==0,"a non-finite value plays silence");
 need(runtime_hid_feedback_flag("rumble",0)==1&&rumble_seen[0][0]==0&&rumble_seen[1][2]==0,"flag rumble 0 silences both handles at once");
 need(!vsend(&left,&(Vib){0.7f,160,0.7f,320})&&rumble_seen[0][0]==0&&rumble_seen[0][2]==0&&!vactual(&got,&left)&&got.al==0.7f,"muted: the game's value is kept, the controller gets silence");
 need(runtime_hid_feedback_flag("rumble",1)==1&&!vsend(&left,&(Vib){0.7f,160,0.7f,320})&&rumble_seen[0][0]==0.7f,"flag rumble 1 unmutes");
 need(runtime_hid_feedback_flag("mfx_upscale",1)==0,"other flags are not taken");
 /* Six-axis: the SDK's full-key handle (nn::hid::utility::GetSixAxisSensorHandle) is 0x20003. */
 uint32_t(*sstart)(const uint32_t *)=runtime_input_override("_ZN2nn3hid6detail18StartSixAxisSensorERKNS0_19SixAxisSensorHandleE");
 uint32_t(*sstates)(int *,Six *,int,const uint32_t *)=runtime_input_override("_ZN2nn3hid6detail22GetSixAxisSensorStatesEPiPNS0_18SixAxisSensorStateEiRKNS0_19SixAxisSensorHandleE");
 uint32_t(*sstate)(Six *,const uint32_t *)=runtime_input_override("_ZN2nn3hid6detail21GetSixAxisSensorStateEPNS0_18SixAxisSensorStateERKNS0_19SixAxisSensorHandleE");
 need(sstart&&sstates&&sstate,"gyro on: six-axis detail functions answered here");
 const uint32_t six=0x20003;struct{Six s[16];unsigned char guard[64];} buf;int n=-1;
 probe_clock_fixed=1;probe_now=1000000000ull;
 memset(&buf,0xa5,sizeof buf);need(!sstart(&six)&&!sstates(&n,buf.s,16,&six)&&n==1&&buf.s[0].attr==0&&near3(buf.s[0].acc,0,0,-1,0)&&near3(buf.s[0].dir,1,0,0,0)&&near3(buf.s[0].dir+6,0,0,1,0),"no controller motion: one still, unconnected state");
 runtime_motion_live(1);feed(100,0,0,-1,0,0,0);probe_now+=1000000;
 memset(&buf,0xa5,sizeof buf);need(!sstates(&n,buf.s,16,&six)&&n==16,"16 states when 100 samples are there");
 for(int i=0;i<16;i++)need(buf.s[i].sampling==100-i&&buf.s[i].attr==1&&buf.s[i].dt==4000000,"newest first, sampling numbers, connected, 4 ms apart");
 for(unsigned i=0;i<sizeof buf.guard;i++)need(buf.guard[i]==0xa5,"six-axis output guard");
 need(near3(buf.s[0].acc,0,0,-1,1e-6)&&near3(buf.s[0].dir,1,0,0,1e-6)&&near3(buf.s[0].dir+3,0,1,0,1e-6)&&near3(buf.s[0].dir+6,0,0,1,1e-6)&&near3(buf.s[0].angle,0,0,0,1e-9),"lying still and flat: identity direction, no angle");
 Six one;need(!sstate(&one,&six)&&one.sampling==100,"single state = the newest");
 /* Turning 90 degrees a second about z (counterclockwise seen from above) for 1 s: a quarter turn. */
 sstart(&six);feed(1,0,0,-1,0,0,0);feed(250,0,0,-1,0,0,M_PI/2);probe_now+=1000000;sstates(&n,buf.s,1,&six);
 need(near(buf.s[0].angle[2],0.25,0.002)&&near(buf.s[0].gyro[2],0.25,1e-6)&&near3(buf.s[0].dir,0,1,0,0.01)&&near3(buf.s[0].dir+3,-1,0,0,0.01)&&near3(buf.s[0].dir+6,0,0,1,0.01),"quarter turn about z: angle 0.25 rev, x axis now along y");
 /* A quarter turn about z, then one about the controller's own x (accelerometer out of range, so no gravity pull): the
    turns compose in the controller's frame (Rz * Rx: x along y, y up, z along x); integrated as reference-frame rates
    (probe negative 20) x would end up pointing up. */
 sstart(&six);feed(1,0,0,-1,0,0,0);feed(250,0,0,-2,0,0,M_PI/2);feed(250,0,0,-2,M_PI/2,0,0);probe_now+=1000000;sstates(&n,buf.s,1,&six);
 need(near3(buf.s[0].dir,0,1,0,0.01)&&near3(buf.s[0].dir+3,0,0,1,0.01)&&near3(buf.s[0].dir+6,1,0,0,0.01),"two turns compose in the controller's frame");
 /* Started flat, then held pitched up 30 degrees without the gyro seeing it: the gravity pull tilts the estimate over (~2 s). */
 sstart(&six);feed(1,0,0,-1,0,0,0);feed(2500,0,-0.5,-0.8660254,0,0,0);probe_now+=1000000;sstates(&n,buf.s,1,&six);
 need(near3(buf.s[0].dir+6,0,-0.5,0.8660254,0.02)&&near3(buf.s[0].dir,1,0,0,0.02),"gravity pulls the tilt in, heading unchanged");
 /* TOTK_GYRO_AXES: a rotation remaps both alike; a reflection also flips the rotation's sense. */
 need(runtime_motion_axes(NULL)==0&&runtime_motion_axes("x,x,z")<0&&runtime_motion_axes("x,y")<0&&runtime_motion_axes("a,b,c")<0&&runtime_motion_axes("x,y,z,")<0,"malformed axis specs rejected");
 need(runtime_motion_axes("-x,y,-z")==1,"axis spec");feed(1,0.1,0.2,-0.9,2*M_PI,4*M_PI,6*M_PI);probe_now+=1000000;sstates(&n,buf.s,1,&six);
 need(near3(buf.s[0].acc,-0.1,0.2,0.9,1e-6)&&near3(buf.s[0].gyro,-1,2,-3,1e-5),"a half turn about y remaps acceleration and rotation alike");
 need(runtime_motion_axes("-x,y,z")==1,"axis spec");feed(1,0.1,0.2,-0.9,2*M_PI,4*M_PI,6*M_PI);probe_now+=1000000;sstates(&n,buf.s,1,&six);
 need(near3(buf.s[0].acc,-0.1,0.2,-0.9,1e-6)&&near3(buf.s[0].gyro,1,-2,-3,1e-5),"a mirror flips the rotation's sense");
 need(runtime_motion_axes("x,y,z")==1,"axis spec");
 need(runtime_hid_feedback_flag("gyro",0)==1&&!sstates(&n,buf.s,16,&six)&&n==1&&buf.s[0].attr==0,"flag gyro 0: a still, unconnected controller");
 need(runtime_hid_feedback_flag("gyro",1)==1&&!sstates(&n,buf.s,16,&six)&&n==16&&buf.s[0].attr==1,"flag gyro 1: motion again");
 probe_now+=300000000;need(!sstates(&n,buf.s,16,&six)&&n==1&&buf.s[0].attr==0,"no sample for 300 ms: no motion");
 feed(1,0,0,-1,0,0,0);runtime_motion_live(0);need(!sstates(&n,buf.s,16,&six)&&n==1&&buf.s[0].attr==0,"controller detached: no motion");
 probe_clock_fixed=0;
}
int main(void){
 void(*init)(void)=runtime_input_override("_ZN2nn3hid14InitializeNpadEv");
 int(*read)(State *,int,const uint32_t *)=runtime_input_override("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadFullKeyStateEiRKj");
 uint32_t id=0;struct{State state;unsigned char guard[32];} out;memset(&out,0xa5,sizeof(out));init();
 runtime_input_key(38,1);runtime_input_key(38,0);need(read(&out.state,1,&id)==1&&out.state.buttons==1&&out.state.attr==1,"short tap latched");
 need(read(&out.state,1,&id)==1&&out.state.buttons==0,"released input clears after sample");
 runtime_input_key(13,1);runtime_input_key(2,1);runtime_input_key(123,1);read(&out.state,1,&id);need(out.state.lx==32767&&out.state.ly==32767&&out.state.rx==-32767,"native key axes");
 runtime_input_clear();read(&out.state,1,&id);need(!out.state.buttons&&!out.state.lx&&!out.state.ly&&!out.state.rx,"focus clear");
 fake_pad.buttons=1;fake_pad.connected=1;read(&out.state,1,&id);need(out.state.buttons==1,"held controller A reaches the game");
 runtime_input_clear();read(&out.state,1,&id);need(!out.state.buttons,"focus clear also releases a held controller button");
 const unsigned dpad[]={3,17,4,5};
 for(unsigned i=0;i<4;i++){
  runtime_input_key(dpad[i],1);read(&out.state,1,&id);
  need(out.state.buttons==(1ULL<<(12+i))&&!out.state.lx&&!out.state.ly&&!out.state.rx&&!out.state.ry,"D-pad independent of analog sticks");
  runtime_input_key(dpad[i],0);read(&out.state,1,&id);need(!out.state.buttons,"D-pad release");
 }
 /* Keys 1-4 (Mac key codes 18-21): one action per press; held-key repeats and the release do nothing more; no game
    button; each key touches only its own action. 1 shows / hides the overlays (frontend.m), 2 frame interpolation (runtime
    flag mfx_interp, v = -1), 3 the picture mode (game_patches*.c), 4 the ultrawide aspect ratio (aspect_patch.h). */
 static const unsigned keys[4]={18,19,20,21};int *count[4]={&help_toggles,&interp_toggles,&output_cycles,&aspect_toggles};
 static const char *const pressed[4]={"key 1 toggles the overlays (only)","key 2 toggles frame interpolation (only)","key 3 cycles the picture mode (only)","key 4 toggles ultrawide (only)"};
 for(int k=0;k<4;k++){
  runtime_input_key(keys[k],1);for(int j=0;j<4;j++)need(*count[j]==(j<k?2:j==k?1:0),pressed[k]);
  runtime_input_key(keys[k],1);need(*count[k]==1,"a held key (auto-repeat) does not act again");
  read(&out.state,1,&id);need(!out.state.buttons&&!out.state.lx&&!out.state.ly,"keys 1-4 are no controller input");
  runtime_input_key(keys[k],0);need(*count[k]==1,"releasing the key does not act");
  runtime_input_key(keys[k],1);runtime_input_key(keys[k],0);need(*count[k]==2,"every press acts");
 }
 id=1;read(&out.state,1,&id);need(!out.state.attr&&!out.state.buttons,"unattached controller");
 for(unsigned i=0;i<32;i++)need(out.guard[i]==0xa5,"state output guard");
 /* Handheld mode (the iPhone): the input is the handheld controller (id 0x20, style bit 1); player 1 is not connected. */
 handheld=1;
 void(*styles)(uint32_t)=runtime_input_override("_ZN2nn3hid24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE");
 void(*ids)(const uint32_t *,uint64_t)=runtime_input_override("_ZN2nn3hid22SetSupportedNpadIdTypeEPKjm");
 uint32_t(*style)(const uint32_t *)=runtime_input_override("_ZN2nn3hid15GetNpadStyleSetERKj");
 int(*hread)(State *,int,const uint32_t *)=runtime_input_override("_ZN2nn3hid13GetNpadStatesEPNS0_17NpadHandheldStateEiRKj");
 const uint32_t both[]={0,0x20};styles(3);ids(both,2);
 uint32_t hid=0x20;id=0;need(style(&hid)==2&&style(&id)==0,"handheld mode: handheld controller only");
 void(*disc)(const uint32_t *)=runtime_input_override("_ZN2nn3hid14DisconnectNpadERKj");disc(&id);need(style(&hid)==2,"handheld mode: disconnecting player 1 keeps the handheld controller");
 runtime_input_key(38,1);runtime_input_key(38,0);need(hread(&out.state,1,&hid)==1&&out.state.buttons==1&&out.state.attr==1,"handheld short tap latched");
 read(&out.state,1,&id);need(!out.state.attr&&!out.state.buttons,"handheld mode: player 1 not attached");
 feedback_checks();
 puts("{\"status\":\"pass\",\"scope\":\"input service state ABI, rumble and six-axis hid answers (on by default, off with =0 and in agent test sessions, left/right handles, batches, mute flags, newest-first states, orientation in the controller frame, gravity pull, axis remap), short tap latch, keyboard axes, four D-pad directions, focus clear of keyboard and controller, key 1 overlays / key 2 frame interpolation / key 3 picture mode / key 4 ultrawide (once per press), unattached controller, handheld mode controller; no game control flow\"}");
}
