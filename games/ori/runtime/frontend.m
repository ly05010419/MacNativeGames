/* Native macOS window and VI service adapter. No guest frame is synthesized. */
#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CATransaction.h>
#import <Metal/Metal.h>
#import <CoreVideo/CoreVideo.h>
#include <stdatomic.h>
#include <math.h>
#include "runtime.h"
#include "game_version.h"
#include "totk_ui_lang.h"
#include "../third_party/d2r_gfx/gfx/nvn/totk_mfx_mode.h"
int mfxup_output_enabled(void);
static NSWindow *window;
uint64_t dm_nvn_frame_count(void);   /* gfx/nvn/hle_nvn.c: game frames presented so far */
static CAMetalLayer *metalLayer;
static NSView *bootOverlay;
static NSTextField *bootStage,*bootDetail;
static NSProgressIndicator *bootProgress;
static NSView *shortcutHelp;
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
@interface TOTKGameWindow:NSWindow
@end
@implementation TOTKGameWindow
-(BOOL)canBecomeKeyWindow{return YES;}
-(BOOL)canBecomeMainWindow{return YES;}
@end
static int cheat_panel_key(NSEvent *e);
static NSView *cheatPanel;
@interface TOTKInputView:NSView
@end
@implementation TOTKInputView
-(BOOL)acceptsFirstResponder{return YES;}
-(void)keyDown:(NSEvent *)e{
 if(cheat_panel_key(e))return;   /* #22: the open cheat panel takes the keys first */
 unsigned mods=((e.modifierFlags&NSEventModifierFlagCommand)?1:0)|((e.modifierFlags&NSEventModifierFlagOption)?2:0)|((e.modifierFlags&NSEventModifierFlagControl)?4:0)|((e.modifierFlags&NSEventModifierFlagShift)?8:0);
 if(!e.isARepeat)runtime_input_key_modifiers(e.keyCode,1,mods);
}
-(void)keyUp:(NSEvent *)e{if(cheatPanel&&!cheatPanel.hidden)return;runtime_input_key_modifiers(e.keyCode,0,0);}
-(void)flagsChanged:(NSEvent *)e{if(e.keyCode==56)runtime_input_key(56,!!(e.modifierFlags&NSEventModifierFlagShift));if(e.keyCode==58)runtime_input_key(58,!!(e.modifierFlags&NSEventModifierFlagOption));}
@end
@interface TOTKShortcutHelp:NSView
@property(strong) NSTextField *label;
@property(strong) NSTimer *timer;
@property NSSize textSize;
-(void)position;
-(void)refresh;
@end
/* Compact HUD (default): live FPS, shader and load readings in the window's top-right corner,
   published by gfx/nvn/fps_overlay.c and refreshed twice a second. TOTK_HUD=corners
   keeps those readings in the game picture; shortcut discovery lives in Game Controls. */
@implementation TOTKShortcutHelp
/* Shortcut discovery lives in Game Controls; this panel keeps the upstream live readings. */
-(instancetype)initWithFrame:(NSRect)frame{if((self=[super initWithFrame:frame])){int compact=hud_compact();self.wantsLayer=YES;self.layer.backgroundColor=[NSColor colorWithCalibratedWhite:0.04 alpha:compact?0.72:0.86].CGColor;self.layer.cornerRadius=compact?7:14;self.layer.borderWidth=1;self.layer.borderColor=[NSColor colorWithCalibratedWhite:1 alpha:compact?0.14:0.2].CGColor;
 self.label=[NSTextField labelWithString:@""];self.label.font=[NSFont systemFontOfSize:17 weight:NSFontWeightMedium];self.label.textColor=NSColor.whiteColor;self.label.alignment=NSTextAlignmentCenter;self.label.maximumNumberOfLines=0;self.label.lineBreakMode=NSLineBreakByWordWrapping;self.label.frame=NSInsetRect(self.bounds,20,18);self.label.autoresizingMask=NSViewWidthSizable|NSViewHeightSizable;[self addSubview:self.label];
 if(compact){self.label.alignment=NSTextAlignmentLeft;self.label.lineBreakMode=NSLineBreakByClipping;[self refresh];
  __weak TOTKShortcutHelp *weak=self;self.timer=[NSTimer scheduledTimerWithTimeInterval:0.5 repeats:YES block:^(NSTimer *t){(void)t;[weak refresh];}];}}return self;}
