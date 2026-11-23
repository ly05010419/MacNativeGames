/* Native macOS window and VI service adapter. No guest frame is synthesized. */
#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>
#import <CoreVideo/CoreVideo.h>
#import <IOKit/pwr_mgt/IOPMLib.h>
#include <stdatomic.h>
#include <math.h>
#include "runtime.h"
#include "game_version.h"
#import "mng_active.h"   /* the day's report of active players (on unless turned off in the Launcher's settings) */
#include "totk_ui_lang.h"
#include "gamepad_layout.h"
#import "playtime.h"
#include "../third_party/d2r_gfx/gfx/nvn/totk_mfx_mode.h"
#include "../third_party/d2r_gfx/gfx/nvn/fps_overlay.h"
int mfxup_output_enabled(void);
static NSWindow *window;
uint64_t dm_nvn_frame_count(void);   /* gfx/nvn/hle_nvn.c: game frames presented so far */
static CAMetalLayer *metalLayer;
static NSView *bootOverlay;
static NSTextField *bootStage,*bootDetail;
static NSProgressIndicator *bootProgress;
static NSView *hudBox;   /* the FPS / shader / load box (TOTKHudBox), nil with TOTK_HUD=corners */
static NSView *fpsGraphBox;
static int fpsGraphEnabled;
/* The settings panel (key 1, settings_panel.h): while it is open the game gets no keyboard or mouse input. */
static int settingsOpen;
static void settings_show(int open);
/* Mouse mode (the settings panel, runtime/gamepad.m): while it is on and the window has the keyboard, the window holds the
   pointer -- moved to the window's centre, detached from the mouse and hidden -- and sends the mouse's buttons and
   movement to gamepad.m. It lets go when the window loses the keyboard (Command-Tab, a click elsewhere), during text
   entry, while the settings panel is open and when mouse mode goes off (front_mouse_update). */
static BOOL mouseHeld;
static int textEntryOpen;
static void front_mouse_update(void);
int taa_enabled(void);   /* third_party/d2r_gfx/gfx/nvn/totk_taa.m: edge smoothing in the settings panel */
void taa_set(int on);
static uint64_t bootStartNS;
static _Atomic uint64_t presentedFrames;
static _Atomic int warmPending;
/* Counts foreground time after warm-up, including menus, like Screen Time.
   Diagnostic profiles have their own counter and never update the user's file. */
static TOTKPlaytime *playtime;
static NSTimer *playtimeTimer;
static BOOL playtimeSleeping;
static void front_playtime_tick(BOOL stopping) {
 if(!playtime)return;
 BOOL running=!stopping&&!playtimeSleeping&&NSApp.isActive&&window.isKeyWindow
  &&!window.miniaturized&&atomic_load(&presentedFrames)&&!atomic_load(&warmPending);
 [playtime tick:runtime_now_ns()/1e9 active:running];
 if(stopping)[playtime save];
}
static void front_playtime_start(void) {
 const char *profile=getenv("TOTK_PROFILE_DIR");
 NSString *dir=[NSString stringWithUTF8String:profile?profile:"user-data"];
 playtime=[[TOTKPlaytime alloc] initWithPath:[dir stringByAppendingPathComponent:@"playtime.json"]];
 /* Keep counting independent of HUD visibility, the legacy corner layout and its refresh timer. */
 playtimeTimer=[NSTimer timerWithTimeInterval:0.5 repeats:YES block:^(NSTimer *t){(void)t;front_playtime_tick(NO);}];
 [NSRunLoop.mainRunLoop addTimer:playtimeTimer forMode:NSRunLoopCommonModes];
 NSNotificationCenter *nc=NSWorkspace.sharedWorkspace.notificationCenter;
 for(NSString *name in @[NSWorkspaceWillSleepNotification,NSWorkspaceScreensDidSleepNotification,NSWorkspaceSessionDidResignActiveNotification])
  [nc addObserverForName:name object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n){
   (void)n;front_playtime_tick(YES);playtimeSleeping=YES;
  }];
 for(NSString *name in @[NSWorkspaceDidWakeNotification,NSWorkspaceScreensDidWakeNotification,NSWorkspaceSessionDidBecomeActiveNotification])
  [nc addObserverForName:name object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n){
   (void)n;playtimeSleeping=NO;[playtime tick:runtime_now_ns()/1e9 active:NO];
  }];
}
static CVDisplayLinkRef displayLink;
static _Atomic int focused;
/* Refresh rate of the window's screen, sampled on the main thread by front_sync_drawable. 0 until its first call, which
   runtime_front_run makes when the window opens, before the game can present; render threads read it (mtl_display_hz). */
