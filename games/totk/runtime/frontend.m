/* Native macOS window and VI service adapter. No guest frame is synthesized. */
#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CATransaction.h>
#import <QuartzCore/CAShapeLayer.h>
#import <Metal/Metal.h>
#import <CoreVideo/CoreVideo.h>
#include <stdatomic.h>
#include <math.h>
#include "runtime.h"
#include "game_version.h"
#include "totk_ui_lang.h"
#include "gamepad_layout.h"
#import "playtime.h"
#include "../third_party/d2r_gfx/gfx/nvn/totk_mfx_mode.h"
#include "../third_party/d2r_gfx/gfx/nvn/fps_overlay.h"
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
static _Atomic int titleFree;   /* runtime_warm_title: the warm-up no longer holds the title back */
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
static pthread_mutex_t vsyncLock=PTHREAD_MUTEX_INITIALIZER;
static void *vsyncEvents[16];
static unsigned vsyncCount;
void hle_bind_system_event(void *,int);
void hle_signal_system_event(void *);
/* TOTK_STATE_REPORTING (gfx/nvn/totk_state_report.m; absent in tests): the game's states for Apple's performance tools on
   macOS 27. Main-thread events only; each call stores a few values and returns. */
extern void totk_sr_start(const char *game,const char *version) __attribute__((weak_import));
extern void totk_sr_picture(int key,int visible) __attribute__((weak_import));
extern void totk_sr_window(int key,int visible) __attribute__((weak_import));
extern void totk_sr_view(int w,int h,int display_hz,int fullscreen) __attribute__((weak_import));
extern void totk_sr_closing(void) __attribute__((weak_import));
extern void totk_sr_settings(int open) __attribute__((weak_import));
int runtime_front_window_visible(void);   /* below: 1 seen, 0 covered, -1 not known yet */
void *front_metal_layer(void){return (__bridge void *)metalLayer;}
/* The start-up screen goes once a frame was presented and the warm-up is done, or lets the title come before it ends
   (runtime_warm_title): the player sees the game from here on. */
static void boot_overlay_hide(void){
 if(!bootOverlay||bootOverlay.hidden)return;
 bootOverlay.hidden=YES;
 fprintf(stderr,"[totk-boot] %.3f start-up screen hidden\n",(runtime_now_ns()-bootStartNS)/1e9);
 if(totk_sr_picture)totk_sr_picture(window.isKeyWindow,runtime_front_window_visible()!=0);
}
void front_frame_presented(void){
 if(atomic_fetch_add_explicit(&presentedFrames,1,memory_order_relaxed)==0){
  fprintf(stderr,"[totk-boot] %.3f first presented frame\n",(runtime_now_ns()-bootStartNS)/1e9);
  dispatch_async(dispatch_get_main_queue(), ^{if(!atomic_load(&warmPending)||atomic_load(&titleFree))boot_overlay_hide();});
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
/* nvn_mtl.m warm_start, where the game's sound starts: the title may be seen. With the rest of the warm-up going on behind
   the menu (a Mac with a complete pipeline archive, TOTK_WARM_BEFORE_TITLE=0) the start-up screen covered a title that
   already played its music and took input until the warm-up ended (2026-10-30); it now goes with the first frame. */
void runtime_warm_title(void){
 atomic_store(&titleFree,1);
 dispatch_async(dispatch_get_main_queue(), ^{if(atomic_load(&presentedFrames))boot_overlay_hide();});
}
uint64_t front_game_res(int docked){(void)docked;return (720ULL<<32)|1280;}
/* TOTK_ASSUME_FOCUS=1: unattended test runs. macOS refuses to activate an app launched from the background while the user is working in
   another one; the window then never becomes key, GetCurrentFocusState stays 0 and the game waits before its first frame. In this mode the
   game is told it is focused, and the window is shown without taking the keyboard from the user (scripted input does not need it). */
static int assume_focus(void){static int v=-1;if(v<0){const char *e=getenv("TOTK_ASSUME_FOCUS");v=e&&atoi(e);}return v;}
/* Keep the guest active when the macOS window loses key status. Window keys and mouse input are cleared
   in windowDidResignKey so they cannot stick; physical controllers follow their saved background-input choice. */
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
  window.acceptsMouseMovedEvents=((TOTKInputView *)window.contentView).cursorAutoHideActive;
 }
 fprintf(stderr,"[totk] mouse %s\n",want?"held by the window (the settings panel, key 1, or Command-Tab lets it go)":"let go");
}
enum { FPS_GRAPH_SAMPLES = 61, FPS_GRAPH_WIDTH = 220, FPS_GRAPH_HEIGHT = 90 };
@interface TOTKHudBox:NSView
@property(strong) NSTextField *label;
@property(strong) NSTimer *timer;
@property NSSize textSize;
-(void)position;
-(void)refresh;
@end
/* Compact HUD (default): one small box in the window's top-right corner with the FPS, shader and load lines (published by
   gfx/nvn/fps_overlay.c, refreshed twice a second); the settings panel's overlay row picks all, FPS + playtime or none.
   TOTK_HUD=corners: the lines stay in the picture's corners and there is no box. */
static int settings_pad_family(void);                 /* settings_panel.h: the controller family button names follow */
static NSString *settings_panel_pad_text(int family);  /* settings_panel.h: the controller's way to the panel */
/* The box's last line (user 2026-10-30): how to open the settings panel, as it is bound now: key 1, and with a
   controller connected its button or chord (L3 + R3 unless the key bindings set one). */
