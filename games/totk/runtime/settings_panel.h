/* The settings panel (Mac; included by frontend.m): key 1 opens and closes it (runtime/input.c), Esc and its close
   button close it. User 2026-10-25: it replaces the shortcut keys 2-7 and their reminder box. One row per setting, a
   pop-up menu or a switch, each applied at once:
     overlay              all / FPS + playtime / off               fps_overlay_set_hud_mode (gfx/nvn/fps_overlay.c)
     frame lock           off / 30 / 40 / 60                 nvn_fps_lock_set (gfx/nvn/hle_nvn.c; user 2026-10-25 "锁帧")
                                                             frame lock and frame interpolation exclude each other: picking
                                                             one turns the other off (user 2026-10-25, not to mislead)
     frame interpolation  off / 30+30 / 40+40 / 50+50 /      runtime flag mfx_interp (gfx/nvn/totk_mfx.m)
                          half the refresh rate / unlimited
     resolution           720p / 1080p / + MetalFX / 2K /    runtime_output_mode_set (game_patches*.c); the player picks,
                          2K + MetalFX (a 4K screen)
                                                             1080p is the start (1.4.3 keeps the last pick); the window
                                                             takes that size at once (settings_window_for)
     aspect ratio         16:9 / 16:10 / 21:9 / 32:9         runtime_aspect_set_ratio (aspect_patch.h, game_patches_botw.c);
                                                             the window takes that shape at once, its height kept
                                                             (runtime_front_resize_aspect; user 2026-10-25: no fitting the
                                                             window any more); TOTK_ASPECT's ratio is offered too
     edge smoothing       switch                             taa_set (gfx/nvn/totk_taa.m)
     controller type      auto / Xbox / PlayStation / Switch runtime_gamepad_type_set (gamepad.m), saved
     DualSense bow trigger switch                            runtime_triggers_choose (gamepad_feedback.m), saved in
                                                             dualsense-triggers.txt; Breath of the Wild 1.9.0 and Tears of
                                                             the Kingdom 1.4.3 only; TOTK_DUALSENSE_TRIGGERS or an agent's
                                                             test session decide first (greyed out)
     mouse                switch                             runtime_mouse_set (gamepad.m), saved
     rumble               switch                             runtime_feedback_choose (hid_feedback.c), saved in rumble.txt
     rumble strength      0.5x / 1x / 2x / 3x / 4x           runtime_rumble_gain_set (gamepad_feedback.m), rumble-gain.txt
     gyro aiming          switch                             runtime_feedback_choose, saved in gyro.txt
                                                             TOTK_RUMBLE / TOTK_GYRO or an agent's test session decide the
                                                             switches first, TOTK_RUMBLE_GAIN the strength (the row greyed
                                                             out); a device the run started without comes at the next
                                                             launch (its title says so)
   Below the rows two buttons (user 2026-10-25): contribute the shaders and pipelines the shared list lacks (sent with gh
   in the background when it is signed in, as the launcher's reminder does; else the GitHub form opens and Finder shows the
   file), and star the project (its GitHub page). A setting this game or start cannot change shows its one value, greyed out. While the panel is open the game gets no
   keyboard, mouse or controller input (TOTKInputView, front_mouse_update and gamepad capture). The
   controls read their values again twice a second, so they always show what is in force. */
#pragma once
#include "aspect_math.h"   /* aspect_near, ASPECT_GAME */
#include "../third_party/d2r_gfx/gfx/nvn/totk_shader_contrib.h"   /* the contribution file, TOTK_REPO_URL */
#include "contrib_submit.h"   /* gh_path, contrib_submit (shared with app/launcher.m) */

