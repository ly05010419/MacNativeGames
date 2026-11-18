/* Tears of the Kingdom's part of the settings panel (shared/runtime/settings_panel.h reads it, SETTINGS_GAME_PART 1 and
   2). The Makefile builds Breath of the Wild, Bananza (GAME_DKB) and Pokopia (GAME_POKOPIA) from this runtime too, hence
   the #ifdef switches. Its pages' rows, one per setting, a pop-up menu, a switch or a slider, each applied at once:
   Display
     overlay              all / FPS + playtime / off               fps_overlay_set_hud_mode (gfx/nvn/fps_overlay.c)
     FPS graph            switch, default on                 fps-graph.txt above the game's folder (both games)
     frame lock           off / 30 / 40 / 60 / 72 / 90 / 120  nvn_fps_lock_set (gfx/nvn/hle_nvn.c; user 2026-10-25 "锁帧")
                                                             frame lock and frame interpolation exclude each other: picking
                                                             one turns the other off (user 2026-10-25, not to mislead)
     frame interpolation  off / 30+30 / 40+40 / 50+50 /      runtime flag mfx_interp (gfx/nvn/totk_mfx.m)
                          unlimited
     GPU keep-alive       switch, default on                 dm_gfx_keepalive_set (gfx/nvn/nvn_mtl.m), at once;
                                                             gpu-keepalive.txt above the game's folder (both games);
                                                             TOTK_GPU_KEEPALIVE first (greyed out)
     resolution           720p / + MetalFX / 1080p /         runtime_output_mode_set (game_patches*.c); the player picks,
                          + MetalFX / 2K /
                          2K + MetalFX (a 4K screen)
                                                             1080p is the start until a choice is saved; the window
                                                             takes that size at once (settings_window_for)
     aspect ratio         16:9 / 16:10 / 21:9 / 32:9         runtime_aspect_set_ratio (aspect_patch.h, game_patches_botw.c);
                                                             the window takes that shape at once, its height kept
                                                             (runtime_front_resize_aspect; user 2026-10-25: no fitting the
                                                             window any more); TOTK_ASPECT's ratio is offered too
     full screen around   switch, default off                front_notch_set (frontend.m), fullscreen-notch.txt; on a screen
     the notch                                               with a camera housing full screen covers the whole panel;
                                                             TOTK_NOTCH decides first (greyed out)
   Input
     controller type      auto / Xbox / PlayStation / Switch runtime_gamepad_type_set (gamepad.m), saved
     background controller switch, default off              runtime_gamepad_background_set, controller-background.txt
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
     screenshot folder    the system's folder chooser          the capture button's pictures (runtime/screenshot.h); the
                                                             choice in screenshot-dir.txt next to controls.txt, and the
                                                             folder checked as it is picked
   Effects (user 2026-10-30, after UltraCam's render distance and shader switches; the rows from FOV on are Tears of the
   Kingdom 1.4.3's, greyed on other versions; "Reset to defaults" at the bottom right sets every row back to the game's own
   and removes its file, a row the environment decides staying)
     edge smoothing       switch                             taa_set (gfx/nvn/totk_taa.m)
     anisotropic filtering off / 2x / 4x / 8x / 16x          dm_gfx_aniso_set (gfx/nvn/nvn_mtl.m: the samplers made again),
                                                             at once; graphics-aniso.txt; TOTK_ANISO first
     ambient occlusion    off / 1.5x / 2x / 3x, default off  dm_gfx_ssao_* (gfx/nvn/totk_ssao.h, PR #1316), saved in ssao.txt
                                                             (off or the strength and radius in percent); Tears of the
                                                             Kingdom 1.2.1 / 1.4.3 and Breath of the Wild 1.9.0 only. Its shaders
                                                             are always the patched ones on the Mac (off: the stock AO,
                                                             bit for bit), so every choice acts at once (user 2026-10-30);
                                                             TOTK_SSAO / _STRENGTH / _RADIUS decide first (greyed out)
     shadow resolution    off / as the game has it / 2048     Breath of the Wild 1.9.0 (PR #1447): runtime_shadow_res_save
                          per cascade                        (game_patches_botw.c, botw_shadow_res.h), shadows.txt (off or
                                                             2048); the game sizes the map once, so the choice is the next
                                                             launch's (its title says so); TOTK_SHADOW_RES decides first;
                                                             off is the effect switch "shadow", at once (botw_effects.h)
     model detail distance 1x / 2x / 4x / 10x / 20x / 50x / runtime_lod_scale_set (game_patches_v143.c): the level of
                                                             detail a model gets at a distance is the one it had at
     100x                                                    1/K of it; graphics-lod.txt (x100); TOTK_LOD_SCALE first
     render distance      1x .. 10x                          runtime_far_scale_set: the camera's far plane x K (user
                                                             2026-10-30: larger, not smaller; the game's 25000 already
                                                             spans the map); graphics-far.txt (percent); TOTK_FAR_SCALE
                                                             first
     field of view        0.8x .. 1.5x                       runtime_fov_scale_set: the game camera's vertical FOV
                                                             (its zooms scale too); graphics-fov.txt (x100)
     field of view        1x .. 1.5x                         Breath of the Wild 1.9.0: runtime_fov_scale_set
                                                             (game_patches_botw.c, botw_fov.h: the main camera's vertical
                                                             FOV x K at its setFovy call, at once; tried in play); graphics-fov.txt, x100
     model detail distance also "always the finest" (graphics-lod.txt "max": the level a model has up close)
     shadow resolution    off / 1x (800) / 2x / 3x / 4x      runtime_shadow_scale_set (game_patches_v143.c: the cascades'
                                                             size x K where the game works it out); off = the effect
                                                             switch "shadow" (totk_ssao.h); graphics-shadow.txt (x100)
     anti-aliasing       Off / FXAA / SMAA; quality         1.4.3: totk_aa_config.h; graphics-aa-method.txt, graphics-fxaa-quality.txt,
                                                             graphics-smaa-quality.txt. FXAA Original / Quality; SMAA Low / Medium / High / Ultra
     FXAA anti-aliasing   switch                             other versions: dm_gfx_fxaa_set; graphics-fxaa.txt
                                                             (Breath of the Wild 1.9.0: gfx/nvn/botw_finish.h; greyed while
                                                             MetalFX replaces its FXAA / upscale draw)
     depth of field       switch                             runtime_dof_set (game_patches_v143.c: no DOF request stored
                                                             while off; events still complete); graphics-dof.txt
     sharpening           off / 25% / 50% / 75% / 100%       dm_gfx_sharpen_set: FSR 1 RCAS on the 3D picture;
                                                             graphics-sharpen.txt (percent); Breath of the Wild 1.9.0: on
                                                             the canvas before the UI (gfx/nvn/botw_finish.h)
     one switch per effect                                   dm_gfx_fx_set (gfx/nvn/totk_fx.h): its passes left out while
                                                             off; graphics-effects.txt lists the ones that are off;
                                                             Breath of the Wild 1.9.0: ambient occlusion (botw_effects.h)
   User (runtime/users.h, docs/users.md; the save area is named after the user's id, so another user
   plays on their own saves; a running game keeps its user)
     user this run        fixed (greyed)                     runtime_users_active: the user this run opened with
                                                             the game opened with, as on a console
     user                 pop-up of the users,               runtime_users_choose: names the user the next launch opens
                          from the next launch               with (at most 8, as on a console)
     Add User… (bottom    the game's text bar                runtime_front_text_input takes the name (at most 10
     right)               takes the name                     characters), runtime_users_add keeps it; the panel closes
                                                             while the bar is up and comes back after */
