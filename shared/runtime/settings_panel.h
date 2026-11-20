/* The settings panel (Mac; included by frontend.m): shared/runtime/settings_panel.h, one for every game that has it, each
   game's runtime/settings_panel.h a link to it. Key 1 opens and closes it (runtime/input.c), Esc and its close button
   close it. User 2026-10-25: it replaces the shortcut keys 2-7 and their reminder box. PR #1210: the pages (Display,
   Input, User, Effects, Key bindings, Update) are chosen by the tabs at the top, or L1 / R1 on a controller; the version
   in the bottom-left corner instead of the overlay box; no key hints at the bottom. One row per setting, a pop-up menu, a
   switch or a slider, each applied at once.
   What differs from game to game is the game's own, games/<game>/runtime/settings_game.h, read here twice
   (SETTINGS_GAME_PART 1 before the panel, 2 after it): the rows of its pages (settings_rows; games/totk's file lists Tears
   of the Kingdom's, Breath of the Wild's, Bananza's and Pokopia's), the key bindings' actions (settings_actions), what is
   restored at the start (settings_restore_start, settings_save_frame), its declarations and helpers, and which of the
   optional pages and features it has (0 or 1; unset is 0):
     SETTINGS_HAS_USERS     the User page and its Add User… button (users.h)
     SETTINGS_HAS_EFFECTS   the Effects page and its Reset to defaults (the game's settings_effects_reset)
     SETTINGS_HAS_BOARD     the 排行榜 page (board_tab.h, board_upload.h) and the board's code in 访问主页
     SETTINGS_HAS_SHOTS     the 截图分享 page (shot_tab.h, shot_upload.h) and its screenshot folder chooser
     SETTINGS_HAS_VR        the VR page while VR is on (zeldaVR), and the panel in the headset
     SETTINGS_HAS_AA_ROWS   effects rows that follow the anti-aliasing method: the panel is made again in place (the game's
                            settings_aa_rebuild, Tears of the Kingdom 1.4.3)
     SETTINGS_HAS_CONTRIB   the 着色器贡献 page (contrib_tab.h); unset is 1, every game had it before this switch. A game
                            whose shaders have no list to contribute to sets 0 (Animal Crossing): no tab, no contrib_tab.h
     SETTINGS_GAME_VERSION  the version the bottom-left corner shows (TOTK_GAME_VERSION unless the game sets it)
     SETTINGS_PANEL_ALPHA   the panel's background opacity (0.9 unless the game sets it)
   A page the game does not have has no tab, and L1 / R1 skip it; every page keeps its SETTINGS_PAGE_* number all the same
   (the input file's "settings <page>", self.pages[SETTINGS_PAGE_*]): settings_page_present, settings_page_tab and
   settings_tab_page go between the two.
   Key bindings: one row per game action with two keyboard and mouse inputs (keyboard_map.h, runtime_keymap_set: in force
   at once, saved in keyboard.txt) and the controller buttons that drive it (controller_map.h, runtime_controller_map_set:
   controls.txt). Click a box, then press a key or a mouse button (key 1 cancels; Command keys stay the system's) or, in
   the controller column, a controller button; with a controller, pick a row and press A, then the new button. An input
   another action holds swaps places with this box's. Right-click clears a box. Two buttons reset the keyboard and mouse,
   or the controller, to the default (the file removed). A capture ends after 6 seconds without input. The last row is the
   screenshot (user 2026-11-11): P by default, and on a controller a button held 3 s (the touchpad by default).
   着色器贡献 (user 2026-11-10, contrib_tab.h): how many shaders were contributed and how many new ones were found, and one
   big switch, "Contribute shaders automatically", the start-up question's choice (totk_contrib_upload.h). At the right of every page's bottom line, 访问主页 (user 2026-11-11, was the display page's star) opens the site
   with the player's codes (settings_home_url); its note shows for a few seconds where the version is.
   排行榜 (user 2026-11-11, board_tab.h): the latest frame-rate board of this game, and the switch "我要上榜", off by
   default and never asked; on, the play's frame numbers are sent and the player's own row is green (board_upload.h).
   截图分享 (user 2026-11-11, shot_tab.h): the game's shared screenshots, and the switch "分享我的截图", off by default and
   never asked; on, every saved screenshot is shared (shot_upload.h); the player's own are marked green and can be deleted.
   Its 截屏 button takes a screenshot as the capture button does: the game's next frame, without the panel.
   Both pages have the player's nickname (user 2026-11-11, nick_row.h): shown on the board and the screenshots instead of
   the anonymous code, typed in the game's text bar after 修改, and an empty one clears it; it turns on no upload switch.
   A setting this game or start cannot change shows its one value, greyed out. While the panel is open the game gets no
   keyboard, mouse or controller input (TOTKInputView, front_mouse_update and gamepad capture). The controls read their
   values again twice a second, so they always show what is in force. */
#pragma once
#include "../third_party/d2r_gfx/gfx/nvn/totk_shader_contrib.h"   /* the contribution file, TOTK_REPO_URL */
#include "../third_party/d2r_gfx/gfx/nvn/totk_contrib_upload.h"   /* the shader inbox: the 着色器贡献 page (contrib_tab.h) */
#include "keyboard_map.h"    /* the keyboard and mouse layout */
#include "controller_map.h"  /* the controller layout */

/* A number in one of the setting files next to controls.txt (runtime_setting_save / runtime_setting_word). */
static void settings_save_int(const char *name, int value, const char *what) {
 char word[32];snprintf(word,sizeof word,"%d",value);runtime_setting_save(name,word,what);
}
static int settings_read_int(const char *name, int *value) {
 char word[32],*end;if(!runtime_setting_word(name,word,sizeof word))return 0;
 long n=strtol(word,&end,10);while(*end==' '||*end=='\n'||*end=='\r'||*end=='\t')end++;
 if(end==word||*end||n<0||n>1000)return 0;*value=(int)n;return 1;
}
/* A number from a setting file, within lo..hi (1.4.3's graphics rows; Breath of the Wild 1.9.0's sharpening, PR #1637). */
static inline int settings_read_number(const char *name, double lo, double hi, double *value) {
 char word[32],*end;if(!runtime_setting_word(name,word,sizeof word))return 0;
 double d=strtod(word,&end);while(*end==' '||*end=='\n'||*end=='\r'||*end=='\t')end++;
 if(end==word||*end||!(d>=lo&&d<=hi))return 0;*value=d;return 1;
}

/* The pages, in tab order. A page added here is counted in perPage (the panel height), given its tab, and its rows a
   SETTINGS_PAGE_* in the game's settings_rows. VR (zeldaVR) is last and only there while VR is on (settings_pages); a page
   the game does not have (its SETTINGS_HAS_*) has no tab (settings_page_present). */
