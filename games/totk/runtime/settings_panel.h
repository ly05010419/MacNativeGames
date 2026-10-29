/* The settings panel (Mac; included by frontend.m): key 1 opens and closes it (runtime/input.c); Esc, B or L3 + R3
   close it too. User 2026-10-25: it replaces the shortcut keys 2-7 and their reminder box. User 2026-10-29: rebuilt to the
   glass design pack (设置界面设计包): a blurred glass panel over the game, 760 x 580 for every page (640 x 560 when the
   window has no room), three pages chosen by the capsule tabs at the top (the white block slides) or by L / R:
   Display, Input and Keys. One focus drives the panel, not the mouse's hover: the d-pad or left stick (or the arrows)
   moves it, A (Return, Space) acts, B (Esc) goes back; a click is just another way to move the focus. A switch flips with
   A; a choice, a slider and the bars change with left / right. Each setting applies at once and is read again twice a
   second, so the panel always shows what is in force:
   Display
     resolution           720p / 1080p / + MetalFX / 2K /    runtime_output_mode_set (game_patches*.c); the window takes
                          2K + MetalFX (a 4K screen)         that size at once (settings_window_for)
     aspect ratio         16:9 / 16:10 / 21:9 / 32:9         runtime_aspect_set_ratio (aspect_patch.h, game_patches_botw.c);
                                                             TOTK_ASPECT's ratio is offered too
     frame cap            off / 30 / 40 / 60 / 72 / 90 / 120  nvn_fps_lock_set (gfx/nvn/hle_nvn.c); it and frame
     frame interpolation  off / 30 -> 60 / 40 -> 80 /         interpolation exclude each other (picking one turns the
                          50 -> 100 / unlimited               other off); runtime flag mfx_interp (gfx/nvn/totk_mfx.m)
     edge smoothing       switch                             taa_set (gfx/nvn/totk_taa.m)
     performance info     all / FPS + playtime / off         fps_overlay_set_hud_mode (gfx/nvn/fps_overlay.c)
     frame graph          switch                             fps-graph.txt
   Input
     input method         controller / keyboard + mouse      settings-keys-device.txt; decides which Keys page and which
                                                             hints (A / B or the key caps) are shown, and greys the mouse
                                                             sensitivity out with the controller
     controller type      auto / Xbox / PlayStation / Switch runtime_gamepad_type_set (gamepad.m), saved
     rumble               bars: off, 0.5x, 1x, 2x, 3x, 4x    runtime_feedback_choose (hid_feedback.c) and
                                                             runtime_rumble_gain_set (gamepad_feedback.m); with
                                                             TOTK_RUMBLE / TOTK_RUMBLE_GAIN or an agent's test session
                                                             the switch and the strength as two greyed rows
     gyro aiming          switch                             runtime_feedback_choose, saved in gyro.txt
     DualSense bow trigger switch                            runtime_triggers_choose (gamepad_feedback.m), dualsense-
                                                             triggers.txt; Breath of the Wild 1.9.0 and Tears of the
                                                             Kingdom 1.4.3 only; TOTK_DUALSENSE_TRIGGERS decides first
     mouse looks          switch                             runtime_mouse_set (gamepad.m), saved
     mouse sensitivity    slider 1-100 (50)                  runtime_mouse_sensitivity_set (gamepad.m), mouse-
                                                             sensitivity.txt; TOTK_MOUSE_SENSITIVITY decides first
   Keys: with the controller as the input method, a photo of the controller with a leader line and a label (glyph and what
   it does) per button; else cards, two columns, the first key or mouse button of each action (keyboard_map.h,
   runtime_keymap_set: in force at once, keyboard.txt). Pick a label or card and press A (or click): it names the action it
   waits for and takes the next button, key or mouse button (B / Esc / key 1 cancel; Command keys stay the system's; 10
   seconds without input end the wait). What another action held swaps places with it. A card is cleared by right-click; Y
   (Delete) restores the default layout of the device shown (the file removed). The controller's layout is
   controller_map.h (runtime_controller_map_set: controls.txt); the button that opens the panel is part of it
   (runtime_gamepad_settings_button, controller-settings.txt).
   A setting this game or start cannot change shows its one value, greyed out. While the panel is open the game gets no
   keyboard, mouse or controller input (TOTKInputView, front_mouse_update and gamepad capture).
   Gone with the redesign (the design pack has no place for them): the version line, the contribute-shaders and the star
   buttons, the close button and the reset buttons. */
#pragma once
#import <CoreImage/CoreImage.h>
#import <QuartzCore/QuartzCore.h>   /* the glass blur (the class looked up by name: Quartz Core brings it) */
#include "totk_fps_limit.h"   /* totk_fps_limits: the frame lock choices */
#include "../third_party/d2r_gfx/gfx/nvn/totk_shader_contrib.h"   /* the contribution file, TOTK_REPO_URL */
#include "contrib_submit.h"   /* gh_path, contrib_submit (shared with app/launcher.m) */
#include "aspect_math.h"     /* aspect_near, ASPECT_GAME */
#include "keyboard_map.h"    /* the keyboard and mouse layout */
#include "controller_map.h"  /* the controller layout */

void dm_gfx_set_flag(const char *name, int value);
void nvn_fps_lock_set(int fps);   /* gfx/nvn/hle_nvn.c: the shared totk_fps_limits choices */
int nvn_fps_lock(void);

/* Picking a resolution sizes the window to it at once (user 2026-10-25): 720p 1280x720, 1080p 1920x1080, MetalFX 2560x1440, native its own size (2K), 2K + MetalFX 3840x2160, in the window's points as the first window (frontend.m runtime_front_run), the picture's shape kept
   (the aspect ratio picked) and shrunk to fit the screen's visible area. Full screen stays as it is. */
static void settings_window_for(int mode) {
 if(!window||(window.styleMask&NSWindowStyleMaskFullScreen))return;
 unsigned w=1920,h=1080;
 if(mode==OUTPUT_MODE_METALFX){w=2560;h=1440;}
 else if(mode==OUTPUT_MODE_NATIVE_METALFX){w=3840;h=2160;}
 else if(mode==OUTPUT_MODE_720P){w=1280;h=720;}
 else if(mode==OUTPUT_MODE_NATIVE&&!runtime_output_native(mode,&w,&h)){w=1920;h=1080;}
 double ratio=runtime_aspect_ratio()>0?runtime_aspect_ratio():(double)w/h;
 NSScreen *screen=window.screen?:NSScreen.mainScreen;if(!screen||!(ratio>0))return;
 NSRect visible=screen.visibleFrame,content=[window contentRectForFrameRect:visible];
 CGFloat ch=round(fmin(h,fmin(content.size.height,content.size.width/ratio))),cw=round(ch*ratio);
 NSRect old=window.frame,f=[window frameRectForContentRect:NSMakeRect(0,0,cw,ch)];
 f.origin=NSMakePoint(NSMidX(old)-f.size.width/2,NSMidY(old)-f.size.height/2);
 f.origin.x=fmax(NSMinX(visible),fmin(f.origin.x,NSMaxX(visible)-f.size.width));
 f.origin.y=fmax(NSMinY(visible),fmin(f.origin.y,NSMaxY(visible)-f.size.height));
 [window setFrame:f display:YES];   /* windowDidResize follows the drawable (front_sync_drawable) */
 fprintf(stderr,"[totk] settings: window %.0fx%.0f for the resolution\n",cw,ch);
}

/* Settings live alongside controls.txt. Restore the graphics switches before the guest starts; output and aspect
   patches become available later, so restore those on the main thread when the guest has installed them. */
static int settingsPendingOutput = -1;
static float settingsPendingAspect;
static NSTimer *settingsRestoreTimer;
static void settings_save_int(const char *name, int value, const char *what) {
 char word[32];snprintf(word,sizeof word,"%d",value);runtime_setting_save(name,word,what);
}
static int settings_read_int(const char *name, int *value) {
 char word[32],*end;if(!runtime_setting_word(name,word,sizeof word))return 0;
 long n=strtol(word,&end,10);while(*end==' '||*end=='\n'||*end=='\r'||*end=='\t')end++;
 if(end==word||*end||n<0||n>1000)return 0;*value=(int)n;return 1;
}
static void settings_save_frame(void) {
 char word[32];int fps=nvn_fps_lock(),mfx=mfx_requested();
 if(fps)snprintf(word,sizeof word,"lock-%d",fps);
 else if(mfx)snprintf(word,sizeof word,"interp-%d",mfx);
 else snprintf(word,sizeof word,"off");
 runtime_setting_save("graphics-frame.txt",word,"frame mode");
}
static void settings_restore_begin(void) {
 int v;char word[32];
 if(settings_read_int("graphics-overlay.txt",&v)&&v<=HUD_MODE_MINIMAL)fps_overlay_set_hud_mode(v);
 if(!getenv("TOTK_MFX")&&runtime_setting_word("graphics-frame.txt",word,sizeof word)){
  int n=0;char tail=0;
  if(sscanf(word,"lock-%d %c",&n,&tail)==1&&(n>0&&totk_fps_limit_valid(n))){dm_gfx_set_flag("mfx_interp",MFX_OFF);nvn_fps_lock_set(n);}
  else if(sscanf(word,"interp-%d %c",&n,&tail)==1&&n>=MFX_30&&n<=MFX_MAX){nvn_fps_lock_set(0);dm_gfx_set_flag("mfx_interp",n);}
  else if(!strcmp(word,"off")||!strcmp(word,"off\n")){nvn_fps_lock_set(0);dm_gfx_set_flag("mfx_interp",MFX_OFF);}
 }
 if(!getenv("TOTK_TAA")&&settings_read_int("graphics-taa.txt",&v)&&(v==0||v==1))taa_set(v);
 const char *output=getenv("TOTK_OUTPUT_RES");
 if((!output||!*output||!strcmp(output,"auto"))&&!getenv("TOTK_MFX_OUTPUT")&&!getenv("TOTK_MFX_UPSCALE")&&
    settings_read_int("graphics-output.txt",&v)&&v<=OUTPUT_MODE_720P)settingsPendingOutput=v;
 if(!getenv("TOTK_ASPECT")&&runtime_setting_word("graphics-aspect.txt",word,sizeof word)){
  char *end;float ratio=strtof(word,&end);while(*end==' '||*end=='\n'||*end=='\r'||*end=='\t')end++;
  if(end!=word&&!*end&&isfinite(ratio)&&ratio>=0.5f&&ratio<=8)settingsPendingAspect=ratio;
 }
}
static void settings_restore_ready(void) {
 if(settingsPendingAspect>0&&runtime_aspect_ratio()>0){
  float r=settingsPendingAspect;settingsPendingAspect=0;
  if(runtime_aspect_set_ratio(r))runtime_front_resize_aspect(r);
 }
 int outputModes=runtime_output_modes();
#ifdef GAME_BOTW
 /* BotW advertises 1080p / MetalFX before its patches are installed. Wait for those patches before deciding whether
    a saved mode is supported; otherwise 1.6.0's native resolution is discarded before its canvas becomes available. */
 if(!(runtime_aspect_ratio()>0))outputModes=0;
#endif
 if(settingsPendingOutput>=0&&outputModes){
  int mode=settingsPendingOutput;settingsPendingOutput=-1;
  if((outputModes&(1<<mode))&&runtime_output_mode_set(mode))settings_window_for(mode);
 }
 if(settingsPendingOutput<0&&settingsPendingAspect<=0){[settingsRestoreTimer invalidate];settingsRestoreTimer=nil;}
}
static void settings_restore_start(void) {
 settings_restore_begin();
 if(settingsPendingOutput<0&&settingsPendingAspect<=0)return;
 settingsRestoreTimer=[NSTimer timerWithTimeInterval:0.5 repeats:YES block:^(NSTimer *t){(void)t;settings_restore_ready();}];
 [NSRunLoop.mainRunLoop addTimer:settingsRestoreTimer forMode:NSRunLoopCommonModes];
}

enum { SETTINGS_PAGE_DISPLAY, SETTINGS_PAGE_INPUT, SETTINGS_PAGE_BINDINGS, SETTINGS_PAGES };
static void settings_refresh_all(void);
@class TOTKValueControl;
@interface TOTKSettingRow:NSObject
@property(copy) NSString *title;
@property(copy) NSArray<NSString *> *choices;   /* nil: an on / off switch, or a slider */
@property(copy) NSArray<NSNumber *> *values;    /* what each choice sets */
@property(copy) int (^get)(void);
@property(copy) void (^set)(int value);
@property(copy) NSString *(^firstTitle)(void);   /* a first choice whose text changes (auto: the controller it sees) */
@property int sliderMin, sliderMax;              /* a slider when sliderMax > sliderMin */
@property NSInteger page;                        /* SETTINGS_PAGE_DISPLAY or SETTINGS_PAGE_INPUT */
@property BOOL divider;                          /* a rule above this row: a new group */
@property BOOL bars;                             /* the choices as bars, the first (off) none lit (rumble) */
@property(copy) BOOL (^enabledIf)(void);         /* also needed to change it (sensitivity: the mouse on) */
@property(strong) TOTKValueControl *control;
-(BOOL)slider;
-(BOOL)enabled;
-(int)value;
-(NSInteger)index;
-(NSString *)text;
-(void)apply:(int)v;
-(void)stepBy:(int)dir;
-(void)activate;
-(void)refresh;
@end

