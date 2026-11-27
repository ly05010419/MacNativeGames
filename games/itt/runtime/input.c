/* Native keyboard and macOS gamepad backed full-key controller. State ABI follows libnx HID. */
#include "runtime.h"
#include "controller_topology.h"
#include "autoload.h"
#include "world_survey.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <sys/file.h>
#include <fcntl.h>
#include <sys/stat.h>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
typedef struct{int64_t sampling;uint64_t buttons;int32_t lx,ly,rx,ry;uint32_t attributes,pad;} NpadState;
_Static_assert(sizeof(NpadState)==40,"Npad full-key ABI");
static pthread_mutex_t inputLock=PTHREAD_MUTEX_INITIALIZER;
static unsigned char keys[128];static uint64_t pending;
static uint32_t styles=1;static bool enabled,allowed=true;static uint32_t allowedMask=1;static int holdType;
static int64_t sample;static uint64_t padSeen[RUNTIME_MAX_GAMEPADS];
static void *styleEvents[RUNTIME_MAX_GAMEPADS];
static uint32_t lastStyleMask;
static _Atomic int padsChanged;
void runtime_input_gamepads_changed(void){atomic_store(&padsChanged,1);}
/* F/T/H/G map the D-pad left/up/right/down, keeping both analog-stick mappings independent. */
static uint64_t button(unsigned code){switch(code){case 38:return 1;case 40:case 49:return 2;case 34:return 4;case 32:return 8;case 6:return 16;case 8:return 32;case 12:return 64;case 14:return 128;case 58:return 256;case 56:return 512;case 36:return 1024;case 48:case 53:return 2048;case 3:return 4096;case 17:return 8192;case 4:return 16384;case 5:return 32768;default:return 0;}}
/* Real user input (a window key, a controller button, a stick pushed past half) stops TOTK_AUTOLOAD and is logged at most
   once a minute. In an agent's test session (TOTK_SLOT_OWNER other than user) the line says the user is playing it: its
   save is a copy, its scripted measurement is void, and its script must not close the game under the user
   (2026-10-15: the user played 12 minutes in a test window and the script closed it at its time limit). */
static void user_input(const char *what){
 runtime_survey_cancel_user();
 runtime_autoload_user_input();
 static _Atomic uint64_t last;uint64_t now=runtime_now_ns(),prev=atomic_load(&last);
 if(prev&&now-prev<60000000000ull)return;if(!atomic_compare_exchange_strong(&last,&prev,now))return;
 const char *owner=getenv("TOTK_SLOT_OWNER");bool agent=owner&&*owner&&strcmp(owner,"user");
 fprintf(stderr,"[input] user input (%s)%s%s\n",what,agent?": the user is playing this test session of ":"",agent?owner:"");
}
/* Keys from the window are the user's; scripted and auto-load presses use the synthetic entry.
   Key 1 toggles the shortcut reminder in frontend.m. Key 2 (Mac key code 19) cycles MetalFX frame interpolation:
   off -> half -> full (totk_mfx_mode.h: generated frames on top of the key 8 frame rate, 60 -> 60+30 / 60+60). Key 3 (key code 20) cycles 2K modes:
   MetalFX 2K -> native 2K -> off (runtime_output_mode_cycle). Held-key repeats do not cycle. */
enum { KEY_INTERP_TOGGLE = 19, KEY_OUTPUT_MODE_CYCLE = 20, KEY_ASPECT_TOGGLE = 21, KEY_MOUSE_TOGGLE = 26, KEY_FPS_CAP_CYCLE = 28 };   /* 21: key 4; 26: key 7; 28: key 8 */
void runtime_input_key_synthetic(unsigned code,int down){if(code>=128)return;pthread_mutex_lock(&inputLock);int press=down&&!keys[code];if(down&&!keys[code])pending|=button(code);keys[code]=(unsigned char)!!down;pthread_mutex_unlock(&inputLock);
 if(press&&code==18){extern void runtime_shortcut_help_toggle(void);runtime_shortcut_help_toggle();}
 if(press&&code==KEY_INTERP_TOGGLE){extern void dm_gfx_set_flag(const char *name,int v);dm_gfx_set_flag("mfx_interp",-1);}
 if(press&&code==KEY_OUTPUT_MODE_CYCLE)runtime_output_mode_cycle();
 if(press&&code==KEY_ASPECT_TOGGLE)runtime_aspect_toggle();
 if(press&&code==KEY_MOUSE_TOGGLE)runtime_mouse_toggle();
 if(press&&code==KEY_FPS_CAP_CYCLE)runtime_fps_cap_cycle();}
