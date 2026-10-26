/* The settings panel (Mac; included by frontend.m): key 1 opens and closes it (runtime/input.c), Esc and its close
   button close it. User 2026-10-25: it replaces the shortcut keys 2-7 and their reminder box. User 2026-10-26: some lines
   hold two settings side by side, MetalFX has a section that opens and closes, and the controls are three buttons that
   open their own sheets. Each setting is a pop-up menu or a switch, applied at once:
     overlay              all / FPS only / off               fps_overlay_set_hud_mode (gfx/nvn/fps_overlay.c)
     frame rate limit     off / 30 / 40 / 60 / custom        nvn_fps_lock_set (gfx/nvn/hle_nvn.c; user 2026-10-25 "锁帧",
                                                             2026-10-26 a highest rate the player types in, 20-240)
     | vertical sync      switch                             runtime flag vsync (gfx/nvn/nvn_mtl.m); on while frame
                                                             interpolation runs (greyed out)
     resolution           720p / 900p / 1080p / 1440p        runtime_picture_set (game_patches*.c), up to the swapchain;
                                                             the window takes that size at once (settings_window_size).
                                                             Breath of the Wild keeps main's list (runtime_output_mode_set)
     | window mode        window / full screen / borderless  settings_window_mode (borderless: a window as big as the
                                                             screen, the Dock and menu bar hidden while it is in front)
     aspect ratio         16:9 / 16:10 / 21:9 / 32:9         runtime_aspect_set_ratio (aspect_patch.h, game_patches_botw.c);
                                                             the window takes that shape at once, its height kept
                                                             (runtime_front_resize_aspect; user 2026-10-25: no fitting the
                                                             window any more); TOTK_ASPECT's ratio is offered too
     MetalFX settings     a section, closed at first:
       spatial preset     off / quality ~67% / balanced ~59% runtime_picture_set: the scene below the canvas, MetalFX
                          / performance 50%                  rebuilding the canvas (1.4.3; greyed out elsewhere)
       | anti-aliasing    FXAA / MetalFX TAA                 taa_set (gfx/nvn/totk_taa.m); only with a preset on (user
                                                             2026-10-26), where presets exist
       frame interpolation switch                            runtime flag mfx_auto (hle_nvn.c nvn_fi_auto_set): adaptive,
       | what it does     a line of text                     at the frame rate limit (N + N) or, without one, at half the
                                                             display's refresh rate (user 2026-10-26, as DLSS 4 and
                                                             Cyberpunk 2077 do it)
     controls             controller / keyboard and mouse    sheets: controls_editor.h's controller page (the controller
                          / gameplay                         type, its buttons, action_editor.h's player actions), its
                                                             keyboard page (keys, mouse buttons), and the gameplay sheet
                                                             below (TOTKGameplaySheet):
       DualSense bow trigger switch                          runtime_triggers_choose (gamepad_feedback.m), saved in
                                                             dualsense-triggers.txt; Breath of the Wild 1.9.0 and Tears of
                                                             the Kingdom 1.4.3 only; TOTK_DUALSENSE_TRIGGERS or an agent's
                                                             test session decide first (greyed out)
       mouse              switch                             runtime_mouse_set (gamepad.m), saved
       rumble             switch                             runtime_feedback_choose (hid_feedback.c), saved in rumble.txt
       rumble strength    0.5x / 1x / 2x / 3x / 4x           runtime_rumble_gain_set (gamepad_feedback.m), rumble-gain.txt
       gyro aiming        switch                             runtime_feedback_choose, saved in gyro.txt
                                                             TOTK_RUMBLE / TOTK_GYRO or an agent's test session decide the
                                                             switches first, TOTK_RUMBLE_GAIN the strength (the row greyed
                                                             out); a device the run started without comes at the next
                                                             launch (its title says so)
   Below the rows two buttons (user 2026-10-25): contribute the shaders and pipelines the shared list lacks (sent with gh
   in the background when it is signed in, as the launcher's reminder does; else the GitHub form opens and Finder shows the
   file), and star the project (its GitHub page). A setting this game or start cannot change shows its one value, greyed out. While the panel is open the game gets no
   keyboard or mouse input (TOTKInputView, front_mouse_update: the pointer is let go); a controller still plays. The
   controls read their values again twice a second, so they always show what is in force. */
#pragma once
#include "aspect_math.h"   /* aspect_near, ASPECT_GAME */
#include "../third_party/d2r_gfx/gfx/nvn/totk_shader_contrib.h"   /* the contribution file, TOTK_REPO_URL */
#include "contrib_submit.h"   /* gh_path, contrib_submit (shared with app/launcher.m) */