/* The panel's colours (design pack 2026-10-29): white on glass, and #111820 on the white block that marks the picked item. */
static NSColor *settings_ink(BOOL pill,CGFloat alpha){return pill?[NSColor colorWithCalibratedRed:0.067 green:0.094 blue:0.125 alpha:alpha]:[NSColor colorWithCalibratedWhite:1 alpha:alpha];}
static NSFont *settings_font(CGFloat size,BOOL bold){return [NSFont systemFontOfSize:size weight:bold?NSFontWeightSemibold:NSFontWeightRegular];}

/* What can be picked in the panel (a row, a key card, a controller button's label): the white block when picked. A
   controller or the keyboard moves the focus; a click moves it too. */
@interface TOTKFocusView:NSView
@property(nonatomic) BOOL focus;
@property(copy) void (^onPick)(void);   /* a click: the focus moves here (the design notes: the focus is not the pointer's) */
-(void)restyle;
@end
@implementation TOTKFocusView
-(BOOL)acceptsFirstMouse:(NSEvent *)e{(void)e;return YES;}
-(void)setFocus:(BOOL)focus{if(_focus==focus)return;_focus=focus;[self restyle];}
-(void)restyle{}
@end

/* What a row shows at its right: the value (with ‹ › when picked), a switch, a slider with its number, or bars. */
@interface TOTKValueControl:NSControl
@property(weak) TOTKSettingRow *row;
@property BOOL pill;      /* drawn on the white block of the picked row */
@end
@implementation TOTKValueControl
-(BOOL)acceptsFirstMouse:(NSEvent *)e{(void)e;return YES;}
-(NSRect)sliderTrack{return NSMakeRect(self.bounds.size.width-22-12-160,floor(self.bounds.size.height/2)-2,160,4);}
/* The value's place at the right; picked, with the arrows 12 pt either side. */
-(NSDictionary *)textAttributes{return @{NSFontAttributeName:settings_font(16,self.pill),NSForegroundColorAttributeName:self.pill?settings_ink(YES,1):settings_ink(NO,self.row.enabled?0.6:0.38)};}
-(NSRect)textRect{NSSize t=[self.row.text sizeWithAttributes:[self textAttributes]];CGFloat right=self.bounds.size.width-(self.pill&&self.row.choices.count>1?22:0);
 return NSMakeRect(right-t.width,floor((self.bounds.size.height-t.height)/2),t.width,t.height);}
-(void)drawRect:(NSRect)dirty{
 (void)dirty;TOTKSettingRow *r=self.row;if(!r)return;
 CGFloat on=r.enabled?1:0.38;NSRect b=self.bounds;BOOL pill=self.pill;
 if(r.slider){
  NSRect tr=[self sliderTrack];double span=r.sliderMax-r.sliderMin,f=span>0?(r.value-r.sliderMin)/span:0;f=fmax(0,fmin(1,f));
  [settings_ink(pill,(pill?0.15:0.12)) setFill];[[NSBezierPath bezierPathWithRoundedRect:tr xRadius:2 yRadius:2] fill];
  NSRect done=tr;done.size.width=floor(tr.size.width*f);[settings_ink(pill,(r.enabled?0.9:0.3)) setFill];[[NSBezierPath bezierPathWithRoundedRect:done xRadius:2 yRadius:2] fill];
  [settings_ink(pill,r.enabled?1:0.4) setFill];[[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(NSMaxX(done)-7,NSMidY(tr)-7,14,14)] fill];
  NSMutableParagraphStyle *right=[NSMutableParagraphStyle new];right.alignment=NSTextAlignmentRight;
  NSDictionary *at=@{NSFontAttributeName:[NSFont monospacedDigitSystemFontOfSize:16 weight:pill?NSFontWeightSemibold:NSFontWeightRegular],
   NSForegroundColorAttributeName:pill?settings_ink(YES,1):settings_ink(NO,0.6*on),NSParagraphStyleAttributeName:right};
  NSString *v=[NSString stringWithFormat:@"%d",r.value];NSSize ts=[v sizeWithAttributes:at];
  [v drawInRect:NSMakeRect(b.size.width-40,floor((b.size.height-ts.height)/2),40,ts.height) withAttributes:at];
 }else if(!r.choices){
  NSRect sw=NSMakeRect(b.size.width-42,floor((b.size.height-24)/2),42,24);BOOL lit=r.value!=0;
  NSColor *track=lit?(pill?settings_ink(YES,on):[NSColor colorWithCalibratedWhite:1 alpha:0.9*on]):(pill?settings_ink(YES,0.18):[NSColor colorWithCalibratedWhite:1 alpha:0.18*on]);
  [track setFill];[[NSBezierPath bezierPathWithRoundedRect:sw xRadius:12 yRadius:12] fill];
  NSColor *knob=lit&&!pill?[NSColor colorWithCalibratedRed:0.102 green:0.133 blue:0.188 alpha:on]:[NSColor colorWithCalibratedWhite:1 alpha:on];
  [knob setFill];[[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(sw.origin.x+(lit?21:3),sw.origin.y+3,18,18)] fill];
 }else if(r.bars){
  NSInteger n=(NSInteger)r.choices.count-1,lit=r.index;CGFloat w=n*30+(n-1)*4,x=b.size.width-w,y=floor((b.size.height-6)/2);
  for(NSInteger i=0;i<n;i++){[(i<lit?settings_ink(pill,0.9*on):settings_ink(pill,0.18*on)) setFill];
   [[NSBezierPath bezierPathWithRoundedRect:NSMakeRect(x+i*34,y,30,6) xRadius:3 yRadius:3] fill];}
 }else{
  NSRect t=[self textRect];[r.text drawAtPoint:t.origin withAttributes:[self textAttributes]];
  if(pill&&r.enabled&&r.choices.count>1){
   NSDictionary *aa=@{NSFontAttributeName:settings_font(16,NO),NSForegroundColorAttributeName:settings_ink(YES,0.4)};NSSize as=[@"‹" sizeWithAttributes:aa];
   [@"‹" drawAtPoint:NSMakePoint(t.origin.x-12-as.width,t.origin.y) withAttributes:aa];[@"›" drawAtPoint:NSMakePoint(NSMaxX(t)+12,t.origin.y) withAttributes:aa];}
 }
}
-(void)setSliderFrom:(NSEvent *)e{
 TOTKSettingRow *r=self.row;NSRect tr=[self sliderTrack];CGFloat x=[self convertPoint:e.locationInWindow fromView:nil].x;
 double f=fmax(0,fmin(1,(x-tr.origin.x)/tr.size.width));[r apply:(int)lround(r.sliderMin+f*(r.sliderMax-r.sliderMin))];
}
-(void)mouseDown:(NSEvent *)e{
 TOTKSettingRow *r=self.row;if(!r.enabled)return;
 TOTKFocusView *band=(TOTKFocusView *)self.superview;if([band isKindOfClass:TOTKFocusView.class]&&band.onPick)band.onPick();
 if(r.slider){[self setSliderFrom:e];return;}
 if(r.choices&&!r.bars&&self.pill&&[self convertPoint:e.locationInWindow fromView:nil].x<NSMidX([self textRect])){[r stepBy:-1];return;}
 [r activate];
}
-(void)mouseDragged:(NSEvent *)e{if(self.row.slider&&self.row.enabled)[self setSliderFrom:e];}
@end

@implementation TOTKSettingRow
-(BOOL)slider{return self.sliderMax>self.sliderMin;}
-(BOOL)enabled{return self.set!=nil&&(!self.enabledIf||self.enabledIf());}
-(int)value{return self.get?self.get():0;}
-(NSInteger)index{NSUInteger i=[self.values indexOfObject:@(self.value)];return i==NSNotFound?-1:(NSInteger)i;}
-(NSString *)text{
 NSInteger i=self.index;if(i<0)return @"—";
 if(i==0&&self.firstTitle)return self.firstTitle();
 return self.choices[(NSUInteger)i];
}
-(void)apply:(int)v{
 if(self.set)self.set(v);
 [self.control.window makeFirstResponder:self.control.window.contentView];   /* key 1 and Esc keep reaching the window */
 settings_refresh_all();   /* a pick may change another row (frame cap / interpolation, mouse / sensitivity) */
}
-(void)stepBy:(int)dir{
 if(!self.enabled)return;
 if(self.slider)[self apply:MAX(self.sliderMin,MIN(self.sliderMax,self.value+dir*5))];
 else if(self.choices){NSInteger n=(NSInteger)self.choices.count;if(n<2)return;NSInteger i=self.index<0?0:self.index;
  if(self.bars){i=MAX(0,MIN(n-1,i+dir));[self apply:self.values[(NSUInteger)i].intValue];return;}   /* bars stop at the ends */
  [self apply:self.values[(NSUInteger)((i+dir+n)%n)].intValue];}
 else [self apply:dir>0];
}
-(void)activate{
 if(!self.enabled||self.slider)return;
 if(self.choices){if(self.bars){NSInteger n=(NSInteger)self.choices.count,i=self.index<0?0:self.index;[self apply:self.values[(NSUInteger)((i+1)%n)].intValue];}else [self stepBy:1];}
 else [self apply:!self.value];
}
-(void)refresh{[self.control setNeedsDisplay:YES];}
@end

/* One row of the display or input page: 50 pt, rounded 14; picked, the white block with the shadow. Clicking it steps the
   value like the control at its right. */
@interface TOTKRowBand:TOTKFocusView
@property(weak) TOTKSettingRow *row;
@property(strong) NSTextField *label;
@end
@implementation TOTKRowBand
-(void)restyle{
 BOOL pill=self.focus;self.wantsLayer=YES;self.layer.cornerRadius=14;
 self.layer.backgroundColor=(pill?[NSColor colorWithCalibratedWhite:1 alpha:0.92]:NSColor.clearColor).CGColor;
 self.layer.shadowColor=NSColor.blackColor.CGColor;self.layer.shadowOpacity=pill?0.18:0;self.layer.shadowRadius=10;self.layer.shadowOffset=NSMakeSize(0,-6);
 self.label.textColor=pill?settings_ink(YES,1):settings_ink(NO,self.row.enabled?0.9:0.38);self.label.font=settings_font(16,pill);
 self.row.control.pill=pill;[self.row.control setNeedsDisplay:YES];
}
-(void)refresh{self.label.textColor=self.focus?settings_ink(YES,1):settings_ink(NO,self.row.enabled?0.9:0.38);[self.row.control setNeedsDisplay:YES];}
-(void)mouseDown:(NSEvent *)e{(void)e;if(!self.row.enabled)return;if(self.onPick)self.onPick();[self.row activate];}
@end

/* The tabs at the top: a dark capsule, the page shown a white block that slides to the tab picked (design pack: L | 画面 操作 按键 | R). */
@interface TOTKTabs:NSControl
@property(copy) NSArray<NSString *> *titles;
@property(nonatomic) NSInteger selectedSegment;
-(NSSize)idealSize;
@end
@implementation TOTKTabs{CALayer *track;CALayer *pill;NSMutableArray<CATextLayer *> *texts;BOOL placed;}
-(BOOL)acceptsFirstMouse:(NSEvent *)e{(void)e;return YES;}
-(NSView *)hitTest:(NSPoint)p{return NSPointInRect([self convertPoint:p fromView:self.superview],self.bounds)?self:nil;}
-(CGFloat)widthOf:(NSUInteger)i{return ceil([self.titles[i] sizeWithAttributes:@{NSFontAttributeName:settings_font(15,YES)}].width)+48;}
-(NSSize)idealSize{CGFloat w=8+2*(self.titles.count-1);for(NSUInteger i=0;i<self.titles.count;i++)w+=[self widthOf:i];return NSMakeSize(w,40);}
-(void)setSelectedSegment:(NSInteger)i{if(_selectedSegment==i&&placed)return;_selectedSegment=i;[self place:placed];placed=YES;}
-(void)setFrameSize:(NSSize)size{[super setFrameSize:size];if(track)[self place:NO];}
-(void)place:(BOOL)animated{
 if(!self.titles.count)return;
 if(!track){
  self.wantsLayer=YES;CGFloat scale=NSScreen.mainScreen.backingScaleFactor?:2;
  track=[CALayer layer];track.backgroundColor=[NSColor colorWithCalibratedWhite:0 alpha:0.25].CGColor;[self.layer addSublayer:track];
  pill=[CALayer layer];pill.backgroundColor=NSColor.whiteColor.CGColor;[self.layer addSublayer:pill];
  texts=[NSMutableArray array];
  for(NSString *t in self.titles){CATextLayer *l=[CATextLayer layer];l.string=t;l.fontSize=15;l.alignmentMode=kCAAlignmentCenter;l.contentsScale=scale;[self.layer addSublayer:l];[texts addObject:l];}
 }
 [CATransaction begin];
 if(animated){[CATransaction setAnimationDuration:0.32];[CATransaction setAnimationTimingFunction:[CAMediaTimingFunction functionWithControlPoints:0.25:0.9:0.3:1]];}
 else [CATransaction setDisableActions:YES];
 NSRect b=self.bounds;track.frame=b;track.cornerRadius=b.size.height/2;
 CGFloat x=4;
 for(NSUInteger i=0;i<self.titles.count;i++){
  CGFloat w=[self widthOf:i];BOOL on=(NSInteger)i==self.selectedSegment;NSRect r=NSMakeRect(x,4,w,b.size.height-8);
  if(on){pill.frame=r;pill.cornerRadius=r.size.height/2;}
  CATextLayer *l=texts[i];NSFont *font=settings_font(15,on);l.font=(__bridge CFTypeRef)font;
  l.frame=NSMakeRect(x,floor(NSMidY(r)-9),w,20);l.foregroundColor=settings_ink(on,on?1:0.7).CGColor;x+=w+2;
 }
 [CATransaction commit];
}
-(void)mouseDown:(NSEvent *)e{
 CGFloat px=[self convertPoint:e.locationInWindow fromView:nil].x,x=4;
 for(NSUInteger i=0;i<self.titles.count;i++){CGFloat w=[self widthOf:i];if(px>=x&&px<x+w+2){self.selectedSegment=(NSInteger)i;[self sendAction:self.action to:self.target];return;}x+=w+2;}
}
@end