/* User-triggered diagnostics only: no automatic capture during play and no changes to
   guest rendering. F9 is otherwise unmapped. The parent directory must already exist.
   Captures stall the GPU/read back textures: never use their timings as FPS evidence. */
static void effect_capture(void){
 const char *root=getenv("ITT_EFFECT_CAPTURE_DIR");
 if(!root||!*root)return;
 /* The existing texture-dump API treats its path as a printf format. */
 if(strlen(root)>800||strchr(root,'%')){fprintf(stderr,"[itt-capture] invalid capture directory\n");return;}
 static uint64_t last;uint64_t now=runtime_now_ns();
 if(last&&now-last<10000000000ull)return;
 char dir[1024],path[1200];
 snprintf(dir,sizeof dir,"%s/effect-%llu",root,(unsigned long long)now);
 if(mkdir(dir,0700)){fprintf(stderr,"[itt-capture] cannot create capture directory\n");return;}
 last=now;
 extern void dm_gfx_passdump_next(long n);
 extern void dm_gfx_texdump_next(const char *path);
 extern void dm_gfx_shot_at(long k,const char *path);
 dm_gfx_passdump_next(4);
 snprintf(path,sizeof path,"%s/texture-%%d.png",dir);dm_gfx_texdump_next(path);
 for(int i=0;i<12;i++){snprintf(path,sizeof path,"%s/frame-%02d.png",dir,i);dm_gfx_shot_at(2+i,path);}
 fprintf(stderr,"[itt-capture] user requested death/effect capture: %s (diagnostic timings only)\n",dir);
}
void runtime_input_key(unsigned code,int down){if(runtime_input_settings_active())return;
 static int capture_down;
 if(code==101){int press=down&&!capture_down;capture_down=!!down;if(press)effect_capture();}if(down&&code<128)user_input("keyboard");
#if defined(__APPLE__)
 #if TARGET_OS_OSX
 /* Host shortcuts are independent of saved bindings and player sources.
    Use the existing edge tracker for repeats, releases and focus clearing. */
 if(code==18||code==KEY_INTERP_TOGGLE||code==KEY_OUTPUT_MODE_CYCLE||code==KEY_ASPECT_TOGGLE||code==KEY_MOUSE_TOGGLE||code==KEY_FPS_CAP_CYCLE){
  runtime_input_key_synthetic(code,down);return;
 }
 if(runtime_keyboard_mac_key(code,down))return;
 /* An inactive reservation must not fall back into player 1. Legacy gameplay
    is only for an unassigned keyboard and a keyboard-enabled player 1. */
 if(runtime_keyboard_slot()>=0||!runtime_input_source_allows(0,INPUT_DEVICE_KEYBOARD))return;
 #endif
#endif
 runtime_input_key_synthetic(code,down);}
void runtime_keys_clear(void){pthread_mutex_lock(&inputLock);memset(keys,0,sizeof(keys));pending=0;pthread_mutex_unlock(&inputLock);}
void runtime_input_clear(void){runtime_keys_clear();runtime_gamepad_clear();}
static void initialize(void){pthread_mutex_lock(&inputLock);enabled=true;pthread_mutex_unlock(&inputLock);fprintf(stderr,"[totk] native keyboard controller initialized\n");}
/* Docked: one full-key controller as player 1 (id 0, style bit 0). Handheld (TOTK_OPERATION_MODE=handheld, the iPhone):
   the same input is the handheld controller (id 0x20, style bit 1, joy-cons attached) and player 1 is not connected;
   the game reads only the handheld controller in that mode. */