-(void)refresh{
 NSFont *mono=[NSFont monospacedSystemFontOfSize:10.5 weight:NSFontWeightMedium];
 NSColor *green=[NSColor colorWithCalibratedRed:80/255.0 green:1 blue:120/255.0 alpha:1],*yellow=[NSColor colorWithCalibratedRed:1 green:230/255.0 blue:40/255.0 alpha:1];
 NSMutableAttributedString *a=[NSMutableAttributedString new];
 for(int i=0;i<3;i++){char t[128];int hl=0;if(!hud_snapshot(i,t,sizeof t,&hl))continue;
  [a appendAttributedString:[[NSAttributedString alloc] initWithString:[([NSString stringWithUTF8String:t] ?: @"") stringByAppendingString:@"\n"] attributes:@{NSFontAttributeName:mono,NSForegroundColorAttributeName:hl?yellow:green}]];}

 if([a isEqualToAttributedString:self.label.attributedStringValue])return;
 self.label.attributedStringValue=a;NSSize z=[self.label sizeThatFits:NSMakeSize(CGFLOAT_MAX,CGFLOAT_MAX)];self.textSize=NSMakeSize(ceil(z.width),ceil(z.height));[self position];}
-(NSView *)hitTest:(NSPoint)p{(void)p;return nil;}
-(void)viewDidMoveToSuperview{[super viewDidMoveToSuperview];[self position];}
-(void)layout{[super layout];[self position];}
-(void)position{if(!self.superview)return;NSRect f,in;
 if(hud_compact()){CGFloat w=self.textSize.width+16,h=self.textSize.height+10,m=10;f=NSMakeRect(floor(self.superview.bounds.size.width-w-m),floor(self.superview.bounds.size.height-h-m),w,h);in=NSMakeRect(8,5,self.textSize.width,self.textSize.height);}
 else{CGFloat w=MIN(560,MAX(280,self.superview.bounds.size.width-40)),h=190;f=NSMakeRect(floor((self.superview.bounds.size.width-w)/2),floor((self.superview.bounds.size.height-h)/2),w,h);in=NSInsetRect(NSMakeRect(0,0,w,h),20,18);}
 if(!NSEqualRects(self.frame,f))self.frame=f;self.label.frame=in;}
@end
void runtime_shortcut_help_toggle(void){int show=fps_overlay_toggle_hints();dispatch_async(dispatch_get_main_queue(), ^{shortcutHelp.hidden=!show;});}
/* Fixed output pixels are independent of window size. The window-sized fallback
   and TOTK_DRAWABLE_SCALE remain available for probes. WindowServer scales the
   drawable; the layer frame preserves the normal or ultrawide camera aspect. */
static CGFloat drawable_scale(void){static CGFloat f=-1;if(f<0){const char *e=getenv("TOTK_DRAWABLE_SCALE");f=e?atof(e):1;if(f<0.25||f>4)f=1;}return f;}
static void front_sync_drawable(void);
#include "display_settings.h"
static void front_sync_drawable(void){
 NSView *v=window.contentView;if(!v||!metalLayer)return;
 CGFloat s=window.backingScaleFactor>0?window.backingScaleFactor:1,ts=drawable_scale();
 CGSize px=displayResolution>=0?CGSizeMake(displaySizes[displayResolution][0],displaySizes[displayResolution][1]):CGSizeMake(round(v.bounds.size.width*s*ts),round(v.bounds.size.height*s*ts));
 if(px.width<1||px.height<1)return;
 CGRect frame=v.bounds;
 const char *stretch=getenv("TOTK_STRETCH");
 if(displayResolution>=0&&(!stretch||strcmp(stretch,"1"))){CGFloat aspect=runtime_aspect_now();if(aspect<=0)aspect=px.width/px.height;CGFloat height=fmin(frame.size.height,frame.size.width/aspect);CGSize size=CGSizeMake(height*aspect,height);frame=CGRectMake((frame.size.width-size.width)/2,(frame.size.height-size.height)/2,size.width,size.height);}
 [CATransaction begin];[CATransaction setDisableActions:YES];metalLayer.frame=frame;metalLayer.contentsScale=s;[CATransaction commit];
 if(CGSizeEqualToSize(metalLayer.drawableSize,px))return;
 metalLayer.drawableSize=px;
 fprintf(stderr,"[totk-display] drawable %.0fx%.0f (%s; window %.0fx%.0f points, backing %.0f)\n",px.width,px.height,displayResolution<0?"automatic":"fixed",v.bounds.size.width,v.bounds.size.height,s);
}
@interface TOTKWindowDelegate:NSObject<NSWindowDelegate,NSApplicationDelegate>
@end
@implementation TOTKWindowDelegate
-(void)applicationDidBecomeActive:(NSNotification *)n{(void)n;display_presentation();}
-(void)applicationDidResignActive:(NSNotification *)n{(void)n;display_presentation();}
-(void)windowDidBecomeKey:(NSNotification *)n{(void)n;atomic_store(&focused,1);runtime_oe_push_message(15);fprintf(stderr,"[totk] window gained focus\n");}
-(void)windowDidResignKey:(NSNotification *)n{(void)n;atomic_store(&focused,0);runtime_input_clear();runtime_oe_push_message(15);fprintf(stderr,"[totk] window lost focus\n");}
/* The process ends with the window. Save writes go straight to the files (runtime/fs_host.c), so a close in the middle of a
   save waits (up to 15 s, the window already gone) until the game has written and committed it (runtime_fs_save_busy). */