static _Atomic int frontDisplayHz;
int runtime_front_display_hz(void){return atomic_load(&frontDisplayHz);}
static pthread_mutex_t vsyncLock=PTHREAD_MUTEX_INITIALIZER;
static void *vsyncEvents[16];
static unsigned vsyncCount;
void hle_bind_system_event(void *,int);
void hle_signal_system_event(void *);
static void front_update_awake(void);
static int window_closing;
int runtime_front_window_visible(void);   /* below: 1 seen, 0 covered, -1 not known yet */
void *front_metal_layer(void){return (__bridge void *)metalLayer;}
/* The start-up screen goes once a frame was presented and the warm-up is done: the player sees the game from here on. */
static void boot_overlay_hide(void){
 if(!bootOverlay||bootOverlay.hidden)return;
 bootOverlay.hidden=YES;
}
void front_frame_presented(void){
 if(atomic_fetch_add_explicit(&presentedFrames,1,memory_order_relaxed)==0){
  fprintf(stderr,"[totk-boot] %.3f first presented frame\n",(runtime_now_ns()-bootStartNS)/1e9);
  dispatch_async(dispatch_get_main_queue(), ^{if(!atomic_load(&warmPending))boot_overlay_hide();});
 }
}
void runtime_boot_progress(const char *stage,uint64_t done,uint64_t total){
 static const char *last;
 if(stage!=last){fprintf(stderr,"[totk-boot] %.3f %s\n",bootStartNS?(runtime_now_ns()-bootStartNS)/1e9:0,stage?stage:"startup");last=stage;}
 NSString *s=[NSString stringWithUTF8String:stage?stage:TOTK_UI("正在启动","Starting")];
 dispatch_async(dispatch_get_main_queue(), ^{
  if(!bootOverlay||bootOverlay.hidden)return;
  bootStage.stringValue=s;
  if(total){bootDetail.stringValue=[NSString stringWithFormat:@"%.1f / %.1f GB",done/1e9,total/1e9];
   bootProgress.indeterminate=NO;bootProgress.doubleValue=(double)done/(double)total*100.0;}
  else {bootDetail.stringValue=@(TOTK_UI("正在处理，请稍候","Working, please wait"));bootProgress.indeterminate=YES;[bootProgress startAnimation:nil];}
 });
}
void runtime_boot_count(const char *stage,uint64_t done,uint64_t total){
 /* Steps counted in items, not bytes (the shader import: tens of thousands of files, minutes on a phone). */
 static const char *last;
 if(stage!=last){fprintf(stderr,"[totk-boot] %.3f %s\n",bootStartNS?(runtime_now_ns()-bootStartNS)/1e9:0,stage?stage:"startup");last=stage;}
 NSString *s=[NSString stringWithUTF8String:stage?stage:TOTK_UI("正在启动","Starting")];
 dispatch_async(dispatch_get_main_queue(), ^{
  if(!bootOverlay||bootOverlay.hidden)return;
  bootStage.stringValue=s;bootDetail.stringValue=[NSString stringWithFormat:@"%llu / %llu",(unsigned long long)done,(unsigned long long)total];
  if(total){bootProgress.indeterminate=NO;bootProgress.doubleValue=(double)done/(double)total*100.0;}
 });
}
void runtime_boot_status(const char *stage,const char *detail){
 if(bootStartNS)fprintf(stderr,"[totk-boot] %.3f %s — %s\n",(runtime_now_ns()-bootStartNS)/1e9,stage?stage:"startup",detail?detail:"");
 NSString *s=[NSString stringWithUTF8String:stage?stage:TOTK_UI("正在启动","Starting")];
 NSString *d=[NSString stringWithUTF8String:detail?detail:TOTK_UI("正在处理，请稍候","Working, please wait")];
 dispatch_async(dispatch_get_main_queue(), ^{
  if(!bootOverlay||bootOverlay.hidden)return;
  bootStage.stringValue=s;bootDetail.stringValue=d;
  bootProgress.indeterminate=YES;[bootProgress startAnimation:nil];
 });
}
void runtime_warm_progress(uint64_t done,uint64_t total,uint64_t shaders,uint64_t shader_total,uint64_t pipelines,uint64_t pipeline_total){
 atomic_store(&warmPending,1);
 dispatch_async(dispatch_get_main_queue(), ^{
  if(!bootOverlay||bootOverlay.hidden)return;
  double percent=total ? fmin(100.0,(double)done/(double)total*100.0) : 100.0;
  bootStage.stringValue=[NSString stringWithFormat:@(TOTK_UI("预热着色器与管线 %.0f%%","Warming shaders and pipelines %.0f%%")),floor(percent)];
  bootDetail.stringValue=[NSString stringWithFormat:@(TOTK_UI("着色器 %llu/%llu · 管线 %llu/%llu","shaders %llu/%llu · pipelines %llu/%llu")),
      (unsigned long long)shaders,(unsigned long long)shader_total,
      (unsigned long long)pipelines,(unsigned long long)pipeline_total];
  bootProgress.indeterminate=NO;
  bootProgress.doubleValue=percent;
 });
}
void runtime_warm_done(void){
 atomic_store(&warmPending,0);
 dispatch_async(dispatch_get_main_queue(), ^{if(atomic_load(&presentedFrames))boot_overlay_hide();});
}
uint64_t front_game_res(int docked){(void)docked;return (720ULL<<32)|1280;}
/* TOTK_ASSUME_FOCUS=1: unattended test runs. macOS refuses to activate an app launched from the background while the user is working in
   another one; the window then never becomes key, GetCurrentFocusState stays 0 and the game waits before its first frame. In this mode the
   game is told it is focused, and the window is shown without taking the keyboard from the user (scripted input does not need it). */
static int assume_focus(void){static int v=-1;if(v<0){const char *e=getenv("TOTK_ASSUME_FOCUS");v=e&&atoi(e);}return v;}
/* Keep the guest active when the macOS window loses key status. Native input is still cleared
   in windowDidResignKey so a held key cannot remain stuck while another app has focus. */
int runtime_front_focus(void){return 1;}
/* ODYSSEY_VSYNC_DIVIDE=N (task B1, measurement only; unset = 1, as before): signal the game's vsync events on every Nth
   display-link callback, e.g. 2 gives 60/s on a 120 Hz panel, to test whether game time depends on the vsync rate. */
static unsigned vsync_divide(void){static unsigned v;if(!v){const char *e=getenv("ODYSSEY_VSYNC_DIVIDE");int n=e?atoi(e):1;v=n>1&&n<=8?(unsigned)n:1;}return v;}
static CVReturn vsync_callback(CVDisplayLinkRef link,const CVTimeStamp *now,const CVTimeStamp *output,CVOptionFlags flags,CVOptionFlags *out,void *ctx){
 (void)link;(void)now;(void)output;(void)flags;(void)out;(void)ctx;
 static unsigned tick;if(vsync_divide()>1&&tick++%vsync_divide())return kCVReturnSuccess;
 pthread_mutex_lock(&vsyncLock);for(unsigned i=0;i<vsyncCount;i++)hle_signal_system_event(vsyncEvents[i]);pthread_mutex_unlock(&vsyncLock);return kCVReturnSuccess;
}
/* 1 / 0: the modifier key e (a flag change) is down / up; -1: not one the layout takes (Command, Caps Lock, fn). Each side
   has its own device bit; a flag change without them (a synthesized event) goes by the shared flag. */
static int front_modifier_down(NSEvent *e){
 NSEventModifierFlags f=e.modifierFlags,shared;unsigned long side,pair;
 switch(e.keyCode){
  case 56:shared=NSEventModifierFlagShift;side=0x2;pair=0x6;break;case 60:shared=NSEventModifierFlagShift;side=0x4;pair=0x6;break;
  case 59:shared=NSEventModifierFlagControl;side=0x1;pair=0x2001;break;case 62:shared=NSEventModifierFlagControl;side=0x2000;pair=0x2001;break;
  case 58:shared=NSEventModifierFlagOption;side=0x20;pair=0x60;break;case 61:shared=NSEventModifierFlagOption;side=0x40;pair=0x60;break;
  default:return -1;}
 if(!(f&shared))return 0;
 return f&pair?!!(f&side):1;
}
@interface TOTKInputView:NSView
@property(strong) NSTimer *cursorHideTimer;
@property BOOL cursorAutoHideActive;
@property BOOL cursorIsHidden;
-(void)setCursorAutoHideEnabled:(BOOL)enabled;
@end
@implementation TOTKInputView
-(BOOL)acceptsFirstResponder{return YES;}
/* While the settings panel is open only key 1 (it closes the panel, runtime/input.c) and Esc (closes it here, not the game's
   menu) do anything; releases still go through so nothing stays held. */
-(void)keyDown:(NSEvent *)e{if(textEntryOpen)return;if(settingsOpen&&e.keyCode!=18){if(e.keyCode==53)settings_show(0);return;}runtime_input_key(e.keyCode,1);}
-(void)keyUp:(NSEvent *)e{runtime_input_key(e.keyCode,0);}
/* Shift, Control and Option, left and right apart, are keys of the layout (keyboard_map.h); they come as flag changes.
   While the settings panel or text entry is open only their releases go through. */
-(void)flagsChanged:(NSEvent *)e{int down=front_modifier_down(e);if(down>=0&&(!(settingsOpen||textEntryOpen)||!down))runtime_input_key(e.keyCode,down);}
/* The game has no mouse controls. In native full screen, keep the pointer out of the picture while idle, but show it again
   as soon as it moves; restore it when the window loses focus or leaves full screen. */
