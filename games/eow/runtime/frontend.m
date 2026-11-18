/* Native macOS window and VI service adapter. No guest frame is synthesized. */
#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>
#import <CoreVideo/CoreVideo.h>
#include <stdatomic.h>
#include <math.h>
#include "runtime.h"
#include "game_version.h"
#include "totk_ui_lang.h"
#include "gamepad_layout.h"
#include "../third_party/d2r_gfx/gfx/nvn/fps_overlay.h"
#import "playtime.h"
static NSWindow *window;
uint64_t dm_nvn_frame_count(void);   /* gfx/nvn/hle_nvn.c: game frames presented so far */
static CAMetalLayer *metalLayer;
static NSView *bootOverlay;
static NSTextField *bootStage,*bootDetail;
static NSProgressIndicator *bootProgress;
static NSView *hudBox;   /* the FPS / shader / load box (TOTKHudBox, shared/runtime/hud_box.h), hidden with TOTK_HUD=corners */
static NSView *fpsGraphBox;
static int fpsGraphEnabled;
/* The settings panel (key 1, settings_panel.h) and the text entry bar: while one is open the game gets no keyboard or
   mouse input. */
static int settingsOpen,textEntryOpen;
static void settings_show(int open);
static uint64_t bootStartNS;
static _Atomic uint64_t presentedFrames;
static _Atomic int warmPending;
/* Counts foreground time after warm-up, including menus, like Screen Time (as games/totk and games/acnh: the corner box's
   playtime line, the session and the total kept in <TOTK_PROFILE_DIR>/playtime.json, beside the save folder, never inside it). */
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
static pthread_mutex_t vsyncLock=PTHREAD_MUTEX_INITIALIZER;
static void *vsyncEvents[16];
static unsigned vsyncCount;
void hle_bind_system_event(void *,int);
void hle_signal_system_event(void *);
void *front_metal_layer(void){return (__bridge void *)metalLayer;}
void front_frame_presented(void){
 if(atomic_fetch_add_explicit(&presentedFrames,1,memory_order_relaxed)==0){
  fprintf(stderr,"[totk-boot] %.3f first presented frame\n",(runtime_now_ns()-bootStartNS)/1e9);
  dispatch_async(dispatch_get_main_queue(), ^{if(!atomic_load(&warmPending))bootOverlay.hidden=YES;});
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
  bootDetail.stringValue=[NSString stringWithFormat:@(TOTK_UI("已处理 %llu/%llu 项 · 着色器 %llu/%llu · 管线 %llu/%llu","%llu/%llu items · shaders %llu/%llu · pipelines %llu/%llu")),
      (unsigned long long)done,(unsigned long long)total,(unsigned long long)shaders,(unsigned long long)shader_total,
      (unsigned long long)pipelines,(unsigned long long)pipeline_total];
  bootProgress.indeterminate=NO;
  bootProgress.doubleValue=percent;
 });
}
void runtime_warm_done(void){
 atomic_store(&warmPending,0);
 dispatch_async(dispatch_get_main_queue(), ^{if(atomic_load(&presentedFrames))bootOverlay.hidden=YES;});
}
uint64_t front_game_res(int docked){(void)docked;return (720ULL<<32)|1280;}
/* TOTK_ASSUME_FOCUS=1: unattended test runs. macOS refuses to activate an app launched from the background while the user is working in
   another one; the window then never becomes key, GetCurrentFocusState stays 0 and the game waits before its first frame. In this mode the
   game is told it is focused, and the window is shown without taking the keyboard from the user (scripted input does not need it). */
static int assume_focus(void){static int v=-1;if(v<0){const char *e=getenv("TOTK_ASSUME_FOCUS");v=e&&atoi(e);}return v;}
/* Keep the guest active when the macOS window loses key status. Native input is still cleared
   in windowDidResignKey so a held key cannot remain stuck while another app has focus. */