static NSString *hud_settings_hint(void) {
 int detected=-1;runtime_gamepad_type(&detected);
 if(detected<=GAMEPAD_TYPE_AUTO)return @(TOTK_UI("设置：键盘 1","Settings: key 1"));
 return [NSString stringWithFormat:@(TOTK_UI("设置：键盘 1 · 手柄 %@","Settings: key 1 · controller %@")),settings_panel_pad_text(settings_pad_family())];
}
@implementation TOTKHudBox
-(instancetype)initWithFrame:(NSRect)frame{if((self=[super initWithFrame:frame])){self.wantsLayer=YES;self.layer.backgroundColor=[NSColor colorWithCalibratedWhite:0.04 alpha:0.72].CGColor;self.layer.cornerRadius=7;self.layer.borderWidth=1;self.layer.borderColor=[NSColor colorWithCalibratedWhite:1 alpha:0.14].CGColor;
 self.label=[NSTextField labelWithString:@""];self.label.alignment=NSTextAlignmentLeft;self.label.maximumNumberOfLines=0;self.label.lineBreakMode=NSLineBreakByClipping;[self addSubview:self.label];
 [self refresh];if(hud_compact()){__weak TOTKHudBox *weak=self;self.timer=[NSTimer scheduledTimerWithTimeInterval:0.5 repeats:YES block:^(NSTimer *t){(void)t;[weak refresh];}];}}return self;}
-(void)refresh{
 BOOL showText=hud_compact()&&fps_overlay_hud_mode()!=HUD_MODE_OFF;
 self.hidden=!showText&&!fpsGraphEnabled;self.label.hidden=!showText;
 if(showText){
  NSFont *mono=[NSFont monospacedSystemFontOfSize:10.5 weight:NSFontWeightMedium];
  NSColor *green=[NSColor colorWithCalibratedRed:80/255.0 green:1 blue:120/255.0 alpha:1],*yellow=[NSColor colorWithCalibratedRed:1 green:230/255.0 blue:40/255.0 alpha:1];
  NSMutableAttributedString *a=[NSMutableAttributedString new];
  int full=fps_overlay_full_hud();   // FPS only: the FPS line (line 0); the others keep their last text, so skip them
  for(int i=0;i<4;i++){char t[128];int hl=0;if((!full&&i!=0)||!hud_snapshot(i,t,sizeof t,&hl))continue;   // FPS, shaders, render pipelines, load
   [a appendAttributedString:[[NSAttributedString alloc] initWithString:[([NSString stringWithUTF8String:t] ?: @"") stringByAppendingString:@"\n"] attributes:@{NSFontAttributeName:mono,NSForegroundColorAttributeName:hl?yellow:green}]];}
  if(!a.length)[a appendAttributedString:[[NSAttributedString alloc] initWithString:@"FPS --\n" attributes:@{NSFontAttributeName:mono,NSForegroundColorAttributeName:green}]];
  if(playtime)[a appendAttributedString:[[NSAttributedString alloc] initWithString:[playtime.text stringByAppendingString:@"\n"]
   attributes:@{NSFontAttributeName:mono,NSForegroundColorAttributeName:green}]];
  int bpct=runtime_gamepad_battery_percent(),bleft=runtime_gamepad_battery_left_percent();
  if(bpct>=0)[a appendAttributedString:[[NSAttributedString alloc] initWithString:bleft>=0?[NSString stringWithFormat:@(TOTK_UI("手柄电量 左 %d%% 右 %d%%\n","Controller L %d%% R %d%%\n")),bleft,bpct]:[NSString stringWithFormat:@(TOTK_UI("手柄电量 %d%%\n","Controller %d%%\n")),bpct]
   attributes:@{NSFontAttributeName:mono,NSForegroundColorAttributeName:green}]];
  /* The commit the app was built from (user 2026-10-25), last and grey: TOTK_SOURCE_COMMIT from the launcher. */
  const char *commit=getenv("TOTK_SOURCE_COMMIT");
  if(commit&&*commit)[a appendAttributedString:[[NSAttributedString alloc] initWithString:[NSString stringWithFormat:@(TOTK_UI("版本 %s\n","build %s\n")),commit]
   attributes:@{NSFontAttributeName:mono,NSForegroundColorAttributeName:[NSColor colorWithCalibratedWhite:0.6 alpha:1]}]];
  [a appendAttributedString:[[NSAttributedString alloc] initWithString:[hud_settings_hint() stringByAppendingString:@"\n"]
   attributes:@{NSFontAttributeName:mono,NSForegroundColorAttributeName:[NSColor colorWithCalibratedWhite:0.6 alpha:1]}]];
  [a deleteCharactersInRange:NSMakeRange(a.length-1,1)];   // no empty line under the last one
  BOOL changed=![a isEqualToAttributedString:self.label.attributedStringValue];
  if(changed)self.label.attributedStringValue=a;
  if(changed||self.textSize.height==0){
   NSSize z=[self.label sizeThatFits:NSMakeSize(CGFLOAT_MAX,CGFLOAT_MAX)];
   self.textSize=NSMakeSize(ceil(z.width),ceil(z.height));
  }
 }else self.textSize=NSZeroSize;
 [self position];}
-(NSView *)hitTest:(NSPoint)p{(void)p;return nil;}
-(void)viewDidMoveToSuperview{[super viewDidMoveToSuperview];[self position];}
-(void)layout{[super layout];[self position];}
-(void)position{if(!self.superview)return;
 CGFloat graphH=fpsGraphEnabled?FPS_GRAPH_HEIGHT:0,graphW=fpsGraphEnabled?FPS_GRAPH_WIDTH:0;
 CGFloat contentW=fmax(self.textSize.width,graphW);
 CGFloat w=contentW+16,h=self.textSize.height+graphH+10,m=10;
 NSRect f=NSMakeRect(floor(self.superview.bounds.size.width-w-m),floor(self.superview.bounds.size.height-h-m),w,h);
 if(!NSEqualRects(self.frame,f))self.frame=f;
 self.label.frame=NSMakeRect(8,graphH+5,self.textSize.width,self.textSize.height);
 if(fpsGraphEnabled)fpsGraphBox.frame=NSMakeRect(8,5,contentW,FPS_GRAPH_HEIGHT);}
