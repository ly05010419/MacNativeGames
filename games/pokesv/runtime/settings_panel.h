/* The settings panel (Mac; included by frontend.m): key 1 opens and closes it (runtime/input.c), Esc and its close button
   close it. Modelled on games/wonder's panel (rows, look), trimmed to what Violet has behind it (owner 2026-10-30):
     frame lock   off / 30 / 40 / 60 / 72 / 90 / 120   nvn_fps_lock_set (gfx/nvn/hle_nvn.c). Off keeps the game's own present
                                                      interval: 30 fps on a 60 Hz display, 60 on a 120 Hz one.
     resolution   1080p / 1080p + MetalFX            flags mfx_upscale and mfx_output (gfx/nvn/psv_mfx_up.m): the finished
                                                      1080p picture is upscaled by MetalFX to the window or full screen
                                                      instead of the bilinear stretch. Violet's own canvas stays 1920x1080:
                                                      a bigger one needs patches to the game's renderer, which this port
                                                      does not have yet. PSV_MFX_OUTPUT / PSV_MFX_UPSCALE decide first.
     interpolation switch                             MetalFX frame interpolation (gfx/nvn/psv_mfx.m, mode "half the
                                                      refresh rate"): the game renders every second display refresh and
                                                      MetalFX makes the frame between, 60 + 60 on a 120 Hz screen. PSV_MFX
                                                      decides first.
     overlay      switch                              the FPS and shader box (fps_overlay_toggle_hints)
   Each pick applies at once and is kept in settings.txt in the profile folder (PSV_PROFILE_DIR, else user-data), read
   again at the next start. While the panel is open the game gets no keyboard input. No controller navigation: use the
   mouse. */
#pragma once
#include "psv_fps_limit.h"   /* psv_fps_limits: the frame lock choices */

void dm_gfx_set_flag(const char *name, int value);
void nvn_fps_lock_set(int fps);
int nvn_fps_lock(void);
int mfxup_enabled(void);
int mtl_display_hz(void);
int fps_overlay_hints_enabled(void);
void runtime_shortcut_help_toggle(void);
int mfx_enabled(void);
void mfx_set(int mode);
enum { SETTINGS_INTERP_MODE = 2 };   /* MFX_HALF (gfx/nvn/psv_mfx_mode.h) */

static int settingsOpen;
static void settings_show(int open);

/* settings.txt: one "name value" line per setting. */
static void settings_path(char *out, size_t n) {
 const char *dir=getenv("PSV_PROFILE_DIR");snprintf(out,n,"%s/settings.txt",dir&&*dir?dir:"user-data");
}
static int settings_mfx_fixed(void) { return getenv("PSV_MFX_OUTPUT")||getenv("PSV_MFX_UPSCALE"); }
static int settings_mfx_on(void) { return mfxup_enabled()&&mfxup_output_enabled(); }
static void settings_mfx_set(int on) { dm_gfx_set_flag("mfx_upscale",on);dm_gfx_set_flag("mfx_output",on); }
static int settings_interp_fixed(void) { return getenv("PSV_MFX")!=NULL; }
static int settingsInterp=-1;   /* the switch: what was asked for (mfx_enabled changes at the next present) */
static int settings_interp_on(void) { return settingsInterp>=0?settingsInterp:mfx_enabled()!=0; }
static void settings_interp_set(int on) { settingsInterp=on!=0;mfx_set(on?SETTINGS_INTERP_MODE:0); }
static void settings_save(void) {
 char path[1024];settings_path(path,sizeof path);FILE *f=fopen(path,"w");
 if(!f){fprintf(stderr,"[psv] settings: cannot write %s\n",path);return;}
 fprintf(f,"frame-lock %d\nmetalfx %d\ninterp %d\noverlay %d\n",nvn_fps_lock(),settings_mfx_on(),settings_interp_on(),fps_overlay_hints_enabled());fclose(f);
}
/* Before the game starts: the frame lock and MetalFX. The overlay waits for its box (settings_restore_overlay). */
static int settingsSavedOverlay=-1;
static void settings_restore(void) {
 if(!nvn_fps_lock())nvn_fps_lock_set(30);   /* the game's own rate when nothing is saved: at 60 the scripts' fixed step runs twice as fast (README) (after PR #1655) */
 char path[1024],name[32];int v;settings_path(path,sizeof path);FILE *f=fopen(path,"r");if(!f)return;
 while(fscanf(f,"%31s %d",name,&v)==2){
  if(!strcmp(name,"frame-lock"))nvn_fps_lock_set(psv_fps_limit_valid(v)?v:30);   /* off, 40 and 72 are no longer offered: 30, the default */
  else if(!strcmp(name,"metalfx")&&(v==0||v==1)&&!settings_mfx_fixed())settings_mfx_set(v);
  else if(!strcmp(name,"interp")&&(v==0||v==1)&&!settings_interp_fixed())settings_interp_set(v);
  else if(!strcmp(name,"overlay")&&(v==0||v==1))settingsSavedOverlay=v;
 }
 fclose(f);fprintf(stderr,"[psv] settings: frame lock %d, MetalFX %d, interpolation %d (%s)\n",nvn_fps_lock(),settings_mfx_on(),settings_interp_on(),path);
}
static void settings_restore_overlay(void) {
 if(settingsSavedOverlay>=0&&settingsSavedOverlay!=fps_overlay_hints_enabled())runtime_shortcut_help_toggle();
}

