/* Actual settings view, shortcut latch and gamepad routing; no game files or player settings. */
#define FRONTEND_REAL_GAMEPAD
#define main command_q_main
#include "../command_q/probe.m"
#undef main
#include "../../runtime/gamepad.m"
#include <assert.h>
void runtime_feedback_attach(GCController *c){(void)c;}
void runtime_feedback_detach(GCController *c){(void)c;}
void runtime_feedback_quiet(void){}
int runtime_gyro_enabled(void){return 0;}
void runtime_motion_mouse(int on){(void)on;}
void runtime_motion_feed_mouse(const double a[3],const double g[3],uint64_t n){(void)a;(void)g;(void)n;}
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static RuntimeGamepadState pad(uint64_t b){return (RuntimeGamepadState){.connected=1,.buttons=b};}
static double instant;
static void press(uint64_t b){[settingsPanel controllerPad:pad(0) time:instant+=1 active:YES];[settingsPanel controllerPad:pad(b) time:instant+=0.1 active:YES];}
@interface SettingsPreviewDelegate:NSObject<NSWindowDelegate>
@end
@implementation SettingsPreviewDelegate
-(void)windowWillClose:(NSNotification *)n{(void)n;[NSApp stop:nil];}
@end
int main(int argc,char **argv){@autoreleasepool{
 if(argc<2)return 2;start=clock_ns();
 NSString *controls=[@(argv[1]) stringByAppendingPathComponent:@"controls.txt"];setenv("TOTK_CONTROLS",controls.UTF8String,1);
 [NSApplication sharedApplication];controller_map_default(controllerMap,controllerModes);
 window=[[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,1000,760) styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskResizable backing:NSBackingStoreBuffered defer:NO];
 window.releasedWhenClosed=NO;window.contentView=[[TOTKInputView alloc] initWithFrame:window.contentView.bounds];
 /* The two buttons the panel opens on, each on its own: the DualSense touchpad (physical 16) and the share button
    (physical 22: Xbox Share, Switch Capture). A second opens it, less does nothing, once per hold, and a layout that
    gives either a Switch button keeps that button and no panel. mode 2 runs runtime_now_ns 100x fast (command_q probe),
    so the second is 10 ms here. */
 mode=2;hidPad=(HIDPadState){.present=1};runtime_gamepad_settings_enable(1);
 RuntimeGamepadState out;
 for (int i = 0; i < 2; i++) {
  uint64_t key = i ? SETTINGS_SHARE : SETTINGS_TOUCHPAD;
  int physical = i ? CONTROLLER_SHARE : CONTROLLER_TOUCHPAD;
  /* A chord that is not the shortcut any more: ZL + ZR + L3 in the game, nothing in the panel. */
  hidPad.buttons=(1ull<<8)|(1ull<<9)|(1ull<<4);refresh_gamepads();usleep(15000);
  CHECK(!runtime_gamepad_settings_requested());
  runtime_gamepad_read(&out);CHECK(out.buttons==((1ull<<4)|(1ull<<8)|(1ull<<9))&&textCapture==0);   /* the game still gets ZL + ZR + L3 as ZL + ZR + L3 */
  hidPad.buttons=key;refresh_gamepads();CHECK(!runtime_gamepad_settings_requested());   /* not before the second */
  runtime_gamepad_read(&out);CHECK(!out.buttons&&textCapture==0);   /* nothing of the game's is taken away */
  usleep(15000);CHECK(runtime_gamepad_settings_requested());CHECK(!runtime_gamepad_settings_requested());
  usleep(15000);refresh_gamepads();CHECK(!runtime_gamepad_settings_requested());   /* once per hold */
  hidPad.buttons=0;refresh_gamepads();usleep(15000);
  hidPad.buttons=key;refresh_gamepads();usleep(4000);hidPad.buttons=0;refresh_gamepads();usleep(15000);
  CHECK(!runtime_gamepad_settings_requested());   /* let go before the second: nothing */
  controllerMap[physical]=1u<<10;hidPad.buttons=key;refresh_gamepads();usleep(15000);
  CHECK(!runtime_gamepad_settings_requested());   /* mapped to +: the button plays, no panel */
  controllerMap[physical]=0;hidPad.buttons=0;refresh_gamepads();usleep(15000);
  runtime_gamepad_read(&out);   /* take the + edge out of the way for the next button */
 }
 runtime_gamepad_settings_enable(0);hidPad.buttons=0;refresh_gamepads();hidPad.buttons=SETTINGS_SHARE;refresh_gamepads();usleep(15000);
 CHECK(!runtime_gamepad_settings_requested());
 mode=0;
 hidPad=(HIDPadState){0};runtime_gamepad_clear();
 settings_controller_tick(pad(0),NO,YES,0);settings_controller_tick(pad(SETTINGS_TOUCHPAD),YES,YES,0.1);CHECK(settingsOpen&&textCapture==1);
 CHECK(settingsPanel.controllerTargets.count==settingsPanel.rows.count+2);
 /* Replace row setters with test-local values; changes must use each row's original control action. */
 __block int choice=0,toggle=0;
 TOTKSettingRow *r=settingsPanel.rows[0];r.choices=@[@"Off",@"30",@"60"];r.values=@[@0,@30,@60];r.get=^int{return choice;};r.set=^(int v){choice=v;};
 NSPopUpButton *popup=(NSPopUpButton *)r.control;[popup removeAllItems];[popup addItemsWithTitles:r.choices];[r refresh];
 settingsPanel.rows[1].set=nil;settingsPanel.rows[1].control.enabled=NO;
 /* The first switch past row 1, found by kind: rows move when the panel gains one (PR #940's FPS graph). */
 NSUInteger sw=NSNotFound;for(NSUInteger i=2;i<settingsPanel.rows.count&&sw==NSNotFound;i++)if(!settingsPanel.rows[i].choices&&settingsPanel.rows[i].control.enabled)sw=i;
 CHECK(sw!=NSNotFound);
 TOTKSettingRow *s=settingsPanel.rows[sw];s.get=^int{return toggle;};s.set=^(int v){toggle=v;};
 settingsPanel.controllerIndex=0;[settingsPanel controllerHighlight];
 [settingsPanel controllerPad:pad(1) time:0.2 active:YES];CHECK(choice==0); /* opening input must be released */
 press(1ull<<14);CHECK(choice==30);press(1ull<<12);CHECK(choice==0);
 press(1ull<<15);CHECK(settingsPanel.controllerIndex!=1&&settingsPanel.controllerIndex>1);
 settingsPanel.controllerIndex=sw;press(1ull<<14);CHECK(toggle==1);press(1ull<<14);CHECK(toggle==1);press(1ull<<12);CHECK(toggle==0);
 for(int family=GAMEPAD_TYPE_XBOX;family<=GAMEPAD_TYPE_SWITCH;family++){
  runtime_gamepad_type_set(family);settingsPanel.controllerIndex=sw;toggle=0;
  settingsPanel.rows[sw].get=^int{return toggle;};settingsPanel.rows[sw].set=^(int v){toggle=v;};
  press(family==GAMEPAD_TYPE_SWITCH?2:1);CHECK(toggle==1);
  press(family==GAMEPAD_TYPE_SWITCH?1:2);CHECK(!settingsOpen&&textCapture==2);
  settings_show(1);CHECK(settingsOpen);
 }
 [settingsPanel controllerPad:pad(0) time:2 active:NO];[settingsPanel controllerPad:pad(1ull<<15) time:3 active:YES];CHECK(!settingsPanel.controllerArmed);
 [settingsPanel controllerPad:pad(0) time:4 active:YES];CHECK(settingsPanel.controllerArmed);
 settingsPanel.controllerIndex=0;[settingsPanel controllerPad:pad(1ull<<15) time:5 active:YES];NSInteger first=settingsPanel.controllerIndex;
 [settingsPanel controllerPad:pad(1ull<<15) time:5.1 active:YES];CHECK(settingsPanel.controllerIndex==first);
 [settingsPanel controllerPad:pad(1ull<<15) time:5.5 active:YES];CHECK(settingsPanel.controllerIndex!=first);
 settings_show(0);textEntryOpen=1;settings_show(1);CHECK(!settingsOpen);
 settings_controller_tick(pad(SETTINGS_TOUCHPAD),YES,YES,6);CHECK(!settingsOpen);textEntryOpen=0;
 settings_controller_tick(pad(SETTINGS_TOUCHPAD),YES,YES,7);CHECK(!settingsOpen);
 settings_controller_tick(pad(0),NO,YES,8);settings_controller_tick(pad(SETTINGS_TOUCHPAD),YES,YES,9);CHECK(settingsOpen);
 settings_controller_tick(pad(0),NO,YES,10);settings_controller_tick(pad(SETTINGS_TOUCHPAD),YES,YES,11);CHECK(!settingsOpen);
 printf("pass: controller settings (%u checks)\n",checks);
 if(argc>2){
  window.title=@"Controller Settings Preview — no game settings changed";
  runtime_gamepad_clear();runtime_gamepad_type_set(!strcmp(argv[2],"--preview")?GAMEPAD_TYPE_AUTO:GAMEPAD_TYPE_PLAYSTATION);
  settings_show(1);[window center];[window.contentView layoutSubtreeIfNeeded];
  if(strcmp(argv[2],"--preview")){
   NSView *v=window.contentView;NSBitmapImageRep *rep=[v bitmapImageRepForCachingDisplayInRect:v.bounds];[v cacheDisplayInRect:v.bounds toBitmapImageRep:rep];
   CHECK([[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:@(argv[2]) atomically:YES]);
  }else{
   SettingsPreviewDelegate *delegate=[SettingsPreviewDelegate new];window.delegate=delegate;
   [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];[window makeKeyAndOrderFront:nil];[NSApp activateIgnoringOtherApps:YES];
   dispatch_async(dispatch_get_main_queue(),^{runtime_gamepad_start();settings_controller_start();});[NSApp run];
  }
 }
 [settingsPanel.timer invalidate];[settingsControllerTimer invalidate];runtime_gamepad_settings_enable(0);return 0;
}}
