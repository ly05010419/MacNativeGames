#include <math.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
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
   keys the sticks (pad_states). It is also the player's default layout (keyboard_map.h keymap_default). */
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
   not toggle it. Keys 2-4 (MetalFX frame interpolation, the 2K picture modes, ultrawide) are no shortcuts any more: none
   of them did anything in this game (game_patches.c answered "unavailable"), and the panel replaces the reminder box. */
enum { KEY_SETTINGS = KEYMAP_SETTINGS_KEY };
/* The screenshot action (keyboard_map.h KEYMAP_SCREENSHOT, P by default; as games/totk, user 2026-11-11): a press of one of
   its inputs takes the capture button's picture (gamepad.m runtime_capture_press -> screenshot.h). Its inputs drive nothing
   in the game (keymap_apply). */
static int screenshot_input_locked(int input){return keymap_action_of(&keyMap,input,NULL)==KEYMAP_SCREENSHOT;}
static void screenshot_input(int input){
 const char *name=keymap_input_name(input);fprintf(stderr,"[totk-input] screenshot: %s\n",name?name:"?");
 if(runtime_capture_press)runtime_capture_press();
}
void runtime_input_key_synthetic(unsigned code,int down){if(code>=128)return;pthread_mutex_lock(&inputLock);int press=down&&!keys[code];if(down&&!keys[code])pending|=button(code);keys[code]=(unsigned char)!!down;pthread_mutex_unlock(&inputLock);
 if(press&&code==KEY_SETTINGS)runtime_settings_toggle();}
void runtime_input_key(unsigned code,int down){
 if(code>=128)return;if(down)user_input("keyboard");
 pthread_mutex_lock(&inputLock);int press=down&&!held[code];held[code]=(unsigned char)!!down;held_changed_locked();int shot=press&&screenshot_input_locked((int)code);pthread_mutex_unlock(&inputLock);
 if(press&&code==KEY_SETTINGS)runtime_settings_toggle();
 if(shot)screenshot_input((int)code);
}
/* A mouse button over the game window (frontend.m: 0 left, 1 right, 2 middle, 3 and 4 the side buttons). */
void runtime_input_mouse(int button,int down){
 if(button<0||button>=KEYMAP_MOUSE_BUTTONS)return;if(down)user_input("mouse");
 pthread_mutex_lock(&inputLock);int press=down&&!held[KEYMAP_MOUSE_FIRST+button];held[KEYMAP_MOUSE_FIRST+button]=(unsigned char)!!down;held_changed_locked();
 int shot=press&&screenshot_input_locked(KEYMAP_MOUSE_FIRST+button);pthread_mutex_unlock(&inputLock);
 if(shot)screenshot_input(KEYMAP_MOUSE_FIRST+button);
}
extern void runtime_front_control_test(const char *) __attribute__((weak_import));
/* Cheats (cheats.c): the buttons held now (keyboard, mouse and controller), in the npad bit order. */
uint64_t runtime_input_buttons(void){uint64_t b=0;pthread_mutex_lock(&inputLock);for(unsigned i=0;i<128;i++)if(keys[i])b|=button(i);b|=heldOut.buttons;pthread_mutex_unlock(&inputLock);
 return b|runtime_gamepad_held();}
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
static uint32_t pad_style(void){return runtime_handheld_mode()?2u:1u;}
static void supported_ids(const uint32_t *ids,uint64_t n){if(n>10||(!ids&&n))runtime_fail("invalid controller ids");
 if(getenv("TOTK_INPUT_LOG")){char b[128];size_t o=0;for(uint64_t i=0;i<n&&o<100;i++)o+=(size_t)snprintf(b+o,sizeof b-o," %#x",ids[i]);b[o]=0;fprintf(stderr,"[totk-input] game supports controller ids:%s\n",b);}pthread_mutex_lock(&inputLock);allowed=false;for(uint64_t i=0;i<n;i++)if(ids[i]==pad_id())allowed=true;pthread_mutex_unlock(&inputLock);}
