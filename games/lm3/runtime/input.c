/* Native keyboard and macOS gamepad backed full-key controller. State ABI follows libnx HID. */
#include "runtime.h"
#include "autoload.h"
#include "world_survey.h"
#include "lm3_controller_support.h"
#include <TargetConditionals.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
typedef struct{int64_t sampling;uint64_t buttons;int32_t lx,ly,rx,ry;uint32_t attributes,pad;} NpadState;
_Static_assert(sizeof(NpadState)==40,"Npad full-key ABI");
static pthread_mutex_t inputLock=PTHREAD_MUTEX_INITIALIZER;
static unsigned char keys[128];static uint64_t pending;
static _Atomic unsigned suspendReasons,suspendClears;
static uint32_t styles=1,supportedIds=0x103,disconnectedIds;static bool enabled;static int holdType;
static uint64_t disconnectGeneration[RUNTIME_GAMEPAD_PLAYERS];
static int64_t sample,padSamples[RUNTIME_GAMEPAD_PLAYERS];static uint64_t padSeen[RUNTIME_GAMEPAD_PLAYERS];
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
/* Exact modifier matching and one action per press, shared by live and diagnostic input.
   Consumed shortcut keys never enter the emulated controller. */
enum { KEY_INTERP_TOGGLE=19, KEY_OUTPUT_MODE_CYCLE=20, KEY_ASPECT_TOGGLE=21 };
extern int runtime_controls_handle_key(unsigned,unsigned) __attribute__((weak_import));
extern int runtime_controls_active(void) __attribute__((weak_import));
extern void runtime_front_control_test(const char *) __attribute__((weak_import));
static unsigned char pressedKeys[128],shortcutHeld[128],blockedKeys[128];
static void clear_keyboard_locked(void){
 for(unsigned i=0;i<128;i++)blockedKeys[i]|=pressedKeys[i];
 memset(keys,0,sizeof keys);memset(pressedKeys,0,sizeof pressedKeys);memset(shortcutHeld,0,sizeof shortcutHeld);pending=0;
}
int runtime_input_suspended(void){return atomic_load(&suspendReasons)!=0||atomic_load(&suspendClears)!=0;}
void runtime_input_suspend(unsigned reason,int on){
 const unsigned valid=RUNTIME_INPUT_SUSPEND_SETTINGS|RUNTIME_INPUT_SUSPEND_PAIRING|RUNTIME_INPUT_SUSPEND_CHEATS;
 if(!reason||(reason&~valid))runtime_fail("invalid input suspension reason");
 pthread_mutex_lock(&inputLock);unsigned old=atomic_load(&suspendReasons),next=on?old|reason:old&~reason;
 if(next!=old){atomic_fetch_add(&suspendClears,1);atomic_store(&suspendReasons,next);}
 pthread_mutex_unlock(&inputLock);
 /* Keep the SDK read gate closed until all concurrent transition clears have
    finished. Never acquire the gamepad lock while holding inputLock here. */
 if(next!=old){runtime_input_clear();atomic_fetch_sub(&suspendClears,1);}
}
static void input_key_modifiers(unsigned code,int down,unsigned modifiers){
 if(code>=128)return;
 pthread_mutex_lock(&inputLock);
 int press=down&&!pressedKeys[code],held=shortcutHeld[code],blocked=blockedKeys[code];pressedKeys[code]=(unsigned char)!!down;
 if(!down){shortcutHeld[code]=0;blockedKeys[code]=0;}
 pthread_mutex_unlock(&inputLock);
 if(held||blocked)return;
 if(press&&runtime_controls_handle_key&&runtime_controls_handle_key(code,modifiers)){
  pthread_mutex_lock(&inputLock);shortcutHeld[code]=1;pthread_mutex_unlock(&inputLock);return;
 }
 pthread_mutex_lock(&inputLock);
 if(runtime_input_suspended()){if(down)blockedKeys[code]=1;keys[code]=0;pthread_mutex_unlock(&inputLock);return;}
 if(down&&!keys[code])pending|=button(code);keys[code]=(unsigned char)!!down;pthread_mutex_unlock(&inputLock);
 if(!runtime_controls_active||!runtime_controls_active()){
  if(press&&code==18){extern void runtime_shortcut_help_toggle(void);runtime_shortcut_help_toggle();}
  if(press&&code==KEY_INTERP_TOGGLE){extern void dm_gfx_set_flag(const char *,int);dm_gfx_set_flag("mfx_interp",-1);}
  if(press&&code==KEY_OUTPUT_MODE_CYCLE)runtime_output_mode_cycle();
  if(press&&code==KEY_ASPECT_TOGGLE)runtime_aspect_toggle();
 }
}
void runtime_input_key_synthetic(unsigned code,int down){input_key_modifiers(code,down,0);}
void runtime_input_key_modifiers(unsigned code,int down,unsigned modifiers){if(down&&code<128)user_input("keyboard");input_key_modifiers(code,down,modifiers);}
void runtime_input_key(unsigned code,int down){runtime_input_key_modifiers(code,down,0);}
void runtime_input_clear(void){pthread_mutex_lock(&inputLock);clear_keyboard_locked();memset(padSeen,0,sizeof padSeen);pthread_mutex_unlock(&inputLock);runtime_gamepad_clear();}
/* Cheats (cheats.c): the buttons held now (keyboard and controller), in the npad bit order. */
uint64_t runtime_input_buttons(void){uint64_t b=0;pthread_mutex_lock(&inputLock);for(unsigned i=0;i<128;i++)if(keys[i])b|=button(i);pthread_mutex_unlock(&inputLock);
 return runtime_input_suspended()?0:b|runtime_gamepad_held();}
