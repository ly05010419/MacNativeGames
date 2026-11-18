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
/* Bayonetta 3 reads one state at a time (GetNpadState), has a touch screen (no touches here: the Mac has none), and sets
   the handheld activation mode. */
static void full_state(NpadState *out,const uint32_t *id){full_states(out,1,id);}
static void handheld_state(NpadState *out,const uint32_t *id){handheld_states(out,1,id);}
static void disconnected_state(NpadState *out,const uint32_t *id){disconnected_states(out,1,id);}
static void mouse_state(MouseState *out){mouse_states(out,1);}
static void handheld_activation(int mode){(void)mode;}
static void initialize_touch(void){}
/* TouchScreenState<N>: {int64 sampling, int32 count, int32 reserved, TouchState[N] of 0x28 bytes}; count 0. */
static void touch_states_1(uint8_t *out){if(!out)runtime_fail("touch output pointer");pthread_mutex_lock(&inputLock);memset(out,0,16+1*0x28);int64_t n=++sample;memcpy(out,&n,8);pthread_mutex_unlock(&inputLock);}
static void touch_states_16(uint8_t *out){if(!out)runtime_fail("touch output pointer");pthread_mutex_lock(&inputLock);memset(out,0,16+16*0x28);int64_t n=++sample;memcpy(out,&n,8);pthread_mutex_unlock(&inputLock);}
static uint32_t show_controller_support_arg(const void *arg){return show_controller_support(NULL,arg);}
/* Echoes of Wisdom waits for style-set changes on an event and reads the hold type back. The one controller is there
   from the start, so the event is signalled once when bound; the game then reads the style set as usual. Joy-Con
   assignment (dual / swap) has no meaning for the Mac's single controller. */
void hle_signal_system_event(void *event);
static void bind_style_event(const uint32_t *id,void *event,int clear_mode){
 if(!id||!event||clear_mode<0||clear_mode>1)runtime_fail("style set event arguments");
 input_log("style set event",*id);hle_bind_system_event(event,clear_mode);hle_signal_system_event(event);
}
static int get_hold_type(void){pthread_mutex_lock(&inputLock);int v=holdType;pthread_mutex_unlock(&inputLock);return v;}
static void assignment_dual(const uint32_t *id){if(!id)runtime_fail("controller id pointer");}
static void swap_assignment(const uint32_t *a,const uint32_t *b){if(!a||!b)runtime_fail("controller id pointer");}
/* Vibration devices and the six-axis sensor: runtime/hid_feedback.c (none unless TOTK_RUMBLE=1 / TOTK_GYRO=1). */
void *runtime_input_override(const char *name){
 void *feedback=runtime_hid_feedback_override(name);if(feedback)return feedback;
 static const struct{const char *name;void *fn;} entries[]={
 {"_ZN2nn3hid14InitializeNpadEv",initialize},{"_ZN2nn3hid22SetSupportedNpadIdTypeEPKjm",supported_ids},{"_ZN2nn3hid24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE",supported_styles},
 {"_ZN2nn3hid15GetNpadStyleSetERKj",style},{"_ZN2nn3hid13GetNpadStatesEPNS0_16NpadFullKeyStateEiRKj",full_states},
 {"_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyDualStateEiRKj",disconnected_states},{"_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyLeftStateEiRKj",disconnected_states},
 {"_ZN2nn3hid13GetNpadStatesEPNS0_17NpadJoyRightStateEiRKj",disconnected_states},{"_ZN2nn3hid13GetNpadStatesEPNS0_17NpadHandheldStateEiRKj",handheld_states},
 {"_ZN2nn3hid18SetNpadJoyHoldTypeENS0_15NpadJoyHoldTypeE",hold_type},{"_ZN2nn3hid18GetNpadJoyHoldTypeEv",get_hold_type},
 {"_ZN2nn3hid27BindNpadStyleSetUpdateEventERKjPNS_2os15SystemEventTypeENS3_14EventClearModeE",bind_style_event},
 {"_ZN2nn3hid28SetNpadJoyAssignmentModeDualERKj",assignment_dual},{"_ZN2nn3hid18SwapNpadAssignmentERKjS2_",swap_assignment},{"_ZN2nn3hid14DisconnectNpadERKj",disconnect},
 {"_ZN2nn3hid21ShowControllerSupportEPNS0_27ControllerSupportResultInfoERKNS0_20ControllerSupportArgE",show_controller_support},
 {"_ZN2nn3hid15InitializeMouseEv",initialize_mouse},{"_ZN2nn3hid18InitializeKeyboardEv",initialize_keyboard},
 {"_ZN2nn3hid14GetMouseStatesEPNS0_10MouseStateEi",mouse_states},{"_ZN2nn3hid16GetKeyboardStateEPNS0_13KeyboardStateE",keyboard_state},
 {"_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj",full_state},{"_ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj",handheld_state},
 {"_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyDualStateERKj",disconnected_state},{"_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyLeftStateERKj",disconnected_state},
 {"_ZN2nn3hid12GetNpadStateEPNS0_17NpadJoyRightStateERKj",disconnected_state},{"_ZN2nn3hid13GetMouseStateEPNS0_10MouseStateE",mouse_state},
 {"_ZN2nn3hid29SetNpadHandheldActivationModeENS0_26NpadHandheldActivationModeE",handheld_activation},{"_ZN2nn3hid21InitializeTouchScreenEv",initialize_touch},
 {"_ZN2nn3hid19GetTouchScreenStateILm1EEEvPNS0_16TouchScreenStateIXT_EEE",touch_states_1},{"_ZN2nn3hid19GetTouchScreenStateILm16EEEvPNS0_16TouchScreenStateIXT_EEE",touch_states_16},
 {"_ZN2nn3hid21ShowControllerSupportERKNS0_20ControllerSupportArgE",show_controller_support_arg}};
 for(unsigned i=0;i<sizeof(entries)/sizeof(*entries);i++)if(!strcmp(name,entries[i].name))return entries[i].fn;return NULL;
}

/* This game's own TOTK_INPUT_FILE commands; the shared runtime/input_file.h (below) runs the rest: gpucapture
   <path>.gputrace (a Metal capture of the next frame), panelshot <page 0-3> <path> (the settings panel). 1: it was one. */
static int input_game_command(char *line) {
    unsigned code; int used = 0;
    if (!strncmp(line, "gpucapture ", 11) && line[11]) {   /* "gpucapture <path>.gputrace": a Metal capture of the next frame (needs MTL_CAPTURE_ENABLED=1) */
        char *e = line + strlen(line); while (e > line + 11 && (e[-1] == '\n' || e[-1] == '\r')) *--e = 0;
        extern void dm_gfx_gpucapture_next(const char *path); dm_gfx_gpucapture_next(line + 11);
    } else if (sscanf(line, "panelshot %u %n", &code, &used) == 1 && used && code <= 3 && line[used]) {   /* "panelshot <page 0-3> <path>": the settings panel as a PNG */
        line[strcspn(line, "\r\n")] = 0; extern void runtime_settings_shot(int page, const char *path); runtime_settings_shot((int)code, line + used);
    } else return 0;
    return 1;
}
/* "flag <name> <v>": none of this game's own (rumble and gyro are hid_feedback.c's, the rest the graphics layer's). */
static int input_game_flag(const char *name, int v) { (void)name; (void)v; return 0; }
#include "input_file.h"   /* TOTK_INPUT_SCRIPT and TOTK_INPUT_FILE (shared/runtime) */