static void supported_styles(uint32_t value){pthread_mutex_lock(&inputLock);styles=value;pthread_mutex_unlock(&inputLock);}
static uint32_t style(const uint32_t *id){if(!id)runtime_fail("controller id pointer");input_log("style set",*id);pthread_mutex_lock(&inputLock);uint32_t v=enabled&&allowed&&*id==pad_id()?(styles&pad_style()):0;pthread_mutex_unlock(&inputLock);return v;}
static int pad_states(NpadState *out,int max,const uint32_t *id,uint32_t want_style);
static int full_states(NpadState *out,int max,const uint32_t *id){return pad_states(out,max,id,1);}
static int handheld_states(NpadState *out,int max,const uint32_t *id){return pad_states(out,max,id,2);}
static int pad_states(NpadState *out,int max,const uint32_t *id,uint32_t want_style){if(max<=0)return 0;if(id)input_log(want_style==2?"handheld state":"full-key state",*id);if(!out||!id)runtime_fail("controller output pointer");pthread_mutex_lock(&inputLock);NpadState state={.sampling=++sample};if(enabled&&allowed&&*id==pad_id()&&want_style==pad_style()&&(styles&want_style)){
 state.attributes=1;for(unsigned i=0;i<128;i++)if(keys[i])state.buttons|=button(i);state.buttons|=heldOut.buttons|pending;pending=0;
 state.lx=32767*((int)keys[2]-(int)keys[0]);state.ly=32767*((int)keys[13]-(int)keys[1]);state.rx=32767*((int)keys[124]-(int)keys[123]);state.ry=32767*((int)keys[126]-(int)keys[125]);
 if(heldOut.lx)state.lx=heldOut.lx;if(heldOut.ly)state.ly=heldOut.ly;if(heldOut.rx)state.rx=heldOut.rx;if(heldOut.ry)state.ry=heldOut.ry;
 RuntimeGamepadState pad;runtime_gamepad_read(&pad);state.buttons|=pad.buttons;
 if(pad.buttons&~padSeen||abs(pad.lx)>16384||abs(pad.ly)>16384||abs(pad.rx)>16384||abs(pad.ry)>16384)user_input("controller");padSeen=pad.buttons;
 if(abs(pad.lx)>abs(state.lx))state.lx=pad.lx;if(abs(pad.ly)>abs(state.ly))state.ly=pad.ly;
 if(abs(pad.rx)>abs(state.rx))state.rx=pad.rx;if(abs(pad.ry)>abs(state.ry))state.ry=pad.ry;
 }*out=state;pthread_mutex_unlock(&inputLock);return 1;}
static int disconnected_states(NpadState *out,int max,const uint32_t *id){if(id)input_log("joy-con state",*id);if(max<=0)return 0;if(!out)runtime_fail("controller output pointer");pthread_mutex_lock(&inputLock);*out=(NpadState){.sampling=++sample};pthread_mutex_unlock(&inputLock);return 1;}
static void hold_type(int type){if(type<0||type>1)runtime_fail("invalid joy hold type");holdType=type;(void)holdType;}
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
/* SDK 7.3.2 (Luigi's Mansion 3): single-sample npad reads, the touch screen, controller colours and the public
   vibration / six-axis entry points (TOTK's SDK only calls their nn::hid::detail forms). */