int runtime_front_focus(void){return 1;}
static CVReturn vsync_callback(CVDisplayLinkRef link,const CVTimeStamp *now,const CVTimeStamp *output,CVOptionFlags flags,CVOptionFlags *out,void *ctx){
 (void)link;(void)now;(void)output;(void)flags;(void)out;(void)ctx;
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
-(void)setCursorAutoHideEnabled:(BOOL)enabled;
@end
/* The shared settings panel (settings_panel.h) and the mouse: Echoes of Wisdom's pointer stays free (nothing to aim), so
   there is no mouse mode to hand over when the panel opens or closes; the panel only needs the pointer shown. */
static void front_mouse_update(void){}
@implementation TOTKInputView
-(BOOL)acceptsFirstResponder{return YES;}
-(void)setCursorAutoHideEnabled:(BOOL)enabled{if(!enabled)[NSCursor setHiddenUntilMouseMoves:NO];}
/* While the settings panel is open only key 1 (it closes the panel, runtime/input.c) and Esc (closes it here, not the
   game's - button) do anything; releases still go through so nothing stays held. */
-(void)keyDown:(NSEvent *)e{if(textEntryOpen)return;if(settingsOpen&&e.keyCode!=18){if(e.keyCode==53)settings_show(0);return;}runtime_input_key(e.keyCode,1);}
-(void)keyUp:(NSEvent *)e{runtime_input_key(e.keyCode,0);}
/* Shift, Control and Option, left and right apart, are keys of the layout (keyboard_map.h); they come as flag changes.
   While the settings panel or text entry is open only their releases go through. */
-(void)flagsChanged:(NSEvent *)e{int down=front_modifier_down(e);if(down>=0&&(!(settingsOpen||textEntryOpen)||!down))runtime_input_key(e.keyCode,down);}
/* The mouse's buttons over the game picture are inputs of the layout (keyboard_map.h; none drives anything by default). */
-(void)mouseDown:(NSEvent *)e{(void)e;if(!settingsOpen&&!textEntryOpen)runtime_input_mouse(0,1);}
-(void)mouseUp:(NSEvent *)e{(void)e;runtime_input_mouse(0,0);}
-(void)rightMouseDown:(NSEvent *)e{(void)e;if(!settingsOpen&&!textEntryOpen)runtime_input_mouse(1,1);}   /* no context menu */
-(void)rightMouseUp:(NSEvent *)e{(void)e;runtime_input_mouse(1,0);}
-(void)otherMouseDown:(NSEvent *)e{if(!settingsOpen&&!textEntryOpen)runtime_input_mouse((int)e.buttonNumber,1);}   /* 2 middle, 3 and 4 the side buttons */
-(void)otherMouseUp:(NSEvent *)e{runtime_input_mouse((int)e.buttonNumber,0);}
@end
/* The corner box and the FPS graph (shared/runtime/hud_box.h, as the Zelda games): FPS, shader, load and playtime lines
   (published by gfx/nvn/fps_overlay.c, refreshed twice a second), the controller's battery and how to open the settings
   panel; the panel's overlay row picks all, FPS full information, FPS + playtime or none. It replaces the shortcut box of
   keys 1-4 (the settings panel replaced those keys). TOTK_HUD=corners: the lines stay in the picture's corners. */
#include "hud_box.h"
/* The capture button (gamepad.m), the screenshot key (P) and a controller button held 3 s: a picture of the game's next
   frame, saved as a PNG (screenshot.h, shared with games/totk). Its folder is the 截图分享 page's row, so that header comes
   before settings_panel.h; a probe that links this file without the renderer offers its own picture and write. */
#if defined(__APPLE__) && TARGET_OS_OSX
#define TOTK_SCREENSHOT_WINDOW 1
#endif
#if defined(TOTK_SCREENSHOT_WINDOW) && !defined(TOTK_SCREENSHOT_PICTURE)
/* The renderer's next frame: mtl_grab_next_game_frame (third_party/d2r_gfx/gfx/nvn/nvn_mtl.m) has the present thread copy
   the game's own frame -- the source texture before the output upscale, no letterbox bars -- and hands the picture over
   when the GPU is done, so neither the main thread nor the game waits. Weak, so a probe links this file without the
   renderer and answers with no picture. */
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
/* The drawable follows the window's pixel size; the present pass scales the game picture (1920x1080 docked) into it and
   keeps 16:9 (black bars on other shapes), so a larger window or full screen shows the game at its own resolution instead
   of a fixed small drawable that the compositor stretches. */
/* TOTK_DRAWABLE_SCALE=<f> (test scripts only): the drawable is that many times the window's pixels and the window server
   scales it down for the screen. It lets a test exercise the 2K output path (a 2560x1440 picture) on a screen smaller than
   that (2026-10-17: the Mac reported a 1920x1080 display); the desktop launcher never sets it. */
static CGFloat drawable_scale(void){static CGFloat f=-1;if(f<0){const char *e=getenv("TOTK_DRAWABLE_SCALE");f=e?atof(e):1;if(f<0.25||f>4)f=1;}return f;}
static void front_sync_drawable(void){
 NSView *v=window.contentView;if(!v||!metalLayer)return;
 CGFloat s=window.backingScaleFactor>0?window.backingScaleFactor:1,ts=drawable_scale();CGSize px=CGSizeMake(round(v.bounds.size.width*s*ts),round(v.bounds.size.height*s*ts));
 if(px.width<1||px.height<1||CGSizeEqualToSize(metalLayer.drawableSize,px))return;
 metalLayer.contentsScale=s;metalLayer.drawableSize=px;fprintf(stderr,"[totk] window drawable %.0fx%.0f (scale %.0f)\n",px.width,px.height,s);
}
@interface TOTKWindowDelegate:NSObject<NSWindowDelegate>
@end
@implementation TOTKWindowDelegate
-(void)windowDidBecomeKey:(NSNotification *)n{(void)n;front_playtime_tick(NO);atomic_store(&focused,1);runtime_oe_push_message(15);fprintf(stderr,"[totk] window gained focus\n");}
-(void)windowDidResignKey:(NSNotification *)n{(void)n;front_playtime_tick(YES);atomic_store(&focused,0);runtime_input_clear();runtime_oe_push_message(15);fprintf(stderr,"[totk] window lost focus\n");}
/* The process ends with the window. Save writes go straight to the files (runtime/fs_host.c), so a close in the middle of a
   save waits (up to 15 s, the window already gone) until the game has written and committed it (runtime_fs_save_busy). */
-(void)windowWillClose:(NSNotification *)n{(void)n;front_playtime_tick(YES);
 if(!runtime_fs_save_busy()){fprintf(stderr,"[totk] window closed; graceful guest shutdown pending\n");_exit(0);}
 fprintf(stderr,"[totk] window closed while the game is saving: quitting when the save is done\n");
 dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0),^{
  uint64_t t0=runtime_now_ns();while(runtime_fs_save_busy()&&runtime_now_ns()-t0<15000000000ull)usleep(20000);
  fprintf(stderr,"[totk] save %s after %.2f s; quitting\n",runtime_fs_save_busy()?"still in progress (gave up)":"done",(runtime_now_ns()-t0)/1e9);_exit(0);});
}
-(void)windowDidResize:(NSNotification *)n{(void)n;front_sync_drawable();}
/* Whether any part of the window can be seen (TOTK, 2026-10-16 night): macOS stops the display link's callbacks for a
   fully covered window, so presents stop and per-picture measurements of that stretch are void; a test session's log
   says when (the user working in other apps covered the test windows of the frame interpolation tests). */
