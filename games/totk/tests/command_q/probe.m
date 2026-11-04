/* Exercise the real AppKit menu and window-close handler without game data. */
#import <CoreGraphics/CoreGraphics.h>
#define TOTK_SCREENSHOT_WINDOW 1   /* frontend.m's capture button, tested below */
#define TOTK_SCREENSHOT_WRITE 0    /* no ImageIO in this build: the write below is the probe's own */
#define TOTK_SCREENSHOT_SAVE screenshot_picture_save
static void screenshot_picture(void (^cb)(CGImageRef img));   /* the renderer's frame: this probe's small image */
#define TOTK_SCREENSHOT_PICTURE screenshot_picture
#include "../../runtime/frontend.m"
#include <time.h>

/* The renderer's next frame, for screenshot.h: this probe has none, so each request is a small image of its own (one per
   press), handed over the way the renderer hands a frame over -- when it has one; the write says it saved the picture. */
static int screenshotsTaken, screenshotsSaved, deferPictures;
static NSMutableArray *deferredPictures;
static void screenshot_picture(void (^cb)(CGImageRef img)) {
    screenshotsTaken++;
    if (deferPictures) { if (!deferredPictures) deferredPictures = [NSMutableArray array]; [deferredPictures addObject:[cb copy]]; return; }
    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    CGContextRef context = CGBitmapContextCreate(NULL, 8, 8, 8, 32, space, (uint32_t)kCGImageAlphaPremultipliedLast);
    CGImageRef image = context ? CGBitmapContextCreateImage(context) : NULL;
    if (context) CGContextRelease(context);
    CGColorSpaceRelease(space);
    cb(image);
    if (image) CGImageRelease(image);   /* the save kept its own, as with the renderer's */
}
static int screenshot_picture_save(CGImageRef img, const char *game, char *path, size_t size, const char **why, int *problem,
                           char *album, size_t album_size) {
    (void)img; (void)game; (void)why; (void)problem; (void)album; (void)album_size;
    screenshotsSaved++;
    snprintf(path, size, "/tmp/probe-%d.png", screenshotsTaken);
    return 1;
}
static int mode;
static uint64_t start;
static uint64_t clock_ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec;}
uint64_t runtime_now_ns(void){return (clock_ns()-start)*(mode==2?100:1);}
int runtime_fs_save_busy(void){return mode==2||(mode==1&&runtime_now_ns()<200000000ull);}
_Noreturn void runtime_fail(const char *message){fprintf(stderr,"FAIL: %s\n",message);exit(1);}
static unsigned inputDown,inputUp,inputClears;
static unsigned lastInputCode;
static int heldInput[128];
void runtime_input_key(unsigned code,int down){lastInputCode=code;if(code<128)heldInput[code]=down;if(down)inputDown++;else inputUp++;}
void runtime_input_clear(void){}
void runtime_input_clear_window(void){inputClears++;memset(heldInput,0,sizeof heldInput);}
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
/* runtime/account.c's local users (the panel's User page): the empty store of a fresh profile. */
int runtime_users_get(RuntimeUser *out,int max){(void)out;(void)max;return 0;}
int runtime_users_active(RuntimeUser *out){if(out)*out=(RuntimeUser){0};return 0;}
int runtime_users_next(RuntimeUser *out){if(out)*out=(RuntimeUser){0};return 0;}
int runtime_users_add(const char *name){(void)name;return -1;}
int runtime_users_choose(int index){(void)index;return -1;}
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
int vr_enabled(void){return 0;}int vr_streamer_managed(void){return 0;}void vr_resolution(int *w,int *h){*w=*h=0;}int vr_set_resolution(int w,int h){(void)w;(void)h;return 0;}
int vr_first_person(void){return 0;}void vr_set_first_person(int on){(void)on;}
int vr_offset_cm(int a){(void)a;return 0;}void vr_set_offset_cm(int a,int cm){(void)a;(void)cm;}
int vr_mirror_mode(void){return 0;}void vr_set_mirror_mode(int on){(void)on;}void *vr_mirror_copy(void){return 0;}
void vr_panel_image(const void *p,int w,int h,int s){(void)p;(void)w;(void)h;(void)s;}
int vr_panel_in_headset(void){return 0;}
void vr_mirror_crop(double o[4]){o[0]=o[1]=0;o[2]=o[3]=1;}
double vr_mirror_aspect(void){return 2;}
int vr_quest_view(void){return 0;}void vr_set_quest_view(int on){(void)on;}int vr_quest_zoom(void){return 2;}void vr_set_quest_zoom(int x){(void)x;}
int vr_motion(void){return 0;}void vr_set_motion(int on){(void)on;}
int vr_6dof(void){return 0;}void vr_set_6dof(int on){(void)on;}int vr_eye_spring_ms(void){return 0;}void vr_set_eye_spring_ms(int ms){(void)ms;}int vr_afr(void){return 0;}void vr_set_afr(int on){(void)on;}int vr_ipd_pct(void){return 100;}void vr_set_ipd_pct(int p){(void)p;}int vr_snap_ms(void){return 0;}void vr_set_snap_ms(int ms){(void)ms;}
int runtime_camera_distance_available(void){return 0;}int runtime_camera_distance_cm(void){return 0;}void runtime_camera_distance_set(int cm){(void)cm;}
int vr_hide_head_mode(void){return -1;}void vr_set_hide_head_mode(int m){(void)m;}
#ifndef FRONTEND_REAL_GAMEPAD
void runtime_mouse_set_hook(void (*hook)(void)){(void)hook;}
#endif
#ifndef FRONTEND_REAL_GAMEPAD
void runtime_capture_set_hook(void (*hook)(void)){(void)hook;}
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
static int fixtureFrameLimit;
void nvn_fps_lock_set(int fps){fixtureFrameLimit=fps;}
/* TOTK_STATE_REPORTING (gfx/nvn/totk_state_report.m): frontend.m declares them weak, the linker still wants them. */
void totk_sr_start(const char *game,const char *version){(void)game;(void)version;}
void totk_sr_picture(int key,int visible){(void)key;(void)visible;}
void totk_sr_window(int key,int visible){(void)key;(void)visible;}
void totk_sr_view(int w,int h,int hz,int fullscreen){(void)w;(void)h;(void)hz;(void)fullscreen;}
void totk_sr_settings(int open){(void)open;}
void totk_sr_closing(void){fprintf(stderr,"[probe] state report: closing\n");}
int nvn_fps_lock(void){return fixtureFrameLimit;}
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
void runtime_cloud_save_quit(void){}   /* the window's close uploads a save not in the sync folder yet (runtime/cloud_save.m) */
int fps_overlay_value(void){return 0;}   /* the HUD's FPS graph (PR #940) */

