/* Native keyboard and macOS gamepad backed full-key controller. State ABI follows libnx HID. */
#include "runtime.h"
#include "autoload.h"
#include "world_survey.h"
#include "keyboard_map.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
typedef struct{int64_t sampling;uint64_t buttons;int32_t lx,ly,rx,ry;uint32_t attributes,pad;} NpadState;
_Static_assert(sizeof(NpadState)==40,"Npad full-key ABI");
static pthread_mutex_t inputLock=PTHREAD_MUTEX_INITIALIZER;
static unsigned char keys[128];static uint64_t pending;
static uint32_t styles=1;static bool enabled,allowed=true;static int holdType;
static int64_t sample;static uint64_t padSeen;
/* Scripted and auto-load presses (runtime_input_key_synthetic: TOTK_INPUT_SCRIPT, TOTK_INPUT_FILE, autoload.c) keep this
   fixed layout, whatever the player's, so test scripts mean the same keys on every Mac: J A, K / Space B, I X, U Y, Q / E
   L / R, Option / Shift ZL / ZR, Return +, Tab / Esc -, F T H G the d-pad, Z / C the stick presses; W A S D and the arrow
   keys the sticks (pad_states). */
static uint64_t button(unsigned code){switch(code){case 38:return 1;case 40:case 49:return 2;case 34:return 4;case 32:return 8;case 6:return 16;case 8:return 32;case 12:return 64;case 14:return 128;case 58:return 256;case 56:return 512;case 36:return 1024;case 48:case 53:return 2048;case 3:return 4096;case 17:return 8192;case 4:return 16384;case 5:return 32768;default:return 0;}}
/* The player's keys and mouse buttons go through their layout (keyboard_map.h, keyboard.txt next to controls.txt, the
   settings panel's key bindings). held: inputs down (keys 0-127, mouse buttons 128-132); heldOut what they drive. */
static KeyMap keyMap;static int keyMapLoaded;
static unsigned char held[KEYMAP_INPUTS];static KeyMapOutput heldOut;
static void keymap_load_locked(void){
 if(keyMapLoaded)return;keyMapLoaded=1;keymap_default(&keyMap);
 char text[8192];if(!runtime_setting_word("keyboard.txt",text,sizeof text)){fprintf(stderr,"[totk-input] keyboard layout: default\n");return;}
 const char *error=NULL;int line=keymap_parse(text,&keyMap,&error);
 if(line)fprintf(stderr,"[totk-input] keyboard.txt line %d: %s; using the default keyboard layout\n",line,error);
 else fprintf(stderr,"[totk-input] keyboard layout: keyboard.txt\n");
}
/* inputLock held: what the held inputs drive now; a press shorter than one game read is latched in pending. */
static void held_changed_locked(void){keymap_load_locked();KeyMapOutput out=keymap_apply(&keyMap,held);pending|=out.buttons&~heldOut.buttons;heldOut=out;}
void runtime_keymap_get(KeyMap *out){pthread_mutex_lock(&inputLock);keymap_load_locked();*out=keyMap;pthread_mutex_unlock(&inputLock);}
/* The settings panel's key bindings: in force at once and saved (keyboard.txt); reset: the default, the file removed. */
void runtime_keymap_set(const KeyMap *map){
 char text[8192];int n=keymap_format(map,text,sizeof text);
 pthread_mutex_lock(&inputLock);keyMapLoaded=1;keyMap=*map;held_changed_locked();pthread_mutex_unlock(&inputLock);
 if(n>0)runtime_setting_save("keyboard.txt",text,"keyboard layout");
}
void runtime_keymap_reset(void){
 KeyMap map;keymap_default(&map);
 pthread_mutex_lock(&inputLock);keyMapLoaded=1;keyMap=map;held_changed_locked();pthread_mutex_unlock(&inputLock);
 runtime_setting_remove("keyboard.txt");fprintf(stderr,"[totk-input] keyboard layout: default (reset)\n");
}
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
/* Keys from the window are the user's (their layout); scripted and auto-load presses use the synthetic entry (the fixed
   layout above). Key 1 (Mac key code 18) opens and closes the settings panel (frontend.m) from either; held-key repeats do
   not toggle it. Keys 2-4 are no shortcuts any more (reported in play, PR #1305: the panel replaces them, as in games/totk). */