static void full_state(NpadState *out,const uint32_t *id){pad_states(out,1,id,1);}
static void handheld_state(NpadState *out,const uint32_t *id){pad_states(out,1,id,2);}
static void joycon_state(NpadState *out,const uint32_t *id){disconnected_states(out,1,id);}
static uint32_t supported_style_set(void){pthread_mutex_lock(&inputLock);uint32_t v=styles;pthread_mutex_unlock(&inputLock);return v;}
static void assignment_single(const uint32_t *id){(void)id;}
/* No touch screen on a Mac window: always zero touches (TouchScreenState<1>: sampling, count, reserved, one 40-byte touch). */
typedef struct{int64_t sampling;int32_t count,reserved;uint8_t touch[40];} TouchScreenState1;
_Static_assert(sizeof(TouchScreenState1)==56,"touch screen ABI");
static void touch_initialize(void){}
static void touch_state(TouchScreenState1 *out){if(!out)runtime_fail("touch output pointer");pthread_mutex_lock(&inputLock);*out=(TouchScreenState1){.sampling=++sample};pthread_mutex_unlock(&inputLock);}
typedef struct{uint32_t main,sub;} NpadColor;
static uint32_t pad_color(NpadColor *out,const uint32_t *id){if(!out||!id)return 0x2c6ca;*out=(NpadColor){0xff323232u,0xffffffffu};return 0;}
static uint32_t pad_colors(NpadColor *left,NpadColor *right,const uint32_t *id){if(!left||!right||!id)return 0x2c6ca;*left=*right=(NpadColor){0xff323232u,0xffffffffu};return 0;}
static uint32_t controller_support_plain(const void *arg){return show_controller_support(NULL,arg);}
/* With TOTK_RUMBLE / TOTK_GYRO off the game gets no handles; should it still call these, they do nothing. */
static void feedback_none(void){}
static void six_state_none(void *out,const void *handle){(void)handle;if(out)memset(out,0,0x60);}
static void vib_info_none(uint32_t *out,const void *handle){(void)handle;if(out)out[0]=out[1]=0;}
static unsigned char six_at_rest(const void *handle){(void)handle;return 1;}
static void *public_feedback(const char *name){
 static const struct{const char *name,*detail;void *off;} map[]={
  {"_ZN2nn3hid25InitializeVibrationDeviceERKNS0_21VibrationDeviceHandleE","_ZN2nn3hid6detail25InitializeVibrationDeviceERKNS0_21VibrationDeviceHandleE",feedback_none},
  {"_ZN2nn3hid18SendVibrationValueERKNS0_21VibrationDeviceHandleERKNS0_14VibrationValueE","_ZN2nn3hid6detail18SendVibrationValueERKNS0_21VibrationDeviceHandleERKNS0_14VibrationValueE",feedback_none},
  {"_ZN2nn3hid22GetVibrationDeviceInfoEPNS0_19VibrationDeviceInfoERKNS0_21VibrationDeviceHandleE","_ZN2nn3hid6detail22GetVibrationDeviceInfoEPNS0_19VibrationDeviceInfoERKNS0_21VibrationDeviceHandleE",vib_info_none},
  {"_ZN2nn3hid18StartSixAxisSensorERKNS0_19SixAxisSensorHandleE","_ZN2nn3hid6detail18StartSixAxisSensorERKNS0_19SixAxisSensorHandleE",feedback_none},
  {"_ZN2nn3hid17StopSixAxisSensorERKNS0_19SixAxisSensorHandleE","_ZN2nn3hid6detail17StopSixAxisSensorERKNS0_19SixAxisSensorHandleE",feedback_none},
  {"_ZN2nn3hid21GetSixAxisSensorStateEPNS0_18SixAxisSensorStateERKNS0_19SixAxisSensorHandleE","_ZN2nn3hid6detail21GetSixAxisSensorStateEPNS0_18SixAxisSensorStateERKNS0_19SixAxisSensorHandleE",six_state_none}};
 for(unsigned i=0;i<sizeof(map)/sizeof(*map);i++)if(!strcmp(name,map[i].name)){void *on=runtime_hid_feedback_override(map[i].detail);return on?on:map[i].off;}
 return NULL;
}
/* Vibration devices and the six-axis sensor: runtime/hid_feedback.c (none unless TOTK_RUMBLE=1 / TOTK_GYRO=1). */
void *runtime_input_override(const char *name){
 void *feedback=runtime_hid_feedback_override(name);if(feedback)return feedback;
 void *lm3=public_feedback(name);if(lm3)return lm3;
 static const struct{const char *name;void *fn;} entries[]={
 {"_ZN2nn3hid14InitializeNpadEv",initialize},{"_ZN2nn3hid22SetSupportedNpadIdTypeEPKjm",supported_ids},{"_ZN2nn3hid24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE",supported_styles},
 {"_ZN2nn3hid15GetNpadStyleSetERKj",style},{"_ZN2nn3hid13GetNpadStatesEPNS0_16NpadFullKeyStateEiRKj",full_states},
 {"_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyDualStateEiRKj",disconnected_states},{"_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyLeftStateEiRKj",disconnected_states},
 {"_ZN2nn3hid13GetNpadStatesEPNS0_17NpadJoyRightStateEiRKj",disconnected_states},{"_ZN2nn3hid13GetNpadStatesEPNS0_17NpadHandheldStateEiRKj",handheld_states},
 {"_ZN2nn3hid18SetNpadJoyHoldTypeENS0_15NpadJoyHoldTypeE",hold_type},{"_ZN2nn3hid14DisconnectNpadERKj",disconnect},
 {"_ZN2nn3hid21ShowControllerSupportEPNS0_27ControllerSupportResultInfoERKNS0_20ControllerSupportArgE",show_controller_support},
 {"_ZN2nn3hid15InitializeMouseEv",initialize_mouse},{"_ZN2nn3hid18InitializeKeyboardEv",initialize_keyboard},
 {"_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj",full_state},{"_ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj",handheld_state},
 {"_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyDualStateERKj",joycon_state},{"_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyLeftStateERKj",joycon_state},
 {"_ZN2nn3hid12GetNpadStateEPNS0_17NpadJoyRightStateERKj",joycon_state},{"_ZN2nn3hid24GetSupportedNpadStyleSetEv",supported_style_set},
 {"_ZN2nn3hid30SetNpadJoyAssignmentModeSingleERKj",assignment_single},{"_ZN2nn3hid21InitializeTouchScreenEv",touch_initialize},
 {"_ZN2nn3hid19GetTouchScreenStateILm1EEEvPNS0_16TouchScreenStateIXT_EEE",touch_state},
 {"_ZN2nn3hid22GetNpadControllerColorEPNS0_19NpadControllerColorERKj",pad_color},
 {"_ZN2nn3hid22GetNpadControllerColorEPNS0_19NpadControllerColorES2_RKj",pad_colors},
 {"_ZN2nn3hid21ShowControllerSupportERKNS0_20ControllerSupportArgE",controller_support_plain},
 {"_ZN2nn3hid21IsSixAxisSensorAtRestERKNS0_19SixAxisSensorHandleE",six_at_rest},
 {"_ZN2nn3hid14GetMouseStatesEPNS0_10MouseStateEi",mouse_states},{"_ZN2nn3hid16GetKeyboardStateEPNS0_13KeyboardStateE",keyboard_state}};
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
   Commands: key <Mac keycode> <milliseconds>, down/up <Mac keycode>, shot <path>, capture, wshot <path>, passdump <frames>, gpucapture <path>.gputrace, querylog <frames>, anomdump,
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
        } else if (!strncmp(line, "capture", 7) && (!line[7] || line[7] == '\n' || line[7] == '\r')) {   /* "capture": the capture button once (screenshot.h; screenshot sharing tests) */
            if (runtime_capture_press) runtime_capture_press();
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
        } else if (!strncmp(line, "fscan ", 6)) {   /* "fscan <float> [<new>]": the heap's floats equal to it (heap offsets logged), set to <new> if given */
            float want = 0, to = 0; int nv = sscanf(line + 6, "%f %f", &want, &to); if (nv < 1) runtime_fail("fscan <float> [<new>]");
            uint64_t base[3], size[3]; runtime_guest_regions(base, size); unsigned hits = 0;
            for (uint64_t o = 0; o + 4 <= size[0]; o += 4) {
                if (!(o & 0x3fff)) {   /* only pages that are mapped and readable */
                    mach_vm_address_t a = base[0] + o; mach_vm_size_t rs = 0; vm_region_basic_info_data_64_t ri; mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t obj;
                    if (mach_vm_region(mach_task_self(), &a, &rs, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&ri, &cnt, &obj) != KERN_SUCCESS || a >= base[0] + size[0]) break;
                    if (a > base[0] + o) { o = a - base[0] - 4; continue; }
                    if (!(ri.protection & VM_PROT_READ)) { o = a + rs - base[0] - 4; continue; }
                }
                float v; memcpy(&v, (void *)(uintptr_t)(base[0] + o), 4);
                if (want < 0) {   /* "fscan -1": projection matrices: m00, four zeros, m11 = 16:9 x m00 */
                    if (!(v > 0.2f && v < 5) || (o & 0x3fff) > 0x3fff - 24) continue;
                    float m[6]; memcpy(m, (void *)(uintptr_t)(base[0] + o), 24);
                    if (m[1] || m[2] || m[3] || m[4] || fabsf(m[5] / m[0] - 1.7777778f) > 0.003f) continue;
                    if (hits < 400) fprintf(stderr, "[fscan] proj +%#llx m00 %g m11 %g\n", (unsigned long long)o, m[0], m[5]);
                    hits++; continue;
                }
                if (fabsf(v - want) < 0.0005f) { if (hits < 400) fprintf(stderr, "[fscan] +%#llx\n", (unsigned long long)o); if (nv == 2) memcpy((void *)(uintptr_t)(base[0] + o), &to, 4); hits++; } }
            fprintf(stderr, "[fscan] %u hit(s) of %g%s\n", hits, want, nv == 2 ? " (set)" : "");
        } else if (!strncmp(line, "mscan ", 6)) {   /* "mscan <hex start> <hex end> <float> [<new>]": the same in main's image */
            unsigned long long a, b; float want, to = 0; int nv = sscanf(line + 6, "%llx %llx %f %f", &a, &b, &want, &to);
            if (nv < 3) runtime_fail("mscan <hex start> <hex end> <float> [<new>]");
            uint8_t *mb = runtime_module_base("main"); unsigned hits = 0;
            for (unsigned long long o = a & ~3ull; o + 4 <= b; o += 4) { float v; memcpy(&v, mb + o, 4);
                if (fabsf(v - want) < 0.002f) { if (hits < 200) fprintf(stderr, "[mscan] main+%#llx %g\n", o, v); if (nv == 4) memcpy(mb + o, &to, 4); hits++; } }
            fprintf(stderr, "[mscan] %u hit(s)\n", hits);
        } else if (!strncmp(line, "fdump ", 6)) {   /* "fdump <hex heap offset> <words>": words as hex and float */
            unsigned long long o; unsigned n; if (sscanf(line + 6, "%llx %u", &o, &n) != 2 || n > 1024) runtime_fail("fdump <hex heap offset> <words>");
            uint64_t base[3], size[3]; runtime_guest_regions(base, size); if (o + 4ull * n > size[0]) runtime_fail("fdump: outside the heap");
            for (unsigned i = 0; i < n; i++) { uint32_t w; float f; memcpy(&w, (void *)(uintptr_t)(base[0] + o + 4 * i), 4); memcpy(&f, &w, 4);
                fprintf(stderr, "[fdump] +%#llx %08x %g\n", o + 4 * i, w, f); }
        } else if (!strncmp(line, "fset ", 5)) {   /* "fset <heap offset> <float>" */
            unsigned long long o; float to; if (sscanf(line + 5, "%llx %f", &o, &to) != 2) runtime_fail("fset <hex heap offset> <float>");
            uint64_t base[3], size[3]; runtime_guest_regions(base, size); if (o + 4 > size[0]) runtime_fail("fset: outside the heap");
            memcpy((void *)(uintptr_t)(base[0] + o), &to, 4); fprintf(stderr, "[fscan] set +%#llx = %g\n", o, to);
        } else if (!strcmp(line, "threads\n")) {   /* every guest thread's wait state and guest return chain, now (a hang: what is each thread waiting for) */
            runtime_dump_threads();
        } else if (!strncmp(line,"control ",8)) { if(runtime_front_control_test)runtime_front_control_test(line+8);
        } else if (!strcmp(line, "close\n")) { runtime_front_request_close(); break; }
        else runtime_fail("invalid diagnostic input command");
    }
    fclose(f); return NULL;
}
__attribute__((constructor)) static void input_file_start(void) {
    const char *e = getenv("TOTK_INPUT_FILE"); if (!e || !*e) return;
    pthread_t t; if (pthread_create(&t, NULL, input_file, (void *)e)) runtime_fail("diagnostic input thread"); pthread_detach(t);
}
