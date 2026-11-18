/* Super Mario Odyssey's part of the settings panel (shared/runtime/settings_panel.h reads it, SETTINGS_GAME_PART 1 and 2),
   adapted from the complete TOTK/BOTW panel at d3389b0 and its later revisions. Its pages (2026-11-17: as the Zelda games',
   with the modules they share in shared/runtime) are Display, Input (with the DSU rows), User, Effects, Key bindings (with
   the screenshot), 着色器贡献, 性能排行榜 and 截图分享; the VR page is the Zelda games' own.
     Effects   anti-aliasing, sharpen (CAS), upscaler, texture filtering (graphics.txt, odyssey_gfx.h) and edge smoothing
               (MetalFX temporal, graphics-taa.txt), moved from Display; "Reset to defaults" (settings_effects_reset)
               sets them back to the game's own, a choice the environment made (ODYSSEY_AA ..., TOTK_TAA) staying.
     User      the user this run opened with and the next launch's (runtime/account.c, shared): each has its own save
               area; the profile's account.uid from before the users is the user "Local", whose saves are the ones
               the game used until now.
   Odyssey keeps its 60 Hz game logic and offers its supported 60+60 interpolation, verified scene resolution caps,
   independent drawable resolution, picture filters and camera aspect ratios. Controller bow triggers are omitted
   because this game has no corresponding bow-state reader. Each setting uses its production runtime implementation. */
#if SETTINGS_GAME_PART == 1
/* Its pages besides Display, Input, Key bindings and 着色器贡献 (no VR, no anti-aliasing quality rows). */
#define SETTINGS_HAS_USERS 1
#define SETTINGS_HAS_EFFECTS 1
#define SETTINGS_HAS_BOARD 1
#define SETTINGS_HAS_SHOTS 1
#include "users.h"           /* the local users (runtime/account.c): the User page */
#include "../third_party/d2r_gfx/gfx/nvn/odyssey_graphics.h"
#include "aspect_math.h"     /* aspect_near, ASPECT_GAME */

void dm_gfx_set_flag(const char *name, int value);
/* Odyssey's logic runs at 60 Hz; interpolation adds pictures without changing that clock. */
static NSTimer *settingsRestoreTimer;
static float settingsPendingAspect;
static void settings_save_frame(void) { dm_gfx_opt_set(GX_FRAME, mfx_requested() != MFX_OFF); }
static void settings_restore_begin(void) {
 int v;char word[32];
 if(settings_read_int("graphics-overlay.txt",&v)&&v<=HUD_MODE_BARE)fps_overlay_set_hud_mode(v);
 if(!getenv("TOTK_MFX"))dm_gfx_set_flag("mfx_interp",dm_gfx_opt_get(GX_FRAME)?MFX_HALF:MFX_OFF);
 if(!getenv("TOTK_TAA")&&settings_read_int("graphics-taa.txt",&v)&&(v==0||v==1))taa_set(v);
 if(!getenv("TOTK_ASPECT")&&runtime_setting_word("graphics-aspect.txt",word,sizeof word)){
  char *end;float ratio=strtof(word,&end);while(*end==' '||*end=='\n'||*end=='\r'||*end=='\t')end++;
  if(end!=word&&!*end&&isfinite(ratio)&&ratio>=0.5f&&ratio<=8)settingsPendingAspect=ratio;
 }
}
/* 60+60 needs a 120 Hz screen (user 2026-11-17: "设置没用"; on a 60 Hz one it only adds pictures the screen drops).
   mtl_display_hz is 0 until the window has sampled its screen: unknown counts as able, the check is made again then. */