enum { KEY_SETTINGS = KEYMAP_SETTINGS_KEY };
void runtime_input_key_synthetic(unsigned code,int down){if(code>=128)return;pthread_mutex_lock(&inputLock);int press=down&&!keys[code];if(down&&!keys[code])pending|=button(code);keys[code]=(unsigned char)!!down;pthread_mutex_unlock(&inputLock);
 if(press&&code==KEY_SETTINGS)runtime_settings_toggle();}
void runtime_input_key(unsigned code,int down){
 if(code>=128)return;if(down)user_input("keyboard");
 pthread_mutex_lock(&inputLock);int press=down&&!held[code];held[code]=(unsigned char)!!down;held_changed_locked();pthread_mutex_unlock(&inputLock);
 if(press&&code==KEY_SETTINGS)runtime_settings_toggle();
}
/* A mouse button over the game window (frontend.m: 0 left, 1 right, 2 middle, 3 and 4 the side buttons). */
void runtime_input_mouse(int button,int down){
 if(button<0||button>=KEYMAP_MOUSE_BUTTONS)return;if(down)user_input("mouse");
 pthread_mutex_lock(&inputLock);held[KEYMAP_MOUSE_FIRST+button]=(unsigned char)!!down;held_changed_locked();pthread_mutex_unlock(&inputLock);
}
void runtime_input_clear(void){pthread_mutex_lock(&inputLock);memset(keys,0,sizeof(keys));memset(held,0,sizeof held);heldOut=(KeyMapOutput){0};pending=0;pthread_mutex_unlock(&inputLock);runtime_gamepad_clear();}
static void initialize(void){pthread_mutex_lock(&inputLock);enabled=true;pthread_mutex_unlock(&inputLock);fprintf(stderr,"[totk] native keyboard controller initialized\n");}
/* Docked: one full-key controller as player 1 (id 0, style bit 0). Handheld (TOTK_OPERATION_MODE=handheld, the iPhone):
   the same input is the handheld controller (id 0x20, style bit 1, joy-cons attached) and player 1 is not connected;
   the game reads only the handheld controller in that mode. */
/* TOTK_INPUT_LOG=1: one line per new (state kind, controller id, style set) the game reads (which pad the game uses). */
static void input_log(const char *what,uint32_t id){static int on=-1;static uint64_t seen[64];static unsigned n;if(on<0)on=getenv("TOTK_INPUT_LOG")!=NULL;if(!on)return;
 uint64_t k=(uint64_t)(uintptr_t)what<<32^id^(uint64_t)styles<<48;for(unsigned i=0;i<n;i++)if(seen[i]==k)return;if(n<64)seen[n++]=k;
 fprintf(stderr,"[totk-input] game reads %s for controller %#x (supported styles %#x, ids allowed %d)\n",what,id,styles,allowed);}
static uint32_t pad_id(void){return runtime_handheld_mode()?0x20u:0u;}
static uint32_t pad_style(void){return runtime_handheld_mode()?2u:(1u|4u|8u|16u);}
static void supported_ids(const uint32_t *ids,uint64_t n){if(n>10||(!ids&&n))runtime_fail("invalid controller ids");
 if(getenv("TOTK_INPUT_LOG")){char b[128];size_t o=0;for(uint64_t i=0;i<n&&o<100;i++)o+=(size_t)snprintf(b+o,sizeof b-o," %#x",ids[i]);b[o]=0;fprintf(stderr,"[totk-input] game supports controller ids:%s\n",b);}pthread_mutex_lock(&inputLock);allowed=false;for(uint64_t i=0;i<n;i++)if(ids[i]==pad_id()||ids[i]==0||ids[i]==0x20)allowed=true;pthread_mutex_unlock(&inputLock);}
