/* Luigi's Mansion 3's part of the settings panel (shared/runtime/settings_panel.h reads it, SETTINGS_GAME_PART 1 and 2), as
   the Zelda games have it: key 1 (or Command-,) opens and closes it. It replaces the "Game Controls" window and its F1-F4
   shortcuts (user 2026-11-23: "la meme ui que BOTW"); its picture rows edit the same settings that window did
   (display_settings.h: saved in the org.macnativegames.totk.display defaults, applied by controls_apply). Modelled on
   games/eow's file, this game's sibling fork.
   Display
     overlay              off / bare FPS / FPS + playtime / all / FPS and frame debug info   fps_overlay_set_hud_mode,
                                                                             graphics-overlay.txt
     FPS graph            switch                         fps-graph.txt
     resolution           720p / 900p / 1080p / 1440p    the target the game's picture is made for (with MetalFX below)
     aspect ratio         16:9 / 16:10 / 21:9            runtime_aspect_set_ratio (aspect_patch.h, 1.4.0 only; greyed on
                                                         1.2.0 or with TOTK_ASPECT): the camera frames that shape and the
                                                         picture keeps it in the window
     window mode          windowed / full screen / borderless
     vertical sync        switch
     frame limit          off / 30 / 60 / 120            the real frames' limiter (nvn_control_pacing) and the game's own
                                                         target frame time (runtime_frame_target_set, 1.4.0)
     frame generation     switch                         MetalFX frame generation, one frame between each pair
   Input
     controller type      auto / Xbox / PlayStation / Switch   runtime_gamepad_type_set (gamepad.m), controller-type.txt
     rumble               switch                         runtime_feedback_choose (hid_feedback.c), rumble.txt
     gyro                 shown only (hid_feedback.c decides it at the start)
     DSU                  switch, server, slot, status   the shared DSU rows
   Effects
     MetalFX upscaling    off / quality / balanced / performance   the 3D render size under the resolution above; Reset to
                                                         defaults: quality, the start's default
   Key bindings, with the screenshot row (P, or the touchpad held 3 s); 性能排行榜 and 截图分享 (both off by default).
   Left out, as in Echoes of Wisdom: the User page (the saves stay under the one account, runtime/account.c), the 着色器贡献
   page, VR. */
#if SETTINGS_GAME_PART == 1
#define SETTINGS_HAS_EFFECTS 1
#define SETTINGS_HAS_BOARD 1
#define SETTINGS_HAS_SHOTS 1
#define SETTINGS_HAS_CONTRIB 0

/* Settings live alongside controls.txt and are restored before the guest starts (the picture settings: display_load). */
static void settings_restore_start(void) {
 int v;
 if(settings_read_int("graphics-overlay.txt",&v)&&v<=HUD_MODE_BARE)fps_overlay_set_hud_mode(v);
}
/* What the shared panel asks of a game that this one does not have: no state reporting (gfx/nvn/totk_state_report.m),
   nothing to tell when the panel opens or closes. */
static void (*const totk_sr_settings)(int open) = NULL;

/* The DSU rows of the input page (runtime/dsu_client.h in gamepad.m): a server to read a controller's motion and buttons from. */
#include "dsu_settings_rows.h"
/* A picture setting: set, saved and applied at once (controls_apply: render size, pacing, frame generation, the window). */
static void settings_display_changed(void) { display_save(); controls_apply(); }
/* The effects page: MetalFX upscaling. */
static void settings_effect_rows(NSMutableArray<TOTKSettingRow *> *rows) {
 [rows addObject:settings_row(settings_text("MetalFX 空间超分","MetalFX upscaling"),
  @[settings_text("关闭 · 100% 渲染","Off · 100% rendering"),settings_text("质量 · 约 67% 渲染","Quality · about 67%"),
    settings_text("平衡 · 约 59% 渲染","Balanced · about 59%"),settings_text("性能 · 50% 渲染","Performance · 50%")],@[@0,@1,@2,@3],
  ^int{return controlQuality;},^(int v){controlQuality=v;settings_display_changed();})];
}
/* The effects page's "Reset to defaults" (settings_panel.h resetEffects:): MetalFX upscaling back to quality, the start's
   default (display_load). */
static void settings_effects_reset(void) {
 controlQuality=1;settings_display_changed();
 fprintf(stderr,"[lm3] settings: the effects page back to the defaults\n");
}