-(void)hideCursorIfIdle:(NSTimer *)timer{
 (void)timer;self.cursorHideTimer=nil;
 if(!self.cursorAutoHideActive||!self.window.isKeyWindow||!(self.window.styleMask&NSWindowStyleMaskFullScreen))return;
 if(!self.cursorIsHidden){[NSCursor hide];self.cursorIsHidden=YES;}
}
-(void)mouseMoved:(NSEvent *)e{
 if(mouseHeld){runtime_mouse_move(e.deltaX,e.deltaY);return;}
 if(!self.cursorAutoHideActive)return;
 [self.cursorHideTimer invalidate];self.cursorHideTimer=nil;
 if(self.cursorIsHidden){[NSCursor unhide];self.cursorIsHidden=NO;}
 self.cursorHideTimer=[NSTimer scheduledTimerWithTimeInterval:1.5 target:self selector:@selector(hideCursorIfIdle:) userInfo:nil repeats:NO];
}
-(void)mouseDragged:(NSEvent *)e{if(mouseHeld)runtime_mouse_move(e.deltaX,e.deltaY);}
-(void)rightMouseDragged:(NSEvent *)e{if(mouseHeld)runtime_mouse_move(e.deltaX,e.deltaY);}
-(void)otherMouseDragged:(NSEvent *)e{if(mouseHeld)runtime_mouse_move(e.deltaX,e.deltaY);}
-(void)mouseDown:(NSEvent *)e{(void)e;if(mouseHeld)runtime_mouse_button(0,1);}
-(void)mouseUp:(NSEvent *)e{(void)e;runtime_mouse_button(0,0);}
-(void)rightMouseDown:(NSEvent *)e{(void)e;if(mouseHeld)runtime_mouse_button(1,1);}   /* no context menu */
-(void)rightMouseUp:(NSEvent *)e{(void)e;runtime_mouse_button(1,0);}
-(void)otherMouseDown:(NSEvent *)e{if(mouseHeld)runtime_mouse_button((int)e.buttonNumber,1);}   /* 2 middle, 3 and 4 the side buttons */
-(void)otherMouseUp:(NSEvent *)e{runtime_mouse_button((int)e.buttonNumber,0);}
-(void)setCursorAutoHideEnabled:(BOOL)enabled{
 self.cursorAutoHideActive=enabled;
 [self.cursorHideTimer invalidate];self.cursorHideTimer=nil;
 if(enabled){
  self.window.acceptsMouseMovedEvents=YES;
  self.cursorHideTimer=[NSTimer scheduledTimerWithTimeInterval:1.5 target:self selector:@selector(hideCursorIfIdle:) userInfo:nil repeats:NO];
 }else{
  if(self.cursorIsHidden){[NSCursor unhide];self.cursorIsHidden=NO;}
  self.window.acceptsMouseMovedEvents=mouseHeld;
 }
}
@end
static void front_mouse_update(void){   /* main thread; also runtime/gamepad.m's hook when mouse mode changes */
 BOOL want=window&&runtime_mouse_enabled()&&window.isKeyWindow&&!textEntryOpen&&!settingsOpen;
 if(want==mouseHeld)return;
 mouseHeld=want;
 if(want){
  NSRect f=[window convertRectToScreen:window.contentView.frame];CGFloat top=NSMaxY(NSScreen.screens.firstObject.frame);
  CGWarpMouseCursorPosition(CGPointMake(NSMidX(f),top-NSMidY(f)));   /* clicks land in the window */
  CGAssociateMouseAndMouseCursorPosition(false);[NSCursor hide];window.acceptsMouseMovedEvents=YES;
 }else{
  CGAssociateMouseAndMouseCursorPosition(true);[NSCursor unhide];runtime_mouse_release();
  window.acceptsMouseMovedEvents=NO;
 }
 fprintf(stderr,"[totk] mouse %s\n",want?"held by the window (the settings panel, key 1, or Command-Tab lets it go)":"let go");
}
#include "hud_box.h"   /* shared/runtime/hud_box.h: the corner box and the FPS graph */
#include "output_resolution.h"
static int frontOutputResolution = -1;
static void front_sync_drawable(void);
/* The current display mode's physical pixels, including Retina; NSScreen.frame is in logical points. */
static CGSize front_native_resolution(void) {
 NSScreen *screen=window.screen?:NSScreen.mainScreen;if(!screen)return CGSizeZero;
 CGDirectDisplayID display=[screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue];
 CGDisplayModeRef mode=CGDisplayCopyDisplayMode(display);
 CGSize size=CGSizeMake(screen.frame.size.width*screen.backingScaleFactor,screen.frame.size.height*screen.backingScaleFactor);
 if(mode){size=CGSizeMake(CGDisplayModeGetPixelWidth(mode),CGDisplayModeGetPixelHeight(mode));CGDisplayModeRelease(mode);}
 if((size.width>size.height)!=(screen.frame.size.width>screen.frame.size.height)){CGFloat swap=size.width;size.width=size.height;size.height=swap;}
 return size;
}
static int front_output_resolution(void) {
 if(frontOutputResolution<0){char word[64]={0};frontOutputResolution=runtime_setting_word("output-resolution.txt",word,sizeof word)?output_resolution_parse(word):OUTPUT_RESOLUTION_NATIVE;}
 return frontOutputResolution;
}
static void front_output_resolution_set(int choice) {
 if(choice<0||(unsigned)choice>=OUTPUT_RESOLUTION_COUNT)return;
 frontOutputResolution=choice;
 runtime_setting_save("output-resolution.txt",output_resolutions[choice].word,"output resolution");
 front_sync_drawable();
}
/* The capture button (gamepad.m), the screenshot key (P) and a controller button held 3 s: a picture of the game's next
   frame, saved as a PNG (screenshot.h, shared with games/totk). Its folder is the 截图分享 page's row, so that header comes
   before settings_panel.h; a probe that links this file without the renderer offers its own picture and write. */
#if defined(__APPLE__) && TARGET_OS_OSX
#define TOTK_SCREENSHOT_WINDOW 1
#endif
#if defined(TOTK_SCREENSHOT_WINDOW) && !defined(TOTK_SCREENSHOT_PICTURE)
/* The renderer's next frame: mtl_grab_next_game_frame (third_party/d2r_gfx/gfx/nvn/nvn_mtl.m) has the present thread copy
   the game's own frame -- the source texture before the output upscale and the picture filters, no letterbox bars -- and
   hands the picture over when the GPU is done, so neither the main thread nor the game waits. Weak, so a probe links this
   file without the renderer and answers with no picture. */
extern void mtl_grab_next_present(void (^cb)(CGImageRef img)) __attribute__((weak_import));
extern void mtl_grab_next_game_frame(void (^cb)(CGImageRef img)) __attribute__((weak_import));
static void screenshot_picture(void (^cb)(CGImageRef img)) {
    if (mtl_grab_next_game_frame) mtl_grab_next_game_frame(cb);
    else if (mtl_grab_next_present) mtl_grab_next_present(cb);
    else cb(NULL);
}
#define TOTK_SCREENSHOT_PICTURE screenshot_picture
#endif
#include "screenshot.h"
#include "settings_panel.h"
#if defined(TOTK_SCREENSHOT_WINDOW)
#include "shot_upload.h"   /* 截图分享: with its switch on, each saved screenshot is shared (screenshot.h totk_screenshot_shared) */
#else
static void screenshot_request(void) {}   /* gamepad.m calls the hook; there is nothing to take a picture with here */
#endif
/* The live FPS in the menu bar: a menu of the game's own, after its app menu on the left, whose title is the frame rate
   ("60 FPS", "50+50 FPS" while frame interpolation shows generated frames), refreshed twice a second with the value the
   overlay uses. Part of the app's own menu bar, so macOS shows it whenever the game is in front (a status item next to
   the clock can be hidden by System Settings > Menu Bar). Independent of key 1; hidden in full screen with the menu bar.
   TOTK_MENUBAR_FPS=0 leaves the menu bar alone. */