@end
@interface TOTKFpsGraph:NSView {
 int _samples[FPS_GRAPH_SAMPLES];
 NSUInteger _sampleCount;
}
@property(strong) NSTimer *timer;
-(void)setEnabled:(BOOL)enabled;
-(void)sample;
@end
@implementation TOTKFpsGraph
-(instancetype)initWithFrame:(NSRect)frame {
 if((self=[super initWithFrame:frame]))self.hidden=YES;
 return self;
}
-(void)setEnabled:(BOOL)enabled {
 self.hidden=!enabled;
 [self.timer invalidate];
 self.timer=nil;
 _sampleCount=0;
 if(!enabled)return;

 [self sample];
 __weak TOTKFpsGraph *weak=self;
 self.timer=[NSTimer scheduledTimerWithTimeInterval:0.5 repeats:YES block:^(NSTimer *t){(void)t;[weak sample];}];
}
-(void)sample {
 int fps=fps_overlay_value();
 if(_sampleCount<FPS_GRAPH_SAMPLES)_samples[_sampleCount++]=fps;
 else {
  for(NSUInteger i=1;i<FPS_GRAPH_SAMPLES;i++)_samples[i-1]=_samples[i];
  _samples[FPS_GRAPH_SAMPLES-1]=fps;
 }
 [self setNeedsDisplay:YES];
}
-(void)drawRect:(NSRect)dirtyRect {
 (void)dirtyRect;
 int minFPS=-1,maxFPS=-1;
 for(NSUInteger i=0;i<_sampleCount;i++) {
  int fps=_samples[i];
  if(fps<0)continue;
  if(minFPS<0||fps<minFPS)minFPS=fps;
  if(fps>maxFPS)maxFPS=fps;
 }

 int currentFPS=_sampleCount?_samples[_sampleCount-1]:-1;
 NSString *currentText=currentFPS<0?@"--":[NSString stringWithFormat:@"%d",currentFPS];
 NSString *stats=maxFPS<0?@"min --   max --   cur --":
  [NSString stringWithFormat:@"min %d   max %d   cur %@",minFPS,maxFPS,currentText];
 NSDictionary *statsStyle=@{NSFontAttributeName:[NSFont monospacedSystemFontOfSize:9 weight:NSFontWeightMedium],
  NSForegroundColorAttributeName:[NSColor colorWithCalibratedWhite:0.88 alpha:1]};
 [stats drawAtPoint:NSMakePoint(8,58) withAttributes:statsStyle];
 if(maxFPS<0)return;

 NSRect plot=NSMakeRect(40,12,self.bounds.size.width-50,36);
 double axisLow=0,axisHigh=fmax(30,ceil(maxFPS*1.10/30)*30);
 double axisMid=(axisLow+axisHigh)/2;

 NSColor *axisColor=[NSColor colorWithCalibratedWhite:0.75 alpha:0.65];
 NSColor *gridColor=[NSColor colorWithCalibratedWhite:0.75 alpha:0.18];
 NSBezierPath *grid=[NSBezierPath bezierPath];
 CGFloat midY=NSMinY(plot)+(axisMid-axisLow)/(axisHigh-axisLow)*plot.size.height;
 [grid moveToPoint:NSMakePoint(NSMinX(plot),midY)];
 [grid lineToPoint:NSMakePoint(NSMaxX(plot),midY)];
 [gridColor setStroke];
 grid.lineWidth=0.5;
 [grid stroke];

 NSBezierPath *axis=[NSBezierPath bezierPath];
 [axis moveToPoint:NSMakePoint(35,NSMinY(plot))];
 [axis lineToPoint:NSMakePoint(35,NSMaxY(plot))];
 NSDictionary *axisStyle=@{NSFontAttributeName:[NSFont monospacedSystemFontOfSize:8 weight:NSFontWeightRegular],
  NSForegroundColorAttributeName:axisColor};
 double axisLabels[]={axisLow,axisMid,axisHigh};
 for(int i=0;i<3;i++) {
  CGFloat y=NSMinY(plot)+(axisLabels[i]-axisLow)/(axisHigh-axisLow)*plot.size.height;
  [axis moveToPoint:NSMakePoint(35,y)];
  [axis lineToPoint:NSMakePoint(39,y)];
  [[NSString stringWithFormat:@"%.0f",axisLabels[i]] drawAtPoint:NSMakePoint(8,y-5) withAttributes:axisStyle];
 }
 [axisColor setStroke];
 axis.lineWidth=0.75;
 [axis stroke];

 NSBezierPath *line=[NSBezierPath bezierPath];
 NSPoint last=NSZeroPoint;
 NSUInteger points=0;
 for(NSUInteger i=0;i<_sampleCount;i++) {
  int fps=_samples[i];
  if(fps<0)continue;
  double xFraction=(double)i/(FPS_GRAPH_SAMPLES-1);
  double yFraction=fmax(0,fmin(1,(fps-axisLow)/(axisHigh-axisLow)));
  NSPoint point=NSMakePoint(NSMinX(plot)+xFraction*plot.size.width,
                            NSMinY(plot)+yFraction*plot.size.height);
  if(points)[line lineToPoint:point];else[line moveToPoint:point];
  last=point;
  points++;
 }

 NSColor *totk_colour=[NSColor colorWithCalibratedRed:0.541 green:0.969 blue:0.588 alpha:1];
 [totk_colour setStroke];
 [totk_colour setFill];
 line.lineWidth=1.5;
 [line stroke];
 if(points==1)[[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(last.x-1,last.y-1,2,2)] fill];
}
@end
/* zeldaVR: the headset's picture on the Mac window (user 2026-10-29: "屏幕 显示 类似vr 那种 两个屏幕"): both eyes side by side,
   the newest image the VR thread handed to the streamer (gfx/nvn/totk_vr.m vr_mirror_copy, an IOSurface, +1), as this
   view's layer contents over the game's picture, 60 times a second; hidden while there is none (VR off, not connected,
   or the VR page's "Mac 屏幕" set to the game's picture). Clicks go through. */
