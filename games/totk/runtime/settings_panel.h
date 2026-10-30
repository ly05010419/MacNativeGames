/* The settings panel (Mac; included by frontend.m): key 1 opens and closes it (runtime/input.c), Esc and its close
   button close it. User 2026-10-25: it replaces the shortcut keys 2-7 and their reminder box. PR #1210: three
   pages (Display, Input, Key bindings) chosen by the tabs at the top, or L1 / R1 on a controller; the version in the
   bottom-left corner instead of the overlay box; no key hints at the bottom. One row per setting, a pop-up menu, a switch
   or a slider, each applied at once:
   Display
     overlay              all / FPS + playtime / off               fps_overlay_set_hud_mode (gfx/nvn/fps_overlay.c)
     FPS graph            switch, default on                 fps-graph.txt above the game's folder (both games)
     frame lock           off / 30 / 40 / 60 / 72 / 90 / 120  nvn_fps_lock_set (gfx/nvn/hle_nvn.c; user 2026-10-25 "锁帧")
                                                             frame lock and frame interpolation exclude each other: picking
                                                             one turns the other off (user 2026-10-25, not to mislead)
     frame interpolation  off / 30+30 / 40+40 / 50+50 /      runtime flag mfx_interp (gfx/nvn/totk_mfx.m)
                          unlimited
     resolution           720p / 1080p / + MetalFX / 2K /    runtime_output_mode_set (game_patches*.c); the player picks,
                          2K + MetalFX (a 4K screen)
                                                             1080p is the start until a choice is saved; the window
                                                             takes that size at once (settings_window_for)
     aspect ratio         16:9 / 16:10 / 21:9 / 32:9         runtime_aspect_set_ratio (aspect_patch.h, game_patches_botw.c);
                                                             the window takes that shape at once, its height kept
                                                             (runtime_front_resize_aspect; user 2026-10-25: no fitting the
                                                             window any more); TOTK_ASPECT's ratio is offered too
     edge smoothing       switch                             taa_set (gfx/nvn/totk_taa.m)
   Input
     controller type      auto / Xbox / PlayStation / Switch runtime_gamepad_type_set (gamepad.m), saved
     DualSense bow trigger switch                            runtime_triggers_choose (gamepad_feedback.m), saved in
                                                             dualsense-triggers.txt; Breath of the Wild 1.9.0 and Tears of
                                                             the Kingdom 1.4.3 only; TOTK_DUALSENSE_TRIGGERS or an agent's
                                                             test session decide first (greyed out)
     mouse                switch                             runtime_mouse_set (gamepad.m), saved
     mouse sensitivity    slider 1-100 (50)                  runtime_mouse_sensitivity_set (gamepad.m), saved in
                                                             mouse-sensitivity.txt; TOTK_MOUSE_SENSITIVITY decides first
     rumble               switch                             runtime_feedback_choose (hid_feedback.c), saved in rumble.txt
     rumble strength      0.5x / 1x / 2x / 3x / 4x           runtime_rumble_gain_set (gamepad_feedback.m), rumble-gain.txt
     gyro aiming          switch                             runtime_feedback_choose, saved in gyro.txt
                                                             TOTK_RUMBLE / TOTK_GYRO or an agent's test session decide the
                                                             switches first, TOTK_RUMBLE_GAIN the strength (the row greyed
                                                             out); a device the run started without comes at the next
                                                             launch (its title says so)
   Key bindings: one row per game action with two keyboard and mouse inputs (keyboard_map.h, runtime_keymap_set: in force
   at once, saved in keyboard.txt) and the controller buttons that drive it (controller_map.h, runtime_controller_map_set:
   controls.txt). Click a box, then press a key or a mouse button (key 1 cancels; Command keys stay the system's) or, in
   the controller column, a controller button; with a controller, pick a row and press A, then the new button. An input
   another action holds swaps places with this box's. Right-click clears a box. Two buttons reset the keyboard and mouse,
   or the controller, to the default (the file removed). A capture ends after 6 seconds without input.
   On the display page, at the right of the bottom line, two buttons (user 2026-10-25): contribute the shaders and pipelines
   the shared list lacks (sent with gh in the background when it is signed in, as the launcher's reminder does; else the
   GitHub form opens and Finder shows the file), and star the project (its GitHub page); their results show for a few
   seconds where the version is.
   A setting this game or start cannot change shows its one value, greyed out. While the panel is open the game gets no
   keyboard, mouse or controller input (TOTKInputView, front_mouse_update and gamepad capture). The controls read their
   values again twice a second, so they always show what is in force. */