static void supported_styles(uint32_t value){pthread_mutex_lock(&inputLock);styles=value;pthread_mutex_unlock(&inputLock);}
static uint32_t style(const uint32_t *id){
    if(!id)runtime_fail("controller id pointer");
    input_log("style set",*id);
    pthread_mutex_lock(&inputLock);
    uint32_t v = 0;
    if(enabled && allowed && (*id == pad_id() || *id == 0 || *id == 0x20)){
        if (runtime_handheld_mode()) v = 2u;
        else if (styles & 4u) v = 4u;
        else v = 1u;
    }
    pthread_mutex_unlock(&inputLock);
    return v;
}
static int pad_states(NpadState *out,int max,const uint32_t *id,uint32_t want_style);
static int full_states(NpadState *out,int max,const uint32_t *id){return pad_states(out,max,id,1);}
static int handheld_states(NpadState *out,int max,const uint32_t *id){return pad_states(out,max,id,2);}
static int joy_dual_states(NpadState *out,int max,const uint32_t *id){return pad_states(out,max,id,4);}
static int joy_left_states(NpadState *out,int max,const uint32_t *id){return pad_states(out,max,id,8);}
static int joy_right_states(NpadState *out,int max,const uint32_t *id){return pad_states(out,max,id,16);}
static int pad_states(NpadState *out,int max,const uint32_t *id,uint32_t want_style){if(max<=0)return 0;if(id)input_log(want_style==2?"handheld state":want_style==4?"joy-dual state":"full-key state",*id);if(!out||!id)runtime_fail("controller output pointer");pthread_mutex_lock(&inputLock);NpadState state={.sampling=++sample};bool id_match=(!id||*id==pad_id()||*id==0||*id==0x20);if(enabled&&allowed&&id_match&&((want_style&pad_style())!=0)){
 state.attributes=1;for(unsigned i=0;i<128;i++)if(keys[i])state.buttons|=button(i);state.buttons|=heldOut.buttons|pending;pending=0;
 state.lx=32767*((int)keys[2]-(int)keys[0]);state.ly=32767*((int)keys[13]-(int)keys[1]);state.rx=32767*((int)keys[124]-(int)keys[123]);state.ry=32767*((int)keys[126]-(int)keys[125]);
 if(heldOut.lx)state.lx=heldOut.lx;if(heldOut.ly)state.ly=heldOut.ly;if(heldOut.rx)state.rx=heldOut.rx;if(heldOut.ry)state.ry=heldOut.ry;
 RuntimeGamepadState pad;runtime_gamepad_read(&pad);state.buttons|=pad.buttons;
 if(pad.buttons&~padSeen||abs(pad.lx)>16384||abs(pad.ly)>16384||abs(pad.rx)>16384||abs(pad.ry)>16384)user_input("controller");padSeen=pad.buttons;
 if(abs(pad.lx)>abs(state.lx))state.lx=pad.lx;if(abs(pad.ly)>abs(state.ly))state.ly=pad.ly;
 if(abs(pad.rx)>abs(state.rx))state.rx=pad.rx;if(abs(pad.ry)>abs(state.ry))state.ry=pad.ry;
 /* NpadButton stick directions (bits 16-23: L left/up/right/down, then R), which the Switch sets from the analog sticks
    past half deflection; Odyssey's camera tutorial left Mario lying with the analog values alone (2026-10-22). */
 const int t=16384;
 state.buttons|=(uint64_t)(state.lx<-t)<<16|(uint64_t)(state.ly>t)<<17|(uint64_t)(state.lx>t)<<18|(uint64_t)(state.ly<-t)<<19|
                (uint64_t)(state.rx<-t)<<20|(uint64_t)(state.ry>t)<<21|(uint64_t)(state.rx>t)<<22|(uint64_t)(state.ry<-t)<<23;
 }*out=state;pthread_mutex_unlock(&inputLock);
 if(getenv("TOTK_INPUT_TRACE")){
  static uint64_t previous;static int32_t sticks[4];
  int changed=state.lx!=sticks[0]||state.ly!=sticks[1]||state.rx!=sticks[2]||state.ry!=sticks[3];
  if(state.buttons!=previous||changed)fprintf(stderr,"[totk-input] guest controller %#x style %#x buttons %#llx L %d,%d R %d,%d sample %lld\n",*id,want_style,(unsigned long long)state.buttons,state.lx,state.ly,state.rx,state.ry,(long long)state.sampling);
  previous=state.buttons;sticks[0]=state.lx;sticks[1]=state.ly;sticks[2]=state.rx;sticks[3]=state.ry;
 }
 return 1;}
