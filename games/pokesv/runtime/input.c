/* Native keyboard and macOS gamepad backed full-key controller. State ABI follows libnx HID. */
#include "runtime.h"
#include "autoload.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <mach/mach.h>
#include <time.h>
#include <math.h>
#include <unistd.h>
typedef struct{int64_t sampling;uint64_t buttons;int32_t lx,ly,rx,ry;uint32_t attributes,pad;} NpadState;
_Static_assert(sizeof(NpadState)==40,"Npad full-key ABI");
static pthread_mutex_t inputLock=PTHREAD_MUTEX_INITIALIZER;
static unsigned char keys[128];static uint64_t pending;
static uint32_t styles=1;static bool enabled,allowed=true;static int holdType;
static int64_t sample;static uint64_t padSeen;
/* F/T/H/G map the D-pad left/up/right/down, keeping both analog-stick mappings independent. */
static uint64_t button(unsigned code){switch(code){case 38:return 1;case 40:case 49:return 2;case 34:return 4;case 32:return 8;case 6:return 16;case 8:return 32;case 12:return 64;case 14:return 128;case 58:return 256;case 56:return 512;case 36:return 1024;case 48:case 53:return 2048;case 3:return 4096;case 17:return 8192;case 4:return 16384;case 5:return 32768;default:return 0;}}
/* Real user input (a window key, a controller button, a stick pushed past half) stops PSV_AUTOLOAD and is logged at most
   once a minute. In an agent's test session (PSV_SLOT_OWNER other than user) the line says the user is playing it: its
   save is a copy, its scripted measurement is void, and its script must not close the game under the user
   (2026-10-15: the user played 12 minutes in a test window and the script closed it at its time limit). */
static void user_input(const char *what){
 runtime_autoload_user_input();
 static _Atomic uint64_t last;uint64_t now=runtime_now_ns(),prev=atomic_load(&last);
 if(prev&&now-prev<60000000000ull)return;if(!atomic_compare_exchange_strong(&last,&prev,now))return;
 const char *owner=getenv("PSV_SLOT_OWNER");bool agent=owner&&*owner&&strcmp(owner,"user");
 fprintf(stderr,"[input] user input (%s)%s%s\n",what,agent?": the user is playing this test session of ":"",agent?owner:"");
}
/* Keys from the window are the user's; scripted and auto-load presses use the synthetic entry.
   Key 1 opens and closes the settings panel (runtime/settings_panel.h). the earlier port's keys 2-4 (frame interpolation, 2K modes, ultrawide) are off
   in this port: interpolated frames are shown without the game's crop, so XC3's 1280x720 cutscenes, drawn in the corner
   of a 1920x1080 picture, came out in the top-left corner over old frames (seen in play, PR #1309); 2K modes and ultrawide
   have nothing behind them for this game (game_patches.c). */
/* Key 0 marks a glitch the player sees: the game frame two frames on as glitch-<time>.png, with frame interpolation on also
   the next interpolated frame as glitch-<time>-interp.png, a Metal capture of that frame
   as glitch-<time>.gputrace (only when the game started with MTL_CAPTURE_ENABLED=1; read it with gpudebug or Xcode), and
   a [glitch] line in the log. Files go to PSV_GLITCH_DIR, else logs/ in the work folder (make boot), else
   ~/Library/Logs/MacNativeGames (the app). */
extern void dm_gfx_shot_at(long frames, const char *path);
extern void dm_gfx_gpucapture_next(const char *path);
static void glitch_mark(void){
 char dir[900],png[1000],trace[1000],stamp[32];const char *d=getenv("PSV_GLITCH_DIR"),*home=getenv("HOME");
 if(d&&*d)snprintf(dir,sizeof dir,"%s",d);else if(!access("logs",W_OK))snprintf(dir,sizeof dir,"logs");
 else snprintf(dir,sizeof dir,"%s/Library/Logs/MacNativeGames",home?home:"/tmp");
 time_t now=time(NULL);strftime(stamp,sizeof stamp,"%Y%m%d-%H%M%S",localtime(&now));
 snprintf(png,sizeof png,"%s/glitch-%s.png",dir,stamp);snprintf(trace,sizeof trace,"%s/glitch-%s.gputrace",dir,stamp);
 fprintf(stderr,"[glitch] key 0: %s\n",png);dm_gfx_shot_at(2,png);
 {extern void dm_gfx_ishot_next(const char *path);char ip[1000];snprintf(ip,sizeof ip,"%s/glitch-%s-interp.png",dir,stamp);dm_gfx_ishot_next(ip);}
 if(getenv("MTL_CAPTURE_ENABLED"))dm_gfx_gpucapture_next(trace);}