static NSEvent *key(NSEventModifierFlags flags){
 return [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint modifierFlags:flags timestamp:0
     windowNumber:window.windowNumber context:nil characters:@"q" charactersIgnoringModifiers:@"q" isARepeat:NO keyCode:12];
}
/* Only this mode replaces window actions; the save/quit and live-menu probes below stay real. */
static unsigned closeCalls,fullCalls;
@interface CommandInputWindow:NSWindow
@end
@implementation CommandInputWindow
-(void)performClose:(id)sender{(void)sender;closeCalls++;}
-(void)toggleFullScreen:(id)sender{(void)sender;fullCalls++;}
@end
static NSEvent *input_event(unsigned short code,NSString *text,NSEventModifierFlags flags,NSEventType type){
 return [NSEvent keyEventWithType:type location:NSZeroPoint modifierFlags:flags timestamp:0
  windowNumber:window.windowNumber context:nil characters:text charactersIgnoringModifiers:text isARepeat:NO keyCode:code];
}
static void probe_command_input(void){
 TOTKInputView *v=(TOTKInputView *)window.contentView;
 [v keyDown:input_event(3,@"f",0,NSEventTypeKeyDown)];
 if(inputDown!=1||lastInputCode!=3||!heldInput[3])runtime_fail("plain F stopped reaching the game");
 [v keyDown:input_event(3,@"f",NSEventModifierFlagCommand,NSEventTypeKeyDown)];
 if(inputDown!=1||fullCalls)runtime_fail("Command-F became gameplay or fullscreen");
 [v keyDown:input_event(13,@"w",NSEventModifierFlagCommand,NSEventTypeKeyDown)];
 if(inputDown!=1||closeCalls)runtime_fail("unbound Command-W became gameplay or a close action");
 [v keyDown:input_event(3,@"f",NSEventModifierFlagControl|NSEventModifierFlagCommand,NSEventTypeKeyDown)];
 if(inputDown!=1||fullCalls!=1)runtime_fail("Control-Command-F lost the existing fullscreen action");
 settingsOpen=1;
 [v keyDown:input_event(3,@"f",NSEventModifierFlagControl|NSEventModifierFlagCommand,NSEventTypeKeyDown)];
 settingsOpen=0;textEntryOpen=1;
 [v keyDown:input_event(12,@"q",NSEventModifierFlagCommand,NSEventTypeKeyDown)];
 textEntryOpen=0;
 if(inputDown!=1||fullCalls!=2||closeCalls!=1)runtime_fail("settings/text-entry state swallowed a menu equivalent");
 [v keyDown:input_event(43,@",",NSEventModifierFlagCommand,NSEventTypeKeyDown)];
 if(!settingsOpen||inputDown!=1)runtime_fail("Command-comma lost the existing settings action");
 settings_show(0);
 [v keyDown:input_event(13,@"w",0,NSEventTypeKeyDown)];
 if(inputDown!=2||lastInputCode!=13||!heldInput[13])runtime_fail("plain W stopped reaching the game");
 [v flagsChanged:input_event(55,@"",NSEventModifierFlagCommand,NSEventTypeFlagsChanged)];
 if(inputClears!=1||heldInput[3]||heldInput[13])runtime_fail("Command left window inputs held");
 [v keyUp:input_event(13,@"w",NSEventModifierFlagCommand,NSEventTypeKeyUp)];
 if(inputUp!=1||lastInputCode!=13)runtime_fail("Command suppressed a key release");
 const unsigned short codes[]={56,60,59,62,58,61};
 const NSEventModifierFlags flags[]={NSEventModifierFlagShift,NSEventModifierFlagShift,NSEventModifierFlagControl,NSEventModifierFlagControl,NSEventModifierFlagOption,NSEventModifierFlagOption};
 for(unsigned i=0;i<6;i++){
  unsigned before=inputDown;
  [v flagsChanged:input_event(codes[i],@"",flags[i],NSEventTypeFlagsChanged)];
  if(inputDown!=before+1||lastInputCode!=codes[i]||!heldInput[codes[i]])runtime_fail("plain modifier stopped reaching the game");
  [v flagsChanged:input_event(codes[i],@"",flags[i]|NSEventModifierFlagCommand,NSEventTypeFlagsChanged)];
  if(inputDown!=before+1||heldInput[codes[i]])runtime_fail("Command modifier combination reached gameplay");
  unsigned beforeUp=inputUp;
  [v flagsChanged:input_event(codes[i],@"",0,NSEventTypeFlagsChanged)];
  if(inputUp!=beforeUp+1||lastInputCode!=codes[i])runtime_fail("plain modifier release was lost");
 }
 puts("PASS: Command input isolation, existing menu equivalents, held keys and ordinary modifiers");
}
static void screenshot_pump(double seconds) {
 [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:seconds]];
 if (shotWrites) dispatch_sync(shotWrites,^{});
}
static void probe_screenshot_generations(void) {
 start=clock_ns(); deferPictures=1;
 bootOverlay=[[NSView alloc] initWithFrame:window.contentView.bounds];bootOverlay.hidden=YES;
 screenshot_take(); uint64_t first=shotRequest;
 void (^old)(CGImageRef)=deferredPictures[0];
 screenshot_pump(3.1);
 if(shotPendingRequest)runtime_fail("screenshot timeout did not invalidate its request");
 old(NULL);screenshot_pump(.05);
 if(screenshotsSaved)runtime_fail("a late frame was saved after its request expired");
 screenshot_take();uint64_t second=shotRequest;
 void (^current)(CGImageRef)=deferredPictures[1];current(NULL);screenshot_pump(.05);
 if(screenshotsSaved!=1||second<=first)runtime_fail("second screenshot did not complete with a fresh generation");
 screenshot_take();uint64_t third=shotRequest;
 if(third<=second)runtime_fail("screenshot generation was reused after completion");
 current(NULL);screenshot_pump(.05);
 if(screenshotsSaved!=1||shotPendingRequest!=third)runtime_fail("an old callback stole a newer screenshot");
 void (^latest)(CGImageRef)=deferredPictures[2];latest(NULL);screenshot_pump(.05);
 if(screenshotsSaved!=2||shotPendingRequest)runtime_fail("latest screenshot did not finish independently");
 puts("PASS: screenshot expiry, late callbacks and nonreused generations");
}
int main(int argc,char **argv){
 @autoreleasepool{
  if(argc!=3)return 1;
  mode=atoi(argv[1]);
  [NSApplication sharedApplication];[NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
  window=[[(mode==4?CommandInputWindow.class:NSWindow.class) alloc] initWithContentRect:NSMakeRect(0,0,320,180)
      styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable backing:NSBackingStoreBuffered defer:NO];
  window.releasedWhenClosed=NO;
  windowDelegate=[TOTKWindowDelegate new];window.delegate=windowDelegate;
  window.contentView=[[TOTKInputView alloc] initWithFrame:window.contentView.bounds];
  [window makeFirstResponder:window.contentView];
  front_install_menu();
  [NSApp finishLaunching];
  if(mode==4){probe_command_input();return 0;}
  if(mode==5){probe_screenshot_generations();return 0;}
  if(atoi(argv[2])){
   NSTextField *field=[[NSTextField alloc] initWithFrame:NSMakeRect(10,10,200,25)];
   [window.contentView addSubview:field];[window makeFirstResponder:field];
   if(![window.firstResponder isKindOfClass:NSTextView.class])runtime_fail("text field has no editor");
  }
  if(mode==3){
   NSMenu *app=NSApp.mainMenu.itemArray[0].submenu;
   NSMenuItem *settings=app.itemArray[0];
   if(![settings.keyEquivalent isEqualToString:@","]||settings.action!=@selector(openSettings:))runtime_fail("native settings menu missing");
   [NSApp sendAction:settings.action to:settings.target from:settings];
   if(!settingsOpen)runtime_fail("native settings action did not open panel");
   settings_show(0);
   NSMenu *view=NSApp.mainMenu.itemArray[1].submenu;
   if(view.itemArray[0].action!=@selector(toggleFullscreen:) || (view.itemArray[0].keyEquivalentModifierMask!=(NSEventModifierFlagControl|NSEventModifierFlagCommand) && view.itemArray[0].keyEquivalentModifierMask!=NSEventModifierFlagFunction))runtime_fail("native full screen action missing");
   NSMenu *rates=view.itemArray[1].submenu;
   for(int i=0;i<2;i++)for(NSMenuItem *item in rates.itemArray)if(item.tag==(i?120:60)){
    [NSApp sendAction:item.action to:item.target from:item];
    if(nvn_fps_lock()!=(i?120:60))runtime_fail("native frame limit did not apply");
   }
   puts("PASS: native Settings, Full Screen and live 60-to-120 menu actions");return 0;
  }
  if([NSApp.mainMenu performKeyEquivalent:key(0)])runtime_fail("plain Q triggered Quit");
  if([NSApp.mainMenu performKeyEquivalent:key(NSEventModifierFlagControl)])runtime_fail("Control-Q triggered Quit");
  /* The capture button (screenshot.h): nothing while the game shows no picture, exactly one picture once it does. */
  if(!bootOverlay){bootOverlay=[[NSView alloc] initWithFrame:window.contentView.bounds];[window.contentView addSubview:bootOverlay];}
  bootOverlay.hidden=NO;
  screenshot_request();[NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
  if(screenshotsTaken!=0)runtime_fail("a screenshot was taken before the game showed a picture");
  bootOverlay.hidden=YES;
  screenshot_request();[NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];
  if(screenshotsTaken!=1)runtime_fail("the capture button did not take exactly one screenshot");
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