static void initialize(void){pthread_mutex_lock(&inputLock);enabled=true;pthread_mutex_unlock(&inputLock);fprintf(stderr,"[totk] native keyboard controller initialized\n");}
/* Docked: keyboard/first physical pad are player 1, the second physical pad is
   player 2. Handheld keeps the original single controller at ID 0x20. */
/* TOTK_INPUT_LOG=1: one line per new (state kind, controller id, style set) the game reads (which pad the game uses). */
static void input_log(const char *what,uint32_t id){static int on=-1;static uint64_t seen[64];static unsigned n;if(on<0)on=getenv("TOTK_INPUT_LOG")!=NULL;if(!on)return;
 uint64_t k=(uint64_t)(uintptr_t)what<<32^id^(uint64_t)styles<<48;for(unsigned i=0;i<n;i++)if(seen[i]==k)return;if(n<64)seen[n++]=k;
 fprintf(stderr,"[totk-input] game reads %s for controller %#x (supported styles %#x, ids allowed %#x)\n",what,id,styles,supportedIds&~disconnectedIds);}
static uint32_t pad_id(void){return runtime_handheld_mode()?0x20u:0u;}
static uint32_t pad_style(void){return runtime_handheld_mode()?2u:1u;}
static uint32_t id_bit(uint32_t id){return id<8?1u<<id:id==0x20?1u<<8:id==0x10?1u<<9:0;}
static int pad_player(uint32_t id){if(id==pad_id())return 0;return !runtime_handheld_mode()&&id==1?1:-1;}
/* inputLock held. This query must not consume either player's press edges. */
static int pad_connected(uint32_t id,int player){return player>=0&&enabled&&
 ((supportedIds&~disconnectedIds)&id_bit(id))&&(styles&pad_style())&&
 (player==0||(runtime_gamepad_connected_mask()&(1u<<(unsigned)player)));}
static void supported_ids(const uint32_t *ids,uint64_t n){if(n>10||(!ids&&n))runtime_fail("invalid controller ids");
 if(getenv("TOTK_INPUT_LOG")){char b[128];size_t o=0;for(uint64_t i=0;i<n&&o<100;i++)o+=(size_t)snprintf(b+o,sizeof b-o," %#x",ids[i]);b[o]=0;fprintf(stderr,"[totk-input] game supports controller ids:%s\n",b);}uint32_t mask=0;for(uint64_t i=0;i<n;i++)mask|=id_bit(ids[i]);pthread_mutex_lock(&inputLock);supportedIds=mask;disconnectedIds=0;pthread_mutex_unlock(&inputLock);}