void runtime_input_key_synthetic(unsigned code,int down){if(code>=128)return;pthread_mutex_lock(&inputLock);int press=down&&!keys[code];if(down&&!keys[code])pending|=button(code);keys[code]=(unsigned char)!!down;pthread_mutex_unlock(&inputLock);
 if(press&&code==18)runtime_settings_toggle();
 if(press&&code==29)glitch_mark();}
void runtime_input_key(unsigned code,int down){if(down&&code<128)user_input("keyboard");runtime_input_key_synthetic(code,down);}
void runtime_input_clear(void){pthread_mutex_lock(&inputLock);memset(keys,0,sizeof(keys));pending=0;pthread_mutex_unlock(&inputLock);runtime_gamepad_clear();}
static void initialize(void){pthread_mutex_lock(&inputLock);enabled=true;pthread_mutex_unlock(&inputLock);fprintf(stderr,"[psv] native keyboard controller initialized\n");}
/* Docked: one full-key controller as player 1 (id 0, style bit 0). Handheld (PSV_OPERATION_MODE=handheld, the iPhone):
   the same input is the handheld controller (id 0x20, style bit 1, joy-cons attached) and player 1 is not connected;
   the game reads only the handheld controller in that mode. */
/* PSV_INPUT_LOG=1: one line per new (state kind, controller id, style set) the game reads (which pad the game uses). */
static void input_log(const char *what,uint32_t id){static int on=-1;static uint64_t seen[64];static unsigned n;if(on<0)on=getenv("PSV_INPUT_LOG")!=NULL;if(!on)return;
 uint64_t k=(uint64_t)(uintptr_t)what<<32^id^(uint64_t)styles<<48;for(unsigned i=0;i<n;i++)if(seen[i]==k)return;if(n<64)seen[n++]=k;
 fprintf(stderr,"[psv-input] game reads %s for controller %#x (supported styles %#x, ids allowed %d)\n",what,id,styles,allowed);}
static uint32_t pad_id(void){return runtime_handheld_mode()?0x20u:0u;}
static uint32_t pad_style(void){return runtime_handheld_mode()?2u:1u;}
static void supported_ids(const uint32_t *ids,uint64_t n){if(n>10||(!ids&&n))runtime_fail("invalid controller ids");
 if(getenv("PSV_INPUT_LOG")){char b[128];size_t o=0;for(uint64_t i=0;i<n&&o<100;i++)o+=(size_t)snprintf(b+o,sizeof b-o," %#x",ids[i]);b[o]=0;fprintf(stderr,"[psv-input] game supports controller ids:%s\n",b);}pthread_mutex_lock(&inputLock);allowed=false;for(uint64_t i=0;i<n;i++)if(ids[i]==pad_id())allowed=true;pthread_mutex_unlock(&inputLock);}
