/* Animal Crossing's part of the settings panel (shared/runtime/settings_panel.h reads it, SETTINGS_GAME_PART 1 and 2). Until
   2026-11-17 this game had its own reduced copy of games/totk's panel (PR #1305: the Zelda panel for Animal Crossing,
   leaving out what this game cannot use); it now has the Zelda games' panel and pages, as far as this game supports them:
   Display
     overlay              all / FPS full information / FPS + playtime / off   fps_overlay_set_hud_mode (shared fps_overlay.c),
                                                                             graphics-overlay.txt
     FPS graph            switch                         fps-graph.txt
     frame rate           30 / 60 / 120 fps, next launch  3.0.1 (60) and 3.0.3 (60, 120) with prepared SpeedFix data: code and
                                                         parameter corrections must be loaded together, so changing modes
                                                         requires a restart. 120 is offered only with a 120 Hz screen
                                                         connected (frame_rate.c display_holds; runtime_display_max_hz): on a
                                                         60 Hz screen the game played at half speed, and a saved 120 starts
                                                         at 60 there. Builds without that data (including 1.2.1) keep 30 fps.
     frame interpolation  not available, greyed out      MetalFX frame interpolation (gfx/nvn/totk_mfx.m) needs the game's
                                                         scene, depth, camera and UI passes; its hooks know Tears of the
                                                         Kingdom's, and in Animal Crossing every frame lacks all four (test
                                                         run 2026-10-29: 0 frames made in 2,100), so 30+30 would only pace
                                                         the game, not add frames. Left out until it has hooks for this game.
     game language        follow the Mac / 14 languages  language.txt, read at the next launch (runtime/services.c
                                                         runtime_desired_language); TOTK_LANG decides first
   Aspect ratio and the game's own resolution are left out: Animal Crossing has no patch for them here. The window opens
   at the game's 1920x1080 output, pixel for pixel (frontend.m), and resizing or full screen scales the picture.
   Input
     controller type      auto / Xbox / PlayStation / Switch   runtime_gamepad_type_set (gamepad.m), controller-type.txt
     rumble               switch                         runtime_rumble_choose (hid_feedback.c), rumble.txt
     DSU                  switch, server, slot, status   the shared DSU rows (dsu_settings_rows.h, gamepad.m dsu_client.h)
   Effects
     picture              1080p / 1080p + MetalFX        the game draws 1920x1080; MetalFX can upscale that picture to a
                          upscaling                      larger window or full screen instead of the plain scaling
                                                         (flags mfx_upscale + mfx_output, gfx/nvn/totk_mfx_up.m),
                                                         graphics-output.txt; TOTK_MFX_UPSCALE / TOTK_MFX_OUTPUT decide first.
                                                         Reset to defaults: plain scaling again (the file removed).
   Key bindings, with the screenshot row (P, or the touchpad held 3 s); 性能排行榜 and 截图分享 (both off by default).
   Left out: the User page (one island per console: see below), the 着色器贡献 page (this game's shaders have no list
   that contributions could go to), VR, rumble strength, gyro aiming and mouse sensitivity (no such settings here).
   The User page: on a console Animal Crossing keeps one island for every user, and the game checks the residents' user
   ids against the console's users. This host gives each user a save folder of its own (runtime/fs_host.c names it after
   the user's id), so a second user would start a second island, and the game's account calls here know one user only
   (runtime/account.c). The saves stay with the one account they have always had (<TOTK_PROFILE_DIR>/account.uid). */
#if SETTINGS_GAME_PART == 1
#define SETTINGS_HAS_EFFECTS 1
#define SETTINGS_HAS_BOARD 1
#define SETTINGS_HAS_SHOTS 1
#define SETTINGS_HAS_CONTRIB 0
/* Opaque (games/totk: 0.9): Animal Crossing's bright screens, and its own letter keyboard in the name entry, showed through
   the key bindings drawing and read as keys out of place (reported in play, PR #1305). */
#define SETTINGS_PANEL_ALPHA 1.0
#include "../third_party/d2r_gfx/gfx/nvn/totk_mfx_up.h"   /* mfxup_*: MetalFX upscaling of the picture */

void dm_gfx_set_flag(const char *name, int value);

/* The picture: 1 MetalFX upscales it (both stages: the game's upscale and the window's output), 0 plain scaling. */
static int settings_picture_locked(void) { return getenv("TOTK_MFX_UPSCALE")||getenv("TOTK_MFX_OUTPUT"); }
static int settings_picture(void) { return mfxup_enabled()&&mfxup_output_enabled(); }
/* The window follows the Picture choice at once (user 2026-11-17: "调整完 没任何效果" / "立刻生效"): MetalFX only has
   something to do when the window is larger than the game's 1920x1080, so on it is 2560x1440 pixels, off 1920x1080
   (frontend.m's first window), 16:9, kept inside the screen's visible area and centred where it was. Full screen stays. */
