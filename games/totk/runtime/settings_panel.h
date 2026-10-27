/* The settings panel (Mac; included by frontend.m): key 1 opens and closes it (runtime/input.c), Esc and its close
   button close it. User 2026-10-25: it replaces the shortcut keys 2-7 and their reminder box. One row per setting, a
   pop-up menu or a switch, each applied at once:
     overlay              all / FPS only / off               fps_overlay_set_hud_mode (gfx/nvn/fps_overlay.c)
     frame lock           off / 30 / 40 / 60                 nvn_fps_lock_set (gfx/nvn/hle_nvn.c; user 2026-10-25 "锁帧")
                                                             frame lock and frame interpolation exclude each other: picking
                                                             one turns the other off (user 2026-10-25, not to mislead)
     frame interpolation  off / 30+30 / 40+40 / 50+50 /      runtime flag mfx_interp (gfx/nvn/totk_mfx.m)
                          unlimited
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

/* Menu language is independent of the game's language. English is the default; an explicit host override wins. */
static int settingsLanguage=-1; /* 0 English, 1 French, 2 Chinese */
static void settings_language_load(void) {
 if(settingsLanguage>=0)return;
 char saved[24]={0};const char *override=getenv("TOTK_UI_LANG");
 const char *word=override&&*override?override:(runtime_setting_word("menu-language.txt",saved,sizeof saved)?saved:"en");
 settingsLanguage=!strncmp(word,"fr",2)?1:!strncmp(word,"zh",2)?2:0;
}
static NSString *settings_tr(NSString *en,NSString *fr,NSString *zh){settings_language_load();return settingsLanguage==1?fr:settingsLanguage==2?zh:en;}
static NSString *settings_text(const char *zh, const char *en) {
 settings_language_load();if(settingsLanguage==2)return @(zh);if(settingsLanguage==0)return @(en);
 static NSDictionary<NSString *,NSString *> *fr;static dispatch_once_t once;
 dispatch_once(&once,^{fr=@{
  @"Overlay":@"Informations à l’écran", @"All (FPS, shaders, load)":@"Tout afficher", @"FPS only":@"FPS uniquement", @"Off":@"Désactivé", @"On":@"Activé",
  @"FPS graph":@"Graphique des FPS", @"Frame lock":@"Limite d’images", @"30 fps":@"30 i/s", @"40 fps":@"40 i/s", @"60 fps":@"60 i/s",
  @"Frame interpolation (MetalFX)":@"Génération d’images (MetalFX)", @"Unlimited":@"Illimitée", @"Resolution":@"Résolution",
  @"1080p + MetalFX upscaling":@"1080p + mise à l’échelle MetalFX", @"2K + MetalFX upscaling (4K screens)":@"2K + MetalFX (écran 4K)", @"2K (2560×1440)":@"2K (2560×1440)",
  @"Aspect ratio":@"Format de l’image", @"Edge smoothing (MetalFX Temporal)":@"Lissage (MetalFX temporel)", @"Controller type":@"Type de manette",
  @"Auto (no controller)":@"Auto (aucune manette)", @"DualSense bow trigger (R2)":@"Gâchette d’arc DualSense (R2)", @"Not in this version":@"Indisponible pour cette version",
  @"Mouse (camera, buttons)":@"Souris (caméra et boutons)", @"Rumble":@"Vibrations", @"Rumble strength":@"Intensité des vibrations", @"Gyro aiming":@"Visée gyroscopique",
  @" (from the next launch)":@" (au prochain lancement)", @" (default)":@" (par défaut)", @"Contribute shaders and pipelines":@"Partager mes shaders", @"⭐ Star the project":@"Soutenir le projet"
 };});
 NSString *key=@(en);return fr[key]?:key;
}
/* A non-interactive reminder appears only after the first visible game frame, not during shader warm-up. */
@interface TOTKSettingsShortcutHint : NSView
@end
@implementation TOTKSettingsShortcutHint
-(NSView *)hitTest:(NSPoint)point { (void)point; return nil; }
@end