static int disconnected_states(NpadState *out,int max,const uint32_t *id){if(id)input_log("joy-con state",*id);if(max<=0)return 0;if(!out)runtime_fail("controller output pointer");pthread_mutex_lock(&inputLock);*out=(NpadState){.sampling=++sample};pthread_mutex_unlock(&inputLock);return 1;}
static void hold_type(int type){if(type<0||type>1)runtime_fail("invalid joy hold type");holdType=type;(void)holdType;}
static int get_hold_type(void) { return holdType; }
static void strap_guide(void) {}
static void controller_support_default(void *arg) { if (arg) memset(arg, 0, 0xec); }
static void set_explain_text(void *a, const char *b, const uint32_t *c) { (void)a; (void)b; (void)c; }
static int get_joy_assignment(const uint32_t *id) { (void)id; return 0; }
static uint32_t set_joy_single(const uint32_t *id, int dev) { (void)id; (void)dev; return 0; }
static uint32_t get_supported_styles(void) { return 0x1f; }
static uint32_t swap_npad(const uint32_t *a, const uint32_t *b) { (void)a; (void)b; return 0; }
static void start_lr(void) {}
static void stop_lr(void) {}
static uint32_t get_six_axis_handles(uint32_t *handles, int count, const uint32_t *id, uint32_t style) {
    (void)id; (void)style;
    if (handles && count > 0) {
        for (int i = 0; i < count; i++) handles[i] = 1 + i;
    }
    return 0;
}
static uint32_t get_vibration_device_handles(uint32_t *handles, int count, const uint32_t *id, uint32_t style) {
    (void)id; (void)style;
    if (handles && count > 0) {
        for (int i = 0; i < count; i++) handles[i] = 1 + i;
    }
    return 0;
}
static uint32_t start_six_axis(const void *h) { (void)h; return 0; }
static uint32_t stop_six_axis(const void *h) { (void)h; return 0; }
static void get_six_axis_state(void *out, const void *h) { if (out) memset(out, 0, 0x60); (void)h; }
static bool is_six_axis_at_rest(const void *h) { (void)h; return true; }
static void get_quaternion(const void *self, float *q) { (void)self; if (q) { q[0]=0; q[1]=0; q[2]=0; q[3]=1.0f; } }
static uint32_t init_vibration(const void *h) { (void)h; return 0; }
static uint32_t send_vibration(const void *h, const void *v) { (void)h; (void)v; return 0; }
static uint32_t parse_vibration_file(void *info, void *ctx, const void *data, size_t size) { (void)info; (void)ctx; (void)data; (void)size; return 0; }
static uint32_t retrieve_vibration(void *val, int pos, void *ctx) { if (val) memset(val, 0, 16); (void)pos; (void)ctx; return 0; }
static void disconnect(const uint32_t *id){if(!id)runtime_fail("disconnect id");input_log("disconnect",*id);if(*id==pad_id()){   /* only the pad in use: in handheld mode TOTK disconnects player 1 */pthread_mutex_lock(&inputLock);allowed=false;pthread_mutex_unlock(&inputLock);runtime_input_clear();}}
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
/* Controller support applet (the Switch's "connect controllers" screen). TOTK opens it in handheld mode; there is
   exactly one controller here, so it returns at once: one player, on the controller in use (ControllerSupportResultInfo:
   int8 player count, 3 bytes padding, uint32 selected id). */
typedef struct{int8_t players;uint8_t pad[3];uint32_t selected;} ControllerSupportResult;
_Static_assert(sizeof(ControllerSupportResult)==8,"controller support result ABI");
static uint32_t show_controller_support(ControllerSupportResult *out,const void *arg){
 (void)arg;if(out)*out=(ControllerSupportResult){.players=1,.selected=pad_id()};
 static int once;if(!once++)fprintf(stderr,"[totk] controller support applet: one player on controller %#x\n",pad_id());
 return 0;
}
/* Bayonetta 3 reads one state at a time (GetNpadState), has a touch screen (no touches here: the Mac has none), and sets
   the handheld activation mode. */