/* TOTK_INPUT_LOG=1: one line per new (state kind, controller id, style set) the game reads (which pad the game uses). */
static void input_log(const char *what,uint32_t id){static int on=-1;static uint64_t seen[64];static unsigned n;if(on<0)on=getenv("TOTK_INPUT_LOG")!=NULL;if(!on)return;
 uint64_t k=(uint64_t)(uintptr_t)what<<32^id^(uint64_t)styles<<48;for(unsigned i=0;i<n;i++)if(seen[i]==k)return;if(n<64)seen[n++]=k;
 fprintf(stderr,"[totk-input] game reads %s for controller %#x (supported styles %#x, ids allowed %d)\n",what,id,styles,allowed);}
static uint32_t pad_id(void){return d3_handheld_controller()?0x20u:0u;}
static uint32_t pad_style(void){return d3_handheld_controller()?2u:1u;}
static bool pad_slot_active(uint32_t id){
 if(d3_handheld_controller())return allowed&&id==0x20u&&runtime_input_source(0)!=INPUT_SOURCE_OFF;
 if(id>=RUNTIME_MAX_GAMEPADS||!(allowedMask&(1u<<id)))return false;
 if(id==0&&runtime_input_source(0)==INPUT_SOURCE_COMPAT)return true;
 return runtime_gamepad_connected(id)!=0;
}
static void supported_ids(const uint32_t *ids,uint64_t n){if(n>10||(!ids&&n))runtime_fail("invalid controller ids");
 if(getenv("TOTK_INPUT_LOG")){char b[128];size_t o=0;for(uint64_t i=0;i<n&&o<100;i++)o+=(size_t)snprintf(b+o,sizeof b-o," %#x",ids[i]);b[o]=0;fprintf(stderr,"[totk-input] game supports controller ids:%s\n",b);}pthread_mutex_lock(&inputLock);allowed=false;allowedMask=0;for(uint64_t i=0;i<n;i++){if(ids[i]==pad_id())allowed=true;if(ids[i]<RUNTIME_MAX_GAMEPADS)allowedMask|=1u<<ids[i];}pthread_mutex_unlock(&inputLock);}
static void supported_styles(uint32_t value){pthread_mutex_lock(&inputLock);styles=value;pthread_mutex_unlock(&inputLock);}
extern void hle_bind_system_event(void *ev, int autoclear);
extern void hle_signal_system_event(void *ev);
static uint32_t style(const uint32_t *id){if(!id)runtime_fail("controller id pointer");input_log("style set",*id);pthread_mutex_lock(&inputLock);bool act=enabled&&pad_slot_active(*id);uint32_t v=act?(styles&pad_style()):0;
 if(*id<RUNTIME_MAX_GAMEPADS){uint32_t bit=1u<<*id;if(act&&!(lastStyleMask&bit)&&styleEvents[*id]){lastStyleMask|=bit;hle_signal_system_event(styleEvents[*id]);}}
 pthread_mutex_unlock(&inputLock);return v;}
/* A controller attach or removal happens on the main thread, which cannot signal the game's event. The next read on a
   guest thread does: otherwise a reconnected pad stays "disconnected" because the game stopped polling it. */
static void note_pad_changes(void){if(!atomic_exchange(&padsChanged,0))return;int actv[RUNTIME_MAX_GAMEPADS]={0};
 for(uint32_t id=0;id<RUNTIME_MAX_GAMEPADS;id++)actv[id]=pad_slot_active(id);
 pthread_mutex_lock(&inputLock);
 for(uint32_t id=0;id<RUNTIME_MAX_GAMEPADS;id++){if(!styleEvents[id])continue;int act=enabled&&actv[id];uint32_t bit=1u<<id;if(act==!!(lastStyleMask&bit))continue;if(act)lastStyleMask|=bit;else lastStyleMask&=~bit;hle_signal_system_event(styleEvents[id]);fprintf(stderr,"[totk-input] controller %u %s\n",id,act?"connected":"disconnected");}
 pthread_mutex_unlock(&inputLock);}
