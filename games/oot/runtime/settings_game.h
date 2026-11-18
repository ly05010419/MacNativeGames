/* Ocarina of Time's part of the settings panel (shared/runtime/settings_panel.h reads it, SETTINGS_GAME_PART 1 and 2).
   Its rows are those of the Tears of the Kingdom runtime it was made from, as they were then (no GPU keep-alive switch,
   edge smoothing a plain switch), with the branches of the other games left out; and its own (the GAME_OOT blocks): the
   display page's resolution is the canvas and the 3D picture together (graphics-canvas.txt, 720p native by default), its
   upscaler, the end-of-frame network at a native resolution and the game's quality profile, all in force at once; with
   no frame mode saved it starts locked at 30. The Effects page has edge smoothing and anisotropic filtering; the rows
   only Tears of the Kingdom 1.4.3 and Breath of the Wild 1.9.0 have show "Not in this version". The GAME_OOT blocks are
   the Mac's (!TARGET_OS_IPHONE; the phone has no panel, ios/Sources/settings_map.h offers the resolution there). */
#if SETTINGS_GAME_PART == 1
/* Its pages besides Display, Input, Key bindings and 着色器贡献: all of them. */
#define SETTINGS_HAS_USERS 1
#define SETTINGS_HAS_EFFECTS 1
#define SETTINGS_HAS_BOARD 1
#define SETTINGS_HAS_SHOTS 1
#define SETTINGS_HAS_VR 1
#include "totk_fps_limit.h"   /* totk_fps_limits: the frame lock choices */
#include "aspect_math.h"     /* aspect_near, ASPECT_GAME */
#include "../zeldaVR/runtime/vr_camera.h"   /* runtime_camera_distance_*: 1.4.3's camera hook, a default elsewhere */
#include "feedback_settings.h"   /* feedback_word: ssao.txt */
#include "users.h"           /* the local users (runtime/account.c): the User page */

void dm_gfx_set_flag(const char *name, int value);
void dm_gfx_ssao_start(int pct); int dm_gfx_ssao_locked(void); int dm_gfx_ssao_live(void); void dm_gfx_ssao_set(int pct);   /* totk_ssao.h */
void dm_gfx_aniso_start(int n); int dm_gfx_aniso_locked(void); int dm_gfx_aniso(void); void dm_gfx_aniso_set(int n);   /* gfx/nvn/nvn_mtl.m */
void nvn_fps_lock_set(int fps);   /* gfx/nvn/hle_nvn.c: the shared totk_fps_limits choices */
int nvn_fps_lock(void);

/* Picking a resolution sizes the window to it at once (user 2026-10-25): 720p 1280x720, 1080p and 720p + MetalFX 1920x1080, MetalFX 2560x1440, native its own size (2K), 2K + MetalFX and native 4K 3840x2160, in the window's points as the first window (frontend.m runtime_front_run), the picture's shape kept
   (the aspect ratio picked) and shrunk to fit the screen's visible area. Full screen stays as it is. */
static void settings_window_size(unsigned w,unsigned h,double ratio) {
 if(!window||(window.styleMask&NSWindowStyleMaskFullScreen))return;
 NSScreen *screen=window.screen?:NSScreen.mainScreen;if(!screen||!(ratio>0)||!w||!h)return;
 NSRect visible=screen.visibleFrame,content=[window contentRectForFrameRect:visible];
 CGFloat ch=round(fmin(h,fmin(content.size.height,content.size.width/ratio))),cw=round(ch*ratio);
 NSRect old=window.frame,f=[window frameRectForContentRect:NSMakeRect(0,0,cw,ch)];
 f.origin=NSMakePoint(NSMidX(old)-f.size.width/2,NSMidY(old)-f.size.height/2);
 f.origin.x=fmax(NSMinX(visible),fmin(f.origin.x,NSMaxX(visible)-f.size.width));
 f.origin.y=fmax(NSMinY(visible),fmin(f.origin.y,NSMaxY(visible)-f.size.height));
 [window setFrame:f display:YES];
 fprintf(stderr,"[totk] settings: window %.0fx%.0f for render %ux%u\n",cw,ch,w,h);
}
static void settings_window_for(int mode) {
 unsigned w=1920,h=1080;
 if(mode==OUTPUT_MODE_METALFX){w=2560;h=1440;}
 else if(mode==OUTPUT_MODE_NATIVE_METALFX||mode==OUTPUT_MODE_NATIVE_4K||mode==OUTPUT_MODE_NATIVE_4K_METALFX||mode==OUTPUT_MODE_NATIVE_6K){w=3840;h=2160;}
 else if(mode==OUTPUT_MODE_NATIVE_5K){w=5120;h=2880;}
 else if(mode==OUTPUT_MODE_720P){w=1280;h=720;}
 else if(mode==OUTPUT_MODE_NATIVE&&!runtime_output_native(mode,&w,&h)){w=1920;h=1080;}
 double ratio=runtime_aspect_ratio()>0?runtime_aspect_ratio():(double)w/h;
 settings_window_size(w,h,ratio);
}