-(void)windowWillClose:(NSNotification *)n{(void)n;
 if(!runtime_fs_save_busy()){fprintf(stderr,"[totk] window closed; graceful guest shutdown pending\n");_exit(0);}
 fprintf(stderr,"[totk] window closed while the game is saving: quitting when the save is done\n");
 dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0),^{
  uint64_t t0=runtime_now_ns();while(runtime_fs_save_busy()&&runtime_now_ns()-t0<15000000000ull)usleep(20000);
  fprintf(stderr,"[totk] save %s after %.2f s; quitting\n",runtime_fs_save_busy()?"still in progress (gave up)":"done",(runtime_now_ns()-t0)/1e9);_exit(0);});
}
-(void)windowDidResize:(NSNotification *)n{(void)n;front_sync_drawable();}
-(void)windowDidChangeScreen:(NSNotification *)n{(void)n;if(displayBorderless)display_apply_mode();front_sync_drawable();}
-(void)windowWillEnterFullScreen:(NSNotification *)n{(void)n;displayTransition=YES;displayMode=TOTK_FULLSCREEN;display_save();}
-(void)windowDidEnterFullScreen:(NSNotification *)n{(void)n;displayTransition=NO;display_apply_mode();front_sync_drawable();}
-(void)windowWillExitFullScreen:(NSNotification *)n{(void)n;displayTransition=YES;if(displayMode==TOTK_FULLSCREEN){displayMode=TOTK_WINDOWED;display_save();}}
-(void)windowDidExitFullScreen:(NSNotification *)n{(void)n;displayTransition=NO;display_apply_mode();front_sync_drawable();}
-(void)windowDidFailToEnterFullScreen:(NSWindow *)w{(void)w;displayTransition=NO;displayMode=TOTK_WINDOWED;display_save();display_apply_mode();}
-(void)windowDidFailToExitFullScreen:(NSWindow *)w{(void)w;displayTransition=NO;displayMode=TOTK_FULLSCREEN;display_save();}
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
/* Cheats menu (runtime/cheats.c): one item per cheat baked into the app, rebuilt each time the menu opens. Code patches
   take effect at the next launch, so their items say so until then; refused cheats are listed, disabled, with the reason. */