void dm_gfx_set_flag(const char *name, int value);
void nvn_fps_lock_set(int fps);   /* gfx/nvn/hle_nvn.c: 0 off, 30, 40 or 60 */
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
  if(sscanf(word,"lock-%d %c",&n,&tail)==1&&(n==30||n==40||n==60)){dm_gfx_set_flag("mfx_interp",MFX_OFF);nvn_fps_lock_set(n);}
  /* MFX_HALF is the last entry in the enum, after MFX_MAX, so it is named apart from the 30 / 40 / 50 range. */
  else if(sscanf(word,"interp-%d %c",&n,&tail)==1&&((n>=MFX_30&&n<=MFX_MAX)||n==MFX_HALF)){nvn_fps_lock_set(0);dm_gfx_set_flag("mfx_interp",n);}
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
 if(settingsPendingOutput>=0&&runtime_output_modes()){
  int mode=settingsPendingOutput;settingsPendingOutput=-1;
  if((runtime_output_modes()&(1<<mode))&&runtime_output_mode_set(mode))settings_window_for(mode);
 }
 if(settingsPendingOutput<0&&settingsPendingAspect<=0){[settingsRestoreTimer invalidate];settingsRestoreTimer=nil;}
}
static void settings_restore_start(void) {
 settings_restore_begin();
 if(settingsPendingOutput<0&&settingsPendingAspect<=0)return;
 settingsRestoreTimer=[NSTimer timerWithTimeInterval:0.5 repeats:YES block:^(NSTimer *t){(void)t;settings_restore_ready();}];
 [NSRunLoop.mainRunLoop addTimer:settingsRestoreTimer forMode:NSRunLoopCommonModes];
}

