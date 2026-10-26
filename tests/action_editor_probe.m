/* Exercise the real AppKit menu and window-close handler without game data. */
#include "../games/totk/runtime/frontend.m"
#include <time.h>

static int mode;
static uint64_t start;
static uint64_t clock_ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec;}
uint64_t runtime_now_ns(void){return (clock_ns()-start)*(mode==2?100:1);}
int runtime_fs_save_busy(void){return mode==2||(mode==1&&runtime_now_ns()<200000000ull);}
_Noreturn void runtime_fail(const char *message){fprintf(stderr,"FAIL: %s\n",message);exit(1);}
void runtime_input_key(unsigned code,int down){(void)code;(void)down;}
void runtime_input_clear(void){}
static int suspended;void runtime_input_suspend(int on){suspended=on;}
void runtime_keyboard_layout_set(const uint8_t keys[CONTROLS_KEYS]){(void)keys;}
void runtime_controller_layout_set(const ControlsLayout *layout){(void)layout;}
void runtime_gamepad_start(void){}
int runtime_gamepad_type(int *detected){if(detected)*detected=-1;return 0;}
int runtime_mouse_enabled(void){return 0;}
void runtime_mouse_set_hook(void (*hook)(void)){(void)hook;}
void runtime_mouse_move(double dx,double dy){(void)dx;(void)dy;}
void runtime_mouse_button(int button,int down){(void)button;(void)down;}
void runtime_mouse_release(void){}
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
int runtime_output_modes(void){return 0;}
int runtime_output_mode(void){return 0;}
int runtime_output_mode_set(int mode){(void)mode;return 0;}
int runtime_aspect_set_ratio(float r){(void)r;return 0;}
float runtime_aspect_ratio(void){return 0;}
float runtime_aspect_start(void){return 0;}
int runtime_output_native(int mode,unsigned *w,unsigned *h){(void)mode;(void)w;(void)h;return 0;}
int runtime_picture_canvas_max(unsigned *w,unsigned *h){(void)w;(void)h;return 0;}
int runtime_picture_presets(void){return 0;}
int runtime_picture_preset(void){return 0;}
int runtime_picture_set(unsigned w,unsigned h,int preset){(void)w;(void)h;(void)preset;return 0;}
uint64_t runtime_output_size_current(void){return (uint64_t)1080<<32|1920;}
int dm_gfx_vsync(void){return 1;}
int runtime_gamepad_type_set(int type){return type;}
int runtime_mouse_set(int on){return on;}
void nvn_fps_lock_set(int fps){(void)fps;}
int nvn_fps_lock(void){return 0;}
/* The settings panel's rumble, gyro and trigger rows (gamepad_feedback.m, hid_feedback.c). */
int runtime_feedback_present(int gyro){(void)gyro;return 0;}
int runtime_feedback_choice(int gyro){(void)gyro;return 0;}
int runtime_feedback_locked(int gyro){(void)gyro;return 0;}
void runtime_feedback_choose(int gyro,int on){(void)gyro;(void)on;}
float runtime_rumble_gain(void){return 1;}
int runtime_rumble_gain_locked(void){return 0;}
void runtime_rumble_gain_set(float gain){(void)gain;}
int runtime_triggers_available(void){return 0;}
int runtime_triggers_choice(void){return 0;}
int runtime_triggers_locked(void){return 0;}
void runtime_triggers_choose(int on){(void)on;}
#include "../games/totk/runtime/action_runtime.h"
#include <assert.h>

int main(int argc,char **argv){@autoreleasepool{
 assert(argc==2);NSString *folder=@(argv[1]);NSString *controls=[folder stringByAppendingPathComponent:@"controls.txt"];setenv("TOTK_CONTROLS",controls.fileSystemRepresentation,1);
 RuntimeActionBindings b;runtime_action_bindings_get(&b);assert(!runtime_action_bindings_enabled());
 ActionLive live={0};runtime_action_bindings_attach(&live);assert(runtime_action_bindings_available());
 [NSApplication sharedApplication];[NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];[NSApp finishLaunching];
 window=[[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,900,700) styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];window.releasedWhenClosed=NO;window.contentView=[TOTKInputView new];front_install_menu();
 TOTKActionEditor *editor=actionEditor;[editor open:nil];assert(editor.panel&&editor.targets.count==16&&suspended);
 editor->draft.target[0]=ACTION_JUMP;[editor apply:nil];[NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];
 assert(!editor.panel&&!suspended);assert(atomic_load(&live.target[0])==ACTION_JUMP);
 NSString *path=[folder stringByAppendingPathComponent:@"player-actions.txt"];NSString *saved=[NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:NULL];assert([saved hasPrefix:@"4,"]);
 [editor open:nil];[editor reset:nil];[editor cancel:nil];[NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];
 assert([[NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:NULL] isEqualToString:saved]);
 [editor open:nil];assert([@"external" writeToFile:path atomically:YES encoding:NSUTF8StringEncoding error:NULL]);[editor apply:nil];assert(editor.panel&&[editor.message.stringValue length]);[editor cancel:nil];[NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];
 assert([saved writeToFile:path atomically:YES encoding:NSUTF8StringEncoding error:NULL]);
 [editor open:nil];runtime_action_bindings_attach(NULL);[editor apply:nil];assert(editor.panel);assert([[NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:NULL] isEqualToString:saved]);[editor cancel:nil];
 puts("PASS: 16-source action editor, live publication, save, cancel/reset, conflict and failed-install refusal");return 0;
}}