static void settings_picture_window(int on) {
 if(!window||(window.styleMask&NSWindowStyleMaskFullScreen))return;
 NSScreen *screen=window.screen?:NSScreen.mainScreen;if(!screen)return;
 CGFloat backing=screen.backingScaleFactor>0?screen.backingScaleFactor:1,w=(on?2560:1920)/backing,h=(on?1440:1080)/backing;
 NSRect vis=screen.visibleFrame,content=[window contentRectForFrameRect:vis];
 CGFloat k=fmin(1,fmin(content.size.width/w,content.size.height/h));w=floor(w*k);h=floor(h*k);
 NSRect old=window.frame,f=[window frameRectForContentRect:NSMakeRect(0,0,w,h)];
 f.origin=NSMakePoint(NSMidX(old)-f.size.width/2,NSMidY(old)-f.size.height/2);
 f.origin.x=fmax(NSMinX(vis),fmin(f.origin.x,NSMaxX(vis)-f.size.width));
 f.origin.y=fmax(NSMinY(vis),fmin(f.origin.y,NSMaxY(vis)-f.size.height));
 [window setFrame:f display:YES];
 fprintf(stderr,"[acnh] settings: window %.0fx%.0f points for %s\n",w,h,on?"1080p + MetalFX (2560x1440)":"1080p");
}
static void settings_picture_set(int on) { mfxup_set(on);mfxup_output_set(on);settings_picture_window(on); }
/* Settings live alongside controls.txt and are restored before the guest starts. */
static void settings_restore_start(void) {
 int v;
 if(settings_read_int("graphics-overlay.txt",&v)&&v<=HUD_MODE_FULL)fps_overlay_set_hud_mode(v);
 if(!settings_picture_locked()&&settings_read_int("graphics-output.txt",&v)&&(v==0||v==1))settings_picture_set(v);
}
/* What the shared panel asks of a game that this one does not have: no state reporting (gfx/nvn/totk_state_report.m),
   nothing to tell when the panel opens or closes. */
static void (*const totk_sr_settings)(int open) = NULL;

/* The game's languages (nn::settings language codes; Animal Crossing has no Portuguese), in their own names. Value 0
   follows the Mac (TOTK_LOCALE / the Mac's language, runtime/services.c). */
static const char *const settings_languages[][2]={{"auto",NULL},{"ja","日本語"},{"en-US","English (US)"},{"en-GB","English (UK)"},
 {"fr","Français"},{"fr-CA","Français (Canada)"},{"de","Deutsch"},{"it","Italiano"},{"es","Español (España)"},
 {"es-419","Español (Latinoamérica)"},{"nl","Nederlands"},{"ru","Русский"},{"ko","한국어"},{"zh-Hans","简体中文"},{"zh-Hant","繁體中文"}};
enum { SETTINGS_LANGUAGES = sizeof settings_languages / sizeof *settings_languages };
static int settings_language(void) {
 char word[32];if(!runtime_setting_word("language.txt",word,sizeof word))return 0;
 word[strcspn(word," \t\r\n")]=0;
 for(int i=1;i<SETTINGS_LANGUAGES;i++)if(!strcmp(word,settings_languages[i][0]))return i;
 return 0;
}
static TOTKSettingRow *settings_language_row(void) {
 NSString *t=settings_text("游戏语言（下次启动生效）","Game language (from the next launch)");
 if(getenv("TOTK_LANG"))return settings_fixed(t,[NSString stringWithFormat:@(TOTK_UI("由 TOTK_LANG 设置（%s）","Set by TOTK_LANG (%s)")),getenv("TOTK_LANG")]);
 NSMutableArray<NSString *> *choices=[NSMutableArray array];NSMutableArray<NSNumber *> *values=[NSMutableArray array];
 for(int i=0;i<SETTINGS_LANGUAGES;i++){[choices addObject:i?@(settings_languages[i][1]):settings_text("跟随 Mac","Follow the Mac")];[values addObject:@(i)];}
 return settings_row(t,choices,values,^int{return settings_language();},^(int v){
  if(v<=0)runtime_setting_remove("language.txt");else runtime_setting_save("language.txt",settings_languages[v][0],"game language");
  fprintf(stderr,"[totk] settings: game language %s from the next launch\n",settings_languages[v>0?v:0][0]);});
}