enum { SETTINGS_PAGE_DISPLAY, SETTINGS_PAGE_INPUT, SETTINGS_PAGE_USERS, SETTINGS_PAGE_EFFECTS, SETTINGS_PAGE_BINDINGS, SETTINGS_PAGE_CONTRIB, SETTINGS_PAGE_BOARD, SETTINGS_PAGE_SHOTS, SETTINGS_PAGE_OCARINA, SETTINGS_PAGE_VR, SETTINGS_PAGES };
static void settings_refresh_all(void);
static void settings_rebuild(void);
static void settings_pointer_used(void);
@interface TOTKSettingRow:NSObject
@property(copy) NSString *title;
@property(copy) NSArray<NSString *> *choices;   /* nil: an on / off switch, or a slider */
@property(copy) NSArray<NSNumber *> *values;    /* what each choice sets */
@property(copy) int (^get)(void);
@property(copy) void (^set)(int value);
@property(copy) NSString *(^firstTitle)(void);   /* a first choice whose text changes (auto: the controller it sees) */
@property(copy) NSString *(^text)(void);         /* a fixed text value beside a button, no picker (the screenshot folder) */
@property BOOL fullWidthText;                  /* a single status line across the whole row */
@property(copy) NSString *buttonTitle;           /* the button at the row's end (with text) */
@property(copy) void (^buttonAction)(void);
@property int sliderMin, sliderMax;              /* a slider when sliderMax > sliderMin */
@property NSInteger page;                        /* SETTINGS_PAGE_DISPLAY, _INPUT, _EFFECTS or _VR */
@property(strong) NSControl *control;
@property(strong) NSTextField *valueLabel;       /* a slider's value */
@property(strong) NSTextField *textLabel;        /* the field showing text() */
-(BOOL)slider;
-(void)changed:(id)sender;
-(void)refresh;
@end
@implementation TOTKSettingRow
-(BOOL)slider{return self.sliderMax>self.sliderMin;}
-(void)changed:(id)sender{
 if(self.buttonAction){self.buttonAction();return;}   /* the text row's button: it acts itself, no value comes back */
 int v=[sender isKindOfClass:NSSlider.class]?((NSSlider *)sender).intValue:[sender isKindOfClass:NSSwitch.class]?((NSSwitch *)sender).state==NSControlStateValueOn:
  self.values[(NSUInteger)MAX(0,((NSPopUpButton *)sender).indexOfSelectedItem)].intValue;
 if(self.set)self.set(v);
 if(self.valueLabel)self.valueLabel.stringValue=[NSString stringWithFormat:@"%d",v];
 [self.control.window makeFirstResponder:self.control.window.contentView];   /* key 1 and Esc keep reaching the window */
 settings_refresh_all();   /* a pick may change another row (frame lock / interpolation) */
}
-(void)refresh{
 if(self.text){NSString *t=self.text();
  if(![self.textLabel.stringValue isEqualToString:t]){self.textLabel.stringValue=t;self.textLabel.toolTip=t;}
  if(self.fullWidthText){CGFloat width=[t sizeWithAttributes:@{NSFontAttributeName:[NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular]}].width;
   CGFloat size=fmax(9,fmin(11,11*self.textLabel.bounds.size.width/fmax(1,width)));
   if(fabs(self.textLabel.font.pointSize-size)>.01)self.textLabel.font=[NSFont monospacedSystemFontOfSize:size weight:NSFontWeightRegular];}
  return;}
 int v=self.get?self.get():0;
 if(self.slider){NSSlider *s=(NSSlider *)self.control;if(s.intValue!=v)s.intValue=v;self.valueLabel.stringValue=[NSString stringWithFormat:@"%d",v];return;}
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
static NSString *settings_on_off(int on) { return on?settings_text("开","On"):settings_text("关","Off"); }

/* Helpers a game may leave unused (unused: no warning for a game without such a row, Animal Crossing). */
__attribute__((unused)) static NSString *settings_ratio_text(float r) {
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
 if(runtime_feedback_locked(gyro))return settings_fixed(t,settings_on_off(runtime_feedback_present(gyro)));
 if(!runtime_feedback_present(gyro))t=[t stringByAppendingString:settings_text("（下次启动生效）"," (from the next launch)")];
 return settings_row(t,nil,nil,^int{return runtime_feedback_choice(gyro);},^(int v){runtime_feedback_choose(gyro,v);});
}
/* Rumble strength in thousandths: 0.5x to 4x, and the strength in force when rumble-gain.txt holds another one. */
static int settings_gain_now(void) { return (int)fmax(1, lroundf(runtime_rumble_gain()*1000)); }
__attribute__((unused)) static TOTKSettingRow *settings_gain_row(void) {
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
/* Mouse mode's sensitivity: a slider from 1 to 100 (50: 20 points of movement turn the camera at full speed). */
__attribute__((unused)) static TOTKSettingRow *settings_sensitivity_row(void) {
 NSString *t=settings_text("鼠标灵敏度","Mouse sensitivity");
 if(runtime_mouse_sensitivity_locked())return settings_fixed(t,[NSString stringWithFormat:@"%d",runtime_mouse_sensitivity()]);
 TOTKSettingRow *r=settings_row(t,nil,nil,^int{return runtime_mouse_sensitivity();},^(int v){runtime_mouse_sensitivity_set(v);});
 r.sliderMin=1;r.sliderMax=100;return r;
}
/* Auto's text: the type it takes the connected controller for (runtime/gamepad.m). */
static NSString *settings_pad_auto_text(void) {
 static const char *const names[GAMEPAD_TYPE_COUNT]={NULL,"Xbox","PlayStation","Switch"};
 int detected=-1;runtime_gamepad_type(&detected);
 if(detected>GAMEPAD_TYPE_AUTO&&detected<GAMEPAD_TYPE_COUNT)return [NSString stringWithFormat:@(TOTK_UI("自动（%s）","Auto (%s)")),names[detected]];
 return settings_text("自动（未接手柄）","Auto (no controller)");
}

/* The panel (below) shows a note in its footer for a few seconds; a row's editor reports through these. */
@class TOTKSettingsPanel;
static TOTKSettingsPanel *settingsPanel;
static void settings_footer_note(NSString *text, BOOL bad) __attribute__((unused));   /* a game whose rows report nothing leaves it unused */

/* ---- Key bindings ---- */
/* The game actions in the order the page lists them; action -1 starts a section. */
typedef struct { int action; const char *zh, *en, *tag; } SettingsAction;
/* The key bindings' row for the settings panel itself: key 1 on the keyboard (fixed), on a controller L3 + R3 together or
   the button picked (gamepad.m runtime_gamepad_settings_button, user 2026-10-29). The screenshot row after it (user
   2026-11-11): two keyboard and mouse inputs as any action (keyboard_map.h KEYMAP_SCREENSHOT, P by default), and the
   controller button held 3 s (gamepad.m runtime_gamepad_screenshot_button, the touchpad by default), which keeps what
   else it drives. */
enum { SETTINGS_BIND_PANEL = 100 };

/* The game's part: its features (SETTINGS_HAS_*), declarations, restore at start, rows and key binding actions. */
#define SETTINGS_GAME_PART 1
#include "settings_game.h"
#undef SETTINGS_GAME_PART
#ifndef SETTINGS_HAS_USERS
#define SETTINGS_HAS_USERS 0
#endif
#ifndef SETTINGS_HAS_EFFECTS
#define SETTINGS_HAS_EFFECTS 0
#endif
#ifndef SETTINGS_HAS_BOARD
#define SETTINGS_HAS_BOARD 0
#endif
#ifndef SETTINGS_HAS_SHOTS
#define SETTINGS_HAS_SHOTS 0
#endif
#ifndef SETTINGS_HAS_VR
#define SETTINGS_HAS_VR 0
#endif
#ifndef SETTINGS_HAS_AA_ROWS
#define SETTINGS_HAS_AA_ROWS 0
#endif
#ifndef SETTINGS_HAS_OCARINA
#define SETTINGS_HAS_OCARINA 0   /* the 陶笛 page: songs played into the game's microphone (Ocarina of Time on the Mac) */
#endif
#ifndef SETTINGS_HAS_CONTRIB
#define SETTINGS_HAS_CONTRIB 1
#endif
#ifndef SETTINGS_GAME_VERSION
#define SETTINGS_GAME_VERSION TOTK_GAME_VERSION
#endif
#ifndef SETTINGS_PANEL_ALPHA
#define SETTINGS_PANEL_ALPHA 0.9
#endif
#if SETTINGS_HAS_BOARD
#include "board_upload.h"   /* the leaderboard: the 排行榜 page (board_tab.h) */
#endif
#if SETTINGS_HAS_SHOTS
#include "shot_upload.h"    /* screenshot sharing: the 截图分享 page (shot_tab.h) */
#endif
#if SETTINGS_HAS_USERS
#include "users.h"           /* the local users (runtime/account.c): the User page */
#endif

/* The tabs: the pages this game has, in SETTINGS_PAGE_* order; VR only while it is on. */
#if SETTINGS_HAS_VR
int vr_enabled(void);   /* zeldaVR/gfx/totk_vr.m: a headset link was asked for (TOTK_VR_CONTROL) */
#endif
static BOOL settings_page_present(NSInteger page) {
 switch(page){
  case SETTINGS_PAGE_USERS:return SETTINGS_HAS_USERS;
  case SETTINGS_PAGE_EFFECTS:return SETTINGS_HAS_EFFECTS;
  case SETTINGS_PAGE_BOARD:return SETTINGS_HAS_BOARD;
  case SETTINGS_PAGE_SHOTS:return SETTINGS_HAS_SHOTS;
  case SETTINGS_PAGE_CONTRIB:return SETTINGS_HAS_CONTRIB;
  case SETTINGS_PAGE_OCARINA:return SETTINGS_HAS_OCARINA;
#if SETTINGS_HAS_VR
  case SETTINGS_PAGE_VR:return vr_enabled()!=0;
#else
  case SETTINGS_PAGE_VR:return NO;
#endif
  default:return page>=0&&page<SETTINGS_PAGES;
 }
}
/* The number of tabs (L1 / R1 go round them), a page's tab and a tab's page. */
static int settings_pages(void){int n=0;for(NSInteger p=0;p<SETTINGS_PAGES;p++)n+=settings_page_present(p);return n;}
static NSInteger settings_page_tab(NSInteger page){NSInteger t=0;for(NSInteger p=0;p<page&&p<SETTINGS_PAGES;p++)t+=settings_page_present(p);return t;}
static NSInteger settings_tab_page(NSInteger tab){for(NSInteger p=0;p<SETTINGS_PAGES;p++)if(settings_page_present(p)&&tab--==0)return p;return SETTINGS_PAGE_DISPLAY;}
#if SETTINGS_HAS_AA_ROWS
static void settings_aa_rebuild(void);   /* the quality row follows the method without losing controller focus */
#endif
#if SETTINGS_HAS_USERS
static void settings_user_added(NSString *name);   /* the User page's add: runs after the global panel is made again */
#endif
#if SETTINGS_HAS_SHOTS
/* Where the capture button's pictures go (runtime/screenshot.h): a folder. The row shows the folder in force, and its
   button opens the system's own folder chooser (the panel the launcher opens for its game data); the chosen folder is
   checked before it is kept (screenshot_problem), so a read-only disk or another account's folder is refused here with
   its reason instead of failing at the next capture. The choice is saved next to controls.txt (runtime_setting_save),
   not in the working folder. The 截图分享 page shows the folder and calls this (user 2026-11-11: moved there from the
   input page). An iPhone and an iPad have no panel here: their pictures go into the photo library, in the album named
   after the game. */
static void settings_choose_screenshot_folder(void) {
 char now[PATH_MAX];screenshot_directory(now,sizeof now);
 NSOpenPanel *panel=[NSOpenPanel openPanel];
 panel.canChooseFiles=NO;panel.canChooseDirectories=YES;panel.allowsMultipleSelection=NO;
 panel.canCreateDirectories=YES;   /* New Folder, as Finder */
 panel.message=[NSString stringWithFormat:@(TOTK_UI("选择截图保存目录。当前：%s","Choose where screenshots are saved. Now: %s")),now];
 panel.prompt=settings_text("选择","Choose");
 [panel setDirectoryURL:[NSURL fileURLWithPath:@(now) isDirectory:YES]];
 if([panel runModal]!=NSModalResponseOK||!panel.URL)return;
 const char *why=NULL;int problem=SHOT_OK;uint64_t freeBytes=0;
 NSString *path=panel.URL.path;
 if(!screenshot_directory_set(path.UTF8String,&why,&problem,&freeBytes)){
  settings_footer_note([NSString stringWithFormat:@(TOTK_UI("未更改：%@（%@）","Not changed: %@ (%@)")),path,@(why?:"?")],YES);
  fprintf(stderr,"[totk] settings: screenshot folder %s refused: %s\n",path.UTF8String,why?why:"?");
  return;
 }
 char kept[PATH_MAX];screenshot_directory(kept,sizeof kept);
 settings_footer_note([NSString stringWithFormat:@(TOTK_UI("截图保存目录：%@","Screenshots: %@")),@(kept)],NO);
 fprintf(stderr,"[totk] settings: screenshot folder %s\n",kept);
 settings_refresh_all();   /* the row shows the folder in force (settings_refresh_all, below) */
}
#endif

static NSString *settings_input_text(int input) {
 if(input==KEYMAP_NONE)return @"—";
 switch(input-KEYMAP_MOUSE_FIRST){
  case 0:return settings_text("鼠标左键","Left click");
  case 1:return settings_text("鼠标右键","Right click");
  case 2:return settings_text("滚轮按下","Wheel click");
  case 3:return settings_text("鼠标侧键 4","Mouse 4");
  case 4:return settings_text("鼠标侧键 5","Mouse 5");
 }
 const char *label=keymap_input_label(input);
 return label?@(label):[NSString stringWithFormat:@"#%d",input];
}
/* The controller family the button names follow: the type picked, else the one auto sees, else Xbox. */
static int settings_pad_family(void) { int detected=-1,type=runtime_gamepad_type(&detected);if(!type)type=detected;return type>GAMEPAD_TYPE_AUTO?type:GAMEPAD_TYPE_XBOX; }
static NSString *settings_pad_button(int physical, int family) {
 static const char *const xbox[]={"A","B","X","Y","LS","RS","LB","RB","LT","RT","Menu","View"};
 static const char *const playstation[]={"×","○","□","△","L3","R3","L1","R1","L2","R2","Options","Create"};
 static const char *const nintendo[]={"B","A","Y","X","L3","R3","L","R","ZL","ZR","+","−"};   /* by label: east is A */
 static const char *const arrows[]={"←","↑","→","↓"};
 if(physical<12)return @((family==GAMEPAD_TYPE_PLAYSTATION?playstation:family==GAMEPAD_TYPE_SWITCH?nintendo:xbox)[physical]);
 if(physical<16)return [NSString stringWithFormat:@"%@ %@",settings_text("十字键","D-pad"),@(arrows[physical-12])];
 if(physical==16)return settings_text("触控板","Touchpad");
 return settings_input_text(KEYMAP_MOUSE_FIRST+physical-CONTROLLER_MOUSE_FIRST);   /* a mouse line in controls.txt */
}
/* What the controller column shows for an action: its buttons, or the stick a direction belongs to. */
/* The controller's way to the settings panel, as the key bindings show it. */
static NSString *settings_panel_pad_text(int family) {
 int b=runtime_gamepad_settings_button();
 if(b>=0)return settings_pad_button(b,family);
 return [NSString stringWithFormat:@"%@ + %@",settings_pad_button(4,family),settings_pad_button(5,family)];
}
/* The controller's screenshot button, as the key bindings show it: the button and how long it is held, or none. */
static NSString *settings_shot_pad_text(int family) {
 int b=runtime_gamepad_screenshot_button();
 if(b<0)return @"—";
 return [NSString stringWithFormat:@(TOTK_UI("%@ 长按 %d 秒","%@ held %d s")),settings_pad_button(b,family),CONTROLLER_SCREENSHOT_HOLD_MS/1000];
}
static NSString *settings_pad_text(int action, const uint16_t *map, int family) {
 if(action==KEYMAP_SCREENSHOT)return settings_shot_pad_text(family);
 if(action>=KEYMAP_MOVE_UP&&action<=KEYMAP_CAMERA_RIGHT){static const char *const arrows[]={"↑","↓","←","→"};int camera=action>=KEYMAP_CAMERA_UP;
  return [NSString stringWithFormat:@"%@ %@",camera?settings_text("右摇杆","Right stick"):settings_text("左摇杆","Left stick"),@(arrows[(action-KEYMAP_MOVE_UP)%4])];}
 NSMutableArray<NSString *> *names=[NSMutableArray array];
 for(int p=0;p<CONTROLLER_BUTTONS;p++)if(controller_map_target(map[p])==action)[names addObject:settings_pad_button(p,family)];
 return names.count?[names componentsJoinedByString:@" / "]:@"—";
}

#include "bindings_diagram.h"   /* the key bindings drawings */

@class TOTKSettingsPanel;
/* One box of the key bindings: slot 0 and 1 the keyboard and mouse inputs, 2 the controller. A right-click clears it. */
@interface TOTKBindingButton:NSButton
@property int gameAction, slot;   /* keyboard_map.h KEYMAP_*; 0-1 keyboard and mouse, 2 controller */
@property(weak) TOTKSettingsPanel *panel;
@end
@interface TOTKFlippedView:NSView
@end
@implementation TOTKFlippedView
-(BOOL)isFlipped{return YES;}
@end

@interface TOTKSettingsPanel:NSView
@property(strong) NSArray<TOTKSettingRow *> *allRows;   /* every page's rows */
@property(strong) NSArray<TOTKSettingRow *> *rows;      /* the page shown (none on the key bindings page) */
@property(strong) NSMutableArray<NSView *> *controllerTargets;   /* what a controller moves through on the page shown */
@property NSInteger page;
@property NSInteger controllerIndex;
#if SETTINGS_HAS_AA_ROWS
@property int aaMethod;
#endif
@property BOOL controllerArmed;
@property uint64_t previousButtons;
@property int previousDirection;
@property double repeatAt;
@property(strong) NSTimer *timer;
@property NSPoint grab;   /* where in the panel the drag started */
@property BOOL dragging;   /* the press being dragged was the panel's own */
@property(strong) NSSegmentedControl *tabs;
@property(strong) NSArray<NSView *> *pages;
@property(strong) NSTextField *versionLabel;   /* the bottom-left corner */
@property(strong) NSArray<NSButton *> *displayButtons;   /* the display page's own (none since 访问主页 is on every page) */
@property(strong) NSButton *homeButton;   /* every page: 访问主页 */
@property(strong) NSArray<NSButton *> *userButtons;   /* the user page: add a user */
@property(strong) NSArray<NSButton *> *resetButtons;   /* the key bindings page: keyboard and mouse, controller */
@property(strong) NSArray<NSButton *> *effectsButtons; /* the effects page: reset to defaults */
@property(strong) NSMutableArray<TOTKBindingButton *> *bindingButtons;
@property(strong) NSMutableArray<NSView *> *bindingTargets;   /* rows with controller buttons, then the reset buttons */
@property(strong) NSTextField *padHeader;
@property(strong) NSScrollView *bindingsScroll;
@property(strong) TOTKBindingButton *capturing;   /* the box waiting for an input */
@property double captureUntil;
@property(strong) id captureMonitor;
@property BOOL drawings;   /* the key bindings page shows the drawings (made at this size), not the list */
@property CGFloat panelW;
@property(strong) TOTKKeyboardDiagram *keyboardDiagram;
@property(strong) TOTKControllerDiagram *padDiagram;
@property(strong) NSSegmentedControl *deviceTabs;   /* keyboard and mouse, or controller */
@property(strong) NSTextField *drawingsHelp;
@property(strong) NSButton *listToggle;   /* "Show as list" / "Show drawings" */
@property(strong) NSArray<NSNumber *> *padOrder;   /* the controller buttons a controller moves through on the drawing */
@property(strong) id clickMonitor;   /* any click in the panel hides the controller's highlight */
-(void)selectPage:(NSInteger)page;
-(void)controllerPad:(RuntimeGamepadState)pad time:(double)now active:(BOOL)active;
-(void)controllerHighlight;
-(void)refresh;
-(void)refreshBindings;
-(void)beginCapture:(TOTKBindingButton *)box;
-(void)endCapture;
-(void)clearBinding:(TOTKBindingButton *)box;
-(void)assignInput:(int)input action:(int)action;
-(void)assignPad:(int)physical action:(int)action;
-(NSMenu *)menuForInput:(int)input pad:(BOOL)pad;
@end

@implementation TOTKBindingButton
-(void)rightMouseDown:(NSEvent *)e{(void)e;if(self.enabled)[self.panel clearBinding:self];}
@end

#if SETTINGS_HAS_CONTRIB
#include "contrib_tab.h"   /* the 着色器贡献 page */
#else
/* No 着色器贡献 page: 访问主页 (below) still opens the site's root (TOTK_STATS_URL for a test: a local one), and there is
   no contributor pseudonym to name. */
static NSURLComponents *contrib_tab_root(void) {
 const char *e=getenv("TOTK_STATS_URL");NSURLComponents *u=e&&*e?[NSURLComponents componentsWithString:@(e)]:nil;
 if(!u.host)return nil;
 u.path=@"/";u.query=nil;return u;
}
static NSString *contrib_tab_pseudonym(NSString *path) { (void)path; return nil; }
#endif
#if SETTINGS_HAS_BOARD || SETTINGS_HAS_SHOTS
#include "nick_row.h"   /* the player's nickname: a row on the 排行榜 and 截图分享 pages */
#endif
#if SETTINGS_HAS_BOARD
#include "board_tab.h"   /* the 排行榜 page */
#endif
#if SETTINGS_HAS_SHOTS
#include "shot_tab.h"   /* the 截图分享 page */
#endif

/* 访问主页 (user 2026-11-11): the site's home with this game, the view of the page shown (the 排行榜 page: the board, the
   着色器贡献 page: the contributors, the 截图分享 page: the photo wall) and the player's codes: the board's anonymous code
   (board_tab_code) and this game's contributor pseudonym (contrib_tab_pseudonym). Both are public names the site shows;
   the ids they come from never leave the Mac. The site keeps them so it knows who came (its 我的排名). */
static void settings_home_open_default(NSURL *url){if(url)[NSWorkspace.sharedWorkspace openURL:url];}
static void (*settings_home_open)(NSURL *)=settings_home_open_default;   /* the probe points it elsewhere: no browser in a test */
static NSURL *settings_home_url(NSInteger page){
 NSURLComponents *u=contrib_tab_root()?:[NSURLComponents componentsWithString:@"https://mac-native-games.com/"];
 u.path=@"/";
 NSMutableArray *q=[NSMutableArray arrayWithObject:[NSURLQueryItem queryItemWithName:@"game" value:@GAME_INBOX_ID]];
 if(page==SETTINGS_PAGE_BOARD)[q addObject:[NSURLQueryItem queryItemWithName:@"view" value:@"board"]];
 else if(page==SETTINGS_PAGE_CONTRIB)[q addObject:[NSURLQueryItem queryItemWithName:@"view" value:@"contrib"]];
#if SETTINGS_HAS_BOARD
 NSString *code=board_tab_code(),*player=contrib_tab_pseudonym(totk_contrib_path());
#else
 NSString *code=nil,*player=contrib_tab_pseudonym(totk_contrib_path());   /* no board: no anonymous code */
#endif
 if(code)[q addObject:[NSURLQueryItem queryItemWithName:@"code" value:code]];
 if(player)[q addObject:[NSURLQueryItem queryItemWithName:@"player" value:player]];
 u.queryItems=q;if(page==SETTINGS_PAGE_SHOTS)u.fragment=@"wall";
 return u.URL;
}

/* Where the player dragged the panel (user 2026-10-25: it can be moved), as the fraction of the free room left and below
   it, so a new opening and a resized window keep the spot; -1 = centred. The page shown last opens again. */
static NSPoint g_settings_at = {-1, -1};
static NSInteger g_settings_page = SETTINGS_PAGE_DISPLAY;
/* The key bindings page: the list instead of the drawings (the player's pick, or a window too small for them), and which
   drawing is shown (0 keyboard and mouse, 1 controller). */
static BOOL g_settings_bindings_list;
static NSInteger g_settings_bindings_device;
/* A controller moved through the panel since it opened: only then is its row highlighted (PR #1210: nothing looks
   selected before the player picks it); a mouse click hides the highlight again. */
static BOOL g_settings_controller_active;
enum { SETTINGS_WIDE_W = 960, SETTINGS_WIDE_H = 530 };
static BOOL settings_drawings_fit(void) {
 NSSize room=window?window.contentView.bounds.size:NSMakeSize(CGFLOAT_MAX,CGFLOAT_MAX);
 return room.width>=SETTINGS_WIDE_W+8&&room.height>=SETTINGS_WIDE_H+8;
}
static BOOL settings_drawings_for(NSInteger page) { return page==SETTINGS_PAGE_BINDINGS&&!g_settings_bindings_list&&settings_drawings_fit(); }
static NSString *settings_version_text(void) {
 const char *commit=getenv("TOTK_SOURCE_COMMIT");   /* the commit the app was built from (build_app.py, the launcher) */
 if(commit&&*commit)return [NSString stringWithFormat:@(TOTK_UI("版本 %s · 构建 %s","Ver. %s · build %s")),SETTINGS_GAME_VERSION,commit];
 return [NSString stringWithFormat:@(TOTK_UI("版本 %s","Ver. %s")),SETTINGS_GAME_VERSION];
}
@implementation TOTKSettingsPanel
/* SETTINGS_W: every tab whole in both languages, with VR's too (measured 2026-11-11, macOS 26, the real NSSegmentedControl:
   the English tabs with VR need about 700 pt and stay inside the bar's 14 pt margins from a 728 pt panel; narrower, the bar
   spills over its margins, and under about 708 pt labels are cut to "…"). A window too narrow for it gets a narrower panel,
   down to SETTINGS_NARROW_W (the width before the 截图分享 tab). */
enum { SETTINGS_W = 740, SETTINGS_NARROW_W = 600, SETTINGS_ROW_H = 34, SETTINGS_GAP = 6, SETTINGS_TOP = 90, SETTINGS_BOTTOM = 34, SETTINGS_POP_W = 260, SETTINGS_POP_MAX_W = 304, SETTINGS_LABEL_W = 350,
       SETTINGS_CELL_W = 118, SETTINGS_CELL_MAX_W = 140, SETTINGS_CELL_GAP = 6, SETTINGS_BIND_ROW_H = 28, SETTINGS_SECTION_H = 24 };
-(instancetype)init{
 NSArray<TOTKSettingRow *> *rows=settings_rows();
 /* The tallest page decides the panel's height (the key bindings page has its own drawing). */
 NSUInteger perPage[SETTINGS_PAGES]={0};for(TOTKSettingRow *r in rows)perPage[r.page]++;
 NSUInteger most=MAX(perPage[SETTINGS_PAGE_VR],perPage[SETTINGS_PAGE_OCARINA]);for(NSUInteger i=0;i<SETTINGS_PAGE_BINDINGS;i++)most=MAX(most,perPage[i]);
 /* The rows close up (down to 24 pt, 2 pt apart: a 26 pt menu then meets the next one's edge) when the panel would not fit the window (21:9 or 32:9 on a laptop
    screen), so its title, close button, tabs and first rows stay in view. */
 CGFloat n=most,rowH=SETTINGS_ROW_H,gap=SETTINGS_GAP,room=window?window.contentView.bounds.size.height-8:CGFLOAT_MAX;
 if(n&&SETTINGS_TOP+n*(rowH+gap)-gap+SETTINGS_BOTTOM>room){gap=2;rowH=fmax(24,floor((room-SETTINGS_TOP-SETTINGS_BOTTOM+gap)/n-gap));}
 /* The key bindings drawings need a wider, taller panel (PR #1210); it is made at that size while they are shown. */
 BOOL drawings=settings_drawings_for(g_settings_page);
 CGFloat roomW=window?window.contentView.bounds.size.width-8:CGFLOAT_MAX;   /* a narrower window: a narrower panel, kept inside */
 CGFloat body=n*(rowH+gap)-gap,h=SETTINGS_TOP+body+SETTINGS_BOTTOM,W=fmax(SETTINGS_NARROW_W,fmin(SETTINGS_W,roomW));
 if(!drawings&&h>room){h=room;body=fmax(0,h-SETTINGS_TOP-SETTINGS_BOTTOM);}   /* more controls: scroll rather than grow past the window */
 if(drawings){W=SETTINGS_WIDE_W;h=fmax(h,SETTINGS_WIDE_H);body=h-SETTINGS_TOP-SETTINGS_BOTTOM;}
 if(!(self=[super initWithFrame:NSMakeRect(0,0,W,h)]))return nil;
#if SETTINGS_HAS_AA_ROWS
 self.aaMethod=dm_gfx_aa_method();
#endif
 self.drawings=drawings;self.panelW=W;self.bindingButtons=[NSMutableArray array];self.bindingTargets=[NSMutableArray array];
 self.allRows=rows;self.controllerTargets=[NSMutableArray array];self.appearance=[NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
 self.wantsLayer=YES;self.layer.backgroundColor=[NSColor colorWithCalibratedRed:0.10 green:0.16 blue:0.20 alpha:SETTINGS_PANEL_ALPHA].CGColor;
 self.layer.cornerRadius=12;self.layer.borderWidth=1;self.layer.borderColor=[NSColor colorWithCalibratedWhite:1 alpha:0.12].CGColor;
 self.autoresizingMask=NSViewMinXMargin|NSViewMaxXMargin|NSViewMinYMargin|NSViewMaxYMargin;
 NSTextField *title=[NSTextField labelWithString:settings_text("设置","Settings")];title.font=[NSFont systemFontOfSize:17 weight:NSFontWeightSemibold];
 title.textColor=NSColor.whiteColor;title.frame=NSMakeRect(18,h-38,300,24);[self addSubview:title];
 NSButton *close=[NSButton buttonWithTitle:@"✕" target:self action:@selector(close:)];close.bordered=NO;close.refusesFirstResponder=YES;
 close.font=[NSFont systemFontOfSize:15 weight:NSFontWeightBold];close.contentTintColor=NSColor.whiteColor;close.frame=NSMakeRect(W-40,h-40,28,28);[self addSubview:close];
 NSArray<NSString *> *pageNames=@[settings_text("画面","Display"),settings_text("输入","Input"),settings_text("用户","User"),settings_text("画面效果","Effects"),settings_text("按键设置","Key bindings"),settings_text("着色器贡献","Shaders"),settings_text("性能排行榜","Performance"),settings_text("截图分享","Screenshots"),settings_text("陶笛","Ocarina"),@"VR"];   /* SETTINGS_PAGE_* order */
 NSMutableArray<NSString *> *tabNames=[NSMutableArray array];   /* the pages this game has (settings_page_present) */
 for(NSInteger p=0;p<SETTINGS_PAGES;p++)if(settings_page_present(p))[tabNames addObject:pageNames[(NSUInteger)p]];
 NSSegmentedControl *tabs=[NSSegmentedControl segmentedControlWithLabels:tabNames
  trackingMode:NSSegmentSwitchTrackingSelectOne target:self action:@selector(tabChanged:)];
 tabs.segmentDistribution=NSSegmentDistributionFillEqually;tabs.refusesFirstResponder=YES;tabs.frame=NSMakeRect(14,h-80,W-28,26);[self addSubview:tabs];self.tabs=tabs;
 NSMutableArray<NSView *> *pages=[NSMutableArray array];
 /* the menus widen with the panel up to 304 pt ("Handheld (less vegetation, faster, default)" whole) while the titles keep
    350 ("GPU keep-alive (holds the clock up; hotter, more power)" is 348); 260 in a narrower panel */
 CGFloat popW=fmax(SETTINGS_POP_W,fmin(SETTINGS_POP_MAX_W,W-28-24-SETTINGS_LABEL_W));
 NSView *(^rowPage)(NSInteger)=^NSView *(NSInteger p){   /* the pages of rows: display, input, effects and (zeldaVR) VR */
  NSUInteger count=0;for(TOTKSettingRow *r in rows)if(r.page==p)count++;
  CGFloat docH=fmax(body,count*(rowH+gap)-gap);
  NSView *page=[[NSView alloc] initWithFrame:NSMakeRect(0,SETTINGS_BOTTOM,W,docH)];NSUInteger k=0;
  for(TOTKSettingRow *r in rows){
   if(r.page!=p)continue;
   CGFloat y=docH-(k+1)*rowH-k*gap,rw=W-28;k++;
   NSView *band=[[NSView alloc] initWithFrame:NSMakeRect(14,y,rw,rowH)];band.wantsLayer=YES;
   band.layer.backgroundColor=[NSColor colorWithCalibratedWhite:0 alpha:0.28].CGColor;band.layer.cornerRadius=6;[page addSubview:band];
   NSTextField *label=[NSTextField labelWithString:r.title];label.font=[NSFont systemFontOfSize:13 weight:NSFontWeightMedium];
   label.textColor=[NSColor colorWithCalibratedWhite:0.92 alpha:1];label.frame=NSMakeRect(12,floor((rowH-18)/2),rw-popW-24,18);[band addSubview:label];
   if(r.slider){
    NSSlider *s=[NSSlider sliderWithValue:r.get?r.get():r.sliderMin minValue:r.sliderMin maxValue:r.sliderMax target:nil action:nil];
    s.continuous=YES;s.frame=NSMakeRect(rw-popW-8,floor((rowH-20)/2),popW-44,20);r.control=s;
    NSTextField *value=[NSTextField labelWithString:@""];value.font=[NSFont monospacedDigitSystemFontOfSize:13 weight:NSFontWeightMedium];
    value.textColor=label.textColor;value.alignment=NSTextAlignmentRight;value.frame=NSMakeRect(rw-46,floor((rowH-18)/2),34,18);[band addSubview:value];r.valueLabel=value;
   }else if(r.text){   /* a fixed value with the button that changes it at the row's end (the screenshot folder) */
    NSButton *b=[NSButton buttonWithTitle:r.buttonTitle?:@"" target:nil action:nil];
    b.controlSize=NSControlSizeSmall;b.font=[NSFont systemFontOfSize:11];[b sizeToFit];
    b.frame=NSMakeRect(rw-8-b.frame.size.width,floor((rowH-b.frame.size.height)/2),b.frame.size.width,b.frame.size.height);
    if(!r.buttonAction){b.hidden=YES;b.enabled=NO;b.frame=NSMakeRect(rw-8,0,0,0);}
    NSTextField *t=[NSTextField labelWithString:r.text()];t.font=[NSFont systemFontOfSize:11];t.textColor=label.textColor;
    t.lineBreakMode=NSLineBreakByTruncatingMiddle;   /* a long path keeps both of its ends */
    t.frame=NSMakeRect(rw-popW-8,floor((rowH-18)/2),popW-b.frame.size.width-14,18);
    if(r.fullWidthText){label.hidden=YES;t.frame=NSMakeRect(12,floor((rowH-18)/2),rw-24,18);t.font=[NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];t.lineBreakMode=NSLineBreakByTruncatingTail;}
    t.toolTip=t.stringValue;[band addSubview:t];r.textLabel=t;r.control=b;
   }else if(!r.choices){NSSwitch *s=[NSSwitch new];[s sizeToFit];s.frame=NSMakeRect(rw-12-s.frame.size.width,(rowH-s.frame.size.height)/2,s.frame.size.width,s.frame.size.height);r.control=s;}
   else{NSPopUpButton *pop=[[NSPopUpButton alloc] initWithFrame:NSMakeRect(rw-popW-8,floor((rowH-26)/2),popW,26) pullsDown:NO];[pop addItemsWithTitles:r.choices];
    pop.enabled=r.set!=nil;r.control=pop;}
   r.control.refusesFirstResponder=YES;r.control.target=r;r.control.action=@selector(changed:);[band addSubview:r.control];
  }
  if(docH>body){
   NSScrollView *scroll=[[NSScrollView alloc] initWithFrame:NSMakeRect(0,SETTINGS_BOTTOM,W,body)];
   scroll.hasVerticalScroller=YES;scroll.autohidesScrollers=YES;scroll.scrollerStyle=NSScrollerStyleOverlay;
   scroll.drawsBackground=NO;scroll.borderType=NSNoBorder;
   page.frame=NSMakeRect(0,0,W,docH);scroll.documentView=page;
   [self addSubview:scroll];[scroll.contentView scrollToPoint:NSMakePoint(0,docH-body)];[scroll reflectScrolledClipView:scroll.contentView];return scroll;
  }
  [self addSubview:page];return page;
 };
 for(NSInteger p=0;p<SETTINGS_PAGE_BINDINGS;p++)[pages addObject:rowPage(p)];
 NSRect bindingsFrame=NSMakeRect(0,SETTINGS_BOTTOM,W,body);
 NSView *bindings=drawings?[self drawingsPageWithFrame:bindingsFrame]:[self bindingsPageWithFrame:bindingsFrame];[self addSubview:bindings];[pages addObject:bindings];
#if SETTINGS_HAS_CONTRIB
 NSView *contrib=[self contribPageWithFrame:bindingsFrame];[self addSubview:contrib];[pages addObject:contrib];   /* contrib_tab.h */
#else
 [pages addObject:[[NSView alloc] initWithFrame:NSZeroRect]];   /* no 着色器贡献 page: its place kept */
#endif
#if SETTINGS_HAS_BOARD
 NSView *board=[self boardPageWithFrame:bindingsFrame];[self addSubview:board];[pages addObject:board];   /* board_tab.h */
#else
 [pages addObject:[[NSView alloc] initWithFrame:NSZeroRect]];   /* no 排行榜 page: its place kept, self.pages[SETTINGS_PAGE_*] */
#endif
#if SETTINGS_HAS_SHOTS
 NSView *shots=[self shotsPageWithFrame:bindingsFrame];[self addSubview:shots];[pages addObject:shots];   /* shot_tab.h */
#else
 [pages addObject:[[NSView alloc] initWithFrame:NSZeroRect]];   /* no 截图分享 page */
#endif
#if SETTINGS_HAS_OCARINA
 [pages addObject:rowPage(SETTINGS_PAGE_OCARINA)];   /* the game's rows on its 陶笛 page (settings_game.h) */
#else
 [pages addObject:[[NSView alloc] initWithFrame:NSZeroRect]];   /* no 陶笛 page: its place kept, self.pages[SETTINGS_PAGE_*] */
#endif
#if SETTINGS_HAS_VR
 if(vr_enabled())[pages addObject:rowPage(SETTINGS_PAGE_VR)];
#endif
 self.pages=pages;
 /* The bottom line: the version on the left (PR #1210; the overlay keeps it too, user 2026-10-25); the reset buttons on the right
    of the key bindings page. */
 NSTextField *version=[NSTextField labelWithString:settings_version_text()];version.font=[NSFont monospacedSystemFontOfSize:10.5 weight:NSFontWeightRegular];
 version.textColor=[NSColor colorWithCalibratedWhite:0.6 alpha:1];version.lineBreakMode=NSLineBreakByTruncatingTail;version.frame=NSMakeRect(18,9,230,16);[self addSubview:version];self.versionLabel=version;
 NSButton *resetPad=[NSButton buttonWithTitle:settings_text("恢复默认手柄按键","Reset controller") target:self action:@selector(resetController:)];
 NSButton *resetKeys=[NSButton buttonWithTitle:settings_text("恢复默认键鼠按键","Reset keyboard & mouse") target:self action:@selector(resetKeyboard:)];
 NSButton *toggle=[NSButton buttonWithTitle:drawings?settings_text("以列表显示","Show as list"):settings_text("以图示显示","Show drawings")
  target:self action:@selector(toggleList:)];
 if(!drawings&&!settings_drawings_fit()){toggle.enabled=NO;toggle.toolTip=settings_text("窗口太小，放不下图示","The window is too small for the drawings");}
 for(NSButton *b in @[resetPad,resetKeys,toggle]){b.controlSize=NSControlSizeSmall;b.font=[NSFont systemFontOfSize:11];b.refusesFirstResponder=YES;[b sizeToFit];
  b.frame=NSMakeRect(0,5,b.frame.size.width,24);b.wantsLayer=YES;[self addSubview:b];}
 self.resetButtons=@[resetKeys,resetPad];self.listToggle=toggle;
 /* Every page's bottom right (user 2026-11-11, was the display page's star): 访问主页, the site with the player's codes
    (settings_home_url), so it knows who came. */
 NSButton *home=[NSButton buttonWithTitle:settings_text("访问主页","Visit homepage") target:self action:@selector(visitHome:)];
 home.controlSize=NSControlSizeSmall;home.font=[NSFont systemFontOfSize:11];home.refusesFirstResponder=YES;[home sizeToFit];
 home.frame=NSMakeRect(0,5,home.frame.size.width,24);home.wantsLayer=YES;[self addSubview:home];self.homeButton=home;
 self.displayButtons=@[];
#if SETTINGS_HAS_EFFECTS
 NSButton *resetEffects=[NSButton buttonWithTitle:settings_text("恢复默认","Reset to defaults") target:self action:@selector(resetEffects:)];
 resetEffects.controlSize=NSControlSizeSmall;resetEffects.font=[NSFont systemFontOfSize:11];resetEffects.refusesFirstResponder=YES;[resetEffects sizeToFit];
 resetEffects.frame=NSMakeRect(0,5,resetEffects.frame.size.width,24);resetEffects.wantsLayer=YES;[self addSubview:resetEffects];
 self.effectsButtons=@[resetEffects];
#else
 self.effectsButtons=@[];
#endif
#if SETTINGS_HAS_USERS
 /* The user page's bottom right: name a new user in the game's text bar. */
 NSButton *addUser=[NSButton buttonWithTitle:settings_text("添加用户…","Add User…") target:self action:@selector(addUser:)];
 addUser.controlSize=NSControlSizeSmall;addUser.font=[NSFont systemFontOfSize:11];addUser.refusesFirstResponder=YES;[addUser sizeToFit];
 addUser.frame=NSMakeRect(0,5,addUser.frame.size.width,24);addUser.wantsLayer=YES;[self addSubview:addUser];
 self.userButtons=@[addUser];
#else
 self.userButtons=@[];
#endif
 [self selectPage:g_settings_page];[self refresh];
 __weak TOTKSettingsPanel *weak=self;
 self.clickMonitor=[NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskLeftMouseDown|NSEventMaskRightMouseDown handler:^NSEvent *(NSEvent *e){
  TOTKSettingsPanel *panel=weak;
  if(panel&&!panel.hidden&&e.window==panel.window&&NSPointInRect([panel convertPoint:e.locationInWindow fromView:nil],panel.bounds))settings_pointer_used();
  return e;}];
 self.timer=[NSTimer scheduledTimerWithTimeInterval:0.5 repeats:YES block:^(NSTimer *t){(void)t;
  TOTKSettingsPanel *panel=weak;if(!panel||panel.hidden)return;
  if(panel.capturing&&runtime_now_ns()/1e9>panel.captureUntil)[panel endCapture];
  [panel refresh];}];
 return self;
}
/* The key bindings page: a line of help, the column titles, and the actions in a scrolling list. */
-(NSView *)bindingsPageWithFrame:(NSRect)frame{
 NSView *page=[[NSView alloc] initWithFrame:frame];CGFloat w=frame.size.width,h=frame.size.height,bw=w-28;
 /* the boxes widen with the panel up to 140 pt ("Controller (PlayStation)" whole above them), the actions keep 200 */
 CGFloat cell=fmax(SETTINGS_CELL_W,fmin(SETTINGS_CELL_MAX_W,floor((bw-6-2*SETTINGS_CELL_GAP-200)/3)));
 CGFloat cx2=bw-6-cell,cx1=cx2-SETTINGS_CELL_GAP-cell,cx0=cx1-SETTINGS_CELL_GAP-cell;
 NSColor *dim=[NSColor colorWithCalibratedWhite:0.7 alpha:1];
 NSTextField *help=[NSTextField wrappingLabelWithString:settings_text(
  "点一个格子，再按键盘按键、鼠标按键或手柄按钮（按 1 取消）；右键点格子清除。已被占用的按键会互换。手柄：选中一行按 A，再按新按钮。",
  "Click a box, then press a key, a mouse button or a controller button (1 cancels); right-click clears it. An input in use swaps places. With a controller: pick a row, press A, then the new button.")];
 help.font=[NSFont systemFontOfSize:11];help.textColor=dim;help.frame=NSMakeRect(18,h-30,w-36,30);[page addSubview:help];
 NSString *titles[4]={settings_text("操作","Action"),settings_text("按键 1","Key 1"),settings_text("按键 2","Key 2"),@""};
 CGFloat xs[4]={24,14+cx0,14+cx1,14+cx2},ws[4]={cx0-16,cell,cell,cell};
 for(int i=0;i<4;i++){NSTextField *t=[NSTextField labelWithString:titles[i]];t.font=[NSFont systemFontOfSize:11 weight:NSFontWeightSemibold];t.textColor=dim;
  t.alignment=i?NSTextAlignmentCenter:NSTextAlignmentLeft;t.lineBreakMode=NSLineBreakByTruncatingTail;t.frame=NSMakeRect(xs[i],h-50,ws[i],16);[page addSubview:t];if(i==3)self.padHeader=t;}
 NSScrollView *scroll=[[NSScrollView alloc] initWithFrame:NSMakeRect(0,0,w,fmax(0,h-54))];
 scroll.hasVerticalScroller=YES;scroll.autohidesScrollers=YES;scroll.drawsBackground=NO;scroll.borderType=NSNoBorder;
 TOTKFlippedView *list=[[TOTKFlippedView alloc] initWithFrame:NSMakeRect(0,0,w,0)];
 int count;const SettingsAction *actions=settings_actions(&count);CGFloat y=0;
 for(int i=0;i<count;i++){
  const SettingsAction *a=&actions[i];
  if(a->action<0){NSTextField *t=[NSTextField labelWithString:@(TOTK_UI(a->zh,a->en))];t.font=[NSFont systemFontOfSize:12 weight:NSFontWeightSemibold];
   t.textColor=[NSColor colorWithCalibratedRed:0.7 green:0.95 blue:0.8 alpha:1];t.frame=NSMakeRect(20,y+5,bw,16);[list addSubview:t];y+=SETTINGS_SECTION_H;continue;}
  NSView *band=[[NSView alloc] initWithFrame:NSMakeRect(14,y,bw,SETTINGS_BIND_ROW_H-2)];band.wantsLayer=YES;
  band.layer.backgroundColor=[NSColor colorWithCalibratedWhite:0 alpha:0.28].CGColor;band.layer.cornerRadius=6;[list addSubview:band];
  NSMutableAttributedString *name=[[NSMutableAttributedString alloc] initWithString:@(TOTK_UI(a->zh,a->en))
   attributes:@{NSFontAttributeName:[NSFont systemFontOfSize:12.5 weight:NSFontWeightMedium],NSForegroundColorAttributeName:[NSColor colorWithCalibratedWhite:0.92 alpha:1]}];
  if(a->tag)[name appendAttributedString:[[NSAttributedString alloc] initWithString:[@"  " stringByAppendingString:@(a->tag)]
   attributes:@{NSFontAttributeName:[NSFont systemFontOfSize:11 weight:NSFontWeightRegular],NSForegroundColorAttributeName:[NSColor colorWithCalibratedWhite:0.6 alpha:1]}]];
  NSTextField *label=[NSTextField labelWithAttributedString:name];label.lineBreakMode=NSLineBreakByTruncatingTail;
  label.frame=NSMakeRect(10,floor((SETTINGS_BIND_ROW_H-2-17)/2),cx0-16,17);[band addSubview:label];
  CGFloat cx[3]={cx0,cx1,cx2};
  for(int slot=0;slot<3;slot++){
   TOTKBindingButton *box=[TOTKBindingButton buttonWithTitle:@"" target:self action:@selector(boxClicked:)];
   box.bordered=NO;box.wantsLayer=YES;box.layer.cornerRadius=5;box.refusesFirstResponder=YES;
   box.frame=NSMakeRect(cx[slot],3,cell,SETTINGS_BIND_ROW_H-8);
   box.panel=self;box.gameAction=a->action;box.slot=slot;
   box.enabled=a->action==SETTINGS_BIND_PANEL?slot==2:slot<2||a->action<KEYMAP_SWITCH_BUTTONS||a->action==KEYMAP_SCREENSHOT;
   [band addSubview:box];[self.bindingButtons addObject:box];
  }
  if(a->action<KEYMAP_SWITCH_BUTTONS||a->action==SETTINGS_BIND_PANEL||a->action==KEYMAP_SCREENSHOT)[self.bindingTargets addObject:band];
  y+=SETTINGS_BIND_ROW_H;
 }
 list.frame=NSMakeRect(0,0,w,y+4);scroll.documentView=list;[page addSubview:scroll];self.bindingsScroll=scroll;
 return page;
}
-(void)tabChanged:(NSSegmentedControl *)tabs{[self selectPage:settings_tab_page(tabs.selectedSegment)];}
-(void)selectPage:(NSInteger)page{
 [self endCapture];
 page=page<0?settings_tab_page(0):settings_page_present(page)?page:settings_tab_page(settings_pages()-1);   /* a page without a tab: the last tab's */
 /* the drawings need the panel made at their size: it is made again on the next turn of the run loop (not under this
    method's feet), on that page */
 if(settings_drawings_for(page)!=self.drawings){g_settings_page=page;dispatch_async(dispatch_get_main_queue(),^{settings_rebuild();});return;}
 self.page=page;g_settings_page=page;self.tabs.selectedSegment=settings_page_tab(page);
 for(NSInteger i=0;i<(NSInteger)self.pages.count;i++)self.pages[(NSUInteger)i].hidden=i!=page;
 NSMutableArray<TOTKSettingRow *> *rows=[NSMutableArray array];
 for(TOTKSettingRow *r in self.allRows)if(r.page==page)[rows addObject:r];
 self.rows=rows;self.controllerIndex=0;
#if SETTINGS_HAS_CONTRIB
 if(page==SETTINGS_PAGE_CONTRIB)[self refreshContribPage];   /* the number now, not at the next tick */
#endif
#if SETTINGS_HAS_BOARD
 if(page==SETTINGS_PAGE_BOARD)[self refreshBoardPage];   /* the board now (read again at most once a minute) */
#endif
#if SETTINGS_HAS_SHOTS
 if(page==SETTINGS_PAGE_SHOTS)[self refreshShotsPage];   /* the pictures now (read again at most once a minute) */
#endif
 [self layoutFooter];[self updateTargets];[self refreshBindings];
 [self.bindingsScroll.documentView scrollPoint:NSZeroPoint];   /* the list from its top (moving with a controller scrolls) */
 [self.window makeFirstResponder:self.window.contentView];   /* key 1 and Esc keep reaching the window */
}
/* The bottom right: 访问主页 on every page at the far right; before it the page's own: on the user page add a user, on
   the effects page the reset, on the key bindings page the list / drawings switch and the reset of the device shown (both
   resets with the list). The version at the bottom left gives way to them. */
-(NSArray<NSButton *> *)footerOwn{
 BOOL bindings=self.page==SETTINGS_PAGE_BINDINGS;
 return self.page==SETTINGS_PAGE_DISPLAY?self.displayButtons:self.page==SETTINGS_PAGE_USERS?self.userButtons:
  self.page==SETTINGS_PAGE_EFFECTS?self.effectsButtons:!bindings?@[]:self.drawings?@[self.resetButtons[g_settings_bindings_device?1:0],self.listToggle]:@[self.resetButtons[1],self.resetButtons[0],self.listToggle];
}
-(void)layoutFooter{
 NSArray<NSButton *> *shown=[self footerOwn];
 for(NSButton *b in [[[@[self.resetButtons[0],self.resetButtons[1],self.listToggle] arrayByAddingObjectsFromArray:self.displayButtons]
   arrayByAddingObjectsFromArray:self.effectsButtons] arrayByAddingObjectsFromArray:self.userButtons])b.hidden=![shown containsObject:b];
 self.homeButton.hidden=NO;
 CGFloat right=self.panelW-14;
 for(NSButton *b in [@[self.homeButton] arrayByAddingObjectsFromArray:shown]){b.frame=NSMakeRect(right-b.frame.size.width,5,b.frame.size.width,24);right-=b.frame.size.width+6;}
 if([self.versionLabel.stringValue isEqualToString:settings_version_text()])[self fitVersion];
}
/* The version where the buttons leave room (230 pt at most). */
-(void)fitVersion{self.versionLabel.frame=NSMakeRect(18,9,fmax(40,fmin(230,self.panelW-36-[self footerButtonsWidth])),16);}
/* A result of the display page's buttons, in the bottom-left corner for a few seconds instead of the version. */
-(void)note:(NSString *)text{
 self.versionLabel.stringValue=text;self.versionLabel.frame=NSMakeRect(18,9,self.panelW-36-[self footerButtonsWidth],16);
 __weak TOTKSettingsPanel *weak=self;NSString *shown=text;
 dispatch_after(dispatch_time(DISPATCH_TIME_NOW,(int64_t)(6*NSEC_PER_SEC)),dispatch_get_main_queue(),^{
  TOTKSettingsPanel *panel=weak;if(!panel||![panel.versionLabel.stringValue isEqualToString:shown])return;
  panel.versionLabel.stringValue=settings_version_text();[panel fitVersion];});
}
-(CGFloat)footerButtonsWidth{
 CGFloat w=self.homeButton.frame.size.width+6;for(NSButton *b in [self footerOwn])if(!b.hidden)w+=b.frame.size.width+6;return w;
}
/* The panel's note about a row: the tab of the page shown. */
static void settings_footer_note(NSString *text, BOOL bad) { (void)bad; [settingsPanel note:text]; }
/* The effects page's reset: every row back to the game's own; the panel is made again so the titles say what is in force. */
#if SETTINGS_HAS_EFFECTS
-(void)resetEffects:(id)sender{(void)sender;settings_effects_reset();dispatch_async(dispatch_get_main_queue(),^{settings_rebuild();});}
#endif
/* 访问主页 (user 2026-11-11, was the star): the site, opened on what the page shown is about, with the player's codes
   (settings_home_url) so the site knows who came. */
-(void)visitHome:(id)sender{(void)sender;NSURL *u=settings_home_url(self.page);
 fprintf(stderr,"[settings] 访问主页 %s\n",u.absoluteString.UTF8String?:"(no address)");if(settings_home_open)settings_home_open(u);
 [self note:settings_text("已在浏览器里打开主页","The homepage is open in the browser")];}
#if SETTINGS_HAS_USERS
/* The user page's button: the game's text bar takes the name (the panel closes while it is up, as it does for the
   game's own keyboards); the user is kept, and the panel comes back with the users read again. */
-(void)addUser:(NSButton *)button{
 (void)button;settings_pointer_used();
 __weak TOTKSettingsPanel *weak=self;
 dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0),^{
  RuntimeTextRequest req={TOTK_UI("给新用户取名（最多 10 个字；回车确认，Esc 取消）","Name the new user (at most 10 characters; Return confirms, Escape cancels)"),"",10,1,0,0,0};
  char name[128];int named=runtime_front_text_input(&req,name,sizeof name);
  NSString *typed=named&&name[0]?@(name):nil;   /* a block cannot capture the array itself */
  dispatch_async(dispatch_get_main_queue(),^{
   TOTKSettingsPanel *panel=weak;if(!panel)return;   /* the window is gone */
   settings_user_added(typed);
  });
 });
}
#endif
/* What a controller moves through: the rows; on the list, its rows and the buttons below; on the drawings, the device
   switch, the controller's buttons (the controller drawing) and the buttons below. */
-(void)updateTargets{
 [self.controllerTargets removeAllObjects];self.padOrder=@[];
 if(self.page==SETTINGS_PAGE_CONTRIB){
#if SETTINGS_HAS_CONTRIB
  [self.controllerTargets addObjectsFromArray:[self contribPageTargets]];
#endif
 }
#if SETTINGS_HAS_BOARD
 else if(self.page==SETTINGS_PAGE_BOARD)[self.controllerTargets addObjectsFromArray:[self boardPageTargets]];
#endif
#if SETTINGS_HAS_SHOTS
 else if(self.page==SETTINGS_PAGE_SHOTS)[self.controllerTargets addObjectsFromArray:[self shotsPageTargets]];
#endif
 else if(self.page!=SETTINGS_PAGE_BINDINGS){for(TOTKSettingRow *r in self.rows)[self.controllerTargets addObject:r.control.superview];
  NSArray<NSButton *> *foot=self.page==SETTINGS_PAGE_DISPLAY?self.displayButtons:self.page==SETTINGS_PAGE_USERS?self.userButtons:@[];
  for(NSButton *b in foot.reverseObjectEnumerator)if(!b.hidden)[self.controllerTargets addObject:b];
  for(NSButton *b in self.effectsButtons)if(!b.hidden)[self.controllerTargets addObject:b];}
 else if(!self.drawings){[self.controllerTargets addObjectsFromArray:self.bindingTargets];
  for(NSButton *b in @[self.resetButtons[0],self.resetButtons[1],self.listToggle])if(!b.hidden&&b.enabled)[self.controllerTargets addObject:b];}
 else{
  [self.controllerTargets addObject:self.deviceTabs];
  if(g_settings_bindings_device){self.padOrder=[self.padDiagram order];for(NSUInteger i=0;i<self.padOrder.count;i++)[self.controllerTargets addObject:self.padDiagram];}
  for(NSButton *b in @[self.resetButtons[0],self.resetButtons[1],self.listToggle])if(!b.hidden)[self.controllerTargets addObject:b];
 }
 [self.controllerTargets addObject:self.homeButton];   /* 访问主页, last on every page */
 if(self.controllerIndex>=(NSInteger)self.controllerTargets.count)self.controllerIndex=0;
 [self controllerHighlight];
}
/* The key bindings drawings: the device switch with a line of help, and the keyboard and mouse or the controller. */
-(NSView *)drawingsPageWithFrame:(NSRect)frame{
 NSView *page=[[NSView alloc] initWithFrame:frame];CGFloat w=frame.size.width,h=frame.size.height;
 NSSegmentedControl *device=[NSSegmentedControl segmentedControlWithLabels:@[settings_text("键盘和鼠标","Keyboard & mouse"),settings_text("手柄","Controller")]
  trackingMode:NSSegmentSwitchTrackingSelectOne target:self action:@selector(deviceChanged:)];
 device.refusesFirstResponder=YES;device.segmentDistribution=NSSegmentDistributionFillEqually;device.frame=NSMakeRect(18,h-28,330,24);
 device.selectedSegment=g_settings_bindings_device;[page addSubview:device];self.deviceTabs=device;
 NSTextField *help=[NSTextField labelWithString:@""];help.font=[NSFont systemFontOfSize:11];help.textColor=[NSColor colorWithCalibratedWhite:0.7 alpha:1];
 help.lineBreakMode=NSLineBreakByTruncatingTail;help.frame=NSMakeRect(362,h-25,w-380,16);[page addSubview:help];self.drawingsHelp=help;
 NSRect area=NSMakeRect(14,0,w-28,h-36);__weak TOTKSettingsPanel *weak=self;
 TOTKKeyboardDiagram *keys=[[TOTKKeyboardDiagram alloc] initWithFrame:area];
 keys.pick=^(int input,NSRect at){settings_pointer_used();[weak popMenuForInput:input pad:NO at:at];};
 TOTKControllerDiagram *pad=[[TOTKControllerDiagram alloc] initWithFrame:area];
 pad.pick=^(int physical,NSRect at){settings_pointer_used();[weak popMenuForInput:physical pad:YES at:at];};
 [page addSubview:keys];[page addSubview:pad];self.keyboardDiagram=keys;self.padDiagram=pad;
 [self showDevice];
 return page;
}
-(void)showDevice{
 BOOL pad=g_settings_bindings_device!=0;
 self.keyboardDiagram.hidden=pad;self.padDiagram.hidden=!pad;self.deviceTabs.selectedSegment=pad;
 self.drawingsHelp.stringValue=pad?settings_text("点图上的按钮，再选它的功能。用手柄：上下选按钮，左右改功能。",
   "Click a button on the drawing, then pick what it does. With a controller: up / down picks a button, left / right changes it.")
  :settings_text("点图上的按键或鼠标按键，再选它的功能。有颜色的按键正在使用。","Click a key or mouse button on the drawing, then pick what it does. Coloured keys are in use.");
}
-(void)deviceChanged:(NSSegmentedControl *)device{
 g_settings_bindings_device=device.selectedSegment;[self showDevice];[self layoutFooter];[self updateTargets];
 [self.window makeFirstResponder:self.window.contentView];
}
-(void)toggleList:(id)sender{(void)sender;g_settings_bindings_list=!g_settings_bindings_list;dispatch_async(dispatch_get_main_queue(),^{settings_rebuild();});}
/* The menu of actions for a key, mouse button or controller button, with the one it drives ticked and "None" last. */
-(NSMenu *)menuForInput:(int)input pad:(BOOL)pad{
 NSMenu *menu=[[NSMenu alloc] initWithTitle:@""];menu.autoenablesItems=NO;
 KeyMap keys;uint16_t map[CONTROLLER_BUTTONS];uint8_t modes[CONTROLLER_BUTTONS];int now;
 if(pad){runtime_controller_map_get(map,modes);now=runtime_gamepad_settings_button()==input?SETTINGS_BIND_PANEL:controller_map_target(map[input]);}else{runtime_keymap_get(&keys);now=keymap_action_of(&keys,input,NULL);}
 NSMenuItem *title=[[NSMenuItem alloc] initWithTitle:pad?settings_pad_button(input,settings_pad_family()):settings_input_text(input) action:nil keyEquivalent:@""];
 title.enabled=NO;[menu addItem:title];
 int count;const SettingsAction *all=settings_actions(&count);
 for(int i=0;i<count;i++){
  const SettingsAction *a=&all[i];
  if(pad&&a->action>=KEYMAP_SWITCH_BUTTONS&&a->action!=SETTINGS_BIND_PANEL&&a->action!=KEYMAP_SCREENSHOT)continue;   /* the sticks stay sticks on a controller */
  if(!pad&&a->action==SETTINGS_BIND_PANEL)continue;   /* key 1, fixed */
  if(pad&&input>CONTROLLER_SWITCH_BUTTONS&&(a->action==SETTINGS_BIND_PANEL||a->action==KEYMAP_SCREENSHOT))continue;   /* not a mouse button */
  if(a->action<0){[menu addItem:[NSMenuItem sectionHeaderWithTitle:@(TOTK_UI(a->zh,a->en))]];continue;}
  /* A controller's screenshot is a hold added to what the button drives: its own tick beside the action's. */
  BOOL hold=pad&&a->action==KEYMAP_SCREENSHOT;
  NSMenuItem *item=[[NSMenuItem alloc] initWithTitle:hold?[NSString stringWithFormat:@(TOTK_UI("截图（长按 %d 秒）","Screenshot (hold %d s)")),CONTROLLER_SCREENSHOT_HOLD_MS/1000]:@(TOTK_UI(a->zh,a->en))
   action:@selector(menuPicked:) keyEquivalent:@""];
  item.target=self;item.tag=a->action;item.representedObject=@[@(input),@(pad)];
  item.state=(hold?runtime_gamepad_screenshot_button()==input:a->action==now)?NSControlStateValueOn:NSControlStateValueOff;[menu addItem:item];
 }
 [menu addItem:[NSMenuItem separatorItem]];
 NSMenuItem *none=[[NSMenuItem alloc] initWithTitle:settings_text("无（不使用）","None (unused)") action:@selector(menuPicked:) keyEquivalent:@""];
 none.target=self;none.tag=-1;none.representedObject=@[@(input),@(pad)];
 none.state=now<0&&!(pad&&runtime_gamepad_screenshot_button()==input)?NSControlStateValueOn:NSControlStateValueOff;[menu addItem:none];
 return menu;
}
-(void)popMenuForInput:(int)input pad:(BOOL)pad at:(NSRect)at{
 NSView *view=pad?(NSView *)self.padDiagram:(NSView *)self.keyboardDiagram;
 if(pad)self.padDiagram.selected=input;else self.keyboardDiagram.selected=input;
 [view setNeedsDisplay:YES];[view displayIfNeeded];
 [[self menuForInput:input pad:pad] popUpMenuPositioningItem:nil atLocation:NSMakePoint(NSMinX(at),NSMaxY(at)+4) inView:view];   /* returns when closed */
 if(pad)self.padDiagram.selected=-1;else self.keyboardDiagram.selected=-1;
 [view setNeedsDisplay:YES];[self.window makeFirstResponder:self.window.contentView];
}
-(void)menuPicked:(NSMenuItem *)item{
 NSArray<NSNumber *> *what=item.representedObject;
 /* None on a controller button: nothing at all, its screenshot hold too */
 if(what[1].boolValue&&item.tag<0&&what[0].intValue==runtime_gamepad_screenshot_button())runtime_gamepad_screenshot_button_set(-1);
 if(what[1].boolValue)[self assignPad:what[0].intValue action:(int)item.tag];else [self assignInput:what[0].intValue action:(int)item.tag];
}
-(void)assignInput:(int)input action:(int)action{
 KeyMap keys;runtime_keymap_get(&keys);
 if(keymap_assign(&keys,input,action)){runtime_keymap_set(&keys);
  fprintf(stderr,"[totk] settings: %s -> %s\n",keymap_input_name(input),action<0?"nothing":keymap_action_name(action));}
 [self refreshBindings];
}
-(void)assignPad:(int)physical action:(int)action{
 if(action==KEYMAP_SCREENSHOT){if(physical<=CONTROLLER_SWITCH_BUTTONS)runtime_gamepad_screenshot_button_set(physical);[self refreshBindings];return;}   /* a hold: what it drives stays */
 if(action==SETTINGS_BIND_PANEL){if(physical<=CONTROLLER_SWITCH_BUTTONS)runtime_gamepad_settings_button_set(physical);[self refreshBindings];return;}
 if(physical==runtime_gamepad_settings_button())runtime_gamepad_settings_button_set(-1);   /* it drives the game again; the panel goes back to L3 + R3 */
 uint16_t map[CONTROLLER_BUTTONS];uint8_t modes[CONTROLLER_BUTTONS];runtime_controller_map_get(map,modes);
 controller_map_assign(map,modes,physical,action);runtime_controller_map_set(map,modes);
 fprintf(stderr,"[totk] settings: controller %s -> %s\n",controller_physical_names[physical],action<0?"nothing":controller_switch_names[action]);
 [self refreshBindings];
}
-(void)refresh{
#if SETTINGS_HAS_AA_ROWS
 if(self.aaMethod!=dm_gfx_aa_method()){
  self.aaMethod=dm_gfx_aa_method();dispatch_async(dispatch_get_main_queue(),^{settings_aa_rebuild();});
 }
#endif
 for(TOTKSettingRow *r in self.allRows)[r refresh];[self refreshBindings];
#if SETTINGS_HAS_CONTRIB
 if(self.page==SETTINGS_PAGE_CONTRIB)[self refreshContribPage];
#endif
#if SETTINGS_HAS_BOARD
 if(self.page==SETTINGS_PAGE_BOARD)[self refreshBoardPage];
#endif
#if SETTINGS_HAS_SHOTS
 if(self.page==SETTINGS_PAGE_SHOTS)[self refreshShotsPage];
#endif
}
-(void)refreshBindings{
 if(self.page!=SETTINGS_PAGE_BINDINGS)return;
 if(self.drawings){[self.keyboardDiagram setNeedsDisplay:YES];[self.padDiagram setNeedsDisplay:YES];return;}
 KeyMap keys;runtime_keymap_get(&keys);uint16_t map[CONTROLLER_BUTTONS];uint8_t modes[CONTROLLER_BUTTONS];runtime_controller_map_get(map,modes);
 int family=settings_pad_family();static const char *const families[GAMEPAD_TYPE_COUNT]={"","Xbox","PlayStation","Switch"};
 NSString *header=[NSString stringWithFormat:@(TOTK_UI("手柄（%s）","Controller (%s)")),families[family]];
 if(![self.padHeader.stringValue isEqualToString:header])self.padHeader.stringValue=header;
 NSMutableParagraphStyle *centered=[NSMutableParagraphStyle new];centered.alignment=NSTextAlignmentCenter;centered.lineBreakMode=NSLineBreakByTruncatingTail;
 for(TOTKBindingButton *box in self.bindingButtons){
  BOOL waiting=box==self.capturing;int action=box.gameAction;
  NSString *text=waiting?(box.slot==2?settings_text("请按手柄按钮…","Press a button…"):settings_text("请按键…","Press a key…")):
   action==SETTINGS_BIND_PANEL?(box.slot==0?@"1":box.slot==1?@"—":settings_panel_pad_text(family)):
   action==KEYMAP_SCREENSHOT&&box.slot==2?settings_shot_pad_text(family):
   box.slot<2?settings_input_text(keys.slot[action][box.slot]):settings_pad_text(action,map,family);
  NSColor *color=waiting?NSColor.blackColor:box.enabled?[NSColor colorWithCalibratedWhite:0.95 alpha:1]:[NSColor colorWithCalibratedWhite:0.55 alpha:1];
  NSAttributedString *title=[[NSAttributedString alloc] initWithString:text attributes:@{NSFontAttributeName:[NSFont systemFontOfSize:11.5 weight:NSFontWeightMedium],
   NSForegroundColorAttributeName:color,NSParagraphStyleAttributeName:centered}];
  if(![box.attributedTitle isEqualToAttributedString:title])box.attributedTitle=title;
  box.layer.backgroundColor=(waiting?[NSColor colorWithCalibratedRed:0.7 green:0.95 blue:0.8 alpha:1]:box.enabled?[NSColor colorWithCalibratedWhite:1 alpha:0.10]:[NSColor clearColor]).CGColor;
  box.toolTip=box.enabled?settings_text("点击后按新的输入；右键清除","Click, then press the new input; right-click clears"):nil;
 }
}
-(void)boxClicked:(TOTKBindingButton *)box{[self beginCapture:box];}
-(void)beginCapture:(TOTKBindingButton *)box{
 [self endCapture];if(!box.enabled)return;
 self.capturing=box;self.captureUntil=runtime_now_ns()/1e9+6;
 if(box.slot==2)runtime_gamepad_settings_enable(0);   /* the button that opens the panel may be the one being bound */
 if(box.slot<2){
  __weak TOTKSettingsPanel *weak=self;
  self.captureMonitor=[NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown|NSEventMaskFlagsChanged|NSEventMaskLeftMouseDown|NSEventMaskRightMouseDown|NSEventMaskOtherMouseDown
   handler:^NSEvent *(NSEvent *e){TOTKSettingsPanel *panel=weak;return panel?[panel captured:e]:e;}];
 }
 [self refreshBindings];
}
-(void)endCapture{
 if(self.captureMonitor){[NSEvent removeMonitor:self.captureMonitor];self.captureMonitor=nil;}
 if(!self.capturing)return;
 if(self.capturing.slot==2)runtime_gamepad_settings_enable(1);
 self.capturing=nil;[self refreshBindings];
}
/* A key, modifier or mouse button for the box waiting for one; nil swallows the event. */
-(NSEvent *)captured:(NSEvent *)e{
 TOTKBindingButton *box=self.capturing;if(!box||box.slot==2)return e;
 int input=-1;
 switch(e.type){
  case NSEventTypeKeyDown:
   if(e.modifierFlags&NSEventModifierFlagCommand)return e;   /* Command-Q, Command-W stay the system's */
   if(e.isARepeat)return nil;
   if(e.keyCode==KEYMAP_SETTINGS_KEY){[self endCapture];return nil;}
   input=e.keyCode;break;
  case NSEventTypeFlagsChanged:if(front_modifier_down(e)!=1)return nil;input=e.keyCode;break;
  case NSEventTypeLeftMouseDown:input=KEYMAP_MOUSE_FIRST;break;
  case NSEventTypeRightMouseDown:input=KEYMAP_MOUSE_FIRST+1;break;
  case NSEventTypeOtherMouseDown:input=e.buttonNumber<KEYMAP_MOUSE_BUTTONS?KEYMAP_MOUSE_FIRST+(int)e.buttonNumber:-1;break;
  default:return e;
 }
 if(input<0||!keymap_bindable(input))return nil;   /* not one the layout takes: keep waiting */
 [self bindInput:input];
 return nil;
}
-(void)bindInput:(int)input{
 TOTKBindingButton *box=self.capturing;if(!box||box.slot==2)return;
 KeyMap keys;runtime_keymap_get(&keys);
 if(keymap_bind(&keys,box.gameAction,box.slot,input)){runtime_keymap_set(&keys);
  fprintf(stderr,"[totk] settings: %s -> %s\n",keymap_input_name(input),keymap_action_name(box.gameAction));}
 [self endCapture];
}
/* A controller button (physical, controller_map.h) for the box waiting for one. The button another action had moves here,
   and that action takes the button this one had. */
-(void)bindPad:(int)physical{
 TOTKBindingButton *box=self.capturing;if(!box||box.slot!=2||physical<0||physical>=CONTROLLER_SWITCH_BUTTONS+1)return;
 if(box.gameAction==SETTINGS_BIND_PANEL){runtime_gamepad_settings_button_set(physical);[self endCapture];return;}
 if(box.gameAction==KEYMAP_SCREENSHOT){runtime_gamepad_screenshot_button_set(physical);[self endCapture];return;}   /* a hold: what it drives stays */
 if(physical==runtime_gamepad_settings_button())runtime_gamepad_settings_button_set(-1);
 uint16_t map[CONTROLLER_BUTTONS];uint8_t modes[CONTROLLER_BUTTONS];runtime_controller_map_get(map,modes);
 int previous=-1;for(int p=0;p<=CONTROLLER_SWITCH_BUTTONS;p++)if(controller_map_target(map[p])==box.gameAction){previous=p;break;}
 uint16_t displaced=map[physical];
 map[physical]=(uint16_t)(1u<<box.gameAction);modes[physical]=CONTROLLER_HOLD;
 if(previous>=0&&previous!=physical){map[previous]=displaced;modes[previous]=CONTROLLER_HOLD;}
 runtime_controller_map_set(map,modes);
 fprintf(stderr,"[totk] settings: controller %s -> %s\n",controller_physical_names[physical],controller_switch_names[box.gameAction]);
 [self endCapture];
}
-(void)clearBinding:(TOTKBindingButton *)box{
 [self endCapture];
 if(box.gameAction==SETTINGS_BIND_PANEL){if(box.slot==2)runtime_gamepad_settings_button_set(-1);}   /* back to L3 + R3 */
 else if(box.gameAction==KEYMAP_SCREENSHOT&&box.slot==2)runtime_gamepad_screenshot_button_set(-1);   /* no button takes screenshots */
 else if(box.slot<2){KeyMap keys;runtime_keymap_get(&keys);if(keymap_bind(&keys,box.gameAction,box.slot,KEYMAP_NONE))runtime_keymap_set(&keys);}
 else{uint16_t map[CONTROLLER_BUTTONS];uint8_t modes[CONTROLLER_BUTTONS];runtime_controller_map_get(map,modes);
  for(int p=0;p<CONTROLLER_BUTTONS;p++)if(controller_map_target(map[p])==box.gameAction){map[p]=0;modes[p]=CONTROLLER_HOLD;}
  runtime_controller_map_set(map,modes);}
 [self refreshBindings];
}
-(void)resetKeyboard:(id)sender{(void)sender;[self endCapture];runtime_keymap_reset();[self refreshBindings];}
-(void)resetController:(id)sender{(void)sender;[self endCapture];runtime_controller_map_reset();runtime_gamepad_settings_button_set(-1);
 runtime_gamepad_screenshot_button_set(CONTROLLER_SCREENSHOT_DEFAULT);[self refreshBindings];}
-(BOOL)controllerEnabled:(NSInteger)i{
 if(i<0||i>=(NSInteger)self.controllerTargets.count)return NO;
 if(self.page==SETTINGS_PAGE_BINDINGS)return YES;
 if(i<(NSInteger)self.rows.count){TOTKSettingRow *r=self.rows[i];return (r.set||r.buttonAction)&&r.control.enabled;}
 return ((NSControl *)self.controllerTargets[i]).enabled;   /* a button after the rows */
}
/* The controller's place: drawn only once a controller has moved in the panel (g_settings_controller_active). On the
   controller drawing it is the picked button (outlined there), not a border. */
-(void)controllerHighlight{
 if(![self controllerEnabled:self.controllerIndex]){
  for(NSUInteger i=0;i<self.controllerTargets.count;i++)if([self controllerEnabled:i]){self.controllerIndex=i;break;}
 }
 int physical=-1;
 for(NSUInteger i=0;i<self.controllerTargets.count;i++){
  NSView *v=self.controllerTargets[i];BOOL here=g_settings_controller_active&&(NSInteger)i==self.controllerIndex;
  if(v==self.padDiagram){if(here)physical=[self padPhysical:i];continue;}
  v.wantsLayer=YES;v.layer.cornerRadius=6;v.layer.borderWidth=here?2:0;
  v.layer.borderColor=[NSColor colorWithCalibratedRed:0.7 green:0.95 blue:0.8 alpha:1].CGColor;
 }
 if(self.padDiagram&&self.padDiagram.selected!=physical){self.padDiagram.selected=physical;[self.padDiagram setNeedsDisplay:YES];}
}
/* The controller button of target i on the controller drawing (its targets follow the device switch), -1 none. */
-(int)padPhysical:(NSUInteger)i{
 if(!self.padOrder.count||i<1||i>self.padOrder.count)return -1;
 return self.padOrder[i-1].intValue;
}
-(void)controllerMove:(int)step{
 NSInteger count=self.controllerTargets.count;
 for(NSInteger n=0;n<count;n++){
  self.controllerIndex=(self.controllerIndex+step+count)%count;
  if([self controllerEnabled:self.controllerIndex])break;
 }
 [self controllerHighlight];
 if(self.controllerIndex>=0&&self.controllerIndex<(NSInteger)self.controllerTargets.count){   /* keep rows in view on every scrolling page */
  NSView *v=self.controllerTargets[(NSUInteger)self.controllerIndex];if(![v isKindOfClass:NSButton.class])[v scrollRectToVisible:NSInsetRect(v.bounds,0,-SETTINGS_SECTION_H)];}
}
-(void)controllerChange:(int)step select:(BOOL)select{
 NSInteger i=self.controllerIndex;if(![self controllerEnabled:i])return;
 if(self.page==SETTINGS_PAGE_BINDINGS){   /* A on a row: wait for the new controller button; on a reset button: press it */
  NSView *target=self.controllerTargets[(NSUInteger)i];
  if(target==self.deviceTabs){self.deviceTabs.selectedSegment=!g_settings_bindings_device;[self deviceChanged:self.deviceTabs];return;}   /* the other device */
  int physical=target==self.padDiagram?[self padPhysical:(NSUInteger)i]:-1;
  if(physical>=0){   /* left / right (or A) go through the actions it can drive, then none */
   static const int cycle[]={KEYMAP_A,KEYMAP_B,KEYMAP_X,KEYMAP_Y,KEYMAP_ZL,KEYMAP_ZR,KEYMAP_L,KEYMAP_R,KEYMAP_LSTICK,KEYMAP_RSTICK,KEYMAP_PLUS,KEYMAP_MINUS,
    KEYMAP_DUP,KEYMAP_DDOWN,KEYMAP_DLEFT,KEYMAP_DRIGHT,SETTINGS_BIND_PANEL,-1};
   const int n=(int)(sizeof cycle/sizeof *cycle);
   uint16_t map[CONTROLLER_BUTTONS];uint8_t modes[CONTROLLER_BUTTONS];runtime_controller_map_get(map,modes);
   int now=runtime_gamepad_settings_button()==physical?SETTINGS_BIND_PANEL:controller_map_target(map[physical]),at=n-1;for(int k=0;k<n;k++)if(cycle[k]==now)at=k;
   [self assignPad:physical action:cycle[(at+(select?1:step)+n)%n]];return;
  }
  if(!select)return;
  if([target isKindOfClass:NSButton.class]){[(NSButton *)target performClick:nil];return;}
  for(TOTKBindingButton *box in self.bindingButtons)if(box.superview==target&&box.slot==2){[self beginCapture:box];return;}
  return;
 }
 if(i>=(NSInteger)self.rows.count){   /* a button after the rows */
  if(select)[(NSButton *)self.controllerTargets[i] performClick:nil];
  return;
 }
 TOTKSettingRow *r=self.rows[i];
 if(r.text&&!r.buttonAction)return;
 if(r.buttonAction){if(select)[r changed:r.control];[self controllerHighlight];return;}   /* the text row: A is its button */
 if(r.slider){if(select)return;NSSlider *s=(NSSlider *)r.control;s.intValue=MAX(r.sliderMin,MIN(r.sliderMax,s.intValue+step*5));}
 else if(r.choices){
  NSPopUpButton *p=(NSPopUpButton *)r.control;NSInteger n=p.numberOfItems;if(n<2)return;
  NSInteger index=p.indexOfSelectedItem;if(index<0)index=0;
  [p selectItemAtIndex:(index+step+n)%n];
 }else{
  NSSwitch *s=(NSSwitch *)r.control;
  s.state=select?(s.state==NSControlStateValueOn?NSControlStateValueOff:NSControlStateValueOn):(step>0?NSControlStateValueOn:NSControlStateValueOff);
 }
 [r changed:r.control];[self controllerHighlight];
}
-(void)controllerPad:(RuntimeGamepadState)pad time:(double)now active:(BOOL)active{
 if(!active||!pad.connected){self.controllerArmed=NO;self.previousButtons=0;self.previousDirection=0;return;}
 int direction=(pad.buttons&(1ull<<13)||pad.ly>16000)?1:(pad.buttons&(1ull<<15)||pad.ly < -16000)?2:
  (pad.buttons&(1ull<<12)||pad.lx < -16000)?3:(pad.buttons&(1ull<<14)||pad.lx>16000)?4:0;
 if(!self.controllerArmed){if(!pad.buttons&&!direction)self.controllerArmed=YES;self.previousButtons=pad.buttons;return;}
 uint64_t edges=pad.buttons&~self.previousButtons;self.previousButtons=pad.buttons;
 if(self.capturing&&self.capturing.slot==2){   /* the next button pressed is the new one */
  uint64_t pressed=edges&((1ull<<(CONTROLLER_SWITCH_BUTTONS+1))-1);
  if(pressed)[self bindPad:__builtin_ctzll(pressed)];
  self.previousDirection=direction;return;
 }
 int detected=-1,type=runtime_gamepad_type(&detected);if(!type)type=detected;
 const uint64_t confirm=type==GAMEPAD_TYPE_SWITCH?2u:1u,cancel=type==GAMEPAD_TYPE_SWITCH?1u:2u;
#if SETTINGS_HAS_SHOTS
 if((edges&(confirm|cancel))&&self.page==SETTINGS_PAGE_SHOTS&&[self shotsPageCloseViewer])return;   /* the whole picture over the 截图分享 page: A or B closes it */
#endif
 if(!g_settings_controller_active&&!(edges&cancel)&&(direction||(edges&confirm))){   /* the first move or A shows where the controller is, nothing more */
  g_settings_controller_active=YES;[self controllerHighlight];
  self.previousDirection=direction;self.repeatAt=now+0.4;return;
 }
 if(edges&(1ull<<6)){[self selectPage:settings_tab_page((settings_page_tab(self.page)+settings_pages()-1)%settings_pages())];return;}   /* L1 / R1: the tabs */
 if(edges&(1ull<<7)){[self selectPage:settings_tab_page((settings_page_tab(self.page)+1)%settings_pages())];return;}
 if(edges&cancel){settings_show(0);return;}
 if(direction&&(direction!=self.previousDirection||now>=self.repeatAt)){
  if(direction<=2)[self controllerMove:direction==1?-1:1];else [self controllerChange:direction==3?-1:1 select:NO];
  self.repeatAt=now+(direction==self.previousDirection?0.14:0.4);
 }
 self.previousDirection=direction;
 if(edges&confirm)[self controllerChange:1 select:YES];
}
-(void)close:(id)sender{(void)sender;settings_show(0);}
-(void)center{if(!self.superview)return;NSSize s=self.superview.bounds.size,p=self.frame.size;
 CGFloat fx=g_settings_at.x<0?0.5:g_settings_at.x,fy=g_settings_at.y<0?0.5:g_settings_at.y;
 [self setFrameOrigin:NSMakePoint(floor(fmax(0,s.width-p.width)*fx),floor(fmax(0,s.height-p.height)*fy))];}
/* Dragging the panel's background (not its controls) moves it, kept inside the window. A drag that started on a view
   keeping its press (the drawings) still reaches the panel: it moves only for a press it saw, or it jumps to a stale grab. */
-(void)mouseDown:(NSEvent *)e{self.grab=[self convertPoint:e.locationInWindow fromView:nil];self.dragging=YES;}
-(void)mouseUp:(NSEvent *)e{(void)e;self.dragging=NO;}
-(void)mouseDragged:(NSEvent *)e{if(!self.superview||!self.dragging)return;
 NSPoint at=[self.superview convertPoint:e.locationInWindow fromView:nil];NSSize s=self.superview.bounds.size,p=self.frame.size;
 CGFloat x=fmin(fmax(0,at.x-self.grab.x),fmax(0,s.width-p.width)),y=fmin(fmax(0,at.y-self.grab.y),fmax(0,s.height-p.height));
 [self setFrameOrigin:NSMakePoint(floor(x),floor(y))];
 g_settings_at=NSMakePoint(s.width>p.width?x/(s.width-p.width):0.5,s.height>p.height?y/(s.height-p.height):0.5);}
-(void)viewDidMoveToSuperview{[super viewDidMoveToSuperview];[self center];}
@end

static void settings_refresh_all(void) { [settingsPanel refresh]; }
/* A mouse click in the panel: the controller's highlight goes until a controller moves again. */
static void settings_pointer_used(void) { if(!g_settings_controller_active)return;g_settings_controller_active=NO;[settingsPanel controllerHighlight]; }
static void settings_panel_replace(void) {   /* main thread; made again: what the game offers is known only once its code is in */
 [settingsPanel endCapture];if(settingsPanel.clickMonitor){[NSEvent removeMonitor:settingsPanel.clickMonitor];settingsPanel.clickMonitor=nil;}[settingsPanel.timer invalidate];[settingsPanel removeFromSuperview];
 settingsPanel=[TOTKSettingsPanel new];[window.contentView addSubview:settingsPanel];
}
/* Made again while open (the drawings page needs another size, or the list view): on the same page and spot. */
static void settings_rebuild(void) {
 if(!window||!settingsOpen)return;
 settings_panel_replace();[settingsPanel center];[settingsPanel refresh];
}
/* The game's code that needs the panel (settings_aa_rebuild). */
#define SETTINGS_GAME_PART 2
#include "settings_game.h"
#undef SETTINGS_GAME_PART
#if SETTINGS_HAS_VR
/* The panel in the headset (zeldaVR, "按建议 都做", 2026-10-30: hold the left X on a Quest controller to open it, the
   Quest buttons then work it by name): while it is open with VR on, its layers are drawn into a bitmap ten times a
   second (twice the size, for the headset's pixels) and handed to the VR thread (gfx/nvn/totk_vr.m vr_panel_image),
   which shows it in the middle of the view; NULL when it closes. */
void vr_panel_image(const void *bgra, int w, int h, int stride);
int vr_panel_in_headset(void);   /* off by default (user 2026-10-30: "先 关闭 设置在 vr里的效果") */
static NSTimer *settingsVRTimer;
static void settings_vr_capture(void) {
 if(!settingsOpen||!settingsPanel||settingsPanel.hidden||!vr_panel_in_headset()){vr_panel_image(NULL,0,0,0);return;}
 const CGFloat k=2;NSSize z=settingsPanel.bounds.size;int w=(int)(z.width*k),h=(int)(z.height*k);if(w<=0||h<=0)return;
 static uint8_t *buf;static size_t cap;size_t need=(size_t)w*h*4;
 if(need>cap){uint8_t *nb=realloc(buf,need);if(!nb)return;buf=nb;cap=need;}
 CGColorSpaceRef cs=CGColorSpaceCreateDeviceRGB();
 CGContextRef ctx=CGBitmapContextCreate(buf,(size_t)w,(size_t)h,8,(size_t)w*4,cs,kCGImageAlphaPremultipliedFirst|kCGBitmapByteOrder32Little);   /* BGRA in memory */
 CGColorSpaceRelease(cs);if(!ctx)return;
 CGContextClearRect(ctx,CGRectMake(0,0,w,h));CGContextScaleCTM(ctx,k,k);[settingsPanel.layer renderInContext:ctx];CGContextRelease(ctx);
 vr_panel_image(buf,w,h,w*4);
}
#endif
static void settings_show(int open) {   /* main thread */
 open=!!open;
 if(!window||open==settingsOpen||(open&&textEntryOpen))return;
 /* Made again at every opening: what the game offers (aspect ratio, resolution) is known only once its code is in. */
 /* With VR running, the first opening shows the VR page (user 2026-10-30: "启动vr的时候 打开设置 默认打开vr tab"); later ones
    the page left last. */
#if SETTINGS_HAS_VR
 {static int vrPageShown;if(open&&!vrPageShown&&vr_enabled()){vrPageShown=1;g_settings_page=SETTINGS_PAGE_VR;}}
#endif
 if(open){g_settings_controller_active=NO;settings_panel_replace();}else [settingsPanel endCapture];
 settingsOpen=open;
 runtime_gamepad_text_capture(open);
 if(open){runtime_input_clear();[settingsPanel center];[settingsPanel refresh];[(TOTKInputView *)window.contentView setCursorAutoHideEnabled:NO];}
 else{[window makeFirstResponder:window.contentView];
  if(window.isKeyWindow&&(window.styleMask&NSWindowStyleMaskFullScreen))[(TOTKInputView *)window.contentView setCursorAutoHideEnabled:YES];}
 settingsPanel.hidden=!open;
#if SETTINGS_HAS_VR
 [settingsVRTimer invalidate];settingsVRTimer=nil;
 if(open&&vr_enabled()&&vr_panel_in_headset())settingsVRTimer=[NSTimer scheduledTimerWithTimeInterval:0.1 repeats:YES block:^(NSTimer *t){(void)t;settings_vr_capture();}];
 settings_vr_capture();   /* the first picture now, or the headset's copy gone */
#endif
 front_mouse_update();
 fprintf(stderr,"[totk] settings panel %s\n",open?"open":"closed");
 if(totk_sr_settings)totk_sr_settings(open);   /* TOTK_STATE_REPORTING: the game gets no input while it is open */
}
#if SETTINGS_HAS_USERS
/* A user named in the game's text bar (main queue, from addUser:): kept before the panel is made again, so its list
   has it, and the result shows for a few seconds where the version is. nil: the bar was cancelled — just come back. */
static void settings_user_added(NSString *name) {
 if(!window||!settingsPanel)return;
 if(name){
  int added=runtime_users_add(name.UTF8String);
  settings_show(1);
  [settingsPanel note:added>=0
   ?[NSString stringWithFormat:@(TOTK_UI("已添加用户“%s”，从下次启动开始玩自己的存档","User \"%s\" added; their saves start from the next launch")),name.UTF8String]
   :settings_text("最多 8 个用户，且名字不能为空","At most 8 users, and the name cannot be empty")];
  fprintf(stderr,"[totk] settings: user \"%s\" %s\n",name.UTF8String,added>=0?"added":"not added (the console is full, or the name is empty)");
 }else settings_show(1);
}
#endif
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
/* Tests (input file "settings <page>", runtime/input.c): the panel opened on that page (SETTINGS_PAGE_*), or closed (-1). */
void runtime_settings_page(int page){dispatch_async(dispatch_get_main_queue(), ^{
 if(page<0||!settings_page_present(page)){settings_show(NO);return;}   /* a page without a tab: closed */
 g_settings_page=page;settings_show(NO);settings_show(YES);
});}