extern int dm_gfx_fps_now(int *generated) __attribute__((weak_import));   // gfx/nvn/nvn_mtl.m (absent in tests)
static NSMenu *fpsMenu;static NSMenuItem *fpsMenuItem;static NSTimer *fpsTimer;
static int menubar_fps_enabled(void){const char *e=getenv("TOTK_MENUBAR_FPS");return !(e&&!strcmp(e,"0"));}
static void front_menubar_fps_add(NSMenu *bar){   // from front_install_menu
 if(!menubar_fps_enabled())return;
 fpsMenu=[[NSMenu alloc] initWithTitle:@"FPS --"];
 NSMenuItem *about=[[NSMenuItem alloc] initWithTitle:@(TOTK_UI("实时帧率（TOTK_MENUBAR_FPS=0 可隐藏）","Live frame rate (TOTK_MENUBAR_FPS=0 hides it)")) action:nil keyEquivalent:@""];
 about.enabled=NO;[fpsMenu addItem:about];fpsMenu.autoenablesItems=NO;
 const char *commit=getenv("TOTK_SOURCE_COMMIT");   /* the commit the app was built from (build_app.py, the launcher) */
 if(commit&&*commit){NSMenuItem *build=[[NSMenuItem alloc] initWithTitle:[NSString stringWithFormat:@(TOTK_UI("版本 %s","Build %s")),commit] action:nil keyEquivalent:@""];build.enabled=NO;[fpsMenu addItem:build];}
 fpsMenuItem=[[NSMenuItem alloc] initWithTitle:@"FPS --" action:nil keyEquivalent:@""];fpsMenuItem.submenu=fpsMenu;[bar addItem:fpsMenuItem];
}
/* TOTK_TITLE_FPS=1: the window title also ends in the frame rate ("Tears of the Kingdom  Mac — Ver. 1.4.3 — 60 FPS"),
   kept after whatever title the game set. Off by default: the menu bar already shows it. */
static int title_fps_enabled(void){const char *e=getenv("TOTK_TITLE_FPS");return e&&!strcmp(e,"1");}
static NSString *titleBase,*titleShown;
static void front_title_fps(NSString *fps){
 if(!title_fps_enabled()||!window)return;
 NSString *now=window.title;
 if(!titleShown||![now isEqualToString:titleShown])titleBase=now;   // the game changed its title: keep that one as the base
 titleShown=[NSString stringWithFormat:@"%@ — %@",titleBase,fps];
 if(![now isEqualToString:titleShown])window.title=titleShown;
}
static void front_menubar_fps(void){   // starts the refresh once the window exists
 if((!fpsMenu&&!title_fps_enabled())||fpsTimer)return;
 fpsTimer=[NSTimer scheduledTimerWithTimeInterval:0.5 repeats:YES block:^(NSTimer *t){(void)t;
  int gen=0,fps=dm_gfx_fps_now?dm_gfx_fps_now(&gen):-1;
  NSString *title=fps<0?@"FPS --":gen>0?[NSString stringWithFormat:@"%d+%d FPS",fps,gen]:[NSString stringWithFormat:@"%d FPS",fps];
  if(fpsMenu&&![fpsMenu.title isEqualToString:title]){fpsMenu.title=title;fpsMenuItem.title=title;}
  front_title_fps(title);}];
}
/* The drawable follows the window's pixel size; the present pass scales the game picture (1920x1080 docked) into it and
   keeps 16:9 (black bars on other shapes), so a larger window or full screen shows the game at its own resolution instead
   of a fixed small drawable that the compositor stretches. */
/* TOTK_DRAWABLE_SCALE=<f> (test scripts only): the drawable is that many times the window's pixels and the window server
   scales it down for the screen. It lets a test exercise the 2K output path (a 2560x1440 picture) on a screen smaller than
   that (2026-10-17: the Mac reported a 1920x1080 display); the desktop launcher never sets it. */
static CGFloat drawable_scale(void){static CGFloat f=-1;if(f<0){const char *e=getenv("TOTK_DRAWABLE_SCALE");f=e?atof(e):1;if(f<0.25||f>4)f=1;}return f;}
static void front_sync_drawable(void){
 NSView *v=window.contentView;if(!v||!metalLayer)return;
 NSScreen *screen=window.screen?:NSScreen.mainScreen;
 CGDirectDisplayID display=[screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue];
 CGDisplayModeRef mode=display?CGDisplayCopyDisplayMode(display):NULL;
 double rate=mode?CGDisplayModeGetRefreshRate(mode):0;if(mode)CGDisplayModeRelease(mode);
 int hz=rate>0?(int)lround(rate):(int)screen.maximumFramesPerSecond;
 if(atomic_exchange(&frontDisplayHz,hz)!=hz)fprintf(stderr,"[odyssey-display] window screen %u: %d Hz\n",display,hz);
 if(displayLink&&display)CVDisplayLinkSetCurrentCGDisplay(displayLink,display);
 CGFloat s=window.backingScaleFactor>0?window.backingScaleFactor:1,ts=drawable_scale();CGSize px=CGSizeMake(round(v.bounds.size.width*s*ts),round(v.bounds.size.height*s*ts));
 CGSize native=front_native_resolution();output_resolution_pixels(front_output_resolution(),native.width,native.height,&px.width,&px.height);
 if(px.width<1||px.height<1||CGSizeEqualToSize(metalLayer.drawableSize,px))return;
 metalLayer.contentsScale=s;metalLayer.drawableSize=px;fprintf(stderr,"[totk] window drawable %.0fx%.0f (scale %.0f)\n",px.width,px.height,s);runtime_aspect_view(px.width,px.height);

}
/* BotW, the settings panel's aspect ratio: the window takes the selected camera ratio. Keep the chosen height across toggles, even when a wide
   window has to shrink to fit the display; a manual resize establishes a new height. AppKit stays on the main queue. */