@interface TOTKCheatMenu:NSObject<NSMenuDelegate>
@end
static NSString *cheat_title(int i){
 int st=runtime_cheats_state(i);const char *why=runtime_cheats_problem(i);
 NSString *title=@(runtime_cheats_name(i))?:@"?";
 if(st&CHEAT_REFUSED)return [title stringByAppendingFormat:@(TOTK_UI("（不支持：%s）"," (not supported: %s)")),why];
 if(why[0])return [title stringByAppendingFormat:@" (%s)",why];   // e.g. a code patch that conflicts with another
 if((st&CHEAT_AT_LAUNCH)&&!!(st&CHEAT_ON)!=!!(st&CHEAT_APPLIED))return [title stringByAppendingString:@(st&CHEAT_ON?TOTK_UI("（重启后生效）"," (applies after restart)"):TOTK_UI("（重启后关闭）"," (turns off after restart)"))];
 return title;
}
/* Code cheats switched since this launch: they apply (or turn off) only when the game starts again. */
static int cheats_pending(void){int k=0;for(int i=0;i<runtime_cheats_count();i++){int st=runtime_cheats_state(i);if((st&CHEAT_AT_LAUNCH)&&!(st&CHEAT_REFUSED)&&!!(st&CHEAT_ON)!=!!(st&CHEAT_APPLIED))k++;}return k;}
static NSString *cheats_pending_note(int k){return k==1?@(TOTK_UI("重启游戏以应用 1 项作弊码更改","Restart the game to apply 1 cheat change")):[NSString stringWithFormat:@(TOTK_UI("重启游戏以应用 %d 项作弊码更改","Restart the game to apply %d cheat changes")),k];}
static int cheats_any_on(void){for(int i=0;i<runtime_cheats_count();i++)if(runtime_cheats_state(i)&CHEAT_ON)return 1;return 0;}
static void cheats_all_off(void){for(int i=0;i<runtime_cheats_count();i++)if(runtime_cheats_state(i)&CHEAT_ON)runtime_cheats_set(i,0);}
@implementation TOTKCheatMenu
-(void)menuNeedsUpdate:(NSMenu *)menu{
 [menu removeAllItems];int n=runtime_cheats_count();
 if(!n){NSMenuItem *none=[[NSMenuItem alloc] initWithTitle:@(TOTK_UI("此 App 未内置作弊码","No cheats are built into this app")) action:nil keyEquivalent:@""];none.enabled=NO;[menu addItem:none];return;}
 for(int i=0;i<n;i++){
  int st=runtime_cheats_state(i);
  NSMenuItem *item=[[NSMenuItem alloc] initWithTitle:cheat_title(i) action:(st&CHEAT_REFUSED)?nil:@selector(toggle:) keyEquivalent:@""];
  item.target=self;item.tag=i;item.state=(st&CHEAT_ON)?NSControlStateValueOn:NSControlStateValueOff;item.enabled=!(st&CHEAT_REFUSED);[menu addItem:item];
 }
 [menu addItem:NSMenuItem.separatorItem];
 int k=cheats_pending();
 if(k){NSMenuItem *note=[[NSMenuItem alloc] initWithTitle:cheats_pending_note(k) action:nil keyEquivalent:@""];note.enabled=NO;[menu addItem:note];}
 NSMenuItem *off=[[NSMenuItem alloc] initWithTitle:@(TOTK_UI("关闭全部作弊码","Turn Off All Cheats")) action:@selector(allOff:) keyEquivalent:@""];
 off.target=self;off.enabled=cheats_any_on();[menu addItem:off];
}
-(void)allOff:(NSMenuItem *)item{(void)item;cheats_all_off();}
-(void)toggle:(NSMenuItem *)item{runtime_cheats_set((int)item.tag,!(runtime_cheats_state((int)item.tag)&CHEAT_ON));}
@end
static TOTKCheatMenu *cheatMenu;
/* Cheat panel (key 5): the same cheats as the menu over the game picture. While it is open the keyboard drives it
   (Up/Down select, Space/Return toggle, 5/Esc close) instead of the game, which keeps running; rows can be clicked. */
@interface TOTKFlippedView:NSView
@end
@implementation TOTKFlippedView
-(BOOL)isFlipped{return YES;}
@end
@interface TOTKCheatPanel:NSView
@property(strong) NSTextField *title,*hint,*note;
@property(strong) NSScrollView *scroll;
@property(strong) TOTKFlippedView *list;
@property(strong) NSMutableArray<NSButton *> *rows;
@property NSInteger selected;
@end
@implementation TOTKCheatPanel
-(instancetype)initWithFrame:(NSRect)frame{if((self=[super initWithFrame:frame])){
 self.wantsLayer=YES;self.layer.backgroundColor=[NSColor colorWithCalibratedWhite:0.05 alpha:0.92].CGColor;self.layer.cornerRadius=12;self.layer.borderWidth=1;self.layer.borderColor=[NSColor colorWithCalibratedWhite:1 alpha:0.2].CGColor;
 self.title=[NSTextField labelWithString:@(TOTK_UI("作弊码","Cheats"))];self.title.font=[NSFont systemFontOfSize:15 weight:NSFontWeightSemibold];self.title.textColor=NSColor.whiteColor;[self addSubview:self.title];
 self.hint=[NSTextField labelWithString:@(TOTK_UI("↑↓ / 方向键 选择 · 空格 / A 切换 · 5 / Esc / B / L3+R3 关闭 · 修改代码的作弊码重启游戏后生效","↑↓ or D-pad select · Space or A toggles · 5, Esc, B or L3+R3 closes · code cheats apply when the game restarts"))];
 self.hint.font=[NSFont systemFontOfSize:11];self.hint.textColor=[NSColor colorWithCalibratedWhite:0.7 alpha:1];self.hint.lineBreakMode=NSLineBreakByTruncatingTail;[self addSubview:self.hint];
 self.note=[NSTextField labelWithString:@""];self.note.font=[NSFont systemFontOfSize:12 weight:NSFontWeightMedium];self.note.textColor=[NSColor colorWithCalibratedRed:1 green:0.85 blue:0.3 alpha:1];self.note.hidden=YES;[self addSubview:self.note];
 self.list=[[TOTKFlippedView alloc] initWithFrame:NSZeroRect];
 self.scroll=[[NSScrollView alloc] initWithFrame:NSZeroRect];self.scroll.drawsBackground=NO;self.scroll.hasVerticalScroller=YES;self.scroll.autohidesScrollers=YES;self.scroll.documentView=self.list;[self addSubview:self.scroll];
 self.rows=[NSMutableArray new];}return self;}