static void full_state(NpadState *out,const uint32_t *id){full_states(out,1,id);}
static void handheld_state(NpadState *out,const uint32_t *id){handheld_states(out,1,id);}
__attribute__((unused)) static void disconnected_state(NpadState *out,const uint32_t *id){disconnected_states(out,1,id);}
static void joy_dual_state(NpadState *out,const uint32_t *id){joy_dual_states(out,1,id);}
static void joy_left_state(NpadState *out,const uint32_t *id){joy_left_states(out,1,id);}
static void joy_right_state(NpadState *out,const uint32_t *id){joy_right_states(out,1,id);}
static void mouse_state(MouseState *out){mouse_states(out,1);}
static void handheld_activation(int mode){(void)mode;}
static void initialize_touch(void){}
/* TouchScreenState<N>: {int64 sampling, int32 count, int32 reserved, TouchState[N] of 0x28 bytes}; count 0. */
static void touch_states_1(uint8_t *out){if(!out)runtime_fail("touch output pointer");pthread_mutex_lock(&inputLock);memset(out,0,16+1*0x28);int64_t n=++sample;memcpy(out,&n,8);pthread_mutex_unlock(&inputLock);}
static void touch_states_16(uint8_t *out){if(!out)runtime_fail("touch output pointer");pthread_mutex_lock(&inputLock);memset(out,0,16+16*0x28);int64_t n=++sample;memcpy(out,&n,8);pthread_mutex_unlock(&inputLock);}
static int touch_states_16_n(uint8_t *out,int count){if(!out||count<=0)return 0;touch_states_16(out);return 1;}
static int touch_states_1_n(uint8_t *out,int count){if(!out||count<=0)return 0;touch_states_1(out);return 1;}
static uint32_t show_controller_support_arg(const void *arg){return show_controller_support(NULL,arg);}
/* Odyssey initializes the console's separate seven-axis sensor during boot. A
   Mac has no corresponding console sensor; report no samples while retaining
   normal controller motion through the six-axis path in hid_feedback.c. */
static uint32_t seven_axis_init(void *work,size_t size){
 if(!work||size<0x80000)runtime_fail("seven-axis sensor work buffer");
 fprintf(stderr,"[odyssey] console seven-axis sensor unavailable; controller motion remains separate\n");
 return 0;
}
/* Success, not void: VR Start (22 October) checks StartSevenSixAxisSensor's nn::Result and aborts on nonzero
   (main+0x7b255c); a void handler left whatever was in w0. Callers of the void SDK functions ignore w0. */
static uint32_t seven_axis_noop(void){return 0;}
static bool seven_axis_at_rest(void){return true;}
static int seven_axis_states(void *out,int count){(void)out;(void)count;return 0;}
static void initialize_debug_pad(void){fprintf(stderr,"[odyssey] debug pad unavailable; normal controller remains active\n");}
static void debug_pad_state(void *out){if(!out)runtime_fail("debug pad state output");memset(out,0,0x20);}
static void joy_dual(const uint32_t *id){if(!id)runtime_fail("joy-dual controller id");}
/* Pairing two single Joy-Con as one (the game's controller settings): there is one virtual controller here; nothing to merge
   (2026-10-29: the SDK version goes to the hid service, which this host does not run). */