static _Atomic int windowVisible=-1;   /* -1 not known yet; read by the present path's stall lines (runtime_front_window_visible) */
static void note_occlusion(const char *when){int v=(window.occlusionState&NSWindowOcclusionStateVisible)!=0;atomic_store(&windowVisible,v);
 fprintf(stderr,"[totk] window %s%s (frame %llu)\n",v?"visible":"covered (or on another Space): no presents until it is visible again",when,(unsigned long long)dm_nvn_frame_count());}
-(void)windowDidChangeOcclusionState:(NSNotification *)n{(void)n;note_occlusion("");}
-(void)windowDidChangeBackingProperties:(NSNotification *)n{(void)n;front_sync_drawable();}
@end
static TOTKWindowDelegate *windowDelegate;
int runtime_front_window_visible(void){return atomic_load(&windowVisible);}
void runtime_front_request_close(void){dispatch_async(dispatch_get_main_queue(), ^{[window performClose:nil];});}
/* Use the window-close path, not terminate:, so Command-Q also waits for active saves. */
static void front_install_menu(void){
 NSMenu *menu=[NSMenu new],*appMenu=[NSMenu new];
 NSMenuItem *appItem=[NSMenuItem new];appItem.submenu=appMenu;[menu addItem:appItem];
 NSMenuItem *quit=[[NSMenuItem alloc] initWithTitle:@(TOTK_UI("退出 塞尔达传说 智慧的再现","Quit The Legend of Zelda: Echoes of Wisdom")) action:@selector(performClose:) keyEquivalent:@"q"];
 quit.keyEquivalentModifierMask=NSEventModifierFlagCommand;quit.target=window;
 [appMenu addItem:quit];NSApp.mainMenu=menu;
}
static void *guest_main(void *callback){@autoreleasepool{((void(*)(void))callback)();}return NULL;}
void runtime_front_run(void(*callback)(void)){
 @autoreleasepool{
  bootStartNS=runtime_now_ns();
  [NSApplication sharedApplication];[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
  runtime_gamepad_start();
  /* Default window: the docked output size, 1920x1080 in the screen's own pixels (960x540 points on a 2x Retina screen),
     so the window shows the picture pixel for pixel instead of stretching it (as games/acnh, 2026-11-17); scaled down,
     16:9 kept, when the screen's visible area is smaller. Resizing or full screen still scales the picture. */
  /* TOTK_WINDOW_SIZE=<w>x<h> (test scripts only, e.g. the 2K output test; the desktop launcher never sets it): another starting size. */
  /* EOW_DISPLAY=<n> (the launcher's monitor choice): NSScreen.screens[n] holds the window, and full screen opens there. */
  NSScreen *screen=NSScreen.mainScreen;
  {const char *d=getenv("EOW_DISPLAY");NSArray<NSScreen *> *all=NSScreen.screens;if(d&&*d){unsigned long i=strtoul(d,NULL,10);if(i<all.count)screen=all[i];}}
  NSRect vis=screen?screen.visibleFrame:NSMakeRect(0,0,1920,1080+28);CGFloat backing=screen&&screen.backingScaleFactor>0?screen.backingScaleFactor:1,ww=1920/backing,wh=1080/backing;
  {const char *e=getenv("TOTK_WINDOW_SIZE");int w=0,h=0;if(e&&sscanf(e,"%dx%d",&w,&h)==2&&w>=320&&h>=180&&w<=8192&&h<=8192){ww=w;wh=h;}}
  CGFloat k=fmin(1,fmin(vis.size.width/ww,(vis.size.height-28)/wh));ww=floor(ww*k);wh=floor(wh*k);
  window=[[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,ww,wh) styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskMiniaturizable|NSWindowStyleMaskResizable backing:NSBackingStoreBuffered defer:NO];
  window.releasedWhenClosed=NO;   /* a close that waits for a save (windowWillClose) must not free the window the static pointer still holds */
  /* An agent's test game (TOTK_SLOT_OWNER other than user) says so in its title: it plays a save copy and its script closes it. */
  const char *owner=getenv("TOTK_SLOT_OWNER"),*purpose=getenv("TOTK_SLOT_PURPOSE");
  window.title=owner&&*owner&&strcmp(owner,"user")?[NSString stringWithFormat:@(TOTK_UI("测试局（%s）Ver. %s：%s — 存档副本，进度不会存进你的存档，脚本会自己关闭","Test session (%s) Ver. %s: %s — save copy, progress is not saved to your save, the script closes it")),owner,TOTK_GAME_VERSION,purpose&&*purpose?purpose:TOTK_UI("代理测试","agent test")]:[NSString stringWithFormat:@"Zelda: Echoes of Wisdom  Mac — Ver. %s",TOTK_GAME_VERSION];
  windowDelegate=[TOTKWindowDelegate new];window.delegate=windowDelegate;
  front_install_menu();
  window.collectionBehavior|=NSWindowCollectionBehaviorFullScreenPrimary;
  window.contentView=[[TOTKInputView alloc] initWithFrame:NSMakeRect(0,0,ww,wh)];
  metalLayer=[CAMetalLayer layer];metalLayer.device=MTLCreateSystemDefaultDevice();metalLayer.pixelFormat=MTLPixelFormatBGRA8Unorm;metalLayer.drawableSize=CGSizeMake(ww,wh);
  metalLayer.framebufferOnly=NO;window.contentView.wantsLayer=YES;window.contentView.layer=metalLayer;
  bootOverlay=[[NSView alloc] initWithFrame:window.contentView.bounds];bootOverlay.autoresizingMask=NSViewWidthSizable|NSViewHeightSizable;
  bootOverlay.wantsLayer=YES;bootOverlay.layer.backgroundColor=[NSColor colorWithCalibratedRed:0.035 green:0.075 blue:0.09 alpha:1].CGColor;
  bootStage=[NSTextField labelWithString:@(TOTK_UI("正在启动 塞尔达传说 智慧的再现","Starting The Legend of Zelda: Echoes of Wisdom"))];
  bootStage.font=[NSFont systemFontOfSize:25 weight:NSFontWeightSemibold];bootStage.textColor=NSColor.whiteColor;
  bootStage.alignment=NSTextAlignmentCenter;bootStage.frame=NSMakeRect(0,wh/2+20,ww,40);bootStage.autoresizingMask=NSViewWidthSizable|NSViewMinYMargin|NSViewMaxYMargin;
  bootDetail=[NSTextField labelWithString:@(TOTK_UI("正在准备游戏环境","Preparing the game environment"))];
  bootDetail.font=[NSFont systemFontOfSize:14];bootDetail.textColor=[NSColor colorWithCalibratedWhite:0.72 alpha:1];
  bootDetail.alignment=NSTextAlignmentCenter;bootDetail.frame=NSMakeRect(0,wh/2-20,ww,28);bootDetail.autoresizingMask=NSViewWidthSizable|NSViewMinYMargin|NSViewMaxYMargin;
  bootProgress=[[NSProgressIndicator alloc] initWithFrame:NSMakeRect(ww/2-140,wh/2-55,280,10)];
  bootProgress.minValue=0;bootProgress.maxValue=100;bootProgress.indeterminate=YES;bootProgress.autoresizingMask=NSViewMinXMargin|NSViewMaxXMargin|NSViewMinYMargin|NSViewMaxYMargin;
  [bootOverlay addSubview:bootStage];[bootOverlay addSubview:bootDetail];[bootOverlay addSubview:bootProgress];
  [window.contentView addSubview:bootOverlay];[bootProgress startAnimation:nil];
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
  [window makeFirstResponder:window.contentView];
  {NSRect f=window.frame;f.origin.x=NSMidX(vis)-f.size.width/2;f.origin.y=NSMidY(vis)-f.size.height/2;[window setFrame:f display:NO];}   /* centred on the chosen screen */
  front_sync_drawable();
  if(assume_focus()){[window orderFrontRegardless];}else{[window makeKeyAndOrderFront:nil];[NSApp activateIgnoringOtherApps:YES];}   /* in front of other apps' windows, keyboard left alone */
  /* EOW_FULLSCREEN=1 (the launcher's "Full screen"): macOS full screen from the start; the green button or ⌃⌘F leaves it. */
  {const char *fs=getenv("EOW_FULLSCREEN");if(fs&&!strcmp(fs,"1"))dispatch_async(dispatch_get_main_queue(),^{if(!(window.styleMask&NSWindowStyleMaskFullScreen))[window toggleFullScreen:nil];});}
  totk_board_start();   /* the leaderboard's rounds (board_upload.h): nothing is sent unless the player turned 我要上榜 on */
#if defined(TOTK_SCREENSHOT_WINDOW)
  totk_screenshot_shared=totk_shot_queue;totk_shot_start();   /* 截图分享 (shot_upload.h): off unless the player turned it on */
#endif
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW,3*NSEC_PER_SEC),dispatch_get_main_queue(),^{note_occlusion(" 3 s after opening");});
  atomic_store(&focused,1);
  if(CVDisplayLinkCreateWithActiveCGDisplays(&displayLink)||CVDisplayLinkSetOutputCallback(displayLink,vsync_callback,NULL)||CVDisplayLinkStart(displayLink))runtime_fail("native display link initialization");
  pthread_t guest;pthread_attr_t attr;pthread_attr_init(&attr);pthread_attr_setstacksize(&attr,8*1024*1024);int err=pthread_create(&guest,&attr,guest_main,(void *)callback);pthread_attr_destroy(&attr);if(err)runtime_fail("native game thread creation");
  [NSApp run];runtime_fail("native event loop returned unexpectedly");
 }
}
/* Text entry for the game (nn::swkbd::ShowKeyboard, runtime/swkbd_host.c; on the Switch a keyboard applet takes the screen).
   User, 2026-10-17: no on-screen keyboard, just the Mac keyboard. A bar at the bottom of the game window shows the game's
   prompt and one text field (input methods work); Return confirms, Escape cancels. Characters the game disallows and text
   beyond its length limit are removed as they are typed (the caller checks again). The calling guest thread waits on a
   semaphore; the game's keys are cleared while the bar has the keyboard. */