-(void)reload{
 for(NSView *v in self.rows)[v removeFromSuperview];[self.rows removeAllObjects];
 int n=runtime_cheats_count();
 for(int i=0;i<n;i++){
  int st=runtime_cheats_state(i);
  NSButton *b=[NSButton checkboxWithTitle:@"" target:self action:@selector(clicked:)];
  b.attributedTitle=[[NSAttributedString alloc] initWithString:cheat_title(i) attributes:@{NSFontAttributeName:[NSFont systemFontOfSize:13],NSForegroundColorAttributeName:(st&CHEAT_REFUSED)?[NSColor colorWithCalibratedWhite:0.5 alpha:1]:NSColor.whiteColor}];
  b.tag=i;b.state=(st&CHEAT_ON)?NSControlStateValueOn:NSControlStateValueOff;b.enabled=!(st&CHEAT_REFUSED);b.refusesFirstResponder=YES;
  b.wantsLayer=YES;b.layer.cornerRadius=5;[self.list addSubview:b];[self.rows addObject:b];
 }
 if(n){   // the last row: turn every cheat off (code cheats then at the next launch)
  NSButton *off=[NSButton buttonWithTitle:@"" target:self action:@selector(allOff:)];off.bordered=NO;off.alignment=NSTextAlignmentLeft;off.tag=n;off.refusesFirstResponder=YES;
  off.attributedTitle=[[NSAttributedString alloc] initWithString:@(TOTK_UI("  关闭全部作弊码","  Turn off all cheats")) attributes:@{NSFontAttributeName:[NSFont systemFontOfSize:13 weight:NSFontWeightSemibold],NSForegroundColorAttributeName:cheats_any_on()?[NSColor colorWithCalibratedRed:1 green:0.55 blue:0.5 alpha:1]:[NSColor colorWithCalibratedWhite:0.5 alpha:1]}];
  off.wantsLayer=YES;off.layer.cornerRadius=5;[self.list addSubview:off];[self.rows addObject:off];
 }
 int k=cheats_pending();self.note.stringValue=k?cheats_pending_note(k):@"";self.note.hidden=!k;
 if(!n){NSButton *none=[NSButton buttonWithTitle:@"" target:nil action:nil];none.bordered=NO;none.enabled=NO;none.attributedTitle=[[NSAttributedString alloc] initWithString:@(TOTK_UI("此 App 未内置作弊码","No cheats are built into this app")) attributes:@{NSForegroundColorAttributeName:NSColor.whiteColor}];[self.list addSubview:none];[self.rows addObject:none];}
 if(self.selected>=(NSInteger)self.rows.count)self.selected=(NSInteger)self.rows.count-1;if(self.selected<0)self.selected=0;
 [self position];[self highlight];
}
-(void)highlight{for(NSUInteger i=0;i<self.rows.count;i++)self.rows[i].layer.backgroundColor=(NSInteger)i==self.selected?[NSColor colorWithCalibratedWhite:1 alpha:0.14].CGColor:NSColor.clearColor.CGColor;
 if(self.selected<(NSInteger)self.rows.count)[self.list scrollRectToVisible:self.rows[(NSUInteger)self.selected].frame];}
-(void)position{if(!self.superview)return;NSSize sv=self.superview.bounds.size;CGFloat noteH=self.note.hidden?0:20,rowH=26,w=MIN(600,MAX(300,sv.width-40)),listH=rowH*self.rows.count+8,h=MIN(listH+76+noteH,MAX(160,sv.height-60));
 NSRect f=NSMakeRect(floor((sv.width-w)/2),floor((sv.height-h)/2),w,h);if(!NSEqualRects(self.frame,f))self.frame=f;
 self.title.frame=NSMakeRect(16,h-34,w-32,20);self.hint.frame=NSMakeRect(16,10,w-32,16);self.note.frame=NSMakeRect(16,30,w-32,16);self.scroll.frame=NSMakeRect(8,34+noteH,w-16,h-74-noteH);
 CGFloat lw=self.scroll.contentSize.width;self.list.frame=NSMakeRect(0,0,lw,listH);
 for(NSUInteger i=0;i<self.rows.count;i++)self.rows[i].frame=NSMakeRect(6,4+rowH*i,lw-12,rowH-2);}
