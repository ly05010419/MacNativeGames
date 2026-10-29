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
#include "totk_ui_lang.h"
#include "gamepad_layout.h"
#include "../third_party/d2r_gfx/gfx/nvn/totk_mfx_mode.h"
#include "../third_party/d2r_gfx/gfx/nvn/fps_overlay.h"
static NSWindow *window;
uint64_t dm_nvn_frame_count(void);   /* gfx/nvn/hle_nvn.c: game frames presented so far */
static CAMetalLayer *metalLayer;
static NSView *bootOverlay;
static NSTextField *bootStage,*bootDetail;
static NSProgressIndicator *bootProgress;
static NSView *hudBox;   /* the FPS / shader / load box (TOTKHudBox), hidden with TOTK_HUD=corners */
static NSView *fpsGraphBox;
static int fpsGraphEnabled;
/* The settings panel (key 1, settings_panel.h) and the text entry bar: while one is open the game gets no keyboard or
   mouse input. */
static int settingsOpen,textEntryOpen;
static void settings_show(int open);
static uint64_t bootStartNS;
static _Atomic uint64_t presentedFrames;
static _Atomic int warmPending;
static CVDisplayLinkRef displayLink;
static _Atomic int focused;
static pthread_mutex_t vsyncLock=PTHREAD_MUTEX_INITIALIZER;
static void *vsyncEvents[16];
static unsigned vsyncCount;
void hle_bind_system_event(void *,int);
void hle_signal_system_event(void *);
static void front_update_awake(void);
static int window_closing;
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
/* ODYSSEY_VSYNC_DIVIDE=N (task B1, measurement only; unset = 1, as before): signal the game's vsync events on every Nth
   display-link callback, e.g. 2 gives 60/s on a 120 Hz panel, to test whether game time depends on the vsync rate. */