static void supported_styles(uint32_t value){pthread_mutex_lock(&inputLock);styles=value;pthread_mutex_unlock(&inputLock);}
static uint32_t style(const uint32_t *id){if(!id)runtime_fail("controller id pointer");input_log("style set",*id);pthread_mutex_lock(&inputLock);uint32_t v=pad_connected(*id,pad_player(*id))?pad_style():0;pthread_mutex_unlock(&inputLock);return v;}
/* SDK-visible states, bounded to four changed snapshots per second/player.
   Only an explicitly opted-in isolated validation process may emit this trace. inputLock held. */
static void pad_diagnostic(uint32_t id,int player,const NpadState *state){
 static int on=-1;static uint64_t at[RUNTIME_GAMEPAD_PLAYERS];static NpadState last[RUNTIME_GAMEPAD_PLAYERS];
 if(on<0){const char *diag=getenv("LM3_INPUT_DIAG"),*owner=getenv("TOTK_SLOT_OWNER");on=diag&&!strcmp(diag,"1")&&owner&&!strcmp(owner,"codex");}
 if(!on||player<0)return;
 if(at[player]&&!memcmp((const uint8_t *)state+8,(const uint8_t *)&last[player]+8,sizeof *state-8))return;
 uint64_t now=runtime_now_ns();if(at[player]&&now-at[player]<250000000ull)return;
 last[player]=*state;at[player]=now;
 fprintf(stderr,"[lm3-pad] player=%d id=%#x connected=%u buttons=%#llx left=%d,%d right=%d,%d physical=%#x suspended=%d\n",
  player+1,id,state->attributes&1,(unsigned long long)state->buttons,state->lx,state->ly,state->rx,state->ry,runtime_gamepad_connected_mask(),runtime_input_suspended());
}
static int pad_states(NpadState *out,int max,const uint32_t *id,uint32_t want_style);
static int full_states(NpadState *out,int max,const uint32_t *id){return pad_states(out,max,id,1);}
static int handheld_states(NpadState *out,int max,const uint32_t *id){return pad_states(out,max,id,2);}
static int pad_states(NpadState *out,int max,const uint32_t *id,uint32_t want_style){if(max<=0)return 0;if(id)input_log(want_style==2?"handheld state":"full-key state",*id);if(!out||!id)runtime_fail("controller output pointer");pthread_mutex_lock(&inputLock);int player=pad_player(*id);NpadState state={.sampling=player>=0?++padSamples[player]:++sample};if(pad_connected(*id,player)&&want_style==pad_style()){
 state.attributes=1;if(!runtime_input_suspended()){
 if(player==0){for(unsigned i=0;i<128;i++)if(keys[i])state.buttons|=button(i);state.buttons|=pending;pending=0;
 state.lx=32767*((int)keys[2]-(int)keys[0]);state.ly=32767*((int)keys[13]-(int)keys[1]);state.rx=32767*((int)keys[124]-(int)keys[123]);state.ry=32767*((int)keys[126]-(int)keys[125]);}
 RuntimeGamepadState pad;runtime_gamepad_read_player((unsigned)player,&pad);state.buttons|=pad.buttons;
 if(pad.buttons&~padSeen[player]||abs(pad.lx)>16384||abs(pad.ly)>16384||abs(pad.rx)>16384||abs(pad.ry)>16384)user_input(player==0?"controller P1":"controller P2");padSeen[player]=pad.buttons;
 if(abs(pad.lx)>abs(state.lx))state.lx=pad.lx;if(abs(pad.ly)>abs(state.ly))state.ly=pad.ly;
 if(abs(pad.rx)>abs(state.rx))state.rx=pad.rx;if(abs(pad.ry)>abs(state.ry))state.ry=pad.ry;
 }}*out=state;pad_diagnostic(*id,player,&state);pthread_mutex_unlock(&inputLock);return 1;}