static NSString *settings_text(const char *zh, const char *en) { return @(TOTK_UI(zh, en)); }
static TOTKSettingRow *settings_row(NSString *title, NSArray<NSString *> *choices, NSArray<NSNumber *> *values,
                                    int (^get)(void), void (^set)(int)) {
 TOTKSettingRow *r=[TOTKSettingRow new];r.title=title;r.choices=choices;r.values=values;r.get=get;r.set=set;return r;
}
/* A setting this game or start cannot change: its one value, greyed out. */
static TOTKSettingRow *settings_fixed(NSString *title, NSString *value) {
 return settings_row(title,@[value],@[@0],^int{return 0;},nil);
}
static NSString *settings_on_off(int on) { return on?settings_text("开","On"):settings_text("关","Off"); }

static NSString *settings_ratio_text(float r) {
 if(fabsf(r*9-16)<0.02f)return @"16:9";
 if(fabsf(r*9-21)<0.05f)return @"21:9";
 if(fabsf(r*9-32)<0.05f)return @"32:9";
 if(fabsf(r*10-16)<0.02f)return @"16:10";
 return [NSString stringWithFormat:@"%.2f:1",r];
}
/* Rumble (gyro 0) or gyro aiming (gyro 1): a switch, or its one value greyed out when the environment or an agent's test
   session decides; a device the game did not get at the start comes at the next launch, which the title says. */
static TOTKSettingRow *settings_feedback_row(int gyro) {
 NSString *t=gyro?settings_text("体感瞄准","Gyro aiming"):settings_text("震动","Rumble");
 if(runtime_feedback_locked(gyro))return settings_fixed(t,settings_on_off(runtime_feedback_present(gyro)));
 if(!runtime_feedback_present(gyro))t=[t stringByAppendingString:settings_text("（下次启动生效）"," (from the next launch)")];
 return settings_row(t,nil,nil,^int{return runtime_feedback_choice(gyro);},^(int v){runtime_feedback_choose(gyro,v);});
}
/* Rumble strength in thousandths: 0.5x to 4x, and the strength in force when rumble-gain.txt holds another one. */
static int settings_gain_now(void) { return (int)fmax(1, lroundf(runtime_rumble_gain()*1000)); }
static TOTKSettingRow *settings_gain_row(void) {
 NSString *t=settings_text("震动强度","Rumble strength");
 int now=settings_gain_now();
 NSString *(^text)(int)=^NSString *(int v){NSString *x=[NSString stringWithFormat:@"%g×",v/1000.0];
  return v==1000?[x stringByAppendingString:settings_text("（默认）"," (default)")]:x;};
 if(runtime_rumble_gain_locked())return settings_fixed(t,text(now));
 NSMutableArray<NSNumber *> *values=[@[@500,@1000,@2000,@3000,@4000] mutableCopy];
 if(![values containsObject:@(now)]){[values addObject:@(now)];[values sortUsingSelector:@selector(compare:)];}
 NSMutableArray<NSString *> *choices=[NSMutableArray array];for(NSNumber *v in values)[choices addObject:text(v.intValue)];
 return settings_row(t,choices,values,^int{return settings_gain_now();},^(int v){runtime_rumble_gain_set(v/1000.0f);});
}
/* The design pack's one rumble row: bars, none lit = off, then 0.5x 1x 2x 3x 4x. When TOTK_RUMBLE / TOTK_RUMBLE_GAIN or an
   agent's test session decide, the switch and the strength rows as before. */
static const int settings_gains[]={500,1000,2000,3000,4000};
static void settings_rumble_rows(NSMutableArray<TOTKSettingRow *> *rows) {
 if(runtime_feedback_locked(0)||runtime_rumble_gain_locked()){[rows addObject:settings_feedback_row(0)];[rows addObject:settings_gain_row()];return;}
 NSString *t=settings_text("震动","Rumble");
 if(!runtime_feedback_present(0))t=[t stringByAppendingString:settings_text("（下次启动生效）"," (from the next launch)")];
 TOTKSettingRow *r=settings_row(t,@[settings_on_off(0),@"0.5×",@"1×",@"2×",@"3×",@"4×"],@[@0,@1,@2,@3,@4,@5],^int{
   if(!runtime_feedback_choice(0))return 0;int now=settings_gain_now(),best=0;
   for(int i=1;i<5;i++)if(abs(settings_gains[i]-now)<abs(settings_gains[best]-now))best=i;return best+1;},
  ^(int v){runtime_feedback_choose(0,v>0);if(v>0)runtime_rumble_gain_set(settings_gains[v-1]/1000.0f);});
 r.bars=YES;[rows addObject:r];
}
/* Mouse mode's sensitivity: a slider from 1 to 100 (50: 20 points of movement turn the camera at full speed). */
static TOTKSettingRow *settings_sensitivity_row(void) {
 NSString *t=settings_text("鼠标灵敏度","Mouse sensitivity");
 if(runtime_mouse_sensitivity_locked())return settings_fixed(t,[NSString stringWithFormat:@"%d",runtime_mouse_sensitivity()]);
 TOTKSettingRow *r=settings_row(t,nil,nil,^int{return runtime_mouse_sensitivity();},^(int v){runtime_mouse_sensitivity_set(v);});
 r.sliderMin=1;r.sliderMax=100;return r;
}
/* Auto's text: the type it takes the connected controller for (runtime/gamepad.m). */
static NSString *settings_pad_auto_text(void) {
 static const char *const names[GAMEPAD_TYPE_COUNT]={NULL,"Xbox","PlayStation","Switch"};
 int detected=-1;runtime_gamepad_type(&detected);
 if(detected>GAMEPAD_TYPE_AUTO&&detected<GAMEPAD_TYPE_COUNT)return [NSString stringWithFormat:@(TOTK_UI("自动（%s）","Auto (%s)")),names[detected]];
 return settings_text("自动（未接手柄）","Auto (no controller)");
}

/* What the key bindings page shows (the input page's first row): 1 the controller photo, 0 the keyboard and mouse cards.
   Kept in settings-keys-device.txt; at first the controller when one is connected. */
static NSInteger g_settings_bindings_device = -1;
static NSInteger settings_keys_device(void) {
 if(g_settings_bindings_device<0){int v;Class gc=NSClassFromString(@"GCController");
  g_settings_bindings_device=settings_read_int("settings-keys-device.txt",&v)&&v<=1?v:[[gc valueForKey:@"controllers"] count]>0;}
 return g_settings_bindings_device;
}
static NSArray<TOTKSettingRow *> *settings_rows(void) {
 NSMutableArray<TOTKSettingRow *> *rows=[NSMutableArray array];
 /* Display (design pack 2026-10-29): resolution, aspect ratio, frame cap | interpolation, edge smoothing | overlay, FPS graph */
 int modes=runtime_output_modes();
 NSMutableArray<NSString *> *res=[NSMutableArray array];NSMutableArray<NSNumber *> *resModes=[NSMutableArray array];
 static const int order[]={OUTPUT_MODE_720P,OUTPUT_MODE_OFF,OUTPUT_MODE_METALFX,OUTPUT_MODE_NATIVE,OUTPUT_MODE_NATIVE_METALFX};
 for(unsigned i=0;i<sizeof order/sizeof *order;i++){
  int m=order[i];if(!(modes&1<<m))continue;unsigned nw=0,nh=0;runtime_output_native(m,&nw,&nh);
  [res addObject:m==OUTPUT_MODE_720P?@"720p":m==OUTPUT_MODE_OFF?@"1080p":m==OUTPUT_MODE_METALFX?settings_text("1080p + MetalFX 放大","1080p + MetalFX upscaling"):
   m==OUTPUT_MODE_NATIVE_METALFX?settings_text("2K + MetalFX 放大（4K 屏）","2K + MetalFX upscaling (4K screens)"):
   nw==2560&&nh==1440?@"2K":
   [NSString stringWithFormat:@(TOTK_UI("原生 %u×%u","Native %u×%u")),nw,nh]];
  [resModes addObject:@(m)];}
 NSString *resolution=settings_text("分辨率","Resolution");
 if(res.count<2)[rows addObject:settings_fixed(resolution,@"1080p")];
 else [rows addObject:settings_row(resolution,res,resModes,
  ^int{return runtime_output_mode();},^(int v){settingsPendingOutput=-1;if(runtime_output_mode_set(v)){settings_window_for(v);settings_save_int("graphics-output.txt",v,"resolution");}})];
 NSString *aspect=settings_text("画面比例","Aspect ratio");
 if(!(runtime_aspect_ratio()>0))[rows addObject:settings_fixed(aspect,@"16:9")];
 else{NSMutableArray<NSNumber *> *ratios=[@[@(16.0/9),@(16.0/10),@(21.0/9),@(32.0/9)] mutableCopy];
  float start=runtime_aspect_start();BOOL listed=NO;   /* TOTK_ASPECT's ratio (2.39, 3440x1440 ...) as one more choice */
  for(NSNumber *r in ratios)listed|=aspect_near(start,r.doubleValue);
  if(start>0&&!listed)[ratios addObject:@(start)];
  NSMutableArray<NSString *> *choices=[NSMutableArray array];NSMutableArray<NSNumber *> *values=[NSMutableArray array];
  for(NSUInteger i=0;i<ratios.count;i++){[choices addObject:settings_ratio_text(ratios[i].floatValue)];[values addObject:@((int)i)];}
  [rows addObject:settings_row(aspect,choices,values,^int{
    float now=runtime_aspect_ratio();for(NSUInteger i=0;i<ratios.count;i++)if(aspect_near(now,ratios[i].doubleValue))return (int)i;return -1;},
   ^(int v){float r=ratios[(NSUInteger)v].floatValue;settingsPendingAspect=0;if(runtime_aspect_set_ratio(r)){runtime_front_resize_aspect(r);char word[32];snprintf(word,sizeof word,"%.8g",r);runtime_setting_save("graphics-aspect.txt",word,"aspect ratio");}})];}
 NSMutableArray<NSString *> *lockChoices=[NSMutableArray array];
 NSMutableArray<NSNumber *> *lockValues=[NSMutableArray array];
 for(unsigned i=0;i<TOTK_FPS_LIMIT_COUNT;i++){
  int fps=totk_fps_limits[i];
  [lockChoices addObject:fps?[NSString stringWithFormat:@"%d",fps]:settings_text("不限","Unlimited")];
  [lockValues addObject:@(fps)];
 }
 NSString *cap=settings_text("帧率上限","Frame rate cap");
 if(getenv("TOTK_MFX"))[rows addObject:settings_fixed(cap,settings_text("由 TOTK_MFX 设置","Set by TOTK_MFX"))];
 else { [rows addObject:settings_row(cap,lockChoices,lockValues,
  ^int{return nvn_fps_lock();},^(int v){nvn_fps_lock_set(v);if(v)dm_gfx_set_flag("mfx_interp",MFX_OFF);settings_save_frame();})]; }
 TOTKSettingRow *interp=settings_row(settings_text("补帧","Frame interpolation"),
  @[settings_on_off(0),@"30 → 60",@"40 → 80",@"50 → 100",settings_text("不限","Unlimited")],
  @[@(MFX_OFF),@(MFX_30),@(MFX_40),@(MFX_50),@(MFX_MAX)],
  ^int{return mfx_requested();},^(int v){dm_gfx_set_flag("mfx_interp",v);if(v!=MFX_OFF)nvn_fps_lock_set(0);settings_save_frame();});
 interp.divider=YES;[rows addObject:interp];
 [rows addObject:settings_row(settings_text("边缘平滑","Edge smoothing"),nil,nil,
  ^int{return taa_enabled();},^(int v){taa_set(v);settings_save_int("graphics-taa.txt",v,"edge smoothing");})];
 TOTKSettingRow *overlay=settings_row(settings_text("性能信息","Overlay"),
  @[settings_text("全部","All"),settings_text("FPS 与游玩时间","FPS + playtime"),settings_on_off(0)],
  @[@(HUD_MODE_ALL),@(HUD_MODE_MINIMAL),@(HUD_MODE_OFF)],
  ^int{return fps_overlay_hud_mode();},^(int v){fps_overlay_set_hud_mode(v);settings_save_int("graphics-overlay.txt",v,"overlay");[(TOTKHudBox *)hudBox refresh];});
 overlay.divider=YES;[rows addObject:overlay];
 [rows addObject:settings_row(settings_text("帧率曲线","FPS graph"),nil,nil,
  ^int{return fpsGraphEnabled;},^(int v){fpsGraphEnabled=v;runtime_setting_save("fps-graph.txt",v?"on":"off","FPS graph");[(TOTKFpsGraph *)fpsGraphBox setEnabled:v];[(TOTKHudBox *)hudBox refresh];})];
 NSUInteger display=rows.count;
 /* Input: input method, controller type, rumble, gyro aiming, bow trigger | mouse, sensitivity */
 [rows addObject:settings_row(settings_text("输入方式","Input method"),@[settings_text("手柄","Controller"),settings_text("键鼠","Keyboard & mouse")],@[@1,@0],
  ^int{return (int)settings_keys_device();},^(int v){g_settings_bindings_device=v;settings_save_int("settings-keys-device.txt",v,"key bindings device");})];
 TOTKSettingRow *pad=settings_row(settings_text("手柄类型","Controller type"),@[settings_pad_auto_text(),@"Xbox",@"PlayStation",@"Switch"],
  @[@(GAMEPAD_TYPE_AUTO),@(GAMEPAD_TYPE_XBOX),@(GAMEPAD_TYPE_PLAYSTATION),@(GAMEPAD_TYPE_SWITCH)],
  ^int{return runtime_gamepad_type(NULL);},^(int v){runtime_gamepad_type_set(v);});
 pad.firstTitle=^NSString *{return settings_pad_auto_text();};[rows addObject:pad];
 settings_rumble_rows(rows);
 [rows addObject:settings_feedback_row(1)];
 NSString *bow=settings_text("DualSense 拉弓扳机","DualSense bow trigger");
 /* Only where the game has the bow state (Breath of the Wild 1.9.0, Tears of the Kingdom 1.4.3): elsewhere there is nothing to switch, so no row. */
 if(!runtime_triggers_available()){}
 else if(runtime_triggers_locked())[rows addObject:settings_fixed(bow,settings_on_off(runtime_triggers_choice()))];
 else [rows addObject:settings_row(bow,nil,nil,^int{return runtime_triggers_choice();},^(int v){runtime_triggers_choose(v);})];
 TOTKSettingRow *mouse=settings_row(settings_text("鼠标控制视角","Mouse camera"),nil,nil,^int{return runtime_mouse_enabled();},^(int v){runtime_mouse_set(v);});
 mouse.divider=YES;[rows addObject:mouse];
 TOTKSettingRow *sens=settings_sensitivity_row();sens.enabledIf=^BOOL{return settings_keys_device()==0;};[rows addObject:sens];   /* greyed out, not picked, with the controller as the input method */
 for(NSUInteger i=0;i<rows.count;i++)rows[i].page=i<display?SETTINGS_PAGE_DISPLAY:SETTINGS_PAGE_INPUT;
 return rows;
}