static int pad_states(NpadState *out,int max,const uint32_t *id,uint32_t want_style);
static int full_states(NpadState *out,int max,const uint32_t *id){return pad_states(out,max,id,1);}
static int handheld_states(NpadState *out,int max,const uint32_t *id){return pad_states(out,max,id,2);}
static int pad_states(NpadState *out,int max,const uint32_t *id,uint32_t want_style){if(max<=0)return 0;if(id)input_log(want_style==2?"handheld state":"full-key state",*id);if(!out||!id)runtime_fail("controller output pointer");note_pad_changes();pthread_mutex_lock(&inputLock);NpadState state={.sampling=++sample};uint32_t cid=*id;if(enabled&&pad_slot_active(cid)&&want_style==pad_style()&&(styles&want_style)){
 unsigned slot=d3_handheld_controller()?0u:cid;
 state.attributes=1;
 if(slot==0&&runtime_input_source_allows(0,INPUT_DEVICE_KEYBOARD)&&!runtime_input_settings_active()){
  for(unsigned i=0;i<128;i++)if(keys[i])state.buttons|=button(i);state.buttons|=pending;pending=0;
  state.lx=32767*((int)keys[2]-(int)keys[0]);state.ly=32767*((int)keys[13]-(int)keys[1]);state.rx=32767*((int)keys[124]-(int)keys[123]);state.ry=32767*((int)keys[126]-(int)keys[125]);
 }
 RuntimeGamepadState pad;runtime_gamepad_read_index(slot,&pad);state.buttons|=pad.buttons;
 if(pad.buttons&~padSeen[slot]||abs(pad.lx)>16384||abs(pad.ly)>16384||abs(pad.rx)>16384||abs(pad.ry)>16384)user_input("controller");padSeen[slot]=pad.buttons;
 if(abs(pad.lx)>abs(state.lx))state.lx=pad.lx;if(abs(pad.ly)>abs(state.ly))state.ly=pad.ly;
 if(abs(pad.rx)>abs(state.rx))state.rx=pad.rx;if(abs(pad.ry)>abs(state.ry))state.ry=pad.ry;
 }*out=state;pthread_mutex_unlock(&inputLock);return 1;}
static int disconnected_states(NpadState *out,int max,const uint32_t *id){if(id)input_log("joy-con state",*id);if(max<=0)return 0;if(!out)runtime_fail("controller output pointer");pthread_mutex_lock(&inputLock);*out=(NpadState){.sampling=++sample};pthread_mutex_unlock(&inputLock);return 1;}
static void hold_type(int type){if(type<0||type>1)runtime_fail("invalid joy hold type");holdType=type;}
static int get_hold_type(void){return holdType;}
static void disconnect(const uint32_t *id){if(!id)runtime_fail("disconnect id");input_log("disconnect",*id);if(*id==pad_id()){   /* only the pad in use: in handheld mode TOTK disconnects player 1 */pthread_mutex_lock(&inputLock);allowed=false;allowedMask&=~1u;pthread_mutex_unlock(&inputLock);runtime_input_clear();}}
/* Mouse and keyboard devices: the Mac keyboard drives the full-key controller, so the game sees no mouse
   (not connected) and a keyboard with no keys, avoiding duplicated input. Layouts follow the SDK (0x28 / 0x30 bytes). */
typedef struct{int64_t sampling;int32_t x,y,dx,dy,wx,wy;uint32_t buttons,attributes;} MouseState;
typedef struct{int64_t sampling;uint32_t modifiers,attributes;uint64_t keys[4];} KeyboardState;
_Static_assert(sizeof(MouseState)==40&&sizeof(KeyboardState)==48,"mouse/keyboard ABI");
static bool mouseReady,keyboardReady;
static void initialize_mouse(void){pthread_mutex_lock(&inputLock);mouseReady=true;pthread_mutex_unlock(&inputLock);}
static void initialize_keyboard(void){pthread_mutex_lock(&inputLock);keyboardReady=true;pthread_mutex_unlock(&inputLock);}
static int mouse_states(MouseState *out,int max){if(max<=0)return 0;if(!out)runtime_fail("mouse output pointer");pthread_mutex_lock(&inputLock);if(!mouseReady)runtime_fail("mouse used before InitializeMouse");*out=(MouseState){.sampling=++sample};pthread_mutex_unlock(&inputLock);return 1;}
static void keyboard_state(KeyboardState *out){if(!out)runtime_fail("keyboard output pointer");pthread_mutex_lock(&inputLock);if(!keyboardReady)runtime_fail("keyboard used before InitializeKeyboard");*out=(KeyboardState){.sampling=++sample};pthread_mutex_unlock(&inputLock);}
/* Controller support applet (the Switch's "connect controllers" screen). TOTK opens it in handheld mode; report all
   currently connected controllers (at least 1). */
