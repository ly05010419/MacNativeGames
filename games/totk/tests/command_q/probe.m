/* Exercise the real AppKit menu and window-close handler without game data. */
#include "../../runtime/frontend.m"
#include <time.h>

static int mode;
static uint64_t start;
static uint64_t clock_ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec;}
uint64_t runtime_now_ns(void){return (clock_ns()-start)*(mode==2?100:1);}
int runtime_fs_save_busy(void){return mode==2||(mode==1&&runtime_now_ns()<200000000ull);}
_Noreturn void runtime_fail(const char *message){fprintf(stderr,"FAIL: %s\n",message);exit(1);}
void runtime_input_key(unsigned code,int down){(void)code;(void)down;}
void runtime_input_clear(void){}
void runtime_input_clear_window(void){}
#ifndef FRONTEND_REAL_GAMEPAD
void runtime_gamepad_start(void){}
void runtime_gamepad_focus(int on){(void)on;}
int runtime_gamepad_background_enabled(void){return 0;}
void runtime_gamepad_background_set(int on){(void)on;}
#endif
#ifndef FRONTEND_REAL_GAMEPAD
void runtime_gamepad_settings_enable(int on){(void)on;}
#endif
#ifndef FRONTEND_REAL_GAMEPAD
int runtime_gamepad_settings_requested(void){return 0;}
#endif
#ifndef FRONTEND_REAL_GAMEPAD
int runtime_gamepad_settings_button(void){return -1;}
void runtime_gamepad_settings_button_set(int physical){(void)physical;}
#endif
#ifndef FRONTEND_REAL_GAMEPAD
static int textCaptureFixture;
#endif
#ifndef FRONTEND_REAL_GAMEPAD
void runtime_gamepad_text_capture(int on){textCaptureFixture=on;}
#endif
#ifndef FRONTEND_REAL_GAMEPAD
void runtime_gamepad_text_read(RuntimeGamepadState *out){*out=(RuntimeGamepadState){0};}
#endif
#ifndef FRONTEND_REAL_GAMEPAD
int runtime_setting_word(const char *n,char *s,size_t z){(void)n;(void)s;(void)z;return 0;}
#endif
#ifndef FRONTEND_REAL_GAMEPAD
void runtime_setting_save(const char *n,const char *s,const char *w){(void)n;(void)s;(void)w;}
#endif
#ifndef FRONTEND_REAL_GAMEPAD
int runtime_shared_setting_word(const char *n,char *s,size_t z){(void)n;(void)s;(void)z;return 0;}
void runtime_shared_setting_save(const char *n,const char *s,const char *w){(void)n;(void)s;(void)w;}
#endif
#ifndef FRONTEND_REAL_GAMEPAD
int runtime_gamepad_type(int *detected){if(detected)*detected=-1;return 0;}
#endif
#ifndef FRONTEND_REAL_GAMEPAD
int runtime_gamepad_battery_percent(void){return -1;}
int runtime_gamepad_battery_left_percent(void){return -1;}
#endif
#ifndef FRONTEND_REAL_GAMEPAD
int runtime_mouse_enabled(void){return 0;}
#endif
#ifndef FRONTEND_REAL_GAMEPAD
void runtime_mouse_set_hook(void (*hook)(void)){(void)hook;}
#endif
#ifndef FRONTEND_REAL_GAMEPAD
void runtime_mouse_move(double dx,double dy){(void)dx;(void)dy;}
#endif
#ifndef FRONTEND_REAL_GAMEPAD
void runtime_mouse_button(int button,int down){(void)button;(void)down;}
#endif
#ifndef FRONTEND_REAL_GAMEPAD
void runtime_mouse_release(void){}
#endif
#ifndef FRONTEND_REAL_GAMEPAD
void runtime_setting_remove(const char *n){(void)n;}
#endif
#ifndef FRONTEND_REAL_GAMEPAD
void runtime_controller_map_get(uint16_t *map,uint8_t *modes){controller_map_default(map,modes);}
#endif
#ifndef FRONTEND_REAL_GAMEPAD
void runtime_controller_map_set(const uint16_t *map,const uint8_t *modes){(void)map;(void)modes;}
#endif
#ifndef FRONTEND_REAL_GAMEPAD
void runtime_controller_map_reset(void){}
#endif
/* runtime/input.c's keyboard and mouse layout (the settings panel's key bindings). */
static KeyMap probeKeyMap;static int probeKeyMapSet;
void runtime_keymap_get(KeyMap *out){if(!probeKeyMapSet){keymap_default(&probeKeyMap);probeKeyMapSet=1;}*out=probeKeyMap;}
void runtime_keymap_set(const KeyMap *map){probeKeyMap=*map;probeKeyMapSet=1;}
void runtime_keymap_reset(void){keymap_default(&probeKeyMap);probeKeyMapSet=1;}
void runtime_input_mouse(int button,int down){(void)button;(void)down;}
void runtime_oe_push_message(uint32_t message){(void)message;}
void hle_bind_system_event(void *event,int autoclear){(void)event;(void)autoclear;}
void hle_signal_system_event(void *event){(void)event;}
uint64_t dm_nvn_frame_count(void){return 0;}
void fps_overlay_set_hud_mode(int mode){(void)mode;}
int hud_compact(void){return 0;}
int hud_snapshot(int line,char *out,size_t n,int *highlight){(void)line;(void)out;(void)n;(void)highlight;return 0;}
int fps_overlay_full_hud(void){return 1;}
int dm_gfx_fps_now(int *generated){if(generated)*generated=0;return -1;}
void runtime_aspect_view(double w,double h){(void)w;(void)h;}
int taa_enabled(void){return 0;}
/* The settings panel (runtime/settings_panel.h) is not opened here. */
void taa_set(int on){(void)on;}
int mfx_requested(void){return 0;}
int fps_overlay_hud_mode(void){return 1;}
void dm_gfx_set_flag(const char *name,int value){(void)name;(void)value;}
void dm_gfx_aniso_start(int n){(void)n;} int dm_gfx_aniso_locked(void){return 0;} int dm_gfx_aniso(void){return 0;} void dm_gfx_aniso_set(int n){(void)n;}   /* the display page's anisotropy row (nvn_mtl.m) */
int runtime_output_modes(void){return 0;}
int runtime_output_mode(void){return 0;}
int runtime_output_mode_set(int mode){(void)mode;return 0;}
int runtime_aspect_set_ratio(float r){(void)r;return 0;}
float runtime_aspect_ratio(void){return 0;}
float runtime_aspect_start(void){return 0;}
int runtime_output_native(int mode,unsigned *w,unsigned *h){(void)mode;(void)w;(void)h;return 0;}
#ifndef FRONTEND_REAL_GAMEPAD
int runtime_gamepad_type_set(int type){return type;}
#endif
#ifndef FRONTEND_REAL_GAMEPAD
int runtime_mouse_set(int on){return on;}
int runtime_mouse_sensitivity(void){return 50;}
int runtime_mouse_sensitivity_locked(void){return 0;}
void runtime_mouse_sensitivity_set(int sensitivity){(void)sensitivity;}
#endif
void nvn_fps_lock_set(int fps){(void)fps;}
/* TOTK_STATE_REPORTING (gfx/nvn/totk_state_report.m): frontend.m declares them weak, the linker still wants them. */
void totk_sr_start(const char *game,const char *version){(void)game;(void)version;}
void totk_sr_picture(int key,int visible){(void)key;(void)visible;}
void totk_sr_window(int key,int visible){(void)key;(void)visible;}
void totk_sr_view(int w,int h,int hz,int fullscreen){(void)w;(void)h;(void)hz;(void)fullscreen;}
void totk_sr_settings(int open){(void)open;}
void totk_sr_closing(void){fprintf(stderr,"[probe] state report: closing\n");}
int nvn_fps_lock(void){return 0;}
/* The controller rows of the settings panel (PR #477, #523): the panel is not opened here. */
int runtime_feedback_locked(int gyro){(void)gyro;return 0;}
int runtime_feedback_present(int gyro){(void)gyro;return 0;}
int runtime_feedback_choice(int gyro){(void)gyro;return 0;}
void runtime_feedback_choose(int gyro,int on){(void)gyro;(void)on;}
int runtime_rumble_gain_locked(void){return 0;}
float runtime_rumble_gain(void){return 1;}
void runtime_rumble_gain_set(float gain){(void)gain;}
int runtime_triggers_available(void){return 0;}
int runtime_triggers_locked(void){return 0;}
int runtime_triggers_choice(void){return 0;}
void runtime_triggers_choose(int on){(void)on;}
void runtime_triggers_release(void){fprintf(stderr,"[probe] triggers released\n");}   /* the window's close lets the DualSense go */
int fps_overlay_value(void){return 0;}   /* the HUD's FPS graph (PR #940) */

