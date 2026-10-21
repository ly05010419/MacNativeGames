/* Native keyboard and macOS gamepad backed full-key controller. State ABI follows libnx HID. */
#include <TargetConditionals.h>
#include "runtime.h"
#include "autoload.h"
#include "world_survey.h"
#include <stdbool.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
typedef struct{int64_t sampling;uint64_t buttons;int32_t lx,ly,rx,ry;uint32_t attributes,pad;} NpadState;
_Static_assert(sizeof(NpadState)==40,"Npad full-key ABI");
static pthread_mutex_t inputLock=PTHREAD_MUTEX_INITIALIZER;
static unsigned char keys[INPUT_SOURCES];static uint64_t pending;
static uint32_t styles=1;static bool enabled,allowed=true;static int holdType;
static int64_t sample;static uint64_t padSeen;
static RuntimeKeyboardBindings inputBindings;
static int inputBindingsLoaded,inputSuspended;
static int32_t mouseRX,mouseRY;static uint64_t mouseLast;
static void bindings_init(void){if(!inputBindingsLoaded){input_bindings_default(&inputBindings);inputBindingsLoaded=1;}}
static uint64_t button(unsigned code){bindings_init();uint64_t result=0;for(unsigned i=0;i<16;i++)for(unsigned j=0;j<2;j++)if(inputBindings.source[i][j]==code+1)result|=1ULL<<i;return result;}
static int bound_down(unsigned action){for(unsigned j=0;j<2;j++){unsigned s=inputBindings.source[action][j];if(s&&keys[s-1])return 1;}return 0;}
void runtime_input_bindings_get(RuntimeKeyboardBindings *b){pthread_mutex_lock(&inputLock);bindings_init();*b=inputBindings;pthread_mutex_unlock(&inputLock);}
int runtime_input_bindings_set(const RuntimeKeyboardBindings *b){if(!input_bindings_valid(b))return 0;pthread_mutex_lock(&inputLock);inputBindings=*b;inputBindingsLoaded=1;pthread_mutex_unlock(&inputLock);runtime_input_clear();return 1;}
int runtime_input_suspended(void){pthread_mutex_lock(&inputLock);int s=inputSuspended;pthread_mutex_unlock(&inputLock);return s;}
void runtime_input_suspend(int suspended){pthread_mutex_lock(&inputLock);inputSuspended=!!suspended;pthread_mutex_unlock(&inputLock);runtime_input_clear();}
void runtime_input_mouse_motion(double dx,double dy){
 if(!isfinite(dx)||!isfinite(dy))return;
 pthread_mutex_lock(&inputLock);bindings_init();
 if(!inputSuspended&&inputBindings.mouse_look){
  uint64_t now=runtime_now_ns();double dt=mouseLast&&now>mouseLast?(now-mouseLast)/1e9:1.0/60.0;
  if(dt<1.0/1000)dt=1.0/1000;if(dt>0.08)dt=1.0/60.0;
  double k=24.0*inputBindings.mouse_sensitivity/100.0/dt,x=dx*k,y=-dy*k*(inputBindings.mouse_invert_y?-1:1);
  mouseRX=(int32_t)(x>32767?32767:x< -32767?-32767:x);mouseRY=(int32_t)(y>32767?32767:y< -32767?-32767:y);mouseLast=now;
 }pthread_mutex_unlock(&inputLock);
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
/* Exact modifier matching and one action per press, shared by live and diagnostic input.
   Consumed shortcut keys never enter the emulated controller. */
enum { KEY_INTERP_TOGGLE=19, KEY_OUTPUT_MODE_CYCLE=20, KEY_ASPECT_TOGGLE=21, KEY_TAA_TOGGLE=23 };
extern int runtime_controls_handle_key(unsigned,unsigned) __attribute__((weak_import));
extern int runtime_controls_active(void) __attribute__((weak_import));
extern void runtime_front_control_test(const char *) __attribute__((weak_import));
static unsigned char pressedKeys[INPUT_SOURCES],shortcutHeld[INPUT_SOURCES];
static void input_key_modifiers(unsigned code,int down,unsigned modifiers){
 if(code>=INPUT_SOURCES)return;
 pthread_mutex_lock(&inputLock);
 if(inputSuspended){pthread_mutex_unlock(&inputLock);return;}
 int press=down&&!pressedKeys[code],held=shortcutHeld[code];pressedKeys[code]=(unsigned char)!!down;
 if(!down)shortcutHeld[code]=0;
 pthread_mutex_unlock(&inputLock);
 if(held)return;
 if(code<INPUT_KEYS&&press&&runtime_controls_handle_key&&runtime_controls_handle_key(code,modifiers)){
  pthread_mutex_lock(&inputLock);shortcutHeld[code]=1;pthread_mutex_unlock(&inputLock);return;
 }
 pthread_mutex_lock(&inputLock);if(down&&!keys[code])pending|=button(code);keys[code]=(unsigned char)!!down;pthread_mutex_unlock(&inputLock);
 if(!runtime_controls_active||!runtime_controls_active()){
  if(press&&code==18){extern void runtime_shortcut_help_toggle(void);runtime_shortcut_help_toggle();}
  if(press&&code==KEY_INTERP_TOGGLE){extern void dm_gfx_set_flag(const char *,int);dm_gfx_set_flag("mfx_interp",-1);}
  if(press&&code==KEY_OUTPUT_MODE_CYCLE)runtime_output_mode_cycle();
  if(press&&code==KEY_ASPECT_TOGGLE)runtime_aspect_toggle();
 }
 if(press&&code==KEY_TAA_TOGGLE){extern void taa_toggle(void);taa_toggle();}
}
void runtime_input_key_synthetic(unsigned code,int down){if(code<INPUT_KEYS)input_key_modifiers(code,down,0);}
void runtime_input_mouse_button(unsigned button,int down){if(button>=INPUT_MOUSE_BUTTONS)return;if(down)user_input("mouse");input_key_modifiers(INPUT_KEYS+button,down,0);}
void runtime_input_key_modifiers(unsigned code,int down,unsigned modifiers){if(down&&code<128)user_input("keyboard");input_key_modifiers(code,down,modifiers);}
void runtime_input_key(unsigned code,int down){runtime_input_key_modifiers(code,down,0);}
void runtime_input_clear(void){pthread_mutex_lock(&inputLock);memset(keys,0,sizeof(keys));memset(pressedKeys,0,sizeof(pressedKeys));memset(shortcutHeld,0,sizeof(shortcutHeld));pending=0;mouseRX=mouseRY=0;mouseLast=0;pthread_mutex_unlock(&inputLock);runtime_gamepad_clear();}

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
 bindings_init();
 state.attributes=1;if(inputSuspended){*out=state;pthread_mutex_unlock(&inputLock);return 1;}for(unsigned i=0;i<INPUT_SOURCES;i++)if(keys[i])state.buttons|=button(i);state.buttons|=pending;pending=0;
 state.lx=32767*(bound_down(INPUT_RIGHT)-bound_down(INPUT_LEFT));state.ly=32767*(bound_down(INPUT_UP)-bound_down(INPUT_DOWN));
 state.rx=32767*(bound_down(INPUT_LOOK_RIGHT)-bound_down(INPUT_LOOK_LEFT));state.ry=32767*(bound_down(INPUT_LOOK_UP)-bound_down(INPUT_LOOK_DOWN));
 uint64_t now=runtime_now_ns();if(mouseLast&&now>=mouseLast&&now-mouseLast<80000000ULL){if(abs(mouseRX)>abs(state.rx))state.rx=mouseRX;if(abs(mouseRY)>abs(state.ry))state.ry=mouseRY;}
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
/* Vibration devices and the six-axis sensor: runtime/hid_feedback.c (none unless TOTK_RUMBLE=1 / TOTK_GYRO=1). */
#ifdef GAME_BOTW
/* SDK 7 (Breath of the Wild 1.6.0, botw.md). The console's own seven-axis sensor (Labo VR) is absent on a Mac: it starts and
   stops without states. Gyroscope zero-drift mode and the dual Joy-Con assignment change nothing for the host controller.
   The one-argument controller applet returns at once, as the two-argument one does (one player). */
static uint32_t seven_axis_init(void *work,unsigned long size){(void)work;(void)size;fprintf(stderr,"[botw] seven-axis sensor: not present\n");return 0;}
static uint32_t seven_axis_ok(void){return 0;}
static void seven_axis_reset(void){}
static int seven_axis_states(void *out,int count){(void)out;(void)count;return 0;}
static void zero_drift_mode(const void *handle,const int *mode){(void)handle;(void)mode;}
static void joy_dual(const uint32_t *id){(void)id;}
static uint32_t show_controller_support_v1(const void *arg){return show_controller_support(NULL,arg);}
#endif
void *runtime_input_override(const char *name){
 void *feedback=runtime_hid_feedback_override(name);if(feedback)return feedback;
 static const struct{const char *name;void *fn;} entries[]={
 {"_ZN2nn3hid14InitializeNpadEv",initialize},{"_ZN2nn3hid22SetSupportedNpadIdTypeEPKjm",supported_ids},{"_ZN2nn3hid24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE",supported_styles},
 {"_ZN2nn3hid15GetNpadStyleSetERKj",style},{"_ZN2nn3hid13GetNpadStatesEPNS0_16NpadFullKeyStateEiRKj",full_states},
 {"_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyDualStateEiRKj",disconnected_states},{"_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyLeftStateEiRKj",disconnected_states},
 {"_ZN2nn3hid13GetNpadStatesEPNS0_17NpadJoyRightStateEiRKj",disconnected_states},{"_ZN2nn3hid13GetNpadStatesEPNS0_17NpadHandheldStateEiRKj",handheld_states},
 {"_ZN2nn3hid18SetNpadJoyHoldTypeENS0_15NpadJoyHoldTypeE",hold_type},{"_ZN2nn3hid14DisconnectNpadERKj",disconnect},
 {"_ZN2nn3hid21ShowControllerSupportEPNS0_27ControllerSupportResultInfoERKNS0_20ControllerSupportArgE",show_controller_support},
 {"_ZN2nn3hid15InitializeMouseEv",initialize_mouse},{"_ZN2nn3hid18InitializeKeyboardEv",initialize_keyboard},
 {"_ZN2nn3hid14GetMouseStatesEPNS0_10MouseStateEi",mouse_states},{"_ZN2nn3hid16GetKeyboardStateEPNS0_13KeyboardStateE",keyboard_state}
#ifdef GAME_BOTW
 ,{"_ZN2nn3hid28InitializeSevenSixAxisSensorEPvm",seven_axis_init},{"_ZN2nn3hid26FinalizeSevenSixAxisSensorEv",seven_axis_ok},
 {"_ZN2nn3hid23StartSevenSixAxisSensorEv",seven_axis_ok},{"_ZN2nn3hid22StopSevenSixAxisSensorEv",seven_axis_ok},
 {"_ZN2nn3hid32ResetSevenSixAxisSensorTimestampEv",seven_axis_reset},
 {"_ZN2nn3hid27GetSevenSixAxisSensorStatesEPNS0_23SevenSixAxisSensorStateEi",seven_axis_states},
 {"_ZN2nn3hid25SetGyroscopeZeroDriftModeERKNS0_19SixAxisSensorHandleERKNS0_22GyroscopeZeroDriftModeE",zero_drift_mode},
 {"_ZN2nn3hid28SetNpadJoyAssignmentModeDualERKj",joy_dual},
 {"_ZN2nn3hid21ShowControllerSupportERKNS0_20ControllerSupportArgE",show_controller_support_v1}
#endif
 };
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
#ifdef GAME_BOTW
        } else if (!strncmp(line, "ashot ", 6)) {   /* "ashot <path>": the next game frame, written in the background, its present time logged (botw.md) */
            line[strcspn(line, "\r\n")] = 0; if (!line[6]) runtime_fail("diagnostic ashot path");
            extern void dm_gfx_ashot(const char *path); dm_gfx_ashot(line + 6);
#endif
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
#if defined(TOTK_GAME_V143) && !TARGET_OS_IPHONE   /* high-frame-rate diagnostics (game_patches_v143.c) */
        } else if (!strncmp(line, "frmwatch ", 9) || !strncmp(line, "frmwatchx ", 10)) {   /* "frmwatch[x] <seconds> [hex offset]": who reads a time value */
            extern void runtime_diag_frm_watch(double secs, unsigned off, int in_x);
            int in_x = line[8] == 'x'; double secs; unsigned off = 0x10;
            if (sscanf(line + 9 + in_x, "%lf %x", &secs, &off) < 1 || secs <= 0 || off > 0x400 || off % 4) runtime_fail("frmwatch[x] <seconds> [hex offset, a multiple of 4]");
            runtime_diag_frm_watch(secs, off, in_x);
        } else if (!strncmp(line, "xpoke ", 6)) {   /* "xpoke <hex offset> <value>": a float into the game's time object */
            extern void runtime_diag_xpoke(unsigned off, float value);
            unsigned off; float value; if (sscanf(line + 6, "%x %f", &off, &value) != 2 || off > 0x400 || off % 4) runtime_fail("xpoke <hex offset, a multiple of 4> <value>");
            runtime_diag_xpoke(off, value);
#endif
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