typedef struct{int8_t players;uint8_t pad[3];uint32_t selected;} ControllerSupportResult;
_Static_assert(sizeof(ControllerSupportResult)==8,"controller support result ABI");
static void controller_support_set_default(void *arg){
 if(!arg)return;
 memset(arg,0,0x210);
 uint8_t *p=(uint8_t *)arg;
 p[0]=0; p[1]=RUNTIME_MAX_GAMEPADS; p[2]=1; p[3]=1;
}
static uint32_t show_controller_support(ControllerSupportResult *out,const void *arg){
 (void)arg;unsigned n=runtime_gamepad_count();if(n<1)n=1;if(n>RUNTIME_MAX_GAMEPADS)n=RUNTIME_MAX_GAMEPADS;
 if(out)*out=(ControllerSupportResult){.players=(int8_t)n,.selected=pad_id()};
 fprintf(stderr,"[totk] controller support applet: %u player(s) on controller %#x\n",n,pad_id());
 return 0;
}
/* Bayonetta 3 reads one state at a time (GetNpadState), has a touch screen (no touches here: the Mac has none), and sets
   the handheld activation mode. */
static void full_state(NpadState *out,const uint32_t *id){full_states(out,1,id);}
static void handheld_state(NpadState *out,const uint32_t *id){handheld_states(out,1,id);}
static void disconnected_state(NpadState *out,const uint32_t *id){disconnected_states(out,1,id);}
static void mouse_state(MouseState *out){mouse_states(out,1);}
static void handheld_activation(int mode){(void)mode;}
static void initialize_touch(void){}
static void bind_style_event(const uint32_t *id, void *ev, int clear_mode) {
    if (!id || !ev) runtime_fail("BindNpadStyleSetUpdateEvent pointer");
    hle_bind_system_event(ev, clear_mode);
    pthread_mutex_lock(&inputLock);
    if (*id < RUNTIME_MAX_GAMEPADS) styleEvents[*id] = ev;
    bool act = pad_slot_active(*id);
    if (act && *id < RUNTIME_MAX_GAMEPADS) lastStyleMask |= 1u << *id;
    pthread_mutex_unlock(&inputLock);
    if (act || *id == pad_id()) hle_signal_system_event(ev);
}
static void strap_guide(void) {}
/* TouchScreenState<N>: {int64 sampling, int32 count, int32 reserved, TouchState[N] of 0x28 bytes}; count 0. */
static void touch_states_1(uint8_t *out){if(!out)runtime_fail("touch output pointer");pthread_mutex_lock(&inputLock);memset(out,0,16+1*0x28);int64_t n=++sample;memcpy(out,&n,8);pthread_mutex_unlock(&inputLock);}
static void touch_states_16(uint8_t *out){if(!out)runtime_fail("touch output pointer");pthread_mutex_lock(&inputLock);memset(out,0,16+16*0x28);int64_t n=++sample;memcpy(out,&n,8);pthread_mutex_unlock(&inputLock);}
static uint32_t show_controller_support_arg(const void *arg){return show_controller_support(NULL,arg);}
/* Vibration devices and the six-axis sensor: runtime/hid_feedback.c (none unless TOTK_RUMBLE=1 / TOTK_GYRO=1). */
void *runtime_input_override(const char *name){
 void *feedback=runtime_hid_feedback_override(name);if(feedback)return feedback;
 static const struct{const char *name;void *fn;} entries[]={
 {"_ZN2nn3hid14InitializeNpadEv",initialize},{"_ZN2nn3hid22SetSupportedNpadIdTypeEPKjm",supported_ids},{"_ZN2nn3hid24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE",supported_styles},
 {"_ZN2nn3hid15GetNpadStyleSetERKj",style},{"_ZN2nn3hid13GetNpadStatesEPNS0_16NpadFullKeyStateEiRKj",full_states},
 {"_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyDualStateEiRKj",disconnected_states},{"_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyLeftStateEiRKj",disconnected_states},
 {"_ZN2nn3hid13GetNpadStatesEPNS0_17NpadJoyRightStateEiRKj",disconnected_states},{"_ZN2nn3hid13GetNpadStatesEPNS0_17NpadHandheldStateEiRKj",handheld_states},
 {"_ZN2nn3hid18SetNpadJoyHoldTypeENS0_15NpadJoyHoldTypeE",hold_type},{"_ZN2nn3hid18GetNpadJoyHoldTypeEv",get_hold_type},{"_ZN2nn3hid14DisconnectNpadERKj",disconnect},
 {"_ZN2nn3hid20ControllerSupportArg10SetDefaultEv",controller_support_set_default},
 {"_ZN2nn3hid21ShowControllerSupportEPNS0_27ControllerSupportResultInfoERKNS0_20ControllerSupportArgE",show_controller_support},
 {"_ZN2nn3hid15InitializeMouseEv",initialize_mouse},{"_ZN2nn3hid18InitializeKeyboardEv",initialize_keyboard},
 {"_ZN2nn3hid14GetMouseStatesEPNS0_10MouseStateEi",mouse_states},{"_ZN2nn3hid16GetKeyboardStateEPNS0_13KeyboardStateE",keyboard_state},
 {"_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj",full_state},{"_ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj",handheld_state},
 {"_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyDualStateERKj",disconnected_state},{"_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyLeftStateERKj",disconnected_state},
 {"_ZN2nn3hid12GetNpadStateEPNS0_17NpadJoyRightStateERKj",disconnected_state},{"_ZN2nn3hid13GetMouseStateEPNS0_10MouseStateE",mouse_state},
 {"_ZN2nn3hid29SetNpadHandheldActivationModeENS0_26NpadHandheldActivationModeE",handheld_activation},{"_ZN2nn3hid21InitializeTouchScreenEv",initialize_touch},
 {"_ZN2nn3hid19GetTouchScreenStateILm1EEEvPNS0_16TouchScreenStateIXT_EEE",touch_states_1},{"_ZN2nn3hid19GetTouchScreenStateILm16EEEvPNS0_16TouchScreenStateIXT_EEE",touch_states_16},
 {"_ZN2nn3hid21ShowControllerSupportERKNS0_20ControllerSupportArgE",show_controller_support_arg},
 {"_ZN2nn3hid27BindNpadStyleSetUpdateEventERKjPNS_2os15SystemEventTypeENS3_14EventClearModeE",bind_style_event},
 {"_ZN2nn3hid24ShowControllerStrapGuideEv",strap_guide}};
 for(unsigned i=0;i<sizeof(entries)/sizeof(*entries);i++)if(!strcmp(name,entries[i].name))return entries[i].fn;return NULL;
}