/* ---- Key bindings ---- */
/* The game actions in the order the page lists them; action -1 starts a section. */
typedef struct { int action; const char *zh, *en, *tag; } SettingsAction;
/* The key bindings' row for the settings panel itself: key 1 on the keyboard (fixed), on a controller L3 + R3 together or
   the button picked (gamepad.m runtime_gamepad_settings_button, user 2026-10-29). */
enum { SETTINGS_BIND_PANEL = 100 };
static const SettingsAction *settings_actions(int *count) {
 static const SettingsAction list[]={
  {-1,"移动","Movement",NULL},
  {KEYMAP_MOVE_UP,"向前","Forward",NULL},{KEYMAP_MOVE_DOWN,"向后","Back",NULL},
  {KEYMAP_MOVE_LEFT,"向左","Left",NULL},{KEYMAP_MOVE_RIGHT,"向右","Right",NULL},
  {-1,"视角","Camera",NULL},
  {KEYMAP_CAMERA_UP,"视角向上","Camera up",NULL},{KEYMAP_CAMERA_DOWN,"视角向下","Camera down",NULL},
  {KEYMAP_CAMERA_LEFT,"视角向左","Camera left",NULL},{KEYMAP_CAMERA_RIGHT,"视角向右","Camera right",NULL},
  {-1,"动作","Actions",NULL},
  {KEYMAP_A,"确认 / 互动","Confirm / Interact","A"},
  {KEYMAP_B,"取消 / 冲刺","Cancel / Sprint","B"},
  {KEYMAP_X,"跳跃","Jump","X"},
  {KEYMAP_Y,"攻击","Attack","Y"},
  {KEYMAP_ZR,"弓（按住拉弓）","Bow (hold to draw)","ZR"},
  {KEYMAP_ZL,"锁定 / 举盾","Lock on / Shield","ZL"},
#ifdef GAME_BOTW
  {KEYMAP_L,"希卡石能力","Rune","L"},
#else
  {KEYMAP_L,"能力","Ability","L"},
#endif
  {KEYMAP_R,"投掷","Throw","R"},
  {KEYMAP_LSTICK,"蹲下","Crouch","L3"},
  {KEYMAP_RSTICK,"主视角","Look (first person)","R3"},
  {-1,"菜单","Menus",NULL},
  {KEYMAP_PLUS,"菜单","Menu","+"},
  {KEYMAP_MINUS,"地图","Map","−"},
  {-1,"十字键","D-pad",NULL},
  {KEYMAP_DUP,"十字键 上","D-pad up","↑"},{KEYMAP_DDOWN,"十字键 下","D-pad down","↓"},
  {KEYMAP_DLEFT,"十字键 左","D-pad left","←"},{KEYMAP_DRIGHT,"十字键 右","D-pad right","→"},
  {-1,"设置","Settings",NULL},
  {SETTINGS_BIND_PANEL,"打开 / 关闭设置面板","Open / close this panel",NULL}};
 *count=(int)(sizeof list/sizeof *list);
 return list;
}
/* The controller family the button names follow: the type picked, else the one auto sees, else Xbox. */
static int settings_pad_family(void) { int detected=-1,type=runtime_gamepad_type(&detected);if(!type)type=detected;return type>GAMEPAD_TYPE_AUTO?type:GAMEPAD_TYPE_XBOX; }
#include "button_glyphs.h"     /* the controller buttons' glyphs (Kenney Input Prompts, CC0) */
#include "controller_photo.h"  /* the controller photo of the key bindings page */

/* The design pack's names of the game actions (the key cards, the controller's labels); the others as in the action list. */
static NSString *settings_action_name(int action) {
 switch(action){
  case KEYMAP_Y:return settings_text("攻击","Attack");
  case KEYMAP_ZR:return settings_text("拉弓","Draw bow");
  case KEYMAP_X:return settings_text("跳跃","Jump");
  case KEYMAP_B:return settings_text("冲刺","Sprint");
  case KEYMAP_ZL:return settings_text("锁定 / 举盾","Lock on / Shield");
  case KEYMAP_R:return settings_text("投掷","Throw");
  case KEYMAP_A:return settings_text("互动","Interact");
  case KEYMAP_LSTICK:return settings_text("蹲下","Crouch");
  case KEYMAP_RSTICK:return settings_text("主视角","Look");
  case KEYMAP_MINUS:return settings_text("地图","Map");
  case KEYMAP_PLUS:return settings_text("菜单","Menu");
  case SETTINGS_BIND_PANEL:return settings_text("设置面板","Settings panel");
 }
 int count;const SettingsAction *all=settings_actions(&count);
 for(int i=0;i<count;i++)if(all[i].action==action)return @(TOTK_UI(all[i].zh,all[i].en));
 return settings_text("未设置","Not set");
}
/* The keyboard and mouse cards: the design pack's twelve, then the other actions the keyboard drives. */
static const int settings_card_actions[]={KEYMAP_Y,KEYMAP_ZR,KEYMAP_X,KEYMAP_B,KEYMAP_ZL,KEYMAP_L,KEYMAP_R,KEYMAP_A,KEYMAP_LSTICK,KEYMAP_RSTICK,KEYMAP_MINUS,KEYMAP_PLUS,
 KEYMAP_MOVE_UP,KEYMAP_MOVE_DOWN,KEYMAP_MOVE_LEFT,KEYMAP_MOVE_RIGHT,KEYMAP_CAMERA_UP,KEYMAP_CAMERA_DOWN,KEYMAP_CAMERA_LEFT,KEYMAP_CAMERA_RIGHT,
 KEYMAP_DUP,KEYMAP_DDOWN,KEYMAP_DLEFT,KEYMAP_DRIGHT};
/* A key cap's text: the key's name, or a mouse button's (left and right are drawn as a mouse instead). */
static NSString *settings_cap_text(int input) {
 if(input==KEYMAP_NONE)return @"—";
 switch(input-KEYMAP_MOUSE_FIRST){case 2:return settings_text("中键","M3");case 3:return @"M4";case 4:return @"M5";}
 const char *label=keymap_input_label(input);return label?@(label):[NSString stringWithFormat:@"#%d",input];
}

@interface TOTKFlippedView:NSView
@end
@implementation TOTKFlippedView
-(BOOL)isFlipped{return YES;}
@end

@class TOTKSettingsPanel;
/* One card of the keyboard and mouse page: the first key or mouse button of an action, and the action. A click (or A)
   waits for the new input; a right-click clears it. */
@interface TOTKKeyCard:TOTKFocusView
@property int gameAction;
@property BOOL waiting;
@property(weak) TOTKSettingsPanel *panel;
@end
/* One label of the controller photo: a button's glyph and what it does; a paddle the photo shows has nothing yet. */
@interface TOTKPadLabel:TOTKFocusView
@property(copy) NSString *glyph;
@property int physical;      /* controller_map.h; -1 a paddle (not set) */
@property BOOL glyphFirst;   /* the right side and the bottom right: the glyph, then the text */
@property BOOL waiting;      /* A pressed on it: the next controller button takes its action (the two swap) */
@property(copy) NSString *text;
@property(weak) TOTKSettingsPanel *panel;
-(CGFloat)idealWidth;
@end
/* The leader lines from the labels to the buttons, over the photo; the picked label's line brighter. */
@interface TOTKPadLines:NSView
@property(copy) NSString *picked;
@end

@interface TOTKSettingsPanel:NSView
@property(strong) NSArray<TOTKSettingRow *> *allRows;   /* every page's rows */
@property(strong) NSArray<TOTKSettingRow *> *rows;      /* the page shown (none on the key bindings page) */
@property(strong) NSMutableArray<TOTKFocusView *> *controllerTargets;   /* what a controller moves through on the page shown */
@property(strong) NSMutableArray<TOTKRowBand *> *bands;
@property(strong) NSMutableArray<TOTKKeyCard *> *cards;
@property(strong) NSMutableArray<TOTKPadLabel *> *padLabels;   /* in the order a controller moves through them */
@property(strong) TOTKPadLines *padLines;
@property(strong) NSScrollView *cardsScroll;
@property NSInteger page;
@property NSInteger controllerIndex;
@property BOOL controllerArmed;
@property uint64_t previousButtons;
@property int previousDirection;
@property double repeatAt;
@property(strong) NSTimer *timer;
@property NSPoint grab;   /* where in the panel the drag started */
@property(strong) TOTKTabs *tabs;
@property(strong) NSArray<NSView *> *pages;
@property(strong) NSTextField *deviceLabel;   /* the bottom left: the controller and its battery */
@property(strong) NSView *battery;
@property(strong) NSView *batteryLevel;
@property(strong) NSView *hints;
@property(nonatomic) BOOL usingPad;   /* the input method is the controller: its buttons in the hints, else the keys */
@property(strong) TOTKKeyCard *capturing;   /* the card waiting for an input */
@property(strong) TOTKPadLabel *padCapturing;   /* the controller label waiting for a button */
@property(strong) id keyMonitor;   /* the keyboard drives the panel like a controller */
@property double captureUntil;
@property(strong) id captureMonitor;
@property(strong) id clickMonitor;
@property BOOL drawings;   /* the key bindings page shown is the controller photo's (else the key cards) */
@property(strong) NSView *cardsPage;
@property(strong) NSView *padPage;   /* nil when the window is too small for the photo */
@property(strong) NSView *banner;    /* over the photo while a button is waited for: which action it is for */
@property(strong) NSTextField *bannerText;
@property CGFloat panelW;
-(void)selectPage:(NSInteger)page;
-(void)controllerPad:(RuntimeGamepadState)pad time:(double)now active:(BOOL)active;
-(void)controllerHighlight;
-(void)focusTarget:(TOTKFocusView *)view;
-(void)refresh;
-(void)refreshBindings;
-(void)beginCapture:(TOTKKeyCard *)card;
-(void)endCapture;
-(void)clearCard:(TOTKKeyCard *)card;
-(void)beginPadCapture:(TOTKPadLabel *)label;
@end