#if SETTINGS_GAME_PART == 1
/* Its pages besides Display, Input, Key bindings and 着色器贡献: all of them. */
#define SETTINGS_HAS_USERS 1
#define SETTINGS_HAS_EFFECTS 1
#define SETTINGS_HAS_BOARD 1
#define SETTINGS_HAS_SHOTS 1
#define SETTINGS_HAS_VR 1
#ifdef TOTK_GAME_V143
#define SETTINGS_HAS_AA_ROWS 1   /* the effects page's quality row follows the method (settings_aa_rebuild, part 2) */
#endif
#include "totk_fps_limit.h"   /* totk_fps_limits: the frame lock choices */
#include "aspect_math.h"     /* aspect_near, ASPECT_GAME */
#include "../zeldaVR/runtime/vr_camera.h"   /* runtime_camera_distance_*: 1.4.3's camera hook, a default elsewhere */
#include "input_prompts.h"
#include "feedback_settings.h"   /* feedback_word: ssao.txt */
#include "users.h"           /* the local users (runtime/account.c): the User page */
#if defined(GAME_DKB) || defined(GAME_POKOPIA)
#include "ns2_resolution.h"
#endif

void dm_gfx_set_flag(const char *name, int value);
void dm_gfx_ssao_start(int pct); int dm_gfx_ssao_locked(void); int dm_gfx_ssao_live(void); void dm_gfx_ssao_set(int pct);   /* totk_ssao.h */
void dm_gfx_aniso_start(int n); int dm_gfx_aniso_locked(void); int dm_gfx_aniso(void); void dm_gfx_aniso_set(int n);   /* gfx/nvn/nvn_mtl.m */
void dm_gfx_keepalive_start(int on); int dm_gfx_keepalive_locked(void); int dm_gfx_keepalive(void); void dm_gfx_keepalive_set(int on);   /* gfx/nvn/nvn_mtl.m */
#ifdef GAME_BOTW190
int runtime_shadow_res_available(void); int runtime_shadow_res_locked(void); int runtime_shadow_res_saved(void);
void runtime_shadow_res_save(int size);   /* game_patches_botw.c */
double runtime_fov_scale(void); void runtime_fov_scale_set(double k); int runtime_fov_scale_locked(void); int runtime_fov_available(void);
#endif
void nvn_fps_lock_set(int fps);   /* gfx/nvn/hle_nvn.c: the shared totk_fps_limits choices */
#ifdef TOTK_GAME_V143   /* the effects page: its shader keys and patch sites are 1.4.3's */
int dm_gfx_fx_count(void); const char *dm_gfx_fx_id(int i); const char *dm_gfx_fx_name(int i, int english);   /* totk_fx.h */
int dm_gfx_fx_available(int i); int dm_gfx_fx_on(int i); void dm_gfx_fx_set(int i, int on);
double runtime_lod_scale(void); void runtime_lod_scale_set(double k); int runtime_lod_scale_locked(void);   /* game_patches_v143.c */
double runtime_far_scale(void); void runtime_far_scale_set(double k); int runtime_far_scale_locked(void);
double runtime_fov_scale(void); void runtime_fov_scale_set(double k); int runtime_fov_scale_locked(void); int runtime_fov_available(void);
double runtime_shadow_scale(void); void runtime_shadow_scale_set(double k); int runtime_shadow_scale_locked(void);
void runtime_dof_set(int on); int runtime_dof_on(void);   /* game_patches_v143.c */
void dm_gfx_fxaa_set(int on); int dm_gfx_fxaa_on(void); void dm_gfx_sharpen_set(int pct); int dm_gfx_sharpen(void);   /* gfx/nvn/totk_finish.h */
void dm_gfx_smaa_set(int on); int dm_gfx_smaa_on(void);   /* optional spatial SMAA, before the UI */
int dm_gfx_smaa_quality(void); void dm_gfx_smaa_quality_set(int q);
int dm_gfx_aa_method(void); void dm_gfx_aa_method_set(int method);
int dm_gfx_fxaa_quality(void); void dm_gfx_fxaa_quality_set(int q);
#endif
int nvn_fps_lock(void);

/* Picking a resolution sizes the window to it at once (user 2026-10-25): 720p 1280x720, 1080p and 720p + MetalFX 1920x1080, MetalFX 2560x1440, native its own size (2K), 2K + MetalFX and native 4K 3840x2160, in the window's points as the first window (frontend.m runtime_front_run), the picture's shape kept
   (the aspect ratio picked) and shrunk to fit the screen's visible area. Full screen stays as it is. */
static void settings_window_size(unsigned w,unsigned h,double ratio) {
 if(!window||win_full())return;   /* full screen, or the full panel around the notch */
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
 else if(mode==OUTPUT_MODE_NATIVE_5K||mode==OUTPUT_MODE_4K_TO_5K_METALFX){w=5120;h=2880;}
 else if(mode==OUTPUT_MODE_720P){w=1280;h=720;}
 else if(mode==OUTPUT_MODE_NATIVE&&!runtime_output_native(mode,&w,&h)){w=1920;h=1080;}
 double ratio=runtime_aspect_ratio()>0?runtime_aspect_ratio():(double)w/h;
 settings_window_size(w,h,ratio);
}

/* Settings live alongside controls.txt. Restore the graphics switches before the guest starts; output and aspect
   patches become available later, so restore those on the main thread when the guest has installed them. */
static int settingsPendingOutput = -1;
static float settingsPendingAspect;
static NSTimer *settingsRestoreTimer;
static void settings_save_frame(void) {
 char word[32];int fps=nvn_fps_lock(),mfx=mfx_requested();
 if(fps)snprintf(word,sizeof word,"lock-%d",fps);
 else if(mfx)snprintf(word,sizeof word,"interp-%d",mfx);
 else snprintf(word,sizeof word,"off");
 runtime_setting_save("graphics-frame.txt",word,"frame mode");
}
/* Ambient occlusion (PR #1316): ssao.txt holds off or the strength and radius in percent, read before the guest starts
   because the shaders are patched as they load. The levels the row offers; a hand-written percent is kept too. */