/* The DSU rows of the input page (runtime/dsu_client.h in gamepad.m): a server to read a controller's motion and buttons from. */
#include "dsu_settings_rows.h"
/* The effects page (2026-11-17: as the Zelda games): this game's picture row, moved from Display. */
static void settings_effect_rows(NSMutableArray<TOTKSettingRow *> *rows) {
 NSString *picture=settings_text("画面","Picture");
 NSArray<NSString *> *pictures=@[@"1080p",settings_text("1080p + MetalFX 放大（大窗口、全屏）","1080p + MetalFX upscaling (large window, full screen)")];
 if(settings_picture_locked())[rows addObject:settings_fixed(picture,pictures[settings_picture()])];
 else [rows addObject:settings_row(picture,pictures,@[@0,@1],^int{return settings_picture();},^(int v){settings_picture_set(v);settings_save_int("graphics-output.txt",v,"picture");})];
}
/* The effects page's "Reset to defaults" (settings_panel.h resetEffects:): the picture back to the plain scaling the game
   starts with (graphics-output.txt removed). A choice the environment made (TOTK_MFX_UPSCALE, TOTK_MFX_OUTPUT) stays, as the
   Zelda games' reset leaves a row the environment decides. */
static void settings_effects_reset(void) {
 if(!settings_picture_locked()){settings_picture_set(0);runtime_setting_remove("graphics-output.txt");}
 fprintf(stderr,"[acnh] settings: the effects page back to the defaults\n");
}

static NSArray<TOTKSettingRow *> *settings_rows(void) {
 NSMutableArray<TOTKSettingRow *> *rows=[NSMutableArray array];
 /* Display */
 [rows addObject:settings_row(settings_text("性能信息","Overlay"),
  @[settings_text("全部（FPS、着色器、读取、游玩时间）","All (FPS, shaders, load, playtime)"),
    settings_text("FPS 全信息（另加卡顿、着色器库、管线、存档没命中）","FPS full information (also hitches, libraries, pipelines, archive misses)"),
    settings_text("FPS 与游玩时间","FPS + playtime"),settings_on_off(0)],
  @[@(HUD_MODE_ALL),@(HUD_MODE_FULL),@(HUD_MODE_MINIMAL),@(HUD_MODE_OFF)],
  ^int{return fps_overlay_hud_mode();},^(int v){fps_overlay_set_hud_mode(v);settings_save_int("graphics-overlay.txt",v,"overlay");[(TOTKHudBox *)hudBox refresh];})];
 [rows addObject:settings_row(settings_text("帧率图","FPS graph"),nil,nil,
  ^int{return fpsGraphEnabled;},^(int v){fpsGraphEnabled=v;runtime_setting_save("fps-graph.txt",v?"on":"off","FPS graph");[(TOTKFpsGraph *)fpsGraphBox setEnabled:v];[(TOTKHudBox *)hudBox refresh];})];
 if(runtime_frame_rate_available()&&!runtime_frame_rate_locked()) {
  NSMutableArray<NSString *> *rateNames=[NSMutableArray arrayWithObject:settings_text("30 帧","30 fps")];NSMutableArray<NSNumber *> *rateValues=[NSMutableArray arrayWithObject:@30];
  if(runtime_frame_rate_offers(60)){[rateNames addObject:settings_text("60 帧（实验性）","60 fps (experimental)")];[rateValues addObject:@60];}
  /* 120 is offered only with a 120 Hz screen connected (frame_rate.c display_holds): below a steady 120 the game slows down. */
  if(runtime_frame_rate_offers(120)){[rateNames addObject:settings_text("120 帧（实验性）","120 fps (experimental)")];[rateValues addObject:@120];}
  [rows addObject:settings_row(settings_text("帧率上限","Frame rate"),rateNames,rateValues,
   ^int{return runtime_frame_rate_choice();},^(int v){runtime_frame_rate_choose(v);})];
  /* The rate's speed corrections and data go into the game as it starts (frame_rate.c), so a new choice needs a restart:
     this row says which rate runs and restarts the game on request (user 2026-11-17: "立刻生效"). The app launcher
     (scripts/acnh_app_launcher.c) starts the game again when it finds restart-requested.txt after the game closed. */
  extern void runtime_front_request_close(void);   /* frontend.m: the window closes as its close button does */
  TOTKSettingRow *restart=[TOTKSettingRow new];restart.title=settings_text("帧率生效","Frame rate in use");
  restart.text=^NSString *{int now=runtime_frame_rate_active(),want=runtime_frame_rate_choice();
   return now==want?[NSString stringWithFormat:@(TOTK_UI("现在 %d 帧","%d fps now")),now]:
    [NSString stringWithFormat:@(TOTK_UI("现在 %d 帧 · 重启后 %d 帧（上次存档后的进度会丢失）","%d fps now · %d fps after a restart (progress since the last save is lost)")),now,want];};
  restart.buttonTitle=settings_text("立即重启","Restart now");
  restart.buttonAction=^{
   if(runtime_frame_rate_active()==runtime_frame_rate_choice()){settings_footer_note(settings_text("已经是这个帧率","Already at this frame rate"),NO);return;}
   runtime_setting_save("restart-requested.txt","frame rate","restart");
   fprintf(stderr,"[acnh-fps] restart requested for %d FPS\n",runtime_frame_rate_choice());
   runtime_front_request_close();};
  [rows addObject:restart];
 } else {
  [rows addObject:settings_fixed(settings_text("帧率上限","Frame rate"),
   runtime_frame_rate_active()==120?settings_text("120 帧（启动配置）","120 fps (launch setting)"):runtime_frame_rate_active()==60?settings_text("60 帧（启动配置）","60 fps (launch setting)"):settings_text("30 帧","30 fps"))];
 }
 [rows addObject:settings_fixed(settings_text("补帧（MetalFX）","Frame interpolation (MetalFX)"),settings_text("这个游戏暂不支持","Not available in this game yet"))];
 [rows addObject:settings_language_row()];
 NSUInteger display=rows.count;
 /* Input */
 TOTKSettingRow *pad=settings_row(settings_text("手柄类型","Controller type"),@[settings_pad_auto_text(),@"Xbox",@"PlayStation",@"Switch"],
  @[@(GAMEPAD_TYPE_AUTO),@(GAMEPAD_TYPE_XBOX),@(GAMEPAD_TYPE_PLAYSTATION),@(GAMEPAD_TYPE_SWITCH)],
  ^int{return runtime_gamepad_type(NULL);},^(int v){runtime_gamepad_type_set(v);});
 pad.firstTitle=^NSString *{return settings_pad_auto_text();};[rows addObject:pad];
 [rows addObject:settings_feedback_row(0)];   /* rumble: hid_feedback.c's switch (runtime_feedback_* with 0 is runtime_rumble_*) */
 settings_dsu_rows(rows);
 NSUInteger input=rows.count;
 /* Effects */
 settings_effect_rows(rows);
 for(NSUInteger i=0;i<rows.count;i++)rows[i].page=i<display?SETTINGS_PAGE_DISPLAY:i<input?SETTINGS_PAGE_INPUT:SETTINGS_PAGE_EFFECTS;
 return rows;
}
/* The game actions the key bindings page lists, in its order; action -1 starts a section (SettingsAction,
   SETTINGS_BIND_PANEL: shared/runtime/settings_panel.h). */