static NSString *settings_shortcut_controller(int chosen, int detected) {
 if(detected<0)return nil;
 int type=chosen==GAMEPAD_TYPE_AUTO?detected:chosen;
 if(type==GAMEPAD_TYPE_PLAYSTATION)return @"L2 + R2 + L3";
 if(type==GAMEPAD_TYPE_SWITCH)return @"ZL + ZR + L3";
 return @"LT + RT + L3";
}
static TOTKSettingsShortcutHint *settings_shortcut_view(NSView *parent, int chosen, int detected) {
 if(!parent)return nil;
 CGFloat width=fmin(360, fmax(0,parent.bounds.size.width-24)), height=64;
 if(width<=0 || parent.bounds.size.height<height+24)return nil;
 CGFloat top=NSMaxY(parent.bounds)-12;
 TOTKSettingsShortcutHint *hint=[[TOTKSettingsShortcutHint alloc] initWithFrame:NSMakeRect(NSMinX(parent.bounds)+12,top-height,width,height)];
 hint.autoresizingMask=NSViewMaxXMargin|NSViewMinYMargin;hint.wantsLayer=YES;
 hint.layer.backgroundColor=[NSColor colorWithCalibratedRed:0.035 green:0.075 blue:0.09 alpha:0.96].CGColor;
 hint.layer.cornerRadius=10;hint.layer.borderWidth=1;hint.layer.borderColor=[NSColor colorWithCalibratedWhite:1 alpha:0.16].CGColor;
 NSTextField *title=[NSTextField labelWithString:settings_tr(@"Open game menu",@"Ouvrir le menu du jeu",@"打开游戏菜单")];
 title.frame=NSMakeRect(14,38,width-28,18);title.font=[NSFont systemFontOfSize:13 weight:NSFontWeightSemibold];
 title.textColor=NSColor.whiteColor;title.lineBreakMode=NSLineBreakByTruncatingTail;
 NSString *controller=settings_shortcut_controller(chosen,detected);
 NSString *shortcut=controller?[NSString stringWithFormat:settings_tr(@"1  ·  Hold %@ for 1 second",@"1  ·  Maintenir %@ pendant 1 s",@"1  ·  按住 %@ 1 秒"),controller]
                              :settings_tr(@"Press 1 on the keyboard",@"Appuyer sur 1 au clavier",@"按键盘上的 1");
 NSTextField *detail=[NSTextField labelWithString:shortcut];detail.frame=NSMakeRect(14,14,width-28,18);
 detail.font=[NSFont systemFontOfSize:12 weight:NSFontWeightMedium];detail.textColor=[NSColor colorWithCalibratedWhite:0.82 alpha:1];
 detail.lineBreakMode=NSLineBreakByTruncatingTail;[hint addSubview:title];[hint addSubview:detail];
 [parent addSubview:hint];return hint;
}
static void settings_shortcut_show(void) {
 int detected=-1,chosen=runtime_gamepad_type(&detected);
 TOTKSettingsShortcutHint *hint=settings_shortcut_view(window.contentView,chosen,detected);
 if(!hint)return;
 __weak TOTKSettingsShortcutHint *weakHint=hint;
 dispatch_after(dispatch_time(DISPATCH_TIME_NOW,6*NSEC_PER_SEC),dispatch_get_main_queue(),^{[weakHint removeFromSuperview];});
}

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
 if(detected>GAMEPAD_TYPE_AUTO&&detected<GAMEPAD_TYPE_COUNT)return [NSString stringWithFormat:settings_tr(@"Auto (%s)",@"Auto (%s)",@"自动（%s）"),names[detected]];
 return settings_text("自动（未接手柄）","Auto (no controller)");
}