-(void)layout{[super layout];[self position];}
-(void)move:(int)d{NSInteger n=(NSInteger)self.rows.count;if(!n)return;self.selected=(self.selected+d+n)%n;[self highlight];}
-(void)set:(NSInteger)i on:(int)on{if(i<0||i>=runtime_cheats_count())return;runtime_cheats_set((int)i,on);[self reload];}
-(void)allOff:(NSButton *)b{(void)b;cheats_all_off();[self reload];}
-(void)toggleSelected{NSInteger i=self.selected;if(i==runtime_cheats_count()&&i>0){[self allOff:nil];return;}if(i<runtime_cheats_count()&&!(runtime_cheats_state((int)i)&CHEAT_REFUSED))[self set:i on:!(runtime_cheats_state((int)i)&CHEAT_ON)];}
-(void)clicked:(NSButton *)b{self.selected=b.tag;[self set:b.tag on:b.state==NSControlStateValueOn];}
@end
static void cheat_panel_toggle(void){
 TOTKCheatPanel *p=(TOTKCheatPanel *)cheatPanel;if(!p)return;
 if(p.hidden){[p reload];p.hidden=NO;runtime_input_clear();}   // keys held when it opens must not stay pressed in the game
 else p.hidden=YES;
}
/* Controllers: both sticks pressed together (L3+R3) open and close the panel; D-pad or left stick select, A toggles,
   B closes. While it is open the game gets no controller input (runtime/gamepad.m). */