void runtime_front_resize_aspect(double ratio){
 if(!isfinite(ratio)||ratio<0.5||ratio>8)return;
 dispatch_async(dispatch_get_main_queue(), ^{
  if(!window||(window.styleMask&NSWindowStyleMaskFullScreen))return;
  NSScreen *screen=window.screen?:NSScreen.mainScreen;if(!screen)return;
  static NSSize lastSize;static CGFloat preferredHeight;
  NSRect oldFrame=window.frame;NSSize current=window.contentView.bounds.size;
  if(preferredHeight<=0||fabs(current.width-lastSize.width)>1||fabs(current.height-lastSize.height)>1)preferredHeight=current.height;
  NSRect visible=screen.visibleFrame;
  NSRect content=[window contentRectForFrameRect:visible];
  CGFloat h=fmin(preferredHeight,fmin(content.size.height,content.size.width/ratio));
  if(h<=0)return;
  NSRect frame=[window frameRectForContentRect:NSMakeRect(0,0,round(h*ratio),round(h))];
  frame.origin=NSMakePoint(NSMidX(oldFrame)-frame.size.width/2,NSMidY(oldFrame)-frame.size.height/2);
  frame.origin.x=fmax(NSMinX(visible),fmin(frame.origin.x,NSMaxX(visible)-frame.size.width));
  frame.origin.y=fmax(NSMinY(visible),fmin(frame.origin.y,NSMaxY(visible)-frame.size.height));
  [window setFrame:frame display:YES];
  lastSize=window.contentView.bounds.size;
  front_sync_drawable();
 });
}
@interface TOTKWindowDelegate:NSObject<NSWindowDelegate>
@end
@implementation TOTKWindowDelegate
-(void)windowDidBecomeKey:(NSNotification *)n{(void)n;front_playtime_tick(NO);atomic_store(&focused,1);if(window.styleMask&NSWindowStyleMaskFullScreen)[(TOTKInputView *)window.contentView setCursorAutoHideEnabled:YES];runtime_oe_push_message(15);front_mouse_update();fprintf(stderr,"[totk] window gained focus\n");front_update_awake();}
-(void)windowDidResignKey:(NSNotification *)n{(void)n;front_playtime_tick(YES);atomic_store(&focused,0);[(TOTKInputView *)window.contentView setCursorAutoHideEnabled:NO];front_mouse_update();runtime_input_clear();runtime_oe_push_message(15);fprintf(stderr,"[totk] window lost focus\n");front_update_awake();}
/* The process ends with the window. Save writes go straight to the files (runtime/fs_host.c), so a close in the middle of a
   save waits (up to 15 s, the window already gone) until the game has written and committed it (runtime_fs_save_busy). */
-(void)windowWillClose:(NSNotification *)n{(void)n;front_playtime_tick(YES);
 [(TOTKInputView *)window.contentView setCursorAutoHideEnabled:NO];
 window_closing=1;front_update_awake();
 if(mouseHeld){mouseHeld=NO;CGAssociateMouseAndMouseCursorPosition(true);[NSCursor unhide];}
 runtime_triggers_release();   /* _exit skips atexit; a save can also keep the game running a while without its window */
 if(!runtime_fs_save_busy()){fprintf(stderr,"[totk] window closed; graceful guest shutdown pending\n");_exit(0);}
 fprintf(stderr,"[totk] window closed while the game is saving: quitting when the save is done\n");
 dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0),^{
  uint64_t t0=runtime_now_ns();while(runtime_fs_save_busy()&&runtime_now_ns()-t0<15000000000ull)usleep(20000);
  fprintf(stderr,"[totk] save %s after %.2f s; quitting\n",runtime_fs_save_busy()?"still in progress (gave up)":"done",(runtime_now_ns()-t0)/1e9);_exit(0);});
}
-(void)screenParametersChanged:(NSNotification *)n{(void)n;front_sync_drawable();}
-(void)windowDidChangeScreen:(NSNotification *)n{(void)n;front_sync_drawable();}
-(void)windowDidResize:(NSNotification *)n{(void)n;front_sync_drawable();}
-(void)windowDidEnterFullScreen:(NSNotification *)n{(void)n;[(TOTKInputView *)window.contentView setCursorAutoHideEnabled:window.isKeyWindow];}
-(void)windowDidExitFullScreen:(NSNotification *)n{(void)n;[(TOTKInputView *)window.contentView setCursorAutoHideEnabled:NO];}
/* Whether any part of the window can be seen (TOTK, 2026-10-16 night): macOS stops the display link's callbacks for a
   fully covered window, so presents stop and per-picture measurements of that stretch are void; a test session's log
   says when (the user working in other apps covered the test windows of the frame interpolation tests). */
static _Atomic int windowVisible=-1;   /* -1 not known yet; read by the present path's stall lines (runtime_front_window_visible) */
static void note_occlusion(const char *when){int v=(window.occlusionState&NSWindowOcclusionStateVisible)!=0;atomic_store(&windowVisible,v);
 fprintf(stderr,"[totk] window %s%s (frame %llu)\n",v?"visible":"covered (or on another Space): no presents until it is visible again",when,(unsigned long long)dm_nvn_frame_count());
 front_update_awake();}
-(void)windowDidChangeOcclusionState:(NSNotification *)n{(void)n;note_occlusion("");}
-(void)windowDidChangeBackingProperties:(NSNotification *)n{(void)n;front_sync_drawable();}
@end
/* Gamepad input does not reset macOS's idle timer, so controller play would let the display dim and sleep. While the game
   window is visible, key and not minimised, the display is kept awake; the assertion is dropped otherwise and on close.
   Called on the main thread only (window delegate, occlusion notes). `pmset -g assertions` lists it while held. */
static IOPMAssertionID awakeAssertion;
static int awakeHeld;
static void front_update_awake(void){
 int want=!window_closing&&atomic_load(&windowVisible)==1&&atomic_load(&focused)&&!window.miniaturized;
 if(want==awakeHeld)return;
 if(want){if(IOPMAssertionCreateWithName(kIOPMAssertionTypePreventUserIdleDisplaySleep,kIOPMAssertionLevelOn,CFSTR(GAME_NAME " is being played"),&awakeAssertion)!=kIOReturnSuccess){fprintf(stderr,"[totk] 【红】display-sleep assertion could not be created\n");return;}}
 else IOPMAssertionRelease(awakeAssertion);
 awakeHeld=want;fprintf(stderr,"[totk] display sleep %s (frame %llu)\n",want?"held off while the game is visible":"allowed again",(unsigned long long)dm_nvn_frame_count());
}
static TOTKWindowDelegate *windowDelegate;
int runtime_front_window_visible(void){return atomic_load(&windowVisible);}
/* Headless test runs (TOTK_HEADLESS) have no window to close: "close" (TOTK_INPUT_FILE), SIGTERM and SIGINT end the game
   here instead, after the GPU has finished what was submitted (up to 10 s; mtl_quiesce), never by a kill in the middle of
   GPU work (2026-11-22: runs killed mid-frame left the GPU busy and its memory held after the process was gone). */
static _Atomic int headlessClosing;
static void *headless_close(void *arg){
 (void)arg;if(atomic_exchange(&headlessClosing,1))return NULL;
 extern int mtl_quiesce(uint64_t timeout_ns);extern uint64_t mtl_last_fence(void);
 fprintf(stderr,"[totk] headless close: waiting for the GPU to finish fence %llu (up to 10 s)\n",(unsigned long long)mtl_last_fence());
 int ok=mtl_quiesce(10000000000ull);
 fprintf(stderr,"[totk] headless close: GPU %s; exiting\n",ok?"idle":"still busy after 10 s");fflush(stderr);
 _exit(ok?0:4);
}
static int headlessSignalPipe[2]={-1,-1};
static void headless_signal(int sig){(void)sig;char c=1;if(headlessSignalPipe[1]>=0)(void)!write(headlessSignalPipe[1],&c,1);}
static void *headless_signal_wait(void *arg){(void)arg;char c;while(read(headlessSignalPipe[0],&c,1)!=1){}headless_close(NULL);return NULL;}
static void headless_close_setup(void){
 if(pipe(headlessSignalPipe))return;
 pthread_t t;if(pthread_create(&t,NULL,headless_signal_wait,NULL))return;pthread_detach(t);
 signal(SIGTERM,headless_signal);signal(SIGINT,headless_signal);
}
void runtime_front_request_close(void){
 if(getenv("TOTK_HEADLESS")){pthread_t t;if(!pthread_create(&t,NULL,headless_close,NULL))pthread_detach(t);return;}
 dispatch_async(dispatch_get_main_queue(), ^{[window performClose:nil];});
}
/* The data folder is the parent of TOTK_PROFILE_DIR: "Odyssey Data" for the app, whose launcher writes last-run.log there;
   a probe's save copy for agent runs, where there is no log and Show Log is disabled. */