static void settings_refresh_all(void);
@interface TOTKSettingRow:NSObject
@property(copy) NSString *title;
@property(copy) NSArray<NSString *> *choices;   /* nil: an on / off switch */
@property(copy) NSArray<NSNumber *> *values;    /* what each choice sets */
@property(copy) int (^get)(void);
@property(copy) void (^set)(int value);
@property(copy) NSString *(^firstTitle)(void);   /* a first choice whose text changes (auto: the controller it sees) */
@property(strong) NSControl *control;
-(void)changed:(id)sender;
-(void)refresh;
@end
@implementation TOTKSettingRow
-(void)changed:(id)sender{
 int v=[sender isKindOfClass:NSSwitch.class]?((NSSwitch *)sender).state==NSControlStateValueOn:self.values[(NSUInteger)MAX(0,((NSPopUpButton *)sender).indexOfSelectedItem)].intValue;
 if(self.set)self.set(v);
 [self.control.window makeFirstResponder:self.control.window.contentView];   /* key 1 and Esc keep reaching the window */
 settings_refresh_all();   /* a pick may change another row (frame lock / interpolation) */
}
-(void)refresh{
 int v=self.get?self.get():0;
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
 if(runtime_feedback_locked(gyro))return settings_fixed(t,runtime_feedback_present(gyro)?settings_text("开","On"):settings_text("关","Off"));
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
/* Auto's text: the type it takes the connected controller for (runtime/gamepad.m). */
static NSString *settings_pad_auto_text(void) {
 static const char *const names[GAMEPAD_TYPE_COUNT]={NULL,"Xbox","PlayStation","Switch"};
 int detected=-1;runtime_gamepad_type(&detected);
 if(detected>GAMEPAD_TYPE_AUTO&&detected<GAMEPAD_TYPE_COUNT)return [NSString stringWithFormat:@(TOTK_UI("自动（%s）","Auto (%s)")),names[detected]];
 return settings_text("自动（未接手柄）","Auto (no controller)");
}

static NSArray<TOTKSettingRow *> *settings_rows(void) {
 NSMutableArray<TOTKSettingRow *> *rows=[NSMutableArray array];
 [rows addObject:settings_row(settings_text("性能信息","Overlay"),
  @[settings_text("全部（FPS、着色器、读取、游玩时间）","All (FPS, shaders, load, playtime)"),settings_text("FPS 与游玩时间","FPS + playtime"),settings_text("关","Off")],
  @[@(HUD_MODE_ALL),@(HUD_MODE_MINIMAL),@(HUD_MODE_OFF)],
  ^int{return fps_overlay_hud_mode();},^(int v){fps_overlay_set_hud_mode(v);settings_save_int("graphics-overlay.txt",v,"overlay");[(TOTKHudBox *)hudBox refresh];})];
 [rows addObject:settings_row(settings_text("帧率图","FPS graph"),nil,nil,
  ^int{return fpsGraphEnabled;},^(int v){fpsGraphEnabled=v;runtime_setting_save("fps-graph.txt",v?"on":"off","FPS graph");[(TOTKFpsGraph *)fpsGraphBox setEnabled:v];[(TOTKHudBox *)hudBox refresh];})];
 [rows addObject:settings_row(settings_text("锁帧","Frame lock"),
  @[settings_text("关","Off"),settings_text("30 帧","30 fps"),settings_text("40 帧","40 fps"),settings_text("60 帧","60 fps")],@[@0,@30,@40,@60],
  ^int{return nvn_fps_lock();},^(int v){nvn_fps_lock_set(v);if(v)dm_gfx_set_flag("mfx_interp",MFX_OFF);settings_save_frame();})];
 [rows addObject:settings_row(settings_text("补帧（MetalFX）","Frame interpolation (MetalFX)"),
  @[settings_text("关","Off"),@"30+30",@"40+40",@"50+50",settings_text("半刷新率","Half the refresh rate"),settings_text("不限帧","Unlimited")],
  @[@(MFX_OFF),@(MFX_30),@(MFX_40),@(MFX_50),@(MFX_HALF),@(MFX_MAX)],
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
 if(res.count<2)[rows addObject:settings_fixed(settings_text("分辨率","Resolution"),@"1080p")];
 else [rows addObject:settings_row(settings_text("分辨率","Resolution"),res,resModes,
  ^int{return runtime_output_mode();},^(int v){settingsPendingOutput=-1;if(runtime_output_mode_set(v)){settings_window_for(v);settings_save_int("graphics-output.txt",v,"resolution");}})];
 if(!(runtime_aspect_ratio()>0))[rows addObject:settings_fixed(settings_text("画面比例","Aspect ratio"),@"16:9")];
 else{NSMutableArray<NSNumber *> *ratios=[@[@(16.0/9),@(16.0/10),@(21.0/9),@(32.0/9)] mutableCopy];
  float start=runtime_aspect_start();BOOL listed=NO;   /* TOTK_ASPECT's ratio (2.39, 3440x1440 ...) as one more choice */
  for(NSNumber *r in ratios)listed|=aspect_near(start,r.doubleValue);
  if(start>0&&!listed)[ratios addObject:@(start)];
  NSMutableArray<NSString *> *choices=[NSMutableArray array];NSMutableArray<NSNumber *> *values=[NSMutableArray array];
  for(NSUInteger i=0;i<ratios.count;i++){[choices addObject:settings_ratio_text(ratios[i].floatValue)];[values addObject:@((int)i)];}
  [rows addObject:settings_row(settings_text("画面比例","Aspect ratio"),choices,values,^int{
    float now=runtime_aspect_ratio();for(NSUInteger i=0;i<ratios.count;i++)if(aspect_near(now,ratios[i].doubleValue))return (int)i;return -1;},
   ^(int v){float r=ratios[(NSUInteger)v].floatValue;settingsPendingAspect=0;if(runtime_aspect_set_ratio(r)){runtime_front_resize_aspect(r);char word[32];snprintf(word,sizeof word,"%.8g",r);runtime_setting_save("graphics-aspect.txt",word,"aspect ratio");}})];}
 [rows addObject:settings_row(settings_text("边缘平滑（MetalFX 时间性）","Edge smoothing (MetalFX Temporal)"),nil,nil,
  ^int{return taa_enabled();},^(int v){taa_set(v);settings_save_int("graphics-taa.txt",v,"edge smoothing");})];
 TOTKSettingRow *pad=settings_row(settings_text("手柄类型","Controller type"),@[settings_pad_auto_text(),@"Xbox",@"PlayStation",@"Switch"],
  @[@(GAMEPAD_TYPE_AUTO),@(GAMEPAD_TYPE_XBOX),@(GAMEPAD_TYPE_PLAYSTATION),@(GAMEPAD_TYPE_SWITCH)],
  ^int{return runtime_gamepad_type(NULL);},^(int v){runtime_gamepad_type_set(v);});
 pad.firstTitle=^NSString *{return settings_pad_auto_text();};[rows addObject:pad];
 NSString *bow=settings_text("DualSense 拉弓扳机（R2）","DualSense bow trigger (R2)");
 if(!runtime_triggers_available())[rows addObject:settings_fixed(bow,settings_text("这个版本没有","Not in this version"))];
 else if(runtime_triggers_locked())[rows addObject:settings_fixed(bow,runtime_triggers_choice()?settings_text("开","On"):settings_text("关","Off"))];
 else [rows addObject:settings_row(bow,nil,nil,^int{return runtime_triggers_choice();},^(int v){runtime_triggers_choose(v);})];
 [rows addObject:settings_row(settings_text("鼠标操作（视角、按键）","Mouse (camera, buttons)"),nil,nil,
  ^int{return runtime_mouse_enabled();},^(int v){runtime_mouse_set(v);})];
 [rows addObject:settings_feedback_row(0)];
 [rows addObject:settings_gain_row()];
 [rows addObject:settings_feedback_row(1)];
 return rows;
}

@interface TOTKSettingsPanel:NSView
@property(strong) NSArray<TOTKSettingRow *> *rows;
@property(strong) NSMutableArray<NSView *> *controllerTargets;
@property NSInteger controllerIndex;
@property BOOL controllerArmed;
@property uint64_t previousButtons;
@property int previousDirection;
@property double repeatAt;
-(void)controllerPad:(RuntimeGamepadState)pad time:(double)now active:(BOOL)active;
-(void)controllerHighlight;
-(void)controllerHint;
@property(strong) NSTimer *timer;
@property NSPoint grab;   /* where in the panel the drag started */
@property(strong) NSTextField *hint;   /* the bottom line; the buttons' results show there */
-(void)refresh;
@end
/* Where the player dragged the panel (user 2026-10-25: it can be moved), as the fraction of the free room left and below
   it, so a new opening and a resized window keep the spot; -1 = centred. */
static NSPoint g_settings_at = {-1, -1};
@implementation TOTKSettingsPanel
enum { SETTINGS_W = 520, SETTINGS_ROW_H = 34, SETTINGS_GAP = 6, SETTINGS_TOP = 54, SETTINGS_BOTTOM = 86, SETTINGS_POP_W = 260 };
-(instancetype)init{
 NSArray<TOTKSettingRow *> *rows=settings_rows();
 /* The rows close up (down to 24 pt, 2 pt apart: a 26 pt menu then meets the next one's edge) when the panel would not fit the window (21:9 or 32:9 on a laptop
    screen), so its title, close button and first rows stay in view. */
 CGFloat n=rows.count,rowH=SETTINGS_ROW_H,gap=SETTINGS_GAP,room=window?window.contentView.bounds.size.height-8:CGFLOAT_MAX;
 if(n&&SETTINGS_TOP+n*(rowH+gap)-gap+SETTINGS_BOTTOM>room){gap=2;rowH=fmax(24,floor((room-SETTINGS_TOP-SETTINGS_BOTTOM+gap)/n-gap));}
 CGFloat h=SETTINGS_TOP+n*(rowH+gap)-gap+SETTINGS_BOTTOM;
 if(!(self=[super initWithFrame:NSMakeRect(0,0,SETTINGS_W,h)]))return nil;
 self.rows=rows;self.controllerTargets=[NSMutableArray array];self.controllerIndex=0;self.appearance=[NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
 self.wantsLayer=YES;self.layer.backgroundColor=[NSColor colorWithCalibratedRed:0.10 green:0.16 blue:0.20 alpha:0.9].CGColor;
 self.layer.cornerRadius=12;self.layer.borderWidth=1;self.layer.borderColor=[NSColor colorWithCalibratedWhite:1 alpha:0.12].CGColor;
 self.autoresizingMask=NSViewMinXMargin|NSViewMaxXMargin|NSViewMinYMargin|NSViewMaxYMargin;
 NSTextField *title=[NSTextField labelWithString:settings_text("设置","Settings")];title.font=[NSFont systemFontOfSize:17 weight:NSFontWeightSemibold];
 title.textColor=NSColor.whiteColor;title.frame=NSMakeRect(18,h-38,300,24);[self addSubview:title];
 NSButton *close=[NSButton buttonWithTitle:@"✕" target:self action:@selector(close:)];close.bordered=NO;close.refusesFirstResponder=YES;
 close.font=[NSFont systemFontOfSize:15 weight:NSFontWeightBold];close.contentTintColor=NSColor.whiteColor;close.frame=NSMakeRect(SETTINGS_W-40,h-40,28,28);[self addSubview:close];
 for(NSUInteger i=0;i<rows.count;i++){
  TOTKSettingRow *r=rows[i];CGFloat y=h-SETTINGS_TOP-(i+1)*rowH-i*gap,rw=SETTINGS_W-28;
  NSView *band=[[NSView alloc] initWithFrame:NSMakeRect(14,y,rw,rowH)];band.wantsLayer=YES;
  band.layer.backgroundColor=[NSColor colorWithCalibratedWhite:0 alpha:0.28].CGColor;band.layer.cornerRadius=6;[self addSubview:band];
  NSTextField *label=[NSTextField labelWithString:r.title];label.font=[NSFont systemFontOfSize:13 weight:NSFontWeightMedium];
  label.textColor=[NSColor colorWithCalibratedWhite:0.92 alpha:1];label.frame=NSMakeRect(12,floor((rowH-18)/2),rw-SETTINGS_POP_W-24,18);[band addSubview:label];
  if(!r.choices){NSSwitch *s=[NSSwitch new];[s sizeToFit];s.frame=NSMakeRect(rw-12-s.frame.size.width,(rowH-s.frame.size.height)/2,s.frame.size.width,s.frame.size.height);r.control=s;}
  else{NSPopUpButton *p=[[NSPopUpButton alloc] initWithFrame:NSMakeRect(rw-SETTINGS_POP_W-8,floor((rowH-26)/2),SETTINGS_POP_W,26) pullsDown:NO];[p addItemsWithTitles:r.choices];
   p.enabled=r.set!=nil;r.control=p;}
  r.control.refusesFirstResponder=YES;r.control.target=r;r.control.action=@selector(changed:);[band addSubview:r.control];[self.controllerTargets addObject:band];
 }
 CGFloat bw=(SETTINGS_W-28-10)/2;
 NSButton *contrib=[NSButton buttonWithTitle:settings_text("贡献着色器和管线","Contribute shaders and pipelines") target:self action:@selector(contribute:)];
 contrib.frame=NSMakeRect(14,40,bw,30);contrib.refusesFirstResponder=YES;[self addSubview:contrib];[self.controllerTargets addObject:contrib];
 NSButton *star=[NSButton buttonWithTitle:settings_text("⭐ 给作者点赞","⭐ Star the project") target:self action:@selector(star:)];
 star.frame=NSMakeRect(14+bw+10,40,bw,30);star.refusesFirstResponder=YES;[self addSubview:star];[self.controllerTargets addObject:star];
 NSTextField *hint=[NSTextField labelWithString:settings_text("按 1 或 Esc 关闭 · 打开时键盘和鼠标不会传给游戏","1 or Esc closes it · keyboard and mouse do not reach the game while it is open")];
 hint.font=[NSFont systemFontOfSize:11];hint.textColor=[NSColor colorWithCalibratedWhite:0.7 alpha:1];hint.alignment=NSTextAlignmentCenter;
 hint.lineBreakMode=NSLineBreakByTruncatingMiddle;hint.frame=NSMakeRect(10,5,SETTINGS_W-20,32);hint.maximumNumberOfLines=2;[self addSubview:hint];self.hint=hint;
 [self refresh];[self controllerHint];[self controllerHighlight];
 __weak TOTKSettingsPanel *weak=self;self.timer=[NSTimer scheduledTimerWithTimeInterval:0.5 repeats:YES block:^(NSTimer *t){(void)t;if(!weak.hidden)[weak refresh];}];
 return self;
}
-(void)refresh{for(TOTKSettingRow *r in self.rows)[r refresh];}
-(BOOL)controllerEnabled:(NSInteger)i{
 if(i<0||i>=(NSInteger)self.controllerTargets.count)return NO;
 if(i<(NSInteger)self.rows.count){TOTKSettingRow *r=self.rows[i];return r.set!=nil&&r.control.enabled;}
 return ((NSButton *)self.controllerTargets[i]).enabled;
}
-(void)controllerHighlight{
 if(![self controllerEnabled:self.controllerIndex]){
  for(NSUInteger i=0;i<self.controllerTargets.count;i++)if([self controllerEnabled:i]){self.controllerIndex=i;break;}
 }
 for(NSUInteger i=0;i<self.controllerTargets.count;i++){
  NSView *v=self.controllerTargets[i];v.wantsLayer=YES;v.layer.cornerRadius=6;
  v.layer.borderWidth=(NSInteger)i==self.controllerIndex?2:0;
  v.layer.borderColor=[NSColor colorWithCalibratedRed:0.7 green:0.95 blue:0.8 alpha:1].CGColor;
 }
}
-(void)controllerHint{
 int detected=-1,type=runtime_gamepad_type(&detected);if(!type)type=detected;
 BOOL ps=type==GAMEPAD_TYPE_PLAYSTATION,sw=type==GAMEPAD_TYPE_SWITCH;
 NSString *select=ps?@"×":@"A",*cancel=ps?@"○":@"B";
 NSString *chord=ps?@"L2 + R2 + L3":sw?@"ZL + ZR + L stick":@"LT + RT + LS";
 self.hint.stringValue=[NSString stringWithFormat:settings_text("↑↓ 选择 · ←→ 调整 · %@ 确定 · %@ 关闭\n按住 %@ 1 秒：打开 / 关闭 · 1 / Esc", "↑↓ Navigate · ←→ Change · %@ Select · %@ Close\nHold %@ for 1 s: open / close · 1 / Esc"),select,cancel,chord];
}
-(void)controllerMove:(int)step{
 NSInteger count=self.controllerTargets.count;
 for(NSInteger n=0;n<count;n++){
  self.controllerIndex=(self.controllerIndex+step+count)%count;
  if([self controllerEnabled:self.controllerIndex])break;
 }
 [self controllerHighlight];[self controllerHint];
}
-(void)controllerChange:(int)step select:(BOOL)select{
 NSInteger i=self.controllerIndex;if(![self controllerEnabled:i])return;
 if(i>=(NSInteger)self.rows.count){if(select)[(NSButton *)self.controllerTargets[i] performClick:nil];return;}
 TOTKSettingRow *r=self.rows[i];
 if(r.choices){
  NSPopUpButton *p=(NSPopUpButton *)r.control;NSInteger n=p.numberOfItems;if(n<2)return;
  NSInteger index=p.indexOfSelectedItem;if(index<0)index=0;
  [p selectItemAtIndex:(index+step+n)%n];
 }else{
  NSSwitch *s=(NSSwitch *)r.control;
  s.state=select?(s.state==NSControlStateValueOn?NSControlStateValueOff:NSControlStateValueOn):(step>0?NSControlStateValueOn:NSControlStateValueOff);
 }
 [r changed:r.control];[self controllerHighlight];[self controllerHint];
}
-(void)controllerPad:(RuntimeGamepadState)pad time:(double)now active:(BOOL)active{
 if(!active||!pad.connected){self.controllerArmed=NO;self.previousButtons=0;self.previousDirection=0;return;}
 int direction=(pad.buttons&(1ull<<13)||pad.ly>16000)?1:(pad.buttons&(1ull<<15)||pad.ly < -16000)?2:
  (pad.buttons&(1ull<<12)||pad.lx < -16000)?3:(pad.buttons&(1ull<<14)||pad.lx>16000)?4:0;
 if(!self.controllerArmed){if(!pad.buttons&&!direction)self.controllerArmed=YES;self.previousButtons=pad.buttons;return;}
 uint64_t edges=pad.buttons&~self.previousButtons;self.previousButtons=pad.buttons;
 int detected=-1,type=runtime_gamepad_type(&detected);if(!type)type=detected;
 if(edges&(type==GAMEPAD_TYPE_SWITCH?1u:2u)){settings_show(0);return;}
 if(direction&&(direction!=self.previousDirection||now>=self.repeatAt)){
  if(direction<=2)[self controllerMove:direction==1?-1:1];else [self controllerChange:direction==3?-1:1 select:NO];
  self.repeatAt=now+(direction==self.previousDirection?0.14:0.4);
 }
 self.previousDirection=direction;
 if(edges&(type==GAMEPAD_TYPE_SWITCH?2u:1u))[self controllerChange:1 select:YES];
}
-(void)close:(id)sender{(void)sender;settings_show(0);}
/* The shaders and pipelines this game met that the shared list lacks (totk_shader_contrib.h): sent with gh off the main
   thread, the issue then opened; without gh (or when it fails) the GitHub form opens and Finder shows the file to drag in. */
-(void)contribute:(NSButton *)button{
 NSString *path=totk_contrib_path();NSUInteger rows=path?totk_contrib_read(path).count:0;
 if(!rows){self.hint.stringValue=settings_text("现在没有要贡献的着色器和管线：公共清单里都有了","Nothing to contribute now: the shared list has them all");return;}
 button.enabled=NO;self.hint.stringValue=[NSString stringWithFormat:@(TOTK_UI("正在提交 %lu 条着色器和管线…","Sending %lu shaders and pipelines…")),(unsigned long)rows];
 __weak TOTKSettingsPanel *weak=self;
 dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY,0),^{
  NSString *gh=gh_path(),*issue=gh?contrib_submit(gh,path,rows):nil;
  dispatch_async(dispatch_get_main_queue(),^{
   button.enabled=YES;
   if(issue){totk_contrib_reminded(path,rows);   /* the launcher's reminder counts from here */
    weak.hint.stringValue=[NSString stringWithFormat:@(TOTK_UI("已提交 %lu 条，谢谢你的贡献！","Sent %lu, thank you for contributing!")),(unsigned long)rows];
    [NSWorkspace.sharedWorkspace openURL:[NSURL URLWithString:issue]];fprintf(stderr,"[totk] settings: contribution sent, %s\n",issue.UTF8String);return;}
   weak.hint.stringValue=gh?settings_text("自动提交没成功：已打开提交页面，把访达里的文件拖进去","Sending did not work: drag the file Finder shows into the form")
                           :settings_text("已打开提交页面：把访达里的文件拖进去","The form is open: drag the file Finder shows into it");
   [NSWorkspace.sharedWorkspace openURL:[NSURL URLWithString:@TOTK_CONTRIB_ISSUE_URL]];
   [NSWorkspace.sharedWorkspace activateFileViewerSelectingURLs:@[[NSURL fileURLWithPath:path]]];});});
}
/* The project's GitHub page, where the player stars it (as the launcher's star button). */
-(void)star:(id)sender{(void)sender;[NSWorkspace.sharedWorkspace openURL:[NSURL URLWithString:@TOTK_REPO_URL]];
 self.hint.stringValue=settings_text("谢谢支持！在打开的 GitHub 页面右上角点 Star","Thank you! Click Star at the top right of the GitHub page");}
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
static void settings_show(int open) {   /* main thread */
 open=!!open;
 if(!window||open==settingsOpen||(open&&textEntryOpen))return;
 /* Made again at every opening: what the game offers (aspect ratio, resolution) is known only once its code is in. */
 if(open){[settingsPanel.timer invalidate];[settingsPanel removeFromSuperview];settingsPanel=[TOTKSettingsPanel new];[window.contentView addSubview:settingsPanel];}
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