static unsigned vsync_divide(void){static unsigned v;if(!v){const char *e=getenv("ODYSSEY_VSYNC_DIVIDE");int n=e?atoi(e):1;v=n>1&&n<=8?(unsigned)n:1;}return v;}
static CVReturn vsync_callback(CVDisplayLinkRef link,const CVTimeStamp *now,const CVTimeStamp *output,CVOptionFlags flags,CVOptionFlags *out,void *ctx){
 (void)link;(void)now;(void)output;(void)flags;(void)out;(void)ctx;
 static unsigned tick;if(vsync_divide()>1&&tick++%vsync_divide())return kCVReturnSuccess;
 /* The Switch display runs at 60 Hz and Animal Crossing paces its frames by VI vsync counts. Signalled on every refresh of a
    120 Hz panel, its loop ran ahead of its 30 Hz logic and filled the gaps by re-presenting the last scene without the
    UI pass: the HUD blinked. Keep a 60 Hz schedule whatever the Mac's refresh rate (as games/wonder in Kolkka's fork). */
 {static uint64_t next;uint64_t t=runtime_now_ns();const uint64_t period=16666667;
  if(next&&t+2000000<next)return kCVReturnSuccess;
  next=next&&t<next+3*period?next+period:t+period;}
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
@end
@implementation TOTKInputView
-(BOOL)acceptsFirstResponder{return YES;}
/* While the settings panel is open only key 1 (it closes the panel, runtime/input.c) and Esc (closes it here, not the
   game's B) do anything; releases still go through so nothing stays held. */
-(void)keyDown:(NSEvent *)e{[NSCursor setHiddenUntilMouseMoves:YES];if(textEntryOpen)return;if(settingsOpen&&e.keyCode!=18){if(e.keyCode==53)settings_show(0);return;}runtime_input_key(e.keyCode,1);}
-(void)keyUp:(NSEvent *)e{runtime_input_key(e.keyCode,0);}
/* Shift, Control and Option, left and right apart, are keys of the layout (keyboard_map.h); they come as flag changes.
   While the settings panel or text entry is open only their releases go through. */
-(void)flagsChanged:(NSEvent *)e{int down=front_modifier_down(e);if(down>=0&&(!(settingsOpen||textEntryOpen)||!down))runtime_input_key(e.keyCode,down);}
/* The mouse's buttons over the game picture are inputs of the layout (keyboard_map.h: the left talks, the right picks up,
   the wheel click the tool ring, the side buttons the d-pad); the pointer stays free (Animal Crossing has nothing to aim). */
-(void)mouseDown:(NSEvent *)e{(void)e;if(!settingsOpen&&!textEntryOpen)runtime_input_mouse(0,1);}
-(void)mouseUp:(NSEvent *)e{(void)e;runtime_input_mouse(0,0);}
-(void)rightMouseDown:(NSEvent *)e{(void)e;if(!settingsOpen&&!textEntryOpen)runtime_input_mouse(1,1);}   /* no context menu */
-(void)rightMouseUp:(NSEvent *)e{(void)e;runtime_input_mouse(1,0);}
-(void)otherMouseDown:(NSEvent *)e{if(!settingsOpen&&!textEntryOpen)runtime_input_mouse((int)e.buttonNumber,1);}   /* 2 middle, 3 and 4 the side buttons */
-(void)otherMouseUp:(NSEvent *)e{runtime_input_mouse((int)e.buttonNumber,0);}
@end
enum { FPS_GRAPH_SAMPLES = 61, FPS_GRAPH_WIDTH = 220, FPS_GRAPH_HEIGHT = 90 };
@interface TOTKHudBox:NSView
@property(strong) NSTextField *label;
@property(strong) NSTimer *timer;
@property NSSize textSize;
-(void)position;
-(void)refresh;
@end
/* Compact HUD (default): one small box in the window's top-right corner with the FPS, shader and load lines (published by
   gfx/nvn/fps_overlay.c, refreshed twice a second) and, when the settings panel turns it on, the FPS graph; the panel's
   overlay row picks all, FPS only or none. TOTK_HUD=corners: the lines stay in the picture's corners and there is no box
   (as games/totk; the key reminder this box had is gone with keys 2-4, reported in play, PR #1305). */
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
  int bpct=full?runtime_gamepad_battery_percent():-1;
  if(bpct>=0)[a appendAttributedString:[[NSAttributedString alloc] initWithString:[NSString stringWithFormat:@(TOTK_UI("手柄电量 %d%%\n","Controller %d%%\n")),bpct]
   attributes:@{NSFontAttributeName:mono,NSForegroundColorAttributeName:green}]];
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
#include "settings_panel.h"
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
-(void)windowDidBecomeKey:(NSNotification *)n{(void)n;atomic_store(&focused,1);runtime_oe_push_message(15);fprintf(stderr,"[totk] window gained focus\n");front_update_awake();}
-(void)windowDidResignKey:(NSNotification *)n{(void)n;atomic_store(&focused,0);runtime_input_clear();runtime_oe_push_message(15);fprintf(stderr,"[totk] window lost focus\n");front_update_awake();}
-(void)windowDidMiniaturize:(NSNotification *)n{(void)n;front_update_awake();}
-(void)windowDidDeminiaturize:(NSNotification *)n{(void)n;front_update_awake();}
/* The process ends with the window. Save writes go straight to the files (runtime/fs_host.c), so a close in the middle of a
   save waits (up to 15 s, the window already gone) until the game has written and committed it (runtime_fs_save_busy). */
-(void)windowWillClose:(NSNotification *)n{(void)n;
 window_closing=1;front_update_awake();
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
 fprintf(stderr,"[totk] window %s%s (frame %llu)\n",v?"visible":"covered (or on another Space): no presents until it is visible again",when,(unsigned long long)dm_nvn_frame_count());front_update_awake();}
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
 if(want){if(IOPMAssertionCreateWithName(kIOPMAssertionTypePreventUserIdleDisplaySleep,kIOPMAssertionLevelOn,CFSTR("Animal Crossing is being played"),&awakeAssertion)!=kIOReturnSuccess){fprintf(stderr,"[totk] 【红】display-sleep assertion could not be created\n");return;}}
 else IOPMAssertionRelease(awakeAssertion);
 awakeHeld=want;fprintf(stderr,"[totk] display sleep %s (frame %llu)\n",want?"held off while the game is visible":"allowed again",(unsigned long long)dm_nvn_frame_count());
}
/* The pointer hides over the game after 3 s without moving while the window is key (keyboard presses hide it at once,
   TOTKInputView keyDown); the first mouse move shows it again. Controller input arrives in gamepad.m, hence the poll. */