static NSURL *front_data_folder(void){const char *p=getenv("TOTK_PROFILE_DIR");return p&&*p?[[NSURL fileURLWithPath:@(p)] URLByDeletingLastPathComponent]:nil;}
static NSURL *front_log_file(void){NSURL *d=front_data_folder();NSURL *f=[d URLByAppendingPathComponent:@"last-run.log"];return f&&[NSFileManager.defaultManager fileExistsAtPath:f.path]?f:nil;}
@interface TOTKMenuTarget:NSObject<NSMenuItemValidation>
@end
@implementation TOTKMenuTarget
-(void)showLog:(id)s{(void)s;NSURL *f=front_log_file();if(f)[NSWorkspace.sharedWorkspace openURL:f];}
-(void)openDataFolder:(id)s{(void)s;NSURL *d=front_data_folder();if(d)[NSWorkspace.sharedWorkspace openURL:d];}
-(BOOL)validateMenuItem:(NSMenuItem *)item{return item.action==@selector(showLog:)?front_log_file()!=nil:front_data_folder()!=nil;}
@end
static TOTKMenuTarget *menuTarget;
static NSMenuItem *menu_item(NSMenu *menu,const char *title,SEL action,NSString *key,NSEventModifierFlags mods,id target){
 NSMenuItem *item=[[NSMenuItem alloc] initWithTitle:@(title) action:action keyEquivalent:key];item.keyEquivalentModifierMask=mods;item.target=target;[menu addItem:item];return item;
}
/* Use the window-close path, not terminate:, so Command-Q also waits for active saves. */
static void front_install_menu(void){
 NSMenu *menu=[NSMenu new],*appMenu=[NSMenu new],*windowMenu=[[NSMenu alloc] initWithTitle:@(TOTK_UI("窗口","Window"))];
 NSMenuItem *appItem=[NSMenuItem new];appItem.submenu=appMenu;[menu addItem:appItem];
 menuTarget=[TOTKMenuTarget new];
 menu_item(appMenu,TOTK_UI("显示日志","Show Log"),@selector(showLog:),@"",0,menuTarget);
 menu_item(appMenu,TOTK_UI("打开数据文件夹","Open Data Folder"),@selector(openDataFolder:),@"",0,menuTarget);
 [appMenu addItem:NSMenuItem.separatorItem];
 menu_item(appMenu,TOTK_UI("隐藏 " GAME_NAME,"Hide " GAME_NAME),@selector(hide:),@"h",NSEventModifierFlagCommand,NSApp);
 [appMenu addItem:NSMenuItem.separatorItem];
 menu_item(appMenu,TOTK_UI("退出 " GAME_NAME,"Quit " GAME_NAME),@selector(performClose:),@"q",NSEventModifierFlagCommand,window);
 NSMenuItem *windowItem=[NSMenuItem new];windowItem.submenu=windowMenu;[menu addItem:windowItem];
 menu_item(windowMenu,TOTK_UI("最小化","Minimize"),@selector(performMiniaturize:),@"m",NSEventModifierFlagCommand,window);
 menu_item(windowMenu,TOTK_UI("进入 / 退出全屏","Toggle Full Screen"),@selector(toggleFullScreen:),@"f",NSEventModifierFlagCommand|NSEventModifierFlagControl,window);
 front_menubar_fps_add(menu);NSApp.mainMenu=menu;NSApp.windowsMenu=windowMenu;front_menubar_fps();
}
static void *guest_main(void *callback){@autoreleasepool{((void(*)(void))callback)();}return NULL;}
void runtime_front_run(void(*callback)(void)){
 @autoreleasepool{
  bootStartNS=runtime_now_ns();
  {const char *commit=getenv("TOTK_SOURCE_COMMIT");fprintf(stderr,"[totk] built from commit %s\n",commit&&*commit?commit:"(not recorded)");}
  [NSApplication sharedApplication];[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
  runtime_mouse_set_hook(front_mouse_update);
  runtime_gamepad_start();
  /* Default window: the docked output size, 1920x1080, scaled down (16:9 kept) when the screen's visible area is smaller. */
  /* TOTK_WINDOW_SIZE=<w>x<h> (test scripts only, e.g. the 2K output test; the desktop launcher never sets it): another starting size. */
  /* BY3_DISPLAY=<n> (the launcher's monitor choice): NSScreen.screens[n] holds the window, and full screen opens there. */
  NSScreen *screen=NSScreen.mainScreen;
  {const char *d=getenv("BY3_DISPLAY");NSArray<NSScreen *> *all=NSScreen.screens;if(d&&*d){unsigned long i=strtoul(d,NULL,10);if(i<all.count)screen=all[i];}}
  NSRect vis=screen?screen.visibleFrame:NSMakeRect(0,0,1920,1080+28);CGFloat ww=1920,wh=1080;
  {const char *e=getenv("TOTK_WINDOW_SIZE");int w=0,h=0;if(e&&sscanf(e,"%dx%d",&w,&h)==2&&w>=320&&h>=180&&w<=8192&&h<=8192){ww=w;wh=h;}}
  CGFloat k=fmin(1,fmin(vis.size.width/ww,(vis.size.height-28)/wh));ww=floor(ww*k);wh=floor(wh*k);
  window=[[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,ww,wh) styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskMiniaturizable|NSWindowStyleMaskResizable backing:NSBackingStoreBuffered defer:NO];
  window.releasedWhenClosed=NO;   /* a close that waits for a save (windowWillClose) must not free the window the static pointer still holds */
  /* An agent's test game (TOTK_SLOT_OWNER other than user) says so in its title: it plays a save copy and its script closes it. */
  const char *owner=getenv("TOTK_SLOT_OWNER"),*purpose=getenv("TOTK_SLOT_PURPOSE");
  /* the update the app was built from (the app launcher sets it; 1.3.0 and 1.4.1 share their executables) */
  const char *version=getenv("ODYSSEY_GAME_VERSION");if(!version||!*version)version=TOTK_GAME_VERSION;
  window.title=owner&&*owner&&strcmp(owner,"user")?[NSString stringWithFormat:@(TOTK_UI("测试局（%s）Ver. %s：%s — 存档副本，进度不会存进你的存档，脚本会自己关闭","Test session (%s) Ver. %s: %s — save copy, progress is not saved to your save, the script closes it")),owner,version,purpose&&*purpose?purpose:TOTK_UI("代理测试","agent test")]:[NSString stringWithFormat:@GAME_NAME " Mac — Ver. %s",version];
  windowDelegate=[TOTKWindowDelegate new];window.delegate=windowDelegate;
  [NSNotificationCenter.defaultCenter addObserver:windowDelegate selector:@selector(screenParametersChanged:) name:NSApplicationDidChangeScreenParametersNotification object:nil];
  front_install_menu();
  window.collectionBehavior|=NSWindowCollectionBehaviorFullScreenPrimary;
  window.contentView=[[TOTKInputView alloc] initWithFrame:NSMakeRect(0,0,ww,wh)];
  metalLayer=[CAMetalLayer layer];metalLayer.device=MTLCreateSystemDefaultDevice();metalLayer.pixelFormat=MTLPixelFormatBGRA8Unorm;metalLayer.drawableSize=CGSizeMake(ww,wh);runtime_aspect_view(ww,wh);
  metalLayer.framebufferOnly=NO;window.contentView.wantsLayer=YES;window.contentView.layer=metalLayer;
  bootOverlay=[[NSView alloc] initWithFrame:window.contentView.bounds];bootOverlay.autoresizingMask=NSViewWidthSizable|NSViewHeightSizable;
  bootOverlay.wantsLayer=YES;bootOverlay.layer.backgroundColor=[NSColor colorWithCalibratedRed:0.035 green:0.075 blue:0.09 alpha:1].CGColor;
  bootStage=[NSTextField labelWithString:@(TOTK_UI("正在启动 " GAME_NAME,"Starting " GAME_NAME))];
  bootStage.font=[NSFont systemFontOfSize:25 weight:NSFontWeightSemibold];bootStage.textColor=NSColor.whiteColor;
  bootStage.alignment=NSTextAlignmentCenter;bootStage.frame=NSMakeRect(0,wh/2+20,ww,40);bootStage.autoresizingMask=NSViewWidthSizable|NSViewMinYMargin|NSViewMaxYMargin;
  bootDetail=[NSTextField labelWithString:@(TOTK_UI("正在准备游戏环境","Preparing the game environment"))];
  bootDetail.font=[NSFont systemFontOfSize:14];bootDetail.textColor=[NSColor colorWithCalibratedWhite:0.72 alpha:1];
  bootDetail.alignment=NSTextAlignmentCenter;bootDetail.frame=NSMakeRect(0,wh/2-20,ww,28);bootDetail.autoresizingMask=NSViewWidthSizable|NSViewMinYMargin|NSViewMaxYMargin;
  bootProgress=[[NSProgressIndicator alloc] initWithFrame:NSMakeRect(ww/2-140,wh/2-55,280,10)];
  bootProgress.minValue=0;bootProgress.maxValue=100;bootProgress.indeterminate=YES;bootProgress.autoresizingMask=NSViewMinXMargin|NSViewMaxXMargin|NSViewMinYMargin|NSViewMaxYMargin;
  [bootOverlay addSubview:bootStage];[bootOverlay addSubview:bootDetail];[bootOverlay addSubview:bootProgress];
  [window.contentView addSubview:bootOverlay];[bootProgress startAnimation:nil];
  front_menubar_fps();
  settings_restore_start();
  settings_controller_start();
  runtime_capture_set_hook(screenshot_request);   /* the controller's capture button, P, a held button: a picture of the game (screenshot.h) */
  front_playtime_start();
  /* The FPS, shader and load lines: in the corner box (compact HUD), or with TOTK_HUD=corners drawn into the game picture's
     corners by the present encoder (gfx/nvn/fps_overlay.c). The box sits under the opaque boot overlay until it is hidden. */
  TOTKHudBox *box=[[TOTKHudBox alloc] initWithFrame:NSZeroRect];box.autoresizingMask=NSViewMinXMargin|NSViewMinYMargin;
  [window.contentView addSubview:box positioned:NSWindowBelow relativeTo:bootOverlay];hudBox=box;
  /* The settings panel's FPS graph switch, kept in fps-graph.txt next to controls.txt ("on" / "off", default off). */
  {char word[16];if(runtime_setting_word("fps-graph.txt",word,sizeof word))fpsGraphEnabled=!strncmp(word,"on",2);}
  TOTKFpsGraph *graph=[[TOTKFpsGraph alloc] initWithFrame:NSZeroRect];[box addSubview:graph];fpsGraphBox=graph;[graph setEnabled:fpsGraphEnabled];[box refresh];
  [window makeFirstResponder:window.contentView];[window center];front_sync_drawable();
  if(assume_focus()){[window orderFrontRegardless];}else{[window makeKeyAndOrderFront:nil];[NSApp activateIgnoringOtherApps:YES];}   /* in front of other apps' windows, keyboard left alone */
  mng_active_start(@"p5r", mng_active_version());
  totk_board_start();   /* the leaderboard's rounds (board_upload.h): nothing is sent unless the player turned 我要上榜 on */
#if defined(TOTK_SCREENSHOT_WINDOW)
  totk_screenshot_shared=totk_shot_queue;totk_shot_start();   /* 截图分享 (shot_upload.h): off unless the player turned it on */
#endif
  [NSNotificationCenter.defaultCenter addObserverForName:NSApplicationDidChangeScreenParametersNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n){(void)n;front_sync_drawable();}];
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW,3*NSEC_PER_SEC),dispatch_get_main_queue(),^{note_occlusion(" 3 s after opening");});
  atomic_store(&focused,1);
  if(CVDisplayLinkCreateWithActiveCGDisplays(&displayLink)||CVDisplayLinkSetOutputCallback(displayLink,vsync_callback,NULL)||CVDisplayLinkStart(displayLink))runtime_fail("native display link initialization");
  front_sync_drawable();   /* the link exists now: point it at the window's screen (the call above came before it) */
  pthread_t guest;pthread_attr_t attr;pthread_attr_init(&attr);pthread_attr_setstacksize(&attr,8*1024*1024);int err=pthread_create(&guest,&attr,guest_main,(void *)callback);pthread_attr_destroy(&attr);if(err)runtime_fail("native game thread creation");
  [NSApp run];runtime_fail("native event loop returned unexpectedly");
 }
}
/* AppKit text entry, including controller navigation; independent of the renderer. */
#include "text_keyboard.h"
static __weak TOTKTextBar *openBar;   /* main thread only */
void runtime_front_text_test(const char *op,const char *text){
 NSString *o=[NSString stringWithUTF8String:op],*t=[NSString stringWithUTF8String:text?text:""]?:@"";
 dispatch_async(dispatch_get_main_queue(), ^{
  TOTKTextBar *bar=openBar;if(!bar){fprintf(stderr,"[totk] keyboard test: no text entry bar is open\n");return;}
  if([o isEqualToString:@"type"]){bar.field.stringValue=[bar.field.stringValue stringByAppendingString:t];[bar refilter];}
  else if([o isEqualToString:@"enter"])[bar control:bar.field textView:(NSTextView *)bar.field.currentEditor doCommandBySelector:@selector(insertNewline:)];
  else if([o isEqualToString:@"shot"]){   /* the bar as drawn (window screenshots of the game picture do not include AppKit views) */
   NSBitmapImageRep *rep=[bar bitmapImageRepForCachingDisplayInRect:bar.bounds];[bar cacheDisplayInRect:bar.bounds toBitmapImageRep:rep];
   NSData *png=[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];BOOL ok=[png writeToFile:t atomically:YES];
   fprintf(stderr,"[totk] keyboard test: bar picture %s %s\n",t.UTF8String,ok?"written":"FAILED");return;}
  else if([o isEqualToString:@"esc"])[bar control:bar.field textView:(NSTextView *)bar.field.currentEditor doCommandBySelector:@selector(cancelOperation:)];
  fprintf(stderr,"[totk] keyboard test: %s -> field \"%s\"\n",o.UTF8String,bar.field.stringValue.UTF8String);
 });
}
int runtime_front_text_input(const RuntimeTextRequest *req,char *out,size_t out_size){
 if(!window||!req||!out||!out_size){fprintf(stderr,"[totk] keyboard: no game window for text entry; cancelled\n");return 0;}
 NSString *prompt=[NSString stringWithUTF8String:req->prompt?req->prompt:""]?:@"",*initial=[NSString stringWithUTF8String:req->initial?req->initial:""]?:@"";
 int maxU=req->max_units,minU=req->min_units,digits=req->digits_only,secure=req->secure;uint32_t dis=req->disabled;
 __block NSString *result=nil;dispatch_semaphore_t sem=dispatch_semaphore_create(0);
 dispatch_async(dispatch_get_main_queue(), ^{
  if(openBar){dispatch_semaphore_signal(sem);return;}   /* do not strand an earlier guest request */
  if(settingsOpen)settings_show(0);
  NSView *cv=window.contentView;
  TOTKTextBar *bar=[[TOTKTextBar alloc] initWithFrame:cv.bounds];
  bar.maxUnits=maxU;bar.minUnits=minU;bar.digitsOnly=digits;bar.disabled=dis;
  [bar buildWithPrompt:prompt initial:initial secure:secure];
  __weak TOTKTextBar *weakBar=bar;
  bar.done=^(NSString *text){result=[text copy];[weakBar removeFromSuperview];openBar=nil;
   runtime_gamepad_text_capture(0);[window makeFirstResponder:window.contentView];
   textEntryOpen=0;front_mouse_update();dispatch_semaphore_signal(sem);};
  [cv addSubview:bar];openBar=bar;
  runtime_input_clear();
  if(window.miniaturized)[window deminiaturize:nil];
  if(!assume_focus()){[NSApp activateIgnoringOtherApps:YES];[window makeKeyAndOrderFront:nil];}   /* test windows never take the keyboard */
  [window makeFirstResponder:bar.field];
  runtime_gamepad_text_capture(1);[bar startController];
  textEntryOpen=1;front_mouse_update();   /* the pointer is the player's while typing */
  fprintf(stderr,"[totk] keyboard: text entry bar shown (%s)\n",prompt.UTF8String);
 });
 dispatch_semaphore_wait(sem,DISPATCH_TIME_FOREVER);
 if(!result)return 0;
 snprintf(out,out_size,"%s",result.UTF8String?result.UTF8String:"");
 return 1;
}
static int viReady;
static unsigned displayIdentity,layerIdentity;
/* TOTK_HEADLESS (test runs without a window: the game boots on the main thread, loader.c): the picture goes to a layer that
   is in no window, the game's own 1920x1080, so screenshots ("shot", TOTK_INPUT_FILE) and the frame timing still work and
   nothing is drawn on the screen (WindowServer is left alone). */
