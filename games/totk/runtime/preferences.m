/* Native AppKit menu bar and preferences window.
 *
 * All user-visible values go through the frozen API in settings.h; this file
 * never touches a module's internals.  The menu bar is validated on every
 * open (NSMenuItemValidation) so environment locks, capability gates and the
 * game window's state are reflected immediately.  The preferences window is a
 * singleton, non-modal NSTabView window with four tabs.
 */
#import <Cocoa/Cocoa.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "settings.h"
#include "preferences.h"
#include "runtime.h"
#include "controller_map.h"
#include "totk_ui_lang.h"

/* Implemented in runtime/frontend.m; the Cocoa shortcut card follows settings changes without a restart. */
void runtime_shortcut_help_set_visible(int on);
void runtime_shortcut_help_refresh_layout(void);

/* ------------------------------------------------------------------------- */
/* Menu item tags.  Values below 1000 are fixed items; 1000..1007 are the
   profiles, 2000.. the game languages and 3000.. the interface languages. */
enum {
    TOTKMenuTagAbout = 100,
    TOTKMenuTagPreferences,
    TOTKMenuTagInterpOff,
    TOTKMenuTagInterp30,
    TOTKMenuTagInterpHalf,
    TOTKMenuTagInterpMax,
    TOTKMenuTagPictureOff,
    TOTKMenuTagPictureMFX2K,
    TOTKMenuTagPictureNative2K,
    TOTKMenuTagPictureMFX4K,
    TOTKMenuTagPictureNative4K,
    TOTKMenuTagAspectToggle,
    TOTKMenuTagRatio21,
    TOTKMenuTagRatio32,
    TOTKMenuTagRatio3440,
    TOTKMenuTagRatioCustom,
    TOTKMenuTagHudCompact,
    TOTKMenuTagHudCorners,
    TOTKMenuTagHints,
    TOTKMenuTagOverlayFPS,
    TOTKMenuTagOverlayShader,
    TOTKMenuTagOverlayLoad,
    TOTKMenuTagManageProfiles,
    TOTKMenuTagFaceAuto,
    TOTKMenuTagFacePosition,
    TOTKMenuTagFaceLabel,
    TOTKMenuTagManageLanguages,
    TOTKMenuTagFullScreen,
    TOTKMenuTagShortcutHelp,
    TOTKMenuTagOpenSettings,
    TOTKMenuTagOpenLogs,
    TOTKMenuTagProfileBase = 1000,
    TOTKMenuTagGameLangBase = 2000,
    TOTKMenuTagUILangBase = 3000
};

static const double kRatio21 = 21.0 / 9.0;
static const double kRatio32 = 32.0 / 9.0;
static const double kRatio3440 = 3440.0 / 1440.0;
static const double kRatioPresets[3] = {21.0 / 9.0, 32.0 / 9.0, 3440.0 / 1440.0};

typedef struct {
    const char *code;
    const char *zh;
    const char *en;
} TOTKLanguage;

static const TOTKLanguage game_languages[] = {
    {"", "跟随系统", "System default"},
    {"en-US", "英语", "English"},
    {"en-GB", "英语（英国）", "English (UK)"},
    {"ja", "日语", "Japanese"},
    {"fr", "法语", "French"},
    {"de", "德语", "German"},
    {"es", "西班牙语", "Spanish"},
    {"es-419", "西班牙语（拉丁美洲）", "Spanish (Latin America)"},
    {"it", "意大利语", "Italian"},
    {"nl", "荷兰语", "Dutch"},
    {"ru", "俄语", "Russian"},
    {"ko", "韩语", "Korean"},
    {"zh-Hans", "简体中文", "Chinese (Simplified)"},
    {"zh-Hant", "繁体中文", "Chinese (Traditional)"},
    {"pt-BR", "葡萄牙语（巴西）", "Portuguese (Brazil)"},
};
static const size_t game_language_count = sizeof game_languages / sizeof *game_languages;

static const TOTKLanguage ui_languages[] = {
    {"", "跟随系统", "System default"},
    {"zh", "中文", "Chinese"},
    {"en", "英语", "English"},
};
static const size_t ui_language_count = sizeof ui_languages / sizeof *ui_languages;

static const char *const action_names_zh[TOTK_ACTION_COUNT] = {
    NULL,
    "A 键", "B 键", "X 键", "Y 键",
    "L 键", "R 键", "ZL 键", "ZR 键",
    "+ 键", "- 键",
    "左摇杆按下", "右摇杆按下",
    "十字键左", "十字键上", "十字键右", "十字键下",
    "左摇杆向左", "左摇杆向右", "左摇杆向上", "左摇杆向下",
    "右摇杆向左", "右摇杆向右", "右摇杆向上", "右摇杆向下",
};
static const char *const action_names_en[TOTK_ACTION_COUNT] = {
    NULL,
    "A button", "B button", "X button", "Y button",
    "L button", "R button", "ZL button", "ZR button",
    "+ button", "- button",
    "Left stick press", "Right stick press",
    "D-pad left", "D-pad up", "D-pad right", "D-pad down",
    "Left stick left", "Left stick right", "Left stick up", "Left stick down",
    "Right stick left", "Right stick right", "Right stick up", "Right stick down",
};

static const char *const switch_names_zh[CONTROLLER_SWITCH_BUTTONS + 1] = {
    "无", "A", "B", "X", "Y", "左摇杆按下", "右摇杆按下", "L", "R", "ZL", "ZR", "+", "-",
    "左", "上", "右", "下",
};
static const char *const switch_names_en[CONTROLLER_SWITCH_BUTTONS + 1] = {
    "None", "A", "B", "X", "Y", "Left stick press", "Right stick press", "L", "R", "ZL", "ZR", "+", "-",
    "Left", "Up", "Right", "Down",
};
static const char *const physical_names_zh[CONTROLLER_BUTTONS] = {
    "南", "东", "西", "北", "左摇杆按下", "右摇杆按下", "L1", "R1", "L2", "R2",
    "菜单", "视图", "左", "上", "右", "下", "触控板",
};
static const char *const physical_names_en[CONTROLLER_BUTTONS] = {
    "South", "East", "West", "North", "L3", "R3", "L1", "R1", "L2", "R2",
    "Menu", "View", "Left", "Up", "Right", "Down", "Touchpad",
};
static const char *const mode_names_zh[4] = {"按住", "轻按", "切换", "瞄准"};
static const char *const mode_names_en[4] = {"Hold", "Tap", "Toggle", "Aim"};

/* ------------------------------------------------------------------------- */
/* Small helpers shared by the menu bar and the window. */

static NSImage *symbol_image(NSString *name) {
    if (@available(macOS 11.0, *)) return [NSImage imageWithSystemSymbolName:name accessibilityDescription:nil];
    return nil;
}

static NSTextField *make_label(NSString *text, NSRect frame, NSFont *font, NSColor *color) {
    NSTextField *label = [NSTextField labelWithString:text];
    label.frame = frame;
    label.font = font;
    label.textColor = color;
    label.lineBreakMode = NSLineBreakByTruncatingTail;
    return label;
}

static NSTextField *make_multiline_label(NSString *text, NSRect frame, NSFont *font, NSColor *color) {
    NSTextField *label = make_label(text, frame, font, color);
    label.maximumNumberOfLines = 0;
    label.lineBreakMode = NSLineBreakByWordWrapping;
    return label;
}

static NSPopUpButton *make_popup(NSArray<NSString *> *titles, NSRect frame, id target, SEL action) {
    NSPopUpButton *popup = [[NSPopUpButton alloc] initWithFrame:frame pullsDown:NO];
    [popup addItemsWithTitles:titles];
    popup.target = target;
    popup.action = action;
    popup.font = [NSFont systemFontOfSize:12];
    popup.controlSize = NSControlSizeRegular;
    return popup;
}

static NSButton *make_checkbox(NSString *title, NSRect frame, id target, SEL action) {
    NSButton *button = [NSButton checkboxWithTitle:title target:target action:action];
    button.frame = frame;
    button.font = [NSFont systemFontOfSize:12];
    return button;
}

static NSButton *make_button(NSString *title, NSRect frame, id target, SEL action) {
    NSButton *button = [NSButton buttonWithTitle:title target:target action:action];
    button.frame = frame;
    button.bezelStyle = NSBezelStyleRounded;
    button.font = [NSFont systemFontOfSize:12];
    return button;
}

static NSString *action_title(int action) {
    if (action <= 0 || action >= TOTK_ACTION_COUNT) return @"";
    return @(TOTK_UI(action_names_zh[action], action_names_en[action]));
}

static NSString *switch_title(int target_plus_one) {
    if (target_plus_one < 0 || target_plus_one > CONTROLLER_SWITCH_BUTTONS) return @"";
    return @(TOTK_UI(switch_names_zh[target_plus_one], switch_names_en[target_plus_one]));
}

static NSString *physical_title(int physical) {
    if (physical < 0 || physical >= CONTROLLER_BUTTONS) return @"";
    return @(TOTK_UI(physical_names_zh[physical], physical_names_en[physical]));
}

static NSString *mode_title(int mode) {
    if (mode < 0 || mode > CONTROLLER_AIM) return @"";
    return @(TOTK_UI(mode_names_zh[mode], mode_names_en[mode]));
}

static int keycode_for_action(int profile, int action) {
    if (profile < 0) return -1;
    for (int key = 0; key < TOTK_KEYCODE_COUNT; key++)
        if (totk_profile_action(profile, key) == action) return key;
    return -1;
}

static int ratio_preset_index(float ratio) {
    for (int i = 0; i < 3; i++)
        if (fabs((double)ratio - kRatioPresets[i]) < 0.005) return i;
    return -1;
}