static const SettingsAction *settings_actions(int *count) {
 static const SettingsAction list[]={
  {-1,"移动","Movement",NULL},
  {KEYMAP_MOVE_UP,"向前","Forward",NULL},{KEYMAP_MOVE_DOWN,"向后","Back",NULL},
  {KEYMAP_MOVE_LEFT,"向左","Left",NULL},{KEYMAP_MOVE_RIGHT,"向右","Right",NULL},
  {-1,"视角","Camera",NULL},
  {KEYMAP_CAMERA_UP,"视角向上","Camera up",NULL},{KEYMAP_CAMERA_DOWN,"视角向下","Camera down",NULL},
  {KEYMAP_CAMERA_LEFT,"视角向左","Camera left",NULL},{KEYMAP_CAMERA_RIGHT,"视角向右","Camera right",NULL},
  {-1,"动作","Actions",NULL},
  {KEYMAP_A,"交谈 / 使用工具 / 确认","Talk / Use tool / Confirm","A"},
  {KEYMAP_B,"取消 / 跑步（按住）","Cancel / Run (hold)","B"},
  {KEYMAP_X,"口袋","Pockets","X"},
  {KEYMAP_Y,"捡起","Pick up","Y"},
  {KEYMAP_L,"L 键","L button","L"},
  {KEYMAP_R,"R 键","R button","R"},
  {KEYMAP_LSTICK,"按下左摇杆","Left stick press","L3"},
  {KEYMAP_RSTICK,"按下右摇杆","Right stick press","R3"},
  {-1,"菜单","Menus",NULL},
  {KEYMAP_ZL,"狸端（NookPhone）","NookPhone","ZL"},
  {KEYMAP_ZR,"表情","Reactions","ZR"},
  {KEYMAP_PLUS,"+ 键","+ button","+"},
  {KEYMAP_MINUS,"− 键","− button","−"},
  {-1,"工具（十字键）","Tools (d-pad)",NULL},
  {KEYMAP_DUP,"工具环","Tool ring","↑"},{KEYMAP_DDOWN,"收起工具","Put the tool away","↓"},
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