static void supported_styles(uint32_t value){pthread_mutex_lock(&inputLock);styles=value;pthread_mutex_unlock(&inputLock);}
static uint32_t style(const uint32_t *id){if(!id)runtime_fail("controller id pointer");input_log("style set",*id);pthread_mutex_lock(&inputLock);uint32_t v=enabled&&allowed&&*id==pad_id()?(styles&pad_style()):0;pthread_mutex_unlock(&inputLock);return v;}
static int pad_states(NpadState *out,int max,const uint32_t *id,uint32_t want_style);
static int full_states(NpadState *out,int max,const uint32_t *id){return pad_states(out,max,id,1);}
static int handheld_states(NpadState *out,int max,const uint32_t *id){return pad_states(out,max,id,2);}
static int pad_states(NpadState *out,int max,const uint32_t *id,uint32_t want_style){if(max<=0)return 0;if(id)input_log(want_style==2?"handheld state":"full-key state",*id);if(!out||!id)runtime_fail("controller output pointer");pthread_mutex_lock(&inputLock);NpadState state={.sampling=++sample};if(enabled&&allowed&&*id==pad_id()&&want_style==pad_style()&&(styles&want_style)){
 state.attributes=1;for(unsigned i=0;i<128;i++)if(keys[i])state.buttons|=button(i);state.buttons|=pending;pending=0;
 state.lx=32767*((int)keys[2]-(int)keys[0]);state.ly=32767*((int)keys[13]-(int)keys[1]);state.rx=32767*((int)keys[124]-(int)keys[123]);state.ry=32767*((int)keys[126]-(int)keys[125]);
 RuntimeGamepadState pad;runtime_gamepad_read(&pad);state.buttons|=pad.buttons;
 if(pad.buttons&~padSeen||abs(pad.lx)>16384||abs(pad.ly)>16384||abs(pad.rx)>16384||abs(pad.ry)>16384)user_input("controller");padSeen=pad.buttons;
 if(abs(pad.lx)>abs(state.lx))state.lx=pad.lx;if(abs(pad.ly)>abs(state.ly))state.ly=pad.ly;
 if(abs(pad.rx)>abs(state.rx))state.rx=pad.rx;if(abs(pad.ry)>abs(state.ry))state.ry=pad.ry;
 }*out=state;pthread_mutex_unlock(&inputLock);return 1;}
static int disconnected_states(NpadState *out,int max,const uint32_t *id){if(id)input_log("joy-con state",*id);if(max<=0)return 0;if(!out)runtime_fail("controller output pointer");pthread_mutex_lock(&inputLock);*out=(NpadState){.sampling=++sample};pthread_mutex_unlock(&inputLock);return 1;}
static void hold_type(int type){if(type<0||type>1)runtime_fail("invalid joy hold type");holdType=type;}
static int get_hold_type(void){return holdType;}
static void disconnect(const uint32_t *id){if(!id)runtime_fail("disconnect id");input_log("disconnect",*id);if(*id==pad_id()){   /* only the pad in use: in handheld mode the earlier port disconnects player 1 */pthread_mutex_lock(&inputLock);allowed=false;pthread_mutex_unlock(&inputLock);runtime_input_clear();}}
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
/* Controller support applet (the Switch's "connect controllers" screen). the earlier port opens it in handheld mode; there is
   exactly one controller here, so it returns at once: one player, on the controller in use (ControllerSupportResultInfo:
   int8 player count, 3 bytes padding, uint32 selected id). */