static NSArray<TOTKSettingRow *> *settings_rows(void) {
 NSMutableArray<TOTKSettingRow *> *rows=[NSMutableArray array];
 /* Display */
 [rows addObject:settings_row(settings_text("性能信息","Overlay"),
  @[settings_on_off(0),settings_text("极简 FPS","Bare FPS"),settings_text("FPS 与游玩时间","FPS + playtime"),settings_text("全部","All"),
    settings_text("FPS 和帧调试信息","FPS and frame debug info")],
  @[@(HUD_MODE_OFF),@(HUD_MODE_BARE),@(HUD_MODE_MINIMAL),@(HUD_MODE_ALL),@(HUD_MODE_FULL)],
  ^int{return fps_overlay_hud_mode();},^(int v){fps_overlay_set_hud_mode(v);settings_save_int("graphics-overlay.txt",v,"overlay");[(TOTKHudBox *)hudBox refresh];})];
 [rows addObject:settings_row(settings_text("帧率图","FPS graph"),nil,nil,
  ^int{return fpsGraphEnabled;},^(int v){fpsGraphEnabled=v;runtime_setting_save("fps-graph.txt",v?"on":"off","FPS graph");[(TOTKFpsGraph *)fpsGraphBox setEnabled:v];[(TOTKHudBox *)hudBox refresh];})];
 [rows addObject:settings_row(settings_text("分辨率","Resolution"),@[@"1280 × 720",@"1600 × 900",@"1920 × 1080",@"2560 × 1440"],@[@0,@1,@2,@3],
  ^int{return displayResolution;},^(int v){displayResolution=v;settings_display_changed();})];
 {NSString *t=settings_text("画面比例","Aspect ratio");NSArray<NSString *> *names=@[settings_text("16:9（游戏原有）","16:9 (the game's own)"),@"16:10",@"21:9"];
  const char *env=getenv("TOTK_ASPECT");
  if((env&&*env)||!runtime_aspect_set_ratio(runtime_aspect_now()))
   [rows addObject:settings_fixed(t,env&&*env?[NSString stringWithFormat:@"TOTK_ASPECT=%s",env]:names[0])];
  else [rows addObject:settings_row(t,names,@[@0,@1,@2],^int{return displayAspect;},^(int v){displayAspect=v;settings_display_changed();})];}
 [rows addObject:settings_row(settings_text("窗口模式","Window mode"),
  @[settings_text("窗口","Windowed"),settings_text("全屏（独立空间）","Full screen"),settings_text("无边框窗口","Borderless window")],@[@0,@1,@2],
  ^int{return displayMode;},^(int v){displayMode=v;settings_display_changed();})];
 [rows addObject:settings_row(settings_text("垂直同步","Vertical sync"),nil,nil,^int{return controlVsync;},^(int v){controlVsync=v;settings_display_changed();})];
 [rows addObject:settings_row(settings_text("帧率上限","Frame limit"),@[settings_on_off(0),@"30",@"60",@"120"],@[@0,@30,@60,@120],
  ^int{return controlCap;},^(int v){controlCap=v;settings_display_changed();})];
 [rows addObject:settings_row(settings_text("补帧（MetalFX）","Frame generation (MetalFX)"),nil,nil,^int{return controlFI;},^(int v){controlFI=v;settings_display_changed();})];
 NSUInteger display=rows.count;
 /* Input */
 TOTKSettingRow *pad=settings_row(settings_text("手柄类型","Controller type"),@[settings_pad_auto_text(),@"Xbox",@"PlayStation",@"Switch"],
  @[@(GAMEPAD_TYPE_AUTO),@(GAMEPAD_TYPE_XBOX),@(GAMEPAD_TYPE_PLAYSTATION),@(GAMEPAD_TYPE_SWITCH)],
  ^int{return runtime_gamepad_type(NULL);},^(int v){runtime_gamepad_type_set(v);});
 pad.firstTitle=^NSString *{return settings_pad_auto_text();};[rows addObject:pad];
 [rows addObject:settings_feedback_row(0)];   /* rumble */
 [rows addObject:settings_feedback_row(1)];   /* gyro: shown, decided at the start */
 settings_dsu_rows(rows);
 NSUInteger input=rows.count;
 /* Effects */
 settings_effect_rows(rows);
 for(NSUInteger i=0;i<rows.count;i++)rows[i].page=i<display?SETTINGS_PAGE_DISPLAY:i<input?SETTINGS_PAGE_INPUT:SETTINGS_PAGE_EFFECTS;
 return rows;
}
/* The game actions the key bindings page lists, in its order; action -1 starts a section (SettingsAction,
   SETTINGS_BIND_PANEL: shared/runtime/settings_panel.h). Named after the Switch buttons; A confirms and B goes back in the menus. */