static void settings_refresh_all(void);
@interface PSVSettingRow:NSObject
@property(copy) NSString *title;
@property(copy) NSArray<NSString *> *choices;   /* nil: an on / off switch */
@property(copy) NSArray<NSNumber *> *values;    /* what each choice sets */
@property(copy) int (^get)(void);
@property(copy) void (^set)(int value);
@property(strong) NSControl *control;
-(void)changed:(id)sender;
-(void)refresh;
@end
@implementation PSVSettingRow
-(void)changed:(id)sender{
 int v=[sender isKindOfClass:NSSwitch.class]?((NSSwitch *)sender).state==NSControlStateValueOn:self.values[(NSUInteger)MAX(0,((NSPopUpButton *)sender).indexOfSelectedItem)].intValue;
 if(self.set)self.set(v);
 settings_save();
 [self.control.window makeFirstResponder:self.control.window.contentView];   /* key 1 and Esc keep reaching the window */
 settings_refresh_all();
}
-(void)refresh{
 int v=self.get?self.get():0;
 if(!self.choices){((NSSwitch *)self.control).state=v?NSControlStateValueOn:NSControlStateValueOff;return;}
 NSPopUpButton *p=(NSPopUpButton *)self.control;NSUInteger i=[self.values indexOfObject:@(v)];
 if(i!=NSNotFound&&(NSInteger)i!=p.indexOfSelectedItem)[p selectItemAtIndex:(NSInteger)i];
}
@end