@implementation TOTKKeyCard
-(void)restyle{
 BOOL pill=self.focus;self.wantsLayer=YES;self.layer.cornerRadius=14;
 self.layer.backgroundColor=[NSColor colorWithCalibratedWhite:1 alpha:pill?0.92:0.06].CGColor;
 self.layer.shadowColor=NSColor.blackColor.CGColor;self.layer.shadowOpacity=pill?0.18:0;self.layer.shadowRadius=10;self.layer.shadowOffset=NSMakeSize(0,-6);
 [self setNeedsDisplay:YES];
}
-(void)drawRect:(NSRect)dirty{
 (void)dirty;BOOL pill=self.focus;NSRect b=self.bounds;KeyMap keys;runtime_keymap_get(&keys);int input=keys.slot[self.gameAction][0];
 NSColor *ink=settings_ink(pill,pill?1:0.9);CGFloat cx=6+26,cy=NSMidY(b);
 if(!self.waiting&&(input==KEYMAP_MOUSE_FIRST||input==KEYMAP_MOUSE_FIRST+1)){   /* the mouse, its left or right half lit */
  NSRect body=NSMakeRect(cx-9,cy-12,18,24);NSBezierPath *outline=[NSBezierPath bezierPathWithRoundedRect:body xRadius:9 yRadius:9];
  [NSGraphicsContext saveGraphicsState];[outline addClip];[ink setFill];
  NSRectFill(NSMakeRect(input==KEYMAP_MOUSE_FIRST?body.origin.x:cx,cy+3,9,9));[NSGraphicsContext restoreGraphicsState];
  NSBezierPath *mid=[NSBezierPath bezierPath];[mid moveToPoint:NSMakePoint(cx,NSMaxY(body))];[mid lineToPoint:NSMakePoint(cx,cy+3)];
  [ink setStroke];outline.lineWidth=1.5;mid.lineWidth=1.5;[outline stroke];[mid stroke];
 }else{   /* a key cap */
  NSString *cap=self.waiting?@"":settings_cap_text(input);
  NSDictionary *at=@{NSFontAttributeName:settings_font(12,YES),NSForegroundColorAttributeName:ink};NSSize ts=[cap sizeWithAttributes:at];
  CGFloat w=fmax(28,ceil(ts.width)+16);NSRect key=NSMakeRect(floor(cx-w/2),floor(cy-14),w,28);
  [(pill?settings_ink(YES,0.15):[NSColor colorWithCalibratedWhite:0 alpha:0.25]) setFill];[[NSBezierPath bezierPathWithRoundedRect:key xRadius:7 yRadius:7] fill];
  NSRect top=key;top.origin.y+=2;top.size.height-=2;
  [(pill?[NSColor colorWithCalibratedRed:0.86 green:0.87 blue:0.88 alpha:1]:[NSColor colorWithCalibratedWhite:0.62 alpha:1]) setFill];
  [settings_ink(pill,pill?0.1:0.14) setFill];
  [NSGraphicsContext saveGraphicsState];[[NSBezierPath bezierPathWithRoundedRect:key xRadius:7 yRadius:7] addClip];
  [[NSColor colorWithCalibratedWhite:pill?0.92:0.62 alpha:1] setFill];NSRectFill(top);[NSGraphicsContext restoreGraphicsState];
  [cap drawAtPoint:NSMakePoint(NSMidX(key)-ts.width/2,NSMidY(top)-ts.height/2) withAttributes:at];
  if(self.waiting){NSBezierPath *dash=[NSBezierPath bezierPathWithRoundedRect:NSInsetRect(key,1,1) xRadius:7 yRadius:7];CGFloat pat[2]={3,3};[dash setLineDash:pat count:2 phase:0];dash.lineWidth=1.5;[ink setStroke];[dash stroke];}
 }
 NSString *name=self.waiting?[NSString stringWithFormat:settings_text("请为「%@」按下新按键","Press a new key for “%@”"),settings_action_name(self.gameAction)]:settings_action_name(self.gameAction);
 NSDictionary *at=@{NSFontAttributeName:settings_font(16,pill),NSForegroundColorAttributeName:ink};NSSize ns=[name sizeWithAttributes:at];
 [name drawAtPoint:NSMakePoint(6+52+6,floor(cy-ns.height/2)) withAttributes:at];
}
-(void)mouseDown:(NSEvent *)e{(void)e;if(self.onPick)self.onPick();[self.panel beginCapture:self];}
-(void)rightMouseDown:(NSEvent *)e{(void)e;if(self.onPick)self.onPick();[self.panel clearCard:self];}
@end

@implementation TOTKPadLabel
-(NSDictionary *)attrs{BOOL empty=self.physical<0||!self.text;
 return @{NSFontAttributeName:settings_font(14,self.focus),NSForegroundColorAttributeName:self.focus?settings_ink(YES,1):settings_ink(NO,empty?0.45:0.9)};}
-(NSString *)shown{return self.text?:settings_text("未设置","Not set");}
-(CGFloat)idealWidth{return 8+30+8+ceil([[self shown] sizeWithAttributes:[self attrs]].width)+8;}
-(void)restyle{
 BOOL pill=self.focus;self.wantsLayer=YES;self.layer.cornerRadius=10;
 self.layer.backgroundColor=(pill?[NSColor colorWithCalibratedWhite:1 alpha:0.94]:NSColor.clearColor).CGColor;
 self.layer.shadowColor=NSColor.blackColor.CGColor;self.layer.shadowOpacity=pill?0.2:0;self.layer.shadowRadius=10;self.layer.shadowOffset=NSMakeSize(0,-6);
 [self setNeedsDisplay:YES];
}
-(void)drawRect:(NSRect)dirty{
 (void)dirty;NSRect b=self.bounds;NSString *t=[self shown];NSDictionary *at=[self attrs];NSSize ts=[t sizeWithAttributes:at];
 CGFloat gx=self.glyphFirst?8:b.size.width-8-30,tx=self.glyphFirst?8+30+8:8;
 NSBezierPath *g=button_glyph_path(self.physical<0?@"FRAME":self.glyph,NSMakeRect(gx,floor((b.size.height-30)/2),30,30));
 [(self.focus?settings_ink(YES,1):settings_ink(NO,self.physical<0?0.6:1)) setFill];[g fill];
 [t drawAtPoint:NSMakePoint(tx,floor((b.size.height-ts.height)/2)) withAttributes:at];
}
-(void)mouseDown:(NSEvent *)e{(void)e;if(self.physical<0)return;if(self.onPick)self.onPick();[self.panel beginPadCapture:self];}
@end

/* The photo's labels and lines (design pack KeysDiagram): the glyph, the physical button (controller_map.h: 0 A south,
   1 B east, 2 X west, 3 Y north, 4 L3, 5 R3, 6 L, 7 R, 8 ZL, 9 ZR, 10 +, 11 −), the side (L R TL TR BL BR), the label's
   middle, and the line from the label to the button in the 704 x 420 drawing (y down; the last point on the button). */
typedef struct { const char *glyph; int physical; char side[3]; CGFloat y; int points; CGFloat p[6]; } SettingsPadLabel;
static const SettingsPadLabel settings_pad_labels[]={   /* the order a controller moves through them */
 {"ZL",8,"L",56,3,{156,56,184,56,236,92.6}},{"L",6,"L",108,3,{156,108,184,108,216.8,102.8}},{"L3",4,"L",160,3,{156,160,184,160,204,151}},
 {"−",11,"TL",24,2,{303.6,42,303.6,123}},{"+",10,"TR",24,2,{402.9,42,402.9,123}},
 {"ZR",9,"R",56,3,{548,56,520,56,460.3,92.6}},{"R",7,"R",108,3,{548,108,520,108,479.5,102.8}},{"Y",3,"R",160,3,{548,160,520,160,477.6,130}},
 {"B",1,"R",212,3,{548,212,520,212,503.2,157.3}},{"A",0,"R",264,3,{548,264,520,264,477.6,184.5}},
 {"R3",5,"BL",384,2,{407.7,367,407.7,247}},{"X",2,"BR",384,2,{438.2,367,438.2,170}},
 {"PL",-1,"L",330,3,{156,330,184,330,226.8,309.4}},{"PR",-1,"R",330,3,{548,330,520,330,483,309.4}}};
enum { SETTINGS_PAD_LABELS = sizeof settings_pad_labels / sizeof *settings_pad_labels, SETTINGS_PAD_PICKABLE = 12 };

@implementation TOTKPadLines
-(BOOL)isFlipped{return YES;}
-(NSView *)hitTest:(NSPoint)p{(void)p;return nil;}
-(void)drawRect:(NSRect)dirty{
 (void)dirty;
 for(int pass=0;pass<2;pass++)for(unsigned i=0;i<SETTINGS_PAD_LABELS;i++){
  const SettingsPadLabel *l=&settings_pad_labels[i];BOOL on=self.picked&&[self.picked isEqualToString:@(l->glyph)];if(on!=(pass==1))continue;
  NSBezierPath *line=[NSBezierPath bezierPath];line.lineJoinStyle=NSLineJoinStyleRound;line.lineWidth=on?1.8:1.2;
  for(int k=0;k<l->points;k++){NSPoint pt=NSMakePoint(l->p[2*k],l->p[2*k+1]);if(k)[line lineToPoint:pt];else [line moveToPoint:pt];}
  [[NSColor colorWithCalibratedWhite:1 alpha:on?1:0.6] setStroke];[line stroke];
  CGFloat r=on?4:2.5,x=l->p[2*l->points-2],y=l->p[2*l->points-1];[[NSColor whiteColor] setFill];[[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(x-r,y-r,2*r,2*r)] fill];
 }
}
@end

/* Where the player dragged the panel (user 2026-10-25: it can be moved), as the fraction of the free room left and below
   it, so a new opening and a resized window keep the spot; -1 = centred. The page shown last opens again. */
static NSPoint g_settings_at = {-1, -1};
static NSInteger g_settings_page = SETTINGS_PAGE_DISPLAY;
/* The design pack's sizes: 640 x 560, the controller photo's page 760 x 580; every page is made at the larger one when the
   window has room (user 2026-10-29: the panel keeps one size when the tabs change). */
enum { SETTINGS_W = 640, SETTINGS_H = 560, SETTINGS_WIDE_W = 760, SETTINGS_WIDE_H = 580, SETTINGS_PAD = 28, SETTINGS_ROW_H = 50, SETTINGS_GAP = 2, SETTINGS_DIV = 17 };
static BOOL settings_drawings_fit(void) {
 NSSize room=window?window.contentView.bounds.size:NSMakeSize(CGFLOAT_MAX,CGFLOAT_MAX);
 return room.width>=SETTINGS_WIDE_W+8&&room.height>=SETTINGS_WIDE_H+8;
}
/* The controller photo when the input method is the controller and the window has room for it; else the key cards. */
static BOOL settings_drawings_for(NSInteger page) { return page==SETTINGS_PAGE_BINDINGS&&settings_keys_device()==1&&settings_drawings_fit(); }
/* The controller GameController sees first (the bottom left of the panel), nil none. */
static NSString *settings_pad_name(void) {
 Class gc=NSClassFromString(@"GCController");id pad=[[gc valueForKey:@"controllers"] firstObject];
 NSString *name=pad?[pad valueForKey:@"vendorName"]:nil;return [name isKindOfClass:NSString.class]&&name.length?name:nil;
}
/* A controller button in a white circle (the design pack's footer), or a key cap when the keyboard and mouse are the input method
   then what it does. */