static BOOL parse_ratio(NSString *text, double *out) {
    if (!text) return NO;
    NSMutableString *clean = [text mutableCopy];
    [clean replaceOccurrencesOfString:@"," withString:@"." options:0 range:NSMakeRange(0, clean.length)];
    NSString *trimmed = [clean stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
    if (!trimmed.length) return NO;
    NSScanner *scanner = [NSScanner scannerWithString:trimmed];
    double value = 0;
    if (![scanner scanDouble:&value] || !scanner.isAtEnd) return NO;
    if (!isfinite(value) || value < 0.5 || value > 8.0) return NO;
    if (out) *out = value;
    return YES;
}

static NSString *ratio_string(float ratio) {
    return [NSString stringWithFormat:@"%.3f", (double)ratio];
}

/* Picture presets.  The five presets are a bijection of the persisted
   output target + upscaler pair; 4K is allowed as supersampling on any
   display, while MetalFX 4K additionally needs a 4K drawable. */
enum {
    TOTKPicturePresetOff = 0,
    TOTKPicturePresetMetalFX2K,
    TOTKPicturePresetNative2K,
    TOTKPicturePresetMetalFX4K,
    TOTKPicturePresetNative4K
};

static void picture_preset_values(int preset, int *res, int *upscaler) {
    switch (preset) {
        case TOTKPicturePresetMetalFX2K:
            *res = TOTK_RES_1440; *upscaler = TOTK_UPSCALE_METALFX; break;
        case TOTKPicturePresetNative2K:
            *res = TOTK_RES_1440; *upscaler = TOTK_UPSCALE_NATIVE; break;
        case TOTKPicturePresetMetalFX4K:
            *res = TOTK_RES_2160; *upscaler = TOTK_UPSCALE_METALFX; break;
        case TOTKPicturePresetNative4K:
            *res = TOTK_RES_2160; *upscaler = TOTK_UPSCALE_NATIVE; break;
        default:
            *res = TOTK_RES_1080; *upscaler = TOTK_UPSCALE_NATIVE; break;
    }
}

static int picture_preset_index(int res, int upscaler) {
    if (res == TOTK_RES_1080) return TOTKPicturePresetOff;
    if (res == TOTK_RES_1440)
        return upscaler == TOTK_UPSCALE_METALFX ? TOTKPicturePresetMetalFX2K : TOTKPicturePresetNative2K;
    if (res == TOTK_RES_2160)
        return upscaler == TOTK_UPSCALE_METALFX ? TOTKPicturePresetMetalFX4K : TOTKPicturePresetNative4K;
    return -1;
}

/* MetalFX 4K presents into the window from a 1080p canvas after the process
   starts with TOTK_OUTPUT_RES=3840x2160 + MetalFX flags.  Gate it on the
   installed 4K overlay only (the capability helper), never on the current
   process already being 4K: a 2K start can persist it and apply it after a
   restart. */
static BOOL picture_mfx4k_available(void) {
    return totk_setting_output_capable(TOTK_RES_2160, TOTK_UPSCALE_METALFX);
}

/* Native 4K renders 3840x2160 and downscales into the window, so it is not
   tied to a 4K physical screen; only the 4K HeapDef overlay is required. */
static BOOL picture_native4k_available(void) {
    return totk_setting_output_capable(TOTK_RES_2160, TOTK_UPSCALE_NATIVE);
}

/* A 4K scene render target is only valid on a native 4K output.  Selecting
   it with a 1080p/2K output used to crash in the game renderer during boot. */
static BOOL picture_native4k_selected(void) {
    return totk_setting_output_res() == TOTK_RES_2160 &&
           totk_setting_upscaler() == TOTK_UPSCALE_NATIVE;
}

static BOOL picture_tag_available(NSInteger tag) {
    if (tag == TOTKMenuTagPictureMFX4K) return picture_mfx4k_available();
    if (tag == TOTKMenuTagPictureNative4K) return picture_native4k_available();
    return YES;
}

static NSString *picture_unavailable_reason(NSInteger tag) {
    if (tag == TOTKMenuTagPictureMFX4K)
        return @(TOTK_UI("当前安装缺少 4K 覆盖层，或图形路径不支持 MetalFX 4K。请使用包含 4K 构建的版本。",
                        "This installation has no 4K overlay, or the graphics path cannot present MetalFX 4K. Use a build with 4K support."));
    if (tag == TOTKMenuTagPictureNative4K)
        return @(TOTK_UI("当前安装缺少 4K 覆盖层，或图形路径不支持原生 4K。请使用包含 4K 构建的版本。",
                        "This installation has no 4K overlay, or the graphics path cannot render native 4K. Use a build with 4K support."));
    return nil;
}

static NSInteger game_language_index(const char *code) {
    const char *current = code ? code : "";
    for (size_t i = 0; i < game_language_count; i++)
        if (!strcmp(current, game_languages[i].code)) return (NSInteger)i;
    return 0;
}

static NSInteger ui_language_index(const char *code) {
    const char *current = code ? code : "";
    for (size_t i = 0; i < ui_language_count; i++)
        if (!strcmp(current, ui_languages[i].code)) return (NSInteger)i;
    return 0;
}

static NSString *settings_folder(void) {
    const char *path = totk_settings_path();
    if (path && *path) {
        NSString *file = [@(path) stringByExpandingTildeInPath];
        return [file stringByDeletingLastPathComponent];
    }
    return [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Application Support/MacNativeGames/TOTK"];
}

static NSString *logs_folder(void) {
    return [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Logs/MacNativeGames"];
}

static NSPanel *s_capture_panel = nil;

/* ------------------------------------------------------------------------- */
@interface TOTKFlippedView : NSView
@end

@implementation TOTKFlippedView
- (BOOL)isFlipped { return YES; }
@end

/* ------------------------------------------------------------------------- */
@interface TOTKPreferencesController : NSObject <NSMenuItemValidation, NSMenuDelegate, NSWindowDelegate, NSTextFieldDelegate>
- (instancetype)initWithGameWindow:(NSWindow *)window;
- (void)setGameWindow:(NSWindow *)window;
- (void)installMenus;
- (void)buildPreferencesWindow;
- (NSView *)buildPictureTab;
- (NSView *)buildHUDTab;
- (NSView *)buildInputTab;
- (NSView *)buildLanguageTab;
- (void)rebuildProfilePopup;
- (void)rebuildInputMenu;
- (void)refresh;
- (void)refreshControls;
- (void)updateKeyLabels;
- (void)updateControllerControls;
- (void)showPreferences;
- (void)selectTab:(NSInteger)index;
- (BOOL)reportResult:(int)rc what:(NSString *)what;
- (void)showError:(NSString *)text;
- (void)menuAction:(NSMenuItem *)item;
- (void)syncStateForItem:(NSMenuItem *)item;
- (BOOL)itemEnabled:(NSInteger)tag;
- (NSString *)tooltipForTag:(NSInteger)tag enabled:(BOOL)enabled;
- (BOOL)runtimeSafe;
- (void)menuWillOpen:(NSMenu *)menu;
- (void)menuDidClose:(NSMenu *)menu;
- (NSMenuItem *)addItem:(NSMenu *)menu title:(NSString *)title tag:(NSInteger)tag key:(NSString *)key mods:(NSEventModifierFlags)mods;
- (NSMenuItem *)addInfoItem:(NSMenu *)menu title:(NSString *)title;
- (NSMenu *)addSubmenu:(NSMenu *)menu title:(NSString *)title;
- (void)interpChanged:(NSPopUpButton *)sender;
- (void)pictureChanged:(NSPopUpButton *)sender;
- (void)outputTargetChanged:(NSPopUpButton *)sender;
- (void)upscalerChanged:(NSPopUpButton *)sender;
- (void)renderChanged:(NSPopUpButton *)sender;
- (void)applyPicturePreset:(int)preset what:(NSString *)what;
- (void)applyPictureRes:(int)res upscaler:(int)upscaler what:(NSString *)what;
- (BOOL)confirm4KPreset:(int)preset;
- (void)setStatus:(NSString *)text warning:(BOOL)warning;
- (void)aspectChanged:(NSButton *)sender;
- (void)ratioPresetChanged:(NSPopUpButton *)sender;
- (void)applyCustomRatio:(id)sender;
- (void)hudLayoutChanged:(NSPopUpButton *)sender;
- (void)hintsChanged:(NSButton *)sender;
- (void)fpsOverlayChanged:(NSButton *)sender;
- (void)shaderOverlayChanged:(NSButton *)sender;
- (void)loadOverlayChanged:(NSButton *)sender;
- (void)profileChanged:(NSPopUpButton *)sender;
- (void)newProfile:(id)sender;
- (void)duplicateProfile:(id)sender;
- (void)renameProfile:(id)sender;
- (void)deleteProfile:(id)sender;
- (void)resetProfile:(id)sender;
- (void)controllerChanged:(NSPopUpButton *)sender;
- (void)changeKey:(NSButton *)sender;
- (void)gameLanguageChanged:(NSPopUpButton *)sender;
- (void)uiLanguageChanged:(NSPopUpButton *)sender;
- (NSString *)promptNameWithTitle:(NSString *)title initial:(NSString *)initial;
- (void)gameWindowWillClose:(NSNotification *)notification;
- (void)windowWillClose:(NSNotification *)notification;
- (void)controlTextDidEndEditing:(NSNotification *)notification;
@end

/* ------------------------------------------------------------------------- */
@implementation TOTKPreferencesController {
    NSWindow *_gameWindow;
    NSWindow *_prefsWindow;
    NSTabView *_tabs;
    NSTextField *_status;
    NSMenuItem *_quitItem;

    NSPopUpButton *_interpPopup;
    NSPopUpButton *_picturePopup;
    NSPopUpButton *_outputPopup;
    NSPopUpButton *_upscalerPopup;
    NSPopUpButton *_renderPopup;
    NSTextField *_pictureRestartLabel;
    NSButton *_aspectCheck;
    NSPopUpButton *_ratioPopup;
    NSTextField *_customRatioField;
    NSButton *_customRatioButton;

    NSPopUpButton *_hudPopup;
    NSButton *_hintsCheck;
    NSButton *_fpsCheck;
    NSButton *_shaderCheck;
    NSButton *_loadCheck;

    NSPopUpButton *_profilePopup;
    NSButton *_deleteProfileButton;
    NSTextField *_keyLabels[TOTK_ACTION_COUNT];
    __strong NSPopUpButton *_ctrlTarget[CONTROLLER_BUTTONS];
    __strong NSPopUpButton *_ctrlMode[CONTROLLER_BUTTONS];
    __strong NSPopUpButton *_ctrlFire[CONTROLLER_BUTTONS];

    NSPopUpButton *_gameLangPopup;
    NSPopUpButton *_uiLangPopup;

    NSMenu *_inputMenu;
    id _keyMonitor;
    BOOL _observingGameClose;
    BOOL _closing;
}

- (instancetype)initWithGameWindow:(NSWindow *)window {
    if ((self = [super init])) {
        [self setGameWindow:window];
        [self installMenus];
        [self buildPreferencesWindow];
    }
    return self;
}

- (void)dealloc {
    if (_keyMonitor) [NSEvent removeMonitor:_keyMonitor];
    [NSNotificationCenter.defaultCenter removeObserver:self];
}

- (void)setGameWindow:(NSWindow *)window {
    if (_observingGameClose) {
        [NSNotificationCenter.defaultCenter removeObserver:self name:NSWindowWillCloseNotification object:_gameWindow];
        _observingGameClose = NO;
    }
    _gameWindow = window;
    _quitItem.target = window;
    if (window) {
        [NSNotificationCenter.defaultCenter addObserver:self
                                               selector:@selector(gameWindowWillClose:)
                                                   name:NSWindowWillCloseNotification
                                                 object:window];
        _observingGameClose = YES;
    }
}

- (void)gameWindowWillClose:(NSNotification *)notification {
    (void)notification;
    _closing = YES;
}

/* ------------------------------------------------------------------------- */
/* Menu construction. */

- (NSMenuItem *)addItem:(NSMenu *)menu title:(NSString *)title tag:(NSInteger)tag key:(NSString *)key mods:(NSEventModifierFlags)mods {
    NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:title action:@selector(menuAction:) keyEquivalent:key ? key : @""];
    item.target = self;
    item.tag = tag;
    item.keyEquivalentModifierMask = mods;
    item.accessibilityLabel = title;
    [menu addItem:item];
    return item;
}

- (NSMenuItem *)addInfoItem:(NSMenu *)menu title:(NSString *)title {
    NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:title action:nil keyEquivalent:@""];
    item.enabled = NO;
    item.accessibilityLabel = title;
    [menu addItem:item];
    return item;
}

- (NSMenu *)addSubmenu:(NSMenu *)menu title:(NSString *)title {
    NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:title action:nil keyEquivalent:@""];
    NSMenu *submenu = [[NSMenu alloc] initWithTitle:title];
    submenu.autoenablesItems = YES;
    submenu.delegate = self;
    item.submenu = submenu;
    item.accessibilityLabel = title;
    [menu addItem:item];
    return submenu;
}

- (void)installMenus {
    if (!NSApp) return;

    NSMenu *mainMenu = [[NSMenu alloc] initWithTitle:@""];
    NSMenuItem *appItem = [[NSMenuItem alloc] initWithTitle:@"Tears of the Kingdom" action:nil keyEquivalent:@""];
    [mainMenu addItem:appItem];
    NSMenu *appMenu = [[NSMenu alloc] initWithTitle:@"Tears of the Kingdom"];
    appMenu.delegate = self;
    appItem.submenu = appMenu;

    NSMenuItem *about = [self addItem:appMenu title:@(TOTK_UI("关于 Tears of the Kingdom", "About Tears of the Kingdom"))
                                 tag:TOTKMenuTagAbout key:@"" mods:0];
    about.image = symbol_image(@"info.circle");
    [appMenu addItem:[NSMenuItem separatorItem]];
    NSMenuItem *prefs = [self addItem:appMenu title:@(TOTK_UI("设置…", "Settings…"))
                                 tag:TOTKMenuTagPreferences key:@"," mods:NSEventModifierFlagCommand];
    prefs.image = symbol_image(@"gearshape");
    [appMenu addItem:[NSMenuItem separatorItem]];

    NSMenuItem *servicesItem = [[NSMenuItem alloc] initWithTitle:@(TOTK_UI("服务", "Services")) action:nil keyEquivalent:@""];
    NSMenu *servicesMenu = [[NSMenu alloc] initWithTitle:@(TOTK_UI("服务", "Services"))];
    servicesMenu.delegate = self;
    servicesItem.submenu = servicesMenu;
    servicesItem.accessibilityLabel = @(TOTK_UI("服务", "Services"));
    [appMenu addItem:servicesItem];

    [appMenu addItem:[NSMenuItem separatorItem]];
    NSMenuItem *hide = [[NSMenuItem alloc] initWithTitle:[NSString stringWithFormat:@(TOTK_UI("隐藏 %@", "Hide %@")), @"Tears of the Kingdom"]
                                                  action:@selector(hide:) keyEquivalent:@"h"];
    hide.target = NSApp;
    hide.keyEquivalentModifierMask = NSEventModifierFlagCommand;
    hide.accessibilityLabel = hide.title;
    [appMenu addItem:hide];
    NSMenuItem *hideOthers = [[NSMenuItem alloc] initWithTitle:@(TOTK_UI("隐藏其他", "Hide Others"))
                                                        action:@selector(hideOtherApplications:) keyEquivalent:@"h"];
    hideOthers.target = NSApp;
    hideOthers.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagOption;
    hideOthers.accessibilityLabel = hideOthers.title;
    [appMenu addItem:hideOthers];
    NSMenuItem *showAll = [[NSMenuItem alloc] initWithTitle:@(TOTK_UI("全部显示", "Show All"))
                                                     action:@selector(unhideAllApplications:) keyEquivalent:@""];
    showAll.target = NSApp;
    showAll.accessibilityLabel = showAll.title;
    [appMenu addItem:showAll];
    [appMenu addItem:[NSMenuItem separatorItem]];
    _quitItem = [self addItem:appMenu title:@(TOTK_UI("退出 Tears of the Kingdom", "Quit Tears of the Kingdom"))
                         tag:0 key:@"q" mods:NSEventModifierFlagCommand];
    _quitItem.action = @selector(performClose:);
    _quitItem.target = _gameWindow;   /* keep the window-close path, which waits for an active save */
    _quitItem.image = nil;

    /* ---- Game ---------------------------------------------------------- */
    NSMenu *gameMenu = [[NSMenu alloc] initWithTitle:@(TOTK_UI("游戏", "Game"))];
    gameMenu.delegate = self;
    NSMenuItem *gameItem = [[NSMenuItem alloc] initWithTitle:@(TOTK_UI("游戏", "Game")) action:nil keyEquivalent:@""];
    gameItem.submenu = gameMenu;
    gameItem.accessibilityLabel = @(TOTK_UI("游戏", "Game"));
    [mainMenu addItem:gameItem];

    [self addItem:gameMenu title:@(TOTK_UI("设置…", "Settings…")) tag:TOTKMenuTagPreferences key:@"" mods:0];
    [gameMenu addItem:[NSMenuItem separatorItem]];

    NSMenu *interp = [self addSubmenu:gameMenu title:@(TOTK_UI("补帧", "Interpolation"))];
    [self addItem:interp title:@(TOTK_UI("关闭", "Off")) tag:TOTKMenuTagInterpOff key:@"" mods:0];
    [self addItem:interp title:@"30+30" tag:TOTKMenuTagInterp30 key:@"" mods:0];
    [self addItem:interp title:@(TOTK_UI("刷新率一半", "Half refresh rate")) tag:TOTKMenuTagInterpHalf key:@"" mods:0];
    [self addItem:interp title:@(TOTK_UI("不限帧", "Uncapped")) tag:TOTKMenuTagInterpMax key:@"" mods:0];

    NSMenu *picture = [self addSubmenu:gameMenu title:@(TOTK_UI("画质", "Picture"))];
    [self addItem:picture title:@(TOTK_UI("关闭（1080p）", "Off (1080p)")) tag:TOTKMenuTagPictureOff key:@"" mods:0];
    [self addItem:picture title:@"MetalFX 2K" tag:TOTKMenuTagPictureMFX2K key:@"" mods:0];
    [self addItem:picture title:@(TOTK_UI("原生 2K", "Native 2K")) tag:TOTKMenuTagPictureNative2K key:@"" mods:0];
    [self addItem:picture title:@"MetalFX 4K" tag:TOTKMenuTagPictureMFX4K key:@"" mods:0];
    [self addItem:picture title:@(TOTK_UI("原生 4K（实验性）", "Native 4K (experimental)")) tag:TOTKMenuTagPictureNative4K key:@"" mods:0];

    [self addItem:gameMenu title:@(TOTK_UI("21:9 宽屏", "Ultrawide")) tag:TOTKMenuTagAspectToggle key:@"" mods:0];
    NSMenu *ratio = [self addSubmenu:gameMenu title:@(TOTK_UI("宽屏比例", "Aspect Ratio"))];
    [self addItem:ratio title:@"21:9" tag:TOTKMenuTagRatio21 key:@"" mods:0];
    [self addItem:ratio title:@"32:9" tag:TOTKMenuTagRatio32 key:@"" mods:0];
    [self addItem:ratio title:@"3440 × 1440" tag:TOTKMenuTagRatio3440 key:@"" mods:0];
    [self addItem:ratio title:@(TOTK_UI("自定义…", "Custom…")) tag:TOTKMenuTagRatioCustom key:@"" mods:0];

    NSMenu *hud = [self addSubmenu:gameMenu title:@"HUD"];
    [self addItem:hud title:@(TOTK_UI("紧凑", "Compact")) tag:TOTKMenuTagHudCompact key:@"" mods:0];
    [self addItem:hud title:@(TOTK_UI("角落", "Corners")) tag:TOTKMenuTagHudCorners key:@"" mods:0];

    [self addItem:gameMenu title:@(TOTK_UI("显示提示", "Show Hints")) tag:TOTKMenuTagHints key:@"" mods:0];

    NSMenu *overlays = [self addSubmenu:gameMenu title:@(TOTK_UI("覆盖层", "Overlays"))];
    [self addItem:overlays title:@"FPS" tag:TOTKMenuTagOverlayFPS key:@"" mods:0];
    [self addItem:overlays title:@(TOTK_UI("着色器", "Shader")) tag:TOTKMenuTagOverlayShader key:@"" mods:0];
    [self addItem:overlays title:@(TOTK_UI("系统负载", "System Load")) tag:TOTKMenuTagOverlayLoad key:@"" mods:0];

    _inputMenu = [self addSubmenu:gameMenu title:@(TOTK_UI("输入", "Input"))];
    [self rebuildInputMenu];

    NSMenu *language = [self addSubmenu:gameMenu title:@(TOTK_UI("语言", "Language"))];
    NSMenu *gameLanguage = [self addSubmenu:language title:@(TOTK_UI("游戏语言", "Game Language"))];
    for (size_t i = 0; i < game_language_count; i++)
        [self addItem:gameLanguage title:@(TOTK_UI(game_languages[i].zh, game_languages[i].en))
                  tag:(NSInteger)(TOTKMenuTagGameLangBase + (NSInteger)i) key:@"" mods:0];
    NSMenu *uiLanguage = [self addSubmenu:language title:@(TOTK_UI("界面语言", "Interface Language"))];
    for (size_t i = 0; i < ui_language_count; i++)
        [self addItem:uiLanguage title:@(TOTK_UI(ui_languages[i].zh, ui_languages[i].en))
                  tag:(NSInteger)(TOTKMenuTagUILangBase + (NSInteger)i) key:@"" mods:0];
    [language addItem:[NSMenuItem separatorItem]];
    [self addItem:language title:@(TOTK_UI("管理语言…", "Manage Languages…")) tag:TOTKMenuTagManageLanguages key:@"" mods:0];

    [gameMenu addItem:[NSMenuItem separatorItem]];
    [self addItem:gameMenu title:@(TOTK_UI("进入全屏", "Enter Full Screen")) tag:TOTKMenuTagFullScreen
              key:@"f" mods:(NSEventModifierFlagCommand | NSEventModifierFlagControl)];

    /* ---- Edit ---------------------------------------------------------- */
    NSMenu *editMenu = [[NSMenu alloc] initWithTitle:@(TOTK_UI("编辑", "Edit"))];
    editMenu.delegate = self;
    NSMenuItem *editItem = [[NSMenuItem alloc] initWithTitle:@(TOTK_UI("编辑", "Edit")) action:nil keyEquivalent:@""];
    editItem.submenu = editMenu;
    editItem.accessibilityLabel = @(TOTK_UI("编辑", "Edit"));
    [mainMenu addItem:editItem];

    NSMenuItem *undo = [[NSMenuItem alloc] initWithTitle:@(TOTK_UI("撤销", "Undo")) action:@selector(undo:) keyEquivalent:@"z"];
    undo.keyEquivalentModifierMask = NSEventModifierFlagCommand;
    undo.accessibilityLabel = undo.title;
    [editMenu addItem:undo];
    NSMenuItem *redo = [[NSMenuItem alloc] initWithTitle:@(TOTK_UI("重做", "Redo")) action:@selector(redo:) keyEquivalent:@"z"];
    redo.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagShift;
    redo.accessibilityLabel = redo.title;
    [editMenu addItem:redo];
    [editMenu addItem:[NSMenuItem separatorItem]];
    NSMenuItem *cut = [[NSMenuItem alloc] initWithTitle:@(TOTK_UI("剪切", "Cut")) action:@selector(cut:) keyEquivalent:@"x"];
    cut.accessibilityLabel = cut.title;
    [editMenu addItem:cut];
    NSMenuItem *copy = [[NSMenuItem alloc] initWithTitle:@(TOTK_UI("拷贝", "Copy")) action:@selector(copy:) keyEquivalent:@"c"];
    copy.accessibilityLabel = copy.title;
    [editMenu addItem:copy];
    NSMenuItem *paste = [[NSMenuItem alloc] initWithTitle:@(TOTK_UI("粘贴", "Paste")) action:@selector(paste:) keyEquivalent:@"v"];
    paste.accessibilityLabel = paste.title;
    [editMenu addItem:paste];
    NSMenuItem *delete = [[NSMenuItem alloc] initWithTitle:@(TOTK_UI("删除", "Delete")) action:@selector(delete:) keyEquivalent:@""];
    delete.accessibilityLabel = delete.title;
    [editMenu addItem:delete];
    NSMenuItem *selectAll = [[NSMenuItem alloc] initWithTitle:@(TOTK_UI("全选", "Select All")) action:@selector(selectAll:) keyEquivalent:@"a"];
    selectAll.accessibilityLabel = selectAll.title;
    [editMenu addItem:selectAll];

    /* ---- Window -------------------------------------------------------- */
    NSMenu *windowMenu = [[NSMenu alloc] initWithTitle:@(TOTK_UI("窗口", "Window"))];
    windowMenu.delegate = self;
    NSMenuItem *windowItem = [[NSMenuItem alloc] initWithTitle:@(TOTK_UI("窗口", "Window")) action:nil keyEquivalent:@""];
    windowItem.submenu = windowMenu;
    windowItem.accessibilityLabel = @(TOTK_UI("窗口", "Window"));
    [mainMenu addItem:windowItem];
    NSMenuItem *minimize = [[NSMenuItem alloc] initWithTitle:@(TOTK_UI("最小化", "Minimize"))
                                                      action:@selector(performMiniaturize:) keyEquivalent:@"m"];
    minimize.keyEquivalentModifierMask = NSEventModifierFlagCommand;
    minimize.accessibilityLabel = minimize.title;
    [windowMenu addItem:minimize];
    NSMenuItem *zoom = [[NSMenuItem alloc] initWithTitle:@(TOTK_UI("缩放", "Zoom"))
                                                   action:@selector(performZoom:) keyEquivalent:@""];
    zoom.accessibilityLabel = zoom.title;
    [windowMenu addItem:zoom];
    [windowMenu addItem:[NSMenuItem separatorItem]];
    NSMenuItem *front = [[NSMenuItem alloc] initWithTitle:@(TOTK_UI("全部置于顶层", "Bring All to Front"))
                                                    action:@selector(arrangeInFront:) keyEquivalent:@""];
    front.accessibilityLabel = front.title;
    [windowMenu addItem:front];

    /* ---- Help ---------------------------------------------------------- */
    NSMenu *helpMenu = [[NSMenu alloc] initWithTitle:@(TOTK_UI("帮助", "Help"))];
    helpMenu.delegate = self;
    NSMenuItem *helpItem = [[NSMenuItem alloc] initWithTitle:@(TOTK_UI("帮助", "Help")) action:nil keyEquivalent:@""];
    helpItem.submenu = helpMenu;
    helpItem.accessibilityLabel = @(TOTK_UI("帮助", "Help"));
    [mainMenu addItem:helpItem];
    NSMenuItem *shortcutHelp = [self addItem:helpMenu title:@(TOTK_UI("快捷键说明", "Shortcut Help"))
                                        tag:TOTKMenuTagShortcutHelp key:@"" mods:0];
    shortcutHelp.image = symbol_image(@"keyboard");
    NSMenuItem *openSettings = [self addItem:helpMenu title:@(TOTK_UI("打开设置文件夹", "Open Settings Folder"))
                                        tag:TOTKMenuTagOpenSettings key:@"" mods:0];
    openSettings.image = symbol_image(@"folder");
    NSMenuItem *openLogs = [self addItem:helpMenu title:@(TOTK_UI("打开日志", "Open Logs"))
                                    tag:TOTKMenuTagOpenLogs key:@"" mods:0];
    openLogs.image = symbol_image(@"doc.text");

    NSApp.mainMenu = mainMenu;
    NSApp.servicesMenu = servicesMenu;
    NSApp.windowsMenu = windowMenu;
    NSApp.helpMenu = helpMenu;
}