static void headless_layer(void){
 if(metalLayer||!getenv("TOTK_HEADLESS"))return;
 metalLayer=[CAMetalLayer layer];metalLayer.device=MTLCreateSystemDefaultDevice();metalLayer.pixelFormat=MTLPixelFormatBGRA8Unorm;
 metalLayer.drawableSize=CGSizeMake(1920,1080);metalLayer.framebufferOnly=NO;runtime_aspect_view(1920,1080);
 fprintf(stderr,"[totk] headless: the picture goes to a 1920x1080 layer in no window\n");
 headless_close_setup();
}
static void vi_initialize(void){headless_layer();if(!metalLayer)runtime_fail("VI requires native macOS window");viReady=1;fprintf(stderr,"[totk] native VI display initialized\n");}
static uint32_t vi_open(void **out){if(!viReady||!out)return 0xf601;*out=&displayIdentity;return 0;}
static uint32_t vi_layer(void **out,void *display){if(!out||display!=&displayIdentity)return 0xf601;*out=&layerIdentity;return 0;}
static uint32_t vi_window(void **out,void *layer){if(!out||layer!=&layerIdentity)return 0xf601;*out=front_metal_layer();return 0;}
static uint32_t vi_vsync(void *ev,void *display){
 if(!ev||display!=&displayIdentity)return 0xf601;hle_bind_system_event(ev,1);
 pthread_mutex_lock(&vsyncLock);if(vsyncCount>=16)runtime_fail("VI display event limit");vsyncEvents[vsyncCount++]=ev;pthread_mutex_unlock(&vsyncLock);return 0;
}
/* Red Dead Redemption (SDK 15.3.2) creates its layer with a size (the 4-argument CreateLayer) and crops it; the window
   scales the game's picture itself, so the size and crop are logged, not applied (games/eow answers its crop the same). */