static NSString *settings_text(const char *zh, const char *en) { return @(PSV_UI(zh, en)); }
static PSVSettingRow *settings_row(NSString *title, NSArray<NSString *> *choices, NSArray<NSNumber *> *values, int (^get)(void), void (^set)(int)) {
 PSVSettingRow *r=[PSVSettingRow new];r.title=title;r.choices=choices;r.values=values;r.get=get;r.set=set;return r;
}
static NSArray<PSVSettingRow *> *settings_rows(void) {
 NSMutableArray<PSVSettingRow *> *rows=[NSMutableArray array];
 NSMutableArray<NSString *> *lockChoices=[NSMutableArray array];NSMutableArray<NSNumber *> *lockValues=[NSMutableArray array];
 for(unsigned i=0;i<PSV_FPS_LIMIT_COUNT;i++){
  int fps=psv_fps_limits[i];
  [lockChoices addObject:fps>PSV_FPS_SAFE_MAX?[NSString stringWithFormat:@(PSV_UI("%d 帧（警告：特效变慢，游戏可能出错）","%d fps (warning: effects run slow, may break the game)")),fps]:
   [NSString stringWithFormat:@(PSV_UI("%d 帧","%d fps")),fps]];
  [lockValues addObject:@(fps)];
 }
 [rows addObject:settings_row(settings_text("锁帧","Frame lock"),lockChoices,lockValues,^int{return nvn_fps_lock();},^(int v){nvn_fps_lock_set(v);})];
 NSString *res=settings_text("分辨率","Resolution");
 if(settings_mfx_fixed())[rows addObject:settings_row(res,@[settings_mfx_on()?settings_text("1080p + MetalFX（PSV_MFX_OUTPUT）","1080p + MetalFX (PSV_MFX_OUTPUT)"):@"1080p"],@[@0],^int{return 0;},nil)];
 else [rows addObject:settings_row(res,@[@"1080p",settings_text("1080p + MetalFX 放大到窗口","1080p + MetalFX upscaling to the window")],@[@0,@1],
  ^int{return settings_mfx_on();},^(int v){settings_mfx_set(v);})];
 NSString *fi=settings_text("补帧（60 → 120 帧，60 帧锁帧）","Frame interpolation (60 to 120 fps, with the 60 fps lock)");
 if(settings_interp_fixed())[rows addObject:settings_row(fi,@[mfx_enabled()?settings_text("开（PSV_MFX）","On (PSV_MFX)"):settings_text("关（PSV_MFX）","Off (PSV_MFX)")],@[@0],^int{return 0;},nil)];
 else [rows addObject:settings_row(fi,nil,nil,^int{return settings_interp_on();},^(int v){settings_interp_set(v);})];
 [rows addObject:settings_row(settings_text("性能信息","Overlay"),nil,nil,^int{return fps_overlay_hints_enabled();},
  ^(int v){if(v!=fps_overlay_hints_enabled())runtime_shortcut_help_toggle();})];
 return rows;
}