- (void)rebuildInputMenu {
    if (!_inputMenu) return;
    [_inputMenu removeAllItems];
    int count = totk_profile_count();
    for (int i = 0; i < count && i < TOTK_PROFILE_MAX; i++) {
        const char *raw = totk_profile_name(i);
        NSString *name = raw && *raw ? @(raw) : @(TOTK_UI("未命名", "Untitled"));
        if (totk_profile_builtin(i)) name = [name stringByAppendingString:@(TOTK_UI("（内置）", " (built-in)"))];
        [self addItem:_inputMenu title:name tag:(NSInteger)(TOTKMenuTagProfileBase + i) key:@"" mods:0];
    }
    if (count <= 0) [self addInfoItem:_inputMenu title:@(TOTK_UI("没有键位配置", "No keymap profiles"))];
    [_inputMenu addItem:[NSMenuItem separatorItem]];
    NSMenuItem *manage = [self addItem:_inputMenu title:@(TOTK_UI("管理配置…", "Manage Profiles…"))
                                   tag:TOTKMenuTagManageProfiles key:@"" mods:0];
    manage.image = symbol_image(@"person.2");
    NSMenu *face = [self addSubmenu:_inputMenu title:@(TOTK_UI("按键面布局", "Face Layout"))];
    [self addItem:face title:@(TOTK_UI("自动", "Auto")) tag:TOTKMenuTagFaceAuto key:@"" mods:0];
    [self addItem:face title:@(TOTK_UI("按位置", "Position")) tag:TOTKMenuTagFacePosition key:@"" mods:0];
    [self addItem:face title:@(TOTK_UI("按标签", "Label")) tag:TOTKMenuTagFaceLabel key:@"" mods:0];
}

/* ------------------------------------------------------------------------- */
/* Menu validation. */

- (BOOL)runtimeSafe {
    if (!_gameWindow || _closing) return NO;
    if (!_gameWindow.isVisible || _gameWindow.isMiniaturized) return NO;
    if (s_capture_panel != nil) return NO;
    return YES;
}

static const char *lock_name_for_tag(NSInteger tag) {
    if (tag >= TOTKMenuTagInterpOff && tag <= TOTKMenuTagInterpMax) return "interp";
    if (tag >= TOTKMenuTagPictureOff && tag <= TOTKMenuTagPictureNative4K) return "picture";
    if ((tag >= TOTKMenuTagAspectToggle && tag <= TOTKMenuTagRatioCustom)) return "aspect";
    if (tag == TOTKMenuTagHudCompact || tag == TOTKMenuTagHudCorners) return "hud";
    if (tag == TOTKMenuTagOverlayFPS) return "fps";
    if (tag == TOTKMenuTagOverlayShader) return "shader";
    if (tag == TOTKMenuTagOverlayLoad) return "load";
    if (tag >= TOTKMenuTagFaceAuto && tag <= TOTKMenuTagFaceLabel) return "face";
    if (tag >= TOTKMenuTagGameLangBase && tag < TOTKMenuTagUILangBase) return "lang";
    if (tag >= TOTKMenuTagUILangBase) return "ui_lang";
    return NULL;
}

- (BOOL)itemEnabled:(NSInteger)tag {
    if (!_gameWindow) return NO;
    if (tag == TOTKMenuTagAbout || tag == TOTKMenuTagPreferences || tag == TOTKMenuTagShortcutHelp ||
        tag == TOTKMenuTagOpenSettings || tag == TOTKMenuTagOpenLogs || tag == TOTKMenuTagManageProfiles ||
        tag == TOTKMenuTagManageLanguages)
        return YES;
    if (![self runtimeSafe]) return NO;
    if (tag >= TOTKMenuTagProfileBase && tag < TOTKMenuTagProfileBase + TOTK_PROFILE_MAX)
        return totk_profile_count() > 0;
    const char *lock = lock_name_for_tag(tag);
    if (lock && totk_setting_locked(lock)) return NO;
    if (tag >= TOTKMenuTagInterpOff && tag <= TOTKMenuTagInterpMax) return totk_capability_interp() != 0;
    if (tag >= TOTKMenuTagPictureOff && tag <= TOTKMenuTagPictureNative4K)
        return totk_capability_output() != 0 && picture_tag_available(tag);
    if (tag >= TOTKMenuTagAspectToggle && tag <= TOTKMenuTagRatioCustom) return totk_capability_aspect() != 0;
    return YES;
}

- (NSString *)descriptionForTag:(NSInteger)tag {
    if (tag == TOTKMenuTagAbout) return @(TOTK_UI("显示版本和致谢信息。", "Show version and acknowledgements."));
    if (tag == TOTKMenuTagPreferences) return @(TOTK_UI("打开设置窗口。", "Open the settings window."));
    if (tag >= TOTKMenuTagInterpOff && tag <= TOTKMenuTagInterpMax)
        return @(TOTK_UI("选择补帧模式。", "Choose the frame interpolation mode."));
    if (tag >= TOTKMenuTagPictureOff && tag <= TOTKMenuTagPictureNative4K) {
        if (tag == TOTKMenuTagPictureMFX4K) {
            NSString *text = @(TOTK_UI("MetalFX 4K：由 MetalFX 将画面呈现为 4K，性能优于原生 4K。",
                                       "MetalFX 4K: MetalFX presents a 4K picture; faster than native 4K."));
            if (runtime_output_max_res() != TOTK_RES_2160)
                text = [text stringByAppendingString:@(TOTK_UI(" 当前进程未以 4K 启动，选择后需要重新启动游戏。",
                                                               " This process did not start at 4K, so a restart is required after selecting it."))];
            return text;
        }
        if (tag == TOTKMenuTagPictureNative4K) {
            NSString *text = @(TOTK_UI("原生 4K（实验性）：以 3840×2160 渲染并缩放到窗口，GPU 与显存占用很高。",
                                       "Native 4K (experimental): render at 3840×2160 and downscale to the window; high GPU and memory load."));
            if (runtime_output_max_res() != TOTK_RES_2160)
                text = [text stringByAppendingString:@(TOTK_UI(" 需要重新启动游戏后生效。",
                                                               " It takes effect after restarting the game."))];
            return text;
        }
        return @(TOTK_UI("选择输出画质。", "Choose the output picture mode."));
    }
    if (tag == TOTKMenuTagAspectToggle) return @(TOTK_UI("切换 21:9 宽屏。", "Toggle the ultrawide aspect ratio."));
    if (tag >= TOTKMenuTagRatio21 && tag <= TOTKMenuTagRatioCustom)
        return @(TOTK_UI("选择宽屏比例。", "Choose the ultrawide aspect ratio."));
    if (tag == TOTKMenuTagHudCompact || tag == TOTKMenuTagHudCorners)
        return @(TOTK_UI("选择 HUD 与提示的位置。", "Choose where the HUD and hints appear."));
    if (tag == TOTKMenuTagHints) return @(TOTK_UI("显示或隐藏提示。", "Show or hide the hints."));
    if (tag >= TOTKMenuTagOverlayFPS && tag <= TOTKMenuTagOverlayLoad)
        return @(TOTK_UI("显示或隐藏这个覆盖层。", "Show or hide this overlay."));
    if (tag >= TOTKMenuTagProfileBase && tag < TOTKMenuTagProfileBase + TOTK_PROFILE_MAX)
        return @(TOTK_UI("使用这个键位配置。", "Use this keymap profile."));
    if (tag == TOTKMenuTagManageProfiles) return @(TOTK_UI("新建、复制、重命名或删除键位配置。", "Create, duplicate, rename or delete keymap profiles."));
    if (tag >= TOTKMenuTagFaceAuto && tag <= TOTKMenuTagFaceLabel)
        return @(TOTK_UI("选择手柄按键面的排列方式。", "Choose how controller face buttons are laid out."));
    if (tag >= TOTKMenuTagGameLangBase && tag < TOTKMenuTagUILangBase)
        return @(TOTK_UI("设置游戏语言（重新启动后生效）。", "Set the game language (takes effect after a restart)."));
    if (tag >= TOTKMenuTagUILangBase) return @(TOTK_UI("设置界面语言（重新启动后生效）。", "Set the interface language (takes effect after a restart)."));
    if (tag == TOTKMenuTagManageLanguages) return @(TOTK_UI("打开语言设置。", "Open the language settings."));
    if (tag == TOTKMenuTagFullScreen) return @(TOTK_UI("进入或退出全屏。", "Enter or leave full screen."));
    if (tag == TOTKMenuTagShortcutHelp) return @(TOTK_UI("显示或隐藏游戏内的快捷键说明。", "Show or hide the in-game shortcut help."));
    if (tag == TOTKMenuTagOpenSettings) return @(TOTK_UI("在访达中显示设置文件。", "Show the settings files in Finder."));
    if (tag == TOTKMenuTagOpenLogs) return @(TOTK_UI("在访达中显示日志文件。", "Show the log files in Finder."));
    return @"";
}