#if defined(TOTK_GAME_V143) || defined(GAME_BOTW190) || defined(TOTK_GAME_V121)
#define SETTINGS_SSAO 1
#endif
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
#ifdef TOTK_GAME_V143
/* "Always the finest": a factor no distance in the map reaches, so a model keeps the level it has up close. */
#define SETTINGS_LOD_MAX 1e6
static void settings_effects_save(void) {
 char line[512]="";size_t n=0;
 for(int i=0;i<dm_gfx_fx_count();i++)if(!dm_gfx_fx_on(i))n+=(size_t)snprintf(line+n,sizeof line-n,"%s%s",n?" ":"",dm_gfx_fx_id(i));
 runtime_setting_save("graphics-effects.txt",n?line:"none","effects");
}
static void settings_aa_save(void) {
 settings_save_int("graphics-fxaa-quality.txt",dm_gfx_fxaa_quality(),"FXAA quality");
 settings_save_int("graphics-smaa-quality.txt",dm_gfx_smaa_quality(),"SMAA quality");
 settings_save_int("graphics-aa-method.txt",dm_gfx_aa_method(),"anti-aliasing method");
}
static void settings_effects_restore(void) {
 char line[512];
 if(runtime_setting_word("graphics-effects.txt",line,sizeof line))
  for(char *w=strtok(line," \t\r\n");w;w=strtok(NULL," \t\r\n"))
   for(int i=0;i<dm_gfx_fx_count();i++)if(!strcmp(w,dm_gfx_fx_id(i)))dm_gfx_fx_set(i,0);
 double d;char word[16];
 if(!runtime_lod_scale_locked()&&runtime_setting_word("graphics-lod.txt",word,sizeof word)&&!strncmp(word,"max",3))runtime_lod_scale_set(SETTINGS_LOD_MAX);
 else if(!runtime_lod_scale_locked()&&settings_read_number("graphics-lod.txt",100,10000,&d))runtime_lod_scale_set(d/100);
 if(!runtime_fov_scale_locked()&&settings_read_number("graphics-fov.txt",50,200,&d))runtime_fov_scale_set(d/100);
 if(!runtime_shadow_scale_locked()&&settings_read_number("graphics-shadow.txt",25,800,&d))runtime_shadow_scale_set(d/100);
 if(runtime_setting_word("graphics-fxaa.txt",word,sizeof word)&&!strncmp(word,"off",3))dm_gfx_fxaa_set(0);
 int method,sq;
 if(settings_read_int("graphics-aa-method.txt",&method)&&method>=0&&method<=2){
  if(settings_read_int("graphics-fxaa-quality.txt",&sq)&&sq>=0&&sq<=1)dm_gfx_fxaa_quality_set(sq);
  if(settings_read_int("graphics-smaa-quality.txt",&sq)&&sq>=0&&sq<=3)dm_gfx_smaa_quality_set(sq);
  dm_gfx_aa_method_set(method);
 }
 if(settings_read_number("graphics-sharpen.txt",0,100,&d))dm_gfx_sharpen_set((int)d);
 if(runtime_setting_word("graphics-dof.txt",word,sizeof word)&&!strncmp(word,"off",3))runtime_dof_set(0);
 if(!runtime_far_scale_locked()&&settings_read_number("graphics-far.txt",0.1,1000,&d))runtime_far_scale_set(d/100);
}
#elif defined(GAME_BOTW190)
static void settings_effects_restore(void);   /* botw_effects.h */
#else
static void settings_effects_restore(void) {}
#endif
static void settings_restore_begin(void) {
 extern int vr_enabled(void);
 int v;char word[32];
 if(settings_read_int("graphics-overlay.txt",&v)&&v<=HUD_MODE_FULL)fps_overlay_set_hud_mode(v);
 if(!getenv("TOTK_MFX")&&runtime_setting_word("graphics-frame.txt",word,sizeof word)){
  int n=0;char tail=0;
  if(sscanf(word,"lock-%d %c",&n,&tail)==1&&(n>0&&totk_fps_limit_valid(n))){dm_gfx_set_flag("mfx_interp",MFX_OFF);nvn_fps_lock_set(n);}
  else if(sscanf(word,"interp-%d %c",&n,&tail)==1&&n>=MFX_30&&n<MFX_NMODES){nvn_fps_lock_set(0);dm_gfx_set_flag("mfx_interp",n);}
  else if(!strcmp(word,"off")||!strcmp(word,"off\n")){nvn_fps_lock_set(0);dm_gfx_set_flag("mfx_interp",MFX_OFF);}
 }
 if(!getenv("TOTK_TAA")&&settings_read_int("graphics-taa.txt",&v)&&(v==0||v==1))taa_set(v);
 settings_ssao_restore();
 if(!dm_gfx_aniso_locked()&&settings_read_int("graphics-aniso.txt",&v)&&v<=16)dm_gfx_aniso_start(v);
 if(!dm_gfx_keepalive_locked()&&runtime_shared_setting_word("gpu-keepalive.txt",word,sizeof word))dm_gfx_keepalive_start(!strncmp(word,"on",2));
 settings_effects_restore();
 /* Edge smoothing's resolve, kept in graphics-taa-mode.txt: "fast" (one compute pass) or "metalfx" (the default);
    TOTK_TAA_FAST decides first. The display page's edge smoothing row (off / fast / MetalFX) writes it. */
 if(!getenv("TOTK_TAA_FAST")&&runtime_setting_word("graphics-taa-mode.txt",word,sizeof word)){
  word[strcspn(word," \t\r\n")]=0;
  if(!strcmp(word,"fast"))taa_set_fast(1);else if(!strcmp(word,"metalfx"))taa_set_fast(0);
 }
 const char *output=getenv("TOTK_OUTPUT_RES");
 if(
#if defined(GAME_BOTW160)
    !vr_enabled()&&
#endif
    (!output||!*output||!strcmp(output,"auto"))&&!getenv("TOTK_MFX_OUTPUT")&&!getenv("TOTK_MFX_UPSCALE")&&
    settings_read_int("graphics-output.txt",&v)&&v<=OUTPUT_MODE_LAST)settingsPendingOutput=v;
 if(
#if defined(GAME_BOTW160)
    !vr_enabled()&&
#endif
    !getenv("TOTK_ASPECT")&&runtime_setting_word("graphics-aspect.txt",word,sizeof word)){
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
#define SETTINGS_HDR_ROWS 2   /* the effects page's HDR rows on 1.4.3 when HDR is available (settings_hdr_rows; PR #1129) */
#if defined(GAME_DKB) || defined(GAME_POKOPIA)
/* The game consumes this request in its native display calculation. */
static void settings_ns2_save_size(void) {
 uint64_t s=runtime_output_size_current();unsigned w=(unsigned)s,h=(unsigned)(s>>32);char word[32];
 snprintf(word,sizeof word,"%ux%u",w,h);runtime_setting_save("graphics-output-size.txt",word,"native render resolution");
 snprintf(word,sizeof word,"%.8g",h?(double)w/h:16.0/9);runtime_setting_save("graphics-aspect.txt",word,"aspect ratio");
}
static TOTKSettingRow *settings_ns2_resolution_row(void) {
 NSString *title=settings_text("分辨率","Resolution");
 uint64_t current=runtime_output_size_current();unsigned w=(unsigned)current,h=(unsigned)(current>>32);
 NSString *picked=[NSString stringWithFormat:@"%ux%u",w,h];
 NSMutableArray<NSString *> *sizes=[NSMutableArray array],*names=[NSMutableArray array];
 unsigned mw=3840,mh=2160;runtime_output_res(&mw,&mh);
 const unsigned heights[]={540,720,1080,1440,2160};
 const char *labels[]={"540p","720p","1080p","2K","4K"};
 for(unsigned k=0;k<5;k++){
  unsigned nh=heights[k],nw=h?(unsigned)(((uint64_t)w*nh+h/2)/h):nh*16/9;
  if(nw>mw){nh=w?(unsigned)((uint64_t)mw*h/w):0;nw=h?(unsigned)(((uint64_t)w*nh+h/2)/h):0;}
  if(nh>mh){nw=(unsigned)((uint64_t)nw*mh/nh);nh=mh;}
  if(!nw||!nh)continue;
  NSString *s=[NSString stringWithFormat:@"%ux%u",nw,nh];if([sizes containsObject:s])continue;
  [sizes addObject:s];[names addObject:nh==heights[k]?[NSString stringWithFormat:@"%s (%u×%u)",labels[k],nw,nh]:[NSString stringWithFormat:@"%u×%u",nw,nh]];
 }
 if(![sizes containsObject:picked]){[sizes addObject:picked];[names addObject:[picked stringByReplacingOccurrencesOfString:@"x" withString:@"×"]];}
 NSInteger selected=[sizes indexOfObject:picked];
 NSMutableArray<NSNumber *> *values=[NSMutableArray array];for(NSUInteger i=0;i<sizes.count;i++)[values addObject:@(i)];
 [names addObject:settings_text("自定义…","Custom…")];[values addObject:@-1];
 return settings_row(title,names,values,^int{return (int)selected;},^(int value){
  NSString *size=value>=0&&value<(int)sizes.count?sizes[(NSUInteger)value]:nil;
  if(value<0){
   NSAlert *alert=[NSAlert new];alert.messageText=settings_text("自定义渲染分辨率","Custom render resolution");
   alert.informativeText=settings_text("输入宽x高，例如3840x2160。保存后立即应用。","Enter widthxheight, e.g. 3840x2160. Applies immediately.");
   NSTextField *field=[[NSTextField alloc] initWithFrame:NSMakeRect(0,0,240,24)];field.stringValue=picked;alert.accessoryView=field;
   [alert addButtonWithTitle:settings_text("保存","Save")];[alert addButtonWithTitle:settings_text("取消","Cancel")];
   if([alert runModal]!=NSAlertFirstButtonReturn){dispatch_async(dispatch_get_main_queue(),^{settings_rebuild();});return;}
   size=[field.stringValue stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
  }
  unsigned nw,nh;
  if(!size||!ns2_parse_resolution(size.UTF8String,&nw,&nh)){
   NSAlert *error=[NSAlert new];error.messageText=settings_text("分辨率格式无效","Invalid resolution");
   error.informativeText=settings_text("请输入宽x高，每边1–16384。原设置保留。","Use widthxheight, each 1–16384. The previous choice is kept.");[error runModal];dispatch_async(dispatch_get_main_queue(),^{settings_rebuild();});return;
  }
  if(!runtime_output_size_set(nw,nh)){
   NSAlert *error=[NSAlert new];error.messageText=settings_text("分辨率超出本次启动容量","Resolution exceeds this run's capacity");
   unsigned mw=0,mh=0;runtime_output_res(&mw,&mh);
   error.informativeText=[NSString stringWithFormat:settings_text("本次最高支持 %u×%u。原设置保留。","This run supports up to %u×%u. The previous choice is kept."),mw,mh];
   [error runModal];dispatch_async(dispatch_get_main_queue(),^{settings_rebuild();});return;
  }
  settings_ns2_save_size();
  dispatch_async(dispatch_get_main_queue(),^{settings_rebuild();});
 });
}
#endif
/* The DSU rows of the input page (runtime/dsu_client.h): a server to read a controller's motion from. */
#include "dsu_settings_rows.h"
/* VR settings (zeldaVR; user 2026-10-26: "放在一个新设置 叫 vr设置里 可以选这几种分辨率"), shown while VR is on
   (TOTK_VR_CONTROL): the per-eye size the streamer sends to the headset. Picking one restarts the streamer the game
   runs (TOTK_VR_STREAMER) and the headset reconnects in a few seconds. Measured on a Quest 3: 1344x1408 and 1680x1760
   hold 72 Hz (about 81 ms); 1920x2048 is sharper at about 60 Hz and 120 ms; 2048x2208 (user 2026-10-26: "增加 2048×2208")
   the sharpest, its H.264 encode takes about 41 ms a frame (vr开发.md). */
int vr_enabled(void);
int vr_streamer_managed(void);
void vr_resolution(int *w, int *h);
void vr_stream_resolution(int *w,int *h);
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
#ifndef GAME_BOTW
 if(vr_hide_head_mode&&vr_set_hide_head_mode)
  [rows addObject:settings_row(settings_text("林克的头","Link's head"),
   @[settings_text("自动（第一人称时隐藏）","Auto (hidden in first person)"),settings_text("隐藏","Hidden"),settings_text("显示","Shown")],@[@-1,@1,@0],
   ^int{return vr_hide_head_mode();},^(int v){vr_set_hide_head_mode(v);})];
#endif
 if(!runtime_camera_distance_available())return;
 [rows addObject:settings_row(settings_text("摄像机距离（第三人称）","Camera distance (third person)"),
  @[settings_text("默认","Default"),settings_text("近 1 米","1 m closer"),settings_text("近 2 米","2 m closer"),settings_text("近 3 米","3 m closer"),
    settings_text("近 4 米","4 m closer"),settings_text("近 5 米","5 m closer")],   /* user 2026-10-26: "改成 近1m 近2m 近3m 近4m 近5m" */
  @[@0,@-100,@-200,@-300,@-400,@-500],^int{return runtime_camera_distance_cm();},^(int v){runtime_camera_distance_set(v);})];
}
static void settings_vr_rows(NSMutableArray<TOTKSettingRow *> *rows) {
 if(!vr_enabled())return;
#ifdef GAME_BOTW190
 [rows addObject:settings_fixed(settings_text("VR 设置 · 视角","VR · view"),settings_text("第三人称","Third person"))];
#elif defined(GAME_BOTW) || defined(TOTK_GAME_V143)
 [rows addObject:settings_row(settings_text("VR 设置 · 视角","VR · view"),@[settings_text("第三人称","Third person"),settings_text("第一人称","First person")],@[@0,@1],
  ^int{return vr_first_person();},^(int v){vr_set_first_person(v);})];
#else
 [rows addObject:settings_row(settings_text("VR 设置 · 视角","VR · view"),@[settings_text("第一人称","First person"),settings_text("第三人称","Third person")],@[@1,@0],
  ^int{return vr_first_person();},^(int v){vr_set_first_person(v);})];
#endif
#ifndef GAME_BOTW
 /* the camera's spring on the head, ms (user 2026-11-02: "用 弹性 解决" the shaking; 0: bound rigidly) */
 {TOTKSettingRow *r=settings_row(settings_text("第一人称镜头弹性（毫秒，0 = 绑死）","First person camera spring (ms, 0 = rigid)"),nil,nil,^int{return vr_eye_spring_ms();},^(int v){vr_set_eye_spring_ms(v);});
  r.sliderMin=0;r.sliderMax=300;[rows addObject:r];}
#endif
 /* The camera's position (user 2026-10-29: "可以改变 摄像头 上下前后左右 位置 ，方便我 实时调整"): -100..100 cm each, from where
    the game's camera faces, applied while dragging (totk_vr.m vr_set_offset_cm, kept in vr-offset.txt). */
 NSArray<NSString *> *axes=@[settings_text("摄像头 上下（厘米）","Camera up / down (cm)"),settings_text("摄像头 前后（厘米）","Camera forward / back (cm)"),
  settings_text("摄像头 左右（厘米）","Camera right / left (cm)")];
 for(int a=0;a<3;a++){TOTKSettingRow *r=settings_row(axes[(NSUInteger)a],nil,nil,^int{return vr_offset_cm(a);},^(int v){vr_set_offset_cm(a,v);});
  r.sliderMin=-100;r.sliderMax=100;[rows addObject:r];}
#if !defined(GAME_BOTW) && !defined(TOTK_GAME_V143)
 /* What the Mac window shows (user 2026-10-29: "屏幕 显示 类似vr 那种 两个屏幕"): both eyes as the headset gets them, or the game's picture. */
 /* "显示 Quest 画面" (user 2026-10-30: "桌面显示 和 quest里面 一样的画面 设置里加一个 按钮 叫 显示quest画面"): the Mac window shows
    both eyes; on, each inside its lens opening as in the headset; off, uncovered ("就是 不遮挡状态"). */
 [rows addObject:settings_row(settings_text("显示 Quest 画面","Show the Quest view"),nil,nil,^int{return vr_quest_view();},^(int v){vr_set_quest_view(v);})];
 /* ... each eye's middle 2..10 times larger then (user 2026-10-30: "设置里可以调 2倍 到 10 倍") */
 {TOTKSettingRow *r=settings_row(settings_text("Mac 预览放大（倍）","Mac preview zoom (x)"),nil,nil,^int{return vr_quest_zoom();},^(int v){vr_set_quest_zoom(v);});
  r.sliderMin=2;r.sliderMax=10;[rows addObject:r];}
 /* The head's position (6DoF) and snap turning ("按建议 都做", 2026-10-30, after BotW BetterVR and UEVR). */
 [rows addObject:settings_row(settings_text("头部位置（6DoF：探身、下蹲）","Head position (6DoF: lean, crouch)"),nil,nil,^int{return vr_6dof();},^(int v){vr_set_6dof(v);})];
 [rows addObject:settings_row(settings_text("立体（左右眼轮流渲染）","Stereo (the eyes rendered in turn)"),nil,nil,^int{return vr_afr();},^(int v){vr_set_afr(v);})];
#endif
 {TOTKSettingRow *r=settings_row(settings_text("立体 · 两眼间距（%）","Stereo · eye distance (%)"),nil,nil,^int{return vr_ipd_pct();},^(int v){vr_set_ipd_pct(v);});
  r.sliderMin=30;r.sliderMax=200;[rows addObject:r];}
 [rows addObject:settings_row(settings_text("体感操作（挥剑、举盾、拉弓、手势）","Motion controls (swing, shield, bow, gestures)"),nil,nil,^int{return vr_motion();},^(int v){vr_set_motion(v);})];
 [rows addObject:settings_row(settings_text("转身方式","Turning"),@[settings_text("平滑","Smooth"),settings_text("分段（小）","Snap (small)"),settings_text("分段（中）","Snap (medium)"),settings_text("分段（大）","Snap (large)")],
  @[@0,@120,@200,@300],^int{return vr_snap_ms();},^(int v){vr_set_snap_ms(v);})];
#if defined(GAME_BOTW160) || defined(TOTK_GAME_V143)
 static const int sizes[][2]={{1280,1380},{1536,1656},{1792,1932},{2048,2208},{2560,2760},{3072,3312},{3584,3864},{4096,4416}};
 NSArray<NSString *> *names=@[@"0.625×　1280×1380",@"0.75×　1536×1656",@"0.875×　1792×1932",@"1.0×　2048×2208",@"1.25×　2560×2760",@"1.5×　3072×3312",@"1.75×　3584×3864",@"2.0×　4096×4416"];
#else
 static const int sizes[][2]={{1344,1408},{1680,1760},{1920,2048},{2048,2208}};
 NSArray<NSString *> *names=@[@"1344×1408",
  @"1680×1760",
  @"1920×2048",
  @"2048×2208"];
#endif
 NSString *title=settings_text("VR 设置 · 分辨率（每眼）","VR · resolution (per eye)");
#if defined(GAME_BOTW160) || defined(TOTK_GAME_V143)
 title=settings_text("渲染分辨率（每眼，立即生效）","Render resolution (per eye, immediate)");
 int stream_w=0,stream_h=0;vr_stream_resolution(&stream_w,&stream_h);
 [rows addObject:settings_fixed(settings_text("串流分辨率（每眼）","Stream resolution (per eye)"),
   stream_w>0?[NSString stringWithFormat:@"%d×%d",stream_w,stream_h]:settings_text("未连接","not connected"))];
#endif
 if(!vr_streamer_managed()){int w=0,h=0;vr_resolution(&w,&h);
  [rows addObject:settings_fixed(title,w>0?[NSString stringWithFormat:@"%d×%d",w,h]:settings_text("未连接","not connected"))];return;}
 NSMutableArray<NSNumber *> *values=[NSMutableArray array];for(NSUInteger i=0;i<names.count;i++)[values addObject:@(i)];
 [rows addObject:settings_row(title,names,values,^int{int w=0,h=0;vr_resolution(&w,&h);
   for(NSUInteger i=0;i<names.count;i++)if(sizes[i][0]==w&&sizes[i][1]==h)return (int)i;return -1;},
  ^(int v){if(v>=0&&(NSUInteger)v<names.count)vr_set_resolution(sizes[v][0],sizes[v][1]);})];
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
#ifdef TOTK_GAME_V143
static TOTKSettingRow *settings_distance_row(int lod) {
 NSString *t=lod?settings_text("模型细节距离（LOD）","Model detail distance (LOD)"):settings_text("渲染距离","Render distance");
 /* the row's values: the factor x100 for the detail distance, x1000 for the render distance (0.1% = 1) */
 double unit=lod?100:1000;
 int (^current)(void)=^int{double k=lod?runtime_lod_scale():runtime_far_scale();return lod&&k>=SETTINGS_LOD_MAX?-1:(int)lround(k*unit);};
 int now=current();
 NSString *(^text)(int)=^NSString *(int v){
  if(v<0)return settings_text("始终最高精度","Always the finest");
  if(v==unit)return settings_text("1×（游戏原样）","1x (as the game has it)");
  return [NSString stringWithFormat:@"%g×",v/unit];};
 if(lod?runtime_lod_scale_locked():runtime_far_scale_locked())return settings_fixed(t,text(now));
 NSMutableArray<NSNumber *> *values=[(lod?@[@100,@200,@400,@1000,@2000,@5000,@10000,@-1]:@[@1000,@2000,@3000,@4000,@5000,@6000,@7000,@8000,@9000,@10000]) mutableCopy];
 if(![values containsObject:@(now)])[values addObject:@(now)];
 NSMutableArray<NSString *> *choices=[NSMutableArray array];for(NSNumber *v in values)[choices addObject:text(v.intValue)];
 return settings_row(t,choices,values,current,^(int v){
  if(lod&&v<0){runtime_lod_scale_set(SETTINGS_LOD_MAX);runtime_setting_save("graphics-lod.txt","max","model detail distance");}
  else if(lod){runtime_lod_scale_set(v/100.0);settings_save_int("graphics-lod.txt",v,"model detail distance");}
  else{char word[16];snprintf(word,sizeof word,"%g",v/10.0);runtime_far_scale_set(v/1000.0);runtime_setting_save("graphics-far.txt",word,"render distance");}});
}
/* The game camera's field of view, in hundredths of the game's own (its zooms scale with it). */
static TOTKSettingRow *settings_fov_row(void) {
 NSString *t=settings_text("视野（FOV）","Field of view (FOV)");
 NSString *(^text)(int)=^NSString *(int v){return v==100?settings_text("1×（游戏原样）","1x (as the game has it)"):[NSString stringWithFormat:@"%g×",v/100.0];};
 int now=(int)lround(runtime_fov_scale()*100);
 if(runtime_fov_scale_locked())return settings_fixed(t,text(now));
 if(!runtime_fov_available())return settings_fixed(t,settings_text("游戏原样（Vert+ 开着）","As the game has it (Vert+ is on)"));
 NSMutableArray<NSNumber *> *values=[@[@80,@90,@100,@110,@120,@130,@140,@150] mutableCopy];
 if(![values containsObject:@(now)])[values addObject:@(now)];
 NSMutableArray<NSString *> *choices=[NSMutableArray array];for(NSNumber *v in values)[choices addObject:text(v.intValue)];
 return settings_row(t,choices,values,^int{return (int)lround(runtime_fov_scale()*100);},^(int v){runtime_fov_scale_set(v/100.0);settings_save_int("graphics-fov.txt",v,"field of view");});
}
/* The sun's shadows: off (the effect switch "shadow": the shadow term lit) or their maps' resolution, x1 being the game's
   800 x 800 per cascade; graphics-shadow.txt holds the resolution (x100), graphics-effects.txt whether they are off. */
static int settings_fx_index(const char *id) { for(int i=0;i<dm_gfx_fx_count();i++)if(!strcmp(dm_gfx_fx_id(i),id))return i; return -1; }
static TOTKSettingRow *settings_shadow_row(void) {
 NSString *t=settings_text("阴影分辨率","Shadow resolution");int fx=settings_fx_index("shadow");
 NSString *(^text)(int)=^NSString *(int v){
  if(!v)return settings_text("关（无阴影）","Off (no shadows)");
  if(v==100)return settings_text("1×（游戏原样 800）","1x (as the game has it, 800)");
  return [NSString stringWithFormat:@"%g×（%d）",v/100.0,v*8];};
 int (^current)(void)=^int{return fx>=0&&!dm_gfx_fx_on(fx)?0:(int)lround(runtime_shadow_scale()*100);};
 if(runtime_shadow_scale_locked())return settings_fixed(t,text(current()));
 NSMutableArray<NSNumber *> *values=[@[@0,@100,@200,@300,@400] mutableCopy];
 if(fx<0||!dm_gfx_fx_available(fx))[values removeObject:@0];
 if(![values containsObject:@(current())])[values addObject:@(current())];
 NSMutableArray<NSString *> *choices=[NSMutableArray array];for(NSNumber *v in values)[choices addObject:text(v.intValue)];
 return settings_row(t,choices,values,current,^(int v){
  if(fx>=0&&dm_gfx_fx_available(fx))dm_gfx_fx_set(fx,v!=0);
  if(v){runtime_shadow_scale_set(v/100.0);settings_save_int("graphics-shadow.txt",v,"shadow resolution");}
  settings_effects_save();});
}
/* HDR / EDR (PR #1129, the RenoDX approach; Mac, 1.4.3): the switch and the game's brightness (its white in SDR-white
   units). hdr.txt holds on / off / auto (hdr_auto.h); each tone-mapping value its own hdr-<name>.txt (totk_hdr.h lists all thirteen; the
   others, peak (0: the display's headroom), UI white, exposure, ..., are set in their files; the page has no room). Off leaves every shader, pipeline and texture as without HDR (nvn_mtl.m keeps both compose shaders).
   Frame interpolation shows SDR MetalFX frames, so turning HDR on turns it off (and picking it turns HDR off). */
static TOTKSettingRow *settings_hdr_choice(int which,NSString *title,const int *milli,int n,NSString *(^text)(int)){
 int (^current)(void)=^int{return (int)lroundf(front_hdr_parameter(which)*1000);};
 NSMutableArray<NSNumber *> *values=[NSMutableArray array];for(int i=0;i<n;i++)[values addObject:@(milli[i])];
 if(![values containsObject:@(current())])[values addObject:@(current())];   /* a hand-written value is kept */
 NSMutableArray<NSString *> *choices=[NSMutableArray array];for(NSNumber *v in values)[choices addObject:text(v.intValue)];
 return settings_row(title,choices,values,current,^(int v){front_hdr_parameter_save(which,v/1000.0f);});
}
static void settings_hdr_rows(NSMutableArray<TOTKSettingRow *> *rows){
 /* Off / Auto / On in this order: a controller stepping from Off reaches Auto without passing On, which turns frame
    interpolation off. Auto keeps the interpolation choice: it runs wherever Auto leaves HDR off (hdr_auto.h). Picking
    interpolation while HDR is on (On, or Auto on an HDR screen) turns HDR off, as before. */
 [rows addObject:settings_row(settings_text("HDR / EDR","HDR / EDR"),@[settings_on_off(0),settings_text("自动（HDR 屏幕上开）","Auto (on an HDR screen)"),settings_on_off(1)],@[@(HDR_CHOICE_OFF),@(HDR_CHOICE_AUTO),@(HDR_CHOICE_ON)],^int{return front_hdr_choice();},^(int v){
  if(v==HDR_CHOICE_ON&&mfx_requested()!=MFX_OFF){dm_gfx_set_flag("mfx_interp",MFX_OFF);settings_save_frame();}
  runtime_front_hdr_set(v);})];
 static const int white[]={500,750,1000,1500,2000};
 [rows addObject:settings_hdr_choice(TOTK_HDR_GAME_WHITE,settings_text("HDR 游戏亮度","HDR game brightness"),white,5,^NSString *(int v){
  return [NSString stringWithFormat:@"%g×",v/1000.0];})];
}
static NSArray<TOTKSettingRow *> *settings_effect_rows(void) {
 NSMutableArray<TOTKSettingRow *> *rows=[NSMutableArray arrayWithObjects:settings_fov_row(),settings_distance_row(1),settings_distance_row(0),settings_shadow_row(),nil];
 /* FXAA (the game's edge smoothing of the 3D picture) and sharpening (FSR 1 RCAS on the 3D picture, not the UI) */
 [rows addObject:settings_row(settings_text("抗锯齿方式","Anti-aliasing method"),@[settings_text("关","Off"),@"FXAA",@"SMAA"],@[@0,@1,@2],
  ^int{return dm_gfx_aa_method();},^(int v){dm_gfx_aa_method_set(v);settings_aa_save();})];
 if(dm_gfx_aa_method()==1)
  [rows addObject:settings_row(settings_text("抗锯齿质量","Anti-aliasing quality"),@[settings_text("原版","Original"),@"Quality"],@[@0,@1],
   ^int{return dm_gfx_fxaa_quality();},^(int v){dm_gfx_fxaa_quality_set(v);settings_aa_save();})];
 else if(dm_gfx_aa_method()==2)
  [rows addObject:settings_row(settings_text("抗锯齿质量","Anti-aliasing quality"),@[@"Low",@"Medium",@"High",@"Ultra"],@[@0,@1,@2,@3],
   ^int{return dm_gfx_smaa_quality();},^(int v){dm_gfx_smaa_quality_set(v);settings_aa_save();})];
 [rows addObject:settings_row(settings_text("景深","Depth of field"),nil,nil,^int{return runtime_dof_on();},^(int v){
  runtime_dof_set(v);runtime_setting_save("graphics-dof.txt",v?"on":"off","depth of field");})];
 [rows addObject:settings_row(settings_text("锐化","Sharpening"),@[settings_text("关（游戏原样）","Off (as the game has it)"),@"25%",@"50%",@"75%",@"100%"],@[@0,@25,@50,@75,@100],
  ^int{return dm_gfx_sharpen();},^(int v){dm_gfx_sharpen_set(v);settings_save_int("graphics-sharpen.txt",v,"sharpening");})];
 for(int i=0;i<dm_gfx_fx_count();i++){
  if(!strcmp(dm_gfx_fx_id(i),"shadow"))continue;   /* in the shadow resolution row */
  NSString *t=@(TOTK_UI(dm_gfx_fx_name(i,0),dm_gfx_fx_name(i,1)));
  if(!dm_gfx_fx_available(i)){[rows addObject:settings_fixed(t,settings_text("这个版本没有","Not in this version"))];continue;}
  [rows addObject:settings_row(t,nil,nil,^int{return dm_gfx_fx_on(i);},^(int v){dm_gfx_fx_set(i,v);settings_effects_save();})];
 }
 if(totk_hdr_available())settings_hdr_rows(rows);
 return rows;
}
#else
static TOTKSettingRow *settings_botw_shadow_row(void);   /* below: Breath of the Wild 1.9.0's, greyed elsewhere */
static TOTKSettingRow *settings_botw_fov_row(void);      /* below: Breath of the Wild 1.9.0's, greyed elsewhere */
#ifdef GAME_BOTW190
#include "botw_effects.h"   /* Breath of the Wild 1.9.0's page: the effect switches its shaders allow (PR #1602) */
#else
static NSArray<TOTKSettingRow *> *settings_effect_rows(void) {   /* other versions: the page says so */
 NSString *none=settings_text("这个版本没有","Not in this version");
 return @[settings_botw_shadow_row(),settings_fixed(settings_text("景深","Depth of field"),none),settings_botw_fov_row(),settings_fixed(settings_text("FXAA 抗锯齿","FXAA anti-aliasing"),none),settings_fixed(settings_text("锐化","Sharpening"),none),settings_fixed(settings_text("模型细节距离（LOD）","Model detail distance (LOD)"),none),settings_fixed(settings_text("渲染距离","Render distance"),none),
          settings_fixed(settings_text("效果开关","Effect switches"),none)];
}
#endif
#endif
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
#ifdef TOTK_GAME_V143
 if(!runtime_fov_scale_locked())runtime_fov_scale_set(1);
 if(!runtime_lod_scale_locked())runtime_lod_scale_set(1);
 if(!runtime_far_scale_locked())runtime_far_scale_set(1);
 if(!runtime_shadow_scale_locked())runtime_shadow_scale_set(1);
 dm_gfx_fxaa_quality_set(0);dm_gfx_aa_method_set(1);dm_gfx_smaa_quality_set(2);dm_gfx_sharpen_set(0);runtime_dof_set(1);
 for(int i=0;i<dm_gfx_fx_count();i++)if(!dm_gfx_fx_on(i))dm_gfx_fx_set(i,1);
 for(const char *const *f=(const char *const[]){"graphics-fov.txt","graphics-lod.txt","graphics-far.txt","graphics-fxaa.txt","graphics-smaa-quality.txt","graphics-aa-method.txt","graphics-fxaa-quality.txt","graphics-sharpen.txt","graphics-effects.txt","graphics-shadow.txt","graphics-dof.txt",NULL};*f;f++)
  runtime_setting_remove(*f);
#endif
#ifdef GAME_BOTW190
 if(runtime_shadow_res_available()&&!runtime_shadow_res_locked()){runtime_shadow_res_save(0);runtime_setting_remove("shadows.txt");}
 if(runtime_fov_available()&&!runtime_fov_scale_locked()){runtime_fov_scale_set(1);runtime_setting_remove("graphics-fov.txt");}
 settings_botw_effects_reset();
#endif
 fprintf(stderr,"[totk] settings: the effects page back to the defaults\n");
}
/* Sun shadows (Breath of the Wild 1.9.0, botw_shadow_res.h): off, the game's sizes or 2048 per cascade. The game sizes its
   map once, at the start, so for the sizes the row shows and saves the next launch's choice, never this run's; off acts at
   once (P9, 2026-11-05: on the Great Plateau the shadows of Link, rocks and cliffs go, with no other evident fault in that scene; AO and other conditions not checked). Where the patch does not act
   (the iOS export's run) the row offers nothing to save. On the effects page of the other versions (TOTK 1.4.3 has its own). */
#ifndef TOTK_GAME_V143
static TOTKSettingRow *settings_botw_shadow_row(void) {
 NSString *t=settings_text("阴影分辨率","Shadow resolution");
#ifndef GAME_BOTW190
 return settings_fixed(t,settings_text("这个版本没有","Not in this version"));
#else
 return settings_botw190_shadow_row(t);   /* botw_effects.h */
#endif
}
/* The field of view (Breath of the Wild 1.9.0, botw_fov.h): as 1.4.3's row, in hundredths of the game's own, at once; from
   1x up only (1x to 1.5x is the useful range, PR #1575). A file or TOTK_FOV_SCALE value outside it is still shown. */
static TOTKSettingRow *settings_botw_fov_row(void) {
 NSString *t=settings_text("视野（FOV）","Field of view (FOV)");
#ifndef GAME_BOTW190
 return settings_fixed(t,settings_text("这个版本没有","Not in this version"));
#else
 if(!runtime_fov_available())return settings_fixed(t,settings_text("这个版本没有","Not in this version"));
 NSString *(^text)(int)=^NSString *(int v){return v==100?settings_text("1×（游戏原样）","1x (as the game has it)"):[NSString stringWithFormat:@"%g×",v/100.0];};
 int now=(int)lround(runtime_fov_scale()*100);
 if(runtime_fov_scale_locked())return settings_fixed(t,text(now));
 NSMutableArray<NSNumber *> *values=[@[@100,@110,@120,@130,@140,@150] mutableCopy];
 if(![values containsObject:@(now)])[values addObject:@(now)];
 NSMutableArray<NSString *> *choices=[NSMutableArray array];for(NSNumber *v in values)[choices addObject:text(v.intValue)];
 return settings_row(t,choices,values,^int{return (int)lround(runtime_fov_scale()*100);},^(int v){runtime_fov_scale_set(v/100.0);settings_save_int("graphics-fov.txt",v,"field of view");});
#endif
}
#endif

static NSArray<TOTKSettingRow *> *settings_rows(void) {
 NSMutableArray<TOTKSettingRow *> *rows=[NSMutableArray array];
 /* Display */
 [rows addObject:settings_row(settings_text("性能信息","Overlay"),
  @[settings_text("全部（FPS、着色器、读取、游玩时间）","All (FPS, shaders, load, playtime)"),
    settings_text("FPS 全信息（另加卡顿、着色器库、管线、存档没命中）","FPS full information (also hitches, libraries, pipelines, archive misses)"),   // user 2026-11-16
    settings_text("FPS 与游玩时间","FPS + playtime"),settings_on_off(0)],
  @[@(HUD_MODE_ALL),@(HUD_MODE_FULL),@(HUD_MODE_MINIMAL),@(HUD_MODE_OFF)],
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
 /* GPU keep-alive (nvn_mtl.m keepalive_start): short spin kernels hold the GPU clock up between frames. A Mac whose GPU
    keeps its clock with the game's own work gains no frame from it, only heat and power (an M4 Max: ~90 % GPU busy with it,
    ~63 % without, the same frames), so the player can turn it off. */
 {NSString *t=settings_text("GPU 保活（防降频，更热更耗电）","GPU keep-alive (holds the clock up; hotter, more power)");
  if(dm_gfx_keepalive_locked())[rows addObject:settings_fixed(t,settings_on_off(dm_gfx_keepalive()))];
  else [rows addObject:settings_row(t,nil,nil,^int{return dm_gfx_keepalive();},
   ^(int v){dm_gfx_keepalive_set(v);runtime_shared_setting_save("gpu-keepalive.txt",v?"on":"off","GPU keep-alive");})];}
 int modes=runtime_output_modes();
 NSMutableArray<NSString *> *res=[NSMutableArray array];NSMutableArray<NSNumber *> *resModes=[NSMutableArray array];
 static const int order[]={OUTPUT_MODE_720P,OUTPUT_MODE_720P_METALFX,OUTPUT_MODE_OFF,OUTPUT_MODE_METALFX,OUTPUT_MODE_NATIVE,OUTPUT_MODE_NATIVE_METALFX,OUTPUT_MODE_NATIVE_4K,OUTPUT_MODE_4K_TO_5K_METALFX,OUTPUT_MODE_NATIVE_5K,
  OUTPUT_MODE_NATIVE_4K_METALFX,OUTPUT_MODE_NATIVE_6K};   /* the last two: VR only (game_patches_v143.c vr_big_mode) */
 for(unsigned i=0;i<sizeof order/sizeof *order;i++){
  int m=order[i];if(!(modes&1<<m))continue;unsigned nw=0,nh=0;runtime_output_native(m,&nw,&nh);
  [res addObject:m==OUTPUT_MODE_720P?@"720p":m==OUTPUT_MODE_720P_METALFX?settings_text("720p + MetalFX 放大","720p + MetalFX upscaling"):m==OUTPUT_MODE_OFF?@"1080p":m==OUTPUT_MODE_METALFX?settings_text("1080p + MetalFX 放大","1080p + MetalFX upscaling"):
   m==OUTPUT_MODE_NATIVE_METALFX?settings_text("2K + MetalFX 放大（4K 屏）","2K + MetalFX upscaling (4K screens)"):
   m==OUTPUT_MODE_4K_TO_5K_METALFX?settings_text("4K + MetalFX 放大到 5K","4K + MetalFX to 5K"):
   m==OUTPUT_MODE_NATIVE_5K?settings_text("原生 5K（5120×2880）","Native 5K (5120×2880)"):
   m==OUTPUT_MODE_NATIVE_4K?settings_text("原生 4K（3840×2160）","Native 4K (3840×2160)"):
   m==OUTPUT_MODE_NATIVE_4K_METALFX?settings_text("4K + MetalFX 8K","4K + MetalFX 8K"):
   m==OUTPUT_MODE_NATIVE_6K?settings_text("原生 6K（约 20 帧）","Native 6K (about 20 fps)"):
   nw==2560&&nh==1440?settings_text("2K（2560×1440）","2K (2560×1440)"):
   [NSString stringWithFormat:@(TOTK_UI("原生 %u×%u","Native %u×%u")),nw,nh]];
  [resModes addObject:@(m)];}
 NSString *resolution=settings_text("分辨率","Resolution");
 int vr_eye_sizing=0;
#if defined(GAME_BOTW160)
 vr_eye_sizing=vr_enabled();
#endif
 if(!vr_eye_sizing){
#if defined(GAME_DKB) || defined(GAME_POKOPIA)
 (void)resolution;
 [rows addObject:settings_ns2_resolution_row()];
#else
 if(res.count<2)[rows addObject:settings_fixed(resolution,@"1080p")];
 else [rows addObject:settings_row(resolution,res,resModes,
  ^int{return runtime_output_mode();},^(int v){settingsPendingOutput=-1;if(runtime_output_mode_set(v)){settings_window_for(v);settings_save_int("graphics-output.txt",v,"resolution");}})];
#endif
 NSString *aspect=settings_text("画面比例","Aspect ratio");
 if(!(runtime_aspect_ratio()>0))[rows addObject:settings_fixed(aspect,@"16:9")];
 else{NSMutableArray<NSNumber *> *ratios=[@[@(16.0/9),@(16.0/10),@(21.0/9),@(32.0/9)] mutableCopy];
#if defined(GAME_DKB) || defined(GAME_POKOPIA)
  float start=runtime_aspect_ratio();
#else
  float start=runtime_aspect_start();
#endif
  BOOL listed=NO;   /* Include the game's custom ratio as one more choice. */
  for(NSNumber *r in ratios)listed|=aspect_near(start,r.doubleValue);
  if(start>0&&!listed)[ratios addObject:@(start)];
  NSMutableArray<NSString *> *choices=[NSMutableArray array];NSMutableArray<NSNumber *> *values=[NSMutableArray array];
  for(NSUInteger i=0;i<ratios.count;i++){[choices addObject:settings_ratio_text(ratios[i].floatValue)];[values addObject:@((int)i)];}
  [rows addObject:settings_row(aspect,choices,values,^int{
    float now=runtime_aspect_ratio();for(NSUInteger i=0;i<ratios.count;i++)if(aspect_near(now,ratios[i].doubleValue))return (int)i;return -1;},
   ^(int v){float r=ratios[(NSUInteger)v].floatValue;settingsPendingAspect=0;if(runtime_aspect_set_ratio(r)){
#if defined(GAME_DKB) || defined(GAME_POKOPIA)
    settings_ns2_save_size();dispatch_async(dispatch_get_main_queue(),^{settings_rebuild();});
#else
    runtime_front_resize_aspect(r);char word[32];snprintf(word,sizeof word,"%.8g",r);runtime_setting_save("graphics-aspect.txt",word,"aspect ratio");
#endif
   }})];}
 }
 /* Full screen around the notch (frontend.m): off by default; takes effect at the next full screen. */
 NSString *notch=settings_text("全屏覆盖刘海两侧","Full screen around the notch");
 if(front_notch_locked())[rows addObject:settings_fixed(notch,settings_text("由 TOTK_NOTCH 设置","Set by TOTK_NOTCH"))];
 else [rows addObject:settings_row(notch,nil,nil,^int{return front_notch_enabled();},^(int v){front_notch_set(v);})];
 NSUInteger display=rows.count;
 /* Input */
 TOTKSettingRow *pad=settings_row(settings_text("手柄类型","Controller type"),@[settings_pad_auto_text(),@"Xbox",@"PlayStation",@"Switch"],
  @[@(GAMEPAD_TYPE_AUTO),@(GAMEPAD_TYPE_XBOX),@(GAMEPAD_TYPE_PLAYSTATION),@(GAMEPAD_TYPE_SWITCH)],
  ^int{return runtime_gamepad_type(NULL);},^(int v){runtime_gamepad_type_set(v);});
 pad.firstTitle=^NSString *{return settings_pad_auto_text();};[rows addObject:pad];
#if defined(TOTK_GAME_V143)
 [rows addObject:settings_row(settings_text("按键提示图标","Button prompts"),
  @[settings_text("自动","Auto"),settings_text("键盘和鼠标","Keyboard / mouse"),@"Xbox",@"PlayStation",@"Switch",settings_text("关闭（游戏原图标）","Off (the game's icons)")],
  @[@(PROMPT_AUTO),@(PROMPT_KEYBOARD),@(PROMPT_XBOX),@(PROMPT_PLAYSTATION),@(PROMPT_SWITCH),@(PROMPT_STYLE_OFF)],
  ^int{return runtime_prompt_style();},^(int v){runtime_prompt_style_set(v);})];
#endif
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
 settings_dsu_rows(rows);
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
 /* Edge smoothing: off, fast (a classic TAA resolve in one compute pass) or MetalFX (its temporal network; on macOS 27 about
    4 ms of GPU a frame at 1080p on an M3 Pro, the fast one well under half of that). graphics-taa.txt keeps on / off and
    graphics-taa-mode.txt the resolve. After ishaanzee's fork (94f13c98). */
 [rows addObject:settings_row(settings_text("边缘平滑","Edge smoothing"),
  @[settings_on_off(0),settings_text("快速","Fast"),@"MetalFX"],@[@0,@1,@2],
  ^int{return taa_enabled()?(taa_fast_enabled()?1:2):0;},
  ^(int v){if(v)taa_set_fast(v==1);taa_set(v!=0);settings_save_int("graphics-taa.txt",v!=0,"edge smoothing");
           if(v)runtime_setting_save("graphics-taa-mode.txt",v==1?"fast":"metalfx","edge smoothing mode");})];
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
  {-1,"设置和截图","Settings & screenshots",NULL},
  {SETTINGS_BIND_PANEL,"打开 / 关闭设置面板","Open / close this panel",NULL},
  {KEYMAP_SCREENSHOT,"截图","Screenshot",NULL}};
 *count=(int)(sizeof list/sizeof *list);
 return list;
}
#elif SETTINGS_GAME_PART == 2
#ifdef TOTK_GAME_V143
/* Changing method adds, removes or replaces the quality row. Preserve the selected row, held-controller state and
   scroll distance from the top; rebuilding must not jump the player back to FOV or repeat a held button. */
static void settings_aa_rebuild(void) {
 if(!window||!settingsOpen)return;
 TOTKSettingsPanel *old=settingsPanel;
 NSInteger index=old.controllerIndex,footer=index-(NSInteger)old.rows.count;
 NSString *selected=index>=0&&index<(NSInteger)old.rows.count?old.rows[(NSUInteger)index].title:nil;
 NSView *page=old.pages[(NSUInteger)old.page];
 NSScrollView *scroll=[page isKindOfClass:NSScrollView.class]?(NSScrollView *)page:nil;
 CGFloat top=scroll?NSMaxY(scroll.documentView.bounds)-NSMaxY(scroll.contentView.bounds):0;
 settings_rebuild();
 settingsPanel.previousButtons=old.previousButtons;settingsPanel.previousDirection=old.previousDirection;
 settingsPanel.controllerArmed=old.controllerArmed;settingsPanel.repeatAt=old.repeatAt;
 if(footer>=0)settingsPanel.controllerIndex=(NSInteger)settingsPanel.rows.count+footer;
 else{
  BOOL found=NO;
  for(NSUInteger i=0;i<settingsPanel.rows.count;i++)if([settingsPanel.rows[i].title isEqualToString:selected]){settingsPanel.controllerIndex=(NSInteger)i;found=YES;break;}
  if(!found)for(NSUInteger i=0;i<settingsPanel.rows.count;i++)if([settingsPanel.rows[i].title isEqualToString:settings_text("抗锯齿方式","Anti-aliasing method")]){settingsPanel.controllerIndex=(NSInteger)i;break;}
 }
 page=settingsPanel.pages[(NSUInteger)settingsPanel.page];scroll=[page isKindOfClass:NSScrollView.class]?(NSScrollView *)page:nil;
 if(scroll){CGFloat y=fmax(0,NSMaxY(scroll.documentView.bounds)-scroll.contentSize.height-top);
  [scroll.contentView scrollToPoint:NSMakePoint(0,y)];[scroll reflectScrolledClipView:scroll.contentView];}
 [settingsPanel controllerHighlight];
}
#endif
#endif