@interface PSVSettingsPanel:NSView
@property(strong) NSArray<PSVSettingRow *> *rows;
@property(strong) NSTimer *timer;
-(void)refresh;
-(void)center;
@end
@implementation PSVSettingsPanel
enum { SETTINGS_W = 520, SETTINGS_ROW_H = 34, SETTINGS_GAP = 6, SETTINGS_TOP = 54, SETTINGS_BOTTOM = 44, SETTINGS_POP_W = 290 };
-(instancetype)init{
 NSArray<PSVSettingRow *> *rows=settings_rows();CGFloat n=rows.count,h=SETTINGS_TOP+n*(SETTINGS_ROW_H+SETTINGS_GAP)-SETTINGS_GAP+SETTINGS_BOTTOM;
 if(!(self=[super initWithFrame:NSMakeRect(0,0,SETTINGS_W,h)]))return nil;
 self.rows=rows;self.appearance=[NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
 self.wantsLayer=YES;self.layer.backgroundColor=[NSColor colorWithCalibratedRed:0.10 green:0.16 blue:0.20 alpha:0.9].CGColor;
 self.layer.cornerRadius=12;self.layer.borderWidth=1;self.layer.borderColor=[NSColor colorWithCalibratedWhite:1 alpha:0.12].CGColor;
 self.autoresizingMask=NSViewMinXMargin|NSViewMaxXMargin|NSViewMinYMargin|NSViewMaxYMargin;
 NSTextField *title=[NSTextField labelWithString:settings_text("设置","Settings")];title.font=[NSFont systemFontOfSize:17 weight:NSFontWeightSemibold];
 title.textColor=NSColor.whiteColor;title.frame=NSMakeRect(18,h-38,300,24);[self addSubview:title];
 NSButton *close=[NSButton buttonWithTitle:@"✕" target:self action:@selector(close:)];close.bordered=NO;close.refusesFirstResponder=YES;
 close.font=[NSFont systemFontOfSize:15 weight:NSFontWeightBold];close.contentTintColor=NSColor.whiteColor;close.frame=NSMakeRect(SETTINGS_W-40,h-40,28,28);[self addSubview:close];
 for(NSUInteger i=0;i<rows.count;i++){
  PSVSettingRow *r=rows[i];CGFloat y=h-SETTINGS_TOP-(i+1)*SETTINGS_ROW_H-i*SETTINGS_GAP,rw=SETTINGS_W-28;
  NSView *band=[[NSView alloc] initWithFrame:NSMakeRect(14,y,rw,SETTINGS_ROW_H)];band.wantsLayer=YES;
  band.layer.backgroundColor=[NSColor colorWithCalibratedWhite:0 alpha:0.28].CGColor;band.layer.cornerRadius=6;[self addSubview:band];
  NSTextField *label=[NSTextField labelWithString:r.title];label.font=[NSFont systemFontOfSize:13 weight:NSFontWeightMedium];
  label.textColor=[NSColor colorWithCalibratedWhite:0.92 alpha:1];label.frame=NSMakeRect(12,floor((SETTINGS_ROW_H-18)/2),rw-SETTINGS_POP_W-24,18);[band addSubview:label];
  if(!r.choices){NSSwitch *s=[NSSwitch new];[s sizeToFit];s.frame=NSMakeRect(rw-12-s.frame.size.width,(SETTINGS_ROW_H-s.frame.size.height)/2,s.frame.size.width,s.frame.size.height);r.control=s;}
  else{NSPopUpButton *p=[[NSPopUpButton alloc] initWithFrame:NSMakeRect(rw-SETTINGS_POP_W-8,floor((SETTINGS_ROW_H-26)/2),SETTINGS_POP_W,26) pullsDown:NO];[p addItemsWithTitles:r.choices];
   p.enabled=r.set!=nil;r.control=p;}
  r.control.refusesFirstResponder=YES;r.control.target=r;r.control.action=@selector(changed:);[band addSubview:r.control];
 }
 NSTextField *hint=[NSTextField labelWithString:settings_text("按 1 或 Esc 关闭 · 打开时键盘不会传给游戏","1 or Esc closes it · the keyboard does not reach the game while it is open")];
 hint.font=[NSFont systemFontOfSize:11];hint.textColor=[NSColor colorWithCalibratedWhite:0.7 alpha:1];hint.alignment=NSTextAlignmentCenter;
 hint.frame=NSMakeRect(10,10,SETTINGS_W-20,18);[self addSubview:hint];
 [self refresh];
 __weak PSVSettingsPanel *weak=self;self.timer=[NSTimer scheduledTimerWithTimeInterval:0.5 repeats:YES block:^(NSTimer *t){(void)t;if(!weak.hidden)[weak refresh];}];
 return self;
}
-(void)refresh{for(PSVSettingRow *r in self.rows)[r refresh];}
-(void)close:(id)sender{(void)sender;settings_show(0);}
-(void)center{if(!self.superview)return;NSSize s=self.superview.bounds.size,p=self.frame.size;
 [self setFrameOrigin:NSMakePoint(floor(fmax(0,s.width-p.width)/2),floor(fmax(0,s.height-p.height)/2))];}
-(void)viewDidMoveToSuperview{[super viewDidMoveToSuperview];[self center];}
@end

static PSVSettingsPanel *settingsPanel;
static void settings_refresh_all(void) { [settingsPanel refresh]; }
static void settings_show(int open) {   /* main thread */
 open=!!open;
 if(!window||open==settingsOpen)return;
 if(open){[settingsPanel.timer invalidate];[settingsPanel removeFromSuperview];settingsPanel=[PSVSettingsPanel new];[window.contentView addSubview:settingsPanel];}
 settingsOpen=open;
 if(open){runtime_input_clear();[settingsPanel center];[settingsPanel refresh];}
 else [window makeFirstResponder:window.contentView];
 settingsPanel.hidden=!open;
 fprintf(stderr,"[psv] settings panel %s\n",open?"open":"closed");
}
void runtime_settings_toggle(void){dispatch_async(dispatch_get_main_queue(), ^{settings_show(!settingsOpen);});}