- (NSString *)tooltipForTag:(NSInteger)tag enabled:(BOOL)enabled {
    if (enabled) return [self descriptionForTag:tag];
    NSString *why = nil;
    if (!_gameWindow) why = @(TOTK_UI("游戏窗口不可用。", "The game window is unavailable."));
    else if ((tag >= TOTKMenuTagInterpOff && tag <= TOTKMenuTagRatioCustom) ||
             (tag >= TOTKMenuTagHudCompact && tag <= TOTKMenuTagFaceLabel) ||
             (tag >= TOTKMenuTagProfileBase && tag < TOTKMenuTagProfileBase + TOTK_PROFILE_MAX) ||
             tag == TOTKMenuTagManageProfiles ||
             tag >= TOTKMenuTagGameLangBase || tag == TOTKMenuTagFullScreen) {
        if (![self runtimeSafe]) why = @(TOTK_UI("游戏窗口不可用、已最小化或正在关闭。", "The game window is unavailable, minimized or closing."));
    }
    const char *lock = lock_name_for_tag(tag);
    if (!why && lock && totk_setting_locked(lock))
        why = @(TOTK_UI("已由环境变量锁定，不能在游戏内修改。",
                        "Locked by an environment variable; it cannot be changed in-game."));
    if (!why && (tag >= TOTKMenuTagInterpOff && tag <= TOTKMenuTagInterpMax) && !totk_capability_interp())
        why = @(TOTK_UI("当前图形路径不支持补帧。", "The current graphics path does not support interpolation."));
    if (!why && (tag >= TOTKMenuTagPictureOff && tag <= TOTKMenuTagPictureNative4K)) {
        if (!totk_capability_output())
            why = @(TOTK_UI("当前图形路径不支持改变输出画质。", "The current graphics path cannot change the output picture."));
        else if (!picture_tag_available(tag))
            why = picture_unavailable_reason(tag);
    }
    if (!why && (tag >= TOTKMenuTagAspectToggle && tag <= TOTKMenuTagRatioCustom) && !totk_capability_aspect())
        why = @(TOTK_UI("这个游戏版本不支持宽屏补丁。", "This game version has no ultrawide patch."));
    if (!why && tag == TOTKMenuTagFullScreen)
        why = @(TOTK_UI("游戏窗口不可用。", "The game window is unavailable."));
    if (!why) why = @(TOTK_UI("当前状态不可用。", "Not available in the current state."));
    return why;
}

- (void)syncStateForItem:(NSMenuItem *)item {
    NSInteger tag = item.tag;
    item.state = NSControlStateValueOff;
    if (tag == TOTKMenuTagInterpOff) item.state = totk_setting_interp() == TOTK_INTERP_OFF ? NSControlStateValueOn : NSControlStateValueOff;
    else if (tag == TOTKMenuTagInterp30) item.state = totk_setting_interp() == TOTK_INTERP_30 ? NSControlStateValueOn : NSControlStateValueOff;
    else if (tag == TOTKMenuTagInterpHalf) item.state = totk_setting_interp() == TOTK_INTERP_HALF ? NSControlStateValueOn : NSControlStateValueOff;
    else if (tag == TOTKMenuTagInterpMax) item.state = totk_setting_interp() == TOTK_INTERP_MAX ? NSControlStateValueOn : NSControlStateValueOff;
    else if (tag >= TOTKMenuTagPictureOff && tag <= TOTKMenuTagPictureNative4K) {
        int preset = picture_preset_index(totk_setting_output_res(), totk_setting_upscaler());
        item.state = preset == (int)(tag - TOTKMenuTagPictureOff) ? NSControlStateValueOn : NSControlStateValueOff;
    }
    else if (tag == TOTKMenuTagAspectToggle) item.state = totk_setting_aspect() ? NSControlStateValueOn : NSControlStateValueOff;
    else if (tag >= TOTKMenuTagRatio21 && tag <= TOTKMenuTagRatio3440) {
        int index = (int)(tag - TOTKMenuTagRatio21);
        item.state = totk_setting_aspect() && ratio_preset_index(totk_setting_aspect_ratio()) == index
                         ? NSControlStateValueOn : NSControlStateValueOff;
    } else if (tag == TOTKMenuTagHudCompact || tag == TOTKMenuTagHudCorners) {
        BOOL corners = totk_setting_hud_corners() != 0;
        item.state = ((tag == TOTKMenuTagHudCorners) == corners) ? NSControlStateValueOn : NSControlStateValueOff;
    } else if (tag == TOTKMenuTagHints || tag == TOTKMenuTagShortcutHelp)
        item.state = totk_setting_hints() ? NSControlStateValueOn : NSControlStateValueOff;
    else if (tag == TOTKMenuTagOverlayFPS) item.state = totk_setting_fps_overlay() ? NSControlStateValueOn : NSControlStateValueOff;
    else if (tag == TOTKMenuTagOverlayShader) item.state = totk_setting_shader_overlay() ? NSControlStateValueOn : NSControlStateValueOff;
    else if (tag == TOTKMenuTagOverlayLoad) item.state = totk_setting_load_overlay() ? NSControlStateValueOn : NSControlStateValueOff;
    else if (tag >= TOTKMenuTagProfileBase && tag < TOTKMenuTagProfileBase + TOTK_PROFILE_MAX)
        item.state = totk_profile_active() == (int)(tag - TOTKMenuTagProfileBase) ? NSControlStateValueOn : NSControlStateValueOff;
    else if (tag >= TOTKMenuTagFaceAuto && tag <= TOTKMenuTagFaceLabel) {
        int want = (int)(tag - TOTKMenuTagFaceAuto);
        item.state = totk_setting_face_layout() == want ? NSControlStateValueOn : NSControlStateValueOff;
    } else if (tag >= TOTKMenuTagGameLangBase && tag < TOTKMenuTagUILangBase) {
        size_t index = (size_t)(tag - TOTKMenuTagGameLangBase);
        item.state = index < game_language_count && !strcmp(totk_setting_game_language() ?: "", game_languages[index].code)
                         ? NSControlStateValueOn : NSControlStateValueOff;
    } else if (tag >= TOTKMenuTagUILangBase) {
        size_t index = (size_t)(tag - TOTKMenuTagUILangBase);
        item.state = index < ui_language_count && !strcmp(totk_setting_ui_language() ?: "", ui_languages[index].code)
                         ? NSControlStateValueOn : NSControlStateValueOff;
    } else if (tag == TOTKMenuTagFullScreen) {
        item.title = (_gameWindow.styleMask & NSWindowStyleMaskFullScreen)
                         ? @(TOTK_UI("退出全屏", "Exit Full Screen"))
                         : @(TOTK_UI("进入全屏", "Enter Full Screen"));
        item.accessibilityLabel = item.title;
    }
}

- (BOOL)validateMenuItem:(NSMenuItem *)menuItem {
    if (menuItem.action != @selector(menuAction:)) return YES;
    [self syncStateForItem:menuItem];
    BOOL enabled = [self itemEnabled:menuItem.tag];
    menuItem.toolTip = [self tooltipForTag:menuItem.tag enabled:enabled];
    return enabled;
}

- (void)menuWillOpen:(NSMenu *)menu {
    (void)menu;
    runtime_input_clear();
}

- (void)menuDidClose:(NSMenu *)menu {
    (void)menu;
    runtime_input_clear();
}

/* ------------------------------------------------------------------------- */
/* Menu actions. */

- (BOOL)reportResult:(int)rc what:(NSString *)what {
    if (rc >= 0) {
        _status.stringValue = [NSString stringWithFormat:@(TOTK_UI("%@ 已保存", "%@ saved")), what];
        _status.textColor = NSColor.secondaryLabelColor;
        return YES;
    }
    NSString *reason = nil;
    switch (rc) {
        case TOTK_SET_ERR_UNAVAILABLE:
            reason = @(TOTK_UI("当前图形路径不支持这个设置，没有修改。", "The current graphics path does not support this setting; nothing was changed."));
            break;
        case TOTK_SET_ERR_INVALID:
            reason = @(TOTK_UI("这个值无效。", "That value is not valid."));
            break;
        case TOTK_SET_ERR_SAVE:
            reason = @(TOTK_UI("保存失败，请检查磁盘空间和权限。", "Saving failed; check disk space and permissions."));
            break;
        case TOTK_SET_ERR_LOCKED:
            reason = @(TOTK_UI("这个设置已由环境变量锁定。", "This setting is locked by an environment variable."));
            break;
        case TOTK_SET_ERR_PENDING:
            reason = @(TOTK_UI("显示模式正在切换，请稍后再试。", "The display mode is changing; try again in a moment."));
            break;
        default:
            reason = [NSString stringWithFormat:@(TOTK_UI("未知错误（%d）。", "Unknown error (%d).")), rc];
            break;
    }
    [self showError:[NSString stringWithFormat:@"%@ — %@", what, reason]];
    fprintf(stderr, "[totk] settings: %s failed (%d)\n", what.UTF8String, rc);
    return NO;
}

- (void)showError:(NSString *)text {
    if (_status) {
        _status.stringValue = text;
        _status.textColor = NSColor.systemRedColor;
    }
    NSBeep();
}

- (void)menuAction:(NSMenuItem *)item {
    NSInteger tag = item.tag;
    int rc = TOTK_SET_OK;
    NSString *what = item.title;

    if (tag == TOTKMenuTagAbout) { [NSApp orderFrontStandardAboutPanel:nil]; return; }
    if (tag == TOTKMenuTagPreferences) { [self showPreferences]; return; }
    if (tag == TOTKMenuTagManageProfiles) { [self selectTab:2]; return; }
    if (tag == TOTKMenuTagManageLanguages) { [self selectTab:3]; return; }
    if (tag == TOTKMenuTagShortcutHelp) {
        runtime_shortcut_help_toggle();
        /* Reflect the persisted setting immediately; the Game > Show Hints item refreshes on its next validation. */
        item.state = totk_setting_hints() ? NSControlStateValueOn : NSControlStateValueOff;
        return;
    }
    if (tag == TOTKMenuTagOpenSettings) {
        NSURL *url = [NSURL fileURLWithPath:settings_folder()];
        if (![[NSWorkspace sharedWorkspace] openURL:url])
            [self showError:@(TOTK_UI("无法打开设置文件夹。", "Could not open the settings folder."))];
        return;
    }
    if (tag == TOTKMenuTagOpenLogs) {
        NSString *folder = logs_folder();
        NSString *log = [folder stringByAppendingPathComponent:@"totk.log"];
        NSWorkspace *workspace = [NSWorkspace sharedWorkspace];
        if ([[NSFileManager defaultManager] fileExistsAtPath:log])
            [workspace activateFileViewerSelectingURLs:@[[NSURL fileURLWithPath:log]]];
        else if (![workspace openURL:[NSURL fileURLWithPath:folder]])
            [self showError:@(TOTK_UI("无法打开日志文件夹。", "Could not open the logs folder."))];
        return;
    }
    if (tag == TOTKMenuTagFullScreen) {
        if ([self runtimeSafe]) [_gameWindow toggleFullScreen:nil];
        return;
    }
    if (![self itemEnabled:tag]) {
        [self showError:[self tooltipForTag:tag enabled:NO]];
        return;
    }

    if (tag >= TOTKMenuTagProfileBase && tag < TOTKMenuTagProfileBase + TOTK_PROFILE_MAX) {
        rc = totk_profile_set_active((int)(tag - TOTKMenuTagProfileBase));
        if (rc >= 0) totk_profile_apply();
        [self reportResult:rc what:what];
        [self refresh];
        return;
    }
    if (tag >= TOTKMenuTagFaceAuto && tag <= TOTKMenuTagFaceLabel) {
        rc = totk_setting_set_face_layout((int)(tag - TOTKMenuTagFaceAuto));
        [self reportResult:rc what:what];
        [self refresh];
        return;
    }
    if (tag >= TOTKMenuTagGameLangBase && tag < TOTKMenuTagUILangBase) {
        size_t index = (size_t)(tag - TOTKMenuTagGameLangBase);
        if (index < game_language_count) rc = totk_setting_set_game_language(game_languages[index].code);
        else rc = TOTK_SET_ERR_INVALID;
        if ([self reportResult:rc what:what]) _status.stringValue = @(TOTK_UI("游戏语言已保存，重新启动后生效。", "Game language saved; it takes effect after a restart."));
        [self refresh];
        return;
    }
    if (tag >= TOTKMenuTagUILangBase) {
        size_t index = (size_t)(tag - TOTKMenuTagUILangBase);
        if (index < ui_language_count) rc = totk_setting_set_ui_language(ui_languages[index].code);
        else rc = TOTK_SET_ERR_INVALID;
        if ([self reportResult:rc what:what]) _status.stringValue = @(TOTK_UI("界面语言已保存，重新启动后生效。", "Interface language saved; it takes effect after a restart."));
        [self refresh];
        return;
    }

    if (tag == TOTKMenuTagInterpOff) rc = totk_setting_set_interp(TOTK_INTERP_OFF);
    else if (tag == TOTKMenuTagInterp30) rc = totk_setting_set_interp(TOTK_INTERP_30);
    else if (tag == TOTKMenuTagInterpHalf) rc = totk_setting_set_interp(TOTK_INTERP_HALF);
    else if (tag == TOTKMenuTagInterpMax) rc = totk_setting_set_interp(TOTK_INTERP_MAX);
    else if (tag >= TOTKMenuTagPictureOff && tag <= TOTKMenuTagPictureNative4K) {
        [self applyPicturePreset:(int)(tag - TOTKMenuTagPictureOff) what:what];
        return;
    }
    else if (tag == TOTKMenuTagAspectToggle) rc = totk_setting_set_aspect(!totk_setting_aspect());
    else if (tag == TOTKMenuTagRatio21) { rc = totk_setting_set_aspect_ratio((float)kRatio21); if (rc >= 0) rc = totk_setting_set_aspect(1); }
    else if (tag == TOTKMenuTagRatio32) { rc = totk_setting_set_aspect_ratio((float)kRatio32); if (rc >= 0) rc = totk_setting_set_aspect(1); }
    else if (tag == TOTKMenuTagRatio3440) { rc = totk_setting_set_aspect_ratio((float)kRatio3440); if (rc >= 0) rc = totk_setting_set_aspect(1); }
    else if (tag == TOTKMenuTagRatioCustom) {
        NSAlert *alert = [NSAlert new];
        alert.messageText = @(TOTK_UI("自定义宽屏比例", "Custom aspect ratio"));
        alert.informativeText = @(TOTK_UI("输入 0.5 到 8.0 之间的数字，例如 2.35。", "Enter a number between 0.5 and 8.0, for example 2.35."));
        [alert addButtonWithTitle:@(TOTK_UI("应用", "Apply"))];
        [alert addButtonWithTitle:@(TOTK_UI("取消", "Cancel"))];
        NSTextField *field = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 220, 24)];
        field.stringValue = ratio_string(totk_setting_aspect_ratio());
        alert.accessoryView = field;
        while ([alert runModal] == NSAlertFirstButtonReturn) {
            double value = 0;
            if (parse_ratio(field.stringValue, &value)) {
                rc = totk_setting_set_aspect_ratio((float)value);
                if (rc >= 0) rc = totk_setting_set_aspect(1);
                [self reportResult:rc what:what];
                [self refresh];
                return;
            }
            [self showError:@(TOTK_UI("宽屏比例必须在 0.5 到 8.0 之间。", "The aspect ratio must be between 0.5 and 8.0."))];
            alert.informativeText = @(TOTK_UI("无效的值。请输入 0.5 到 8.0 之间的数字。", "Invalid value. Enter a number between 0.5 and 8.0."));
        }
        return;
    }
    else if (tag == TOTKMenuTagHudCompact) rc = totk_setting_set_hud_corners(0);
    else if (tag == TOTKMenuTagHudCorners) rc = totk_setting_set_hud_corners(1);
    else if (tag == TOTKMenuTagHints) rc = totk_setting_set_hints(!totk_setting_hints());
    else if (tag == TOTKMenuTagOverlayFPS) rc = totk_setting_set_fps_overlay(!totk_setting_fps_overlay());
    else if (tag == TOTKMenuTagOverlayShader) rc = totk_setting_set_shader_overlay(!totk_setting_shader_overlay());
    else if (tag == TOTKMenuTagOverlayLoad) rc = totk_setting_set_load_overlay(!totk_setting_load_overlay());
    else return;

    [self reportResult:rc what:what];
    if (tag == TOTKMenuTagHints) runtime_shortcut_help_set_visible(totk_setting_hints());
    else if (tag == TOTKMenuTagHudCompact || tag == TOTKMenuTagHudCorners) runtime_shortcut_help_refresh_layout();
    [self refresh];
}