void *vr_mirror_copy(void) __attribute__((weak_import));
void vr_mirror_crop(double out[4]);   /* gfx/nvn/totk_vr.m: the part of the image to show (x, y, w, h; 0..1) */
double vr_mirror_aspect(void);   /* gfx/nvn/totk_vr.m: the whole image's width / height */
int vr_quest_view(void);   /* gfx/nvn/totk_vr.m: each eye inside its lens opening ("显示 Quest 画面") */
@interface TOTKVRMirror:NSView
@property(strong) NSTimer *timer;
@property void *shown;
@property(strong) CALayer *picture;       /* the eyes, aspect-fit in the view (with "显示 Quest 画面": the left eye) */
@property(strong) CALayer *picture2;      /* with "显示 Quest 画面": the right eye */
@property(strong) CAShapeLayer *lens,*lens2;   /* each eye's lens opening (user 2026-10-30: "mac上 显示 比quest 多的 画面") */
@property int lensOn;
@end
@implementation TOTKVRMirror
-(instancetype)initWithFrame:(NSRect)frame{
 if((self=[super initWithFrame:frame])){self.wantsLayer=YES;self.layer.backgroundColor=NSColor.blackColor.CGColor;self.hidden=YES;
  self.picture=[CALayer layer];self.picture.contentsGravity=kCAGravityResize;[self.layer addSublayer:self.picture];
  self.lens=[CAShapeLayer layer];self.picture.mask=self.lens;
  self.picture2=[CALayer layer];self.picture2.contentsGravity=kCAGravityResize;self.picture2.hidden=YES;[self.layer addSublayer:self.picture2];
  self.lens2=[CAShapeLayer layer];self.picture2.mask=self.lens2;
  if(vr_mirror_copy){__weak TOTKVRMirror *weak=self;self.timer=[NSTimer scheduledTimerWithTimeInterval:1.0/60 repeats:YES block:^(NSTimer *t){(void)t;[weak tick];}];}}
 return self;
}
/* "显示 Quest 画面": the VR thread's own image for the window (gfx/nvn/totk_vr.m mac_view_encode): each eye's middle
   enlarged there (user 2026-10-30: "把 现在的 图像 中间的 地方 放大2倍显示", "设置里可以调 2倍 到 10 倍"; close to the
   Quest's own screen, whose lens predistortion enlarges the middle: a plain zoom, not the lens's curve), drawn from the
   game's picture at the pose it was rendered with; here each half goes in its own lens opening (k_quest_lens). */
/* Each eye's opening as the Quest 3 shows it (user 2026-11-01: "quest 遮罩 用这种 真实的效果 不是两个圆"): the outline of
   the area its own screen shows, from 59 of its screencaps (adb exec-out screencap, both eyes 4128x2208; pixels lit in
   60% of them, each eye's outermost lit pixel at 96 angles round its centre; tools/quest_lens_outline.py, 2026-11-01). Unit
   coordinates of the eye's half, y from the top. Before: an ellipse per eye. */
static const float k_quest_lens[2][192]={   /* x, y per point */{
  0.999,0.51,0.9999,0.5396,0.9987,0.5694,0.9992,0.5999,0.9994,0.6312,0.999,0.6634,0.9998,0.6975,0.9996,0.7331,0.9998,0.7713,0.9998,0.8125,0.9915,0.8514,0.9593,0.8737,
  0.926,0.8937,0.8944,0.9139,0.8606,0.9304,0.8272,0.9461,0.7941,0.9611,0.7594,0.9722,0.7258,0.9845,0.6909,0.9929,0.6557,0.9991,0.62,1,0.5844,0.9994,0.5499,1,
  0.5155,1,0.4812,0.9989,0.4465,0.9994,0.4113,0.9995,0.3753,0.9991,0.3376,0.9998,0.2985,0.9996,0.2664,0.982,0.2388,0.9579,0.2123,0.934,0.188,0.9088,0.1647,0.8838,
  0.1434,0.8577,0.1228,0.8319,0.1042,0.805,0.086,0.7782,0.0682,0.7513,0.054,0.7227,0.0401,0.694,0.0282,0.6646,0.0166,0.6349,0.0089,0.6041,0.0034,0.573,0.002,0.5414,
  0.0009,0.51,0.0001,0.4784,0.0015,0.4467,0.0013,0.4144,0.0016,0.3813,0.0006,0.3466,0.0006,0.3106,0.0087,0.2764,0.0362,0.2513,0.0634,0.2276,0.0888,0.2039,0.114,0.1809,
  0.1406,0.1596,0.1672,0.1388,0.1963,0.1212,0.2242,0.1025,0.2533,0.0856,0.2836,0.0705,0.3148,0.0572,0.3457,0.0425,0.3783,0.0314,0.4109,0.0187,0.445,0.0097,0.4798,0.0011,
  0.5155,0.0018,0.5512,0.0011,0.5872,0.0007,0.6238,0.0009,0.6612,0.0016,0.7002,0.0013,0.7406,0.0019,0.7723,0.0233,0.7999,0.0494,0.8262,0.0754,0.85,0.1025,0.8727,0.1293,
  0.893,0.1571,0.9111,0.1857,0.9299,0.2128,0.9449,0.2418,0.9611,0.2695,0.9752,0.2981,0.9873,0.3273,0.999,0.3565,0.9994,0.3888,0.9992,0.42,0.9987,0.4505,0.9999,0.4803},
{
  0.9981,0.5095,0.9989,0.5411,0.9994,0.573,0.992,0.604,0.9843,0.6349,0.9744,0.6652,0.9607,0.6942,0.9467,0.723,0.9308,0.7509,0.913,0.7777,0.8964,0.8056,0.8777,0.8326,
  0.8556,0.8573,0.8343,0.8834,0.8109,0.9083,0.7866,0.9335,0.7602,0.9574,0.7326,0.9815,0.7005,0.9991,0.6614,0.9993,0.6237,0.9986,0.5877,0.999,0.5524,0.9989,0.5178,0.9984,
  0.4835,0.9995,0.4492,0.9984,0.4146,0.9989,0.3793,0.999,0.3433,0.9986,0.3075,0.9941,0.274,0.9823,0.2396,0.9718,0.2058,0.959,0.1728,0.9441,0.1383,0.9299,0.1058,0.912,
  0.073,0.8932,0.0412,0.8721,0.009,0.8498,0.0024,0.81,0.0026,0.769,0.0029,0.731,0.0027,0.6956,0.0036,0.6617,0.0034,0.6297,0.0036,0.5987,0.0022,0.5687,0.003,0.5389,
  0.002,0.5095,0.003,0.48,0.0022,0.4503,0.0036,0.4203,0.0034,0.3892,0.0054,0.3578,0.0117,0.3268,0.0255,0.2984,0.0396,0.2699,0.0557,0.2423,0.0707,0.2134,0.0879,0.1852,
  0.1073,0.1579,0.1289,0.1315,0.1502,0.1034,0.1728,0.0749,0.2,0.0505,0.2276,0.0244,0.2591,0.0031,0.2988,0.0008,0.3378,0.0011,0.3752,0.0004,0.4118,0.0003,0.4478,0.0006,
  0.4835,0.0013,0.5192,0.0006,0.554,0.0093,0.5881,0.0182,0.6212,0.0291,0.6533,0.042,0.6849,0.0551,0.7154,0.07,0.7447,0.0867,0.7748,0.102,0.8038,0.1193,0.8317,0.1383,
  0.8583,0.1591,0.885,0.1804,0.9118,0.2023,0.9388,0.2251,0.9644,0.25,0.9903,0.2759,0.9984,0.3102,1,0.3456,1,0.3798,1,0.4132,1,0.4458,0.9989,0.4779}};