@interface TOTKTextBar:NSView<NSTextFieldDelegate>
@property(strong) NSTextField *field;
@property int maxUnits,minUnits,digitsOnly;
@property uint32_t disabled;
@property(copy) void(^done)(NSString *text);   /* nil: cancelled */
-(void)refilter;
@end
@implementation TOTKTextBar
-(BOOL)allowed:(unichar)c{
 BOOL digit=c>='0'&&c<='9';uint32_t d=self.disabled;
 if(self.digitsOnly&&!digit)return NO;
 if(c<0x20||c==0x7f)return NO;
 if((d&1u<<1&&c==' ')||(d&1u<<2&&c=='@')||(d&1u<<3&&c=='%')||(d&1u<<4&&c=='/')||(d&1u<<5&&c=='\\')||(d&1u<<6&&digit))return NO;
 return YES;
}
-(void)controlTextDidChange:(NSNotification *)n{(void)n;[self refilter];}
-(void)refilter{
 NSText *ed=self.field.currentEditor;if([ed isKindOfClass:NSTextView.class]&&((NSTextView *)ed).hasMarkedText)return;   /* still composing */
 NSString *s=self.field.stringValue;NSMutableString *t=[NSMutableString string];
 [s enumerateSubstringsInRange:NSMakeRange(0,s.length) options:NSStringEnumerationByComposedCharacterSequences usingBlock:^(NSString *sub,NSRange r,NSRange er,BOOL *stop){
  (void)r;(void)er;
  if(sub.length==1&&![self allowed:[sub characterAtIndex:0]])return;
  if(self.maxUnits>0&&t.length+sub.length>(NSUInteger)self.maxUnits){*stop=YES;return;}
  [t appendString:sub];}];
 if(![t isEqualToString:s]){self.field.stringValue=t;NSBeep();}
}
-(void)finish:(NSString *)text{void(^d)(NSString *)=self.done;self.done=nil;if(d)d(text);}
-(BOOL)control:(NSControl *)c textView:(NSTextView *)tv doCommandBySelector:(SEL)sel{
 (void)c;(void)tv;
 if(sel==@selector(insertNewline:)){[self refilter];NSString *s=self.field.stringValue;
  if((NSInteger)s.length<self.minUnits||!s.length){NSBeep();return YES;}[self finish:s];return YES;}
 if(sel==@selector(cancelOperation:)){[self finish:nil];return YES;}
 return NO;
}
@end
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
  NSView *cv=window.contentView;CGFloat w=fmin(720,cv.bounds.size.width-40),h=104;
  TOTKTextBar *bar=[[TOTKTextBar alloc] initWithFrame:NSMakeRect(floor((cv.bounds.size.width-w)/2),48,w,h)];
  bar.autoresizingMask=NSViewMinXMargin|NSViewMaxXMargin|NSViewMaxYMargin;bar.wantsLayer=YES;bar.layer.cornerRadius=12;
  bar.layer.backgroundColor=[NSColor colorWithCalibratedRed:0.035 green:0.075 blue:0.09 alpha:0.92].CGColor;
  bar.maxUnits=maxU;bar.minUnits=minU;bar.digitsOnly=digits;bar.disabled=dis;
  NSTextField *label=[NSTextField labelWithString:prompt];label.font=[NSFont systemFontOfSize:16 weight:NSFontWeightSemibold];
  label.textColor=NSColor.whiteColor;label.frame=NSMakeRect(18,h-36,w-36,22);label.lineBreakMode=NSLineBreakByTruncatingTail;
  NSString *limit=maxU>0?[NSString stringWithFormat:@(TOTK_UI("最多 %d 个字 · ","Up to %d characters · ")),maxU]:@"";
  NSTextField *hint=[NSTextField labelWithString:[NSString stringWithFormat:@(TOTK_UI("%@用键盘输入，回车确定，Esc 取消","%@Type with the keyboard, Return to confirm, Esc to cancel")),limit]];
  hint.font=[NSFont systemFontOfSize:12];hint.textColor=[NSColor colorWithCalibratedWhite:0.72 alpha:1];hint.alignment=NSTextAlignmentRight;
  hint.frame=NSMakeRect(18,h-36,w-36,20);hint.autoresizingMask=NSViewWidthSizable;
  NSTextField *field=secure?[[NSSecureTextField alloc] initWithFrame:NSMakeRect(18,16,w-36,34)]:[[NSTextField alloc] initWithFrame:NSMakeRect(18,16,w-36,34)];
  field.font=[NSFont systemFontOfSize:20];field.stringValue=initial;field.delegate=bar;field.bezelStyle=NSTextFieldRoundedBezel;bar.field=field;
  __weak TOTKTextBar *weakBar=bar;
  bar.done=^(NSString *text){result=[text copy];[weakBar removeFromSuperview];runtime_gamepad_text_capture(0);
   [window makeFirstResponder:window.contentView];textEntryOpen=0;dispatch_semaphore_signal(sem);};
  [bar addSubview:label];[bar addSubview:hint];[bar addSubview:field];[cv addSubview:bar];openBar=bar;
  if(settingsOpen)settings_show(0);
  runtime_input_clear();runtime_gamepad_text_capture(1);textEntryOpen=1;
  if(window.miniaturized)[window deminiaturize:nil];
  if(!assume_focus()){[NSApp activateIgnoringOtherApps:YES];[window makeKeyAndOrderFront:nil];}   /* test windows never take the keyboard */
  [window makeFirstResponder:field];
  fprintf(stderr,"[totk] keyboard: text entry bar shown (%s)\n",prompt.UTF8String);
 });
 dispatch_semaphore_wait(sem,DISPATCH_TIME_FOREVER);
 if(!result)return 0;
 snprintf(out,out_size,"%s",result.UTF8String?result.UTF8String:"");
 return 1;
}
static int viReady;
static unsigned displayIdentity,layerIdentity;
static void vi_initialize(void){if(!metalLayer)runtime_fail("VI requires native macOS window");viReady=1;fprintf(stderr,"[totk] native VI display initialized\n");}
static uint32_t vi_open(void **out){if(!viReady||!out)return 0xf601;*out=&displayIdentity;return 0;}
static uint32_t vi_layer(void **out,void *display){if(!out||display!=&displayIdentity)return 0xf601;*out=&layerIdentity;return 0;}
static uint32_t vi_window(void **out,void *layer){if(!out||layer!=&layerIdentity)return 0xf601;*out=front_metal_layer();return 0;}
static uint32_t vi_vsync(void *ev,void *display){
 if(!ev||display!=&displayIdentity)return 0xf601;hle_bind_system_event(ev,1);
 pthread_mutex_lock(&vsyncLock);if(vsyncCount>=16)runtime_fail("VI display event limit");vsyncEvents[vsyncCount++]=ev;pthread_mutex_unlock(&vsyncLock);return 0;
}
/* Echoes of Wisdom crops its layer (nn::vi::SetLayerCrop); the Switch compositor then shows only that part of each
   buffer. Recorded and logged for now; the present path uses NVN's window crop (nvnWindowSetCrop) only. */