/* ------------------------------------------------------------------------- */
/* Preferences window. */

- (void)buildPreferencesWindow {
    NSRect contentRect = NSMakeRect(0, 0, 760, 540);
    _prefsWindow = [[NSWindow alloc] initWithContentRect:contentRect
                                              styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                        NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
    _prefsWindow.title = @(TOTK_UI("设置", "Settings"));
    _prefsWindow.releasedWhenClosed = NO;
    _prefsWindow.delegate = self;
    _prefsWindow.tabbingMode = NSWindowTabbingModeDisallowed;
    _prefsWindow.collectionBehavior = NSWindowCollectionBehaviorFullScreenAuxiliary;
    _prefsWindow.hidesOnDeactivate = NO;
    _prefsWindow.contentMinSize = NSMakeSize(680, 480);

    NSView *content = _prefsWindow.contentView;

    _tabs = [[NSTabView alloc] initWithFrame:NSMakeRect(12, 32, 736, 498)];
    _tabs.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    _tabs.accessibilityLabel = @(TOTK_UI("设置分类", "Settings categories"));
    [content addSubview:_tabs];

    struct { NSString *label; NSView *view; } tabs[] = {
        {@(TOTK_UI("画面", "Picture")), [self buildPictureTab]},
        {@(TOTK_UI("HUD", "HUD")), [self buildHUDTab]},
        {@(TOTK_UI("输入", "Input")), [self buildInputTab]},
        {@(TOTK_UI("语言", "Language")), [self buildLanguageTab]},
    };
    for (size_t i = 0; i < sizeof tabs / sizeof *tabs; i++) {
        NSTabViewItem *item = [[NSTabViewItem alloc] initWithIdentifier:@(i)];
        item.label = tabs[i].label;
        item.view = tabs[i].view;
        [_tabs addTabViewItem:item];
    }

    _status = make_label(@(TOTK_UI("设置会立即保存。", "Changes are saved immediately.")),
                         NSMakeRect(20, 8, 720, 18), [NSFont systemFontOfSize:11], NSColor.secondaryLabelColor);
    _status.autoresizingMask = NSViewWidthSizable | NSViewMaxYMargin;
    _status.accessibilityLabel = @(TOTK_UI("设置状态", "Settings status"));
    [content addSubview:_status];

    if (![_prefsWindow setFrameUsingName:@"TOTKPreferencesWindow"]) [_prefsWindow center];
    [_prefsWindow setFrameAutosaveName:@"TOTKPreferencesWindow"];

    __weak TOTKPreferencesController *weakSelf = self;
    _keyMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown handler:^NSEvent *(NSEvent *event) {
        TOTKPreferencesController *strong = weakSelf;
        if (!strong) return event;
        if (event.window != strong->_prefsWindow || s_capture_panel != nil) return event;
        if (event.keyCode == 53) {   /* Escape closes the settings window */
            [strong->_prefsWindow performClose:nil];
            return nil;
        }
        if ((event.modifierFlags & NSEventModifierFlagCommand) && event.keyCode == 13) {   /* Command-W */
            [strong->_prefsWindow performClose:nil];
            return nil;
        }
        return event;
    }];

    [self refreshControls];
}

- (NSView *)buildPictureTab {
    NSView *view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 700, 450)];
    NSFont *headerFont = [NSFont systemFontOfSize:13 weight:NSFontWeightSemibold];
    NSFont *bodyFont = [NSFont systemFontOfSize:11];
    NSColor *bodyColor = NSColor.secondaryLabelColor;

    [view addSubview:make_label(@(TOTK_UI("补帧", "Interpolation")), NSMakeRect(20, 414, 300, 18), headerFont, NSColor.labelColor)];
    [view addSubview:make_label(@(TOTK_UI("在相邻帧之间生成过渡帧，让画面更平滑。", "Generate in-between frames to make motion smoother.")),
                                NSMakeRect(20, 390, 640, 18), bodyFont, bodyColor)];
    _interpPopup = make_popup(@[@(TOTK_UI("关闭", "Off")), @"30+30", @(TOTK_UI("刷新率一半", "Half refresh rate")), @(TOTK_UI("不限帧", "Uncapped"))],
                              NSMakeRect(20, 354, 300, 26), self, @selector(interpChanged:));
    _interpPopup.accessibilityLabel = @(TOTK_UI("补帧模式", "Interpolation mode"));
    [view addSubview:_interpPopup];

    [view addSubview:make_label(@(TOTK_UI("画质", "Picture")), NSMakeRect(20, 326, 300, 18), headerFont, NSColor.labelColor)];
    [view addSubview:make_label(@(TOTK_UI("选择输出的画面模式；4K 以超采样呈现，GPU 负载明显更高。", "Choose the output picture mode; 4K is supersampled and costs noticeably more GPU time.")),
                                NSMakeRect(20, 302, 650, 18), bodyFont, bodyColor)];
    _picturePopup = make_popup(@[@(TOTK_UI("关闭（1080p）", "Off (1080p)")), @"MetalFX 2K", @(TOTK_UI("原生 2K", "Native 2K")),
                                  @"MetalFX 4K", @(TOTK_UI("原生 4K（实验性）", "Native 4K (experimental)"))],
                               NSMakeRect(20, 272, 300, 26), self, @selector(pictureChanged:));
    for (int i = 0; i < 5; i++) {
        NSMenuItem *preset = [_picturePopup itemAtIndex:i];
        preset.tag = TOTKMenuTagPictureOff + i;
    }
    _picturePopup.accessibilityLabel = @(TOTK_UI("画质模式", "Picture mode"));
    [view addSubview:_picturePopup];

    [view addSubview:make_label(@(TOTK_UI("高级输出", "Advanced output")), NSMakeRect(20, 238, 300, 18), headerFont, NSColor.labelColor)];
    [view addSubview:make_label(@(TOTK_UI("输出目标", "Output target")), NSMakeRect(20, 216, 200, 16), bodyFont, bodyColor)];
    [view addSubview:make_label(@(TOTK_UI("放大方式", "Upscaler")), NSMakeRect(240, 216, 200, 16), bodyFont, bodyColor)];
    [view addSubview:make_label(@(TOTK_UI("场景渲染", "Scene render")), NSMakeRect(460, 216, 200, 16), bodyFont, bodyColor)];

    _outputPopup = make_popup(@[@"1080p", @"1440p", @"4K"], NSMakeRect(20, 190, 200, 26), self, @selector(outputTargetChanged:));
    _outputPopup.accessibilityLabel = @(TOTK_UI("输出目标分辨率", "Output target resolution"));
    [view addSubview:_outputPopup];
    _upscalerPopup = make_popup(@[@(TOTK_UI("原生", "Native")), @"MetalFX"], NSMakeRect(240, 190, 200, 26), self, @selector(upscalerChanged:));
    _upscalerPopup.accessibilityLabel = @(TOTK_UI("放大方式", "Upscaler"));
    [view addSubview:_upscalerPopup];
    _renderPopup = make_popup(@[@(TOTK_UI("游戏默认", "Game default")), @"720p", @"1080p", @"1440p", @(TOTK_UI("4K（实验性）", "4K (experimental)"))],
                              NSMakeRect(460, 190, 200, 26), self, @selector(renderChanged:));
    _renderPopup.accessibilityLabel = @(TOTK_UI("场景渲染分辨率", "Scene render resolution"));
    [view addSubview:_renderPopup];

    _pictureRestartLabel = make_multiline_label(@"", NSMakeRect(20, 158, 650, 30), bodyFont, NSColor.systemOrangeColor);
    _pictureRestartLabel.hidden = YES;
    _pictureRestartLabel.accessibilityLabel = @(TOTK_UI("画质重启提示", "Picture restart notice"));
    [view addSubview:_pictureRestartLabel];

    [view addSubview:make_label(@(TOTK_UI("宽屏", "Ultrawide")), NSMakeRect(20, 130, 300, 18), headerFont, NSColor.labelColor)];
    _aspectCheck = make_checkbox(@(TOTK_UI("启用宽屏画面（游戏版本支持时）", "Enable the ultrawide picture (when the game version supports it)")),
                                 NSMakeRect(20, 106, 560, 22), self, @selector(aspectChanged:));
    _aspectCheck.accessibilityLabel = _aspectCheck.title;
    [view addSubview:_aspectCheck];

    [view addSubview:make_label(@(TOTK_UI("宽屏比例", "Aspect ratio")), NSMakeRect(20, 82, 200, 18), bodyFont, NSColor.labelColor)];
    _ratioPopup = make_popup(@[@"21:9", @"32:9", @"3440 × 1440", @(TOTK_UI("自定义…", "Custom…"))],
                             NSMakeRect(20, 56, 190, 26), self, @selector(ratioPresetChanged:));
    _ratioPopup.accessibilityLabel = @(TOTK_UI("宽屏比例预设", "Aspect ratio preset"));
    [view addSubview:_ratioPopup];

    _customRatioField = [[NSTextField alloc] initWithFrame:NSMakeRect(220, 56, 100, 24)];
    _customRatioField.delegate = self;
    _customRatioField.placeholderString = @"2.35";
    _customRatioField.accessibilityLabel = @(TOTK_UI("自定义宽屏比例", "Custom aspect ratio"));
    [view addSubview:_customRatioField];
    _customRatioButton = make_button(@(TOTK_UI("应用", "Apply")), NSMakeRect(328, 56, 74, 24), self, @selector(applyCustomRatio:));
    _customRatioButton.accessibilityLabel = @(TOTK_UI("应用自定义宽屏比例", "Apply the custom aspect ratio"));
    [view addSubview:_customRatioButton];

    [view addSubview:make_multiline_label(@(TOTK_UI("4K 会把 3840×2160 的画面缩放到当前窗口，属于超采样，不要求 4K 显示器；GPU 占用、显存和功耗会明显升高。原生 4K 为实验性功能，画质更改可能需要重新启动游戏。自定义比例必须在 0.5 到 8.0 之间。",
                                                     "4K renders at 3840×2160 and downscales into the current window (supersampling), so it does not require a 4K display; GPU, memory and power use rise noticeably. Native 4K is experimental and a picture change may need a game restart. A custom ratio must be between 0.5 and 8.0.")),
                                          NSMakeRect(20, 8, 650, 42), bodyFont, bodyColor)];
    return view;
}

- (NSView *)buildHUDTab {
    NSView *view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 700, 450)];
    NSFont *headerFont = [NSFont systemFontOfSize:13 weight:NSFontWeightSemibold];
    NSFont *bodyFont = [NSFont systemFontOfSize:11];
    NSColor *bodyColor = NSColor.secondaryLabelColor;

    [view addSubview:make_label(@(TOTK_UI("HUD 布局", "HUD layout")), NSMakeRect(20, 414, 300, 18), headerFont, NSColor.labelColor)];
    [view addSubview:make_label(@(TOTK_UI("紧凑：把覆盖层放进窗口右上角的小面板；角落：把覆盖层画在画面的角落。",
                                          "Compact: put the overlays in a small panel at the window's top-right; Corners: draw them in the picture's corners.")),
                                NSMakeRect(20, 382, 650, 28), bodyFont, bodyColor)];
    _hudPopup = make_popup(@[@(TOTK_UI("紧凑", "Compact")), @(TOTK_UI("角落", "Corners"))],
                           NSMakeRect(20, 346, 240, 26), self, @selector(hudLayoutChanged:));
    _hudPopup.accessibilityLabel = @(TOTK_UI("HUD 布局", "HUD layout"));
    [view addSubview:_hudPopup];

    [view addSubview:make_label(@(TOTK_UI("提示", "Hints")), NSMakeRect(20, 302, 300, 18), headerFont, NSColor.labelColor)];
    _hintsCheck = make_checkbox(@(TOTK_UI("显示游戏内的快捷键提示", "Show the in-game shortcut hints")),
                                NSMakeRect(20, 274, 500, 22), self, @selector(hintsChanged:));
    _hintsCheck.accessibilityLabel = _hintsCheck.title;
    [view addSubview:_hintsCheck];

    [view addSubview:make_label(@(TOTK_UI("覆盖层", "Overlays")), NSMakeRect(20, 232, 300, 18), headerFont, NSColor.labelColor)];
    _fpsCheck = make_checkbox(@"FPS", NSMakeRect(20, 202, 400, 22), self, @selector(fpsOverlayChanged:));
    _fpsCheck.accessibilityLabel = @(TOTK_UI("显示 FPS 覆盖层", "Show the FPS overlay"));
    [view addSubview:_fpsCheck];
    _shaderCheck = make_checkbox(@(TOTK_UI("着色器", "Shader")), NSMakeRect(20, 174, 400, 22), self, @selector(shaderOverlayChanged:));
    _shaderCheck.accessibilityLabel = @(TOTK_UI("显示着色器覆盖层", "Show the shader overlay"));
    [view addSubview:_shaderCheck];
    _loadCheck = make_checkbox(@(TOTK_UI("系统负载", "System Load")), NSMakeRect(20, 146, 400, 22), self, @selector(loadOverlayChanged:));
    _loadCheck.accessibilityLabel = @(TOTK_UI("显示系统负载覆盖层", "Show the system load overlay"));
    [view addSubview:_loadCheck];

    [view addSubview:make_multiline_label(@(TOTK_UI("按 1 可以在游戏内快速显示或隐藏所有提示和覆盖层。",
                                                     "Press 1 in-game to quickly show or hide all hints and overlays.")),
                                          NSMakeRect(20, 96, 650, 34), bodyFont, bodyColor)];
    return view;
}