static int disconnected_states(NpadState *out,int max,const uint32_t *id){if(id)input_log("joy-con state",*id);if(max<=0)return 0;if(!out)runtime_fail("controller output pointer");pthread_mutex_lock(&inputLock);*out=(NpadState){.sampling=++sample};pthread_mutex_unlock(&inputLock);return 1;}
static void hold_type(int type){if(type<0||type>1)runtime_fail("invalid joy hold type");holdType=type;(void)holdType;}
static void disconnect(const uint32_t *id){if(!id)runtime_fail("disconnect id");input_log("disconnect",*id);int player=pad_player(*id);if(player>=0){pthread_mutex_lock(&inputLock);disconnectedIds|=id_bit(*id);disconnectGeneration[player]++;padSeen[player]=0;if(player==0)clear_keyboard_locked();pthread_mutex_unlock(&inputLock);runtime_gamepad_clear_player((unsigned)player);}}
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
_Static_assert(sizeof(RuntimeControllerSupportResult)==8,"controller support result ABI");
static uint32_t show_controller_support(RuntimeControllerSupportResult *out,const void *arg){
 RuntimeControllerSupportResult result={0};Lm3ControllerRequest request;
 if(out)*out=result;
 if(!lm3_controller_request(arg,&request))return LM3_CONTROLLER_SUPPORT_CANCELED;
 uint64_t beforeDisconnect[RUNTIME_GAMEPAD_PLAYERS];
 pthread_mutex_lock(&inputLock);unsigned allowed=enabled&&(styles&pad_style())?supportedIds:0;
 memcpy(beforeDisconnect,disconnectGeneration,sizeof beforeDisconnect);pthread_mutex_unlock(&inputLock);
 if(runtime_handheld_mode()){
  if(!(allowed&id_bit(0x20))||request.minimum>1)return LM3_CONTROLLER_SUPPORT_CANCELED;
  result=(RuntimeControllerSupportResult){.players=1,.selected=0x20};
 }else{
  allowed&=3;
#if defined(GAME_LM3) && TARGET_OS_OSX
  uint32_t rc=runtime_controller_support_run(request.minimum,request.maximum,allowed,request.single_mode,&result);
  if(rc)return rc;
#else
  if(!(allowed&1)||request.minimum>1)return LM3_CONTROLLER_SUPPORT_CANCELED;
  result=(RuntimeControllerSupportResult){.players=1,.selected=0};
#endif
 }
 pthread_mutex_lock(&inputLock);
 /* The pairing UI may wait while another guest thread changes its HID
    contract. Commit only against the current styles/IDs and physical slots.
    A pre-existing DisconnectNpad is resolved by pairing; a new disconnect
    during the wait cancels that selection instead of resurrecting it. */
 unsigned current=enabled&&(styles&pad_style())?supportedIds:0;
 int valid;
 if(runtime_handheld_mode())valid=(current&id_bit(0x20))&&result.players==1&&result.selected==0x20;
 else{
#if defined(GAME_LM3) && TARGET_OS_OSX
  valid=lm3_controller_result_valid(&request,current&3,runtime_gamepad_connected_mask(),&result);
#else
  valid=(current&1)&&result.players==1&&result.selected==0;
#endif
 }
 unsigned selected=result.players==2?3u:result.selected==0x20?1u:result.selected<2?1u<<result.selected:0;
 for(unsigned p=0;p<RUNTIME_GAMEPAD_PLAYERS;p++)
  if((selected&(1u<<p))&&beforeDisconnect[p]!=disconnectGeneration[p])valid=0;
 if(valid)disconnectedIds&=~(result.selected==0x20?id_bit(0x20):selected);
 pthread_mutex_unlock(&inputLock);
 if(!valid)return LM3_CONTROLLER_SUPPORT_CANCELED;
 runtime_input_clear();if(out)*out=result;
 fprintf(stderr,"[lm3-input] controller support: %d..%d requested, %d players selected (first ID %#x)\n",request.minimum,request.maximum,result.players,result.selected);
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
   Commands: key <Mac keycode> <milliseconds>, down/up <Mac keycode>, shot <path>, wshot <path>, passdump <frames>, querylog <frames>, anomdump,
   kbtest / kbtype <text> / kbenter / kbesc / kbshot <path> (the text entry bar), threads (dump every guest thread's wait), hletrace <0|1|2> (host calls the game makes), framestep <ms> (pin the frame step), close. */
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