static NSView *settings_hint(NSString *glyph,BOOL key,NSString *text){
 NSTextField *g=[NSTextField labelWithString:glyph];g.font=[NSFont systemFontOfSize:10 weight:NSFontWeightBold];g.textColor=settings_ink(YES,1);g.alignment=NSTextAlignmentCenter;[g sizeToFit];
 CGFloat gw=key?fmax(20,ceil(g.frame.size.width)+10):20;
 NSView *dot=[[NSView alloc] initWithFrame:NSMakeRect(0,0,gw,20)];dot.wantsLayer=YES;dot.layer.backgroundColor=[NSColor colorWithCalibratedWhite:1 alpha:0.9].CGColor;dot.layer.cornerRadius=key?5:10;
 g.frame=NSMakeRect(0,floor((20-g.frame.size.height)/2),gw,g.frame.size.height);[dot addSubview:g];
 NSTextField *t=[NSTextField labelWithString:text];t.font=settings_font(13,NO);t.textColor=settings_ink(NO,0.6);[t sizeToFit];
 NSView *v=[[NSView alloc] initWithFrame:NSMakeRect(0,0,gw+7+t.frame.size.width,20)];t.frame=NSMakeRect(gw+7,floor((20-t.frame.size.height)/2),t.frame.size.width,t.frame.size.height);[v addSubview:dot];[v addSubview:t];return v;
}
static NSView *settings_chip(NSString *text){
 NSTextField *t=[NSTextField labelWithString:text];t.font=[NSFont systemFontOfSize:12 weight:NSFontWeightBold];t.textColor=settings_ink(NO,0.6);[t sizeToFit];
 CGFloat w=t.frame.size.width+14;NSView *v=[[NSView alloc] initWithFrame:NSMakeRect(0,0,w,21)];v.wantsLayer=YES;v.layer.cornerRadius=6;
 v.layer.backgroundColor=[NSColor colorWithCalibratedWhite:1 alpha:0.1].CGColor;t.frame=NSMakeRect(7,floor((21-t.frame.size.height)/2),t.frame.size.width,t.frame.size.height);[v addSubview:t];return v;
}
@implementation TOTKSettingsPanel
-(instancetype)init{
 NSArray<TOTKSettingRow *> *rows=settings_rows();
 BOOL wide=settings_drawings_fit();
 CGFloat W=wide?SETTINGS_WIDE_W:SETTINGS_W,h=wide?SETTINGS_WIDE_H:SETTINGS_H,room=window?window.contentView.bounds.size.height-8:CGFLOAT_MAX;
 if(h>room)h=fmax(320,room);   /* a short window (21:9, 32:9 on a laptop screen): the rows close up below */
 if(!(self=[super initWithFrame:NSMakeRect(0,0,W,h)]))return nil;
 self.panelW=W;self.allRows=rows;self.appearance=[NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
 self.controllerTargets=[NSMutableArray array];self.bands=[NSMutableArray array];self.cards=[NSMutableArray array];self.padLabels=[NSMutableArray array];
 self.autoresizingMask=NSViewMinXMargin|NSViewMaxXMargin|NSViewMinYMargin|NSViewMaxYMargin;
 /* The glass (design pack): the game behind blurred, the dark blue-grey tint over it, a 1 pt rim of white 16 %, a white
    18 % line inside the top, and a 100 pt shadow 40 below. */
 self.wantsLayer=YES;self.layer.shadowColor=NSColor.blackColor.CGColor;self.layer.shadowOpacity=0.4;self.layer.shadowRadius=50;self.layer.shadowOffset=NSMakeSize(0,-40);
 self.layer.shadowPath=[NSBezierPath bezierPathWithRoundedRect:NSMakeRect(0,0,W,h) xRadius:28 yRadius:28].CGPath;
 NSVisualEffectView *glass=[[NSVisualEffectView alloc] initWithFrame:NSMakeRect(0,0,W,h)];glass.blendingMode=NSVisualEffectBlendingModeWithinWindow;glass.state=NSVisualEffectStateActive;
 /* A light HUD material blurs what is behind; with 23 % of the design's tint it gives the design's grey over white
    (rgba(34,44,58,.42) over the page, 2026-10-29: 162 164 168 against 162 166 172). */
 glass.material=NSVisualEffectMaterialHUDWindow;glass.appearance=[NSAppearance appearanceNamed:NSAppearanceNameAqua];
 glass.wantsLayer=YES;glass.layer.cornerRadius=28;glass.layer.masksToBounds=YES;
 glass.layer.borderWidth=1;glass.layer.borderColor=[NSColor colorWithCalibratedWhite:1 alpha:0.16].CGColor;
 NSView *tint=[[NSView alloc] initWithFrame:glass.bounds];tint.wantsLayer=YES;
 tint.layer.backgroundColor=[NSColor colorWithCalibratedRed:34/255.0 green:44/255.0 blue:58/255.0 alpha:0.23].CGColor;[glass addSubview:tint];
 CALayer *shine=[CALayer layer];shine.frame=CGRectMake(24,h-2,W-48,1);shine.backgroundColor=[NSColor colorWithCalibratedWhite:1 alpha:0.18].CGColor;[tint.layer addSublayer:shine];
 [self addSubview:glass];
 /* The tabs: L | 画面 操作 按键 | R, 24 pt from the top (22 on the photo's page). */
 TOTKTabs *tabs=[TOTKTabs new];tabs.titles=@[settings_text("画面","Display"),settings_text("操作","Input"),settings_text("按键","Keys")];tabs.target=self;tabs.action=@selector(tabChanged:);
 NSSize ts=[tabs idealSize];NSView *chipL=settings_chip(@"L"),*chipR=settings_chip(@"R");
 CGFloat top=22,tx=floor((W-(chipL.frame.size.width+12+ts.width+12+chipR.frame.size.width))/2),ty=h-top-ts.height;
 chipL.frame=NSMakeRect(tx,ty+floor((ts.height-21)/2),chipL.frame.size.width,21);tabs.frame=NSMakeRect(NSMaxX(chipL.frame)+12,ty,ts.width,ts.height);
 chipR.frame=NSMakeRect(NSMaxX(tabs.frame)+12,chipL.frame.origin.y,chipR.frame.size.width,21);
 [self addSubview:chipL];[self addSubview:tabs];[self addSubview:chipR];self.tabs=tabs;
 /* The bottom line, 20 pt from the bottom: the controller and its battery; the buttons' hints at the right. */
 NSTextField *device=[NSTextField labelWithString:@""];device.font=settings_font(13,NO);device.textColor=settings_ink(NO,0.6);device.frame=NSMakeRect(SETTINGS_PAD+6+24,20,300,18);
 [self addSubview:device];self.deviceLabel=device;
 /* The controller glyph (SF Symbol) before its name (user 2026-10-29). */
 NSImageView *glyph=[NSImageView imageViewWithImage:[NSImage imageWithSystemSymbolName:@"gamecontroller" accessibilityDescription:nil]?:[NSImage new]];
 glyph.symbolConfiguration=[NSImageSymbolConfiguration configurationWithPointSize:15 weight:NSFontWeightRegular];glyph.contentTintColor=settings_ink(NO,0.6);
 glyph.frame=NSMakeRect(SETTINGS_PAD+6,19,20,20);[self addSubview:glyph];
 NSView *battery=[[NSView alloc] initWithFrame:NSMakeRect(0,24,22,11)];battery.wantsLayer=YES;battery.layer.borderWidth=1.5;battery.layer.cornerRadius=3;
 battery.layer.borderColor=settings_ink(NO,0.6).CGColor;NSView *level=[[NSView alloc] initWithFrame:NSMakeRect(3,3,13,5)];level.wantsLayer=YES;level.layer.cornerRadius=1;
 level.layer.backgroundColor=settings_ink(NO,0.75).CGColor;[battery addSubview:level];[self addSubview:battery];self.battery=battery;self.batteryLevel=level;
 NSView *hints=[[NSView alloc] initWithFrame:NSZeroRect];[self addSubview:hints];self.hints=hints;
 [self layoutHints];
 /* The display and input pages: the rows from 86 pt down (24 + the tabs + 22), 50 pt each and 2 apart, a rule between groups. */
 NSUInteger perPage[2]={0,0},dividers[2]={0,0};for(TOTKSettingRow *r in rows){perPage[r.page]++;if(r.divider)dividers[r.page]++;}
 CGFloat bodyTop=h-top-ts.height-22,bodyBottom=wide?48:60,rowH=SETTINGS_ROW_H,gap=SETTINGS_GAP,div=SETTINGS_DIV,n=MAX(perPage[0],perPage[1]),dn=MAX(dividers[0],dividers[1]);
 if(n&&n*(rowH+gap)-gap+dn*div>bodyTop-bodyBottom){div=9;rowH=fmax(26,floor((bodyTop-bodyBottom-dn*div+gap)/n-gap));}
 NSMutableArray<NSView *> *pages=[NSMutableArray array];__weak TOTKSettingsPanel *weak=self;
 for(NSInteger p=0;p<2;p++){
  NSView *page=[[NSView alloc] initWithFrame:NSMakeRect(0,0,W,h)];NSUInteger k=0;CGFloat y=bodyTop;
  for(TOTKSettingRow *r in rows){
   if(r.page!=p)continue;
   if(r.divider&&k>0){y-=div;NSView *rule=[[NSView alloc] initWithFrame:NSMakeRect(SETTINGS_PAD+18,y+floor(div/2),W-2*SETTINGS_PAD-36,1)];rule.wantsLayer=YES;
    rule.layer.backgroundColor=[NSColor colorWithCalibratedWhite:1 alpha:0.1].CGColor;[page addSubview:rule];}
   y-=rowH;CGFloat rw=W-2*SETTINGS_PAD;k++;
   TOTKRowBand *band=[[TOTKRowBand alloc] initWithFrame:NSMakeRect(SETTINGS_PAD,y,rw,rowH)];band.row=r;[page addSubview:band];[self.bands addObject:band];
   NSTextField *label=[NSTextField labelWithString:r.title];label.font=settings_font(16,YES);[label sizeToFit];label.lineBreakMode=NSLineBreakByTruncatingTail;
   CGFloat lw=fmin(label.frame.size.width+4,rw-200);label.frame=NSMakeRect(18,floor((rowH-label.frame.size.height)/2),lw,label.frame.size.height);[band addSubview:label];band.label=label;
   TOTKValueControl *c=[TOTKValueControl new];c.row=r;c.refusesFirstResponder=YES;c.frame=NSMakeRect(18+lw+12,0,rw-18-(18+lw+12),rowH);[band addSubview:c];r.control=c;
   __weak TOTKRowBand *wb=band;band.onPick=^{[weak focusTarget:wb];};
   [band restyle];y-=gap;
  }
  [self addSubview:page];[pages addObject:page];
 }
 NSView *cards=[self cardsPageWithTop:h-top-ts.height-24 bottom:60];[self addSubview:cards];self.cardsPage=cards;[pages addObject:cards];
 if(wide){self.padPage=[self padPageWithTop:h-top-ts.height-8];[self addSubview:self.padPage];}
 /* The prompt over the photo while a button is waited for (the action it is for), above the bottom line. */
 NSView *banner=[[NSView alloc] initWithFrame:NSMakeRect(0,50,W,32)];banner.wantsLayer=YES;banner.layer.cornerRadius=16;banner.layer.backgroundColor=[NSColor colorWithCalibratedWhite:1 alpha:0.94].CGColor;
 banner.layer.shadowColor=NSColor.blackColor.CGColor;banner.layer.shadowOpacity=0.2;banner.layer.shadowRadius=10;banner.layer.shadowOffset=NSMakeSize(0,-6);banner.hidden=YES;
 NSTextField *bt=[NSTextField labelWithString:@""];bt.font=settings_font(14,YES);bt.textColor=settings_ink(YES,1);[banner addSubview:bt];[self addSubview:banner];self.banner=banner;self.bannerText=bt;
 self.pages=pages;
 [self selectPage:g_settings_page];[self refresh];
 self.keyMonitor=[NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown handler:^NSEvent *(NSEvent *e){TOTKSettingsPanel *panel=weak;return panel?[panel keyed:e]:e;}];
 self.timer=[NSTimer scheduledTimerWithTimeInterval:0.5 repeats:YES block:^(NSTimer *t){(void)t;
  TOTKSettingsPanel *panel=weak;if(!panel||panel.hidden)return;
  if((panel.capturing||panel.padCapturing)&&runtime_now_ns()/1e9>panel.captureUntil)[panel endCapture];
  [panel refresh];}];
 return self;
}
/* The keyboard and mouse page: two columns of cards, 52 pt, 8 apart (the design pack's twelve first), scrolling. */
-(NSView *)cardsPageWithTop:(CGFloat)top bottom:(CGFloat)bottom{
 CGFloat W=self.panelW,h=self.frame.size.height,cw=floor((W-2*SETTINGS_PAD-8)/2);
 NSView *page=[[NSView alloc] initWithFrame:NSMakeRect(0,0,W,h)];
 NSScrollView *scroll=[[NSScrollView alloc] initWithFrame:NSMakeRect(SETTINGS_PAD-12,bottom,W-2*SETTINGS_PAD+24,top-bottom)];
 scroll.hasVerticalScroller=YES;scroll.autohidesScrollers=YES;scroll.drawsBackground=NO;scroll.borderType=NSNoBorder;scroll.scrollerStyle=NSScrollerStyleOverlay;
 unsigned n=sizeof settings_card_actions/sizeof *settings_card_actions;CGFloat listH=((n+1)/2)*60-8+24;
 TOTKFlippedView *list=[[TOTKFlippedView alloc] initWithFrame:NSMakeRect(0,0,scroll.frame.size.width,listH)];__weak TOTKSettingsPanel *weak=self;
 for(unsigned i=0;i<n;i++){
  TOTKKeyCard *card=[[TOTKKeyCard alloc] initWithFrame:NSMakeRect(12+(i%2)*(cw+8),(i/2)*60,cw,52)];card.gameAction=settings_card_actions[i];card.panel=self;
  __weak TOTKKeyCard *wc=card;card.onPick=^{[weak focusTarget:wc];};[card restyle];[list addSubview:card];[self.cards addObject:card];
 }
 scroll.documentView=list;[page addSubview:scroll];self.cardsScroll=scroll;
 return page;
}
/* The controller page: the photo (360 x 252 at 172, 90 of the 704 x 420 drawing), its lines and labels. */
-(NSView *)padPageWithTop:(CGFloat)top{
 CGFloat W=self.panelW,h=self.frame.size.height;NSView *page=[[NSView alloc] initWithFrame:NSMakeRect(0,0,W,h)];
 TOTKFlippedView *area=[[TOTKFlippedView alloc] initWithFrame:NSMakeRect(SETTINGS_PAD,top-420,704,420)];[page addSubview:area];
 NSImage *photo=[[NSImage alloc] initWithData:[NSData dataWithBytesNoCopy:(void *)controller_photo_png length:controller_photo_png_len freeWhenDone:NO]];photo.size=NSMakeSize(360,252);
 NSImageView *image=[NSImageView imageViewWithImage:photo];image.frame=NSMakeRect(172,90,360,252);image.imageScaling=NSImageScaleProportionallyUpOrDown;
 image.wantsLayer=YES;image.layer.shadowColor=NSColor.blackColor.CGColor;image.layer.shadowOpacity=0.35;image.layer.shadowRadius=12;image.layer.shadowOffset=NSMakeSize(0,-18);
 [area addSubview:image];
 TOTKPadLines *lines=[[TOTKPadLines alloc] initWithFrame:area.bounds];[area addSubview:lines];self.padLines=lines;
 __weak TOTKSettingsPanel *weak=self;
 for(unsigned i=0;i<SETTINGS_PAD_LABELS;i++){
  const SettingsPadLabel *l=&settings_pad_labels[i];TOTKPadLabel *label=[[TOTKPadLabel alloc] initWithFrame:NSMakeRect(0,l->y-17,100,34)];
  label.glyph=@(l->glyph);label.physical=l->physical;label.panel=self;label.glyphFirst=l->side[0]=='R'||!strcmp(l->side,"TR")||!strcmp(l->side,"BR");
  __weak TOTKPadLabel *wl=label;label.onPick=^{[weak focusTarget:wl];};[label restyle];[area addSubview:label];[self.padLabels addObject:label];
 }
 return page;
}
/* The labels' places: the left ones end 554 pt from the drawing's right, the right ones start 554 from its left (TL 386,
   TR 388, BL 286, BR 428), so a longer text grows away from the photo. */
-(void)layoutPadLabels{
 for(unsigned i=0;i<self.padLabels.count&&i<SETTINGS_PAD_LABELS;i++){
  TOTKPadLabel *label=self.padLabels[i];const char *side=settings_pad_labels[i].side;CGFloat w=[label idealWidth],x;
  if(!strcmp(side,"L"))x=704-554-w;else if(!strcmp(side,"R"))x=554;else if(!strcmp(side,"TL"))x=704-386-w;else if(!strcmp(side,"TR"))x=388;
  else if(!strcmp(side,"BL"))x=704-286-w;else x=428;
  NSRect f=NSMakeRect(x,settings_pad_labels[i].y-17,w,34);if(!NSEqualRects(f,label.frame)){label.frame=f;[label setNeedsDisplay:YES];}
 }
}
/* The bottom right: what A and B do here (the input method picked in the panel decides whether they are the controller's
   buttons or the keys, not which was touched last); while a button is waited for, only the way out. on the key bindings page Y (or Delete) restores the
   default layout of the device shown. */
-(void)layoutHints{
 for(NSView *v in [self.hints.subviews copy])[v removeFromSuperview];
 int f=settings_pad_family();BOOL ps=f==GAMEPAD_TYPE_PLAYSTATION,pad=self.usingPad;NSInteger p=self.page;
 BOOL waiting=self.capturing||self.padCapturing;
 NSString *confirm=p==SETTINGS_PAGE_DISPLAY?settings_text("确定","Select"):p==SETTINGS_PAGE_INPUT?settings_text("切换","Change"):settings_text("更改","Change");
 NSMutableArray<NSView *> *items=[NSMutableArray array];
 if(!waiting&&p==SETTINGS_PAGE_BINDINGS)[items addObject:settings_hint(pad?(ps?@"△":@"Y"):@"⌫",!pad,settings_text("恢复默认","Defaults"))];
 if(!waiting)[items addObject:settings_hint(pad?(ps?@"×":@"A"):@"↩",!pad,confirm)];
 [items addObject:settings_hint(pad?(ps?@"○":@"B"):@"Esc",!pad,waiting?settings_text("取消","Cancel"):settings_text("返回","Back"))];
 CGFloat x=0;for(NSView *v in items){v.frame=NSMakeRect(x,0,v.frame.size.width,20);[self.hints addSubview:v];x=NSMaxX(v.frame)+20;}
 CGFloat w=fmax(0,x-20);self.hints.frame=NSMakeRect(self.panelW-SETTINGS_PAD-6-w,20,w,20);
}
-(void)setUsingPad:(BOOL)pad{if(_usingPad==pad)return;_usingPad=pad;[self layoutHints];}
/* The prompt over the photo: the action the waited-for button will do. */
-(void)updateBanner{
 TOTKPadLabel *l=self.padCapturing;self.banner.hidden=!l;if(!l)return;
 self.bannerText.stringValue=[NSString stringWithFormat:settings_text("请为「%@」按下手柄上的新按键","Press the new button for “%@”"),l.text?:settings_text("未设置","Not set")];
 [self.bannerText sizeToFit];CGFloat w=self.bannerText.frame.size.width+40;
 self.banner.frame=NSMakeRect(floor((self.panelW-w)/2),50,w,32);self.bannerText.frame=NSMakeRect(20,floor((32-self.bannerText.frame.size.height)/2),self.bannerText.frame.size.width,self.bannerText.frame.size.height);
}
-(void)tabChanged:(TOTKTabs *)tabs{[self selectPage:tabs.selectedSegment];}
-(void)selectPage:(NSInteger)page{
 [self endCapture];
 page=page<0?0:page>=SETTINGS_PAGES?SETTINGS_PAGES-1:page;
 BOOL changed=self.window&&page!=self.page;
 self.drawings=settings_drawings_for(page)&&self.padPage;
 self.page=page;g_settings_page=page;self.tabs.selectedSegment=page;_usingPad=settings_keys_device()==1;
 for(NSInteger i=0;i<(NSInteger)self.pages.count;i++)self.pages[(NSUInteger)i].hidden=i!=page||(i==SETTINGS_PAGE_BINDINGS&&self.drawings);
 self.padPage.hidden=!(page==SETTINGS_PAGE_BINDINGS&&self.drawings);
 NSView *shown=page==SETTINGS_PAGE_BINDINGS?(self.drawings?self.padPage:self.cardsPage):self.pages[(NSUInteger)page];
 if(changed){shown.alphaValue=0;[NSAnimationContext runAnimationGroup:^(NSAnimationContext *c){c.duration=0.22;shown.animator.alphaValue=1;}];}
 NSMutableArray<TOTKSettingRow *> *rows=[NSMutableArray array];
 for(TOTKSettingRow *r in self.allRows)if(r.page==page)[rows addObject:r];
 self.rows=rows;self.controllerIndex=0;
 [self layoutHints];
 [self updateTargets];[self refreshBindings];
 [self.cardsScroll.documentView scrollPoint:NSZeroPoint];
 [self.window makeFirstResponder:self.window.contentView];   /* key 1 and Esc keep reaching the window */
}
/* What a controller moves through: the rows; the key cards; the controller's labels (not the paddles). */
-(void)updateTargets{
 [self.controllerTargets removeAllObjects];
 if(self.page!=SETTINGS_PAGE_BINDINGS){for(TOTKRowBand *b in self.bands)if(b.row.page==self.page)[self.controllerTargets addObject:b];}
 else if(self.drawings){for(NSUInteger i=0;i<SETTINGS_PAD_PICKABLE&&i<self.padLabels.count;i++)[self.controllerTargets addObject:self.padLabels[i]];}
 else [self.controllerTargets addObjectsFromArray:self.cards];
 if(self.controllerIndex>=(NSInteger)self.controllerTargets.count)self.controllerIndex=0;
 [self controllerHighlight];
}
-(BOOL)controllerEnabled:(NSInteger)i{
 if(i<0||i>=(NSInteger)self.controllerTargets.count)return NO;
 TOTKFocusView *v=self.controllerTargets[(NSUInteger)i];
 return ![v isKindOfClass:TOTKRowBand.class]||((TOTKRowBand *)v).row.enabled;
}
/* The picked item is always shown (the design pack): the white block, and on the photo its line brighter. */
-(void)controllerHighlight{
 if(![self controllerEnabled:self.controllerIndex])
  for(NSUInteger i=0;i<self.controllerTargets.count;i++)if([self controllerEnabled:(NSInteger)i]){self.controllerIndex=(NSInteger)i;break;}
 for(TOTKRowBand *b in self.bands)b.focus=NO;
 for(NSUInteger i=0;i<self.controllerTargets.count;i++)self.controllerTargets[i].focus=(NSInteger)i==self.controllerIndex;
 if(self.padLines){NSString *picked=nil;for(TOTKPadLabel *l in self.padLabels)if(l.focus)picked=l.glyph;
  if(picked!=self.padLines.picked&&![picked isEqual:self.padLines.picked]){self.padLines.picked=picked;[self.padLines setNeedsDisplay:YES];}
  [self layoutPadLabels];}
}
-(void)focusTarget:(TOTKFocusView *)view{
 NSUInteger i=[self.controllerTargets indexOfObject:view];if(i==NSNotFound||![self controllerEnabled:(NSInteger)i])return;
 self.controllerIndex=(NSInteger)i;[self controllerHighlight];
}
-(void)controllerMove:(int)step{
 NSInteger count=self.controllerTargets.count;if(!count)return;
 if(abs(step)>1){NSInteger to=self.controllerIndex+step;if(to<0||to>=count)return;self.controllerIndex=to;}   /* the cards: a row up or down */
 else for(NSInteger n=0;n<count;n++){self.controllerIndex=(self.controllerIndex+step+count)%count;if([self controllerEnabled:self.controllerIndex])break;}
 [self controllerHighlight];
 TOTKFocusView *v=self.controllerTargets[(NSUInteger)self.controllerIndex];
 if([v isKindOfClass:TOTKKeyCard.class])[v scrollRectToVisible:NSInsetRect(v.bounds,0,-12)];   /* keep the card in view */
}
/* A on what is picked: a switch flips; a card waits for its new key; a controller label waits for its new button.
   Left / right: a row's value, slider or bars (the design notes: no A needed); on the cards and labels a step sideways. */
-(void)controllerChange:(int)step select:(BOOL)select{
 NSInteger i=self.controllerIndex;if(![self controllerEnabled:i])return;
 TOTKFocusView *target=self.controllerTargets[(NSUInteger)i];
 if([target isKindOfClass:TOTKKeyCard.class]){if(select)[self beginCapture:(TOTKKeyCard *)target];else [self controllerMove:step];return;}
 if([target isKindOfClass:TOTKPadLabel.class]){if(select)[self beginPadCapture:(TOTKPadLabel *)target];else [self controllerMove:step];return;}
 TOTKSettingRow *r=((TOTKRowBand *)target).row;
 if(select){if(!r.choices&&!r.slider)[r activate];}else [r stepBy:step];
 [self controllerHighlight];
}
/* Y (or Delete) on the key bindings page: the default layout of the device shown. */
-(void)resetDevice{
 [self endCapture];
 if(self.drawings){runtime_controller_map_reset();runtime_gamepad_settings_button_set(-1);}else runtime_keymap_reset();
 [self refreshBindings];
}
-(void)refresh{
 self.usingPad=settings_keys_device()==1;
 if(self.page==SETTINGS_PAGE_BINDINGS&&self.window&&(settings_drawings_for(self.page)&&self.padPage?YES:NO)!=self.drawings)[self selectPage:self.page];   /* the input method changed */
 for(TOTKSettingRow *r in self.allRows)[r refresh];
 for(TOTKRowBand *b in self.bands)[b refresh];
 NSString *name=settings_pad_name();int pct=name?runtime_gamepad_battery_percent():-1;
 NSString *text=name?:settings_text("未连接手柄","No controller");
 if(![self.deviceLabel.stringValue isEqualToString:text]){self.deviceLabel.stringValue=text;[self.deviceLabel sizeToFit];}
 self.battery.hidden=pct<0;self.battery.frame=NSMakeRect(NSMaxX(self.deviceLabel.frame)+8,NSMidY(self.deviceLabel.frame)-5.5,22,11);
 self.batteryLevel.frame=NSMakeRect(3,3,fmax(1,16*fmin(100,pct)/100.0),5);
 [self refreshBindings];
}
-(void)refreshBindings{
 if(self.page!=SETTINGS_PAGE_BINDINGS)return;
 for(TOTKKeyCard *c in self.cards){BOOL waiting=c==self.capturing;if(c.waiting!=waiting)c.waiting=waiting;[c setNeedsDisplay:YES];}
 if(!self.padLabels.count)return;
 uint16_t map[CONTROLLER_BUTTONS];uint8_t modes[CONTROLLER_BUTTONS];runtime_controller_map_get(map,modes);int panelButton=runtime_gamepad_settings_button();
 for(TOTKPadLabel *l in self.padLabels){
  if(l.physical<0)continue;int action=panelButton==l.physical?SETTINGS_BIND_PANEL:controller_map_target(map[l.physical]);
  NSString *t=action<0?nil:settings_action_name(action);if(t!=l.text&&![t isEqualToString:l.text]){l.text=t;[l setNeedsDisplay:YES];}
 }
 [self layoutPadLabels];
}
-(void)beginCapture:(TOTKKeyCard *)card{
 [self endCapture];[self focusTarget:card];
 self.capturing=card;self.captureUntil=runtime_now_ns()/1e9+10;
 __weak TOTKSettingsPanel *weak=self;
 self.captureMonitor=[NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown|NSEventMaskFlagsChanged|NSEventMaskLeftMouseDown|NSEventMaskRightMouseDown|NSEventMaskOtherMouseDown
  handler:^NSEvent *(NSEvent *e){TOTKSettingsPanel *panel=weak;return panel?[panel captured:e]:e;}];
 [self layoutHints];[self refreshBindings];
}
-(void)endCapture{
 if(self.captureMonitor){[NSEvent removeMonitor:self.captureMonitor];self.captureMonitor=nil;}
 if(self.padCapturing){self.padCapturing.waiting=NO;[self.padCapturing setNeedsDisplay:YES];self.padCapturing=nil;runtime_gamepad_settings_enable(1);[self layoutPadLabels];[self updateBanner];[self layoutHints];}
 if(!self.capturing)return;
 self.capturing=nil;[self layoutHints];[self refreshBindings];
}
/* A on a controller label: the next button pressed (B cancels) takes this button's action, and this button takes that
   one's: the two swap. 10 seconds without one end the wait. */
-(void)beginPadCapture:(TOTKPadLabel *)label{
 [self endCapture];if(label.physical<0)return;[self focusTarget:label];
 self.padCapturing=label;label.waiting=YES;self.captureUntil=runtime_now_ns()/1e9+10;
 runtime_gamepad_settings_enable(0);   /* the button that opens the panel may be the one pressed */
 [label setNeedsDisplay:YES];[self layoutPadLabels];[self updateBanner];[self layoutHints];
}
-(void)bindPad:(int)physical{
 TOTKPadLabel *label=self.padCapturing;if(!label)return;int from=label.physical;
 if(physical!=from&&physical>=0&&physical<=CONTROLLER_SWITCH_BUTTONS){
  uint16_t map[CONTROLLER_BUTTONS];uint8_t modes[CONTROLLER_BUTTONS];runtime_controller_map_get(map,modes);
  uint16_t m=map[from];uint8_t mode=modes[from];map[from]=map[physical];modes[from]=modes[physical];map[physical]=m;modes[physical]=mode;
  runtime_controller_map_set(map,modes);
  int panel=runtime_gamepad_settings_button();if(panel==from)runtime_gamepad_settings_button_set(physical);else if(panel==physical)runtime_gamepad_settings_button_set(from);
  fprintf(stderr,"[totk] settings: controller %s <-> %s\n",controller_physical_names[from],controller_physical_names[physical]);
 }
 [self endCapture];[self refreshBindings];
}
/* A key, modifier or mouse button for the card waiting for one; nil swallows the event. */
-(NSEvent *)captured:(NSEvent *)e{
 if(!self.capturing)return e;
 int input=-1;
 switch(e.type){
  case NSEventTypeKeyDown:
   if(e.modifierFlags&NSEventModifierFlagCommand)return e;   /* Command-Q, Command-W stay the system's */
   if(e.isARepeat)return nil;
   if(e.keyCode==KEYMAP_SETTINGS_KEY||e.keyCode==53){[self endCapture];return nil;}   /* key 1 or Esc: no new key */
   input=e.keyCode;break;
  case NSEventTypeFlagsChanged:if(front_modifier_down(e)!=1)return nil;input=e.keyCode;break;
  case NSEventTypeLeftMouseDown:input=KEYMAP_MOUSE_FIRST;break;
  case NSEventTypeRightMouseDown:input=KEYMAP_MOUSE_FIRST+1;break;
  case NSEventTypeOtherMouseDown:input=e.buttonNumber<KEYMAP_MOUSE_BUTTONS?KEYMAP_MOUSE_FIRST+(int)e.buttonNumber:-1;break;
  default:return e;
 }
 if(input<0||!keymap_bindable(input))return nil;   /* not one the layout takes: keep waiting */
 KeyMap keys;runtime_keymap_get(&keys);TOTKKeyCard *card=self.capturing;
 if(keymap_bind(&keys,card.gameAction,0,input)){runtime_keymap_set(&keys);
  fprintf(stderr,"[totk] settings: %s -> %s\n",keymap_input_name(input),keymap_action_name(card.gameAction));}
 [self endCapture];
 return nil;
}
-(void)clearCard:(TOTKKeyCard *)card{
 [self endCapture];
 KeyMap keys;runtime_keymap_get(&keys);if(keymap_bind(&keys,card.gameAction,0,KEYMAP_NONE))runtime_keymap_set(&keys);
 [self refreshBindings];
}
-(void)controllerPad:(RuntimeGamepadState)pad time:(double)now active:(BOOL)active{
 if(!active||!pad.connected){self.controllerArmed=NO;self.previousButtons=0;self.previousDirection=0;return;}
 int direction=(pad.buttons&(1ull<<13)||pad.ly>16000)?1:(pad.buttons&(1ull<<15)||pad.ly < -16000)?2:
  (pad.buttons&(1ull<<12)||pad.lx < -16000)?3:(pad.buttons&(1ull<<14)||pad.lx>16000)?4:0;
 if(!self.controllerArmed){if(!pad.buttons&&!direction)self.controllerArmed=YES;self.previousButtons=pad.buttons;return;}
 uint64_t edges=pad.buttons&~self.previousButtons;self.previousButtons=pad.buttons;
 int detected=-1,type=runtime_gamepad_type(&detected);if(!type)type=detected;
 const uint64_t confirm=type==GAMEPAD_TYPE_SWITCH?2u:1u,cancel=type==GAMEPAD_TYPE_SWITCH?1u:2u;
 if(self.padCapturing){   /* B cancels; any other button is the new one */
  uint64_t pressed=edges&((1ull<<(CONTROLLER_SWITCH_BUTTONS+1))-1);
  if(pressed&cancel)[self endCapture];else if(pressed)[self bindPad:__builtin_ctzll(pressed)];
  self.previousDirection=direction;return;
 }
 [self input:direction edges:edges confirm:confirm cancel:cancel time:now];
}
/* One step of the panel from a controller or the keyboard: direction 1 up 2 down 3 left 4 right; the edges' buttons (A
   confirm, B cancel, Y 8 the defaults, L 64 / R 128 the tabs). */
-(void)input:(int)direction edges:(uint64_t)edges confirm:(uint64_t)confirm cancel:(uint64_t)cancel time:(double)now{
 if(edges&cancel){if(self.capturing)[self endCapture];else settings_show(0);return;}   /* B: back (out of a wait for a key first) */
 if(self.capturing){self.previousDirection=direction;return;}
 if(edges&(1ull<<6)){[self selectPage:(self.page+SETTINGS_PAGES-1)%SETTINGS_PAGES];return;}   /* L / R: the tabs */
 if(edges&(1ull<<7)){[self selectPage:(self.page+1)%SETTINGS_PAGES];return;}
 if((edges&(1ull<<3))&&self.page==SETTINGS_PAGE_BINDINGS){[self resetDevice];return;}
 BOOL cards=self.page==SETTINGS_PAGE_BINDINGS&&!self.drawings;
 if(direction&&(direction!=self.previousDirection||now>=self.repeatAt)){
  if(direction<=2)[self controllerMove:(direction==1?-1:1)*(cards?2:1)];else [self controllerChange:direction==3?-1:1 select:NO];
  self.repeatAt=now+(direction==self.previousDirection?0.14:0.4);
 }
 self.previousDirection=direction;
 if(edges&confirm)[self controllerChange:1 select:YES];
}
/* The keyboard drives the panel too (the arrows, Return or Space, Q / E the tabs, Delete the defaults; Esc and key 1 close
   it as before); then the hints show the keys. */
-(NSEvent *)keyed:(NSEvent *)e{
 if(self.hidden||e.window!=self.window||(e.modifierFlags&NSEventModifierFlagCommand))return e;
 if(self.padCapturing&&e.keyCode==53){[self endCapture];return nil;}   /* Esc: no new button */
 if(self.capturing||self.padCapturing)return e;
 int direction=0;uint64_t edges=0;
 switch(e.keyCode){
  case 126:direction=1;break;case 125:direction=2;break;case 123:direction=3;break;case 124:direction=4;break;
  case 36:case 76:case 49:edges=1;break;
  case 12:edges=1ull<<6;break;case 14:edges=1ull<<7;break;
  case 51:case 117:edges=1ull<<3;break;
  default:return e;
 }
 self.previousDirection=0;[self input:direction edges:edges confirm:1 cancel:2 time:runtime_now_ns()/1e9];self.previousDirection=0;
 return nil;
}
-(void)close:(id)sender{(void)sender;settings_show(0);}
-(void)center{if(!self.superview)return;NSSize s=self.superview.bounds.size,p=self.frame.size;
 CGFloat fx=g_settings_at.x<0?0.5:g_settings_at.x,fy=g_settings_at.y<0?0.5:g_settings_at.y;
 [self setFrameOrigin:NSMakePoint(floor(fmax(0,s.width-p.width)*fx),floor(fmax(0,s.height-p.height)*fy))];}
/* Dragging the panel's background (not its controls) moves it, kept inside the window. */
-(void)mouseDown:(NSEvent *)e{self.grab=[self convertPoint:e.locationInWindow fromView:nil];}
-(void)mouseDragged:(NSEvent *)e{if(!self.superview)return;
 NSPoint at=[self.superview convertPoint:e.locationInWindow fromView:nil];NSSize s=self.superview.bounds.size,p=self.frame.size;
 CGFloat x=fmin(fmax(0,at.x-self.grab.x),fmax(0,s.width-p.width)),y=fmin(fmax(0,at.y-self.grab.y),fmax(0,s.height-p.height));
 [self setFrameOrigin:NSMakePoint(floor(x),floor(y))];
 g_settings_at=NSMakePoint(s.width>p.width?x/(s.width-p.width):0.5,s.height>p.height?y/(s.height-p.height):0.5);}
-(void)viewDidMoveToSuperview{[super viewDidMoveToSuperview];[self center];}
@end

static TOTKSettingsPanel *settingsPanel;
static void settings_refresh_all(void) { [settingsPanel refresh]; }
static void settings_panel_replace(void) {   /* main thread; made again: what the game offers is known only once its code is in */
 [settingsPanel endCapture];if(settingsPanel.clickMonitor){[NSEvent removeMonitor:settingsPanel.clickMonitor];settingsPanel.clickMonitor=nil;}if(settingsPanel.keyMonitor){[NSEvent removeMonitor:settingsPanel.keyMonitor];settingsPanel.keyMonitor=nil;}[settingsPanel.timer invalidate];[settingsPanel removeFromSuperview];
 settingsPanel=[TOTKSettingsPanel new];[window.contentView addSubview:settingsPanel];
}
static void settings_show(int open) {   /* main thread */
 open=!!open;
 if(!window||open==settingsOpen||(open&&textEntryOpen))return;
 /* Made again at every opening: what the game offers (aspect ratio, resolution) is known only once its code is in. */
 if(open)settings_panel_replace();else [settingsPanel endCapture];
 settingsOpen=open;
 runtime_gamepad_text_capture(open);
 if(open){runtime_input_clear();[settingsPanel center];[settingsPanel refresh];[(TOTKInputView *)window.contentView setCursorAutoHideEnabled:NO];}
 else{[window makeFirstResponder:window.contentView];
  if(window.isKeyWindow&&(window.styleMask&NSWindowStyleMaskFullScreen))[(TOTKInputView *)window.contentView setCursorAutoHideEnabled:YES];}
 settingsPanel.hidden=!open;
 front_mouse_update();
 fprintf(stderr,"[totk] settings panel %s\n",open?"open":"closed");
 if(totk_sr_settings)totk_sr_settings(open);   /* TOTK_STATE_REPORTING: the game gets no input while it is open */
}
/* A single window-lifetime timer owns settings input. It never consumes the text keyboard's edges. */
static NSTimer *settingsControllerTimer;
static BOOL settingsControllerReady;
static void settings_controller_tick(RuntimeGamepadState pad,BOOL requested,BOOL active,double now){
 if(textEntryOpen||!active||!pad.connected){
  settingsControllerReady=NO;
  if(settingsOpen)[settingsPanel controllerPad:pad time:now active:NO];
  return;
 }
 if(!settingsControllerReady){if(!pad.buttons&&!pad.lx&&!pad.ly)settingsControllerReady=YES;return;}
 if(requested){settings_show(!settingsOpen);return;}
 if(settingsOpen)[settingsPanel controllerPad:pad time:now active:YES];
}
static void settings_controller_start(void){
 if(settingsControllerTimer)return;
 runtime_gamepad_settings_enable(1);
 settingsControllerTimer=[NSTimer timerWithTimeInterval:1.0/60 repeats:YES block:^(NSTimer *t){
  (void)t;BOOL requested=runtime_gamepad_settings_requested();
  if(textEntryOpen){settingsControllerReady=NO;return;}
  RuntimeGamepadState pad;runtime_gamepad_text_read(&pad);
  settings_controller_tick(pad,requested,window.isKeyWindow&&NSApp.active,runtime_now_ns()/1e9);
 }];
 [NSRunLoop.mainRunLoop addTimer:settingsControllerTimer forMode:NSRunLoopCommonModes];
}
void runtime_settings_toggle(void){dispatch_async(dispatch_get_main_queue(), ^{settings_show(!settingsOpen);});}