/* Settings live alongside controls.txt. Restore the graphics switches before the guest starts; output and aspect
   patches become available later, so restore those on the main thread when the guest has installed them. */
static int settingsPendingOutput = -1;
#if defined(GAME_OOT) && !TARGET_OS_IPHONE
static int settingsOotCanvas;   /* the resolution row: index in settings_oot_canvases */
static const struct { unsigned w, h; } settings_oot_canvases[] = {{1280,720},{1280,720},{1920,1080},{1920,1080},{2560,1440},{2560,1440},{3840,2160},{3840,2160},{1280,720},{0,0}};   /* the canvas of each runtime_oot_canvas_set index */
extern void runtime_oot_canvas_set(int index, int live);
/* The window is the size of the picture (user 2026-11-09): the canvas. */
static void settings_oot_window_sync(void) {
 unsigned w=settings_oot_canvases[settingsOotCanvas].w;
 if(!w)w=runtime_handheld_mode()?1920:2048;
 settings_window_size(w,w*9/16,runtime_aspect_ratio()>0?runtime_aspect_ratio():16.0/9);
}
#endif
static float settingsPendingAspect;
static NSTimer *settingsRestoreTimer;
#if defined(GAME_OOT) && !TARGET_OS_IPHONE
/* The saved resolution choice (graphics-canvas.txt), or the default. Also the size the window opens at (frontend.m): it
   used to open at 1920x1080 and take the choice's size 1.5 s later, larger first and then smaller once 720p became the
   default (user 2026-11-10: "窗口 为什么 先变大 后变小？"). */
static int settings_oot_canvas_saved(void){int v;return settings_read_int("graphics-canvas.txt",&v)&&((v>=0&&v<6)||v==8)?v==8?0:v:1;}
#endif
static void settings_save_frame(void) {
 char word[32];int fps=nvn_fps_lock(),mfx=mfx_requested();
 if(fps)snprintf(word,sizeof word,"lock-%d",fps);
 else if(mfx)snprintf(word,sizeof word,"interp-%d",mfx);
 else snprintf(word,sizeof word,"off");
 runtime_setting_save("graphics-frame.txt",word,"frame mode");
}
/* Ambient occlusion (PR #1316): ssao.txt holds off or the strength and radius in percent, read before the guest starts
   because the shaders are patched as they load. The levels the row offers; a hand-written percent is kept too. */
static int settingsSsao;   /* the saved choice: 0 off, else percent */
static void settings_ssao_restore(void) {
 char word[32];if(!runtime_setting_word("ssao.txt",word,sizeof word))return;
 const char *w;size_t n=feedback_word(word,&w);char *end;long pct=strtol(w,&end,10);
 if(n==3&&!strncmp(w,"off",3))pct=0;
 else if(end!=w+n||pct<25||pct>400){fprintf(stderr,"[totk] ssao.txt: not off or a percent in 25..400; off\n");return;}
#ifdef SETTINGS_SSAO
 settingsSsao=(int)pct;dm_gfx_ssao_start(settingsSsao);
#endif
}
/* The effects page: graphics-effects.txt holds the ids of the effects that are off, one word each ("none" when all are
   on); graphics-lod.txt the model detail distance x100, graphics-far.txt the render distance in percent (0.1 to 100). */