int mtl_display_hz(void);   /* gfx/nvn/nvn_mtl.m */
static int settings_screen_hz(void) { return mtl_display_hz(); }
static BOOL settings_interp_possible(void) { int hz=settings_screen_hz(); return hz<=0||hz>=120; }
static BOOL settingsInterpChecked;
static void settings_restore_ready(void) {
 if(settingsPendingAspect>0&&runtime_aspect_ratio()>0){
  float r=settingsPendingAspect;settingsPendingAspect=0;
  if(runtime_aspect_set_ratio(r))runtime_front_resize_aspect(r);
 }
 if(!settingsInterpChecked&&settings_screen_hz()>0){   /* the saved choice stays: a 120 Hz screen gets it at the next launch */
  settingsInterpChecked=YES;
  if(!settings_interp_possible()&&mfx_requested()!=MFX_OFF){
   dm_gfx_set_flag("mfx_interp",MFX_OFF);
   fprintf(stderr,"[odyssey] settings: 60+60 off on this %d Hz screen (needs 120 Hz; the saved choice is kept)\n",settings_screen_hz());
  }
 }
 if(settingsPendingAspect<=0&&settingsInterpChecked){[settingsRestoreTimer invalidate];settingsRestoreTimer=nil;}
}
static void settings_restore_start(void) {
 settings_restore_begin();
 /* The shader contribution (totk_contrib_upload.h): this game asks no question at start (its launcher is a plain C
    program); the settings panel's switch decides, and when it is on the rows go in the background while playing. It used
    to be started only when an aspect ratio was waiting to be restored (an early return above it). */
 totk_inbox_start(totk_inbox_claim(totk_contrib_path()),@"mac");   /* this game's file only: claimed before the game writes to it */
 settingsRestoreTimer=[NSTimer timerWithTimeInterval:0.5 repeats:YES block:^(NSTimer *t){(void)t;settings_restore_ready();}];
 [NSRunLoop.mainRunLoop addTimer:settingsRestoreTimer forMode:NSRunLoopCommonModes];
}

/* The version the panel's corner shows: ODYSSEY_GAME_VERSION, the update the app was built from (the app launcher sets
   it; 1.3.0 and 1.4.1 share their executables), before this build's (frontend.m does the same for the window title). */
static const char *settings_odyssey_version(void) {
 const char *version=getenv("ODYSSEY_GAME_VERSION");return version&&*version?version:TOTK_GAME_VERSION;
}
#define SETTINGS_GAME_VERSION settings_odyssey_version()
/* What the shared panel asks of a game that this one does not have: no state reporting (gfx/nvn/totk_state_report.m),
   nothing to tell when the panel opens or closes. */
static void (*const totk_sr_settings)(int open) = NULL;

/* The DSU rows of the input page (runtime/dsu_client.h in gamepad.m): a server to read a controller's motion from. */
#include "dsu_settings_rows.h"
/* The effects page (user 2026-11-17: as the Zelda games): the picture filters of odyssey_gfx.h (graphics.txt) and edge
   smoothing (graphics-taa.txt). */
static void settings_effect_rows(NSMutableArray<TOTKSettingRow *> *rows) {
 [rows addObject:settings_row(settings_text("抗锯齿","Anti-aliasing"),@[@"Off",@"FXAA",@"SMAA"],@[@0,@1,@2],
  ^int{return dm_gfx_opt_get(GX_AA);},^(int v){dm_gfx_opt_set(GX_AA,v);})];
 [rows addObject:settings_row(settings_text("锐化（CAS）","Sharpen (CAS)"),nil,nil,
  ^int{return dm_gfx_opt_get(GX_CAS);},^(int v){dm_gfx_opt_set(GX_CAS,v);})];
 [rows addObject:settings_row(settings_text("放大器","Upscaler"),@[@"MetalFX",@"FSR 1",@"Lanczos",@"Bilinear"],@[@0,@1,@2,@3],
  ^int{return dm_gfx_opt_get(GX_UP);},^(int v){dm_gfx_opt_set(GX_UP,v);})];
 [rows addObject:settings_row(settings_text("纹理过滤","Texture filtering"),@[settings_text("游戏默认","Game default"),@"4×",@"8×",@"16×"],@[@0,@1,@2,@3],
  ^int{return dm_gfx_opt_get(GX_TEX);},^(int v){dm_gfx_opt_set(GX_TEX,v);})];
 [rows addObject:settings_row(settings_text("边缘平滑（MetalFX 时间性）","Edge smoothing (MetalFX Temporal)"),nil,nil,
  ^int{return taa_enabled();},^(int v){taa_set(v);settings_save_int("graphics-taa.txt",v,"edge smoothing");})];
}
/* The effects page's "Reset to defaults" (settings_panel.h resetEffects:): each row back to the game's own (odyssey_gfx.h's
   first choice: off, off, MetalFX, the game's filtering; edge smoothing off), kept in graphics.txt with its other lines
   (the render resolution, frame generation) and graphics-taa.txt removed. A choice the environment made stays, as the
   Zelda games' reset leaves a row the environment decides. */