/* TOTK_INPUT_SCRIPT="sec:keycode:ms,...": press Mac key codes at given seconds after start (diagnostics without a
   picture: does the game react to a button). Key codes are the frontend's (J=38 is A, Return=36 is Plus). */
#include <unistd.h>
static void *input_script(void *arg) {
    const char *p = arg; uint64_t t0 = runtime_now_ns();
    while (p && *p) {
        double at = strtod(p, (char **)&p); if (*p++ != ':') break;
        unsigned code = (unsigned)strtoul(p, (char **)&p, 10); if (*p++ != ':') break;
        unsigned ms = (unsigned)strtoul(p, (char **)&p, 10); if (*p == ',') p++;
        while ((runtime_now_ns() - t0) / 1e9 < at) usleep(10000);
        fprintf(stderr, "[totk-input] script: key %u down for %u ms at %.1f s\n", code, ms, at);
        runtime_input_key_synthetic(code, 1); usleep(ms * 1000); runtime_input_key_synthetic(code, 0);
    }
    return NULL;
}
__attribute__((constructor)) static void input_script_start(void) {
    const char *e = getenv("TOTK_INPUT_SCRIPT"); if (!e) return;
    pthread_t t; pthread_create(&t, NULL, input_script, (void *)e); pthread_detach(t);
}