static void front_start_cursor_hiding(void){
 static NSPoint last;static int still;
 [NSTimer scheduledTimerWithTimeInterval:1 repeats:YES block:^(NSTimer *t){(void)t;NSPoint p=[NSEvent mouseLocation];
  if(!NSEqualPoints(p,last)){last=p;still=0;return;}
  if(++still==3&&window.isKeyWindow&&NSPointInRect(p,window.frame))[NSCursor setHiddenUntilMouseMoves:YES];}];
}
static TOTKWindowDelegate *windowDelegate;
int runtime_front_window_visible(void){return atomic_load(&windowVisible);}
void runtime_front_request_close(void){dispatch_async(dispatch_get_main_queue(), ^{[window performClose:nil];});}
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
 menu_item(appMenu,TOTK_UI("隐藏 Animal Crossing","Hide Animal Crossing"),@selector(hide:),@"h",NSEventModifierFlagCommand,NSApp);
 [appMenu addItem:NSMenuItem.separatorItem];
 menu_item(appMenu,TOTK_UI("退出 Animal Crossing","Quit Animal Crossing"),@selector(performClose:),@"q",NSEventModifierFlagCommand,window);
 NSMenuItem *windowItem=[NSMenuItem new];windowItem.submenu=windowMenu;[menu addItem:windowItem];
 menu_item(windowMenu,TOTK_UI("最小化","Minimize"),@selector(performMiniaturize:),@"m",NSEventModifierFlagCommand,window);
 menu_item(windowMenu,TOTK_UI("进入 / 退出全屏","Toggle Full Screen"),@selector(toggleFullScreen:),@"f",NSEventModifierFlagCommand|NSEventModifierFlagControl,window);
 NSApp.mainMenu=menu;NSApp.windowsMenu=windowMenu;
}
static void *guest_main(void *callback){@autoreleasepool{((void(*)(void))callback)();}return NULL;}
void runtime_front_run(void(*callback)(void)){
 @autoreleasepool{
  bootStartNS=runtime_now_ns();
  [NSApplication sharedApplication];[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
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
  window.title=owner&&*owner&&strcmp(owner,"user")?[NSString stringWithFormat:@(TOTK_UI("测试局（%s）Ver. %s：%s — 存档副本，进度不会存进你的存档，脚本会自己关闭","Test session (%s) Ver. %s: %s — save copy, progress is not saved to your save, the script closes it")),owner,TOTK_GAME_VERSION,purpose&&*purpose?purpose:TOTK_UI("代理测试","agent test")]:[NSString stringWithFormat:@"Animal Crossing: New Horizons — Ver. %s",TOTK_GAME_VERSION];
  windowDelegate=[TOTKWindowDelegate new];window.delegate=windowDelegate;
  front_install_menu();front_start_cursor_hiding();
  window.collectionBehavior|=NSWindowCollectionBehaviorFullScreenPrimary;
  window.contentView=[[TOTKInputView alloc] initWithFrame:NSMakeRect(0,0,ww,wh)];
  metalLayer=[CAMetalLayer layer];metalLayer.device=MTLCreateSystemDefaultDevice();metalLayer.pixelFormat=MTLPixelFormatBGRA8Unorm;metalLayer.drawableSize=CGSizeMake(ww,wh);
  metalLayer.framebufferOnly=NO;window.contentView.wantsLayer=YES;window.contentView.layer=metalLayer;
  bootOverlay=[[NSView alloc] initWithFrame:window.contentView.bounds];bootOverlay.autoresizingMask=NSViewWidthSizable|NSViewHeightSizable;
  bootOverlay.wantsLayer=YES;bootOverlay.layer.backgroundColor=[NSColor colorWithCalibratedRed:0.035 green:0.075 blue:0.09 alpha:1].CGColor;
  bootStage=[NSTextField labelWithString:@(TOTK_UI("正在启动 Animal Crossing: New Horizons","Starting Animal Crossing: New Horizons"))];
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
  /* BY3_FULLSCREEN=1 (the launcher's "Full screen"): macOS full screen from the start; the green button or ⌃⌘F leaves it. */
  {const char *fs=getenv("BY3_FULLSCREEN");if(fs&&!strcmp(fs,"1"))dispatch_async(dispatch_get_main_queue(),^{if(!(window.styleMask&NSWindowStyleMaskFullScreen))[window toggleFullScreen:nil];});}
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
static uint32_t vi_scale(void *layer,int mode){if(layer!=&layerIdentity)return 0xf601;(void)mode;return 0;}
static uint32_t vi_vsync(void *ev,void *display){
 if(!ev||display!=&displayIdentity)return 0xf601;hle_bind_system_event(ev,1);
 pthread_mutex_lock(&vsyncLock);if(vsyncCount>=16)runtime_fail("VI display event limit");vsyncEvents[vsyncCount++]=ev;pthread_mutex_unlock(&vsyncLock);return 0;
}
void *runtime_front_override(const char *name){
 static const struct{const char *name;void *fn;} entries[]={
 {"_ZN2nn2vi10InitializeEv",vi_initialize},{"_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE",vi_open},
 {"_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE",vi_layer},{"_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE",vi_window},
 {"_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE",vi_vsync},
 {"_ZN2nn2vi19SetLayerScalingModeEPNS0_5LayerENS0_11ScalingModeE",vi_scale}};
 for(unsigned i=0;i<sizeof(entries)/sizeof(*entries);i++)if(!strcmp(name,entries[i].name))return entries[i].fn;return NULL;
}