static int layerCrop[4];
static void vi_layer_crop(void *layer,int x,int y,int w,int h){
 if(layer!=&layerIdentity)runtime_fail("SetLayerCrop on an unknown layer");
 int v[4]={x,y,w,h};if(memcmp(v,layerCrop,sizeof v)){memcpy(layerCrop,v,sizeof v);fprintf(stderr,"[totk] VI layer crop %d,%d %dx%d (not applied)\n",x,y,w,h);}
}
static void vi_destroy_layer(void *layer){if(layer!=&layerIdentity)runtime_fail("DestroyLayer on an unknown layer");}
static void vi_close_display(void *display){if(display!=&displayIdentity)runtime_fail("CloseDisplay on an unknown display");}
static void vi_finalize(void){}
void *runtime_front_override(const char *name){
 static const struct{const char *name;void *fn;} entries[]={
 {"_ZN2nn2vi12SetLayerCropEPNS0_5LayerEiiii",vi_layer_crop},{"_ZN2nn2vi12DestroyLayerEPNS0_5LayerE",vi_destroy_layer},
 {"_ZN2nn2vi12CloseDisplayEPNS0_7DisplayE",vi_close_display},{"_ZN2nn2vi8FinalizeEv",vi_finalize},
 {"_ZN2nn2vi10InitializeEv",vi_initialize},{"_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE",vi_open},
 {"_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE",vi_layer},{"_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE",vi_window},
 {"_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE",vi_vsync}};
 for(unsigned i=0;i<sizeof(entries)/sizeof(*entries);i++)if(!strcmp(name,entries[i].name))return entries[i].fn;return NULL;
}