typedef struct{int8_t players;uint8_t pad[3];uint32_t selected;} ControllerSupportResult;
_Static_assert(sizeof(ControllerSupportResult)==8,"controller support result ABI");
static uint32_t show_controller_support(ControllerSupportResult *out,const void *arg){
 (void)arg;if(out)*out=(ControllerSupportResult){.players=1,.selected=pad_id()};
 static int once;if(!once++)fprintf(stderr,"[psv] controller support applet: one player on controller %#x\n",pad_id());
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
/* GetTouchScreenStates<16>(states, count) (Xenoblade Chronicles 3): the newest state first, as the hid ring returns them;
   with no touch screen there is one state of no touches. Returns how many states were written. */
static int touch_history_16(uint8_t *out,int count){if(count<=0)return 0;touch_states_16(out);return 1;}
static uint32_t show_controller_support_arg(const void *arg){return show_controller_support(NULL,arg);}
/* Npad assignment and style events (Pokémon Violet). There is one full-key controller, always assigned as a dual pair:
   the style set never changes, so its update event is signalled once at bind (the game reads the style it has) and
   assignment requests change nothing. Colours are a neutral grey pair; the player LEDs follow the Switch patterns. */
static void style_event(const uint32_t *id,void *event,int clear){if(!id||!event)runtime_fail("style set event arguments");hle_bind_system_event(event,clear==1);hle_signal_system_event(event);}
static uint32_t get_supported_styles(void){pthread_mutex_lock(&inputLock);uint32_t v=styles;pthread_mutex_unlock(&inputLock);return v;}
static int communicationMode;
static void set_communication_mode(int mode){communicationMode=mode;}
static int get_communication_mode(void){return communicationMode;}
static int get_handheld_activation(void){return 0;}   /* Dual */
static void joy_dual(const uint32_t *id){if(!id)runtime_fail("joy assignment id");}
static void joy_single(const uint32_t *id){joy_dual(id);}
static void joy_single_device(const uint32_t *id,int device){(void)device;joy_dual(id);}
static bool joy_single_out(uint32_t *out,const uint32_t *id,int device){(void)out;(void)device;joy_dual(id);return false;}
static int joy_assignment(const uint32_t *id){joy_dual(id);return 0;}   /* NpadJoyAssignmentMode_Dual */
static uint32_t merge_joys(const uint32_t *a,const uint32_t *b){if(!a||!b)runtime_fail("merge joy ids");return 0;}
static void swap_assignment(const uint32_t *a,const uint32_t *b){if(!a||!b)runtime_fail("swap joy ids");}
static void lr_assignment(void){}
static uint8_t player_led(const uint32_t *id){static const uint8_t pattern[8]={1,3,7,15,9,5,13,6};if(!id)runtime_fail("player led id");return *id<8?pattern[*id]:0;}
typedef struct{uint32_t main,sub;} NpadColor;
static uint32_t controller_color(NpadColor *out,const uint32_t *id){if(!out||!id)runtime_fail("controller colour arguments");*out=(NpadColor){0xff828282,0xff0f0f0f};return 0;}
static uint32_t controller_colors(NpadColor *left,NpadColor *right,const uint32_t *id){uint32_t r=controller_color(left,id);if(!right)runtime_fail("controller colour arguments");*right=*left;return r;}
static uint32_t strap_guide(void){return 0;}
/* Vibration devices and the six-axis sensor: runtime/hid_feedback.c (none unless PSV_RUMBLE=1 / PSV_GYRO=1). */
void *runtime_input_override(const char *name){
 void *feedback=runtime_hid_feedback_override(name);if(feedback)return feedback;
 static const struct{const char *name;void *fn;} entries[]={
 {"_ZN2nn3hid14InitializeNpadEv",initialize},{"_ZN2nn3hid22SetSupportedNpadIdTypeEPKjm",supported_ids},{"_ZN2nn3hid24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE",supported_styles},
 {"_ZN2nn3hid15GetNpadStyleSetERKj",style},{"_ZN2nn3hid13GetNpadStatesEPNS0_16NpadFullKeyStateEiRKj",full_states},
 {"_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyDualStateEiRKj",disconnected_states},{"_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyLeftStateEiRKj",disconnected_states},
 {"_ZN2nn3hid13GetNpadStatesEPNS0_17NpadJoyRightStateEiRKj",disconnected_states},{"_ZN2nn3hid13GetNpadStatesEPNS0_17NpadHandheldStateEiRKj",handheld_states},
 {"_ZN2nn3hid18SetNpadJoyHoldTypeENS0_15NpadJoyHoldTypeE",hold_type},{"_ZN2nn3hid18GetNpadJoyHoldTypeEv",get_hold_type},{"_ZN2nn3hid14DisconnectNpadERKj",disconnect},
 {"_ZN2nn3hid21ShowControllerSupportEPNS0_27ControllerSupportResultInfoERKNS0_20ControllerSupportArgE",show_controller_support},
 {"_ZN2nn3hid15InitializeMouseEv",initialize_mouse},{"_ZN2nn3hid18InitializeKeyboardEv",initialize_keyboard},
 {"_ZN2nn3hid14GetMouseStatesEPNS0_10MouseStateEi",mouse_states},{"_ZN2nn3hid16GetKeyboardStateEPNS0_13KeyboardStateE",keyboard_state},
 {"_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj",full_state},{"_ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj",handheld_state},
 {"_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyDualStateERKj",disconnected_state},{"_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyLeftStateERKj",disconnected_state},
 {"_ZN2nn3hid12GetNpadStateEPNS0_17NpadJoyRightStateERKj",disconnected_state},{"_ZN2nn3hid13GetMouseStateEPNS0_10MouseStateE",mouse_state},
 {"_ZN2nn3hid29SetNpadHandheldActivationModeENS0_26NpadHandheldActivationModeE",handheld_activation},{"_ZN2nn3hid21InitializeTouchScreenEv",initialize_touch},
 {"_ZN2nn3hid19GetTouchScreenStateILm1EEEvPNS0_16TouchScreenStateIXT_EEE",touch_states_1},{"_ZN2nn3hid19GetTouchScreenStateILm16EEEvPNS0_16TouchScreenStateIXT_EEE",touch_states_16},
 {"_ZN2nn3hid20GetTouchScreenStatesILm16EEEiPNS0_16TouchScreenStateIXT_EEEi",touch_history_16},
 {"_ZN2nn3hid21ShowControllerSupportERKNS0_20ControllerSupportArgE",show_controller_support_arg},
 {"_ZN2nn3hid27BindNpadStyleSetUpdateEventERKjPNS_2os15SystemEventTypeENS3_14EventClearModeE",style_event},
 {"_ZN2nn3hid24GetSupportedNpadStyleSetEv",get_supported_styles},{"_ZN2nn3hid24GetNpadCommunicationModeEv",get_communication_mode},
 {"_ZN2nn3hid24SetNpadCommunicationModeENS0_21NpadCommunicationModeE",set_communication_mode},{"_ZN2nn3hid29GetNpadHandheldActivationModeEv",get_handheld_activation},
 {"_ZN2nn3hid28SetNpadJoyAssignmentModeDualERKj",joy_dual},{"_ZN2nn3hid30SetNpadJoyAssignmentModeSingleERKj",joy_single},
 {"_ZN2nn3hid30SetNpadJoyAssignmentModeSingleERKjNS0_17NpadJoyDeviceTypeE",joy_single_device},
 {"_ZN2nn3hid30SetNpadJoyAssignmentModeSingleEPjRKjNS0_17NpadJoyDeviceTypeE",joy_single_out},{"_ZN2nn3hid20GetNpadJoyAssignmentERKj",joy_assignment},
 {"_ZN2nn3hid23MergeSingleJoyAsDualJoyERKjS2_",merge_joys},{"_ZN2nn3hid18SwapNpadAssignmentERKjS2_",swap_assignment},
 {"_ZN2nn3hid21StartLrAssignmentModeEv",lr_assignment},{"_ZN2nn3hid20StopLrAssignmentModeEv",lr_assignment},{"_ZN2nn3hid19GetPlayerLedPatternERKj",player_led},
 {"_ZN2nn3hid22GetNpadControllerColorEPNS0_19NpadControllerColorERKj",controller_color},
 {"_ZN2nn3hid22GetNpadControllerColorEPNS0_19NpadControllerColorES2_RKj",controller_colors},{"_ZN2nn3hid24ShowControllerStrapGuideEv",strap_guide}};
 for(unsigned i=0;i<sizeof(entries)/sizeof(*entries);i++)if(!strcmp(name,entries[i].name))return entries[i].fn;return NULL;
}

/* PSV_INPUT_SCRIPT="sec:keycode:ms,...": press Mac key codes at given seconds after start (diagnostics without a
   picture: does the game react to a button). Key codes are the frontend's (J=38 is A, Return=36 is Plus). */
#include <unistd.h>
static void *input_script(void *arg) {
    const char *p = arg; uint64_t t0 = runtime_now_ns();
    while (p && *p) {
        double at = strtod(p, (char **)&p); if (*p++ != ':') break;
        unsigned code = (unsigned)strtoul(p, (char **)&p, 10); if (*p++ != ':') break;
        unsigned ms = (unsigned)strtoul(p, (char **)&p, 10); if (*p == ',') p++;
        while ((runtime_now_ns() - t0) / 1e9 < at) usleep(10000);
        fprintf(stderr, "[psv-input] script: key %u down for %u ms at %.1f s\n", code, ms, at);
        runtime_input_key_synthetic(code, 1); usleep(ms * 1000); runtime_input_key_synthetic(code, 0);
    }
    return NULL;
}
__attribute__((constructor)) static void input_script_start(void) {
    const char *e = getenv("PSV_INPUT_SCRIPT"); if (!e) return;
    pthread_t t; pthread_create(&t, NULL, input_script, (void *)e); pthread_detach(t);
}

/* Optional local integration-test command stream. Appending complete lines lets tests inspect each
   actual frame before choosing the next input, instead of guessing load/menu timings. Disabled normally.
   Commands: key <Mac keycode> <milliseconds>, down/up <Mac keycode>, shot <path>, wshot <path>, passdump <frames>, gpucapture <path>.gputrace, querylog <frames>, anomdump,
   kbtest / kbtype <text> / kbenter / kbesc / kbshot <path> (the text entry bar), threads (dump every guest thread's wait), hletrace <0|1|2> (host calls the game makes), framestep <ms> (pin the frame step), close. */
extern void dm_gfx_shot_at(long frames, const char *path);
extern void runtime_front_request_close(void);
static void *input_file(void *arg) {
    FILE *f = fopen((const char *)arg, "r"); if (!f) runtime_fail("cannot open PSV_INPUT_FILE");
    char line[1200];
    for (;;) {
        if (!fgets(line, sizeof line, f)) { if (ferror(f)) runtime_fail("diagnostic input read"); clearerr(f); usleep(100000); continue; }
        unsigned code, ms; int used = 0;
        if (sscanf(line, "down %u %n", &code, &used) == 1 && used && !line[used] && code < 128) {
            fprintf(stderr,"[psv-input] file: key %u down\n",code);runtime_input_key_synthetic(code,1);
        } else if (sscanf(line, "up %u %n", &code, &used) == 1 && used && !line[used] && code < 128) {
            fprintf(stderr,"[psv-input] file: key %u up\n",code);runtime_input_key_synthetic(code,0);
        } else if (sscanf(line, "key %u %u %n", &code, &ms, &used) == 2 && used && !line[used] && code < 128 && ms > 0 && ms <= 5000) {
            fprintf(stderr, "[psv-input] file: key %u for %u ms\n", code, ms);
            runtime_input_key_synthetic(code, 1); usleep(ms * 1000); runtime_input_key_synthetic(code, 0);
        } else if (sscanf(line, "fpslock %u %n", &code, &used) == 1 && used && !line[used]) {   /* diagnostic: the frame lock without the panel */
            extern void nvn_fps_lock_set(int fps); nvn_fps_lock_set((int)code); fprintf(stderr, "[psv-input] file: frame lock %u\n", code);
        } else if (!strncmp(line, "find ", 5)) {
            /* Diagnostic: "find <hex value>" lists the 8-byte words in main's data and bss that hold that value (main+0x...). */
            uint64_t v = strtoull(line + 5, NULL, 16); uint8_t *mb = runtime_module_base("main"); int hits = 0;
            uint64_t lo = 0x4383000, hi = 0x48d4000;   /* Violet 4.0.0 main: DATA_OFF to the end of the image */
            for (uint64_t o = lo; mb && o + 8 <= hi && hits < 32; o += 8) { uint64_t q = 0; vm_size_t got = 0;
                if (vm_read_overwrite(mach_task_self(), (vm_address_t)(mb + o), 8, (vm_address_t)&q, &got) == KERN_SUCCESS && q == v) { fprintf(stderr, "[psv-input] find %#llx at main+%#llx\n", (unsigned long long)v, (unsigned long long)o); hits++; } }
            fprintf(stderr, "[psv-input] find %#llx: %d hits\n", (unsigned long long)v, hits);
        } else if (!strncmp(line, "findf ", 6)) {
            /* Diagnostic (2026-11-02): "findf <x> <y> <z> [<tolerance>]" lists readable and writable memory holding three
               consecutive floats within the tolerance (default 0.01) of x, y, z: a camera or an object position on the heap. */
            float x = 0, y = 0, z = 0, tol = 0.01f; int n = sscanf(line + 6, "%f %f %f %f", &x, &y, &z, &tol);
            if (n >= 3) { unsigned hits = 0; vm_address_t ra = 0; vm_size_t rs = 0; natural_t depth = 0;
                for (;;) { struct vm_region_submap_info_64 info; mach_msg_type_number_t cnt = VM_REGION_SUBMAP_INFO_COUNT_64;
                    if (vm_region_recurse_64(mach_task_self(), &ra, &rs, &depth, (vm_region_recurse_info_t)&info, &cnt) != KERN_SUCCESS) break;
                    if (info.is_submap) { depth++; continue; }
                    if ((info.protection & 3) == 3 && rs < ((vm_size_t)4 << 30)) {
                        const float *f = (const float *)ra; size_t nf = rs / 4;
                        for (size_t i = 0; i + 2 < nf && hits < 40; i++)
                            if (fabsf(f[i] - x) < tol && fabsf(f[i + 1] - y) < tol && fabsf(f[i + 2] - z) < tol) { fprintf(stderr, "[psv-input] findf %#llx\n", (unsigned long long)(ra + i * 4)); hits++; }
                    }
                    ra += rs; }
                fprintf(stderr, "[psv-input] findf %g %g %g: %u hits\n", x, y, z, hits); }
        } else if (!strncmp(line, "findd ", 6) || !strncmp(line, "poke4 ", 6)) {
            /* Diagnostic (2026-11-02): "findd <value>" lists writable memory holding that 4-byte value (first 200 hits),
               "poke4 <addr> <value>" writes 4 bytes. Floats as their bit pattern (1/30 = 0x3d088889). */
            uint64_t a1 = 0, a2 = 0; char *e1 = NULL; a1 = strtoull(line + 6, &e1, 0); if (e1) a2 = strtoull(e1, NULL, 0);
            if (line[1] == 'o') { uint32_t v = (uint32_t)a2;
                int ok = vm_write(mach_task_self(), (vm_address_t)a1, (vm_offset_t)&v, 4) == KERN_SUCCESS;
                fprintf(stderr, "[psv-input] poke4 %#llx = %#x %s\n", (unsigned long long)a1, v, ok ? "ok" : "FAILED");
            } else { unsigned hits = 0; vm_address_t ra = 0; vm_size_t rs = 0; natural_t depth = 0; uint32_t want = (uint32_t)a1;
                for (;;) { struct vm_region_submap_info_64 info; mach_msg_type_number_t cnt = VM_REGION_SUBMAP_INFO_COUNT_64;
                    if (vm_region_recurse_64(mach_task_self(), &ra, &rs, &depth, (vm_region_recurse_info_t)&info, &cnt) != KERN_SUCCESS) break;
                    if (info.is_submap) { depth++; continue; }
                    if ((info.protection & 3) == 3 && rs < ((vm_size_t)4 << 30) && ra >= 0x9000000000ull) {
                        const uint32_t *q = (const uint32_t *)ra; size_t nq = rs / 4;
                        for (size_t i = 0; i < nq && hits < 200; i++) if (q[i] == want) { fprintf(stderr, "[psv-input] findd %#llx\n", (unsigned long long)(ra + i * 4)); hits++; }
                    }
                    ra += rs; }
                fprintf(stderr, "[psv-input] findd %#x: %u hits\n", want, hits); }
        } else if (!strncmp(line, "findq ", 6) || !strncmp(line, "poke8 ", 6)) {
            /* Diagnostic (2026-11-02): "findq <value>" lists writable memory holding that 8-byte value (first 60 hits);
               "poke8 <addr> <value>" writes 8 bytes. Values in hex (0x...) or decimal. */
            uint64_t a1 = 0, a2 = 0; char *e1 = NULL; a1 = strtoull(line + 6, &e1, 0); if (e1) a2 = strtoull(e1, NULL, 0);
            if (line[1] == 'o') {
                int ok = vm_write(mach_task_self(), (vm_address_t)a1, (vm_offset_t)&a2, 8) == KERN_SUCCESS;
                fprintf(stderr, "[psv-input] poke8 %#llx = %#llx %s\n", (unsigned long long)a1, (unsigned long long)a2, ok ? "ok" : "FAILED");
            } else { unsigned hits = 0; vm_address_t ra = 0; vm_size_t rs = 0; natural_t depth = 0;
                for (;;) { struct vm_region_submap_info_64 info; mach_msg_type_number_t cnt = VM_REGION_SUBMAP_INFO_COUNT_64;
                    if (vm_region_recurse_64(mach_task_self(), &ra, &rs, &depth, (vm_region_recurse_info_t)&info, &cnt) != KERN_SUCCESS) break;
                    if (info.is_submap) { depth++; continue; }
                    if ((info.protection & 3) == 3 && rs < ((vm_size_t)4 << 30)) {
                        const uint64_t *q = (const uint64_t *)ra; size_t nq = rs / 8;
                        for (size_t i = 0; i < nq && hits < 60; i++) if (q[i] == a1) { fprintf(stderr, "[psv-input] findq %#llx\n", (unsigned long long)(ra + i * 8)); hits++; }
                    }
                    ra += rs; }
                fprintf(stderr, "[psv-input] findq %#llx: %u hits\n", (unsigned long long)a1, hits); }
        } else if (!strncmp(line, "peek ", 5) || !strncmp(line, "poke1 ", 6)) {
            /* Diagnostic (2026-10-31): "peek <addr> [<off> ...] <words>" reads guest memory: <addr> is main+0x<hex> or 0x<hex>;
               every <off> after it dereferences the pointer there and adds the offset. "poke1 <addr> <value>" writes one byte.
               Reads go through vm_read, so a bad address is reported, not a crash. */
            char *tok[16]; int nt = 0; for (char *s = strtok(line + (line[1] == 'e' ? 5 : 6), " \t\r\n"); s && nt < 16; s = strtok(NULL, " \t\r\n")) tok[nt++] = s;
            uint64_t a = 0; int ok = nt >= 2;
            if (ok) { if (!strncmp(tok[0], "main+", 5)) { uint8_t *mb = runtime_module_base("main"); a = (uint64_t)(uintptr_t)mb + strtoull(tok[0] + 5, NULL, 16); } else a = strtoull(tok[0], NULL, 16); }
            if (ok && line[1] == 'o') {
                uint8_t v = (uint8_t)strtoul(tok[1], NULL, 0);
                ok = vm_write(mach_task_self(), (vm_address_t)a, (vm_offset_t)&v, 1) == KERN_SUCCESS;
                fprintf(stderr, "[psv-input] poke1 %#llx = %u %s\n", (unsigned long long)a, v, ok ? "ok" : "FAILED");
            } else if (ok) {
                for (int i = 1; i < nt - 1 && ok; i++) { uint64_t p = 0; vm_size_t got = 0;
                    ok = vm_read_overwrite(mach_task_self(), (vm_address_t)a, 8, (vm_address_t)&p, &got) == KERN_SUCCESS && got == 8; a = p + strtoull(tok[i], NULL, 16); }
                unsigned words = (unsigned)strtoul(tok[nt - 1], NULL, 0); if (words > 32) words = 32;
                uint64_t w[32] = {0}; vm_size_t got = 0;
                if (ok) ok = vm_read_overwrite(mach_task_self(), (vm_address_t)a, words * 8, (vm_address_t)w, &got) == KERN_SUCCESS;
                fprintf(stderr, "[psv-input] peek %#llx:%s", (unsigned long long)a, ok ? "" : " unreadable");
                for (unsigned i = 0; ok && i < words; i++) fprintf(stderr, " %016llx", (unsigned long long)w[i]);
                fprintf(stderr, "\n");
            }
        } else if (sscanf(line, "passdump %u %n", &code, &used) == 1 && used && !line[used] && code > 0 && code < 16) {
            extern void dm_gfx_passdump_next(long n); dm_gfx_passdump_next((long)code);
        } else if (!strncmp(line, "ishot ", 6) && line[6]) {   /* "ishot <path>": the next interpolated frame (frame interpolation on) */
            line[strcspn(line, "\r\n")] = 0; extern void dm_gfx_ishot_next(const char *path); dm_gfx_ishot_next(line + 6);
        } else if (!strncmp(line, "gpucapture ", 11) && line[11]) {   /* "gpucapture <path>.gputrace": a Metal capture of the next frame (needs MTL_CAPTURE_ENABLED=1) */
            char *e = line + strlen(line); while (e > line + 11 && (e[-1] == '\n' || e[-1] == '\r')) *--e = 0;
            extern void dm_gfx_gpucapture_next(const char *path); dm_gfx_gpucapture_next(line + 11);
        } else if (!strncmp(line, "anomdump", 8) && (line[8] == '\n' || line[8] == '\r' || !line[8])) {   /* PSV_ANOMALY=<dir>: write the per-draw records (with GPU sample counts) of the last frames now */
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
    const char *e = getenv("PSV_INPUT_FILE"); if (!e || !*e) return;
    pthread_t t; if (pthread_create(&t, NULL, input_file, (void *)e)) runtime_fail("diagnostic input thread"); pthread_detach(t);
}