- (NSView *)buildInputTab {
    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 700, 450)];
    scroll.hasVerticalScroller = YES;
    scroll.autohidesScrollers = YES;
    scroll.drawsBackground = NO;
    scroll.borderType = NSNoBorder;
    scroll.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    scroll.accessibilityLabel = @(TOTK_UI("键盘与手柄按键映射", "Keyboard and controller mappings"));

    TOTKFlippedView *doc = [[TOTKFlippedView alloc] initWithFrame:NSMakeRect(0, 0, 690, 1216)];
    doc.autoresizingMask = NSViewWidthSizable;
    CGFloat y = 8;
    NSFont *bodyFont = [NSFont systemFontOfSize:12];
    NSFont *headerFont = [NSFont systemFontOfSize:13 weight:NSFontWeightSemibold];
    NSColor *bodyColor = NSColor.secondaryLabelColor;

    _profilePopup = make_popup(@[@(TOTK_UI("默认", "Default"))], NSMakeRect(0, y, 205, 26), self, @selector(profileChanged:));
    _profilePopup.accessibilityLabel = @(TOTK_UI("键位配置", "Keymap profile"));
    [doc addSubview:_profilePopup];

    NSButton *newButton = make_button(@(TOTK_UI("新建", "New")), NSMakeRect(214, y, 72, 26), self, @selector(newProfile:));
    newButton.image = symbol_image(@"plus");
    newButton.imagePosition = NSImageLeading;
    newButton.accessibilityLabel = @(TOTK_UI("新建键位配置", "New keymap profile"));
    [doc addSubview:newButton];

    NSButton *duplicateButton = make_button(@(TOTK_UI("复制", "Duplicate")), NSMakeRect(290, y, 82, 26), self, @selector(duplicateProfile:));
    duplicateButton.image = symbol_image(@"square.on.square");
    duplicateButton.imagePosition = NSImageLeading;
    duplicateButton.accessibilityLabel = @(TOTK_UI("复制当前键位配置", "Duplicate the current keymap profile"));
    [doc addSubview:duplicateButton];

    NSButton *renameButton = make_button(@(TOTK_UI("重命名", "Rename")), NSMakeRect(376, y, 82, 26), self, @selector(renameProfile:));
    renameButton.image = symbol_image(@"pencil");
    renameButton.imagePosition = NSImageLeading;
    renameButton.accessibilityLabel = @(TOTK_UI("重命名当前键位配置", "Rename the current keymap profile"));
    [doc addSubview:renameButton];

    _deleteProfileButton = make_button(@(TOTK_UI("删除", "Delete")), NSMakeRect(462, y, 72, 26), self, @selector(deleteProfile:));
    _deleteProfileButton.image = symbol_image(@"trash");
    _deleteProfileButton.imagePosition = NSImageLeading;
    _deleteProfileButton.accessibilityLabel = @(TOTK_UI("删除当前键位配置", "Delete the current keymap profile"));
    [doc addSubview:_deleteProfileButton];

    NSButton *resetButton = make_button(@(TOTK_UI("重置", "Reset")), NSMakeRect(538, y, 82, 26), self, @selector(resetProfile:));
    resetButton.image = symbol_image(@"arrow.counterclockwise");
    resetButton.imagePosition = NSImageLeading;
    resetButton.accessibilityLabel = @(TOTK_UI("重置当前键位配置", "Reset the current keymap profile"));
    [doc addSubview:resetButton];

    y += 36;
    [doc addSubview:make_label(@(TOTK_UI("键盘（点“更改…”后按下一个键；Esc 取消，Delete 清除）",
                                        "Keyboard (click Change, then press a key; Escape cancels, Delete clears)")),
                                NSMakeRect(0, y, 690, 18), headerFont, NSColor.labelColor)];
    y += 24;

    for (int action = 1; action < TOTK_ACTION_COUNT; action++) {
        [doc addSubview:make_label(action_title(action), NSMakeRect(0, y, 180, 20), bodyFont, NSColor.labelColor)];
        NSTextField *key = make_label(@(TOTK_UI("未绑定", "Unbound")), NSMakeRect(188, y, 270, 20), bodyFont, bodyColor);
        key.accessibilityLabel = [NSString stringWithFormat:@(TOTK_UI("%@ 的当前按键", "Current key for %@")), action_title(action)];
        [doc addSubview:key];
        _keyLabels[action] = key;
        NSButton *change = make_button(@(TOTK_UI("更改…", "Change…")), NSMakeRect(468, y - 2, 94, 24), self, @selector(changeKey:));
        change.tag = action;
        change.accessibilityLabel = [NSString stringWithFormat:@(TOTK_UI("更改 %@ 的按键", "Change the key for %@")), action_title(action)];
        [doc addSubview:change];
        y += 26;
    }

    y += 12;
    [doc addSubview:make_label(@(TOTK_UI("手柄（目标：Switch 按键；模式：按住 / 轻按 / 切换 / 瞄准）",
                                        "Controller (target: Switch button; mode: Hold / Tap / Toggle / Aim)")),
                                NSMakeRect(0, y, 690, 18), headerFont, NSColor.labelColor)];
    y += 24;

    NSMutableArray<NSString *> *targetTitles = [NSMutableArray arrayWithCapacity:CONTROLLER_SWITCH_BUTTONS + 1];
    for (int i = 0; i <= CONTROLLER_SWITCH_BUTTONS; i++) [targetTitles addObject:switch_title(i)];
    NSMutableArray<NSString *> *modeTitles = [NSMutableArray arrayWithCapacity:4];
    for (int i = 0; i <= CONTROLLER_AIM; i++) [modeTitles addObject:mode_title(i)];
    NSMutableArray<NSString *> *fireTitles = [NSMutableArray arrayWithCapacity:CONTROLLER_BUTTONS];
    for (int i = 0; i < CONTROLLER_BUTTONS; i++) [fireTitles addObject:physical_title(i)];

    for (int physical = 0; physical < CONTROLLER_BUTTONS; physical++) {
        [doc addSubview:make_label(physical_title(physical), NSMakeRect(0, y, 88, 20), bodyFont, NSColor.labelColor)];

        NSPopUpButton *target = make_popup(targetTitles, NSMakeRect(94, y - 3, 130, 26), self, @selector(controllerChanged:));
        target.tag = 100 + physical;
        target.accessibilityLabel = [NSString stringWithFormat:@(TOTK_UI("%@ 的目标按键", "Target button for %@")), physical_title(physical)];
        [doc addSubview:target];
        _ctrlTarget[physical] = target;

        NSPopUpButton *mode = make_popup(modeTitles, NSMakeRect(230, y - 3, 104, 26), self, @selector(controllerChanged:));
        mode.tag = 200 + physical;
        mode.accessibilityLabel = [NSString stringWithFormat:@(TOTK_UI("%@ 的按住模式", "Hold mode for %@")), physical_title(physical)];
        [doc addSubview:mode];
        _ctrlMode[physical] = mode;

        NSPopUpButton *fire = make_popup(fireTitles, NSMakeRect(340, y - 3, 136, 26), self, @selector(controllerChanged:));
        fire.tag = 300 + physical;
        fire.accessibilityLabel = [NSString stringWithFormat:@(TOTK_UI("%@ 的瞄准开火键", "Aim fire button for %@")), physical_title(physical)];
        for (NSMenuItem *item in fire.itemArray)
            if (item.tag == physical) item.enabled = NO;
        [doc addSubview:fire];
        _ctrlFire[physical] = fire;
        y += 28;
    }

    doc.frame = NSMakeRect(0, 0, 690, y + 12);
    scroll.documentView = doc;
    return scroll;
}

- (NSView *)buildLanguageTab {
    NSView *view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 700, 450)];
    NSFont *headerFont = [NSFont systemFontOfSize:13 weight:NSFontWeightSemibold];
    NSFont *bodyFont = [NSFont systemFontOfSize:11];
    NSColor *bodyColor = NSColor.secondaryLabelColor;

    [view addSubview:make_label(@(TOTK_UI("游戏语言", "Game language")), NSMakeRect(20, 414, 300, 18), headerFont, NSColor.labelColor)];
    [view addSubview:make_label(@(TOTK_UI("跟随系统，或固定为一种游戏支持的语言。", "Follow the system, or pin one of the languages the game supports.")),
                                NSMakeRect(20, 390, 640, 18), bodyFont, bodyColor)];
    NSMutableArray<NSString *> *gameTitles = [NSMutableArray arrayWithCapacity:game_language_count];
    for (size_t i = 0; i < game_language_count; i++) [gameTitles addObject:@(TOTK_UI(game_languages[i].zh, game_languages[i].en))];
    _gameLangPopup = make_popup(gameTitles, NSMakeRect(20, 354, 320, 26), self, @selector(gameLanguageChanged:));
    _gameLangPopup.accessibilityLabel = @(TOTK_UI("游戏语言", "Game language"));
    [view addSubview:_gameLangPopup];

    [view addSubview:make_label(@(TOTK_UI("界面语言", "Interface language")), NSMakeRect(20, 306, 300, 18), headerFont, NSColor.labelColor)];
    [view addSubview:make_label(@(TOTK_UI("菜单、设置窗口和提示使用的语言。", "The language of the menu bar, settings window and hints.")),
                                NSMakeRect(20, 282, 640, 18), bodyFont, bodyColor)];
    NSMutableArray<NSString *> *uiTitles = [NSMutableArray arrayWithCapacity:ui_language_count];
    for (size_t i = 0; i < ui_language_count; i++) [uiTitles addObject:@(TOTK_UI(ui_languages[i].zh, ui_languages[i].en))];
    _uiLangPopup = make_popup(uiTitles, NSMakeRect(20, 246, 320, 26), self, @selector(uiLanguageChanged:));
    _uiLangPopup.accessibilityLabel = @(TOTK_UI("界面语言", "Interface language"));
    [view addSubview:_uiLangPopup];

    NSColor *noteColor = NSColor.systemOrangeColor;
    [view addSubview:make_multiline_label(@(TOTK_UI("更改游戏语言后需要重新启动游戏。界面语言会在下次启动时生效。",
                                                     "Changing the game language takes effect after restarting the game. The interface language applies at the next launch.")),
                                          NSMakeRect(20, 170, 640, 48), [NSFont systemFontOfSize:12], noteColor)];
    return view;
}

/* ------------------------------------------------------------------------- */
/* Window refresh. */

- (void)refresh {
    [self refreshControls];
}

- (void)refreshControls {
    if (!_prefsWindow) return;

    int interp = totk_setting_interp();
    if (interp >= TOTK_INTERP_OFF && interp <= TOTK_INTERP_MAX) [_interpPopup selectItemAtIndex:interp];
    BOOL interpEnabled = totk_capability_interp() != 0 && !totk_setting_locked("interp");
    _interpPopup.enabled = interpEnabled;
    _interpPopup.toolTip = [self tooltipForTag:TOTKMenuTagInterpOff enabled:interpEnabled];

    int outputRes = totk_setting_output_res();
    int outputUpscaler = totk_setting_upscaler();
    int picturePreset = picture_preset_index(outputRes, outputUpscaler);
    BOOL pictureEnabled = totk_capability_output() != 0 && !totk_setting_locked("picture");
    if (picturePreset >= 0 && picturePreset <= TOTKPicturePresetNative4K)
        [_picturePopup selectItemAtIndex:picturePreset];
    for (int i = 0; i < 5; i++) {
        NSMenuItem *preset = [_picturePopup itemAtIndex:i];
        preset.tag = TOTKMenuTagPictureOff + i;
        preset.enabled = pictureEnabled && picture_tag_available(preset.tag);
        preset.toolTip = preset.enabled ? nil : picture_unavailable_reason(preset.tag);
    }
    _picturePopup.enabled = pictureEnabled;
    _picturePopup.toolTip = [self tooltipForTag:TOTKMenuTagPictureOff enabled:pictureEnabled];

    _outputPopup.enabled = pictureEnabled;
    if (outputRes >= TOTK_RES_1080 && outputRes <= TOTK_RES_2160) [_outputPopup selectItemAtIndex:outputRes];
    _upscalerPopup.enabled = pictureEnabled;
    [_upscalerPopup selectItemAtIndex:outputUpscaler == TOTK_UPSCALE_METALFX ? 1 : 0];
    NSMenuItem *mfxItem = [_upscalerPopup itemAtIndex:1];
    mfxItem.enabled = pictureEnabled && outputRes != TOTK_RES_1080 &&
                      !(outputRes == TOTK_RES_2160 && !picture_mfx4k_available());
    if (!mfxItem.enabled) {
        mfxItem.toolTip = outputRes == TOTK_RES_1080
                              ? @(TOTK_UI("1080p 只能使用原生放大。", "1080p only supports native scaling."))
                              : picture_unavailable_reason(TOTKMenuTagPictureMFX4K);
    } else {
        mfxItem.toolTip = nil;
    }

    BOOL renderEnabled = !totk_setting_locked("render");
    _renderPopup.enabled = renderEnabled;
    int renderRes = totk_setting_render_res();
    if (renderRes >= TOTK_RENDER_GAME && renderRes <= TOTK_RENDER_2160) [_renderPopup selectItemAtIndex:renderRes];
    NSMenuItem *render4K = [_renderPopup itemAtIndex:TOTK_RENDER_2160];
    render4K.enabled = renderEnabled && picture_native4k_available() && picture_native4k_selected();
    render4K.toolTip = render4K.enabled ? nil :
        (picture_native4k_available()
            ? @(TOTK_UI("先选择“原生 4K”作为画质预设，才能使用 4K 场景渲染。",
                        "Select Native 4K as the picture preset before enabling a 4K scene render."))
            : picture_unavailable_reason(TOTKMenuTagPictureNative4K));

    NSString *restartText = nil;
    if (outputRes == TOTK_RES_2160 && runtime_output_max_res() != TOTK_RES_2160) {
        restartText = outputUpscaler == TOTK_UPSCALE_METALFX
                          ? @(TOTK_UI("MetalFX 4K 已保存，需要重新启动游戏后生效。",
                                      "MetalFX 4K is saved; restart the game for it to take effect."))
                          : @(TOTK_UI("原生 4K（实验性）已保存，需要重新启动游戏后生效。",
                                      "Native 4K (experimental) is saved; restart the game for it to take effect."));
    } else if (totk_setting_picture_restart_required()) {
        restartText = @(TOTK_UI("画质更改已保存，需要重新启动游戏后生效。",
                                "The picture change is saved; restart the game for it to take effect."));
    }
    _pictureRestartLabel.stringValue = restartText ?: @"";
    _pictureRestartLabel.hidden = restartText == nil;
    _pictureRestartLabel.toolTip = restartText;

    BOOL aspectEnabled = totk_capability_aspect() != 0 && !totk_setting_locked("aspect");
    _aspectCheck.enabled = aspectEnabled;
    _aspectCheck.toolTip = [self tooltipForTag:TOTKMenuTagAspectToggle enabled:aspectEnabled];
    _aspectCheck.state = totk_setting_aspect() ? NSControlStateValueOn : NSControlStateValueOff;
    _ratioPopup.enabled = aspectEnabled;
    _ratioPopup.toolTip = [self tooltipForTag:TOTKMenuTagRatio21 enabled:aspectEnabled];
    int preset = ratio_preset_index(totk_setting_aspect_ratio());
    [_ratioPopup selectItemAtIndex:preset < 0 ? 3 : preset];
    _customRatioField.stringValue = ratio_string(totk_setting_aspect_ratio());
    _customRatioField.enabled = aspectEnabled && preset < 0;
    _customRatioButton.enabled = aspectEnabled && preset < 0;

    BOOL hudEnabled = !totk_setting_locked("hud");
    _hudPopup.enabled = hudEnabled;
    _hudPopup.toolTip = [self tooltipForTag:TOTKMenuTagHudCompact enabled:hudEnabled];
    [_hudPopup selectItemAtIndex:totk_setting_hud_corners() ? 1 : 0];

    _hintsCheck.state = totk_setting_hints() ? NSControlStateValueOn : NSControlStateValueOff;
    _fpsCheck.state = totk_setting_fps_overlay() ? NSControlStateValueOn : NSControlStateValueOff;
    _shaderCheck.state = totk_setting_shader_overlay() ? NSControlStateValueOn : NSControlStateValueOff;
    _loadCheck.state = totk_setting_load_overlay() ? NSControlStateValueOn : NSControlStateValueOff;
    _fpsCheck.enabled = !totk_setting_locked("fps");
    _shaderCheck.enabled = !totk_setting_locked("shader");
    _loadCheck.enabled = !totk_setting_locked("load");
    _fpsCheck.toolTip = [self tooltipForTag:TOTKMenuTagOverlayFPS enabled:_fpsCheck.enabled];
    _shaderCheck.toolTip = [self tooltipForTag:TOTKMenuTagOverlayShader enabled:_shaderCheck.enabled];
    _loadCheck.toolTip = [self tooltipForTag:TOTKMenuTagOverlayLoad enabled:_loadCheck.enabled];

    [self rebuildProfilePopup];
    [self updateKeyLabels];
    [self updateControllerControls];

    NSInteger gameIndex = game_language_index(totk_setting_game_language());
    if (gameIndex >= 0 && (size_t)gameIndex < game_language_count) [_gameLangPopup selectItemAtIndex:gameIndex];
    NSInteger uiIndex = ui_language_index(totk_setting_ui_language());
    if (uiIndex >= 0 && (size_t)uiIndex < ui_language_count) [_uiLangPopup selectItemAtIndex:uiIndex];
    BOOL gameLangEnabled = !totk_setting_locked("lang");
    BOOL uiLangEnabled = !totk_setting_locked("ui_lang");
    _gameLangPopup.enabled = gameLangEnabled;
    _uiLangPopup.enabled = uiLangEnabled;
    _gameLangPopup.toolTip = [self tooltipForTag:TOTKMenuTagGameLangBase enabled:gameLangEnabled];
    _uiLangPopup.toolTip = [self tooltipForTag:TOTKMenuTagUILangBase enabled:uiLangEnabled];

    [self rebuildInputMenu];
}