#pragma once
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
static void settings_rebuild(void);
static void settings_pointer_used(void);
@interface TOTKSettingRow:NSObject
@property(copy) NSString *title;
@property(copy) NSArray<NSString *> *choices;   /* nil: an on / off switch, or a slider */
@property(copy) NSArray<NSNumber *> *values;    /* what each choice sets */
@property(copy) int (^get)(void);
@property(copy) void (^set)(int value);
@property(copy) NSString *(^firstTitle)(void);   /* a first choice whose text changes (auto: the controller it sees) */
@property int sliderMin, sliderMax;              /* a slider when sliderMax > sliderMin */
@property NSInteger page;                        /* SETTINGS_PAGE_DISPLAY or SETTINGS_PAGE_INPUT */
@property(strong) NSControl *control;
@property(strong) NSTextField *valueLabel;       /* a slider's value */
-(BOOL)slider;
-(void)changed:(id)sender;
-(void)refresh;
@end
@implementation TOTKSettingRow
-(BOOL)slider{return self.sliderMax>self.sliderMin;}
-(void)changed:(id)sender{
 int v=[sender isKindOfClass:NSSlider.class]?((NSSlider *)sender).intValue:[sender isKindOfClass:NSSwitch.class]?((NSSwitch *)sender).state==NSControlStateValueOn:
  self.values[(NSUInteger)MAX(0,((NSPopUpButton *)sender).indexOfSelectedItem)].intValue;
 if(self.set)self.set(v);
 if(self.valueLabel)self.valueLabel.stringValue=[NSString stringWithFormat:@"%d",v];
 [self.control.window makeFirstResponder:self.control.window.contentView];   /* key 1 and Esc keep reaching the window */
 settings_refresh_all();   /* a pick may change another row (frame lock / interpolation) */
}
-(void)refresh{
 int v=self.get?self.get():0;
 if(self.slider){NSSlider *s=(NSSlider *)self.control;if(s.intValue!=v)s.intValue=v;self.valueLabel.stringValue=[NSString stringWithFormat:@"%d",v];return;}
 if(!self.choices){((NSSwitch *)self.control).state=v?NSControlStateValueOn:NSControlStateValueOff;return;}
 NSPopUpButton *p=(NSPopUpButton *)self.control;
 if(self.firstTitle&&p.numberOfItems){NSString *t=self.firstTitle();if(![[p itemAtIndex:0].title isEqualToString:t])[p itemAtIndex:0].title=t;}
 NSUInteger i=[self.values indexOfObject:@(v)];
 if(i!=NSNotFound&&(NSInteger)i!=p.indexOfSelectedItem)[p selectItemAtIndex:(NSInteger)i];
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

static NSArray<TOTKSettingRow *> *settings_rows(void) {
 NSMutableArray<TOTKSettingRow *> *rows=[NSMutableArray array];
 /* Display */
 [rows addObject:settings_row(settings_text("性能信息","Overlay"),
  @[settings_text("全部（FPS、着色器、读取、游玩时间）","All (FPS, shaders, load, playtime)"),settings_text("FPS 与游玩时间","FPS + playtime"),settings_on_off(0)],
  @[@(HUD_MODE_ALL),@(HUD_MODE_MINIMAL),@(HUD_MODE_OFF)],
  ^int{return fps_overlay_hud_mode();},^(int v){fps_overlay_set_hud_mode(v);settings_save_int("graphics-overlay.txt",v,"overlay");[(TOTKHudBox *)hudBox refresh];})];
 [rows addObject:settings_row(settings_text("帧率图","FPS graph"),nil,nil,
  ^int{return fpsGraphEnabled;},^(int v){fpsGraphEnabled=v;runtime_shared_setting_save("fps-graph.txt",v?"on":"off","FPS graph");[(TOTKFpsGraph *)fpsGraphBox setEnabled:v];[(TOTKHudBox *)hudBox refresh];})];
 NSMutableArray<NSString *> *lockChoices=[NSMutableArray array];
 NSMutableArray<NSNumber *> *lockValues=[NSMutableArray array];
 for(unsigned i=0;i<TOTK_FPS_LIMIT_COUNT;i++){
  int fps=totk_fps_limits[i];
  [lockChoices addObject:fps?[NSString stringWithFormat:@(TOTK_UI("%d 帧","%d fps")),fps]:settings_on_off(0)];
  [lockValues addObject:@(fps)];
 }
 if(getenv("TOTK_MFX"))[rows addObject:settings_fixed(settings_text("锁帧","Frame lock"),settings_text("由 TOTK_MFX 设置","Set by TOTK_MFX"))];
 else { [rows addObject:settings_row(settings_text("锁帧","Frame lock"),lockChoices,lockValues,
  ^int{return nvn_fps_lock();},^(int v){nvn_fps_lock_set(v);if(v)dm_gfx_set_flag("mfx_interp",MFX_OFF);settings_save_frame();})]; }
 [rows addObject:settings_row(settings_text("补帧（MetalFX）","Frame interpolation (MetalFX)"),
  @[settings_on_off(0),@"30+30",@"40+40",@"50+50",settings_text("不限帧","Unlimited")],
  @[@(MFX_OFF),@(MFX_30),@(MFX_40),@(MFX_50),@(MFX_MAX)],
  ^int{return mfx_requested();},^(int v){dm_gfx_set_flag("mfx_interp",v);if(v!=MFX_OFF)nvn_fps_lock_set(0);settings_save_frame();})];
 int modes=runtime_output_modes();
 NSMutableArray<NSString *> *res=[NSMutableArray array];NSMutableArray<NSNumber *> *resModes=[NSMutableArray array];
 static const int order[]={OUTPUT_MODE_720P,OUTPUT_MODE_OFF,OUTPUT_MODE_METALFX,OUTPUT_MODE_NATIVE,OUTPUT_MODE_NATIVE_METALFX};
 for(unsigned i=0;i<sizeof order/sizeof *order;i++){
  int m=order[i];if(!(modes&1<<m))continue;unsigned nw=0,nh=0;runtime_output_native(m,&nw,&nh);
  [res addObject:m==OUTPUT_MODE_720P?@"720p":m==OUTPUT_MODE_OFF?@"1080p":m==OUTPUT_MODE_METALFX?settings_text("1080p + MetalFX 放大","1080p + MetalFX upscaling"):
   m==OUTPUT_MODE_NATIVE_METALFX?settings_text("2K + MetalFX 放大（4K 屏）","2K + MetalFX upscaling (4K screens)"):
   nw==2560&&nh==1440?settings_text("2K（2560×1440）","2K (2560×1440)"):
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
 [rows addObject:settings_row(settings_text("边缘平滑（MetalFX 时间性）","Edge smoothing (MetalFX Temporal)"),nil,nil,
  ^int{return taa_enabled();},^(int v){taa_set(v);settings_save_int("graphics-taa.txt",v,"edge smoothing");})];
 NSUInteger display=rows.count;
 /* Input */
 TOTKSettingRow *pad=settings_row(settings_text("手柄类型","Controller type"),@[settings_pad_auto_text(),@"Xbox",@"PlayStation",@"Switch"],
  @[@(GAMEPAD_TYPE_AUTO),@(GAMEPAD_TYPE_XBOX),@(GAMEPAD_TYPE_PLAYSTATION),@(GAMEPAD_TYPE_SWITCH)],
  ^int{return runtime_gamepad_type(NULL);},^(int v){runtime_gamepad_type_set(v);});
 pad.firstTitle=^NSString *{return settings_pad_auto_text();};[rows addObject:pad];
 NSString *bow=settings_text("DualSense 拉弓扳机（R2）","DualSense bow trigger (R2)");
 if(!runtime_triggers_available())[rows addObject:settings_fixed(bow,settings_text("这个版本没有","Not in this version"))];
 else if(runtime_triggers_locked())[rows addObject:settings_fixed(bow,settings_on_off(runtime_triggers_choice()))];
 else [rows addObject:settings_row(bow,nil,nil,^int{return runtime_triggers_choice();},^(int v){runtime_triggers_choose(v);})];
 [rows addObject:settings_row(settings_text("鼠标操作（视角、按键）","Mouse (camera, buttons)"),nil,nil,
  ^int{return runtime_mouse_enabled();},^(int v){runtime_mouse_set(v);})];
 [rows addObject:settings_sensitivity_row()];
 [rows addObject:settings_feedback_row(0)];
 [rows addObject:settings_gain_row()];
 [rows addObject:settings_feedback_row(1)];
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
static NSString *settings_input_text(int input) {
 if(input==KEYMAP_NONE)return @"—";
 switch(input-KEYMAP_MOUSE_FIRST){
  case 0:return settings_text("鼠标左键","Left click");
  case 1:return settings_text("鼠标右键","Right click");
  case 2:return settings_text("滚轮按下","Wheel click");
  case 3:return settings_text("鼠标侧键 4","Mouse 4");
  case 4:return settings_text("鼠标侧键 5","Mouse 5");
 }
 const char *label=keymap_input_label(input);
 return label?@(label):[NSString stringWithFormat:@"#%d",input];
}
/* The controller family the button names follow: the type picked, else the one auto sees, else Xbox. */
static int settings_pad_family(void) { int detected=-1,type=runtime_gamepad_type(&detected);if(!type)type=detected;return type>GAMEPAD_TYPE_AUTO?type:GAMEPAD_TYPE_XBOX; }
static NSString *settings_pad_button(int physical, int family) {
 static const char *const xbox[]={"A","B","X","Y","LS","RS","LB","RB","LT","RT","Menu","View"};
 static const char *const playstation[]={"×","○","□","△","L3","R3","L1","R1","L2","R2","Options","Create"};
 static const char *const nintendo[]={"B","A","Y","X","L3","R3","L","R","ZL","ZR","+","−"};   /* by label: east is A */
 static const char *const arrows[]={"←","↑","→","↓"};
 if(physical<12)return @((family==GAMEPAD_TYPE_PLAYSTATION?playstation:family==GAMEPAD_TYPE_SWITCH?nintendo:xbox)[physical]);
 if(physical<16)return [NSString stringWithFormat:@"%@ %@",settings_text("十字键","D-pad"),@(arrows[physical-12])];
 if(physical==16)return settings_text("触控板","Touchpad");
 return settings_input_text(KEYMAP_MOUSE_FIRST+physical-CONTROLLER_MOUSE_FIRST);   /* a mouse line in controls.txt */
}
/* What the controller column shows for an action: its buttons, or the stick a direction belongs to. */
/* The controller's way to the settings panel, as the key bindings show it. */
static NSString *settings_panel_pad_text(int family) {
 int b=runtime_gamepad_settings_button();
 if(b>=0)return settings_pad_button(b,family);
 return [NSString stringWithFormat:@"%@ + %@",settings_pad_button(4,family),settings_pad_button(5,family)];
}
static NSString *settings_pad_text(int action, const uint16_t *map, int family) {
 if(action>=KEYMAP_MOVE_UP){static const char *const arrows[]={"↑","↓","←","→"};int camera=action>=KEYMAP_CAMERA_UP;
  return [NSString stringWithFormat:@"%@ %@",camera?settings_text("右摇杆","Right stick"):settings_text("左摇杆","Left stick"),@(arrows[(action-KEYMAP_MOVE_UP)%4])];}
 NSMutableArray<NSString *> *names=[NSMutableArray array];
 for(int p=0;p<CONTROLLER_BUTTONS;p++)if(controller_map_target(map[p])==action)[names addObject:settings_pad_button(p,family)];
 return names.count?[names componentsJoinedByString:@" / "]:@"—";
}

#include "bindings_diagram.h"   /* the key bindings drawings */

@class TOTKSettingsPanel;
/* One box of the key bindings: slot 0 and 1 the keyboard and mouse inputs, 2 the controller. A right-click clears it. */
@interface TOTKBindingButton:NSButton
@property int gameAction, slot;   /* keyboard_map.h KEYMAP_*; 0-1 keyboard and mouse, 2 controller */
@property(weak) TOTKSettingsPanel *panel;
@end
@interface TOTKFlippedView:NSView
@end
@implementation TOTKFlippedView
-(BOOL)isFlipped{return YES;}
@end

@interface TOTKSettingsPanel:NSView
@property(strong) NSArray<TOTKSettingRow *> *allRows;   /* every page's rows */
@property(strong) NSArray<TOTKSettingRow *> *rows;      /* the page shown (none on the key bindings page) */
@property(strong) NSMutableArray<NSView *> *controllerTargets;   /* what a controller moves through on the page shown */
@property NSInteger page;
@property NSInteger controllerIndex;
@property BOOL controllerArmed;
@property uint64_t previousButtons;
@property int previousDirection;
@property double repeatAt;
@property(strong) NSTimer *timer;
@property NSPoint grab;   /* where in the panel the drag started */
@property(strong) NSSegmentedControl *tabs;
@property(strong) NSArray<NSView *> *pages;
@property(strong) NSTextField *versionLabel;   /* the bottom-left corner */
@property(strong) NSArray<NSButton *> *displayButtons;   /* the display page: contribute shaders, star the project */
@property(strong) NSArray<NSButton *> *resetButtons;   /* the key bindings page: keyboard and mouse, controller */
@property(strong) NSMutableArray<TOTKBindingButton *> *bindingButtons;
@property(strong) NSMutableArray<NSView *> *bindingTargets;   /* rows with controller buttons, then the reset buttons */
@property(strong) NSTextField *padHeader;
@property(strong) NSScrollView *bindingsScroll;
@property(strong) TOTKBindingButton *capturing;   /* the box waiting for an input */
@property double captureUntil;
@property(strong) id captureMonitor;
@property BOOL drawings;   /* the key bindings page shows the drawings (made at this size), not the list */
@property CGFloat panelW;
@property(strong) TOTKKeyboardDiagram *keyboardDiagram;
@property(strong) TOTKControllerDiagram *padDiagram;
@property(strong) NSSegmentedControl *deviceTabs;   /* keyboard and mouse, or controller */
@property(strong) NSTextField *drawingsHelp;
@property(strong) NSButton *listToggle;   /* "Show as list" / "Show drawings" */
@property(strong) NSArray<NSNumber *> *padOrder;   /* the controller buttons a controller moves through on the drawing */
@property(strong) id clickMonitor;   /* any click in the panel hides the controller's highlight */
-(void)selectPage:(NSInteger)page;
-(void)controllerPad:(RuntimeGamepadState)pad time:(double)now active:(BOOL)active;
-(void)controllerHighlight;
-(void)refresh;
-(void)refreshBindings;
-(void)beginCapture:(TOTKBindingButton *)box;
-(void)endCapture;
-(void)clearBinding:(TOTKBindingButton *)box;
-(void)assignInput:(int)input action:(int)action;
-(void)assignPad:(int)physical action:(int)action;
-(NSMenu *)menuForInput:(int)input pad:(BOOL)pad;
@end

@implementation TOTKBindingButton
-(void)rightMouseDown:(NSEvent *)e{(void)e;if(self.enabled)[self.panel clearBinding:self];}
@end

/* Where the player dragged the panel (user 2026-10-25: it can be moved), as the fraction of the free room left and below
   it, so a new opening and a resized window keep the spot; -1 = centred. The page shown last opens again. */
static NSPoint g_settings_at = {-1, -1};
static NSInteger g_settings_page = SETTINGS_PAGE_DISPLAY;
/* The key bindings page: the list instead of the drawings (the player's pick, or a window too small for them), and which
   drawing is shown (0 keyboard and mouse, 1 controller). */
static BOOL g_settings_bindings_list;
static NSInteger g_settings_bindings_device;
/* A controller moved through the panel since it opened: only then is its row highlighted (PR #1210: nothing looks
   selected before the player picks it); a mouse click hides the highlight again. */
static BOOL g_settings_controller_active;
enum { SETTINGS_WIDE_W = 960, SETTINGS_WIDE_H = 530 };
static BOOL settings_drawings_fit(void) {
 NSSize room=window?window.contentView.bounds.size:NSMakeSize(CGFLOAT_MAX,CGFLOAT_MAX);
 return room.width>=SETTINGS_WIDE_W+8&&room.height>=SETTINGS_WIDE_H+8;
}
static BOOL settings_drawings_for(NSInteger page) { return page==SETTINGS_PAGE_BINDINGS&&!g_settings_bindings_list&&settings_drawings_fit(); }
static NSString *settings_version_text(void) {
 const char *commit=getenv("TOTK_SOURCE_COMMIT");   /* the commit the app was built from (build_app.py, the launcher) */
 if(commit&&*commit)return [NSString stringWithFormat:@(TOTK_UI("版本 %s · 构建 %s","Ver. %s · build %s")),TOTK_GAME_VERSION,commit];
 return [NSString stringWithFormat:@(TOTK_UI("版本 %s","Ver. %s")),TOTK_GAME_VERSION];
}
@implementation TOTKSettingsPanel
enum { SETTINGS_W = 600, SETTINGS_ROW_H = 34, SETTINGS_GAP = 6, SETTINGS_TOP = 90, SETTINGS_BOTTOM = 34, SETTINGS_POP_W = 260,
       SETTINGS_CELL_W = 118, SETTINGS_CELL_GAP = 6, SETTINGS_BIND_ROW_H = 28, SETTINGS_SECTION_H = 24 };
-(instancetype)init{
 NSArray<TOTKSettingRow *> *rows=settings_rows();
 NSUInteger perPage[2]={0,0};for(TOTKSettingRow *r in rows)perPage[r.page]++;
 /* The rows close up (down to 24 pt, 2 pt apart: a 26 pt menu then meets the next one's edge) when the panel would not fit the window (21:9 or 32:9 on a laptop
    screen), so its title, close button, tabs and first rows stay in view. */
 CGFloat n=MAX(perPage[0],perPage[1]),rowH=SETTINGS_ROW_H,gap=SETTINGS_GAP,room=window?window.contentView.bounds.size.height-8:CGFLOAT_MAX;
 if(n&&SETTINGS_TOP+n*(rowH+gap)-gap+SETTINGS_BOTTOM>room){gap=2;rowH=fmax(24,floor((room-SETTINGS_TOP-SETTINGS_BOTTOM+gap)/n-gap));}
 /* The key bindings drawings need a wider, taller panel (PR #1210); it is made at that size while they are shown. */
 BOOL drawings=settings_drawings_for(g_settings_page);
 CGFloat body=n*(rowH+gap)-gap,h=SETTINGS_TOP+body+SETTINGS_BOTTOM,W=SETTINGS_W;
 if(drawings){W=SETTINGS_WIDE_W;h=fmax(h,SETTINGS_WIDE_H);body=h-SETTINGS_TOP-SETTINGS_BOTTOM;}
 if(!(self=[super initWithFrame:NSMakeRect(0,0,W,h)]))return nil;
 self.drawings=drawings;self.panelW=W;self.bindingButtons=[NSMutableArray array];self.bindingTargets=[NSMutableArray array];
 self.allRows=rows;self.controllerTargets=[NSMutableArray array];self.appearance=[NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
 self.wantsLayer=YES;self.layer.backgroundColor=[NSColor colorWithCalibratedRed:0.10 green:0.16 blue:0.20 alpha:0.9].CGColor;
 self.layer.cornerRadius=12;self.layer.borderWidth=1;self.layer.borderColor=[NSColor colorWithCalibratedWhite:1 alpha:0.12].CGColor;
 self.autoresizingMask=NSViewMinXMargin|NSViewMaxXMargin|NSViewMinYMargin|NSViewMaxYMargin;
 NSTextField *title=[NSTextField labelWithString:settings_text("设置","Settings")];title.font=[NSFont systemFontOfSize:17 weight:NSFontWeightSemibold];
 title.textColor=NSColor.whiteColor;title.frame=NSMakeRect(18,h-38,300,24);[self addSubview:title];
 NSButton *close=[NSButton buttonWithTitle:@"✕" target:self action:@selector(close:)];close.bordered=NO;close.refusesFirstResponder=YES;
 close.font=[NSFont systemFontOfSize:15 weight:NSFontWeightBold];close.contentTintColor=NSColor.whiteColor;close.frame=NSMakeRect(W-40,h-40,28,28);[self addSubview:close];
 NSSegmentedControl *tabs=[NSSegmentedControl segmentedControlWithLabels:@[settings_text("画面","Display"),settings_text("输入","Input"),settings_text("按键设置","Key bindings")]
  trackingMode:NSSegmentSwitchTrackingSelectOne target:self action:@selector(tabChanged:)];
 tabs.segmentDistribution=NSSegmentDistributionFillEqually;tabs.refusesFirstResponder=YES;tabs.frame=NSMakeRect(14,h-80,W-28,26);[self addSubview:tabs];self.tabs=tabs;
 NSMutableArray<NSView *> *pages=[NSMutableArray array];
 for(NSInteger p=0;p<2;p++){
  NSView *page=[[NSView alloc] initWithFrame:NSMakeRect(0,SETTINGS_BOTTOM,W,body)];NSUInteger k=0;
  for(TOTKSettingRow *r in rows){
   if(r.page!=p)continue;
   CGFloat y=body-(k+1)*rowH-k*gap,rw=W-28;k++;
   NSView *band=[[NSView alloc] initWithFrame:NSMakeRect(14,y,rw,rowH)];band.wantsLayer=YES;
   band.layer.backgroundColor=[NSColor colorWithCalibratedWhite:0 alpha:0.28].CGColor;band.layer.cornerRadius=6;[page addSubview:band];
   NSTextField *label=[NSTextField labelWithString:r.title];label.font=[NSFont systemFontOfSize:13 weight:NSFontWeightMedium];
   label.textColor=[NSColor colorWithCalibratedWhite:0.92 alpha:1];label.frame=NSMakeRect(12,floor((rowH-18)/2),rw-SETTINGS_POP_W-24,18);[band addSubview:label];
   if(r.slider){
    NSSlider *s=[NSSlider sliderWithValue:r.get?r.get():r.sliderMin minValue:r.sliderMin maxValue:r.sliderMax target:nil action:nil];
    s.continuous=YES;s.frame=NSMakeRect(rw-SETTINGS_POP_W-8,floor((rowH-20)/2),SETTINGS_POP_W-44,20);r.control=s;
    NSTextField *value=[NSTextField labelWithString:@""];value.font=[NSFont monospacedDigitSystemFontOfSize:13 weight:NSFontWeightMedium];
    value.textColor=label.textColor;value.alignment=NSTextAlignmentRight;value.frame=NSMakeRect(rw-46,floor((rowH-18)/2),34,18);[band addSubview:value];r.valueLabel=value;
   }else if(!r.choices){NSSwitch *s=[NSSwitch new];[s sizeToFit];s.frame=NSMakeRect(rw-12-s.frame.size.width,(rowH-s.frame.size.height)/2,s.frame.size.width,s.frame.size.height);r.control=s;}
   else{NSPopUpButton *pop=[[NSPopUpButton alloc] initWithFrame:NSMakeRect(rw-SETTINGS_POP_W-8,floor((rowH-26)/2),SETTINGS_POP_W,26) pullsDown:NO];[pop addItemsWithTitles:r.choices];
    pop.enabled=r.set!=nil;r.control=pop;}
   r.control.refusesFirstResponder=YES;r.control.target=r;r.control.action=@selector(changed:);[band addSubview:r.control];
  }
  [self addSubview:page];[pages addObject:page];
 }
 NSRect bindingsFrame=NSMakeRect(0,SETTINGS_BOTTOM,W,body);
 NSView *bindings=drawings?[self drawingsPageWithFrame:bindingsFrame]:[self bindingsPageWithFrame:bindingsFrame];[self addSubview:bindings];[pages addObject:bindings];
 self.pages=pages;
 /* The bottom line: the version on the left (PR #1210; the overlay keeps it too, user 2026-10-25); the reset buttons on the right
    of the key bindings page. */
 NSTextField *version=[NSTextField labelWithString:settings_version_text()];version.font=[NSFont monospacedSystemFontOfSize:10.5 weight:NSFontWeightRegular];
 version.textColor=[NSColor colorWithCalibratedWhite:0.6 alpha:1];version.lineBreakMode=NSLineBreakByTruncatingTail;version.frame=NSMakeRect(18,9,230,16);[self addSubview:version];self.versionLabel=version;
 NSButton *resetPad=[NSButton buttonWithTitle:settings_text("恢复默认手柄按键","Reset controller") target:self action:@selector(resetController:)];
 NSButton *resetKeys=[NSButton buttonWithTitle:settings_text("恢复默认键鼠按键","Reset keyboard & mouse") target:self action:@selector(resetKeyboard:)];
 NSButton *toggle=[NSButton buttonWithTitle:drawings?settings_text("以列表显示","Show as list"):settings_text("以图示显示","Show drawings")
  target:self action:@selector(toggleList:)];
 if(!drawings&&!settings_drawings_fit()){toggle.enabled=NO;toggle.toolTip=settings_text("窗口太小，放不下图示","The window is too small for the drawings");}
 for(NSButton *b in @[resetPad,resetKeys,toggle]){b.controlSize=NSControlSizeSmall;b.font=[NSFont systemFontOfSize:11];b.refusesFirstResponder=YES;[b sizeToFit];
  b.frame=NSMakeRect(0,5,b.frame.size.width,24);b.wantsLayer=YES;[self addSubview:b];}
 self.resetButtons=@[resetKeys,resetPad];self.listToggle=toggle;
 /* The display page's bottom right (user 2026-10-25, kept on the first page only): contribute the shaders and pipelines the
    shared list lacks, and star the project. */
 NSButton *contrib=[NSButton buttonWithTitle:settings_text("贡献着色器和管线","Contribute shaders and pipelines") target:self action:@selector(contribute:)];
 NSButton *star=[NSButton buttonWithTitle:settings_text("⭐ 给作者点赞","⭐ Star the project") target:self action:@selector(star:)];
 for(NSButton *b in @[contrib,star]){b.controlSize=NSControlSizeSmall;b.font=[NSFont systemFontOfSize:11];b.refusesFirstResponder=YES;[b sizeToFit];
  b.frame=NSMakeRect(0,5,b.frame.size.width,24);b.wantsLayer=YES;[self addSubview:b];}
 self.displayButtons=@[star,contrib];
 [self selectPage:g_settings_page];[self refresh];
 __weak TOTKSettingsPanel *weak=self;
 self.clickMonitor=[NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskLeftMouseDown|NSEventMaskRightMouseDown handler:^NSEvent *(NSEvent *e){
  TOTKSettingsPanel *panel=weak;
  if(panel&&!panel.hidden&&e.window==panel.window&&NSPointInRect([panel convertPoint:e.locationInWindow fromView:nil],panel.bounds))settings_pointer_used();
  return e;}];
 self.timer=[NSTimer scheduledTimerWithTimeInterval:0.5 repeats:YES block:^(NSTimer *t){(void)t;
  TOTKSettingsPanel *panel=weak;if(!panel||panel.hidden)return;
  if(panel.capturing&&runtime_now_ns()/1e9>panel.captureUntil)[panel endCapture];
  [panel refresh];}];
 return self;
}
/* The key bindings page: a line of help, the column titles, and the actions in a scrolling list. */
-(NSView *)bindingsPageWithFrame:(NSRect)frame{
 NSView *page=[[NSView alloc] initWithFrame:frame];CGFloat w=frame.size.width,h=frame.size.height,bw=w-28;
 CGFloat cx2=bw-6-SETTINGS_CELL_W,cx1=cx2-SETTINGS_CELL_GAP-SETTINGS_CELL_W,cx0=cx1-SETTINGS_CELL_GAP-SETTINGS_CELL_W;
 NSColor *dim=[NSColor colorWithCalibratedWhite:0.7 alpha:1];
 NSTextField *help=[NSTextField wrappingLabelWithString:settings_text(
  "点一个格子，再按键盘按键、鼠标按键或手柄按钮（按 1 取消）；右键点格子清除。已被占用的按键会互换。手柄：选中一行按 A，再按新按钮。",
  "Click a box, then press a key, a mouse button or a controller button (1 cancels); right-click clears it. An input in use swaps places. With a controller: pick a row, press A, then the new button.")];
 help.font=[NSFont systemFontOfSize:11];help.textColor=dim;help.frame=NSMakeRect(18,h-30,w-36,30);[page addSubview:help];
 NSString *titles[4]={settings_text("操作","Action"),settings_text("按键 1","Key 1"),settings_text("按键 2","Key 2"),@""};
 CGFloat xs[4]={24,14+cx0,14+cx1,14+cx2},ws[4]={cx0-16,SETTINGS_CELL_W,SETTINGS_CELL_W,SETTINGS_CELL_W};
 for(int i=0;i<4;i++){NSTextField *t=[NSTextField labelWithString:titles[i]];t.font=[NSFont systemFontOfSize:11 weight:NSFontWeightSemibold];t.textColor=dim;
  t.alignment=i?NSTextAlignmentCenter:NSTextAlignmentLeft;t.lineBreakMode=NSLineBreakByTruncatingTail;t.frame=NSMakeRect(xs[i],h-50,ws[i],16);[page addSubview:t];if(i==3)self.padHeader=t;}
 NSScrollView *scroll=[[NSScrollView alloc] initWithFrame:NSMakeRect(0,0,w,fmax(0,h-54))];
 scroll.hasVerticalScroller=YES;scroll.autohidesScrollers=YES;scroll.drawsBackground=NO;scroll.borderType=NSNoBorder;
 TOTKFlippedView *list=[[TOTKFlippedView alloc] initWithFrame:NSMakeRect(0,0,w,0)];
 int count;const SettingsAction *actions=settings_actions(&count);CGFloat y=0;
 for(int i=0;i<count;i++){
  const SettingsAction *a=&actions[i];
  if(a->action<0){NSTextField *t=[NSTextField labelWithString:@(TOTK_UI(a->zh,a->en))];t.font=[NSFont systemFontOfSize:12 weight:NSFontWeightSemibold];
   t.textColor=[NSColor colorWithCalibratedRed:0.7 green:0.95 blue:0.8 alpha:1];t.frame=NSMakeRect(20,y+5,bw,16);[list addSubview:t];y+=SETTINGS_SECTION_H;continue;}
  NSView *band=[[NSView alloc] initWithFrame:NSMakeRect(14,y,bw,SETTINGS_BIND_ROW_H-2)];band.wantsLayer=YES;
  band.layer.backgroundColor=[NSColor colorWithCalibratedWhite:0 alpha:0.28].CGColor;band.layer.cornerRadius=6;[list addSubview:band];
  NSMutableAttributedString *name=[[NSMutableAttributedString alloc] initWithString:@(TOTK_UI(a->zh,a->en))
   attributes:@{NSFontAttributeName:[NSFont systemFontOfSize:12.5 weight:NSFontWeightMedium],NSForegroundColorAttributeName:[NSColor colorWithCalibratedWhite:0.92 alpha:1]}];
  if(a->tag)[name appendAttributedString:[[NSAttributedString alloc] initWithString:[@"  " stringByAppendingString:@(a->tag)]
   attributes:@{NSFontAttributeName:[NSFont systemFontOfSize:11 weight:NSFontWeightRegular],NSForegroundColorAttributeName:[NSColor colorWithCalibratedWhite:0.6 alpha:1]}]];
  NSTextField *label=[NSTextField labelWithAttributedString:name];label.lineBreakMode=NSLineBreakByTruncatingTail;
  label.frame=NSMakeRect(10,floor((SETTINGS_BIND_ROW_H-2-17)/2),cx0-16,17);[band addSubview:label];
  CGFloat cx[3]={cx0,cx1,cx2};
  for(int slot=0;slot<3;slot++){
   TOTKBindingButton *box=[TOTKBindingButton buttonWithTitle:@"" target:self action:@selector(boxClicked:)];
   box.bordered=NO;box.wantsLayer=YES;box.layer.cornerRadius=5;box.refusesFirstResponder=YES;
   box.frame=NSMakeRect(cx[slot],3,SETTINGS_CELL_W,SETTINGS_BIND_ROW_H-8);
   box.panel=self;box.gameAction=a->action;box.slot=slot;
   box.enabled=a->action==SETTINGS_BIND_PANEL?slot==2:slot<2||a->action<KEYMAP_SWITCH_BUTTONS;
   [band addSubview:box];[self.bindingButtons addObject:box];
  }
  if(a->action<KEYMAP_SWITCH_BUTTONS||a->action==SETTINGS_BIND_PANEL)[self.bindingTargets addObject:band];
  y+=SETTINGS_BIND_ROW_H;
 }
 list.frame=NSMakeRect(0,0,w,y+4);scroll.documentView=list;[page addSubview:scroll];self.bindingsScroll=scroll;
 return page;
}
-(void)tabChanged:(NSSegmentedControl *)tabs{[self selectPage:tabs.selectedSegment];}
-(void)selectPage:(NSInteger)page{
 [self endCapture];
 page=page<0?0:page>=SETTINGS_PAGES?SETTINGS_PAGES-1:page;
 /* the drawings need the panel made at their size: it is made again on the next turn of the run loop (not under this
    method's feet), on that page */
 if(settings_drawings_for(page)!=self.drawings){g_settings_page=page;dispatch_async(dispatch_get_main_queue(),^{settings_rebuild();});return;}
 self.page=page;g_settings_page=page;self.tabs.selectedSegment=page;
 for(NSInteger i=0;i<(NSInteger)self.pages.count;i++)self.pages[(NSUInteger)i].hidden=i!=page;
 NSMutableArray<TOTKSettingRow *> *rows=[NSMutableArray array];
 for(TOTKSettingRow *r in self.allRows)if(r.page==page)[rows addObject:r];
 self.rows=rows;self.controllerIndex=0;
 [self layoutFooter];[self updateTargets];[self refreshBindings];
 [self.bindingsScroll.documentView scrollPoint:NSZeroPoint];   /* the list from its top (moving with a controller scrolls) */
 [self.window makeFirstResponder:self.window.contentView];   /* key 1 and Esc keep reaching the window */
}
/* The bottom right: on the display page contribute and star; on the key bindings page the list / drawings switch and the
   reset of the device shown (both resets with the list). */
-(void)layoutFooter{
 BOOL bindings=self.page==SETTINGS_PAGE_BINDINGS;
 NSArray<NSButton *> *shown=self.page==SETTINGS_PAGE_DISPLAY?self.displayButtons:!bindings?@[]:self.drawings?@[self.resetButtons[g_settings_bindings_device?1:0],self.listToggle]:@[self.resetButtons[1],self.resetButtons[0],self.listToggle];
 for(NSButton *b in [@[self.resetButtons[0],self.resetButtons[1],self.listToggle] arrayByAddingObjectsFromArray:self.displayButtons])b.hidden=![shown containsObject:b];
 CGFloat right=self.panelW-14;
 for(NSButton *b in shown){b.frame=NSMakeRect(right-b.frame.size.width,5,b.frame.size.width,24);right-=b.frame.size.width+6;}
}
/* A result of the display page's buttons, in the bottom-left corner for a few seconds instead of the version. */
-(void)note:(NSString *)text{
 self.versionLabel.stringValue=text;self.versionLabel.frame=NSMakeRect(18,9,self.panelW-36-[self footerButtonsWidth],16);
 __weak TOTKSettingsPanel *weak=self;NSString *shown=text;
 dispatch_after(dispatch_time(DISPATCH_TIME_NOW,(int64_t)(6*NSEC_PER_SEC)),dispatch_get_main_queue(),^{
  TOTKSettingsPanel *panel=weak;if(!panel||![panel.versionLabel.stringValue isEqualToString:shown])return;
  panel.versionLabel.stringValue=settings_version_text();panel.versionLabel.frame=NSMakeRect(18,9,230,16);});
}
-(CGFloat)footerButtonsWidth{CGFloat w=0;for(NSButton *b in self.displayButtons)if(!b.hidden)w+=b.frame.size.width+6;return w;}
/* The shaders and pipelines this game met that the shared list lacks (totk_shader_contrib.h): sent with gh off the main
   thread, the issue then opened; without gh (or when it fails) the GitHub form opens and Finder shows the file to drag in. */
-(void)contribute:(NSButton *)button{
 NSString *path=totk_contrib_path();NSUInteger rows=path?totk_contrib_read(path).count:0;
 if(!rows){[self note:settings_text("现在没有要贡献的着色器和管线：公共清单里都有了","Nothing to contribute now: the shared list has them all")];return;}
 button.enabled=NO;[self note:[NSString stringWithFormat:@(TOTK_UI("正在提交 %lu 条着色器和管线…","Sending %lu shaders and pipelines…")),(unsigned long)rows]];
 __weak TOTKSettingsPanel *weak=self;
 dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY,0),^{
  NSString *gh=gh_path(),*issue=gh?contrib_submit(gh,path,rows):nil;
  dispatch_async(dispatch_get_main_queue(),^{
   button.enabled=YES;
   if(issue){totk_contrib_reminded(path,rows);   /* the launcher's reminder counts from here */
    [weak note:[NSString stringWithFormat:@(TOTK_UI("已提交 %lu 条，谢谢你的贡献！","Sent %lu, thank you for contributing!")),(unsigned long)rows]];
    [NSWorkspace.sharedWorkspace openURL:[NSURL URLWithString:issue]];fprintf(stderr,"[totk] settings: contribution sent, %s\n",issue.UTF8String);return;}
   [weak note:gh?settings_text("自动提交没成功：已打开提交页面，把访达里的文件拖进去","Sending did not work: drag the file Finder shows into the form")
                           :settings_text("已打开提交页面：把访达里的文件拖进去","The form is open: drag the file Finder shows into it")];
   [NSWorkspace.sharedWorkspace openURL:[NSURL URLWithString:@TOTK_CONTRIB_ISSUE_URL]];
   [NSWorkspace.sharedWorkspace activateFileViewerSelectingURLs:@[[NSURL fileURLWithPath:path]]];});});
}
/* The project's GitHub page, where the player stars it (as the launcher's star button). */
-(void)star:(id)sender{(void)sender;[NSWorkspace.sharedWorkspace openURL:[NSURL URLWithString:@TOTK_REPO_URL]];
 [self note:settings_text("谢谢支持！在打开的 GitHub 页面右上角点 Star","Thank you! Click Star at the top right of the GitHub page")];}
/* What a controller moves through: the rows; on the list, its rows and the buttons below; on the drawings, the device
   switch, the controller's buttons (the controller drawing) and the buttons below. */
-(void)updateTargets{
 [self.controllerTargets removeAllObjects];self.padOrder=@[];
 if(self.page!=SETTINGS_PAGE_BINDINGS){for(TOTKSettingRow *r in self.rows)[self.controllerTargets addObject:r.control.superview];
  for(NSButton *b in self.displayButtons.reverseObjectEnumerator)if(!b.hidden)[self.controllerTargets addObject:b];}
 else if(!self.drawings){[self.controllerTargets addObjectsFromArray:self.bindingTargets];
  for(NSButton *b in @[self.resetButtons[0],self.resetButtons[1],self.listToggle])if(!b.hidden&&b.enabled)[self.controllerTargets addObject:b];}
 else{
  [self.controllerTargets addObject:self.deviceTabs];
  if(g_settings_bindings_device){self.padOrder=[self.padDiagram order];for(NSUInteger i=0;i<self.padOrder.count;i++)[self.controllerTargets addObject:self.padDiagram];}
  for(NSButton *b in @[self.resetButtons[0],self.resetButtons[1],self.listToggle])if(!b.hidden)[self.controllerTargets addObject:b];
 }
 if(self.controllerIndex>=(NSInteger)self.controllerTargets.count)self.controllerIndex=0;
 [self controllerHighlight];
}
/* The key bindings drawings: the device switch with a line of help, and the keyboard and mouse or the controller. */
-(NSView *)drawingsPageWithFrame:(NSRect)frame{
 NSView *page=[[NSView alloc] initWithFrame:frame];CGFloat w=frame.size.width,h=frame.size.height;
 NSSegmentedControl *device=[NSSegmentedControl segmentedControlWithLabels:@[settings_text("键盘和鼠标","Keyboard & mouse"),settings_text("手柄","Controller")]
  trackingMode:NSSegmentSwitchTrackingSelectOne target:self action:@selector(deviceChanged:)];
 device.refusesFirstResponder=YES;device.segmentDistribution=NSSegmentDistributionFillEqually;device.frame=NSMakeRect(18,h-28,330,24);
 device.selectedSegment=g_settings_bindings_device;[page addSubview:device];self.deviceTabs=device;
 NSTextField *help=[NSTextField labelWithString:@""];help.font=[NSFont systemFontOfSize:11];help.textColor=[NSColor colorWithCalibratedWhite:0.7 alpha:1];
 help.lineBreakMode=NSLineBreakByTruncatingTail;help.frame=NSMakeRect(362,h-25,w-380,16);[page addSubview:help];self.drawingsHelp=help;
 NSRect area=NSMakeRect(14,0,w-28,h-36);__weak TOTKSettingsPanel *weak=self;
 TOTKKeyboardDiagram *keys=[[TOTKKeyboardDiagram alloc] initWithFrame:area];
 keys.pick=^(int input,NSRect at){settings_pointer_used();[weak popMenuForInput:input pad:NO at:at];};
 TOTKControllerDiagram *pad=[[TOTKControllerDiagram alloc] initWithFrame:area];
 pad.pick=^(int physical,NSRect at){settings_pointer_used();[weak popMenuForInput:physical pad:YES at:at];};
 [page addSubview:keys];[page addSubview:pad];self.keyboardDiagram=keys;self.padDiagram=pad;
 [self showDevice];
 return page;
}
-(void)showDevice{
 BOOL pad=g_settings_bindings_device!=0;
 self.keyboardDiagram.hidden=pad;self.padDiagram.hidden=!pad;self.deviceTabs.selectedSegment=pad;
 self.drawingsHelp.stringValue=pad?settings_text("点图上的按钮，再选它的功能。用手柄：上下选按钮，左右改功能。",
   "Click a button on the drawing, then pick what it does. With a controller: up / down picks a button, left / right changes it.")
  :settings_text("点图上的按键或鼠标按键，再选它的功能。有颜色的按键正在使用。","Click a key or mouse button on the drawing, then pick what it does. Coloured keys are in use.");
}
-(void)deviceChanged:(NSSegmentedControl *)device{
 g_settings_bindings_device=device.selectedSegment;[self showDevice];[self layoutFooter];[self updateTargets];
 [self.window makeFirstResponder:self.window.contentView];
}
-(void)toggleList:(id)sender{(void)sender;g_settings_bindings_list=!g_settings_bindings_list;dispatch_async(dispatch_get_main_queue(),^{settings_rebuild();});}
/* The menu of actions for a key, mouse button or controller button, with the one it drives ticked and "None" last. */
-(NSMenu *)menuForInput:(int)input pad:(BOOL)pad{
 NSMenu *menu=[[NSMenu alloc] initWithTitle:@""];menu.autoenablesItems=NO;
 KeyMap keys;uint16_t map[CONTROLLER_BUTTONS];uint8_t modes[CONTROLLER_BUTTONS];int now;
 if(pad){runtime_controller_map_get(map,modes);now=runtime_gamepad_settings_button()==input?SETTINGS_BIND_PANEL:controller_map_target(map[input]);}else{runtime_keymap_get(&keys);now=keymap_action_of(&keys,input,NULL);}
 NSMenuItem *title=[[NSMenuItem alloc] initWithTitle:pad?settings_pad_button(input,settings_pad_family()):settings_input_text(input) action:nil keyEquivalent:@""];
 title.enabled=NO;[menu addItem:title];
 int count;const SettingsAction *all=settings_actions(&count);
 for(int i=0;i<count;i++){
  const SettingsAction *a=&all[i];
  if(pad&&a->action>=KEYMAP_SWITCH_BUTTONS&&a->action!=SETTINGS_BIND_PANEL)continue;   /* the sticks stay sticks on a controller */
  if(!pad&&a->action==SETTINGS_BIND_PANEL)continue;   /* key 1, fixed */
  if(pad&&input>CONTROLLER_SWITCH_BUTTONS&&a->action==SETTINGS_BIND_PANEL)continue;   /* not a mouse button */
  if(a->action<0){[menu addItem:[NSMenuItem sectionHeaderWithTitle:@(TOTK_UI(a->zh,a->en))]];continue;}
  NSMenuItem *item=[[NSMenuItem alloc] initWithTitle:@(TOTK_UI(a->zh,a->en)) action:@selector(menuPicked:) keyEquivalent:@""];
  item.target=self;item.tag=a->action;item.representedObject=@[@(input),@(pad)];item.state=a->action==now?NSControlStateValueOn:NSControlStateValueOff;[menu addItem:item];
 }
 [menu addItem:[NSMenuItem separatorItem]];
 NSMenuItem *none=[[NSMenuItem alloc] initWithTitle:settings_text("无（不使用）","None (unused)") action:@selector(menuPicked:) keyEquivalent:@""];
 none.target=self;none.tag=-1;none.representedObject=@[@(input),@(pad)];none.state=now<0?NSControlStateValueOn:NSControlStateValueOff;[menu addItem:none];
 return menu;
}
-(void)popMenuForInput:(int)input pad:(BOOL)pad at:(NSRect)at{
 NSView *view=pad?(NSView *)self.padDiagram:(NSView *)self.keyboardDiagram;
 if(pad)self.padDiagram.selected=input;else self.keyboardDiagram.selected=input;
 [view setNeedsDisplay:YES];[view displayIfNeeded];
 [[self menuForInput:input pad:pad] popUpMenuPositioningItem:nil atLocation:NSMakePoint(NSMinX(at),NSMaxY(at)+4) inView:view];   /* returns when closed */
 if(pad)self.padDiagram.selected=-1;else self.keyboardDiagram.selected=-1;
 [view setNeedsDisplay:YES];[self.window makeFirstResponder:self.window.contentView];
}
-(void)menuPicked:(NSMenuItem *)item{
 NSArray<NSNumber *> *what=item.representedObject;
 if(what[1].boolValue)[self assignPad:what[0].intValue action:(int)item.tag];else [self assignInput:what[0].intValue action:(int)item.tag];
}
-(void)assignInput:(int)input action:(int)action{
 KeyMap keys;runtime_keymap_get(&keys);
 if(keymap_assign(&keys,input,action)){runtime_keymap_set(&keys);
  fprintf(stderr,"[totk] settings: %s -> %s\n",keymap_input_name(input),action<0?"nothing":keymap_action_name(action));}
 [self refreshBindings];
}
-(void)assignPad:(int)physical action:(int)action{
 if(action==SETTINGS_BIND_PANEL){if(physical<=CONTROLLER_SWITCH_BUTTONS)runtime_gamepad_settings_button_set(physical);[self refreshBindings];return;}
 if(physical==runtime_gamepad_settings_button())runtime_gamepad_settings_button_set(-1);   /* it drives the game again; the panel goes back to L3 + R3 */
 uint16_t map[CONTROLLER_BUTTONS];uint8_t modes[CONTROLLER_BUTTONS];runtime_controller_map_get(map,modes);
 controller_map_assign(map,modes,physical,action);runtime_controller_map_set(map,modes);
 fprintf(stderr,"[totk] settings: controller %s -> %s\n",controller_physical_names[physical],action<0?"nothing":controller_switch_names[action]);
 [self refreshBindings];
}
-(void)refresh{for(TOTKSettingRow *r in self.allRows)[r refresh];[self refreshBindings];}
-(void)refreshBindings{
 if(self.page!=SETTINGS_PAGE_BINDINGS)return;
 if(self.drawings){[self.keyboardDiagram setNeedsDisplay:YES];[self.padDiagram setNeedsDisplay:YES];return;}
 KeyMap keys;runtime_keymap_get(&keys);uint16_t map[CONTROLLER_BUTTONS];uint8_t modes[CONTROLLER_BUTTONS];runtime_controller_map_get(map,modes);
 int family=settings_pad_family();static const char *const families[GAMEPAD_TYPE_COUNT]={"","Xbox","PlayStation","Switch"};
 NSString *header=[NSString stringWithFormat:@(TOTK_UI("手柄（%s）","Controller (%s)")),families[family]];
 if(![self.padHeader.stringValue isEqualToString:header])self.padHeader.stringValue=header;
 NSMutableParagraphStyle *centered=[NSMutableParagraphStyle new];centered.alignment=NSTextAlignmentCenter;centered.lineBreakMode=NSLineBreakByTruncatingTail;
 for(TOTKBindingButton *box in self.bindingButtons){
  BOOL waiting=box==self.capturing;int action=box.gameAction;
  NSString *text=waiting?(box.slot==2?settings_text("请按手柄按钮…","Press a button…"):settings_text("请按键…","Press a key…")):
   action==SETTINGS_BIND_PANEL?(box.slot==0?@"1":box.slot==1?@"—":settings_panel_pad_text(family)):
   box.slot<2?settings_input_text(keys.slot[action][box.slot]):settings_pad_text(action,map,family);
  NSColor *color=waiting?NSColor.blackColor:box.enabled?[NSColor colorWithCalibratedWhite:0.95 alpha:1]:[NSColor colorWithCalibratedWhite:0.55 alpha:1];
  NSAttributedString *title=[[NSAttributedString alloc] initWithString:text attributes:@{NSFontAttributeName:[NSFont systemFontOfSize:11.5 weight:NSFontWeightMedium],
   NSForegroundColorAttributeName:color,NSParagraphStyleAttributeName:centered}];
  if(![box.attributedTitle isEqualToAttributedString:title])box.attributedTitle=title;
  box.layer.backgroundColor=(waiting?[NSColor colorWithCalibratedRed:0.7 green:0.95 blue:0.8 alpha:1]:box.enabled?[NSColor colorWithCalibratedWhite:1 alpha:0.10]:[NSColor clearColor]).CGColor;
  box.toolTip=box.enabled?settings_text("点击后按新的输入；右键清除","Click, then press the new input; right-click clears"):nil;
 }
}
-(void)boxClicked:(TOTKBindingButton *)box{[self beginCapture:box];}
-(void)beginCapture:(TOTKBindingButton *)box{
 [self endCapture];if(!box.enabled)return;
 self.capturing=box;self.captureUntil=runtime_now_ns()/1e9+6;
 if(box.slot==2)runtime_gamepad_settings_enable(0);   /* the button that opens the panel may be the one being bound */
 if(box.slot<2){
  __weak TOTKSettingsPanel *weak=self;
  self.captureMonitor=[NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown|NSEventMaskFlagsChanged|NSEventMaskLeftMouseDown|NSEventMaskRightMouseDown|NSEventMaskOtherMouseDown
   handler:^NSEvent *(NSEvent *e){TOTKSettingsPanel *panel=weak;return panel?[panel captured:e]:e;}];
 }
 [self refreshBindings];
}
-(void)endCapture{
 if(self.captureMonitor){[NSEvent removeMonitor:self.captureMonitor];self.captureMonitor=nil;}
 if(!self.capturing)return;
 if(self.capturing.slot==2)runtime_gamepad_settings_enable(1);
 self.capturing=nil;[self refreshBindings];
}
/* A key, modifier or mouse button for the box waiting for one; nil swallows the event. */
-(NSEvent *)captured:(NSEvent *)e{
 TOTKBindingButton *box=self.capturing;if(!box||box.slot==2)return e;
 int input=-1;
 switch(e.type){
  case NSEventTypeKeyDown:
   if(e.modifierFlags&NSEventModifierFlagCommand)return e;   /* Command-Q, Command-W stay the system's */
   if(e.isARepeat)return nil;
   if(e.keyCode==KEYMAP_SETTINGS_KEY){[self endCapture];return nil;}
   input=e.keyCode;break;
  case NSEventTypeFlagsChanged:if(front_modifier_down(e)!=1)return nil;input=e.keyCode;break;
  case NSEventTypeLeftMouseDown:input=KEYMAP_MOUSE_FIRST;break;
  case NSEventTypeRightMouseDown:input=KEYMAP_MOUSE_FIRST+1;break;
  case NSEventTypeOtherMouseDown:input=e.buttonNumber<KEYMAP_MOUSE_BUTTONS?KEYMAP_MOUSE_FIRST+(int)e.buttonNumber:-1;break;
  default:return e;
 }
 if(input<0||!keymap_bindable(input))return nil;   /* not one the layout takes: keep waiting */
 [self bindInput:input];
 return nil;
}
-(void)bindInput:(int)input{
 TOTKBindingButton *box=self.capturing;if(!box||box.slot==2)return;
 KeyMap keys;runtime_keymap_get(&keys);
 if(keymap_bind(&keys,box.gameAction,box.slot,input)){runtime_keymap_set(&keys);
  fprintf(stderr,"[totk] settings: %s -> %s\n",keymap_input_name(input),keymap_action_name(box.gameAction));}
 [self endCapture];
}
/* A controller button (physical, controller_map.h) for the box waiting for one. The button another action had moves here,
   and that action takes the button this one had. */
-(void)bindPad:(int)physical{
 TOTKBindingButton *box=self.capturing;if(!box||box.slot!=2||physical<0||physical>=CONTROLLER_SWITCH_BUTTONS+1)return;
 if(box.gameAction==SETTINGS_BIND_PANEL){runtime_gamepad_settings_button_set(physical);[self endCapture];return;}
 if(physical==runtime_gamepad_settings_button())runtime_gamepad_settings_button_set(-1);
 uint16_t map[CONTROLLER_BUTTONS];uint8_t modes[CONTROLLER_BUTTONS];runtime_controller_map_get(map,modes);
 int previous=-1;for(int p=0;p<=CONTROLLER_SWITCH_BUTTONS;p++)if(controller_map_target(map[p])==box.gameAction){previous=p;break;}
 uint16_t displaced=map[physical];
 map[physical]=(uint16_t)(1u<<box.gameAction);modes[physical]=CONTROLLER_HOLD;
 if(previous>=0&&previous!=physical){map[previous]=displaced;modes[previous]=CONTROLLER_HOLD;}
 runtime_controller_map_set(map,modes);
 fprintf(stderr,"[totk] settings: controller %s -> %s\n",controller_physical_names[physical],controller_switch_names[box.gameAction]);
 [self endCapture];
}
-(void)clearBinding:(TOTKBindingButton *)box{
 [self endCapture];
 if(box.gameAction==SETTINGS_BIND_PANEL){if(box.slot==2)runtime_gamepad_settings_button_set(-1);}   /* back to L3 + R3 */
 else if(box.slot<2){KeyMap keys;runtime_keymap_get(&keys);if(keymap_bind(&keys,box.gameAction,box.slot,KEYMAP_NONE))runtime_keymap_set(&keys);}
 else{uint16_t map[CONTROLLER_BUTTONS];uint8_t modes[CONTROLLER_BUTTONS];runtime_controller_map_get(map,modes);
  for(int p=0;p<CONTROLLER_BUTTONS;p++)if(controller_map_target(map[p])==box.gameAction){map[p]=0;modes[p]=CONTROLLER_HOLD;}
  runtime_controller_map_set(map,modes);}
 [self refreshBindings];
}
-(void)resetKeyboard:(id)sender{(void)sender;[self endCapture];runtime_keymap_reset();[self refreshBindings];}
-(void)resetController:(id)sender{(void)sender;[self endCapture];runtime_controller_map_reset();runtime_gamepad_settings_button_set(-1);[self refreshBindings];}
-(BOOL)controllerEnabled:(NSInteger)i{
 if(i<0||i>=(NSInteger)self.controllerTargets.count)return NO;
 if(self.page==SETTINGS_PAGE_BINDINGS)return YES;
 TOTKSettingRow *r=self.rows[i];return r.set!=nil&&r.control.enabled;
}
/* The controller's place: drawn only once a controller has moved in the panel (g_settings_controller_active). On the
   controller drawing it is the picked button (outlined there), not a border. */
-(void)controllerHighlight{
 if(![self controllerEnabled:self.controllerIndex]){
  for(NSUInteger i=0;i<self.controllerTargets.count;i++)if([self controllerEnabled:i]){self.controllerIndex=i;break;}
 }
 int physical=-1;
 for(NSUInteger i=0;i<self.controllerTargets.count;i++){
  NSView *v=self.controllerTargets[i];BOOL here=g_settings_controller_active&&(NSInteger)i==self.controllerIndex;
  if(v==self.padDiagram){if(here)physical=[self padPhysical:i];continue;}
  v.wantsLayer=YES;v.layer.cornerRadius=6;v.layer.borderWidth=here?2:0;
  v.layer.borderColor=[NSColor colorWithCalibratedRed:0.7 green:0.95 blue:0.8 alpha:1].CGColor;
 }
 if(self.padDiagram&&self.padDiagram.selected!=physical){self.padDiagram.selected=physical;[self.padDiagram setNeedsDisplay:YES];}
}
/* The controller button of target i on the controller drawing (its targets follow the device switch), -1 none. */
-(int)padPhysical:(NSUInteger)i{
 if(!self.padOrder.count||i<1||i>self.padOrder.count)return -1;
 return self.padOrder[i-1].intValue;
}
-(void)controllerMove:(int)step{
 NSInteger count=self.controllerTargets.count;
 for(NSInteger n=0;n<count;n++){
  self.controllerIndex=(self.controllerIndex+step+count)%count;
  if([self controllerEnabled:self.controllerIndex])break;
 }
 [self controllerHighlight];
 if(self.page==SETTINGS_PAGE_BINDINGS&&self.controllerIndex>=0&&self.controllerIndex<(NSInteger)self.controllerTargets.count){   /* keep the row in view */
  NSView *v=self.controllerTargets[(NSUInteger)self.controllerIndex];if(![v isKindOfClass:NSButton.class])[v scrollRectToVisible:NSInsetRect(v.bounds,0,-SETTINGS_SECTION_H)];}
}
-(void)controllerChange:(int)step select:(BOOL)select{
 NSInteger i=self.controllerIndex;if(![self controllerEnabled:i])return;
 if(self.page==SETTINGS_PAGE_BINDINGS){   /* A on a row: wait for the new controller button; on a reset button: press it */
  NSView *target=self.controllerTargets[(NSUInteger)i];
  if(target==self.deviceTabs){self.deviceTabs.selectedSegment=!g_settings_bindings_device;[self deviceChanged:self.deviceTabs];return;}   /* the other device */
  int physical=target==self.padDiagram?[self padPhysical:(NSUInteger)i]:-1;
  if(physical>=0){   /* left / right (or A) go through the actions it can drive, then none */
   static const int cycle[]={KEYMAP_A,KEYMAP_B,KEYMAP_X,KEYMAP_Y,KEYMAP_ZL,KEYMAP_ZR,KEYMAP_L,KEYMAP_R,KEYMAP_LSTICK,KEYMAP_RSTICK,KEYMAP_PLUS,KEYMAP_MINUS,
    KEYMAP_DUP,KEYMAP_DDOWN,KEYMAP_DLEFT,KEYMAP_DRIGHT,SETTINGS_BIND_PANEL,-1};
   const int n=(int)(sizeof cycle/sizeof *cycle);
   uint16_t map[CONTROLLER_BUTTONS];uint8_t modes[CONTROLLER_BUTTONS];runtime_controller_map_get(map,modes);
   int now=runtime_gamepad_settings_button()==physical?SETTINGS_BIND_PANEL:controller_map_target(map[physical]),at=n-1;for(int k=0;k<n;k++)if(cycle[k]==now)at=k;
   [self assignPad:physical action:cycle[(at+(select?1:step)+n)%n]];return;
  }
  if(!select)return;
  if([target isKindOfClass:NSButton.class]){[(NSButton *)target performClick:nil];return;}
  for(TOTKBindingButton *box in self.bindingButtons)if(box.superview==target&&box.slot==2){[self beginCapture:box];return;}
  return;
 }
 TOTKSettingRow *r=self.rows[i];
 if(r.slider){if(select)return;NSSlider *s=(NSSlider *)r.control;s.intValue=MAX(r.sliderMin,MIN(r.sliderMax,s.intValue+step*5));}
 else if(r.choices){
  NSPopUpButton *p=(NSPopUpButton *)r.control;NSInteger n=p.numberOfItems;if(n<2)return;
  NSInteger index=p.indexOfSelectedItem;if(index<0)index=0;
  [p selectItemAtIndex:(index+step+n)%n];
 }else{
  NSSwitch *s=(NSSwitch *)r.control;
  s.state=select?(s.state==NSControlStateValueOn?NSControlStateValueOff:NSControlStateValueOn):(step>0?NSControlStateValueOn:NSControlStateValueOff);
 }
 [r changed:r.control];[self controllerHighlight];
}
-(void)controllerPad:(RuntimeGamepadState)pad time:(double)now active:(BOOL)active{
 if(!active||!pad.connected){self.controllerArmed=NO;self.previousButtons=0;self.previousDirection=0;return;}
 int direction=(pad.buttons&(1ull<<13)||pad.ly>16000)?1:(pad.buttons&(1ull<<15)||pad.ly < -16000)?2:
  (pad.buttons&(1ull<<12)||pad.lx < -16000)?3:(pad.buttons&(1ull<<14)||pad.lx>16000)?4:0;
 if(!self.controllerArmed){if(!pad.buttons&&!direction)self.controllerArmed=YES;self.previousButtons=pad.buttons;return;}
 uint64_t edges=pad.buttons&~self.previousButtons;self.previousButtons=pad.buttons;
 if(self.capturing&&self.capturing.slot==2){   /* the next button pressed is the new one */
  uint64_t pressed=edges&((1ull<<(CONTROLLER_SWITCH_BUTTONS+1))-1);
  if(pressed)[self bindPad:__builtin_ctzll(pressed)];
  self.previousDirection=direction;return;
 }
 int detected=-1,type=runtime_gamepad_type(&detected);if(!type)type=detected;
 const uint64_t confirm=type==GAMEPAD_TYPE_SWITCH?2u:1u,cancel=type==GAMEPAD_TYPE_SWITCH?1u:2u;
 if(!g_settings_controller_active&&!(edges&cancel)&&(direction||(edges&confirm))){   /* the first move or A shows where the controller is, nothing more */
  g_settings_controller_active=YES;[self controllerHighlight];
  self.previousDirection=direction;self.repeatAt=now+0.4;return;
 }
 if(edges&(1ull<<6)){[self selectPage:(self.page+SETTINGS_PAGES-1)%SETTINGS_PAGES];return;}   /* L1 / R1: the tabs */
 if(edges&(1ull<<7)){[self selectPage:(self.page+1)%SETTINGS_PAGES];return;}
 if(edges&cancel){settings_show(0);return;}
 if(direction&&(direction!=self.previousDirection||now>=self.repeatAt)){
  if(direction<=2)[self controllerMove:direction==1?-1:1];else [self controllerChange:direction==3?-1:1 select:NO];
  self.repeatAt=now+(direction==self.previousDirection?0.14:0.4);
 }
 self.previousDirection=direction;
 if(edges&confirm)[self controllerChange:1 select:YES];
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
/* A mouse click in the panel: the controller's highlight goes until a controller moves again. */
static void settings_pointer_used(void) { if(!g_settings_controller_active)return;g_settings_controller_active=NO;[settingsPanel controllerHighlight]; }
static void settings_panel_replace(void) {   /* main thread; made again: what the game offers is known only once its code is in */
 [settingsPanel endCapture];if(settingsPanel.clickMonitor){[NSEvent removeMonitor:settingsPanel.clickMonitor];settingsPanel.clickMonitor=nil;}[settingsPanel.timer invalidate];[settingsPanel removeFromSuperview];
 settingsPanel=[TOTKSettingsPanel new];[window.contentView addSubview:settingsPanel];
}
/* Made again while open (the drawings page needs another size, or the list view): on the same page and spot. */
static void settings_rebuild(void) {
 if(!window||!settingsOpen)return;
 settings_panel_replace();[settingsPanel center];[settingsPanel refresh];
}
static void settings_show(int open) {   /* main thread */
 open=!!open;
 if(!window||open==settingsOpen||(open&&textEntryOpen))return;
 /* Made again at every opening: what the game offers (aspect ratio, resolution) is known only once its code is in. */
 if(open){g_settings_controller_active=NO;settings_panel_replace();}else [settingsPanel endCapture];
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