void dm_gfx_set_flag(const char *name, int value);
void nvn_fps_lock_set(int fps);   /* gfx/nvn/hle_nvn.c: 0 off, else the highest frame rate, 20..240 */
int nvn_fps_lock(void);
int dm_gfx_vsync(void);           /* gfx/nvn/nvn_mtl.m: the vertical sync switch (flag vsync) */
enum { SETTINGS_FPS_MIN = 20, SETTINGS_FPS_MAX = 240, SETTINGS_CUSTOM = -1 };

/* Picking a resolution sizes the window to it at once (user 2026-10-25), in the window's points as the first window
   (frontend.m runtime_front_run), the picture's shape kept (the aspect ratio picked) and shrunk to fit the screen's visible
   area. Full screen and a borderless window stay as they are. */
static void settings_window_size(unsigned w, unsigned h) {
 if(!window||(window.styleMask&NSWindowStyleMaskFullScreen)||!(window.styleMask&NSWindowStyleMaskTitled))return;
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
/* Breath of the Wild's list (OUTPUT_MODE_*): 720p 1280x720, 1080p 1920x1080, MetalFX 2560x1440, native its own size (2K),
   2K + MetalFX 3840x2160. */
static void settings_window_for(int mode) {
 unsigned w=1920,h=1080;
 if(mode==OUTPUT_MODE_METALFX){w=2560;h=1440;}
 else if(mode==OUTPUT_MODE_NATIVE_METALFX){w=3840;h=2160;}
 else if(mode==OUTPUT_MODE_720P){w=1280;h=720;}
 else if(mode==OUTPUT_MODE_NATIVE&&!runtime_output_native(mode,&w,&h)){w=1920;h=1080;}
 settings_window_size(w,h);
}

/* The window mode (user 2026-10-26): a window, full screen in its own Space, or borderless: the window without its title
   bar as big as the screen, the Dock and menu bar hidden while the game is in front (settings_presentation). A pick made
   in full screen waits until the system has left it (settings_window_mode_after_full, frontend.m's window delegate). */
enum { WINDOW_MODE_WINDOW, WINDOW_MODE_FULL, WINDOW_MODE_BORDERLESS };
static BOOL windowBorderless;
static NSRect windowFrameBefore;
static int windowModeAfterFull=-1;
static void settings_presentation(void) {
 if(!window||(window.styleMask&NSWindowStyleMaskFullScreen))return;
 NSApplicationPresentationOptions want=windowBorderless&&window.isMainWindow?NSApplicationPresentationAutoHideDock|NSApplicationPresentationAutoHideMenuBar:NSApplicationPresentationDefault;
 if(NSApp.presentationOptions!=want)NSApp.presentationOptions=want;
}
static void settings_borderless(BOOL on) {
 NSScreen *screen=window.screen?:NSScreen.mainScreen;
 if(on&&!screen)return;
 if(on!=windowBorderless){
  if(on){windowFrameBefore=window.frame;window.styleMask=NSWindowStyleMaskBorderless;}
  else window.styleMask=NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskMiniaturizable|NSWindowStyleMaskResizable;
  windowBorderless=on;fprintf(stderr,"[totk] settings: window %s\n",on?"borderless":"with its title bar");
 }
 if(on&&!NSEqualRects(window.frame,screen.frame))[window setFrame:screen.frame display:YES];
 else if(!on&&!NSIsEmptyRect(windowFrameBefore)){[window setFrame:windowFrameBefore display:YES];windowFrameBefore=NSZeroRect;}
 [window makeFirstResponder:window.contentView];settings_presentation();
}
static int settings_window_mode(void) {
 return window&&(window.styleMask&NSWindowStyleMaskFullScreen)?WINDOW_MODE_FULL:windowBorderless?WINDOW_MODE_BORDERLESS:WINDOW_MODE_WINDOW;
}
static void settings_window_mode_set(int mode) {
 if(!window)return;
 if(window.styleMask&NSWindowStyleMaskFullScreen){if(mode!=WINDOW_MODE_FULL){windowModeAfterFull=mode;[window toggleFullScreen:nil];}return;}
 if(mode==WINDOW_MODE_FULL){settings_borderless(NO);[window toggleFullScreen:nil];return;}
 settings_borderless(mode==WINDOW_MODE_BORDERLESS);
}
static void settings_window_mode_after_full(void) {
 int mode=windowModeAfterFull;windowModeAfterFull=-1;
 if(mode==WINDOW_MODE_BORDERLESS)settings_borderless(YES);else settings_presentation();
}

static void settings_refresh_all(void);
static void settings_rebuild(void);
@interface TOTKSettingRow:NSObject
@property(copy) NSString *title;
@property(copy) NSArray<NSString *> *choices;   /* nil: an on / off switch, or a note or buttons (below) */
@property(copy) NSArray<NSNumber *> *values;    /* what each choice sets */
@property(copy) int (^get)(void);
@property(copy) void (^set)(int value);
@property(copy) NSString *(^firstTitle)(void);   /* a first choice whose text changes (auto: the controller it sees) */
@property(copy) NSString *(^lastTitle)(void);    /* a last choice whose text changes (custom: the rate typed in) */
@property(copy) int (^usable)(void);             /* 0: greyed out for now (anti-aliasing without a MetalFX preset) */
@property(copy) NSString *(^note)(void);         /* a line of text in place of a title and a control */
@property(copy) NSArray<NSString *> *buttons;    /* buttons in place of a control, press gets the one clicked */
@property(copy) void (^press)(int index);
@property BOOL indent;                           /* a line inside an open section */
@property(strong) NSControl *control;
-(void)changed:(id)sender;
-(void)refresh;
@end
@implementation TOTKSettingRow
-(void)changed:(id)sender{
 if(self.press){self.press((int)[sender tag]);return;}
 int v=[sender isKindOfClass:NSSwitch.class]?((NSSwitch *)sender).state==NSControlStateValueOn:self.values[(NSUInteger)MAX(0,((NSPopUpButton *)sender).indexOfSelectedItem)].intValue;
 if(self.set)self.set(v);
 [self.control.window makeFirstResponder:self.control.window.contentView];   /* key 1 and Esc keep reaching the window */
 settings_refresh_all();   /* a pick may change another row (anti-aliasing with the preset, vsync with interpolation) */
}
-(void)refresh{
 if(self.note){NSTextField *t=(NSTextField *)self.control;NSString *n=self.note();if(![t.stringValue isEqualToString:n])t.stringValue=n;return;}
 if(self.press)return;
 BOOL on=self.set&&(!self.usable||self.usable());
 if(self.control.enabled!=on)self.control.enabled=on;
 int v=self.get?self.get():0;
 if(!self.choices){((NSSwitch *)self.control).state=v?NSControlStateValueOn:NSControlStateValueOff;return;}
 NSPopUpButton *p=(NSPopUpButton *)self.control;
 if(self.firstTitle&&p.numberOfItems){NSString *t=self.firstTitle();if(![[p itemAtIndex:0].title isEqualToString:t])[p itemAtIndex:0].title=t;}
 if(self.lastTitle&&p.numberOfItems){NSMenuItem *last=[p itemAtIndex:p.numberOfItems-1];NSString *t=self.lastTitle();if(![last.title isEqualToString:t])last.title=t;}
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
static TOTKSettingRow *settings_note(NSString *(^note)(void)) {
 TOTKSettingRow *r=[TOTKSettingRow new];r.title=@"";r.note=note;return r;
}
static TOTKSettingRow *settings_buttons(NSString *title, NSArray<NSString *> *buttons, void (^press)(int)) {
 TOTKSettingRow *r=[TOTKSettingRow new];r.title=title;r.buttons=buttons;r.press=press;return r;
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

/* The custom frame rate limit (user 2026-10-26): a whole number 20..240; 0 for anything else. */
static int settings_fps_parse(NSString *text) {
 NSScanner *scan=[NSScanner scannerWithString:text?:@""];NSInteger v=0;
 if(![scan scanInteger:&v]||!scan.atEnd||v<SETTINGS_FPS_MIN||v>SETTINGS_FPS_MAX)return 0;
 return (int)v;
}
static void settings_fps_ask(void) {
 if(!window||window.attachedSheet)return;
 NSAlert *alert=[NSAlert new];alert.messageText=settings_text("自定义帧率上限","Custom frame rate limit");
 alert.informativeText=[NSString stringWithFormat:@(TOTK_UI("输入 %d–%d 之间的整数（帧 / 秒）。打开补帧时按这个帧率补帧。","A whole number from %d to %d (frames per second). Frame interpolation doubles it.")),SETTINGS_FPS_MIN,SETTINGS_FPS_MAX];
 NSTextField *field=[[NSTextField alloc] initWithFrame:NSMakeRect(0,0,120,24)];int now=nvn_fps_lock();
 field.stringValue=now?[NSString stringWithFormat:@"%d",now]:@"";field.placeholderString=@"45";alert.accessoryView=field;
 [alert addButtonWithTitle:settings_text("确定","OK")];[alert addButtonWithTitle:settings_text("取消","Cancel")];
 alert.window.initialFirstResponder=field;
 [alert beginSheetModalForWindow:window completionHandler:^(NSModalResponse response){
  if(response==NSAlertFirstButtonReturn){int v=settings_fps_parse(field.stringValue);if(v)nvn_fps_lock_set(v);else NSBeep();}
  settings_refresh_all();}];
}
static int settings_display_hz(void) {
 NSScreen *screen=window.screen?:NSScreen.mainScreen;NSInteger hz=screen?screen.maximumFramesPerSecond:60;
 return hz>0?(int)hz:60;
}
/* What the adaptive frame interpolation does now (hle_nvn.c nvn_fi_auto_set). */
static NSString *settings_fi_note(void) {
 if(mfx_requested()==MFX_OFF)return settings_text("关：只显示游戏自己的帧","Off: the game's own frames only");
 int lock=nvn_fps_lock(),hz=settings_display_hz(),half=MAX(30,hz/2);
 if(lock&&2*lock>hz)return [NSString stringWithFormat:@(TOTK_UI("按帧率上限补：%d + %d = %d 帧（超过屏幕 %d Hz）","At the limit: %d + %d = %d fps (above the %d Hz screen)")),lock,lock,2*lock,hz];
 if(lock)return [NSString stringWithFormat:@(TOTK_UI("按帧率上限补：%d + %d = %d 帧","At the limit: %d + %d = %d fps")),lock,lock,2*lock];
 return [NSString stringWithFormat:@(TOTK_UI("自适应：最高 %d + %d（屏幕 %d Hz）","Adaptive: up to %d + %d (%d Hz screen)")),half,half,hz];
}
/* Breath of the Wild's resolution list (OUTPUT_MODE_*, main's row). */
static TOTKSettingRow *settings_output_row(void) {
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
 if(res.count<2)return settings_fixed(settings_text("分辨率","Resolution"),@"1080p");
 return settings_row(settings_text("分辨率","Resolution"),res,resModes,
  ^int{return runtime_output_mode();},^(int v){if(runtime_output_mode_set(v))settings_window_for(v);});
}
/* The canvas (user 2026-10-26): 720p, 900p, 1080p and 1440p, up to the swapchain; the MetalFX preset stays. */
static TOTKSettingRow *settings_resolution_row(void) {
 unsigned mw=0,mh=0;if(!runtime_picture_canvas_max(&mw,&mh))return settings_output_row();
 static const unsigned sizes[][2]={{1280,720},{1600,900},{1920,1080},{2560,1440}};
 NSMutableArray<NSString *> *choices=[NSMutableArray array];NSMutableArray<NSNumber *> *values=[NSMutableArray array];
 for(int i=0;i<4;i++)if(sizes[i][0]<=mw&&sizes[i][1]<=mh){[choices addObject:[NSString stringWithFormat:@"%up",sizes[i][1]]];[values addObject:@(i)];}
 if(choices.count<2)return settings_fixed(settings_text("分辨率","Resolution"),@"1080p");
 return settings_row(settings_text("分辨率","Resolution"),choices,values,^int{
   uint64_t now=runtime_output_size_current();for(int i=0;i<4;i++)if(now==((uint64_t)sizes[i][1]<<32|sizes[i][0]))return i;return -1;},
  ^(int v){if(runtime_picture_set(sizes[v][0],sizes[v][1],runtime_picture_preset()))settings_window_size(sizes[v][0],sizes[v][1]);});
}
static TOTKSettingRow *settings_preset_row(void) {
 NSString *t=settings_text("MetalFX 超分","MetalFX upscaling");
 if(!runtime_picture_presets())return settings_fixed(t,settings_text("关闭（仅 1.4.3）","Off (1.4.3 only)"));
 return settings_row(t,@[settings_text("关闭","Off"),settings_text("画质（≈67%）","Quality (~67%)"),settings_text("平衡（≈59%）","Balanced (~59%)"),settings_text("性能（50%）","Performance (50%)")],
  @[@(PICTURE_PRESET_OFF),@(PICTURE_PRESET_QUALITY),@(PICTURE_PRESET_BALANCED),@(PICTURE_PRESET_PERFORMANCE)],
  ^int{return runtime_picture_preset();},^(int v){uint64_t now=runtime_output_size_current();
   if(runtime_picture_set((unsigned)now,(unsigned)(now>>32),v)&&v==PICTURE_PRESET_OFF)taa_set(0);});   /* anti-aliasing goes with the preset */
}

/* The gameplay sheet's rows (the controls line's third button): main's controller rows but the type, which went to the
   controller sheet with its buttons (user 2026-10-26). */
static NSArray<TOTKSettingRow *> *settings_gameplay_rows(void) {
 NSMutableArray<TOTKSettingRow *> *rows=[NSMutableArray array];
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
@interface TOTKGameplaySheet:NSObject
@property(strong) NSPanel *panel;
@property(strong) NSArray<TOTKSettingRow *> *rows;
@property(strong) NSTimer *timer;
-(void)open;
@end
static TOTKGameplaySheet *gameplaySheet;
static void settings_open_controls(int which) {   /* the controls line: 0 controller, 1 keyboard and mouse, 2 gameplay */
 if(!actionEditor)actionEditor=[TOTKActionEditor new];
 if(!controlsEditor)controlsEditor=[TOTKControlsEditor new];
 if(!gameplaySheet)gameplaySheet=[TOTKGameplaySheet new];
 if(which==0)[controlsEditor openPage:CONTROLS_PAGE_CONTROLLER];
 else if(which==1)[controlsEditor openPage:CONTROLS_PAGE_KEYBOARD];
 else [gameplaySheet open];
}

static BOOL g_settings_mfx_open;   /* the MetalFX section is open (kept for this run) */
/* The panel's lines, each one setting across or two side by side (user 2026-10-26). */
static NSArray<NSArray<TOTKSettingRow *> *> *settings_lines(void) {
 NSMutableArray<NSArray<TOTKSettingRow *> *> *lines=[NSMutableArray array];
 [lines addObject:@[settings_row(settings_text("性能信息","Overlay"),
  @[settings_text("全部（FPS、着色器、读取）","All (FPS, shaders, load)"),settings_text("只显示 FPS","FPS only"),settings_text("关","Off")],
  @[@(HUD_MODE_ALL),@(HUD_MODE_MINIMAL),@(HUD_MODE_OFF)],
  ^int{return fps_overlay_hud_mode();},^(int v){fps_overlay_set_hud_mode(v);[(TOTKHudBox *)hudBox refresh];})]];
 TOTKSettingRow *cap=settings_row(settings_text("帧率上限","Frame rate limit"),
  @[settings_text("关","Off"),settings_text("30 帧","30 fps"),settings_text("40 帧","40 fps"),settings_text("60 帧","60 fps"),settings_text("自定义…","Custom…")],
  @[@0,@30,@40,@60,@(SETTINGS_CUSTOM)],
  ^int{int l=nvn_fps_lock();return l==0||l==30||l==40||l==60?l:SETTINGS_CUSTOM;},^(int v){if(v==SETTINGS_CUSTOM)settings_fps_ask();else nvn_fps_lock_set(v);});
 cap.lastTitle=^NSString *{int l=nvn_fps_lock();
  return l&&l!=30&&l!=40&&l!=60?[NSString stringWithFormat:@(TOTK_UI("自定义（%d 帧）","Custom (%d fps)")),l]:settings_text("自定义…","Custom…");};
 TOTKSettingRow *vsync=settings_row(settings_text("垂直同步","Vertical sync"),nil,nil,
  ^int{return dm_gfx_vsync()||mfx_requested()!=MFX_OFF;},^(int v){dm_gfx_set_flag("vsync",v);});
 vsync.usable=^int{return mfx_requested()==MFX_OFF;};   /* interpolation orders its pictures on the display */
 [lines addObject:@[cap,vsync]];
 [lines addObject:@[settings_resolution_row(),settings_row(settings_text("窗口模式","Window mode"),
  @[settings_text("窗口","Window"),settings_text("全屏","Full screen"),settings_text("无边框","Borderless")],@[@(WINDOW_MODE_WINDOW),@(WINDOW_MODE_FULL),@(WINDOW_MODE_BORDERLESS)],
  ^int{return settings_window_mode();},^(int v){settings_window_mode_set(v);})]];
 NSMutableArray<TOTKSettingRow *> *aspect=[NSMutableArray array];
 if(!(runtime_aspect_ratio()>0))[aspect addObject:settings_fixed(settings_text("画面比例","Aspect ratio"),@"16:9")];
 else{NSMutableArray<NSNumber *> *ratios=[@[@(16.0/9),@(16.0/10),@(21.0/9),@(32.0/9)] mutableCopy];
  float start=runtime_aspect_start();BOOL listed=NO;   /* TOTK_ASPECT's ratio (2.39, 3440x1440 ...) as one more choice */
  for(NSNumber *r in ratios)listed|=aspect_near(start,r.doubleValue);
  if(start>0&&!listed)[ratios addObject:@(start)];
  NSMutableArray<NSString *> *choices=[NSMutableArray array];NSMutableArray<NSNumber *> *values=[NSMutableArray array];
  for(NSUInteger i=0;i<ratios.count;i++){[choices addObject:settings_ratio_text(ratios[i].floatValue)];[values addObject:@((int)i)];}
  [aspect addObject:settings_row(settings_text("画面比例","Aspect ratio"),choices,values,^int{
    float now=runtime_aspect_ratio();for(NSUInteger i=0;i<ratios.count;i++)if(aspect_near(now,ratios[i].doubleValue))return (int)i;return -1;},
   ^(int v){float r=ratios[(NSUInteger)v].floatValue;if(runtime_aspect_set_ratio(r))runtime_front_resize_aspect(r);})];}
 [lines addObject:aspect];
 [lines addObject:@[settings_buttons(settings_text("MetalFX 设置","MetalFX settings"),@[g_settings_mfx_open?settings_text("收起 ▾","Hide ▾"):settings_text("展开 ▸","Show ▸")],
  ^(int i){(void)i;g_settings_mfx_open=!g_settings_mfx_open;dispatch_async(dispatch_get_main_queue(),^{settings_rebuild();});})]];
 if(g_settings_mfx_open){
  TOTKSettingRow *preset=settings_preset_row(),*aa=settings_row(settings_text("抗锯齿","Anti-aliasing"),
   @[settings_text("FXAA（游戏原有）","FXAA (the game's)"),@"MetalFX TAA"],@[@0,@1],^int{return taa_enabled();},^(int v){taa_set(v);});
  aa.usable=^int{return !runtime_picture_presets()||runtime_picture_preset()!=PICTURE_PRESET_OFF;};   /* only with a preset on (user 2026-10-26) */
  TOTKSettingRow *fi=settings_row(settings_text("补帧（自适应）","Frame interpolation (adaptive)"),nil,nil,
   ^int{return mfx_requested()!=MFX_OFF;},^(int v){dm_gfx_set_flag("mfx_auto",v);}),*note=settings_note(^NSString *{return settings_fi_note();});
  preset.indent=aa.indent=fi.indent=note.indent=YES;
  [lines addObject:@[preset,aa]];[lines addObject:@[fi,note]];
 }
 [lines addObject:@[settings_buttons(settings_text("操作设置","Controls"),
  @[settings_text("手柄","Controller"),settings_text("键鼠","Keyboard & mouse"),settings_text("游戏性设置","Gameplay")],^(int i){settings_open_controls(i);})]];
 return lines;
}

enum { SETTINGS_W = PANEL_W, SETTINGS_ROW_H = PANEL_ROW_H, SETTINGS_GAP = PANEL_GAP, SETTINGS_TOP = 54, SETTINGS_BOTTOM = 86, SETTINGS_POP_W = 260,
       SETTINGS_HALF_POP_W = 150, SETTINGS_INDENT = 18 };
/* One line at (x, y), w wide: one setting across, or two side by side, each on its own band. */
static void settings_place(NSView *view, NSArray<TOTKSettingRow *> *line, CGFloat x, CGFloat y, CGFloat w, CGFloat h) {
 if(line.firstObject.indent){x+=SETTINGS_INDENT;w-=SETTINGS_INDENT;}
 CGFloat cw=line.count>1?floor((w-8)/2):w;
 for(NSUInteger c=0;c<line.count;c++){
  TOTKSettingRow *r=line[c];
  NSView *band=panel_band(view,NSMakeRect(x+c*(cw+8),y,cw,h));
  if(r.note){NSTextField *t=[NSTextField labelWithString:@""];t.font=[NSFont systemFontOfSize:12];t.textColor=[NSColor colorWithCalibratedWhite:0.75 alpha:1];
   t.lineBreakMode=NSLineBreakByTruncatingTail;t.frame=NSMakeRect(12,floor((h-16)/2),cw-24,16);[band addSubview:t];r.control=t;continue;}
  CGFloat right;   /* where the control starts: the title takes the room to its left */
  if(r.buttons){
   NSUInteger n=r.buttons.count;CGFloat bw=n>1?120:96,total=n*bw+(n-1)*6;right=cw-8-total;
   for(NSUInteger i=0;i<n;i++){NSButton *b=[NSButton buttonWithTitle:r.buttons[i] target:r action:@selector(changed:)];b.tag=(NSInteger)i;
    b.frame=NSMakeRect(right+i*(bw+6),floor((h-28)/2),bw,28);b.refusesFirstResponder=YES;[band addSubview:b];if(!i)r.control=b;}
  }else if(!r.choices){NSSwitch *sw=[NSSwitch new];[sw sizeToFit];right=cw-12-sw.frame.size.width;
   sw.frame=NSMakeRect(right,(h-sw.frame.size.height)/2,sw.frame.size.width,sw.frame.size.height);r.control=sw;}
  else{CGFloat pw=line.count>1?SETTINGS_HALF_POP_W:SETTINGS_POP_W;right=cw-pw-8;
   NSPopUpButton *p=[[NSPopUpButton alloc] initWithFrame:NSMakeRect(right,floor((h-26)/2),pw,26) pullsDown:NO];[p addItemsWithTitles:r.choices];r.control=p;}
  if(!r.buttons){r.control.refusesFirstResponder=YES;r.control.target=r;r.control.action=@selector(changed:);[band addSubview:r.control];}
  panel_label(band,r.title,12,right-18,h);
 }
}

@implementation TOTKGameplaySheet
-(void)open{
 if(self.panel||!window||window.attachedSheet)return;
 self.rows=settings_gameplay_rows();CGFloat n=self.rows.count,h=PANEL_SHEET_TOP+n*(SETTINGS_ROW_H+SETTINGS_GAP)-SETTINGS_GAP+56;
 self.panel=panel_sheet(settings_text("游戏性设置","Gameplay"),SETTINGS_W,h);NSView *view=self.panel.contentView;
 for(NSUInteger i=0;i<self.rows.count;i++)settings_place(view,@[self.rows[i]],14,h-PANEL_SHEET_TOP-(i+1)*SETTINGS_ROW_H-i*SETTINGS_GAP,SETTINGS_W-28,SETTINGS_ROW_H);
 SEL done=@selector(done:);panel_buttons(view,SETTINGS_W,@[settings_text("完成","Done")],self,&done);
 NSButton *esc=[NSButton buttonWithTitle:@"" target:self action:@selector(done:)];esc.keyEquivalent=@"\e";esc.frame=NSMakeRect(-100,-100,10,10);   /* Esc closes it too, off screen */[view addSubview:esc];
 [self refresh];__weak TOTKGameplaySheet *weak=self;
 self.timer=[NSTimer scheduledTimerWithTimeInterval:0.5 repeats:YES block:^(NSTimer *t){(void)t;[weak refresh];}];
 [window beginSheet:self.panel completionHandler:^(NSModalResponse response){(void)response;
  [self.timer invalidate];self.timer=nil;[self.panel orderOut:nil];self.panel=nil;settings_refresh_all();}];
}
-(void)refresh{for(TOTKSettingRow *r in self.rows)[r refresh];}
-(void)done:(id)sender{(void)sender;if(self.panel)[window endSheet:self.panel];}
@end

@interface TOTKSettingsPanel:NSView
@property(strong) NSArray<TOTKSettingRow *> *rows;   /* every setting of every line, in order */
@property(strong) NSTimer *timer;
@property NSPoint grab;   /* where in the panel the drag started */
@property(strong) NSTextField *hint;   /* the bottom line; the buttons' results show there */
-(void)refresh;
@end
/* Where the player dragged the panel (user 2026-10-25: it can be moved), as the fraction of the free room left and below
   it, so a new opening and a resized window keep the spot; -1 = centred. */
static NSPoint g_settings_at = {-1, -1};
@implementation TOTKSettingsPanel
-(instancetype)init{
 NSArray<NSArray<TOTKSettingRow *> *> *lines=settings_lines();
 /* The lines close up (down to 26 pt, 2 pt apart) when the panel would not fit the window (21:9 or 32:9 on a laptop
    screen), so its title, close button and first lines stay in view. */
 CGFloat n=lines.count,rowH=SETTINGS_ROW_H,gap=SETTINGS_GAP,room=window?window.contentView.bounds.size.height-8:CGFLOAT_MAX;
 if(n&&SETTINGS_TOP+n*(rowH+gap)-gap+SETTINGS_BOTTOM>room){gap=2;rowH=fmax(26,floor((room-SETTINGS_TOP-SETTINGS_BOTTOM+gap)/n-gap));}
 CGFloat h=SETTINGS_TOP+n*(rowH+gap)-gap+SETTINGS_BOTTOM;
 if(!(self=[super initWithFrame:NSMakeRect(0,0,SETTINGS_W,h)]))return nil;
 NSMutableArray<TOTKSettingRow *> *rows=[NSMutableArray array];for(NSArray *line in lines)[rows addObjectsFromArray:line];
 self.rows=rows;panel_dark(self);
 self.layer.cornerRadius=12;self.layer.borderWidth=1;self.layer.borderColor=[NSColor colorWithCalibratedWhite:1 alpha:0.12].CGColor;
 self.autoresizingMask=NSViewMinXMargin|NSViewMaxXMargin|NSViewMinYMargin|NSViewMaxYMargin;
 NSTextField *title=[NSTextField labelWithString:settings_text("设置","Settings")];title.font=[NSFont systemFontOfSize:17 weight:NSFontWeightSemibold];
 title.textColor=NSColor.whiteColor;title.frame=NSMakeRect(18,h-38,300,24);[self addSubview:title];
 NSButton *close=[NSButton buttonWithTitle:@"✕" target:self action:@selector(close:)];close.bordered=NO;close.refusesFirstResponder=YES;
 close.font=[NSFont systemFontOfSize:15 weight:NSFontWeightBold];close.contentTintColor=NSColor.whiteColor;close.frame=NSMakeRect(SETTINGS_W-40,h-40,28,28);[self addSubview:close];
 for(NSUInteger i=0;i<lines.count;i++)settings_place(self,lines[i],14,h-SETTINGS_TOP-(i+1)*rowH-i*gap,SETTINGS_W-28,rowH);
 CGFloat bw=(SETTINGS_W-28-10)/2;
 NSButton *contrib=[NSButton buttonWithTitle:settings_text("贡献着色器和管线","Contribute shaders and pipelines") target:self action:@selector(contribute:)];
 contrib.frame=NSMakeRect(14,40,bw,30);contrib.refusesFirstResponder=YES;[self addSubview:contrib];
 NSButton *star=[NSButton buttonWithTitle:settings_text("⭐ 给作者点赞","⭐ Star the project") target:self action:@selector(star:)];
 star.frame=NSMakeRect(14+bw+10,40,bw,30);star.refusesFirstResponder=YES;[self addSubview:star];
 NSTextField *hint=[NSTextField labelWithString:settings_text("按 1 或 Esc 关闭 · 打开时键盘和鼠标不会传给游戏","1 or Esc closes it · keyboard and mouse do not reach the game while it is open")];
 hint.font=[NSFont systemFontOfSize:11];hint.textColor=[NSColor colorWithCalibratedWhite:0.7 alpha:1];hint.alignment=NSTextAlignmentCenter;
 hint.lineBreakMode=NSLineBreakByTruncatingMiddle;hint.frame=NSMakeRect(10,12,SETTINGS_W-20,16);[self addSubview:hint];self.hint=hint;
 [self refresh];
 __weak TOTKSettingsPanel *weak=self;self.timer=[NSTimer scheduledTimerWithTimeInterval:0.5 repeats:YES block:^(NSTimer *t){(void)t;if(!weak.hidden)[weak refresh];}];
 return self;
}
-(void)refresh{for(TOTKSettingRow *r in self.rows)[r refresh];}
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
/* The MetalFX section opened or closed: the panel made again, its top edge where it was. */
static void settings_rebuild(void) {
 if(!window||!settingsOpen||!settingsPanel)return;
 NSRect was=settingsPanel.frame;[settingsPanel.timer invalidate];[settingsPanel removeFromSuperview];
 settingsPanel=[TOTKSettingsPanel new];[window.contentView addSubview:settingsPanel];
 NSSize room=window.contentView.bounds.size,size=settingsPanel.frame.size;
 [settingsPanel setFrameOrigin:NSMakePoint(fmin(fmax(0,was.origin.x),fmax(0,room.width-size.width)),fmin(fmax(0,NSMaxY(was)-size.height),fmax(0,room.height-size.height)))];
 [settingsPanel refresh];
}
static void settings_show(int open) {   /* main thread */
 open=!!open;
 if(!window||open==settingsOpen)return;
 /* Made again at every opening: what the game offers (aspect ratio, resolution) is known only once its code is in. */
 if(open){[settingsPanel.timer invalidate];[settingsPanel removeFromSuperview];settingsPanel=[TOTKSettingsPanel new];[window.contentView addSubview:settingsPanel];}
 settingsOpen=open;
 if(open){runtime_input_clear();[settingsPanel center];[settingsPanel refresh];[(TOTKInputView *)window.contentView setCursorAutoHideEnabled:NO];}
 else{[window makeFirstResponder:window.contentView];
  if(window.isKeyWindow&&(window.styleMask&NSWindowStyleMaskFullScreen))[(TOTKInputView *)window.contentView setCursorAutoHideEnabled:YES];}
 settingsPanel.hidden=!open;
 front_mouse_update();
 fprintf(stderr,"[totk] settings panel %s\n",open?"open":"closed");
}
void runtime_settings_toggle(void){dispatch_async(dispatch_get_main_queue(), ^{settings_show(!settingsOpen);});}