/* Optional local integration-test command stream. Appending complete lines lets tests inspect each
   actual frame before choosing the next input, instead of guessing load/menu timings. Disabled normally.
   Commands: key <Mac keycode> <milliseconds>, down/up <Mac keycode>, shot <path>, wshot <path>, passdump <frames>, gpucapture <path>.gputrace, querylog <frames>, anomdump,
   kbtest / kbtype <text> / kbenter / kbesc / kbshot <path> (the text entry bar), threads (dump every guest thread's wait), hletrace <0|1|2> (host calls the game makes), framestep <ms> (pin the frame step), close. */
extern void dm_gfx_shot_at(long frames, const char *path);
extern void runtime_front_request_close(void);
static void *input_file(void *arg) {
    FILE *f = fopen((const char *)arg, "r"); if (!f) runtime_fail("cannot open TOTK_INPUT_FILE");
    if(fcntl(fileno(f),F_SETFD,FD_CLOEXEC))runtime_fail("protect diagnostic input descriptor");
    if(flock(fileno(f),LOCK_EX|LOCK_NB))runtime_fail("TOTK_INPUT_FILE is already used by another process");
    char line[1200];
    for (;;) {
        if (!fgets(line, sizeof line, f)) { if (ferror(f)) runtime_fail("diagnostic input read"); clearerr(f); usleep(100000); continue; }
        if (runtime_survey_command(line)) continue;
        unsigned code, ms; int used = 0;
        if (sscanf(line, "down %u %n", &code, &used) == 1 && used && !line[used] && code < 128) {
            fprintf(stderr,"[totk-input] file: key %u down\n",code);runtime_input_key_synthetic(code,1);
        } else if (sscanf(line, "up %u %n", &code, &used) == 1 && used && !line[used] && code < 128) {
            fprintf(stderr,"[totk-input] file: key %u up\n",code);runtime_input_key_synthetic(code,0);
        } else if (sscanf(line, "key %u %u %n", &code, &ms, &used) == 2 && used && !line[used] && code < 128 && ms > 0 && ms <= 5000) {
            fprintf(stderr, "[totk-input] file: key %u for %u ms\n", code, ms);
            runtime_input_key_synthetic(code, 1); usleep(ms * 1000); runtime_input_key_synthetic(code, 0);
        } else if (sscanf(line, "passdump %u %n", &code, &used) == 1 && used && !line[used] && code > 0 && code < 16) {
            extern void dm_gfx_passdump_next(long n); dm_gfx_passdump_next((long)code);
        } else if (!strncmp(line, "gpucapture ", 11) && line[11]) {   /* "gpucapture <path>.gputrace": a Metal capture of the next frame (needs MTL_CAPTURE_ENABLED=1) */
            char *e = line + strlen(line); while (e > line + 11 && (e[-1] == '\n' || e[-1] == '\r')) *--e = 0;
            extern void dm_gfx_gpucapture_next(const char *path); dm_gfx_gpucapture_next(line + 11);
        } else if (!strncmp(line, "anomdump", 8) && (line[8] == '\n' || line[8] == '\r' || !line[8])) {   /* TOTK_ANOMALY=<dir>: write the per-draw records (with GPU sample counts) of the last frames now */
            extern void dm_gfx_anomdump(void); dm_gfx_anomdump();
        } else if (sscanf(line, "querylog %u %n", &code, &used) == 1 && used && !line[used] && code > 0 && code <= 600) {
            extern void dm_gfx_querylog_next(long n); dm_gfx_querylog_next((long)code);   /* occlusion query results of the next n frames */
        } else if (!strncmp(line, "flag ", 5)) {
            /* "flag <name> <0|1>": switch a graphics behaviour while the game runs (A/B without restarting); rumble / gyro mute and unmute those */
            char name[64]; int v; if (sscanf(line + 5, "%63s %d", name, &v) != 2) runtime_fail("diagnostic flag needs <name> <0|1>");
            extern void dm_gfx_set_flag(const char *name, int v); if (!runtime_hid_feedback_flag(name, v)) dm_gfx_set_flag(name, v);
        } else if (!strncmp(line, "texdump ", 8)) {
            extern void dm_gfx_texdump_next(const char *path);
            line[strcspn(line, "\r\n")] = 0; if (!line[8] || !strstr(line + 8, "%d")) runtime_fail("diagnostic texdump path needs %d");
            dm_gfx_texdump_next(line + 8);
        } else if (sscanf(line, "shotn %u %n", &code, &used) == 1 && used && code > 0 && code <= 60) {
            /* "shotn <n> <path with %d>": screenshots of n consecutive game frames (frame flicker); %d = 0..n-1 */
            line[strcspn(line, "\r\n")] = 0; const char *fmt = line + used;
            if (!*fmt || !strstr(fmt, "%d") || strchr(strstr(fmt, "%d") + 2, '%')) runtime_fail("diagnostic shotn path needs one %d");
            for (unsigned i = 0; i < code; i++) { char p[1200]; snprintf(p, sizeof p, fmt, (int)i); dm_gfx_shot_at(2 + (long)i, p); }
        } else if (!strncmp(line, "shot ", 5)) {
            line[strcspn(line, "\r\n")] = 0; if (!line[5]) runtime_fail("diagnostic screenshot path");
            dm_gfx_shot_at(2, line + 5);
        } else if (!strncmp(line, "wshot ", 6)) {   /* "wshot <path>": the next shown picture as the window shows it (FPS box and the bottom-left box included) */
            line[strcspn(line, "\r\n")] = 0; if (!line[6]) runtime_fail("diagnostic window screenshot path");
            extern void dm_gfx_window_shot(const char *path); dm_gfx_window_shot(line + 6);
        } else if (!strcmp(line, "kbtest\n")) {   /* text entry bar the way the game names a horse (runtime/swkbd_host.c) */
            runtime_swkbd_selftest();
        } else if (!strncmp(line, "kbtype ", 7)) {   /* "kbtype <text>": typed into the open bar */
            line[strcspn(line, "\r\n")] = 0; runtime_front_text_test("type", line + 7);
        } else if (!strncmp(line, "kbshot ", 7)) {   /* "kbshot <path>": the open bar as a PNG */
            line[strcspn(line, "\r\n")] = 0; runtime_front_text_test("shot", line + 7);
        } else if (!strcmp(line, "kbenter\n") || !strcmp(line, "kbesc\n")) {
            runtime_front_text_test(line[2] == 'e' && line[3] == 'n' ? "enter" : "esc", NULL);
        } else if (sscanf(line, "hletrace %u %n", &code, &used) == 1 && used && !line[used] && code <= 2) {
            runtime_hle_trace_set((int)code);   /* 1 count host calls, 2 also log the first call of each, 0 stop and print the counts */
        } else if (!strncmp(line, "framestep ", 10)) {   /* "framestep <ms>": pin the real-time frame step, 0 = back to real time */
            char *end; double ms = strtod(line + 10, &end); if (end == line + 10 || ms < 0 || ms > 100) runtime_fail("framestep takes milliseconds (0 = real time)");
            runtime_frame_step_pin(ms);
        } else if (!strcmp(line, "threads\n")) {   /* every guest thread's wait state and guest return chain, now (a hang: what is each thread waiting for) */
            runtime_dump_threads();
        } else if (!strcmp(line, "close\n")) { runtime_front_request_close(); break; }
        else runtime_fail("invalid diagnostic input command");
    }
    fclose(f); return NULL;
}
__attribute__((constructor)) static void input_file_start(void) {
    const char *e = getenv("TOTK_INPUT_FILE"); if (!e || !*e) return;
    pthread_t t; if (pthread_create(&t, NULL, input_file, (void *)e)) runtime_fail("diagnostic input thread"); pthread_detach(t);
}