static uint32_t vi_layer_sized(void **out,void *display,int w,int h){uint32_t r=vi_layer(out,display);fprintf(stderr,"[totk] VI layer %dx%d\n",w,h);return r;}
static int layerCrop[4];
static void vi_layer_crop(void *layer,int x,int y,int w,int h){
 if(layer!=&layerIdentity)runtime_fail("SetLayerCrop on an unknown layer");
 int v[4]={x,y,w,h};if(memcmp(v,layerCrop,sizeof v)){memcpy(layerCrop,v,sizeof v);fprintf(stderr,"[totk] VI layer crop %d,%d %dx%d (not applied)\n",x,y,w,h);}
}
static void vi_destroy_layer(void *layer){if(layer!=&layerIdentity)runtime_fail("DestroyLayer on an unknown layer");}
static void vi_close_display(void *display){if(display!=&displayIdentity)runtime_fail("CloseDisplay on an unknown display");}
static void vi_finalize(void){}
/* As games/totk's: the layer's scaling mode (Mario Kart 8 Deluxe sets 2, FitToLayer, after CreateLayer); the window scales itself. */
static uint32_t vi_scaling_mode(void *layer,int mode){if(layer!=&layerIdentity)return 0xf601;(void)mode;return 0;}
void *runtime_front_override(const char *name){
 static const struct{const char *name;void *fn;} entries[]={
 {"_ZN2nn2vi19SetLayerScalingModeEPNS0_5LayerENS0_11ScalingModeE",vi_scaling_mode},
 {"_ZN2nn2vi10InitializeEv",vi_initialize},{"_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE",vi_open},
 {"_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE",vi_layer},{"_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE",vi_window},
 {"_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE",vi_vsync},
 {"_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayEii",vi_layer_sized},{"_ZN2nn2vi12SetLayerCropEPNS0_5LayerEiiii",vi_layer_crop},
 {"_ZN2nn2vi12DestroyLayerEPNS0_5LayerE",vi_destroy_layer},{"_ZN2nn2vi12CloseDisplayEPNS0_7DisplayE",vi_close_display},
 {"_ZN2nn2vi8FinalizeEv",vi_finalize}};
 for(unsigned i=0;i<sizeof(entries)/sizeof(*entries);i++)if(!strcmp(name,entries[i].name))return entries[i].fn;return NULL;
}