- (void)rebuildProfilePopup {
    if (!_profilePopup) return;
    [_profilePopup removeAllItems];
    int count = totk_profile_count();
    for (int i = 0; i < count && i < TOTK_PROFILE_MAX; i++) {
        const char *raw = totk_profile_name(i);
        NSString *name = raw && *raw ? @(raw) : @(TOTK_UI("未命名", "Untitled"));
        if (totk_profile_builtin(i)) name = [name stringByAppendingString:@(TOTK_UI("（内置）", " (built-in)"))];
        [_profilePopup addItemWithTitle:name];
    }
    if (count <= 0) [_profilePopup addItemWithTitle:@(TOTK_UI("没有配置", "No profiles"))];
    int active = totk_profile_active();
    if (active >= 0 && active < [_profilePopup numberOfItems]) [_profilePopup selectItemAtIndex:active];
    _deleteProfileButton.enabled = active >= 0 && !totk_profile_builtin(active) && count > 1;
    _deleteProfileButton.toolTip = _deleteProfileButton.enabled ? @"" :
        @(TOTK_UI("内置配置不能删除，并且至少保留一个配置。", "The built-in profile cannot be deleted, and at least one profile is kept."));
}

- (void)updateKeyLabels {
    int active = totk_profile_active();
    for (int action = 1; action < TOTK_ACTION_COUNT; action++) {
        if (!_keyLabels[action]) continue;
        int key = keycode_for_action(active, action);
        if (key >= 0) {
            const char *name = totk_keycode_name((unsigned)key);
            _keyLabels[action].stringValue = name && *name ? @(name) : [NSString stringWithFormat:@"#%d", key];
        } else {
            _keyLabels[action].stringValue = @(TOTK_UI("未绑定", "Unbound"));
        }
    }
}

- (void)updateControllerControls {
    int active = totk_profile_active();
    BOOL usable = active >= 0;
    for (int physical = 0; physical < CONTROLLER_BUTTONS; physical++) {
        int target = usable ? totk_profile_controller_target(active, physical) : -1;
        int mode = usable ? controller_mode_kind((uint8_t)totk_profile_controller_mode(active, physical)) : CONTROLLER_HOLD;
        int fire = usable ? totk_profile_controller_fire(active, physical) : 0;
        if (mode < 0 || mode > CONTROLLER_AIM) mode = CONTROLLER_HOLD;
        if (fire < 0 || fire >= CONTROLLER_BUTTONS || fire == physical) fire = 0;
        NSInteger targetIndex = target < 0 ? 0 : (target >= CONTROLLER_SWITCH_BUTTONS ? 0 : target + 1);
        [_ctrlTarget[physical] selectItemAtIndex:targetIndex];
        [_ctrlMode[physical] selectItemAtIndex:mode];
        [_ctrlFire[physical] selectItemAtIndex:fire];
        _ctrlTarget[physical].enabled = usable;
        _ctrlMode[physical].enabled = usable && target >= 0;
        _ctrlFire[physical].enabled = usable && target >= 0 && mode == CONTROLLER_AIM;
        _ctrlFire[physical].toolTip = _ctrlFire[physical].enabled
            ? @(TOTK_UI("瞄准时按下这个键就会开火。", "Pressing this button fires while aiming."))
            : @(TOTK_UI("只有“瞄准”模式使用开火键。", "The fire button is used only in Aim mode."));
    }
}

/* ------------------------------------------------------------------------- */
/* Control actions. */

- (void)applySettingResult:(int)rc what:(NSString *)what {
    [self reportResult:rc what:what];
    [self refreshControls];
}

- (void)interpChanged:(NSPopUpButton *)sender {
    int mode = (int)sender.indexOfSelectedItem;
    [self applySettingResult:totk_setting_set_interp(mode) what:@(TOTK_UI("补帧模式", "Interpolation mode"))];
}

- (void)pictureChanged:(NSPopUpButton *)sender {
    int preset = (int)sender.indexOfSelectedItem;
    if (preset < 0 || preset > TOTKPicturePresetNative4K) return;
    int res = 0, upscaler = 0;
    picture_preset_values(preset, &res, &upscaler);
    [self applyPictureRes:res upscaler:upscaler what:sender.titleOfSelectedItem ?: @(TOTK_UI("画质模式", "Picture mode"))];
}

- (void)outputTargetChanged:(NSPopUpButton *)sender {
    int res = (int)sender.indexOfSelectedItem;
    if (res < TOTK_RES_1080 || res > TOTK_RES_2160) return;
    [self applyPictureRes:res upscaler:totk_setting_upscaler() what:@(TOTK_UI("输出目标", "Output target"))];
}

- (void)upscalerChanged:(NSPopUpButton *)sender {
    int upscaler = (int)sender.indexOfSelectedItem == 1 ? TOTK_UPSCALE_METALFX : TOTK_UPSCALE_NATIVE;
    [self applyPictureRes:totk_setting_output_res() upscaler:upscaler what:@(TOTK_UI("放大方式", "Upscaler"))];
}

- (void)renderChanged:(NSPopUpButton *)sender {
    int render = (int)sender.indexOfSelectedItem;
    if (render < TOTK_RENDER_GAME || render > TOTK_RENDER_2160) return;
    if (render == TOTK_RENDER_2160 && !picture_native4k_selected()) {
        [self showError:(picture_native4k_available()
            ? @(TOTK_UI("4K 场景渲染只支持“原生 4K”输出；请先切换到原生 4K。",
                        "A 4K scene render requires Native 4K output; select Native 4K first."))
            : picture_unavailable_reason(TOTKMenuTagPictureNative4K))];
        [self refreshControls];
        return;
    }
    [self applySettingResult:totk_setting_set_render_res(render) what:@(TOTK_UI("场景渲染", "Scene render"))];
}

- (void)applyPicturePreset:(int)preset what:(NSString *)what {
    if (preset < 0 || preset > TOTKPicturePresetNative4K) return;
    int res = 0, upscaler = 0;
    picture_preset_values(preset, &res, &upscaler);
    [self applyPictureRes:res upscaler:upscaler what:what];
}

- (void)applyPictureRes:(int)res upscaler:(int)upscaler what:(NSString *)what {
    if (res == TOTK_RES_1080 && upscaler == TOTK_UPSCALE_METALFX) {
        [self showError:@(TOTK_UI("1080p 只能使用原生放大；请先切换到 2K 或选择原生。",
                                 "1080p only supports native scaling; switch to 2K or choose Native first."))];
        [self refreshControls];
        return;
    }
    if (res == TOTK_RES_2160 && upscaler == TOTK_UPSCALE_METALFX && !picture_mfx4k_available()) {
        [self showError:picture_unavailable_reason(TOTKMenuTagPictureMFX4K)];
        [self refreshControls];
        return;
    }
    if (res == TOTK_RES_2160 && upscaler == TOTK_UPSCALE_NATIVE && !picture_native4k_available()) {
        [self showError:picture_unavailable_reason(TOTKMenuTagPictureNative4K)];
        [self refreshControls];
        return;
    }

    int currentRes = totk_setting_output_res();
    int currentUpscaler = totk_setting_upscaler();
    int targetPreset = picture_preset_index(res, upscaler);
    if (res == TOTK_RES_2160 &&
        targetPreset != picture_preset_index(currentRes, currentUpscaler) &&
        ![self confirm4KPreset:targetPreset]) {
        [self refreshControls];
        return;
    }

    /* Setters commit one field at a time.  Drop MetalFX first when going to
       native (MetalFX + 1080p is invalid), otherwise move the output target
       first and let the upscaler follow.  Each step persists independently,
       so a restart-required step still lets the next step commit. */
    int rc = TOTK_SET_OK;
    int restart = 0;
    if (upscaler == TOTK_UPSCALE_NATIVE && currentUpscaler == TOTK_UPSCALE_METALFX) {
        rc = totk_setting_set_upscaler(TOTK_UPSCALE_NATIVE);
        if (rc < 0) goto picture_done;
        if (rc == TOTK_SET_RESTART) restart = 1;
        currentUpscaler = TOTK_UPSCALE_NATIVE;
        if (currentRes == res) goto picture_done;
    }
    if (currentRes != res) {
        rc = totk_setting_set_output_res(res);
        if (rc < 0) goto picture_done;
        if (rc == TOTK_SET_RESTART) restart = 1;
    }
    if (totk_setting_upscaler() != upscaler) {
        rc = totk_setting_set_upscaler(upscaler);
        if (rc < 0) goto picture_done;
        if (rc == TOTK_SET_RESTART) restart = 1;
    }

picture_done:
    if (rc < 0) {
        [self reportResult:rc what:what];
        [self refreshControls];
        return;
    }
    if (restart) rc = TOTK_SET_RESTART;
    [self refreshControls];
    if (!_pictureRestartLabel.hidden)
        [self setStatus:_pictureRestartLabel.stringValue warning:YES];
    else
        [self reportResult:rc what:what];
}

- (BOOL)confirm4KPreset:(int)preset {
    NSAlert *alert = [NSAlert new];
    if (preset == TOTKPicturePresetNative4K) {
        alert.messageText = @(TOTK_UI("启用原生 4K（实验性）？", "Enable Native 4K (experimental)?"));
        alert.informativeText = @(TOTK_UI("游戏将以 3840×2160 原生渲染并缩放到当前窗口，不要求 4K 显示器。GPU 与显存占用会显著上升，帧率可能下降；需要重新启动游戏后生效。",
                                           "The game renders natively at 3840×2160 and downscales into the current window; a 4K display is not required. GPU and memory use rise sharply and frame rate may drop; restart the game for it to take effect."));
    } else {
        alert.messageText = @(TOTK_UI("启用 MetalFX 4K？", "Enable MetalFX 4K?"));
        alert.informativeText = @(TOTK_UI("MetalFX 会把 4K 画面呈现到当前窗口，GPU 负载较高。如果当前进程不是以 4K 启动的，保存后需要重新启动游戏。",
                                           "MetalFX presents a 4K picture into the current window and costs more GPU time. If this process did not start at 4K, restart the game after saving."));
    }
    [alert addButtonWithTitle:@(TOTK_UI("启用", "Enable"))];
    [alert addButtonWithTitle:@(TOTK_UI("取消", "Cancel"))];
    return [alert runModal] == NSAlertFirstButtonReturn;
}

- (void)setStatus:(NSString *)text warning:(BOOL)warning {
    if (!_status) return;
    _status.stringValue = text;
    _status.textColor = warning ? NSColor.systemOrangeColor : NSColor.secondaryLabelColor;
}

- (void)aspectChanged:(NSButton *)sender {
    [self applySettingResult:totk_setting_set_aspect(sender.state == NSControlStateValueOn)
                        what:@(TOTK_UI("宽屏", "Ultrawide"))];
}

- (void)ratioPresetChanged:(NSPopUpButton *)sender {
    NSInteger index = sender.indexOfSelectedItem;
    if (index == 3) {
        _customRatioField.enabled = YES;
        _customRatioButton.enabled = YES;
        [_prefsWindow makeFirstResponder:_customRatioField];
        return;
    }
    if (index < 0 || index > 2) return;
    int rc = totk_setting_set_aspect_ratio((float)kRatioPresets[index]);
    if (rc >= 0) rc = totk_setting_set_aspect(1);
    [self applySettingResult:rc what:@(TOTK_UI("宽屏比例", "Aspect ratio"))];
}

- (void)applyCustomRatio:(id)sender {
    (void)sender;
    double value = 0;
    if (!parse_ratio(_customRatioField.stringValue, &value)) {
        [self showError:@(TOTK_UI("宽屏比例必须在 0.5 到 8.0 之间。", "The aspect ratio must be between 0.5 and 8.0."))];
        _customRatioField.stringValue = ratio_string(totk_setting_aspect_ratio());
        return;
    }
    int rc = totk_setting_set_aspect_ratio((float)value);
    if (rc >= 0) rc = totk_setting_set_aspect(1);
    [self applySettingResult:rc what:@(TOTK_UI("宽屏比例", "Aspect ratio"))];
}

- (void)controlTextDidEndEditing:(NSNotification *)notification {
    if (notification.object == _customRatioField && _customRatioField.enabled) [self applyCustomRatio:nil];
}

- (void)hudLayoutChanged:(NSPopUpButton *)sender {
    int rc = totk_setting_set_hud_corners((int)sender.indexOfSelectedItem == 1);
    [self applySettingResult:rc what:@(TOTK_UI("HUD 布局", "HUD layout"))];
    runtime_shortcut_help_refresh_layout();
}

- (void)hintsChanged:(NSButton *)sender {
    int rc = totk_setting_set_hints(sender.state == NSControlStateValueOn);
    [self applySettingResult:rc what:@(TOTK_UI("提示", "Hints"))];
    runtime_shortcut_help_set_visible(totk_setting_hints());
}

- (void)fpsOverlayChanged:(NSButton *)sender {
    [self applySettingResult:totk_setting_set_fps_overlay(sender.state == NSControlStateValueOn)
                        what:@"FPS"];
}

- (void)shaderOverlayChanged:(NSButton *)sender {
    [self applySettingResult:totk_setting_set_shader_overlay(sender.state == NSControlStateValueOn)
                        what:@(TOTK_UI("着色器覆盖层", "Shader overlay"))];
}

- (void)loadOverlayChanged:(NSButton *)sender {
    [self applySettingResult:totk_setting_set_load_overlay(sender.state == NSControlStateValueOn)
                        what:@(TOTK_UI("系统负载覆盖层", "System load overlay"))];
}

- (void)profileChanged:(NSPopUpButton *)sender {
    int index = (int)sender.indexOfSelectedItem;
    if (index < 0 || index >= totk_profile_count()) return;
    int rc = totk_profile_set_active(index);
    if (rc >= 0) totk_profile_apply();
    [self reportResult:rc what:@(TOTK_UI("键位配置", "Keymap profile"))];
    [self refreshControls];
}