static NSArray<TOTKSettingRow *> *settings_rows(void) {
 NSMutableArray<TOTKSettingRow *> *rows=[NSMutableArray array];
 [rows addObject:settings_row(settings_text("性能信息","Overlay"),
  @[settings_text("全部（FPS、着色器、读取）","All (FPS, shaders, load)"),settings_text("只显示 FPS","FPS only"),settings_text("关","Off")],
  @[@(HUD_MODE_ALL),@(HUD_MODE_MINIMAL),@(HUD_MODE_OFF)],
  ^int{return fps_overlay_hud_mode();},^(int v){fps_overlay_set_hud_mode(v);[(TOTKHudBox *)hudBox refresh];})];
 [rows addObject:settings_row(settings_text("帧率图","FPS graph"),nil,nil,
  ^int{return fpsGraphEnabled;},^(int v){fpsGraphEnabled=v;runtime_setting_save("fps-graph.txt",v?"on":"off","FPS graph");[(TOTKFpsGraph *)fpsGraphBox setEnabled:v];[(TOTKHudBox *)hudBox refresh];})];
 [rows addObject:settings_row(settings_text("锁帧","Frame lock"),
  @[settings_text("关","Off"),settings_text("30 帧","30 fps"),settings_text("40 帧","40 fps"),settings_text("60 帧","60 fps")],@[@0,@30,@40,@60],
  ^int{return nvn_fps_lock();},^(int v){nvn_fps_lock_set(v);if(v)dm_gfx_set_flag("mfx_interp",MFX_OFF);})];
 [rows addObject:settings_row(settings_text("补帧（MetalFX）","Frame interpolation (MetalFX)"),
  @[settings_text("关","Off"),@"30+30",@"40+40",@"50+50",settings_text("不限帧","Unlimited")],
  @[@(MFX_OFF),@(MFX_30),@(MFX_40),@(MFX_50),@(MFX_MAX)],
  ^int{return mfx_requested();},^(int v){dm_gfx_set_flag("mfx_interp",v);if(v!=MFX_OFF)nvn_fps_lock_set(0);})];
 int modes=runtime_output_modes();
 NSMutableArray<NSString *> *res=[NSMutableArray array];NSMutableArray<NSNumber *> *resModes=[NSMutableArray array];
 static const int order[]={OUTPUT_MODE_720P,OUTPUT_MODE_OFF,OUTPUT_MODE_METALFX,OUTPUT_MODE_NATIVE,OUTPUT_MODE_NATIVE_METALFX};
 for(unsigned i=0;i<sizeof order/sizeof *order;i++){
  int m=order[i];if(!(modes&1<<m))continue;unsigned nw=0,nh=0;runtime_output_native(m,&nw,&nh);
  [res addObject:m==OUTPUT_MODE_720P?@"720p":m==OUTPUT_MODE_OFF?@"1080p":m==OUTPUT_MODE_METALFX?settings_text("1080p + MetalFX 放大","1080p + MetalFX upscaling"):
   m==OUTPUT_MODE_NATIVE_METALFX?settings_text("2K + MetalFX 放大（4K 屏）","2K + MetalFX upscaling (4K screens)"):
   nw==2560&&nh==1440?settings_text("2K（2560×1440）","2K (2560×1440)"):
   [NSString stringWithFormat:settings_tr(@"Native %u×%u",@"Natif %u×%u",@"原生 %u×%u"),nw,nh]];
  [resModes addObject:@(m)];}
 if(res.count<2)[rows addObject:settings_fixed(settings_text("分辨率","Resolution"),@"1080p")];
 else [rows addObject:settings_row(settings_text("分辨率","Resolution"),res,resModes,
  ^int{return runtime_output_mode();},^(int v){if(runtime_output_mode_set(v))settings_window_for(v);})];
 if(!(runtime_aspect_ratio()>0))[rows addObject:settings_fixed(settings_text("画面比例","Aspect ratio"),@"16:9")];
 else{NSMutableArray<NSNumber *> *ratios=[@[@(16.0/9),@(16.0/10),@(21.0/9),@(32.0/9)] mutableCopy];
  float start=runtime_aspect_start();BOOL listed=NO;   /* TOTK_ASPECT's ratio (2.39, 3440x1440 ...) as one more choice */
  for(NSNumber *r in ratios)listed|=aspect_near(start,r.doubleValue);
  if(start>0&&!listed)[ratios addObject:@(start)];
  NSMutableArray<NSString *> *choices=[NSMutableArray array];NSMutableArray<NSNumber *> *values=[NSMutableArray array];
  for(NSUInteger i=0;i<ratios.count;i++){[choices addObject:settings_ratio_text(ratios[i].floatValue)];[values addObject:@((int)i)];}
  [rows addObject:settings_row(settings_text("画面比例","Aspect ratio"),choices,values,^int{
    float now=runtime_aspect_ratio();for(NSUInteger i=0;i<ratios.count;i++)if(aspect_near(now,ratios[i].doubleValue))return (int)i;return -1;},
   ^(int v){float r=ratios[(NSUInteger)v].floatValue;if(runtime_aspect_set_ratio(r))runtime_front_resize_aspect(r);})];}
 [rows addObject:settings_row(settings_text("边缘平滑（MetalFX 时间性）","Edge smoothing (MetalFX Temporal)"),nil,nil,
  ^int{return taa_enabled();},^(int v){taa_set(v);})];
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
@property(strong) NSView *projectOverlay;
@property NSInteger tab;
@property BOOL quitArmed;
@property NSInteger controllerIndex;
@property BOOL controllerArmed;
@property uint64_t previousButtons;
@property int previousDirection;
@property double repeatAt;
-(void)controllerPad:(RuntimeGamepadState)pad time:(double)now active:(BOOL)active;
-(void)controllerHighlight;
-(void)controllerHint;
-(void)selectTab:(NSInteger)tab;
-(void)rebuild;
@property(strong) NSTimer *timer;
@property NSPoint grab;   /* where in the panel the drag started */
@property(strong) NSTextField *hint;   /* the bottom line; the buttons' results show there */
-(void)refresh;
@end
/* Where the player dragged the panel (user 2026-10-25: it can be moved), as the fraction of the free room left and below
   it, so a new opening and a resized window keep the spot; -1 = centred. */
static NSPoint g_settings_at = {-1, -1};
@implementation TOTKSettingsPanel
enum { SETTINGS_W = 760, SETTINGS_H = 568, SETTINGS_POP_W = 300 };
static NSArray<NSNumber *> *settings_tabs(void){ return @[@0,@1,@4,@5]; }
static NSString *settings_tab_name(NSInteger tab){
 switch(tab){
 case 0:return settings_tr(@"Graphics",@"Graphismes",@"画面");
 case 1:return settings_tr(@"Controls",@"Commandes",@"控制");
 case 4:return settings_tr(@"Saves",@"Sauvegardes",@"存档");
 default:return settings_tr(@"Settings",@"Paramètres",@"设置");
 }
}
static NSString *settings_tab_detail(NSInteger tab){
 switch(tab){
 case 0:return settings_tr(@"Picture, performance and display",@"Image, performances et affichage",@"画面、性能和显示");
 case 1:return settings_tr(@"Controller, vibration and aiming",@"Manette, vibrations et visée",@"手柄、震动和瞄准");
 case 4:return settings_tr(@"Open this profile’s save folder in Finder.",@"Ouvre le dossier des sauvegardes de ce profil dans le Finder.",@"在访达中打开当前用户的存档文件夹。");
 default:return settings_tr(@"Menu preferences and game controls",@"Préférences du menu et gestion du jeu",@"菜单偏好与游戏管理");
 }
}
-(instancetype)init{
 settings_language_load();
 CGFloat scale=window?fmin(1,fmin((window.contentView.bounds.size.width-16)/SETTINGS_W,(window.contentView.bounds.size.height-16)/SETTINGS_H)):1;
 scale=fmax(0.25,scale);
 if(!(self=[super initWithFrame:NSMakeRect(0,0,SETTINGS_W*scale,SETTINGS_H*scale)]))return nil;
 self.bounds=NSMakeRect(0,0,SETTINGS_W,SETTINGS_H);
 self.autoresizingMask=NSViewMinXMargin|NSViewMaxXMargin|NSViewMinYMargin|NSViewMaxYMargin;
 self.appearance=[NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];self.wantsLayer=YES;
 self.layer.backgroundColor=[NSColor colorWithCalibratedRed:0.035 green:0.075 blue:0.09 alpha:0.98].CGColor;
 self.layer.cornerRadius=16;self.layer.borderWidth=1;self.layer.borderColor=[NSColor colorWithWhite:1 alpha:0.14].CGColor;
 self.tab=0;[self rebuild];
 __weak TOTKSettingsPanel *weak=self;self.timer=[NSTimer scheduledTimerWithTimeInterval:0.5 repeats:YES block:^(NSTimer *t){(void)t;if(!weak.hidden)[weak refresh];}];
 return self;
}
-(NSTextField *)menuLabel:(NSString *)text frame:(NSRect)frame size:(CGFloat)size color:(NSColor *)color{
 NSTextField *label=[NSTextField labelWithString:text];label.frame=frame;label.font=[NSFont systemFontOfSize:size weight:NSFontWeightMedium];
 label.textColor=color;label.lineBreakMode=NSLineBreakByTruncatingTail;[self addSubview:label];return label;
}
-(void)selectTab:(NSInteger)tab{
 if(![settings_tabs() containsObject:@(tab)])return;
 self.tab=tab;self.quitArmed=NO;self.controllerIndex=0;[self rebuild];
}
-(void)tabClicked:(NSButton *)sender{[self selectTab:sender.tag];}
-(void)rebuild{
 for(NSView *view in self.subviews.copy)[view removeFromSuperview];
 self.controllerTargets=[NSMutableArray array];
 NSColor *text=[NSColor colorWithCalibratedWhite:0.96 alpha:1],*muted=[NSColor colorWithCalibratedRed:0.69 green:0.77 blue:0.79 alpha:1];
 [self menuLabel:settings_tr(@"Game menu",@"Menu du jeu",@"游戏菜单") frame:NSMakeRect(26,518,400,30) size:22 color:text];
 NSButton *close=[NSButton buttonWithTitle:@"×" target:self action:@selector(close:)];close.bordered=NO;close.font=[NSFont systemFontOfSize:22 weight:NSFontWeightMedium];
 close.frame=NSMakeRect(718,518,26,28);close.refusesFirstResponder=YES;[self addSubview:close];
 NSArray<NSNumber *> *tabs=settings_tabs();CGFloat gap=5,available=SETTINGS_W-52,tw=(available-gap*(tabs.count-1))/tabs.count;
 for(NSUInteger i=0;i<tabs.count;i++){
  NSInteger n=tabs[i].integerValue;NSButton *b=[NSButton buttonWithTitle:settings_tab_name(n) target:self action:@selector(tabClicked:)];b.tag=n;
  b.frame=NSMakeRect(26+i*(tw+gap),469,tw,36);b.bordered=NO;b.refusesFirstResponder=YES;b.wantsLayer=YES;b.layer.cornerRadius=8;
  b.layer.backgroundColor=(n==self.tab?[NSColor colorWithCalibratedRed:0.16 green:0.47 blue:0.45 alpha:1]:[NSColor colorWithWhite:1 alpha:0.07]).CGColor;
  b.font=[NSFont systemFontOfSize:12 weight:n==self.tab?NSFontWeightSemibold:NSFontWeightMedium];b.contentTintColor=text;[self addSubview:b];
 }
 [self menuLabel:settings_tab_name(self.tab) frame:NSMakeRect(28,431,300,25) size:16 color:text];
 NSTextField *detail=[self menuLabel:settings_tab_detail(self.tab) frame:NSMakeRect(28,404,704,22) size:12 color:muted];
 detail.maximumNumberOfLines=1;
 NSArray<TOTKSettingRow *> *all=settings_rows();
 if(self.tab==0)self.rows=[all subarrayWithRange:NSMakeRange(0,7)];
 else if(self.tab==1)self.rows=[all subarrayWithRange:NSMakeRange(7,all.count-7)];
 else if(self.tab==5){
  __weak TOTKSettingsPanel *weak=self;
  TOTKSettingRow *language=settings_row(settings_tr(@"Menu language",@"Langue du menu",@"菜单语言"),@[@"English",@"Français",@"中文"],@[@0,@1,@2],
   ^int{return settingsLanguage;},^(int v){settingsLanguage=v;runtime_setting_save("menu-language.txt",v==1?"fr":v==2?"zh":"en","menu language");[weak rebuild];});
  self.rows=@[language];
 }
 else self.rows=@[];
 for(NSUInteger i=0;i<self.rows.count;i++){
  TOTKSettingRow *r=self.rows[i];CGFloat y=354-i*42,rw=SETTINGS_W-52;
  NSView *band=[[NSView alloc] initWithFrame:NSMakeRect(26,y,rw,37)];band.wantsLayer=YES;
  band.layer.backgroundColor=[NSColor colorWithCalibratedWhite:1 alpha:0.055].CGColor;band.layer.cornerRadius=8;[self addSubview:band];
  NSTextField *label=[NSTextField labelWithString:r.title];label.font=[NSFont systemFontOfSize:13 weight:NSFontWeightMedium];label.textColor=text;
  label.frame=NSMakeRect(14,10,rw-SETTINGS_POP_W-28,19);label.lineBreakMode=NSLineBreakByTruncatingTail;[band addSubview:label];
  if(!r.choices){NSSwitch *s=[NSSwitch new];[s sizeToFit];s.frame=NSMakeRect(rw-14-s.frame.size.width,(37-s.frame.size.height)/2,s.frame.size.width,s.frame.size.height);r.control=s;}
  else{NSPopUpButton *p=[[NSPopUpButton alloc] initWithFrame:NSMakeRect(rw-SETTINGS_POP_W-10,5,SETTINGS_POP_W,27) pullsDown:NO];[p addItemsWithTitles:r.choices];p.enabled=r.set!=nil;r.control=p;}
  r.control.refusesFirstResponder=YES;r.control.target=r;r.control.action=@selector(changed:);[band addSubview:r.control];[self.controllerTargets addObject:band];
 }
 if(self.tab==4){
  NSButton *open=[NSButton buttonWithTitle:settings_tr(@"Open save folder",@"Ouvrir le dossier des sauvegardes",@"打开存档文件夹") target:self action:@selector(openSaves:)];
  open.frame=NSMakeRect(26,345,SETTINGS_W-52,48);open.bordered=NO;open.refusesFirstResponder=YES;open.wantsLayer=YES;open.layer.cornerRadius=8;
  open.layer.backgroundColor=[NSColor colorWithCalibratedRed:0.16 green:0.36 blue:0.36 alpha:1].CGColor;
  open.font=[NSFont systemFontOfSize:14 weight:NSFontWeightSemibold];open.contentTintColor=text;
  open.image=[NSImage imageWithSystemSymbolName:@"folder" accessibilityDescription:nil];open.imagePosition=NSImageLeft;
  [self addSubview:open];[self.controllerTargets addObject:open];
 }
 /* The project actions sit in a separate card anchored to the game window, visible with every tab. */
 [self.projectOverlay removeFromSuperview];self.projectOverlay=nil;
 if(self.superview){
  NSView *parent=self.superview;CGFloat width=fmin(320,fmax(0,parent.bounds.size.width-24)),scale=width/320;
  if(scale>0){
   NSView *card=[[NSView alloc] initWithFrame:NSMakeRect(NSMaxX(parent.bounds)-width-12,12,width,70*scale)];
   card.bounds=NSMakeRect(0,0,320,70);card.autoresizingMask=NSViewMinXMargin|NSViewMaxYMargin;card.wantsLayer=YES;
   card.layer.backgroundColor=[NSColor colorWithCalibratedRed:0.035 green:0.075 blue:0.09 alpha:0.96].CGColor;
   card.layer.cornerRadius=12;card.layer.borderWidth=1;card.layer.borderColor=[NSColor colorWithCalibratedWhite:1 alpha:0.15].CGColor;
   NSTextField *caption=[NSTextField labelWithString:settings_tr(@"COMMUNITY",@"COMMUNAUTÉ",@"社区")];
   caption.frame=NSMakeRect(13,48,294,15);caption.font=[NSFont systemFontOfSize:10 weight:NSFontWeightSemibold];
   caption.textColor=[NSColor colorWithCalibratedRed:0.68 green:0.77 blue:0.78 alpha:1];[card addSubview:caption];
   NSButton *contrib=[NSButton buttonWithTitle:settings_tr(@"Share shaders",@"Partager les shaders",@"分享着色器") target:self action:@selector(contribute:)];
   contrib.frame=NSMakeRect(9,8,184,34);contrib.bordered=NO;contrib.refusesFirstResponder=YES;contrib.wantsLayer=YES;
   contrib.layer.cornerRadius=8;contrib.layer.backgroundColor=[NSColor colorWithCalibratedRed:0.13 green:0.27 blue:0.29 alpha:1].CGColor;
   contrib.font=[NSFont systemFontOfSize:12 weight:NSFontWeightSemibold];contrib.contentTintColor=text;
   contrib.image=[NSImage imageWithSystemSymbolName:@"arrow.up.doc" accessibilityDescription:nil];contrib.imagePosition=NSImageLeft;
   NSButton *star=[NSButton buttonWithTitle:@"GitHub" target:self action:@selector(star:)];
   star.frame=NSMakeRect(199,8,112,34);star.bordered=NO;star.refusesFirstResponder=YES;star.wantsLayer=YES;
   star.layer.cornerRadius=8;star.layer.backgroundColor=[NSColor colorWithCalibratedRed:0.16 green:0.36 blue:0.34 alpha:1].CGColor;
   star.font=[NSFont systemFontOfSize:12 weight:NSFontWeightSemibold];star.contentTintColor=text;
   star.image=[NSImage imageWithSystemSymbolName:@"star" accessibilityDescription:nil];star.imagePosition=NSImageLeft;
   [card addSubview:contrib];[card addSubview:star];[parent addSubview:card];self.projectOverlay=card;
   [self.controllerTargets addObject:contrib];[self.controllerTargets addObject:star];
  }
 }
 NSButton *quit=[NSButton buttonWithTitle:self.quitArmed?settings_tr(@"Confirm quit",@"Confirmer la fermeture",@"确认退出"):settings_tr(@"Quit game",@"Quitter le jeu",@"退出游戏") target:self action:@selector(quitGame:)];
 quit.frame=NSMakeRect(SETTINGS_W-194,58,168,34);quit.bordered=NO;quit.wantsLayer=YES;quit.layer.cornerRadius=8;
 quit.layer.backgroundColor=[NSColor colorWithCalibratedRed:0.40 green:0.19 blue:0.20 alpha:1].CGColor;
 quit.font=[NSFont systemFontOfSize:13 weight:NSFontWeightSemibold];quit.contentTintColor=[NSColor colorWithCalibratedRed:1 green:0.88 blue:0.86 alpha:1];
 quit.refusesFirstResponder=YES;[self addSubview:quit];[self.controllerTargets addObject:quit];
 self.hint=[self menuLabel:@"" frame:NSMakeRect(26,14,SETTINGS_W-52,34) size:11 color:muted];self.hint.maximumNumberOfLines=2;
 [self refresh];[self controllerHint];[self controllerHighlight];
}
-(void)openSaves:(id)sender{
 (void)sender;
 const char *path=runtime_fs_save_root();
 NSString *folder=path&&*path?[NSString stringWithUTF8String:path]:nil;
 if(!folder||![NSWorkspace.sharedWorkspace openURL:[NSURL fileURLWithPath:folder isDirectory:YES]]){
  self.hint.stringValue=settings_tr(@"Could not open the save folder.",@"Impossible d’ouvrir le dossier des sauvegardes.",@"无法打开存档文件夹。");
 }
}
-(void)quitGame:(id)sender{
 if(!self.quitArmed){self.quitArmed=YES;[(NSButton *)sender setTitle:settings_tr(@"Confirm quit",@"Confirmer la fermeture",@"确认退出")];
  self.hint.stringValue=settings_tr(@"Unsaved progress will be lost. Select again to quit.",@"La progression non sauvegardée sera perdue. Sélectionne à nouveau pour quitter.",@"未保存的进度将丢失。再次选择以退出。");return;}
 [window performClose:nil];
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
 NSString *triggers=ps?@"L2 / R2":sw?@"ZL / ZR":@"LT / RT";
 self.hint.stringValue=[NSString stringWithFormat:settings_tr(@"%@ Change tab   ·   ↑↓ Navigate   ·   ←→ Adjust   ·   %@ Select   ·   %@ Close",@"%@ Onglets   ·   ↑↓ Naviguer   ·   ←→ Régler   ·   %@ Choisir   ·   %@ Fermer",@"%@ 切换分页   ·   ↑↓ 选择   ·   ←→ 调整   ·   %@ 确定   ·   %@ 关闭"),triggers,select,cancel];
}
-(void)controllerMove:(int)step{
 if(self.quitArmed){self.quitArmed=NO;NSButton *quit=(NSButton *)self.controllerTargets.lastObject;quit.title=settings_tr(@"Quit game",@"Quitter le jeu",@"退出游戏");}
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
 [r changed:r.control];[self controllerHighlight];if(!self.quitArmed)[self controllerHint];
}
-(void)controllerPad:(RuntimeGamepadState)pad time:(double)now active:(BOOL)active{
 if(!active||!pad.connected){self.controllerArmed=NO;self.previousButtons=0;self.previousDirection=0;return;}
 int direction=(pad.buttons&(1ull<<13)||pad.ly>16000)?1:(pad.buttons&(1ull<<15)||pad.ly < -16000)?2:
  (pad.buttons&(1ull<<12)||pad.lx < -16000)?3:(pad.buttons&(1ull<<14)||pad.lx>16000)?4:0;
 if(!self.controllerArmed){if(!pad.buttons&&!direction)self.controllerArmed=YES;self.previousButtons=pad.buttons;return;}
 uint64_t edges=pad.buttons&~self.previousButtons;self.previousButtons=pad.buttons;
 int detected=-1,type=runtime_gamepad_type(&detected);if(!type)type=detected;
 if(edges&(type==GAMEPAD_TYPE_SWITCH?1u:2u)){settings_show(0);return;}
 if(edges&((1ull<<8)|(1ull<<9))){
  NSArray<NSNumber *> *tabs=settings_tabs();NSInteger index=[tabs indexOfObject:@(self.tab)];
  NSInteger delta=(edges&(1ull<<9))?1:-1;[self selectTab:tabs[(index+delta+(NSInteger)tabs.count)%(NSInteger)tabs.count].integerValue];return;
 }
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
 if(!rows){self.hint.stringValue=settings_tr(@"Nothing to contribute now: the shared list has them all",@"Aucun shader à partager pour le moment.",@"目前没有需要贡献的着色器和管线。");return;}
 button.enabled=NO;self.hint.stringValue=[NSString stringWithFormat:settings_tr(@"Sending %lu shaders and pipelines…",@"Envoi de %lu shaders et pipelines…",@"正在提交 %lu 条着色器和管线…"),(unsigned long)rows];
 __weak TOTKSettingsPanel *weak=self;
 dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY,0),^{
  NSString *gh=gh_path(),*issue=gh?contrib_submit(gh,path,rows):nil;
  dispatch_async(dispatch_get_main_queue(),^{
   button.enabled=YES;
   if(issue){totk_contrib_reminded(path,rows);   /* the launcher's reminder counts from here */
    weak.hint.stringValue=[NSString stringWithFormat:settings_tr(@"Sent %lu, thank you for contributing!",@"%lu éléments envoyés, merci pour ta contribution !",@"已提交 %lu 条，谢谢你的贡献！"),(unsigned long)rows];
    [NSWorkspace.sharedWorkspace openURL:[NSURL URLWithString:issue]];fprintf(stderr,"[totk] settings: contribution sent, %s\n",issue.UTF8String);return;}
   weak.hint.stringValue=gh?settings_tr(@"Sending did not work: drag the file Finder shows into the form",@"L’envoi a échoué : glisse le fichier affiché dans le formulaire.",@"自动提交没成功：把访达里的文件拖进表单")
                           :settings_tr(@"The form is open: drag the file Finder shows into it",@"Glisse le fichier affiché dans le formulaire ouvert.",@"已打开提交页面：把访达里的文件拖进去");
   [NSWorkspace.sharedWorkspace openURL:[NSURL URLWithString:@TOTK_CONTRIB_ISSUE_URL]];
   [NSWorkspace.sharedWorkspace activateFileViewerSelectingURLs:@[[NSURL fileURLWithPath:path]]];});});
}
/* The project's GitHub page, where the player stars it (as the launcher's star button). */
-(void)star:(id)sender{(void)sender;[NSWorkspace.sharedWorkspace openURL:[NSURL URLWithString:@TOTK_REPO_URL]];
 self.hint.stringValue=settings_tr(@"Thank you! Click Star at the top right of the GitHub page",@"Merci ! Clique sur Star en haut de la page GitHub.",@"谢谢支持！在打开的 GitHub 页面右上角点 Star");}