static CGPathRef mirror_path(CGSize z,int eye){   /* +1; eye -1: the whole rectangle */
 CGMutablePathRef p=CGPathCreateMutable();
 if(eye<0||eye>1){CGPathAddRect(p,NULL,CGRectMake(0,0,z.width,z.height));return p;}
 for(int i=0;i<96;i++){const CGFloat x=k_quest_lens[eye][2*i]*z.width,y=(1-k_quest_lens[eye][2*i+1])*z.height;   /* the layer's y is up */
  if(i)CGPathAddLineToPoint(p,NULL,x,y);else CGPathMoveToPoint(p,NULL,x,y);}
 CGPathCloseSubpath(p);return p;
}
-(void)tick{
 void *s=vr_mirror_copy();
 if(!s){if(!self.hidden){self.hidden=YES;self.picture.contents=nil;self.picture2.contents=nil;self.shown=NULL;}return;}
 self.hidden=NO;
 [CATransaction begin];[CATransaction setDisableActions:YES];
 double c[4];vr_mirror_crop(c);   /* both eyes, or the left eye's middle at 16:9 */
 const int lens=c[2]>=0.99&&vr_quest_view();
 if(s!=self.shown){self.picture.contents=(__bridge id)s;self.picture2.contents=lens?(__bridge id)s:nil;self.shown=s;}
 /* the picture aspect-fit; with "显示 Quest 画面" each eye in its own layer, inside the opening its lens shows (k_quest_lens), black outside */
 double iw=c[2],ih=c[3],ar=ih>0?iw/ih*vr_mirror_aspect():16.0/9;NSSize b=self.bounds.size;double w=b.width,h=w/ar;if(h>b.height){h=b.height;w=h*ar;}
 CGRect fit=CGRectMake(floor((b.width-w)/2),floor((b.height-h)/2),w,h);
 CGRect crop=lens?CGRectMake(0,0,0.5,1):CGRectMake(c[0],c[1],c[2],c[3]),crop2=CGRectMake(0.5,0,0.5,1);   /* the eyes, unit coordinates */
 if(!CGRectEqualToRect(self.picture.contentsRect,crop))self.picture.contentsRect=crop;
 if(lens&&!CGRectEqualToRect(self.picture2.contentsRect,crop2))self.picture2.contentsRect=crop2;
 CGRect left=lens?CGRectMake(fit.origin.x,fit.origin.y,floor(w/2),h):fit,right=CGRectMake(fit.origin.x+floor(w/2),fit.origin.y,w-floor(w/2),h);
 if(!CGRectEqualToRect(self.picture.frame,left)||lens!=self.lensOn){
  if(lens!=self.lensOn)self.picture2.contents=lens?(__bridge id)s:nil;
  self.lensOn=lens;self.picture.frame=left;self.picture2.frame=right;self.picture2.hidden=!lens;
  CGPathRef p=mirror_path(left.size,lens?0:-1);self.lens.path=p;CGPathRelease(p);
  p=mirror_path(right.size,1);self.lens2.path=p;CGPathRelease(p);
 }
 [CATransaction commit];
 CFRelease(s);
}
-(NSView *)hitTest:(NSPoint)p{(void)p;return nil;}
@end
#include "settings_panel.h"
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
/* TOTK_STATE_REPORTING: the drawable, the refresh rate of the screen the window is on, full screen. */
static void front_sr_view(void){
 if(!totk_sr_view||!window||!metalLayer)return;
 CGSize px=metalLayer.drawableSize;
 totk_sr_view((int)px.width,(int)px.height,window.screen?(int)window.screen.maximumFramesPerSecond:0,(window.styleMask&NSWindowStyleMaskFullScreen)!=0);
}
static void front_sr_window(void){if(totk_sr_window&&window)totk_sr_window(window.isKeyWindow,runtime_front_window_visible()!=0);}
static void front_sync_drawable(void){
 NSView *v=window.contentView;if(!v||!metalLayer)return;
 CGFloat s=window.backingScaleFactor>0?window.backingScaleFactor:1,ts=drawable_scale();CGSize px=CGSizeMake(round(v.bounds.size.width*s*ts),round(v.bounds.size.height*s*ts));
 if(px.width<1||px.height<1||CGSizeEqualToSize(metalLayer.drawableSize,px))return;
 metalLayer.contentsScale=s;metalLayer.drawableSize=px;fprintf(stderr,"[totk] window drawable %.0fx%.0f (scale %.0f)\n",px.width,px.height,s);runtime_aspect_view(px.width,px.height);
 front_sr_view();
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
-(void)windowDidBecomeKey:(NSNotification *)n{(void)n;front_playtime_tick(NO);atomic_store(&focused,1);runtime_gamepad_focus(1);if(window.styleMask&NSWindowStyleMaskFullScreen)[(TOTKInputView *)window.contentView setCursorAutoHideEnabled:YES];runtime_oe_push_message(15);front_mouse_update();fprintf(stderr,"[totk] window gained focus\n");front_sr_window();}
-(void)windowDidResignKey:(NSNotification *)n{(void)n;front_playtime_tick(YES);atomic_store(&focused,0);[(TOTKInputView *)window.contentView setCursorAutoHideEnabled:NO];front_mouse_update();runtime_gamepad_focus(0);runtime_input_clear_window();runtime_oe_push_message(15);fprintf(stderr,"[totk] window lost focus\n");front_sr_window();}
/* The process ends with the window. Save writes go straight to the files (runtime/fs_host.c), so a close in the middle of a
   save waits (up to 15 s, the window already gone) until the game has written and committed it (runtime_fs_save_busy). */
-(void)windowWillClose:(NSNotification *)n{(void)n;front_playtime_tick(YES);
 if(totk_sr_closing)totk_sr_closing();
 [(TOTKInputView *)window.contentView setCursorAutoHideEnabled:NO];
 if(mouseHeld){mouseHeld=NO;CGAssociateMouseAndMouseCursorPosition(true);[NSCursor unhide];}
 runtime_triggers_release();   /* _exit skips atexit; a save can also keep the game running a while without its window */
 if(!runtime_fs_save_busy()){fprintf(stderr,"[totk] window closed; graceful guest shutdown pending\n");_exit(0);}
 fprintf(stderr,"[totk] window closed while the game is saving: quitting when the save is done\n");
 dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0),^{
  uint64_t t0=runtime_now_ns();while(runtime_fs_save_busy()&&runtime_now_ns()-t0<15000000000ull)usleep(20000);
  fprintf(stderr,"[totk] save %s after %.2f s; quitting\n",runtime_fs_save_busy()?"still in progress (gave up)":"done",(runtime_now_ns()-t0)/1e9);_exit(0);});
}
-(void)windowDidResize:(NSNotification *)n{(void)n;front_sync_drawable();}
-(void)windowDidEnterFullScreen:(NSNotification *)n{(void)n;[(TOTKInputView *)window.contentView setCursorAutoHideEnabled:window.isKeyWindow];front_sr_view();}
-(void)windowDidExitFullScreen:(NSNotification *)n{(void)n;[(TOTKInputView *)window.contentView setCursorAutoHideEnabled:NO];front_sr_view();}
-(void)windowDidChangeScreen:(NSNotification *)n{(void)n;front_sr_view();}
/* Whether any part of the window can be seen (TOTK, 2026-10-16 night): macOS stops the display link's callbacks for a
   fully covered window, so presents stop and per-picture measurements of that stretch are void; a test session's log
   says when (the user working in other apps covered the test windows of the frame interpolation tests). */