static NSEvent *key(NSEventModifierFlags flags){
 return [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint modifierFlags:flags timestamp:0
     windowNumber:window.windowNumber context:nil characters:@"q" charactersIgnoringModifiers:@"q" isARepeat:NO keyCode:12];
}
int main(int argc,char **argv){
 @autoreleasepool{
  if(argc!=3)return 1;
  mode=atoi(argv[1]);
  [NSApplication sharedApplication];[NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
  window=[[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,320,180)
      styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable backing:NSBackingStoreBuffered defer:NO];
  window.releasedWhenClosed=NO;
  windowDelegate=[TOTKWindowDelegate new];window.delegate=windowDelegate;
  window.contentView=[[TOTKInputView alloc] initWithFrame:window.contentView.bounds];
  [window makeFirstResponder:window.contentView];
  front_install_menu();
  [NSApp finishLaunching];
  if(atoi(argv[2])){
   NSTextField *field=[[NSTextField alloc] initWithFrame:NSMakeRect(10,10,200,25)];
   [window.contentView addSubview:field];[window makeFirstResponder:field];
   if(![window.firstResponder isKindOfClass:NSTextView.class])runtime_fail("text field has no editor");
  }
  if([NSApp.mainMenu performKeyEquivalent:key(0)])runtime_fail("plain Q triggered Quit");
  if([NSApp.mainMenu performKeyEquivalent:key(NSEventModifierFlagControl)])runtime_fail("Control-Q triggered Quit");
  dispatch_async(dispatch_get_main_queue(),^{
   start=clock_ns();
   if(![NSApp.mainMenu performKeyEquivalent:key(NSEventModifierFlagCommand)])runtime_fail("Command-Q was not handled");
   /* Only the save-wait path returns. The close callback must exit the process. */
  });
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW,3*NSEC_PER_SEC),dispatch_get_main_queue(),^{runtime_fail("Quit did not exit");});
  [NSApp run];
  runtime_fail("event loop returned");
 }
}