- (NSString *)promptNameWithTitle:(NSString *)title initial:(NSString *)initial {
    NSString *value = initial ?: @"";
    BOOL warned = NO;
    while (YES) {
        NSAlert *alert = [NSAlert new];
        alert.messageText = title;
        alert.informativeText = warned
            ? @(TOTK_UI("名称必须是 1 到 63 个字符，且不能包含控制字符。", "The name must be 1 to 63 characters and contain no control characters."))
            : @(TOTK_UI("名称长度为 1 到 63 个字符。", "The name must be 1 to 63 characters."));
        [alert addButtonWithTitle:@(TOTK_UI("确定", "OK"))];
        [alert addButtonWithTitle:@(TOTK_UI("取消", "Cancel"))];
        NSTextField *field = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 280, 24)];
        field.stringValue = value;
        field.accessibilityLabel = @(TOTK_UI("配置名称", "Profile name"));
        alert.accessoryView = field;
        alert.window.initialFirstResponder = field;
        if ([alert runModal] != NSAlertFirstButtonReturn) return nil;
        NSString *name = [field.stringValue stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
        if (name.length > 0 &&
            [name lengthOfBytesUsingEncoding:NSUTF8StringEncoding] < TOTK_PROFILE_NAME_MAX &&
            [name rangeOfCharacterFromSet:[NSCharacterSet controlCharacterSet]].location == NSNotFound)
            return name;
        value = field.stringValue;
        warned = YES;
        NSBeep();
    }
}

- (void)newProfile:(id)sender {
    (void)sender;
    NSString *name = [self promptNameWithTitle:@(TOTK_UI("新建键位配置", "New keymap profile")) initial:@""];
    if (!name) return;
    int before = totk_profile_count();
    int rc = totk_profile_add(name.UTF8String, -1);
    BOOL ok = [self reportResult:rc what:@(TOTK_UI("键位配置", "Keymap profile"))];
    if (ok && totk_profile_count() > before) {
        totk_profile_set_active(totk_profile_count() - 1);
        totk_profile_apply();
    }
    [self refreshControls];
}

- (void)duplicateProfile:(id)sender {
    (void)sender;
    int active = totk_profile_active();
    if (active < 0) return;
    const char *raw = totk_profile_name(active);
    NSString *suggested = raw && *raw ? [NSString stringWithFormat:@"%@ %@", @(raw), @(TOTK_UI("副本", "copy"))]
                                      : @(TOTK_UI("新配置", "New profile"));
    NSString *name = [self promptNameWithTitle:@(TOTK_UI("复制键位配置", "Duplicate keymap profile")) initial:suggested];
    if (!name) return;
    int before = totk_profile_count();
    int rc = totk_profile_add(name.UTF8String, active);
    BOOL ok = [self reportResult:rc what:@(TOTK_UI("键位配置", "Keymap profile"))];
    if (ok && totk_profile_count() > before) {
        totk_profile_set_active(totk_profile_count() - 1);
        totk_profile_apply();
    }
    [self refreshControls];
}

- (void)renameProfile:(id)sender {
    (void)sender;
    int active = totk_profile_active();
    if (active < 0) return;
    const char *raw = totk_profile_name(active);
    NSString *name = [self promptNameWithTitle:@(TOTK_UI("重命名键位配置", "Rename keymap profile"))
                                      initial:raw && *raw ? @(raw) : @""];
    if (!name) return;
    int rc = totk_profile_rename(active, name.UTF8String);
    [self reportResult:rc what:@(TOTK_UI("键位配置名称", "Profile name"))];
    [self refreshControls];
}

- (void)deleteProfile:(id)sender {
    (void)sender;
    int active = totk_profile_active();
    if (active < 0 || totk_profile_builtin(active) || totk_profile_count() <= 1) {
        [self showError:@(TOTK_UI("内置配置不能删除，并且至少保留一个配置。", "The built-in profile cannot be deleted, and at least one profile is kept."))];
        return;
    }
    NSAlert *alert = [NSAlert new];
    alert.messageText = @(TOTK_UI("删除键位配置？", "Delete keymap profile?"));
    const char *raw = totk_profile_name(active);
    alert.informativeText = [NSString stringWithFormat:@(TOTK_UI("“%@”将被删除，无法撤销。", "“%@” will be deleted; this cannot be undone.")),
                                                       raw && *raw ? @(raw) : @""];
    alert.alertStyle = NSAlertStyleWarning;
    [alert addButtonWithTitle:@(TOTK_UI("删除", "Delete"))];
    [alert addButtonWithTitle:@(TOTK_UI("取消", "Cancel"))];
    if ([alert runModal] != NSAlertFirstButtonReturn) return;
    int rc = totk_profile_delete(active);
    if (rc >= 0) totk_profile_apply();
    [self reportResult:rc what:@(TOTK_UI("键位配置", "Keymap profile"))];
    [self refreshControls];
}

- (void)resetProfile:(id)sender {
    (void)sender;
    int active = totk_profile_active();
    if (active < 0) return;
    NSAlert *alert = [NSAlert new];
    alert.messageText = @(TOTK_UI("重置键位配置？", "Reset keymap profile?"));
    alert.informativeText = @(TOTK_UI("这个配置中的所有按键和手柄映射将恢复为默认值。", "All keyboard and controller mappings in this profile return to their defaults."));
    alert.alertStyle = NSAlertStyleWarning;
    [alert addButtonWithTitle:@(TOTK_UI("重置", "Reset"))];
    [alert addButtonWithTitle:@(TOTK_UI("取消", "Cancel"))];
    if ([alert runModal] != NSAlertFirstButtonReturn) return;
    int rc = totk_profile_reset(active);
    if (rc >= 0) totk_profile_apply();
    [self reportResult:rc what:@(TOTK_UI("键位配置", "Keymap profile"))];
    [self refreshControls];
}

- (void)controllerChanged:(NSPopUpButton *)sender {
    NSInteger tag = sender.tag;
    int physical = (int)(tag % 100);
    if (physical < 0 || physical >= CONTROLLER_BUTTONS) return;
    int active = totk_profile_active();
    if (active < 0) return;

    int target = (int)_ctrlTarget[physical].indexOfSelectedItem - 1;
    int mode = (int)_ctrlMode[physical].indexOfSelectedItem;
    int fire = (int)_ctrlFire[physical].indexOfSelectedItem;
    if (target < 0) { target = -1; mode = CONTROLLER_HOLD; fire = 0; }
    if (mode != CONTROLLER_AIM) fire = 0;
    else if (fire == physical) {
        fire = 0;
        while (fire == physical && fire < CONTROLLER_BUTTONS - 1) fire++;
        if (fire == physical) {
            [self showError:@(TOTK_UI("开火键必须是另一个按钮。", "The fire button must be another button."))];
            [self updateControllerControls];
            return;
        }
    }
    int rc = totk_profile_set_controller(active, physical, target, mode, fire);
    if (rc >= 0) totk_profile_apply();
    [self reportResult:rc what:@(TOTK_UI("手柄映射", "Controller mapping"))];
    [self updateControllerControls];
}

/* ------------------------------------------------------------------------- */
/* Key capture panel. */

- (void)changeKey:(NSButton *)sender {
    int action = (int)sender.tag;
    int active = totk_profile_active();
    if (active < 0) return;
    int previous = keycode_for_action(active, action);

    NSPanel *panel = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, 460, 172)
                                                styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                                  backing:NSBackingStoreBuffered
                                                    defer:NO];
    panel.title = @(TOTK_UI("设置按键", "Set Key"));
    panel.releasedWhenClosed = NO;
    panel.delegate = self;
    panel.becomesKeyOnlyIfNeeded = NO;
    panel.level = NSModalPanelWindowLevel;
    NSView *cv = panel.contentView;

    [cv addSubview:make_label([NSString stringWithFormat:@(TOTK_UI("为“%@”按下一个键", "Press a key for “%@”")), action_title(action)],
                              NSMakeRect(20, 118, 420, 24), [NSFont systemFontOfSize:15 weight:NSFontWeightSemibold],
                              NSColor.labelColor)];
    [cv addSubview:make_label(@(TOTK_UI("Esc 取消 · Delete 清除绑定", "Escape cancels · Delete clears the binding")),
                              NSMakeRect(20, 92, 420, 18), [NSFont systemFontOfSize:11], NSColor.secondaryLabelColor)];
    NSTextField *error = make_multiline_label(@"", NSMakeRect(20, 46, 420, 40), [NSFont systemFontOfSize:11], NSColor.systemRedColor);
    [cv addSubview:error];
    panel.accessibilityLabel = @(TOTK_UI("按键设置面板", "Key binding panel"));

    if (_prefsWindow) {
        NSRect pf = _prefsWindow.frame, pf2 = panel.frame;
        pf2.origin = NSMakePoint(NSMidX(pf) - pf2.size.width / 2, NSMidY(pf) - pf2.size.height / 2);
        [panel setFrame:pf2 display:NO];
    }

    __block BOOL finished = NO;
    __block id monitor = nil;
    s_capture_panel = panel;
    void (^finish)(void) = ^{
        if (finished) return;
        finished = YES;
        if (monitor) [NSEvent removeMonitor:monitor];
        monitor = nil;
        if (s_capture_panel == panel) s_capture_panel = nil;
        if (NSApp.modalWindow == panel) [NSApp stopModal];
        [panel orderOut:nil];
        if (self->_prefsWindow && !self->_closing) [self->_prefsWindow makeKeyAndOrderFront:nil];
    };

    monitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown handler:^NSEvent *(NSEvent *event) {
        if (event.window != panel) return event;
        if (event.isARepeat) return nil;
        unsigned code = (unsigned)event.keyCode;
        NSEventModifierFlags flags = event.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask;
        if ((flags & (NSEventModifierFlagCommand | NSEventModifierFlagControl)) || code == 57) {
            error.stringValue = @(TOTK_UI("请不要使用 Command、Control 或 Caps Lock。", "Do not use Command, Control or Caps Lock."));
            NSBeep();
            return nil;
        }
        if (code == 53) {   /* Escape */
            finish();
            return nil;
        }
        if (code == 51 || code == 117) {   /* Delete or forward delete clears */
            if (previous < 0) {
                error.stringValue = @(TOTK_UI("这个操作还没有绑定按键。", "This action has no key to clear."));
                NSBeep();
                return nil;
            }
            int rc = totk_profile_set_action(active, previous, TOTK_ACTION_NONE);
            if (rc >= 0) {
                totk_profile_apply();
                [self updateKeyLabels];
                [self reportResult:rc what:@(TOTK_UI("按键绑定", "Key binding"))];
                finish();
            } else {
                error.stringValue = @(TOTK_UI("清除绑定失败。", "Could not clear the binding."));
                NSBeep();
            }
            return nil;
        }
        if (code == 18 || code == 19 || code == 20 || code == 21) {
            error.stringValue = @(TOTK_UI("1、2、3、4 保留给游戏内的快捷操作。", "1, 2, 3 and 4 are reserved for the in-game shortcuts."));
            NSBeep();
            return nil;
        }
        if (code >= TOTK_KEYCODE_COUNT) {
            error.stringValue = @(TOTK_UI("这个键不受支持。", "That key is not supported."));
            NSBeep();
            return nil;
        }
        int existing = totk_profile_action(active, (int)code);
        if (existing != TOTK_ACTION_NONE && existing != action) {
            error.stringValue = [NSString stringWithFormat:@(TOTK_UI("这个键已经用于“%@”。", "That key is already used by “%@”.")),
                                                           action_title(existing)];
            NSBeep();
            return nil;
        }
        int rc = totk_profile_set_action(active, (int)code, action);
        if (rc < 0) {
            error.stringValue = @(TOTK_UI("保存按键失败。", "Could not save the key binding."));
            NSBeep();
            return nil;
        }
        if (previous >= 0 && previous != (int)code) totk_profile_set_action(active, previous, TOTK_ACTION_NONE);
        totk_profile_apply();
        [self updateKeyLabels];
        [self reportResult:rc what:@(TOTK_UI("按键绑定", "Key binding"))];
        finish();
        return nil;
    }];

    runtime_input_clear();
    [panel makeKeyAndOrderFront:nil];
    [NSApp runModalForWindow:panel];
    finish();
}

- (void)gameLanguageChanged:(NSPopUpButton *)sender {
    NSInteger index = sender.indexOfSelectedItem;
    if (index < 0 || (size_t)index >= game_language_count) return;
    int rc = totk_setting_set_game_language(game_languages[index].code);
    if ([self reportResult:rc what:@(TOTK_UI("游戏语言", "Game language"))])
        _status.stringValue = @(TOTK_UI("游戏语言已保存，重新启动后生效。", "Game language saved; it takes effect after a restart."));
    [self refreshControls];
}

- (void)uiLanguageChanged:(NSPopUpButton *)sender {
    NSInteger index = sender.indexOfSelectedItem;
    if (index < 0 || (size_t)index >= ui_language_count) return;
    int rc = totk_setting_set_ui_language(ui_languages[index].code);
    if ([self reportResult:rc what:@(TOTK_UI("界面语言", "Interface language"))])
        _status.stringValue = @(TOTK_UI("界面语言已保存，重新启动后生效。", "Interface language saved; it takes effect after a restart."));
    [self refreshControls];
}

/* ------------------------------------------------------------------------- */
/* Window management. */

- (void)showPreferences {
    [self refreshControls];
    if (_prefsWindow.miniaturized) [_prefsWindow deminiaturize:nil];
    [NSApp activateIgnoringOtherApps:YES];
    [_prefsWindow makeKeyAndOrderFront:nil];
}

- (void)selectTab:(NSInteger)index {
    if (_tabs && index >= 0 && index < (NSInteger)_tabs.numberOfTabViewItems)
        [_tabs selectTabViewItemAtIndex:index];
    [self showPreferences];
}

- (void)windowWillClose:(NSNotification *)notification {
    NSWindow *window = notification.object;
    if (window != _prefsWindow) {
        if (NSApp.modalWindow == window) [NSApp stopModal];
        return;
    }
    if (_closing) return;
    int rc = totk_settings_save_now();
    if (rc != TOTK_SET_OK) fprintf(stderr, "[totk] settings: save on close failed (%d)\n", rc);
    totk_preferences_flush();
    __weak TOTKPreferencesController *weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{
        TOTKPreferencesController *strong = weakSelf;
        if (!strong || strong->_closing || !strong->_gameWindow) return;
        if (strong->_gameWindow.miniaturized) [strong->_gameWindow deminiaturize:nil];
        [strong->_gameWindow makeKeyAndOrderFront:nil];
        [strong->_gameWindow makeFirstResponder:strong->_gameWindow.contentView];
    });
}

@end

/* ------------------------------------------------------------------------- */
static TOTKPreferencesController *s_controller = nil;

void totk_preferences_install(NSWindow *gameWindow) {
    if (![NSThread isMainThread]) {
        dispatch_async(dispatch_get_main_queue(), ^{ totk_preferences_install(gameWindow); });
        return;
    }
    if (s_controller) {
        [s_controller setGameWindow:gameWindow];
        [s_controller refresh];
        return;
    }
    s_controller = [[TOTKPreferencesController alloc] initWithGameWindow:gameWindow];
}

void totk_preferences_refresh(void) {
    if (!s_controller) return;
    if (![NSThread isMainThread]) {
        dispatch_async(dispatch_get_main_queue(), ^{ [s_controller refresh]; });
        return;
    }
    [s_controller refresh];
}

void totk_preferences_flush(void) {
    totk_settings_flush();
}

void totk_preferences_show(void) {
    if (!s_controller) return;
    if (![NSThread isMainThread]) {
        dispatch_async(dispatch_get_main_queue(), ^{ [s_controller showPreferences]; });
        return;
    }
    [s_controller showPreferences];
}
