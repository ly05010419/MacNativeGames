/* Echoes of Wisdom's part of the settings panel (shared/runtime/settings_panel.h reads it, SETTINGS_GAME_PART 1 and 2).
   Until 2026-11-17 this game had no panel: keys 1-4 toggled a reminder box and three shortcuts, of which only key 1 did
   anything here (game_patches.c answered "unavailable" to the 2K and ultrawide keys, and frame interpolation has no hooks
   for this game). It now has the Zelda games' panel and pages, as far as this game supports them (modelled on
   games/acnh's, the other fork of games/by3):
   Display
     overlay              all / FPS full information / FPS + playtime / off   fps_overlay_set_hud_mode (shared fps_overlay.c),
                                                                             graphics-overlay.txt
     FPS graph            switch                         fps-graph.txt
     frame rate           60 fps, greyed out             the game's logic counts frames (README.md: more than 60 is not
                                                         done), so there is nothing to choose
     frame interpolation  not available, greyed out      MetalFX frame interpolation (gfx/nvn/totk_mfx.m) needs the game's
                                                         scene, depth, camera and UI passes; its hooks know Tears of the
                                                         Kingdom's, not this game's
   Input
     controller type      auto / Xbox / PlayStation / Switch   runtime_gamepad_type_set (gamepad.m), controller-type.txt
     rumble               switch                         runtime_feedback_choose (hid_feedback.c), rumble.txt; the game asks
                                                         for its vibration devices at the start, so turned on in a run
                                                         without them it says "from the next launch"
     DSU                  switch, server, slot, status   the shared DSU rows (dsu_settings_rows.h, gamepad.m dsu_client.h)
   Effects
     picture              1080p / 1080p + MetalFX        the window shows the game's picture scaled; MetalFX can upscale it
                          upscaling                      to a larger window or full screen instead of the plain scaling
                                                         (the present's output stage, flags mfx_upscale + mfx_output,
                                                         gfx/nvn/nvn_mtl.m output_upscale), graphics-output.txt;
                                                         TOTK_MFX_UPSCALE / TOTK_MFX_OUTPUT decide first. Reset to
                                                         defaults: plain scaling again (the file removed).
   Key bindings, with the screenshot row (P, or the touchpad held 3 s); 性能排行榜 and 截图分享 (both off by default).
   Left out: the User page (see below), the 着色器贡献 page (this game's shader list grows from a work folder's cache by
   hand, README.md Shaders; no contribution file or upload), VR, the game's language (it follows the Mac: app/launcher.c
   sets TOTK_LOCALE), aspect ratio and render resolution (no patch for them here), gyro aiming (the game reads no motion),
   rumble strength and mouse sensitivity (no such settings here).
   The User page: the shared users (shared/runtime/users.h, account.c) give each user a save area of their own; this game
   keeps the one account it has always had (runtime/account.c, <TOTK_PROFILE_DIR>/account.uid) and its saves under it
   (runtime/fs_host.c profile/save/<uid>), which the app's launcher backs up before each start. Moving those saves under a
   users list is a migration of the player's data that has not been tried on a real profile; until it is, the saves stay
   exactly where they are. The game has its own save files inside that one save area. */
#if SETTINGS_GAME_PART == 1
#define SETTINGS_HAS_EFFECTS 1
#define SETTINGS_HAS_BOARD 1
#define SETTINGS_HAS_SHOTS 1
#define SETTINGS_HAS_CONTRIB 0
#include "../third_party/d2r_gfx/gfx/nvn/totk_mfx_up.h"   /* mfxup_*: MetalFX upscaling of the picture */

/* The picture: 1 MetalFX upscales it (both flags: the game's own upscale stage, which has no hook in this game, and the
   window's output), 0 plain scaling. */
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
 fprintf(stderr,"[eow] settings: window %.0fx%.0f points for %s\n",w,h,on?"1080p + MetalFX (2560x1440)":"1080p");
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

/* The DSU rows of the input page (runtime/dsu_client.h in gamepad.m): a server to read a controller's motion and buttons from. */
#include "dsu_settings_rows.h"
/* The effects page: this game's picture row. */
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
 fprintf(stderr,"[eow] settings: the effects page back to the defaults\n");
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
 [rows addObject:settings_fixed(settings_text("帧率上限","Frame rate"),settings_text("60 帧（游戏自己的节奏）","60 fps (the game's own pace)"))];
 [rows addObject:settings_fixed(settings_text("补帧（MetalFX）","Frame interpolation (MetalFX)"),settings_text("这个游戏暂不支持","Not available in this game yet"))];
 NSUInteger display=rows.count;
 /* Input */
 TOTKSettingRow *pad=settings_row(settings_text("手柄类型","Controller type"),@[settings_pad_auto_text(),@"Xbox",@"PlayStation",@"Switch"],
  @[@(GAMEPAD_TYPE_AUTO),@(GAMEPAD_TYPE_XBOX),@(GAMEPAD_TYPE_PLAYSTATION),@(GAMEPAD_TYPE_SWITCH)],
  ^int{return runtime_gamepad_type(NULL);},^(int v){runtime_gamepad_type_set(v);});
 pad.firstTitle=^NSString *{return settings_pad_auto_text();};[rows addObject:pad];
 [rows addObject:settings_feedback_row(0)];   /* rumble: hid_feedback.c's switch (runtime_feedback_* with 0) */
 settings_dsu_rows(rows);
 NSUInteger input=rows.count;
 /* Effects */
 settings_effect_rows(rows);
 for(NSUInteger i=0;i<rows.count;i++)rows[i].page=i<display?SETTINGS_PAGE_DISPLAY:i<input?SETTINGS_PAGE_INPUT:SETTINGS_PAGE_EFFECTS;
 return rows;
}
/* The game actions the key bindings page lists, in its order; action -1 starts a section (SettingsAction,
   SETTINGS_BIND_PANEL: shared/runtime/settings_panel.h). Named after the Switch buttons: what each does in Echoes of
   Wisdom is not all confirmed in play yet (README.md "Keys"); A confirms and B goes back in the menus. */
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