enum { PAD_A = 1, PAD_B = 2, PAD_L3 = 16, PAD_R3 = 32, PAD_UP = 1 << 13, PAD_DOWN = 1 << 15, PAD_STICK_UP = 1 << 16, PAD_STICK_DOWN = 1 << 17 };
static int cheat_panel_pad(uint64_t held, uint64_t pressed){
 TOTKCheatPanel *p=(TOTKCheatPanel *)cheatPanel;if(!p||!runtime_cheats_count())return 0;   /* LM3: no panel without cheats */
 if((held&(PAD_L3|PAD_R3))==(PAD_L3|PAD_R3)&&(pressed&(PAD_L3|PAD_R3))){cheat_panel_toggle();return 1;}
 if(p.hidden)return 0;
 if(pressed&(PAD_UP|PAD_STICK_UP))[p move:-1];
 if(pressed&(PAD_DOWN|PAD_STICK_DOWN))[p move:1];
 if(pressed&PAD_A)[p toggleSelected];
 if(pressed&PAD_B)p.hidden=YES;   // returning 1 keeps this B from the game; it stays hidden until released
 return 1;
}
static int cheat_panel_key(NSEvent *e){
 if(e.keyCode==23&&runtime_cheats_count()){if(!e.isARepeat)cheat_panel_toggle();return 1;}   // key 5 (key 4 is the ultrawide view)
 TOTKCheatPanel *p=(TOTKCheatPanel *)cheatPanel;if(!p||p.hidden)return 0;
 switch(e.keyCode){
 case 126:[p move:-1];break;                    // up
 case 125:[p move:1];break;                     // down
 case 49:case 36:case 76:if(!e.isARepeat)[p toggleSelected];break;   // space, return, enter
 case 53:p.hidden=YES;break;                    // esc
 default:break;                                 // every other key stays away from the game while the panel is open
 }
 return 1;
}
/* Use the window-close path, not terminate:, so Command-Q also waits for active saves. */
static void front_install_menu(void){
 NSMenu *menu=[NSMenu new],*appMenu=[NSMenu new];
 NSMenuItem *appItem=[NSMenuItem new];appItem.submenu=appMenu;[menu addItem:appItem];
 NSMenuItem *quit=[[NSMenuItem alloc] initWithTitle:@(TOTK_UI("退出 奥日与黑暗森林","Quit Ori and the Blind Forest")) action:@selector(performClose:) keyEquivalent:@"q"];
 quit.keyEquivalentModifierMask=NSEventModifierFlagCommand;quit.target=window;
 displaySettings=[TOTKDisplaySettings new];
 NSMenuItem *settings=[[NSMenuItem alloc] initWithTitle:@(TOTK_UI("控制菜单…","Game Controls…")) action:@selector(showSettings:) keyEquivalent:@","];
 settings.keyEquivalentModifierMask=NSEventModifierFlagCommand;settings.target=displaySettings;
 [appMenu addItem:settings];[appMenu addItem:NSMenuItem.separatorItem];
 [appMenu addItem:quit];
 NSMenu *cheats=[[NSMenu alloc] initWithTitle:@(TOTK_UI("作弊","Cheats"))];cheatMenu=[TOTKCheatMenu new];cheats.delegate=cheatMenu;cheats.autoenablesItems=NO;
 NSMenuItem *cheatsItem=[NSMenuItem new];cheatsItem.submenu=cheats;if(runtime_cheats_count())[menu addItem:cheatsItem];   /* LM3: no menu without cheats */
 NSApp.mainMenu=menu;
}
static void *guest_main(void *callback){@autoreleasepool{((void(*)(void))callback)();}return NULL;}
void runtime_front_run(void(*callback)(void)){
 @autoreleasepool{
  bootStartNS=runtime_now_ns();
  [NSApplication sharedApplication];[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
  runtime_gamepad_start();if(!controlsLoaded)display_load();
  /* Default window: the docked output size, 1920x1080, scaled down (16:9 kept) when the screen's visible area is smaller. */
  /* TOTK_WINDOW_SIZE=<w>x<h> (test scripts only, e.g. the 2K output test; the desktop launcher never sets it): another starting size. */
  NSRect vis=NSScreen.mainScreen?NSScreen.mainScreen.visibleFrame:NSMakeRect(0,0,1920,1080+28);CGFloat ww=1920,wh=1080;
  {const char *e=getenv("TOTK_WINDOW_SIZE");int w=0,h=0;if(e&&sscanf(e,"%dx%d",&w,&h)==2&&w>=320&&h>=180&&w<=8192&&h<=8192){ww=w;wh=h;}}
  CGFloat k=fmin(1,fmin(vis.size.width/ww,(vis.size.height-28)/wh));ww=floor(ww*k);wh=floor(wh*k);
  window=[[TOTKGameWindow alloc] initWithContentRect:NSMakeRect(0,0,ww,wh) styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskMiniaturizable|NSWindowStyleMaskResizable backing:NSBackingStoreBuffered defer:NO];
  window.releasedWhenClosed=NO;   /* a close that waits for a save (windowWillClose) must not free the window the static pointer still holds */
  /* An agent's test game (TOTK_SLOT_OWNER other than user) says so in its title: it plays a save copy and its script closes it. */
  const char *owner=getenv("TOTK_SLOT_OWNER"),*purpose=getenv("TOTK_SLOT_PURPOSE");
  window.title=owner&&*owner&&strcmp(owner,"user")?[NSString stringWithFormat:@(TOTK_UI("测试局（%s）Ver. %s：%s — 存档副本，进度不会存进你的存档，脚本会自己关闭","Test session (%s) Ver. %s: %s — save copy, progress is not saved to your save, the script closes it")),owner,TOTK_GAME_VERSION,purpose&&*purpose?purpose:TOTK_UI("代理测试","agent test")]:[NSString stringWithFormat:@"%@  Mac — Ver. %s",@(GAME_NAME),TOTK_GAME_VERSION];
  windowDelegate=[TOTKWindowDelegate new];window.delegate=windowDelegate;NSApp.delegate=windowDelegate;
  front_install_menu();
  window.collectionBehavior|=NSWindowCollectionBehaviorFullScreenPrimary;
  window.contentView=[[TOTKInputView alloc] initWithFrame:NSMakeRect(0,0,ww,wh)];
  metalLayer=[CAMetalLayer layer];metalLayer.device=MTLCreateSystemDefaultDevice();metalLayer.pixelFormat=MTLPixelFormatBGRA8Unorm;metalLayer.drawableSize=CGSizeMake(ww,wh);
  metalLayer.framebufferOnly=NO;window.contentView.wantsLayer=YES;window.contentView.layer=[CALayer layer];window.contentView.layer.backgroundColor=NSColor.blackColor.CGColor;[window.contentView.layer addSublayer:metalLayer];
  bootOverlay=[[NSView alloc] initWithFrame:window.contentView.bounds];bootOverlay.autoresizingMask=NSViewWidthSizable|NSViewHeightSizable;
  bootOverlay.wantsLayer=YES;bootOverlay.layer.backgroundColor=[NSColor colorWithCalibratedRed:0.035 green:0.075 blue:0.09 alpha:1].CGColor;
  bootStage=[NSTextField labelWithString:@(TOTK_UI("正在启动","Starting"))];
  bootStage.font=[NSFont systemFontOfSize:25 weight:NSFontWeightSemibold];bootStage.textColor=NSColor.whiteColor;
  bootStage.alignment=NSTextAlignmentCenter;bootStage.frame=NSMakeRect(0,wh/2+20,ww,40);bootStage.autoresizingMask=NSViewWidthSizable|NSViewMinYMargin|NSViewMaxYMargin;
  bootDetail=[NSTextField labelWithString:@(TOTK_UI("正在准备游戏环境","Preparing the game environment"))];
  bootDetail.font=[NSFont systemFontOfSize:14];bootDetail.textColor=[NSColor colorWithCalibratedWhite:0.72 alpha:1];
  bootDetail.alignment=NSTextAlignmentCenter;bootDetail.frame=NSMakeRect(0,wh/2-20,ww,28);bootDetail.autoresizingMask=NSViewWidthSizable|NSViewMinYMargin|NSViewMaxYMargin;
  bootProgress=[[NSProgressIndicator alloc] initWithFrame:NSMakeRect(ww/2-140,wh/2-55,280,10)];
  bootProgress.minValue=0;bootProgress.maxValue=100;bootProgress.indeterminate=YES;bootProgress.autoresizingMask=NSViewMinXMargin|NSViewMaxXMargin|NSViewMinYMargin|NSViewMaxYMargin;
  [bootOverlay addSubview:bootStage];[bootOverlay addSubview:bootDetail];[bootOverlay addSubview:bootProgress];
  [window.contentView addSubview:bootOverlay];[bootProgress startAnimation:nil];
  if(hud_compact()){shortcutHelp=[[TOTKShortcutHelp alloc] initWithFrame:NSZeroRect];shortcutHelp.autoresizingMask=hud_compact()?NSViewMinXMargin|NSViewMinYMargin:NSViewMinXMargin|NSViewMaxXMargin|NSViewMinYMargin|NSViewMaxYMargin;
  /* Keep discovery/warm-up visible; the help appears after the opaque boot
     overlay is hidden. Key 1 still controls its requested visibility. */
  [window.contentView addSubview:shortcutHelp positioned:NSWindowBelow relativeTo:bootOverlay];
  [(TOTKShortcutHelp *)shortcutHelp position];}
  cheatPanel=[[TOTKCheatPanel alloc] initWithFrame:NSZeroRect];cheatPanel.hidden=YES;cheatPanel.autoresizingMask=NSViewMinXMargin|NSViewMaxXMargin|NSViewMinYMargin|NSViewMaxYMargin;
  [window.contentView addSubview:cheatPanel];runtime_gamepad_set_overlay(cheat_panel_pad);
  /* The FPS, shader and load lines: in the corner panel above (compact HUD), or with TOTK_HUD=corners drawn into the game picture's corners by the present encoder (gfx/nvn/fps_overlay.c). */
  NSButton *controlButton=[NSButton buttonWithTitle:@(TOTK_UI("控制菜单","Game Controls")) target:displaySettings action:@selector(showSettings:)];
  controlButton.frame=NSMakeRect(14,wh-46,120,30);controlButton.autoresizingMask=NSViewMaxXMargin|NSViewMinYMargin;
  [window.contentView addSubview:controlButton positioned:NSWindowAbove relativeTo:bootOverlay];
  [window makeFirstResponder:window.contentView];[window center];front_sync_drawable();
  if(assume_focus()){[window orderFrontRegardless];}else{[window makeKeyAndOrderFront:nil];[NSApp activateIgnoringOtherApps:YES];}   /* in front of other apps' windows, keyboard left alone */
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW,3*NSEC_PER_SEC),dispatch_get_main_queue(),^{note_occlusion(" 3 s after opening");});
  atomic_store(&focused,1);
  displayWindowFrame=window.frame;display_apply_mode();
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
  bar.done=^(NSString *text){result=[text copy];[weakBar removeFromSuperview];[window makeFirstResponder:window.contentView];dispatch_semaphore_signal(sem);};
  [bar addSubview:label];[bar addSubview:hint];[bar addSubview:field];[cv addSubview:bar];openBar=bar;
  runtime_input_clear();
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
void *runtime_front_override(const char *name){
 static const struct{const char *name;void *fn;} entries[]={
 {"_ZN2nn2vi10InitializeEv",vi_initialize},{"_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE",vi_open},
 {"_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE",vi_layer},{"_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE",vi_window},
 {"_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE",vi_vsync}};
 for(unsigned i=0;i<sizeof(entries)/sizeof(*entries);i++)if(!strcmp(name,entries[i].name))return entries[i].fn;return NULL;
}