static void settings_effects_reset(void) {
 const char *post=getenv("ODYSSEY_POST");
 if(!getenv("ODYSSEY_AA")&&!post)dm_gfx_opt_set(GX_AA,GX_AA_OFF);
 if(!getenv("ODYSSEY_CAS")&&!post)dm_gfx_opt_set(GX_CAS,0);
 if(!getenv("ODYSSEY_UPSCALER"))dm_gfx_opt_set(GX_UP,GX_UP_METALFX);
 if(!getenv("ODYSSEY_TEXTURES"))dm_gfx_opt_set(GX_TEX,0);
 if(!getenv("TOTK_TAA")){taa_set(0);runtime_setting_remove("graphics-taa.txt");}
 fprintf(stderr,"[odyssey] settings: the effects page back to the defaults\n");
}

static NSArray<TOTKSettingRow *> *settings_rows(void) {
 NSMutableArray<TOTKSettingRow *> *rows=[NSMutableArray array];
 /* Display */
 [rows addObject:settings_row(settings_text("性能信息","Overlay"),
  @[settings_on_off(0),settings_text("极简 FPS","Bare FPS"),settings_text("FPS 与游玩时间","FPS + playtime"),settings_text("全部","All"),
    settings_text("FPS 和帧调试信息","FPS and frame debug info")],   // user 2026-11-18: no notes in brackets, in reverse, the debug one last
  @[@(HUD_MODE_OFF),@(HUD_MODE_BARE),@(HUD_MODE_MINIMAL),@(HUD_MODE_ALL),@(HUD_MODE_FULL)],
  ^int{return fps_overlay_hud_mode();},^(int v){fps_overlay_set_hud_mode(v);settings_save_int("graphics-overlay.txt",v,"overlay");[(TOTKHudBox *)hudBox refresh];})];
 [rows addObject:settings_row(settings_text("帧率图","FPS graph"),nil,nil,
  ^int{return fpsGraphEnabled;},^(int v){fpsGraphEnabled=v;runtime_setting_save("fps-graph.txt",v?"on":"off","FPS graph");[(TOTKFpsGraph *)fpsGraphBox setEnabled:v];[(TOTKHudBox *)hudBox refresh];})];
 [rows addObject:settings_fixed(settings_text("游戏帧率","Game frame rate"),@"60 fps")];
 if(settings_interp_possible())[rows addObject:settings_row(settings_text("补帧（MetalFX）","Frame interpolation (MetalFX)"),
  @[settings_on_off(0),@"60+60 (120 Hz)"],@[@(MFX_OFF),@(MFX_HALF)],
  ^int{return mfx_requested();},^(int v){dm_gfx_set_flag("mfx_interp",v);settings_save_frame();})];
 else [rows addObject:settings_fixed([NSString stringWithFormat:@(TOTK_UI("补帧（MetalFX）· 这块屏幕 %d Hz，需要 120 Hz","Frame interpolation (MetalFX) · this screen is %d Hz, needs 120 Hz")),settings_screen_hz()],
  settings_on_off(0))];
 NSMutableArray<NSString *> *outputs=[NSMutableArray array];NSMutableArray<NSNumber *> *outputValues=[NSMutableArray array];
 for(unsigned i=0;i<OUTPUT_RESOLUTION_COUNT;i++){
  NSString *label=i==OUTPUT_RESOLUTION_NATIVE?settings_text("原生分辨率（屏幕）","Native (display)"):i==OUTPUT_RESOLUTION_AUTO?settings_text("自动（跟随窗口）","Auto (window)"):
   [NSString stringWithFormat:@"%u × %u",output_resolutions[i].width,output_resolutions[i].height];
  [outputs addObject:label];[outputValues addObject:@(i)];
 }
 [rows addObject:settings_row(settings_text("输出分辨率（缩放）","Output resolution (scaled)"),outputs,outputValues,
  ^int{return front_output_resolution();},^(int v){front_output_resolution_set(v);})];
 unsigned maxW,maxH;runtime_scene_resolution_max(&maxW,&maxH);
 static const unsigned sceneSizes[][2]={{1600,900},{1920,1080},{2240,1260},{2560,1440}};
 NSMutableArray<NSString *> *scenes=[NSMutableArray array];NSMutableArray<NSNumber *> *sceneValues=[NSMutableArray array];
 for(unsigned i=0;i<4;i++)if(sceneSizes[i][0]<=maxW&&sceneSizes[i][1]<=maxH){
  [scenes addObject:[NSString stringWithFormat:@"%u × %u",sceneSizes[i][0],sceneSizes[i][1]]];[sceneValues addObject:@(i)];
 }
 /* Only the sizes this app's canvas holds (build_odyssey.py --render-res, default 2560x1440); a smaller canvas is named */
 /* The game picks its scene size only when it builds a scene (main+0x5efc20's one caller is a constructor, main+0x6f144c):
    a pick counts from the next area, door, pipe or kingdom (user 2026-11-17: "设置没用"). */
 NSString *sceneName=settings_text("渲染分辨率（下个场景生效）","Render resolution (from the next area)");
 NSString *sceneTitle=maxW>=2560?sceneName:
  [NSString stringWithFormat:@"%@ (%@ %u × %u)",sceneName,settings_text("这个包最大","this app: up to"),maxW,maxH];
 [rows addObject:settings_row(sceneTitle,scenes,sceneValues,^int{
  unsigned w,h;runtime_scene_resolution(&w,&h);for(int i=0;i<4;i++)if(sceneSizes[i][0]==w&&sceneSizes[i][1]==h)return i;return 0;
 },^(int v){dm_gfx_opt_set(GX_RES,v);})];
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
 /* The rows the window before the game had (user 2026-11-17: "去掉这个 都放在设置里去设置"); scripts/odyssey_app_launcher.c
    reads their files. Full screen or window: at once, and display.txt is how the next launch starts. */
 [rows addObject:settings_row(settings_text("显示方式","Display"),@[settings_text("全屏","Full screen"),settings_text("窗口","Window")],@[@0,@1],
  ^int{return window&&(window.styleMask&NSWindowStyleMaskFullScreen)?0:1;},
  ^(int v){runtime_setting_save("display.txt",v?"window":"fullscreen","display");
   if(window&&(!!(window.styleMask&NSWindowStyleMaskFullScreen))!=(v==0))[window toggleFullScreen:nil];})];
 /* The session limit (session-minutes.txt, 0 = none; the launcher reads it again while the game runs): the copied
    renderer has caused GPU driver panics in long sessions, so 20 minutes stays the default. */
 [rows addObject:settings_row(settings_text("单次游玩时长","Session limit"),
  @[settings_text("20 分钟","20 minutes"),settings_text("60 分钟","60 minutes"),settings_text("90 分钟","90 minutes"),
    settings_text("120 分钟","120 minutes"),settings_text("不限","No limit")],@[@20,@60,@90,@120,@0],
  ^int{char w[16];return runtime_setting_word("session-minutes.txt",w,sizeof w)?atoi(w):20;},
  ^(int v){char w[16];snprintf(w,sizeof w,"%d",v);runtime_setting_save("session-minutes.txt",w,"session limit");})];
 NSUInteger display=rows.count;
 /* Input */
 TOTKSettingRow *pad=settings_row(settings_text("手柄类型","Controller type"),@[settings_pad_auto_text(),@"Xbox",@"PlayStation",@"Switch"],
  @[@(GAMEPAD_TYPE_AUTO),@(GAMEPAD_TYPE_XBOX),@(GAMEPAD_TYPE_PLAYSTATION),@(GAMEPAD_TYPE_SWITCH)],
  ^int{return runtime_gamepad_type(NULL);},^(int v){runtime_gamepad_type_set(v);});
 pad.firstTitle=^NSString *{return settings_pad_auto_text();};[rows addObject:pad];
 [rows addObject:settings_row(settings_text("鼠标操作（视角、按键）","Mouse (camera, buttons)"),nil,nil,
  ^int{return runtime_mouse_enabled();},^(int v){runtime_mouse_set(v);})];
 [rows addObject:settings_sensitivity_row()];
 [rows addObject:settings_feedback_row(0)];
 [rows addObject:settings_gain_row()];
 [rows addObject:settings_feedback_row(1)];
 /* How A / B / X / Y map (face-buttons.txt, TOTK_FACE_BUTTONS at launch, runtime/gamepad_layout.h). */
 [rows addObject:settings_row(settings_text("手柄 A / B / X / Y（下次启动生效）","Controller A / B / X / Y (next launch)"),
  @[settings_text("按标注（印的字母）","By label (as printed)"),settings_text("按位置","By position")],@[@0,@1],
  ^int{char w[16];return runtime_setting_word("face-buttons.txt",w,sizeof w)&&!strncmp(w,"position",8);},
  ^(int v){runtime_setting_save("face-buttons.txt",v?"position":"label","controller button layout");})];
 settings_dsu_rows(rows);
 NSUInteger input=rows.count;
 /* User (runtime/users.h, docs/users.md; as the Zelda games): the user this run opened with, and the one the next launch
    gets — the save area is named after the user's id, so another user plays on their own saves. A running game keeps its
    user. */
 RuntimeUser usersAll[RUNTIME_USERS_MAX],usersMe,usersNext;
 int usersCount=runtime_users_get(usersAll,RUNTIME_USERS_MAX);
 int usersMeOk=!runtime_users_active(&usersMe);runtime_users_next(&usersNext);
 NSMutableArray<NSString *> *userNames=[NSMutableArray array];NSMutableArray<NSNumber *> *userIndex=[NSMutableArray array];
 int userPicked=0;
 for(int i=0;i<usersCount;i++){
  [userNames addObject:@(usersAll[i].name)];[userIndex addObject:@(i)];
  if(usersAll[i].lo==usersNext.lo&&usersAll[i].hi==usersNext.hi)userPicked=i;
 }
 [rows addObject:settings_fixed(settings_text("本次运行的用户","User this run"),usersMeOk?@(usersMe.name):settings_text("（无）","(none)"))];
 [rows addObject:settings_row(settings_text("用户（下次启动生效）","User (from the next launch)"),userNames,userIndex,
  ^int{return userPicked;},^(int v){runtime_users_choose(v);
   /* the list's order does not change, but the pick does: the row is made again with the new one in force */
   dispatch_async(dispatch_get_main_queue(),^{settings_rebuild();});})];
 NSUInteger users=rows.count;
 /* Effects */
 settings_effect_rows(rows);
 for(NSUInteger i=0;i<rows.count;i++)rows[i].page=i<display?SETTINGS_PAGE_DISPLAY:i<input?SETTINGS_PAGE_INPUT:i<users?SETTINGS_PAGE_USERS:SETTINGS_PAGE_EFFECTS;
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
  {KEYMAP_A,"跳跃 / 确认","Jump / Confirm","A"},
  {KEYMAP_B,"跳跃 / 取消","Jump / Cancel","B"},
  {KEYMAP_X,"投帽","Throw cap","X"},
  {KEYMAP_Y,"投帽","Throw cap","Y"},
  {KEYMAP_ZR,"蹲下 / 下砸","Crouch / Ground pound","ZR"},
  {KEYMAP_ZL,"蹲下 / 下砸","Crouch / Ground pound","ZL"},
#ifdef GAME_BOTW
  {KEYMAP_L,"希卡石能力","Rune","L"},
#else
  {KEYMAP_L,"L 按钮","L button","L"},
#endif
  {KEYMAP_R,"R 按钮","R button","R"},
  {KEYMAP_LSTICK,"左摇杆按下","Left stick press","L3"},
  {KEYMAP_RSTICK,"右摇杆按下","Right stick press","R3"},
  {-1,"菜单","Menus",NULL},
  {KEYMAP_PLUS,"菜单","Menu","+"},
  {KEYMAP_MINUS,"地图","Map","−"},
  {-1,"十字键","D-pad",NULL},
  {KEYMAP_DUP,"十字键 上","D-pad up","↑"},{KEYMAP_DDOWN,"十字键 下","D-pad down","↓"},
  {KEYMAP_DLEFT,"十字键 左","D-pad left","←"},{KEYMAP_DRIGHT,"十字键 右","D-pad right","→"},
  {-1,"设置和截图","Settings & screenshots",NULL},
  {SETTINGS_BIND_PANEL,"打开 / 关闭设置面板","Open / close this panel",NULL},
  {KEYMAP_SCREENSHOT,"截图","Screenshot",NULL}};
 *count=(int)(sizeof list/sizeof *list);
 return list;
}
#endif