static uint32_t merge_single_joy(const uint32_t *a,const uint32_t *b){(void)a;(void)b;return 0;}
typedef struct{uint32_t main,sub;} NpadColor;
static uint32_t pad_color(NpadColor *out,const uint32_t *id){if(!out||!id)return 0x2c6ca;*out=(NpadColor){0xff323232u,0xffffffffu};return 0;}
static uint32_t pad_colors(NpadColor *left,NpadColor *right,const uint32_t *id){if(!left||!right||!id)return 0x2c6ca;*left=*right=(NpadColor){0xff323232u,0xffffffffu};return 0;}
/* Vibration devices and the six-axis sensor: runtime/hid_feedback.c (none unless TOTK_RUMBLE=1 / TOTK_GYRO=1). */
void *runtime_input_override(const char *name){
 void *feedback=runtime_hid_feedback_override(name);if(feedback)return feedback;
 static const struct{const char *name;void *fn;} entries[]={
 {"_ZN2nn3hid14InitializeNpadEv",initialize},{"_ZN2nn3hid22SetSupportedNpadIdTypeEPKjm",supported_ids},{"_ZN2nn3hid24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE",supported_styles},
 {"_ZN2nn3hid15GetNpadStyleSetERKj",style},{"_ZN2nn3hid13GetNpadStatesEPNS0_16NpadFullKeyStateEiRKj",full_states},
 {"_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyDualStateEiRKj",joy_dual_states},{"_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyLeftStateEiRKj",joy_left_states},
 {"_ZN2nn3hid13GetNpadStatesEPNS0_17NpadJoyRightStateEiRKj",joy_right_states},{"_ZN2nn3hid13GetNpadStatesEPNS0_17NpadHandheldStateEiRKj",handheld_states},
 {"_ZN2nn3hid18SetNpadJoyHoldTypeENS0_15NpadJoyHoldTypeE",hold_type},{"_ZN2nn3hid14DisconnectNpadERKj",disconnect},
 {"_ZN2nn3hid21ShowControllerSupportEPNS0_27ControllerSupportResultInfoERKNS0_20ControllerSupportArgE",show_controller_support},
 {"_ZN2nn3hid15InitializeMouseEv",initialize_mouse},{"_ZN2nn3hid18InitializeKeyboardEv",initialize_keyboard},
 {"_ZN2nn3hid14GetMouseStatesEPNS0_10MouseStateEi",mouse_states},{"_ZN2nn3hid16GetKeyboardStateEPNS0_13KeyboardStateE",keyboard_state},
 {"_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj",full_state},{"_ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj",handheld_state},
 {"_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyDualStateERKj",joy_dual_state},{"_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyLeftStateERKj",joy_left_state},
 {"_ZN2nn3hid12GetNpadStateEPNS0_17NpadJoyRightStateERKj",joy_right_state},{"_ZN2nn3hid13GetMouseStateEPNS0_10MouseStateE",mouse_state},
 {"_ZN2nn3hid29SetNpadHandheldActivationModeENS0_26NpadHandheldActivationModeE",handheld_activation},{"_ZN2nn3hid21InitializeTouchScreenEv",initialize_touch},
 {"_ZN2nn3hid19GetTouchScreenStateILm1EEEvPNS0_16TouchScreenStateIXT_EEE",touch_states_1},{"_ZN2nn3hid19GetTouchScreenStateILm16EEEvPNS0_16TouchScreenStateIXT_EEE",touch_states_16},
 {"_ZN2nn3hid20GetTouchScreenStatesILm16EEEiPNS0_16TouchScreenStateIXT_EEEi",touch_states_16_n},{"_ZN2nn3hid20GetTouchScreenStatesILm1EEEiPNS0_16TouchScreenStateIXT_EEEi",touch_states_1_n},
 {"_ZN2nn3hid21ShowControllerSupportERKNS0_20ControllerSupportArgE",show_controller_support_arg},
 {"_ZN2nn3hid28InitializeSevenSixAxisSensorEPvm",seven_axis_init},
 {"_ZN2nn3hid26FinalizeSevenSixAxisSensorEv",seven_axis_noop},
 {"_ZN2nn3hid23StartSevenSixAxisSensorEv",seven_axis_noop},
 {"_ZN2nn3hid22StopSevenSixAxisSensorEv",seven_axis_noop},
 {"_ZN2nn3hid32ResetSevenSixAxisSensorTimestampEv",seven_axis_noop},
 {"_ZN2nn3hid26IsSevenSixAxisSensorAtRestEv",seven_axis_at_rest},
 {"_ZN2nn3hid27GetSevenSixAxisSensorStatesEPNS0_23SevenSixAxisSensorStateEi",seven_axis_states},
 {"_ZN2nn3hid18InitializeDebugPadEv",initialize_debug_pad},
 {"_ZN2nn3hid16GetDebugPadStateEPNS0_13DebugPadStateE",debug_pad_state},
 {"_ZN2nn3hid17GetDebugPadStatesEPNS0_13DebugPadStateEi",seven_axis_states},
 {"_ZN2nn3hid28SetNpadJoyAssignmentModeDualERKj",joy_dual},
 {"_ZN2nn3hid23MergeSingleJoyAsDualJoyERKjS2_",merge_single_joy},
 {"_ZN2nn3hid18GetNpadJoyHoldTypeEv",get_hold_type},
 {"_ZN2nn3hid24ShowControllerStrapGuideEv",strap_guide},
 {"_ZN2nn3hid20ControllerSupportArg10SetDefaultEv",controller_support_default},
 {"_ZN2nn3hid14SetExplainTextEPNS0_20ControllerSupportArgEPKcRKj",set_explain_text},
 {"_ZN2nn3hid20GetNpadJoyAssignmentERKj",get_joy_assignment},
 {"_ZN2nn3hid30SetNpadJoyAssignmentModeSingleERKjNS0_17NpadJoyDeviceTypeE",set_joy_single},
 {"_ZN2nn3hid24GetSupportedNpadStyleSetEv",get_supported_styles},
 {"_ZN2nn3hid18SwapNpadAssignmentERKjS2_",swap_npad},
 {"_ZN2nn3hid21StartLrAssignmentModeEv",start_lr},
 {"_ZN2nn3hid20StopLrAssignmentModeEv",stop_lr},
 {"_ZN2nn3hid18StartSixAxisSensorERKNS0_19SixAxisSensorHandleE",start_six_axis},
 {"_ZN2nn3hid17StopSixAxisSensorERKNS0_19SixAxisSensorHandleE",stop_six_axis},
 {"_ZN2nn3hid21GetSixAxisSensorStateEPNS0_18SixAxisSensorStateERKNS0_19SixAxisSensorHandleE",get_six_axis_state},
 {"_ZN2nn3hid21IsSixAxisSensorAtRestERKNS0_19SixAxisSensorHandleE",is_six_axis_at_rest},
 {"_ZNK2nn3hid18SixAxisSensorState13GetQuaternionEPNS_4util4neon12Vector4fTypeE",get_quaternion},
 {"_ZN2nn3hid25InitializeVibrationDeviceERKNS0_21VibrationDeviceHandleE",init_vibration},
 {"_ZN2nn3hid18SendVibrationValueERKNS0_21VibrationDeviceHandleERKNS0_14VibrationValueE",send_vibration},
 {"_ZN2nn3hid18ParseVibrationFileEPNS0_17VibrationFileInfoEPNS0_26VibrationFileParserContextEPKvm",parse_vibration_file},
 {"_ZN2nn3hid22RetrieveVibrationValueEPNS0_14VibrationValueEiPNS0_26VibrationFileParserContextE",retrieve_vibration},
 {"_ZN2nn3hid23GetSixAxisSensorHandlesEPNS0_19SixAxisSensorHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE",get_six_axis_handles},
 {"_ZN2nn3hid25GetVibrationDeviceHandlesEPNS0_21VibrationDeviceHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE",get_vibration_device_handles},
 {"_ZN2nn3hid22GetNpadControllerColorEPNS0_19NpadControllerColorERKj",pad_color},
 {"_ZN2nn3hid22GetNpadControllerColorEPNS0_19NpadControllerColorES2_RKj",pad_colors}};
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
   kbtest / kbtype <text> / kbenter / kbesc / kbshot <path> (the text entry bar), panelshot <page 0-3> <path> (the settings panel), threads (dump every guest thread's wait), hletrace <0|1|2> (host calls the game makes), framestep <ms> (pin the frame step), close. */
extern void dm_gfx_shot_at(long frames, const char *path);
extern void runtime_front_request_close(void);
static void *input_file(void *arg) {
    FILE *f = fopen((const char *)arg, "r"); if (!f) runtime_fail("cannot open TOTK_INPUT_FILE");
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
        } else if (sscanf(line, "panelshot %u %n", &code, &used) == 1 && used && code <= 3 && line[used]) {   /* "panelshot <page 0-3> <path>": the settings panel as a PNG */
            line[strcspn(line, "\r\n")] = 0; extern void runtime_settings_shot(int page, const char *path); runtime_settings_shot((int)code, line + used);
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