static _Atomic int windowVisible=-1;   /* -1 not known yet; read by the present path's stall lines (runtime_front_window_visible) */
static void note_occlusion(const char *when){int v=(window.occlusionState&NSWindowOcclusionStateVisible)!=0;atomic_store(&windowVisible,v);
 fprintf(stderr,"[totk] window %s%s (frame %llu)\n",v?"visible":"covered (or on another Space): no presents until it is visible again",when,(unsigned long long)dm_nvn_frame_count());
 front_sr_window();}
-(void)windowDidChangeOcclusionState:(NSNotification *)n{(void)n;note_occlusion("");}
-(void)windowDidChangeBackingProperties:(NSNotification *)n{(void)n;front_sync_drawable();}
@end
static TOTKWindowDelegate *windowDelegate;
int runtime_front_window_visible(void){return atomic_load(&windowVisible);}
void runtime_front_request_close(void){dispatch_async(dispatch_get_main_queue(), ^{[window performClose:nil];});}
/* Native app-menu actions must also work while a settings control owns first responder. */
@interface TOTKMenuActions:NSObject<NSMenuItemValidation>
-(void)openSettings:(id)sender;
-(void)toggleFullscreen:(id)sender;
-(void)chooseFrameLimit:(NSMenuItem *)sender;
@end
@implementation TOTKMenuActions
-(void)openSettings:(id)sender{(void)sender;settings_show(1);}
-(void)toggleFullscreen:(id)sender{(void)sender;[window toggleFullScreen:nil];}
-(void)chooseFrameLimit:(NSMenuItem *)sender{
 if(getenv("TOTK_MFX"))return;   /* the panel's frame lock row says "Set by TOTK_MFX" then: so does the menu */
 int fps=(int)sender.tag;nvn_fps_lock_set(fps);
 if(fps)dm_gfx_set_flag("mfx_interp",MFX_OFF);
 settings_save_frame();settings_refresh_all();
}
-(void)displayRate:(id)sender{(void)sender;}
-(BOOL)validateMenuItem:(NSMenuItem *)item{
 if(item.action==@selector(toggleFullscreen:))item.title=@((window.styleMask&NSWindowStyleMaskFullScreen)?TOTK_UI("退出全屏","Exit Full Screen"):TOTK_UI("进入全屏","Enter Full Screen"));
 if(item.action==@selector(chooseFrameLimit:)){item.state=nvn_fps_lock()==item.tag?NSControlStateValueOn:NSControlStateValueOff;if(getenv("TOTK_MFX"))return NO;}
 /* the screen the window is on now (another display, a changed refresh rate): read when the menu opens */
 if(item.action==@selector(displayRate:)){item.title=[NSString stringWithFormat:@(TOTK_UI("显示器刷新率：%ld Hz","Display refresh: %ld Hz")),(long)(window.screen?:NSScreen.mainScreen).maximumFramesPerSecond];return NO;}
 return window!=nil && !(item.action==@selector(openSettings:)&&textEntryOpen);
}
@end
static TOTKMenuActions *menuActions;
/* Use the window-close path, not terminate:, so Command-Q also waits for active saves. */
static void front_install_menu(void){
 menuActions=[TOTKMenuActions new];
 NSMenu *menu=[NSMenu new],*appMenu=[[NSMenu alloc] initWithTitle:@GAME_NAME];
 NSMenuItem *appItem=[[NSMenuItem alloc] initWithTitle:@GAME_NAME action:nil keyEquivalent:@""];
 appItem.submenu=appMenu;[menu addItem:appItem];
 NSMenuItem *settings=[[NSMenuItem alloc] initWithTitle:@(TOTK_UI("设置…","Settings…")) action:@selector(openSettings:) keyEquivalent:@","];
 settings.keyEquivalentModifierMask=NSEventModifierFlagCommand;settings.target=menuActions;[appMenu addItem:settings];
 [appMenu addItem:NSMenuItem.separatorItem];
 NSMenuItem *quit=[[NSMenuItem alloc] initWithTitle:@(TOTK_UI("退出 " GAME_NAME,"Quit " GAME_NAME)) action:@selector(performClose:) keyEquivalent:@"q"];
 quit.keyEquivalentModifierMask=NSEventModifierFlagCommand;quit.target=window;[appMenu addItem:quit];
 NSMenu *view=[[NSMenu alloc] initWithTitle:@(TOTK_UI("显示","View"))];
 NSMenuItem *viewItem=[[NSMenuItem alloc] initWithTitle:view.title action:nil keyEquivalent:@""];viewItem.submenu=view;[menu addItem:viewItem];
 NSMenuItem *full=[[NSMenuItem alloc] initWithTitle:@(TOTK_UI("进入全屏","Enter Full Screen")) action:@selector(toggleFullscreen:) keyEquivalent:@"f"];
 full.keyEquivalentModifierMask=NSEventModifierFlagControl|NSEventModifierFlagCommand;full.target=menuActions;[view addItem:full];
 NSMenu *rates=[[NSMenu alloc] initWithTitle:@(TOTK_UI("渲染帧率上限","Render Frame Limit"))];
 NSMenuItem *ratesItem=[[NSMenuItem alloc] initWithTitle:rates.title action:nil keyEquivalent:@""];ratesItem.submenu=rates;[view addItem:ratesItem];
 for(unsigned i=0;i<TOTK_FPS_LIMIT_COUNT;i++){   /* the panel's frame lock choices (totk_fps_limit.h), named as there */
  int fps=totk_fps_limits[i];
  NSString *title=fps?[NSString stringWithFormat:@(TOTK_UI("%d 帧","%d fps")),fps]:@(TOTK_UI("关","Off"));
  NSMenuItem *item=[[NSMenuItem alloc] initWithTitle:title action:@selector(chooseFrameLimit:) keyEquivalent:@""];
  item.tag=fps;item.target=menuActions;[rates addItem:item];
 }
 NSMenuItem *screen=[[NSMenuItem alloc] initWithTitle:@"" action:@selector(displayRate:) keyEquivalent:@""];
 screen.target=menuActions;[view addItem:screen];
 front_menubar_fps_add(menu);NSApp.mainMenu=menu;
}
/* execv can make Launch Services show the nested runtime as a generic Unix executable.
   Recover the enclosing app's own icon, including launches from the diagnostic command line. */