static void settings_effects_restore(void) {}
static void settings_restore_begin(void) {
 int v;char word[32];
 if(settings_read_int("graphics-overlay.txt",&v)&&v<=HUD_MODE_BARE)fps_overlay_set_hud_mode(v);
 if(!getenv("TOTK_MFX")&&runtime_setting_word("graphics-frame.txt",word,sizeof word)){
  int n=0;char tail=0;
  if(sscanf(word,"lock-%d %c",&n,&tail)==1&&(n>0&&totk_fps_limit_valid(n))){dm_gfx_set_flag("mfx_interp",MFX_OFF);nvn_fps_lock_set(n);}
  else if(sscanf(word,"interp-%d %c",&n,&tail)==1&&n>=MFX_30&&n<MFX_NMODES){nvn_fps_lock_set(0);dm_gfx_set_flag("mfx_interp",n);}
  else if(!strcmp(word,"off")||!strcmp(word,"off\n")){nvn_fps_lock_set(0);dm_gfx_set_flag("mfx_interp",MFX_OFF);}
 }
#if defined(GAME_OOT) && !TARGET_OS_IPHONE
 else if(!getenv("TOTK_MFX"))nvn_fps_lock_set(30);   /* no frame mode saved: locked at 30 (user 2026-11-10: "游戏 默认 锁30 启动"); the panel's "off" is saved as a choice and stays off */
#endif
#if defined(GAME_OOT) && !TARGET_OS_IPHONE
 {extern void netbp_set_flag(int);if(!getenv("OOT_NET_BYPASS")&&settings_read_int("graphics-net-bypass.txt",&v)&&(v==0||v==1))netbp_set_flag(v);}
 {extern void runtime_operation_mode_choose(int,int);if(!getenv("TOTK_OPERATION_MODE")&&!(settings_read_int("graphics-profile.txt",&v)&&v==0))runtime_operation_mode_choose(0,0);}   /* no choice saved: the handheld quality profile (user 2026-11-10, after comparing in the panel) */   /* the handheld quality profile, before the guest starts */
 {extern void netbp_copy_set_flag(int);if(!getenv("OOT_NET_COPY")&&settings_read_int("graphics-net-copy.txt",&v)&&(v==0||v==1))netbp_copy_set_flag(v);}
 {int c=settings_oot_canvas_saved();   /* no choice saved: 1, 720p native (user 2026-11-10: "默认 用 掌机， 跳过 收尾网络 720原生"; it was 2, 1080p from 720p, since 2026-11-09); 6, 7: a 4K canvas, not offered until the game has the memory for it; 9: no longer listed */
 settingsOotCanvas=c;runtime_oot_canvas_set(c,0);dispatch_after(dispatch_time(DISPATCH_TIME_NOW,(int64_t)(1.5*NSEC_PER_SEC)),dispatch_get_main_queue(),^{settings_oot_window_sync();});}
#endif
 if(!getenv("TOTK_TAA")&&settings_read_int("graphics-taa.txt",&v)&&(v==0||v==1))taa_set(v);
 settings_ssao_restore();
 if(!dm_gfx_aniso_locked()&&settings_read_int("graphics-aniso.txt",&v)&&v<=16)dm_gfx_aniso_start(v);
 settings_effects_restore();
 const char *output=getenv("TOTK_OUTPUT_RES");
 if((!output||!*output||!strcmp(output,"auto"))&&!getenv("TOTK_MFX_OUTPUT")&&!getenv("TOTK_MFX_UPSCALE")&&
    settings_read_int("graphics-output.txt",&v)&&v<=OUTPUT_MODE_LAST)settingsPendingOutput=v;
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
#define SETTINGS_HDR_ROWS 2   /* the effects page's HDR rows on 1.4.3 when HDR is available (settings_hdr_rows; PR #1129) */
/* VR settings (zeldaVR; user 2026-10-26: "放在一个新设置 叫 vr设置里 可以选这几种分辨率"), shown while VR is on
   (TOTK_VR_CONTROL): the per-eye size the streamer sends to the headset. Picking one restarts the streamer the game
   runs (TOTK_VR_STREAMER) and the headset reconnects in a few seconds. Measured on a Quest 3: 1344x1408 and 1680x1760
   hold 72 Hz (about 81 ms); 1920x2048 is sharper at about 60 Hz and 120 ms; 2048x2208 (user 2026-10-26: "增加 2048×2208")
   the sharpest, its H.264 encode takes about 41 ms a frame (vr开发.md). */
int vr_enabled(void);
int vr_streamer_managed(void);
void vr_resolution(int *w, int *h);
int vr_set_resolution(int w, int h);
int vr_first_person(void);
void vr_set_first_person(int on);
int vr_offset_cm(int axis);
void vr_set_offset_cm(int axis, int cm);
int vr_mirror_mode(void);
void vr_set_mirror_mode(int on);
int vr_quest_view(void);
void vr_set_quest_view(int on);
int vr_quest_zoom(void);
void vr_set_quest_zoom(int x);
int vr_eye_spring_ms(void);
void vr_set_eye_spring_ms(int ms);
int vr_afr(void);
void vr_set_afr(int on);
int vr_ipd_pct(void);
void vr_set_ipd_pct(int pct);
int vr_6dof(void);
void vr_set_6dof(int on);
int vr_snap_ms(void);
void vr_set_snap_ms(int ms);
int vr_motion(void);
void vr_set_motion(int on);
/* Third-person camera distance (zeldaVR, user 2026-10-26: "在第三人称 时候 调整 摄像机距离 ... 在设置里 我可以设置"):
   1.4.3's camera hook (game_patches_v143.c) moves the camera along its view; with VR and without. Other versions have
   no hook: the row is left out (runtime_camera_distance_available, ../zeldaVR/runtime/vr_camera.h). */
/* Link's head (zeldaVR, user 2026-10-26: "第一人称 我希望 去掉头", "先做到 第三人称里 头是去掉 或者隐藏 这样方便调试"): the
   graphics layer leaves the head's meshes out of the picture, not out of the shadows (nvn_mtl.m vr_head_mesh). Auto:
   while VR's first person puts the camera in his head. */
int vr_hide_head_mode(void) __attribute__((weak_import));
void vr_set_hide_head_mode(int m) __attribute__((weak_import));
static void settings_camera_rows(NSMutableArray<TOTKSettingRow *> *rows) {
 if(vr_hide_head_mode&&vr_set_hide_head_mode)
  [rows addObject:settings_row(settings_text("林克的头","Link's head"),
   @[settings_text("自动（第一人称时隐藏）","Auto (hidden in first person)"),settings_text("隐藏","Hidden"),settings_text("显示","Shown")],@[@-1,@1,@0],
   ^int{return vr_hide_head_mode();},^(int v){vr_set_hide_head_mode(v);})];
 if(!runtime_camera_distance_available())return;
 [rows addObject:settings_row(settings_text("摄像机距离（第三人称）","Camera distance (third person)"),
  @[settings_text("默认","Default"),settings_text("近 1 米","1 m closer"),settings_text("近 2 米","2 m closer"),settings_text("近 3 米","3 m closer"),
    settings_text("近 4 米","4 m closer"),settings_text("近 5 米","5 m closer")],   /* user 2026-10-26: "改成 近1m 近2m 近3m 近4m 近5m" */
  @[@0,@-100,@-200,@-300,@-400,@-500],^int{return runtime_camera_distance_cm();},^(int v){runtime_camera_distance_set(v);})];
}
static void settings_vr_rows(NSMutableArray<TOTKSettingRow *> *rows) {
 if(!vr_enabled())return;
 [rows addObject:settings_row(settings_text("VR 设置 · 视角","VR · view"),@[settings_text("第一人称","First person"),settings_text("第三人称","Third person")],@[@1,@0],
  ^int{return vr_first_person();},^(int v){vr_set_first_person(v);})];
 /* the camera's spring on the head, ms (user 2026-11-02: "用 弹性 解决" the shaking; 0: bound rigidly) */
 {TOTKSettingRow *r=settings_row(settings_text("第一人称镜头弹性（毫秒，0 = 绑死）","First person camera spring (ms, 0 = rigid)"),nil,nil,^int{return vr_eye_spring_ms();},^(int v){vr_set_eye_spring_ms(v);});
  r.sliderMin=0;r.sliderMax=300;[rows addObject:r];}
 /* The camera's position (user 2026-10-29: "可以改变 摄像头 上下前后左右 位置 ，方便我 实时调整"): -100..100 cm each, from where
    the game's camera faces, applied while dragging (totk_vr.m vr_set_offset_cm, kept in vr-offset.txt). */
 NSArray<NSString *> *axes=@[settings_text("摄像头 上下（厘米）","Camera up / down (cm)"),settings_text("摄像头 前后（厘米）","Camera forward / back (cm)"),
  settings_text("摄像头 左右（厘米）","Camera right / left (cm)")];
 for(int a=0;a<3;a++){TOTKSettingRow *r=settings_row(axes[(NSUInteger)a],nil,nil,^int{return vr_offset_cm(a);},^(int v){vr_set_offset_cm(a,v);});
  r.sliderMin=-100;r.sliderMax=100;[rows addObject:r];}
 /* What the Mac window shows (user 2026-10-29: "屏幕 显示 类似vr 那种 两个屏幕"): both eyes as the headset gets them, or the game's picture. */
 /* "显示 Quest 画面" (user 2026-10-30: "桌面显示 和 quest里面 一样的画面 设置里加一个 按钮 叫 显示quest画面"): the Mac window shows
    both eyes; on, each inside its lens opening as in the headset; off, uncovered ("就是 不遮挡状态"). */
 [rows addObject:settings_row(settings_text("显示 Quest 画面","Show the Quest view"),nil,nil,^int{return vr_quest_view();},^(int v){vr_set_quest_view(v);})];
 /* ... each eye's middle 2..10 times larger then (user 2026-10-30: "设置里可以调 2倍 到 10 倍") */
 {TOTKSettingRow *r=settings_row(settings_text("Quest 画面放大（倍）","Quest view zoom (x)"),nil,nil,^int{return vr_quest_zoom();},^(int v){vr_set_quest_zoom(v);});
  r.sliderMin=2;r.sliderMax=10;[rows addObject:r];}
 /* The head's position (6DoF) and snap turning ("按建议 都做", 2026-10-30, after BotW BetterVR and UEVR). */
 [rows addObject:settings_row(settings_text("头部位置（6DoF：探身、下蹲）","Head position (6DoF: lean, crouch)"),nil,nil,^int{return vr_6dof();},^(int v){vr_set_6dof(v);})];
 [rows addObject:settings_row(settings_text("立体（左右眼轮流渲染）","Stereo (the eyes rendered in turn)"),nil,nil,^int{return vr_afr();},^(int v){vr_set_afr(v);})];
 {TOTKSettingRow *r=settings_row(settings_text("立体 · 两眼间距（%）","Stereo · eye distance (%)"),nil,nil,^int{return vr_ipd_pct();},^(int v){vr_set_ipd_pct(v);});
  r.sliderMin=30;r.sliderMax=200;[rows addObject:r];}
 [rows addObject:settings_row(settings_text("体感操作（挥剑、举盾、拉弓、手势）","Motion controls (swing, shield, bow, gestures)"),nil,nil,^int{return vr_motion();},^(int v){vr_set_motion(v);})];
 [rows addObject:settings_row(settings_text("转身方式","Turning"),@[settings_text("平滑","Smooth"),settings_text("分段（小）","Snap (small)"),settings_text("分段（中）","Snap (medium)"),settings_text("分段（大）","Snap (large)")],
  @[@0,@120,@200,@300],^int{return vr_snap_ms();},^(int v){vr_set_snap_ms(v);})];
 static const int sizes[][2]={{1344,1408},{1680,1760},{1920,2048},{2048,2208}};
 NSArray<NSString *> *names=@[settings_text("1344×1408（流畅，72 帧）","1344×1408 (smooth, 72 Hz)"),
  settings_text("1680×1760（推荐，72 帧）","1680×1760 (recommended, 72 Hz)"),
  settings_text("1920×2048（更清楚，约 60 帧）","1920×2048 (sharper, about 60 Hz)"),
  settings_text("2048×2208（最清楚，约 54 帧）","2048×2208 (sharpest, about 54 Hz)")];
 NSString *title=settings_text("VR 设置 · 分辨率（每眼）","VR · resolution (per eye)");
 if(!vr_streamer_managed()){int w=0,h=0;vr_resolution(&w,&h);
  [rows addObject:settings_fixed(title,w>0?[NSString stringWithFormat:@"%d×%d",w,h]:settings_text("未连接","not connected"))];return;}
 [rows addObject:settings_row(title,names,@[@0,@1,@2,@3],^int{int w=0,h=0;vr_resolution(&w,&h);
   for(int i=0;i<4;i++)if(sizes[i][0]==w&&sizes[i][1]==h)return i;return -1;},
  ^(int v){if(v>=0&&v<4)vr_set_resolution(sizes[v][0],sizes[v][1]);})];
}

/* Ambient occlusion: off, or the shadow pre-pass's AO with strength and radius both scaled (1.5x is the PR's tested
   level), applied at once. A run whose shaders carry no parameters (TOTK_SSAO=0) shows its one value. */
static TOTKSettingRow *settings_ssao_row(void) {
 NSString *t=settings_text("环境光遮蔽增强","Stronger ambient occlusion");
#ifndef SETTINGS_SSAO
 return settings_fixed(t,settings_text("这个版本没有","Not in this version"));
#else
 if(dm_gfx_ssao_locked())return settings_fixed(t,settings_text("由 TOTK_SSAO 设置","Set by TOTK_SSAO"));
 if(!dm_gfx_ssao_live())return settings_fixed(t,settings_text("关（游戏原样）","Off (as the game has it)"));
 NSMutableArray<NSNumber *> *values=[@[@0,@150,@200,@300] mutableCopy];
 if(![values containsObject:@(settingsSsao)]){[values addObject:@(settingsSsao)];[values sortUsingSelector:@selector(compare:)];}
 NSMutableArray<NSString *> *choices=[NSMutableArray array];
 for(NSNumber *v in values)[choices addObject:v.intValue?[NSString stringWithFormat:@"%g×",v.intValue/100.0]:settings_text("关（游戏原样）","Off (as the game has it)")];
 return settings_row(t,choices,values,^int{return settingsSsao;},^(int v){
  settingsSsao=v;dm_gfx_ssao_set(v);
  if(v)settings_save_int("ssao.txt",v,"ambient occlusion");else runtime_setting_save("ssao.txt","off","ambient occlusion");});
#endif
}
/* The effects page: the two distances (1.4.3), then a switch per effect (a greyed "not in this version" when the game's
   shaders are not the ones the switches know). */
static TOTKSettingRow *settings_botw_shadow_row(void);   /* below: Breath of the Wild 1.9.0's, greyed elsewhere */
static TOTKSettingRow *settings_botw_fov_row(void);      /* below: Breath of the Wild 1.9.0's, greyed elsewhere */
#if defined(GAME_OOT) && !TARGET_OS_IPHONE
/* The upscaler at the end of the frame: the game's own network or MetalFX (oot_net_bypass.inc). Takes effect on the next frame. */
extern int netbp_flag(void); extern void netbp_set_flag(int);
static TOTKSettingRow *settings_oot_upscaler_row(void) {
 return settings_row(settings_text("放大方式","Upscaler"),@[settings_text("游戏自带","Game's own"),@"MetalFX"],@[@0,@1],
  ^int{return netbp_flag();},^(int v){netbp_set_flag(v);settings_save_int("graphics-net-bypass.txt",v,"upscaler");});
}
#endif
#if defined(GAME_OOT) && !TARGET_OS_IPHONE
/* At a native resolution the game's end-of-frame network still runs, 1:1 (about 2 ms a frame with its two preparing
   dispatches); "skip" copies the picture to the canvas instead (oot_net_bypass.inc, net_copy). Takes effect on the next
   frame. Measured 2026-11-10 at native 1080p: 44 -> 48 fps. The user compares the two pictures here. */
extern int netbp_copy_flag(void); extern void netbp_copy_set_flag(int);
static TOTKSettingRow *settings_oot_net_copy_row(void) {
 return settings_row(settings_text("原生分辨率时的收尾网络","End-of-frame network at native resolution"),@[settings_text("运行（游戏原样）","Runs (as the game does)"),settings_text("跳过（更快，默认）","Skipped (faster, default)")],@[@0,@1],
  ^int{return netbp_copy_flag();},^(int v){netbp_copy_set_flag(v);settings_save_int("graphics-net-copy.txt",v,"network at native resolution");});
}
/* The game's own quality profile: told it is docked or handheld (runtime/services.c). Handheld draws less vegetation
   (grass 500 threadgroups a draw against 1,231 docked); measured 2026-11-10 at native 1080p, switched in one game (n90i):
   44 -> 52 fps. In force at once (the game gets the "operation mode changed" notification a Switch sends when it is taken
   off the dock); the resolution stays the row above's. Saved in graphics-profile.txt; TOTK_OPERATION_MODE in the
   environment or the launcher's default (the mode of the start, with the handheld controller) is left alone. */
extern void runtime_operation_mode_choose(int docked,int notify); extern int runtime_operation_mode_docked(void);
static TOTKSettingRow *settings_oot_profile_row(void) {
 return settings_row(settings_text("画质档位","Quality profile"),@[settings_text("底座","Docked"),settings_text("掌机（植被较少，更快，默认）","Handheld (less vegetation, faster, default)")],@[@0,@1],
  ^int{return !runtime_operation_mode_docked();},^(int v){runtime_operation_mode_choose(!v,1);settings_save_int("graphics-profile.txt",v,"quality profile");});
}
#endif
#if defined(GAME_OOT) && !TARGET_OS_IPHONE
/* The resolution row: the canvas and the 3D picture together, in force at once. */
static TOTKSettingRow *settings_oot_output_row(void) {
 return settings_row(settings_text("分辨率","Resolution"),
  @[settings_text("720p（480p 放大）","720p (from 480p)"),settings_text("720p（原生，默认）","720p (native, default)"),settings_text("1080p（720p 放大）","1080p (from 720p)"),settings_text("1080p（原生）","1080p (native)"),settings_text("2K（1080p 放大）","2K (from 1080p)"),settings_text("2K（2560×1440 原生）","2K (2560×1440 native)")],
  @[@0,@1,@2,@3,@4,@5],^int{return settingsOotCanvas;},^(int v){
   settingsOotCanvas=v;runtime_oot_canvas_set(v,1);settings_save_int("graphics-canvas.txt",v,"resolution");dispatch_async(dispatch_get_main_queue(),^{settings_oot_window_sync();});});
}
#endif
static NSArray<TOTKSettingRow *> *settings_effect_rows(void) {   /* other versions: the page says so */
 NSString *none=settings_text("这个版本没有","Not in this version");
 NSArray<TOTKSettingRow *> *rows=@[settings_botw_shadow_row(),settings_fixed(settings_text("景深","Depth of field"),none),settings_botw_fov_row(),settings_fixed(settings_text("FXAA 抗锯齿","FXAA anti-aliasing"),none),settings_fixed(settings_text("锐化","Sharpening"),none),settings_fixed(settings_text("模型细节距离（LOD）","Model detail distance (LOD)"),none),settings_fixed(settings_text("渲染距离","Render distance"),none),
          settings_fixed(settings_text("效果开关","Effect switches"),none)];
 return rows;
}
/* Anisotropic filtering: off (the game's 1x) to 16x for the material textures, in force at once (dm_gfx_aniso_set makes
   the game's samplers again with the new level). */
static TOTKSettingRow *settings_aniso_row(void) {
 NSString *t=settings_text("各向异性过滤","Anisotropic filtering");
 NSString *(^text)(int)=^NSString *(int v){return v<=1?settings_text("关（游戏原样）","Off (as the game has it)"):[NSString stringWithFormat:@"%d×",v];};
 if(dm_gfx_aniso_locked())return settings_fixed(t,text(dm_gfx_aniso()));
 return settings_row(t,@[text(0),text(2),text(4),text(8),text(16)],@[@0,@2,@4,@8,@16],^int{return dm_gfx_aniso()<=1?0:dm_gfx_aniso();},^(int v){
  dm_gfx_aniso_set(v);settings_save_int("graphics-aniso.txt",v,"anisotropic filtering");});
}
/* The effects page's "Reset to defaults" (user 2026-10-30): every row of the page back to the game's own, its file removed;
   a row the environment decides stays. */
static void settings_effects_reset(void) {
 if(!getenv("TOTK_TAA")){taa_set(0);runtime_setting_remove("graphics-taa.txt");}
#ifdef SETTINGS_SSAO
 if(!dm_gfx_ssao_locked()&&dm_gfx_ssao_live()){settingsSsao=0;dm_gfx_ssao_set(0);runtime_setting_remove("ssao.txt");}
#endif
 if(!dm_gfx_aniso_locked()){dm_gfx_aniso_set(0);runtime_setting_remove("graphics-aniso.txt");}
 fprintf(stderr,"[totk] settings: the effects page back to the defaults\n");
}
/* Sun shadows (Breath of the Wild 1.9.0, botw_shadow_res.h): the game's sizes or 2048 per cascade. The game sizes its map
   once, at the start, so the row shows and saves the next launch's choice, never this run's. Where the patch does not act
   (the iOS export's run) the row offers nothing to save. On the effects page of the other versions (TOTK 1.4.3 has its own). */
static TOTKSettingRow *settings_botw_shadow_row(void) {
 NSString *t=settings_text("阴影分辨率","Shadow resolution");
 return settings_fixed(t,settings_text("这个版本没有","Not in this version"));
}
/* The field of view (Breath of the Wild 1.9.0, botw_fov.h): as 1.4.3's row, in hundredths of the game's own, at once; from
   1x up only (1x to 1.5x is the useful range, PR #1575). A file or TOTK_FOV_SCALE value outside it is still shown. */
static TOTKSettingRow *settings_botw_fov_row(void) {
 NSString *t=settings_text("视野（FOV）","Field of view (FOV)");
 return settings_fixed(t,settings_text("这个版本没有","Not in this version"));
}

static NSArray<TOTKSettingRow *> *settings_rows(void) {
 NSMutableArray<TOTKSettingRow *> *rows=[NSMutableArray array];
 /* Display */
 [rows addObject:settings_row(settings_text("性能信息","Overlay"),
  @[settings_text("全部（FPS、着色器、读取、游玩时间）","All (FPS, shaders, load, playtime)"),
    settings_text("FPS 全信息（另加卡顿、着色器库、管线、存档没命中）","FPS full information (also hitches, libraries, pipelines, archive misses)"),   // user 2026-11-16
    settings_text("FPS 与游玩时间","FPS + playtime"),
    settings_text("极简 FPS（只有 FPS 和分辨率，放大 3 倍）","Bare FPS (FPS and resolution only, 3x the size)"),settings_on_off(0)],   // user 2026-11-18
  @[@(HUD_MODE_ALL),@(HUD_MODE_FULL),@(HUD_MODE_MINIMAL),@(HUD_MODE_BARE),@(HUD_MODE_OFF)],
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
  @[settings_on_off(0),@"30+30",@"40+40",@"50+50",@"60+60",settings_text("不限帧","Unlimited")],
  @[@(MFX_OFF),@(MFX_30),@(MFX_40),@(MFX_50),@(MFX_60),@(MFX_MAX)],
  ^int{return mfx_requested();},^(int v){dm_gfx_set_flag("mfx_interp",v);if(v!=MFX_OFF)nvn_fps_lock_set(0);settings_save_frame();
   if(v!=MFX_OFF&&front_hdr_enabled())runtime_front_hdr_set(0);})];   /* interpolated frames are SDR: HDR goes off (PR #1129) */
 int modes=runtime_output_modes();
 NSMutableArray<NSString *> *res=[NSMutableArray array];NSMutableArray<NSNumber *> *resModes=[NSMutableArray array];
 static const int order[]={OUTPUT_MODE_720P,OUTPUT_MODE_720P_METALFX,OUTPUT_MODE_OFF,OUTPUT_MODE_METALFX,OUTPUT_MODE_NATIVE,OUTPUT_MODE_NATIVE_METALFX,OUTPUT_MODE_NATIVE_4K,OUTPUT_MODE_NATIVE_5K,
  OUTPUT_MODE_NATIVE_4K_METALFX,OUTPUT_MODE_NATIVE_6K};   /* the last two: VR only (game_patches_v143.c vr_big_mode) */
 for(unsigned i=0;i<sizeof order/sizeof *order;i++){
  int m=order[i];if(!(modes&1<<m))continue;unsigned nw=0,nh=0;runtime_output_native(m,&nw,&nh);
  [res addObject:m==OUTPUT_MODE_720P?@"720p":m==OUTPUT_MODE_720P_METALFX?settings_text("720p + MetalFX 放大","720p + MetalFX upscaling"):m==OUTPUT_MODE_OFF?@"1080p":m==OUTPUT_MODE_METALFX?settings_text("1080p + MetalFX 放大","1080p + MetalFX upscaling"):
   m==OUTPUT_MODE_NATIVE_METALFX?settings_text("2K + MetalFX 放大（4K 屏）","2K + MetalFX upscaling (4K screens)"):
   m==OUTPUT_MODE_NATIVE_5K?settings_text("原生 5K（5120×2880）","Native 5K (5120×2880)"):
   m==OUTPUT_MODE_NATIVE_4K?settings_text("原生 4K（3840×2160）","Native 4K (3840×2160)"):
   m==OUTPUT_MODE_NATIVE_4K_METALFX?settings_text("4K + MetalFX 8K","4K + MetalFX 8K"):
   m==OUTPUT_MODE_NATIVE_6K?settings_text("原生 6K（约 20 帧）","Native 6K (about 20 fps)"):
   nw==2560&&nh==1440?settings_text("2K（2560×1440）","2K (2560×1440)"):
   [NSString stringWithFormat:@(TOTK_UI("原生 %u×%u","Native %u×%u")),nw,nh]];
  [resModes addObject:@(m)];}
 NSString *resolution=settings_text("分辨率","Resolution");
#if   defined(GAME_OOT) && !TARGET_OS_IPHONE
 (void)resolution;   /* every resolution setting of this game is on this page (user 2026-11-09) */
 [rows addObject:settings_oot_output_row()];[rows addObject:settings_oot_upscaler_row()];[rows addObject:settings_oot_net_copy_row()];[rows addObject:settings_oot_profile_row()];
#else
 if(res.count<2)[rows addObject:settings_fixed(resolution,@"1080p")];
 else [rows addObject:settings_row(resolution,res,resModes,
  ^int{return runtime_output_mode();},^(int v){settingsPendingOutput=-1;if(runtime_output_mode_set(v)){settings_window_for(v);settings_save_int("graphics-output.txt",v,"resolution");}})];
#endif
 NSString *aspect=settings_text("画面比例","Aspect ratio");
 if(!(runtime_aspect_ratio()>0))[rows addObject:settings_fixed(aspect,@"16:9")];
 else{NSMutableArray<NSNumber *> *ratios=[@[@(16.0/9),@(16.0/10),@(21.0/9),@(32.0/9)] mutableCopy];
  float start=runtime_aspect_start();
  BOOL listed=NO;   /* Include the game's custom ratio as one more choice. */
  for(NSNumber *r in ratios)listed|=aspect_near(start,r.doubleValue);
  if(start>0&&!listed)[ratios addObject:@(start)];
  NSMutableArray<NSString *> *choices=[NSMutableArray array];NSMutableArray<NSNumber *> *values=[NSMutableArray array];
  for(NSUInteger i=0;i<ratios.count;i++){[choices addObject:settings_ratio_text(ratios[i].floatValue)];[values addObject:@((int)i)];}
  [rows addObject:settings_row(aspect,choices,values,^int{
    float now=runtime_aspect_ratio();for(NSUInteger i=0;i<ratios.count;i++)if(aspect_near(now,ratios[i].doubleValue))return (int)i;return -1;},
   ^(int v){float r=ratios[(NSUInteger)v].floatValue;settingsPendingAspect=0;if(runtime_aspect_set_ratio(r)){
    runtime_front_resize_aspect(r);char word[32];snprintf(word,sizeof word,"%.8g",r);runtime_setting_save("graphics-aspect.txt",word,"aspect ratio");
   }})];}
 NSUInteger display=rows.count;
 /* Input */
 TOTKSettingRow *pad=settings_row(settings_text("手柄类型","Controller type"),@[settings_pad_auto_text(),@"Xbox",@"PlayStation",@"Switch"],
  @[@(GAMEPAD_TYPE_AUTO),@(GAMEPAD_TYPE_XBOX),@(GAMEPAD_TYPE_PLAYSTATION),@(GAMEPAD_TYPE_SWITCH)],
  ^int{return runtime_gamepad_type(NULL);},^(int v){runtime_gamepad_type_set(v);});
 pad.firstTitle=^NSString *{return settings_pad_auto_text();};[rows addObject:pad];
 [rows addObject:settings_row(settings_text("后台手柄输入","Controller input in background"),nil,nil,
  ^int{return runtime_gamepad_background_enabled();},^(int v){runtime_gamepad_background_set(v);})];
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
 NSUInteger input=rows.count;
 /* User (runtime/users.h, docs/users.md): the user this run opened with, and the one the next launch gets — the save
    area is named after the user's id, so another user plays on their own saves. A running game keeps its user. */
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
 /* Effects (user 2026-10-30: edge smoothing, stronger AO and anisotropy moved here from the display page) */
 [rows addObject:settings_row(settings_text("边缘平滑（MetalFX 时间性）","Edge smoothing (MetalFX Temporal)"),nil,nil,
  ^int{return taa_enabled();},^(int v){taa_set(v);settings_save_int("graphics-taa.txt",v,"edge smoothing");})];
 [rows addObject:settings_ssao_row()];
 [rows addObject:settings_aniso_row()];
 [rows addObjectsFromArray:settings_effect_rows()];
 for(NSUInteger i=0;i<rows.count;i++)rows[i].page=i<display?SETTINGS_PAGE_DISPLAY:i<input?SETTINGS_PAGE_INPUT:i<users?SETTINGS_PAGE_USERS:SETTINGS_PAGE_EFFECTS;
 if(vr_enabled()){   /* zeldaVR, its own page: the game's render resolution again (the display page's setting), camera, VR settings */
  NSUInteger vr=rows.count;
  if(res.count>=2)[rows addObject:settings_row(settings_text("游戏渲染分辨率","Game render resolution"),res,resModes,
   ^int{return runtime_output_mode();},^(int v){settingsPendingOutput=-1;if(runtime_output_mode_set(v)){settings_window_for(v);settings_save_int("graphics-output.txt",v,"resolution");}})];
  settings_camera_rows(rows);
  settings_vr_rows(rows);
  for(NSUInteger i=vr;i<rows.count;i++)rows[i].page=SETTINGS_PAGE_VR;
 }
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
  {KEYMAP_A,"确认 / 互动","Confirm / Interact","A"},
  {KEYMAP_B,"取消 / 冲刺","Cancel / Sprint","B"},
  {KEYMAP_X,"跳跃","Jump","X"},
  {KEYMAP_Y,"攻击","Attack","Y"},
  {KEYMAP_ZR,"弓（按住拉弓）","Bow (hold to draw)","ZR"},
  {KEYMAP_ZL,"锁定 / 举盾","Lock on / Shield","ZL"},
  {KEYMAP_L,"能力","Ability","L"},
  {KEYMAP_R,"投掷","Throw","R"},
  {KEYMAP_LSTICK,"蹲下","Crouch","L3"},
  {KEYMAP_RSTICK,"主视角","Look (first person)","R3"},
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