static const SettingsAction *settings_actions(int *count) {
 static const SettingsAction list[]={
  {-1,"移动（左摇杆）","Movement (left stick)",NULL},
  {KEYMAP_MOVE_UP,"向前","Forward",NULL},{KEYMAP_MOVE_DOWN,"向后","Back",NULL},
  {KEYMAP_MOVE_LEFT,"向左","Left",NULL},{KEYMAP_MOVE_RIGHT,"向右","Right",NULL},
  {-1,"右摇杆","Right stick",NULL},
  {KEYMAP_CAMERA_UP,"右摇杆 向上","Right stick up",NULL},{KEYMAP_CAMERA_DOWN,"右摇杆 向下","Right stick down",NULL},
  {KEYMAP_CAMERA_LEFT,"右摇杆 向左","Right stick left",NULL},{KEYMAP_CAMERA_RIGHT,"右摇杆 向右","Right stick right",NULL},
  {-1,"按键","Buttons",NULL},
  {KEYMAP_A,"A 键（确认）","A button (confirm)","A"},
  {KEYMAP_B,"B 键（返回）","B button (back)","B"},
  {KEYMAP_X,"X 键","X button","X"},
  {KEYMAP_Y,"Y 键","Y button","Y"},
  {KEYMAP_L,"L 键","L button","L"},
  {KEYMAP_R,"R 键","R button","R"},
  {KEYMAP_ZL,"ZL 键","ZL button","ZL"},
  {KEYMAP_ZR,"ZR 键","ZR button","ZR"},
  {KEYMAP_LSTICK,"按下左摇杆","Left stick press","L3"},
  {KEYMAP_RSTICK,"按下右摇杆","Right stick press","R3"},
  {KEYMAP_PLUS,"+ 键","+ button","+"},
  {KEYMAP_MINUS,"− 键","− button","−"},
  {-1,"十字键","D-pad",NULL},
  {KEYMAP_DUP,"十字键 上","D-pad up","↑"},{KEYMAP_DDOWN,"十字键 下","D-pad down","↓"},
  {KEYMAP_DLEFT,"十字键 左","D-pad left","←"},{KEYMAP_DRIGHT,"十字键 右","D-pad right","→"},
  {-1,"设置和截图","Settings & screenshots",NULL},
  {SETTINGS_BIND_PANEL,"打开 / 关闭设置面板","Open / close this panel",NULL},
  {KEYMAP_SCREENSHOT,"截图","Screenshot",NULL}};
 *count=(int)(sizeof list/sizeof *list);
 return list;
}
#elif SETTINGS_GAME_PART == 2
/* Diagnostics (TOTK_INPUT_FILE "panelshot <page> <path>", runtime/input.c): the panel open on page 0 display, 1 input, 2 the
   keyboard and mouse drawing, 3 the controller drawing, drawn into a PNG (window screenshots do not show AppKit views to a
   script). */
void runtime_settings_shot(int page,const char *path){
 NSString *file=@(path);
 dispatch_async(dispatch_get_main_queue(), ^{
  g_settings_bindings_list=NO;
  if(page>=2){g_settings_bindings_device=page-2;g_settings_page=SETTINGS_PAGE_BINDINGS;}else g_settings_page=page?SETTINGS_PAGE_INPUT:SETTINGS_PAGE_DISPLAY;
  if(settingsOpen)settings_rebuild();else settings_show(1);
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW,(int64_t)(0.5*NSEC_PER_SEC)),dispatch_get_main_queue(),^{
   TOTKSettingsPanel *panel=settingsPanel;if(!panel){fprintf(stderr,"[totk] panelshot: no panel\n");return;}
   [panel refresh];[panel displayIfNeeded];
   NSBitmapImageRep *rep=[panel bitmapImageRepForCachingDisplayInRect:panel.bounds];[panel cacheDisplayInRect:panel.bounds toBitmapImageRep:rep];
   BOOL ok=[[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:file atomically:YES];
   fprintf(stderr,"[totk] panelshot: page %d %s %s\n",page,file.UTF8String,ok?"written":"FAILED");});
 });
}
#endif