static void front_install_bundle_icon(void) {
 NSString *path=NSProcessInfo.processInfo.arguments.firstObject;
 if(!path.length)return;
 if(!path.isAbsolutePath)path=[NSFileManager.defaultManager.currentDirectoryPath stringByAppendingPathComponent:path];
 path=path.stringByResolvingSymlinksInPath;
 while(path.length>1){
  if([path.pathExtension.lowercaseString isEqualToString:@"app"]){
   NSBundle *bundle=[NSBundle bundleWithPath:path];
   NSString *name=[bundle objectForInfoDictionaryKey:@"CFBundleIconFile"];
   if(![name isKindOfClass:NSString.class]||!name.length)return;
   if(!name.pathExtension.length)name=[name stringByAppendingPathExtension:@"icns"];
   NSImage *icon=[[NSImage alloc] initWithContentsOfFile:[bundle.resourcePath stringByAppendingPathComponent:name]];
   if(icon)NSApp.applicationIconImage=icon;   /* the app's own AppIcon.icns, as the build made it (scripts/extract_app_icon.py) */
   return;
  }
  path=path.stringByDeletingLastPathComponent;
 }
}
static void *guest_main(void *callback){@autoreleasepool{((void(*)(void))callback)();}return NULL;}
void runtime_front_run(void(*callback)(void)){
 @autoreleasepool{
  bootStartNS=runtime_now_ns();
  {const char *commit=getenv("TOTK_SOURCE_COMMIT");fprintf(stderr,"[totk] built from commit %s\n",commit&&*commit?commit:"(not recorded)");}
#ifdef GAME_BOTW
  if(totk_sr_start)totk_sr_start("botw",TOTK_GAME_VERSION);   /* TOTK_STATE_REPORTING=1: starting */
#else
  if(totk_sr_start)totk_sr_start("totk",TOTK_GAME_VERSION);
#endif
  [NSApplication sharedApplication];[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
  front_install_bundle_icon();
  runtime_mouse_set_hook(front_mouse_update);
  runtime_gamepad_start();
  /* Default window: the docked output size, 1920x1080, scaled down (16:9 kept) when the screen's visible area is smaller. */
  /* TOTK_WINDOW_SIZE=<w>x<h> (test scripts only, e.g. the 2K output test; the desktop launcher never sets it): another starting size. */
  NSRect vis=NSScreen.mainScreen?NSScreen.mainScreen.visibleFrame:NSMakeRect(0,0,1920,1080+28);CGFloat ww=1920,wh=1080;
  {const char *e=getenv("TOTK_WINDOW_SIZE");int w=0,h=0;if(e&&sscanf(e,"%dx%d",&w,&h)==2&&w>=320&&h>=180&&w<=8192&&h<=8192){ww=w;wh=h;}}
  CGFloat k=fmin(1,fmin(vis.size.width/ww,(vis.size.height-28)/wh));ww=floor(ww*k);wh=floor(wh*k);
  window=[[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,ww,wh) styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskMiniaturizable|NSWindowStyleMaskResizable backing:NSBackingStoreBuffered defer:NO];
  window.releasedWhenClosed=NO;   /* a close that waits for a save (windowWillClose) must not free the window the static pointer still holds */
  /* An agent's test game (TOTK_SLOT_OWNER other than user) says so in its title: it plays a save copy and its script closes it. */
  const char *owner=getenv("TOTK_SLOT_OWNER"),*purpose=getenv("TOTK_SLOT_PURPOSE");
  window.title=owner&&*owner&&strcmp(owner,"user")?[NSString stringWithFormat:@(TOTK_UI("测试局（%s）Ver. %s：%s — 存档副本，进度不会存进你的存档，脚本会自己关闭","Test session (%s) Ver. %s: %s — save copy, progress is not saved to your save, the script closes it")),owner,TOTK_GAME_VERSION,purpose&&*purpose?purpose:TOTK_UI("代理测试","agent test")]:[NSString stringWithFormat:@GAME_NAME "  Mac — Ver. %s",TOTK_GAME_VERSION];
  windowDelegate=[TOTKWindowDelegate new];window.delegate=windowDelegate;
  front_install_menu();
  window.collectionBehavior|=NSWindowCollectionBehaviorFullScreenPrimary;
  window.tabbingMode=NSWindowTabbingModeDisallowed;
  window.contentView=[[TOTKInputView alloc] initWithFrame:NSMakeRect(0,0,ww,wh)];
  metalLayer=[CAMetalLayer layer];metalLayer.device=MTLCreateSystemDefaultDevice();metalLayer.pixelFormat=MTLPixelFormatBGRA8Unorm;metalLayer.drawableSize=CGSizeMake(ww,wh);runtime_aspect_view(ww,wh);
  metalLayer.framebufferOnly=NO;window.contentView.wantsLayer=YES;window.contentView.layer=metalLayer;
  bootOverlay=[[NSView alloc] initWithFrame:window.contentView.bounds];bootOverlay.autoresizingMask=NSViewWidthSizable|NSViewHeightSizable;
  bootOverlay.wantsLayer=YES;bootOverlay.layer.backgroundColor=[NSColor colorWithCalibratedRed:0.035 green:0.075 blue:0.09 alpha:1].CGColor;
  bootStage=[NSTextField labelWithString:@(TOTK_UI("正在启动海拉鲁","Starting Hyrule"))];
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
  front_playtime_start();
  /* The FPS, shader and load lines: in the corner box (compact HUD), or with TOTK_HUD=corners drawn into the game picture's
     corners by the present encoder (gfx/nvn/fps_overlay.c). The box sits under the opaque boot overlay until it is hidden. */
  {TOTKVRMirror *mirror=[[TOTKVRMirror alloc] initWithFrame:window.contentView.bounds];mirror.autoresizingMask=NSViewWidthSizable|NSViewHeightSizable;   /* zeldaVR */
   [window.contentView addSubview:mirror positioned:NSWindowBelow relativeTo:bootOverlay];}
  TOTKHudBox *box=[[TOTKHudBox alloc] initWithFrame:NSZeroRect];box.autoresizingMask=NSViewMinXMargin|NSViewMinYMargin;
  [window.contentView addSubview:box positioned:NSWindowBelow relativeTo:bootOverlay];hudBox=box;
  /* The settings panel's FPS graph switch, one for both games (user 2026-10-29: "改成两个游戏共用 默认打开"): fps-graph.txt
     in the folder above the game's ("on" / "off"), on when there is none. */
  {char word[16];fpsGraphEnabled=1;if(runtime_shared_setting_word("fps-graph.txt",word,sizeof word))fpsGraphEnabled=!!strncmp(word,"off",3);}
  TOTKFpsGraph *graph=[[TOTKFpsGraph alloc] initWithFrame:NSZeroRect];[box addSubview:graph];fpsGraphBox=graph;[graph setEnabled:fpsGraphEnabled];[box refresh];
  [window makeFirstResponder:window.contentView];[window center];front_sync_drawable();
  if(assume_focus()){[window orderFrontRegardless];}else{[window makeKeyAndOrderFront:nil];[NSApp activateIgnoringOtherApps:YES];}   /* in front of other apps' windows, keyboard left alone */
  front_sr_view();
  [NSNotificationCenter.defaultCenter addObserverForName:NSApplicationDidChangeScreenParametersNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n){(void)n;front_sr_view();}];   /* a refresh rate changed on the same screen */
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW,3*NSEC_PER_SEC),dispatch_get_main_queue(),^{note_occlusion(" 3 s after opening");});
  atomic_store(&focused,1);
  runtime_gamepad_focus(window.isKeyWindow||assume_focus());   /* a test window never becomes key (TOTK_ASSUME_FOCUS) */
  if(CVDisplayLinkCreateWithActiveCGDisplays(&displayLink)||CVDisplayLinkSetOutputCallback(displayLink,vsync_callback,NULL)||CVDisplayLinkStart(displayLink))runtime_fail("native display link initialization");
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
static void vi_initialize(void){if(!metalLayer)runtime_fail("VI requires native macOS window");viReady=1;fprintf(stderr,"[totk] native VI display initialized\n");}
static uint32_t vi_open(void **out){if(!viReady||!out)return 0xf601;*out=&displayIdentity;return 0;}
static uint32_t vi_layer(void **out,void *display){if(!out||display!=&displayIdentity)return 0xf601;*out=&layerIdentity;return 0;}
static uint32_t vi_window(void **out,void *layer){if(!out||layer!=&layerIdentity)return 0xf601;*out=front_metal_layer();return 0;}
static uint32_t vi_vsync(void *ev,void *display){
 if(!ev||display!=&displayIdentity)return 0xf601;hle_bind_system_event(ev,1);
 pthread_mutex_lock(&vsyncLock);if(vsyncCount>=16)runtime_fail("VI display event limit");vsyncEvents[vsyncCount++]=ev;pthread_mutex_unlock(&vsyncLock);return 0;
}
#ifdef GAME_BOTW
static uint32_t vi_scaling_mode(void *layer,int mode){(void)layer;(void)mode;return 0;}
#endif
void *runtime_front_override(const char *name){
 static const struct{const char *name;void *fn;} entries[]={
 {"_ZN2nn2vi10InitializeEv",vi_initialize},{"_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE",vi_open},
 {"_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE",vi_layer},{"_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE",vi_window},
 {"_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE",vi_vsync}
#ifdef GAME_BOTW
 ,{"_ZN2nn2vi19SetLayerScalingModeEPNS0_5LayerENS0_11ScalingModeE",vi_scaling_mode}   /* SDK 7: the window scales itself */
#endif
 };
 for(unsigned i=0;i<sizeof(entries)/sizeof(*entries);i++)if(!strcmp(name,entries[i].name))return entries[i].fn;return NULL;
}