-(void)center{if(!self.superview)return;NSSize s=self.superview.bounds.size,p=self.frame.size;
 CGFloat fx=g_settings_at.x<0?0.5:g_settings_at.x,fy=g_settings_at.y<0?0.5:g_settings_at.y;
 [self setFrameOrigin:NSMakePoint(floor(fmax(0,s.width-p.width)*fx),floor(fmax(0,s.height-p.height)*fy))];}
-(void)resizeWithOldSuperviewSize:(NSSize)oldSize{
 [super resizeWithOldSuperviewSize:oldSize];if(!self.superview)return;
 NSSize size=self.superview.bounds.size;CGFloat scale=fmax(0.25,fmin(1,fmin((size.width-16)/SETTINGS_W,(size.height-16)/SETTINGS_H)));
 [self setFrameSize:NSMakeSize(SETTINGS_W*scale,SETTINGS_H*scale)];[self center];
}
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
 if(open){[settingsPanel.projectOverlay removeFromSuperview];[settingsPanel.timer invalidate];[settingsPanel removeFromSuperview];settingsPanel=[TOTKSettingsPanel new];[window.contentView addSubview:settingsPanel];[settingsPanel rebuild];}
 settingsOpen=open;
 runtime_gamepad_text_capture(open);
 if(open){runtime_input_clear();[settingsPanel center];[settingsPanel refresh];[(TOTKInputView *)window.contentView setCursorAutoHideEnabled:NO];}
 else{[settingsPanel.projectOverlay removeFromSuperview];settingsPanel.projectOverlay=nil;[window makeFirstResponder:window.contentView];
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
 /* Stick drift or a held gameplay button must not prevent arming after focus/connection. A fully held chord still
    requires release first, avoiding an accidental opening on return to the game. */
 const uint64_t chord=(1ull<<8)|(1ull<<9)|(1ull<<4);
 if(!settingsControllerReady){if((pad.buttons&chord)!=chord)settingsControllerReady=YES;return;}
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
