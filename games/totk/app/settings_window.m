/* The launcher's settings window: hold Option while the app opens (or launch it with --settings, or TOTK_SETTINGS=1); it also
   opens by itself after a start that failed on one of these settings or crashed within a minute (app/launcher.m). The
   game's Settings… (Command-comma) opens it in a save-only mode beside the running game (--settings-only).
   Each control edits one entry of the saved settings (app/launch_settings.h); choosing the game's default removes the entry,
   so a later change of a default in the runtime reaches the player. A setting the environment already gives (open --env)
   is shown but cannot be changed here: the environment wins at launch. Text is Chinese or English (TOTK_UI). */
#import "settings_window.h"
#include "game_version.h"
#include "include/totk_ui_lang.h"
#include "launch_settings.h"

#define UI(zh, en) @(TOTK_UI(zh, en))
static NSString *const kPicture = @"picture";   /* one control for TOTK_MFX_UPSCALE + TOTK_MFX_OUTPUT (key 3's MetalFX 2K) */

@interface SettingsWindow : NSObject <NSWindowDelegate, NSTextFieldDelegate>
@property NSWindow *window;
@property NSTabView *tabs;
@property NSMutableDictionary<NSString *, NSString *> *values;
@property NSSet<NSString *> *env;
@property NSMutableDictionary<NSString *, NSControl *> *controls;
@property NSMutableDictionary<NSString *, NSTextField *> *helps;
@property NSMutableDictionary<NSString *, NSString *> *helpTexts;
@property NSMutableDictionary<NSString *, NSNumber *> *tabOf;
@property NSMutableSet<NSString *> *invalid;
@property NSTextField *aspectText, *gainLabel, *note;
@property NSButton *save, *once, *always;
@property NSString *highlight;
@property BOOL editOnly;
@property LaunchChoice choice;
@end

@implementation SettingsWindow

- (NSString *)def:(NSString *)name { const LaunchSetting *s = launch_setting(name.UTF8String); return s ? @(s->def) : @""; }
/* What the game gets for name at this launch, as far as the window knows. */
- (NSString *)effective:(NSString *)name {
    if ([self.env containsObject:name]) { const char *v = getenv(name.UTF8String); return v ? @(v) : @""; }
    return self.values[name] ?: [self def:name];
}
- (BOOL)locked:(NSString *)name {
    return [name isEqualToString:kPicture] ? [self.env containsObject:@"TOTK_MFX_UPSCALE"] || [self.env containsObject:@"TOTK_MFX_OUTPUT"]
                                           : [self.env containsObject:name];
}
- (void)set:(NSString *)name to:(NSString *)value {   /* nil or the default: removed */
    if (!value.length || [value isEqualToString:[self def:name]]) [self.values removeObjectForKey:name];
    else self.values[name] = value;
}

/* ---- building ---- */
- (NSTextField *)label:(NSString *)s {
    NSTextField *l = [NSTextField labelWithString:s]; l.alignment = NSTextAlignmentRight; return l;
}
- (NSTextField *)help:(NSString *)s {
    NSTextField *h = [NSTextField wrappingLabelWithString:s ?: @""];
    h.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize]; h.textColor = NSColor.secondaryLabelColor;
    h.preferredMaxLayoutWidth = 430; [h.widthAnchor constraintEqualToConstant:430].active = YES;
    return h;
}
- (void)row:(NSMutableArray *)rows tab:(NSInteger)tab name:(NSString *)name label:(NSString *)label control:(NSView *)control
       help:(NSString *)help tip:(NSString *)tip {
    NSTextField *l = [self label:label ?: @""];
    l.toolTip = tip; control.toolTip = tip;
    [rows addObject:@[l, control]];
    if (name) { self.tabOf[name] = @(tab); if ([control isKindOfClass:NSControl.class]) self.controls[name] = (NSControl *)control; }
    if (help || (name && [self locked:name])) {
        NSTextField *h = [self help:help]; h.toolTip = tip;
        [rows addObject:@[NSGridCell.emptyContentView, h]];
        if (name) { self.helps[name] = h; self.helpTexts[name] = help ?: @""; }
    }
}
- (NSButton *)check:(NSString *)name title:(NSString *)title {
    NSButton *b = [NSButton checkboxWithTitle:title target:self action:@selector(boolChanged:)];
    b.identifier = name; return b;
}
/* items: @[title, value] pairs; value @"" is the game's default. */
- (NSPopUpButton *)popup:(NSString *)name items:(NSArray<NSArray<NSString *> *> *)items {
    NSPopUpButton *p = [NSPopUpButton new]; p.identifier = name; p.target = self; p.action = @selector(popupChanged:);
    for (NSArray<NSString *> *item in items) { [p addItemWithTitle:item[0]]; p.lastItem.representedObject = item[1]; }
    return p;
}
- (NSView *)grid:(NSArray *)rows {
    NSGridView *g = [NSGridView gridViewWithViews:rows];
    g.rowSpacing = 6; g.columnSpacing = 12; [g columnAtIndex:0].xPlacement = NSGridCellPlacementTrailing;
    [g columnAtIndex:0].width = 150;
    for (NSInteger i = 0; i < g.numberOfRows; i++) {   /* a little more space above each setting than above its help line */
        NSGridRow *r = [g rowAtIndex:i];
        if ([r cellAtIndex:0].contentView != NSGridCell.emptyContentView && i) r.topPadding = 8;
        r.yPlacement = NSGridCellPlacementCenter;
    }
    NSView *box = [NSView new]; g.translatesAutoresizingMaskIntoConstraints = NO; [box addSubview:g];
    [NSLayoutConstraint activateConstraints:@[[g.topAnchor constraintEqualToAnchor:box.topAnchor constant:14],
                                              [g.leadingAnchor constraintEqualToAnchor:box.leadingAnchor constant:12],
                                              [g.trailingAnchor constraintLessThanOrEqualToAnchor:box.trailingAnchor constant:-12],
                                              [g.bottomAnchor constraintLessThanOrEqualToAnchor:box.bottomAnchor constant:-12]]];
    return box;
}
- (void)tab:(NSString *)title view:(NSView *)view {
    NSTabViewItem *item = [NSTabViewItem new]; item.view = view; item.label = title; [self.tabs addTabViewItem:item];
}

- (void)build:(NSString *)problem {
    self.controls = [NSMutableDictionary dictionary]; self.helps = [NSMutableDictionary dictionary];
    self.helpTexts = [NSMutableDictionary dictionary]; self.tabOf = [NSMutableDictionary dictionary]; self.invalid = [NSMutableSet set];
    self.tabs = [NSTabView new];

    NSMutableArray *rows = [NSMutableArray array];   /* ---- performance ---- */
    [self row:rows tab:0 name:@"TOTK_UNCAPPED" label:UI("不限帧率", "Uncapped")
      control:[self check:@"TOTK_UNCAPPED" title:UI("不等显示器刷新", "Don't wait for the display")]
         help:UI("游戏按自己的时钟运行，显示器每次刷新显示最新的一帧。", "The game runs on its own clock; each screen refresh shows the newest frame.")
          tip:UI("类似其他游戏的关闭垂直同步。关闭后游戏等待显示器，帧率按 60 / 30 / 20 的档位变化。", "Like vsync off in other games. Off: the game waits for the display and its frame rate moves in 60 / 30 / 20 steps.")];
    [self row:rows tab:0 name:@"TOTK_UNCAPPED_HZ" label:UI("帧率上限", "Frame rate cap")
      control:[self popup:@"TOTK_UNCAPPED_HZ" items:@[@[UI("120（默认）", "120 (default)"), @""], @[UI("显示器刷新率", "The display's refresh rate"), @"display"],
                                                       @[@"90", @"90"], @[@"60", @"60"], @[@"45", @"45"], @[@"40", @"40"], @[@"30", @"30"]]]
         help:UI("60 Hz 屏幕上选 60 或“显示器刷新率”：不画看不到的帧，更凉更省电。", "On a 60 Hz screen, 60 or the display's rate skips frames nobody sees: cooler and quieter.")
          tip:UI("不限帧率时游戏时钟的上限（30–120）。高于屏幕刷新率的帧不会显示，只会多耗电。", "The ceiling of the uncapped clock (30-120). Frames above the screen's refresh rate are never shown; they only cost power.")];
    [self row:rows tab:0 name:@"TOTK_UNCAPPED_INTERVAL" label:UI("稳定帧节奏", "Steady pacing")
      control:[self check:@"TOTK_UNCAPPED_INTERVAL" title:UI("忽略游戏自己的出图间隔", "Ignore the game's own frame interval")]
         help:LAUNCH_BOTW ? UI("本游戏默认开启。", "On by default in this game.")
                          : UI("重场景保持上限，不掉到 30 或 20 帧。本游戏中仍属实验性。", "Holds the cap in heavy scenes instead of dropping to 30 or 20 fps. Still experimental in this game.")
          tip:UI("游戏会把出图间隔设为 1、2 或 3，不限帧率时按时钟节拍计算：上限 30 时重场景只有 15 帧。开启后每个节拍出一帧。", "The game sets a present interval of 1, 2 or 3, which counts ticks of the uncapped clock: with a 30 cap a heavy scene gets 15 fps. On: one frame per tick.")];
    [self row:rows tab:0 name:@"TOTK_GPU_KEEPALIVE" label:UI("GPU 保活", "GPU keep-alive")
      control:[self check:@"TOTK_GPU_KEEPALIVE" title:UI("保持 GPU 高频", "Keep the GPU clock up")]
         help:UI("在 GPU 空闲时填入短任务，避免降频。无风扇的 Mac 上关闭会更凉。", "Fills idle GPU time so its clock stays high. Off runs cooler on a fanless Mac.")
          tip:UI("开启时 GPU 帧时间更短，但多用一个 GPU 核心和电量；在 MacBook Air 上会更热。", "On: shorter GPU frame times, at the cost of one GPU core and power; a MacBook Air runs hotter.")];
    [self row:rows tab:0 name:@"TOTK_MFX" label:UI("补帧", "Frame interpolation")
      control:[self popup:@"TOTK_MFX" items:@[@[UI("关（默认）", "Off (default)"), @""], @[UI("30+30（游戏 30 帧）", "30+30 (the game at 30 fps)"), @"interp30"],
                                               @[UI("刷新率的一半", "Half the refresh rate"), @"interp"], @[UI("每一帧（不限帧）", "Every frame (uncapped)"), @"interpmax"]]]
         help:UI("启动时的 MetalFX 补帧；游戏中按 2 键切换。", "MetalFX frame interpolation at start; key 2 changes it while playing.")
          tip:UI("30+30 和刷新率一半由补帧控制节奏，期间暂停不限帧率。", "30+30 and half the refresh rate pace the game themselves and pause the uncapped mode.")];
#if !LAUNCH_BOTW
    [self row:rows tab:0 name:@"TOTK_FPS60" label:UI("60 帧补丁", "60 fps patch")
      control:[self check:@"TOTK_FPS60" title:UI("允许游戏跑到 60 帧", "Let the game run at 60 fps")]
         help:UI("关闭 = 主机原版的 30 帧节奏（也会关闭不限帧率）。", "Off: the console's own 30 fps pacing (also turns uncapped off).")
          tip:UI("游戏的帧率控制器补丁，最低出图间隔改为 1。", "A patch of the game's frame controller that allows a present interval of 1.")];
#endif
    [self tab:UI("性能", "Performance") view:[self grid:rows]];

    rows = [NSMutableArray array];   /* ---- controller ---- */
    [self row:rows tab:1 name:@"TOTK_RUMBLE" label:UI("震动", "Rumble") control:[self check:@"TOTK_RUMBLE" title:UI("开启", "On")]
         help:UI("游戏的 HD 震动交给手柄的触觉马达。", "The game's HD rumble plays on the controller's haptics.")
          tip:UI("需要支持触觉反馈的手柄（DualSense、Xbox 等）。游戏中可用 flag rumble 0 静音。", "Needs a controller with haptics (DualSense, Xbox and others). \"flag rumble 0\" mutes it while playing.")];
    NSSlider *gain = [NSSlider sliderWithValue:1 minValue:0.25 maxValue:4 target:self action:@selector(gainChanged:)];
    gain.identifier = @"TOTK_RUMBLE_GAIN"; gain.numberOfTickMarks = 16; gain.allowsTickMarkValuesOnly = YES;
    [gain.widthAnchor constraintEqualToConstant:220].active = YES;
    self.gainLabel = [NSTextField labelWithString:@"1.00×"]; self.gainLabel.font = [NSFont monospacedDigitSystemFontOfSize:NSFont.systemFontSize weight:NSFontWeightRegular];
    NSStackView *gainRow = [NSStackView stackViewWithViews:@[gain, self.gainLabel]];
    [self row:rows tab:1 name:@"TOTK_RUMBLE_GAIN" label:UI("震动强度", "Rumble strength") control:gainRow
         help:UI("1.0 是游戏原本的强度；马达较弱的手柄可以调高（最多 4）。", "1.0 is the game's own strength; raise it for weak motors (up to 4).")
          tip:UI("所有震动值乘以这个倍数。", "Every rumble value is multiplied by this.")];
    self.controls[@"TOTK_RUMBLE_GAIN"] = gain;
    [self row:rows tab:1 name:@"TOTK_GYRO" label:UI("体感", "Gyro") control:[self check:@"TOTK_GYRO" title:UI("体感瞄准", "Motion aiming")]
         help:UI("弓箭瞄准、究极手等体感操作跟随手柄。", "Bow aiming, Ultrahand and other motion controls follow the controller.")
          tip:UI("需要带陀螺仪的手柄。游戏中可用 flag gyro 0 暂停。", "Needs a controller with a gyro. \"flag gyro 0\" pauses it while playing.")];
    NSTextField *axes = [NSTextField textFieldWithString:@""]; axes.identifier = @"TOTK_GYRO_AXES"; axes.delegate = self;
    axes.placeholderString = UI("x,y,z（默认）", "x,y,z (default)"); [axes.widthAnchor constraintEqualToConstant:160].active = YES;
    [self row:rows tab:1 name:@"TOTK_GYRO_AXES" label:UI("体感轴向", "Gyro axes") control:axes
         help:UI("仅在体感方向不对时修改，例如 -x,y,-z 反转两个轴。", "Change only if motion goes the wrong way, e.g. -x,y,-z flips two axes.")
          tip:UI("x、y、z 各写一次，可加 + 或 -，用逗号分隔。", "x, y and z once each, each with an optional + or -, separated by commas.")];
    [self row:rows tab:1 name:@"TOTK_FACE_BUTTONS" label:UI("面键", "Face buttons")
      control:[self popup:@"TOTK_FACE_BUTTONS" items:@[@[UI("自动（默认）", "Automatic (default)"), @""], @[UI("按位置", "By position"), @"position"], @[UI("按标签", "By label"), @"label"]]]
         help:UI("自动：任天堂手柄按标签，其他手柄按位置（和游戏的按键提示一致）。", "Automatic: Nintendo controllers by label, others by position (as the game's prompts show them).")
          tip:UI("按位置：右边的键是 A、下边 B、上边 X、左边 Y。按标签：印着 A 的键就是 A。", "By position: right is A, bottom B, top X, left Y. By label: the button printed A is A.")];
    [self tab:UI("手柄", "Controller") view:[self grid:rows]];

    rows = [NSMutableArray array];   /* ---- display and overlays ---- */
    [self row:rows tab:2 name:kPicture label:UI("启动时的画质", "Picture at start")
      control:[self popup:kPicture items:@[@[UI("1080p（默认）", "1080p (default)"), @""], @[@"MetalFX 2K", @"1"]]]
         help:UI("游戏中按 3 键切换。", "Key 3 changes it while playing.")
          tip:UI("MetalFX 2K：MetalFX 把游戏画面放大到窗口或全屏的尺寸。", "MetalFX 2K: MetalFX scales the game's picture up to the window or the full screen.")];
#if LAUNCH_ASPECT
    NSPopUpButton *aspect = [self popup:@"TOTK_ASPECT" items:@[@[UI("16:9（默认）", "16:9 (default)"), @""], @[@"21:9", @"21:9"], @[@"32:9", @"32:9"],
                                                               @[UI("自定义…", "Custom…"), @"custom"]]];
    self.aspectText = [NSTextField textFieldWithString:@""]; self.aspectText.identifier = @"TOTK_ASPECT"; self.aspectText.delegate = self;
    self.aspectText.placeholderString = @"3440x1440"; [self.aspectText.widthAnchor constraintEqualToConstant:110].active = YES;
    NSStackView *aspectRow = [NSStackView stackViewWithViews:@[aspect, self.aspectText]];
    [self row:rows tab:2 name:@"TOTK_ASPECT" label:UI("宽高比", "Aspect ratio") control:aspectRow
         help:UI("以宽屏启动；4 键在 16:9 和这个比例之间切换。自定义：w:h、wxh 或比值。", "Starts wide; key 4 switches between 16:9 and this ratio. Custom: w:h, wxh or a ratio.")
          tip:UI("镜头两侧看到更多，画面铺满更宽的窗口；HUD 留在中间的 16:9 区域。", "The camera sees more at the sides and the picture fills a wider window; the HUD stays in the middle 16:9.")];
    self.controls[@"TOTK_ASPECT"] = aspect;
#endif
    [self row:rows tab:2 name:@"TOTK_HUD" label:UI("提示布局", "Overlay layout")
      control:[self popup:@"TOTK_HUD" items:@[@[UI("右上角一个小框（默认）", "One box, top right (default)"), @""], @[UI("画面四角", "In the corners"), @"corners"]]]
         help:nil tip:UI("FPS、着色器、负载读数和快捷键说明的位置。", "Where the FPS, shader and load readings and the shortcut reminder go.")];
    [self row:rows tab:2 name:@"TOTK_FPS_OVERLAY" label:UI("屏幕提示", "Overlays")
      control:[self check:@"TOTK_FPS_OVERLAY" title:UI("启动时显示", "Show at start")]
         help:nil tip:UI("游戏中按 1 键显示或隐藏全部提示。", "Key 1 shows or hides all overlays while playing.")];
    [self row:rows tab:2 name:@"TOTK_SHADER_OVERLAY" label:nil control:[self check:@"TOTK_SHADER_OVERLAY" title:UI("着色器计数", "Shader counter")]
         help:nil tip:UI("正在编译和已缓存的着色器数量。", "Shaders being compiled and already cached.")];
    [self row:rows tab:2 name:@"TOTK_LOAD_OVERLAY" label:nil control:[self check:@"TOTK_LOAD_OVERLAY" title:UI("系统负载", "System load")]
         help:UI("游戏中按 1 键显示或隐藏全部提示。", "Key 1 shows or hides all overlays while playing.")
          tip:UI("CPU、GPU 占用和温度读数。", "CPU and GPU load and temperature readings.")];
    [self tab:UI("画面", "Display") view:[self grid:rows]];

    rows = [NSMutableArray array];   /* ---- language ---- */
    NSMutableArray *languages = [NSMutableArray arrayWithObject:@[UI("跟随系统（默认）", "Follow the Mac (default)"), @""]];
    NSDictionary *names = @{@"en-US": @"English", @"ja": @"日本語", @"de": @"Deutsch", @"fr": @"Français", @"es": @"Español", @"it": @"Italiano",
                            @"nl": @"Nederlands", @"ru": @"Русский", @"ko": @"한국어", @"zh-Hans": @"简体中文", @"pt": @"Português", @"pt-BR": @"Português (Brasil)"};
    for (int i = 0; launch_languages[i]; i++) [languages addObject:@[names[@(launch_languages[i])] ?: @(launch_languages[i]), @(launch_languages[i])]];
    [self row:rows tab:3 name:@"TOTK_LANG" label:UI("游戏语言", "Game language") control:[self popup:@"TOTK_LANG" items:languages]
         help:UI("游戏的文字和语音。“跟随系统”也会跟随本 App 单独设置的语言。", "The game's text and voices. Follow the Mac also follows a language set for this app alone.")
          tip:UI("单独设置：defaults write org.macnativegames.totk AppleLanguages -array pt-BR", "A language for this app alone: defaults write org.macnativegames.totk AppleLanguages -array pt-BR")];
    [self row:rows tab:3 name:@"TOTK_UI_LANG" label:UI("启动器文字", "Launcher text")
      control:[self popup:@"TOTK_UI_LANG" items:@[@[UI("跟随系统（默认）", "Follow the Mac (default)"), @""], @[@"中文", @"zh"], @[@"English", @"en"]]]
         help:UI("这个窗口、启动画面、屏幕提示和键盘输入提示。", "This window, the start screen, the overlays and the keyboard prompt.")
          tip:UI("中文系统上默认中文，其他系统默认英文。这个窗口下次打开时换语言。", "Chinese on a Chinese Mac, English otherwise. This window changes language the next time it opens.")];
    [self tab:UI("语言", "Language") view:[self grid:rows]];

    CGFloat h = 0;
    for (NSTabViewItem *item in self.tabs.tabViewItems) h = MAX(h, item.view.fittingSize.height);
    [self.tabs.widthAnchor constraintEqualToConstant:660].active = YES;
    [self.tabs.heightAnchor constraintEqualToConstant:h + 40].active = YES;

    self.note = [NSTextField wrappingLabelWithString:problem ?: @""]; self.note.textColor = NSColor.systemRedColor;
    self.note.preferredMaxLayoutWidth = 660; [self.note.widthAnchor constraintEqualToConstant:660].active = YES; self.note.hidden = !problem;
    NSTextField *hint = [self help:self.editOnly ? UI("游戏正在运行：保存后，退出游戏（⌘Q）再重新打开，新设置才会生效。", "The game is running: after you save, quit it (⌘Q) and open it again for the new settings to apply.")
                                                 : UI("打开游戏时按住 Option（⌥）键可再次显示这个窗口；游戏中按 ⌘, 也可以。", "Hold Option (⌥) while opening the game, or press ⌘, while playing, to show this window again.")];
    self.always = [NSButton checkboxWithTitle:UI("每次启动都显示这个窗口", "Show this window at every start") target:nil action:nil];
    NSButton *reset = [NSButton buttonWithTitle:UI("恢复默认", "Reset to defaults") target:self action:@selector(reset:)];
    reset.toolTip = UI("所有设置回到游戏的默认值（点“保存并启动”后才保存）。", "Every setting back to the game's default (saved only with Save and Launch).");
    NSButton *quit = [NSButton buttonWithTitle:self.editOnly ? UI("取消", "Cancel") : UI("退出", "Quit") target:self action:@selector(quit:)];
    quit.keyEquivalent = @"\033";
    self.once = [NSButton buttonWithTitle:UI("仅本次启动", "Launch once") target:self action:@selector(launchOnce:)];
    self.once.toolTip = UI("用这些设置启动，但不保存。", "Start with these settings without saving them.");
    self.save = [NSButton buttonWithTitle:self.editOnly ? UI("保存", "Save") : UI("保存并启动", "Save and Launch") target:self action:@selector(saveAndLaunch:)];
    if (self.editOnly) self.save.toolTip = UI("保存后，退出游戏（⌘Q）再重新打开即可生效。", "Saved settings apply after you quit the game (⌘Q) and open it again.");
    self.save.keyEquivalent = @"\r";
    NSStackView *buttons = [NSStackView new];
    [buttons addView:reset inGravity:NSStackViewGravityLeading];
    for (NSButton *b in self.editOnly ? @[quit, self.save] : @[quit, self.once, self.save]) [buttons addView:b inGravity:NSStackViewGravityTrailing];
    [buttons.widthAnchor constraintEqualToConstant:660].active = YES;
    NSStackView *stack = [NSStackView stackViewWithViews:@[self.note, self.tabs, self.always, hint, buttons]];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical; stack.alignment = NSLayoutAttributeLeading; stack.spacing = 12;
    stack.edgeInsets = NSEdgeInsetsMake(18, 20, 18, 20);
    [stack setCustomSpacing:4 afterView:self.always];

    self.window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 700, 560) styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                                backing:NSBackingStoreBuffered defer:NO];
    self.window.title = [NSString stringWithFormat:@"%@ — %@", @GAME_NAME, UI("启动设置", "Launch settings")];
    self.window.contentView = stack; self.window.delegate = self; self.window.releasedWhenClosed = NO;
}

/* ---- the values in the controls ---- */
- (void)load {
    self.aspectText.stringValue = @"";
    for (NSString *name in self.controls) {
        NSControl *c = self.controls[name];
        NSString *v = [name isEqualToString:kPicture] ? ([[self effective:@"TOTK_MFX_UPSCALE"] isEqualToString:@"1"] &&
                                                         [[self effective:@"TOTK_MFX_OUTPUT"] isEqualToString:@"1"] ? @"1" : @"")
                                                      : [self effective:name];
        if ([c isKindOfClass:NSPopUpButton.class]) {
            NSPopUpButton *p = (NSPopUpButton *)c;
            NSString *want = [v isEqualToString:[self def:name]] ? @"" : v;
            NSInteger i = [p indexOfItemWithRepresentedObject:want];
            if (i < 0 && [name isEqualToString:@"TOTK_ASPECT"]) { i = [p indexOfItemWithRepresentedObject:@"custom"]; self.aspectText.stringValue = v; }
            else if (i < 0) { [p addItemWithTitle:v]; p.lastItem.representedObject = v; i = p.numberOfItems - 1; }
            [p selectItemAtIndex:i];
        } else if ([c isKindOfClass:NSSlider.class]) {
            c.doubleValue = v.doubleValue > 0 ? v.doubleValue : 1;
        } else if ([c isKindOfClass:NSButton.class]) {
            ((NSButton *)c).state = ![v isEqualToString:@"0"];
        } else if ([c isKindOfClass:NSTextField.class]) {
            c.stringValue = [v isEqualToString:[self def:name]] ? @"" : v;
        }
        c.enabled = ![self locked:name];
    }
    [self.invalid removeAllObjects];
    /* a saved text the game would refuse shows in red (and is not passed on: launch_settings_resolve) */
    NSTextField *axes = (NSTextField *)self.controls[@"TOTK_GYRO_AXES"];
    if (axes.stringValue.length && ![self locked:@"TOTK_GYRO_AXES"]) [self textChanged:axes];
    if (self.aspectText.stringValue.length && ![self locked:@"TOTK_ASPECT"]) [self textChanged:self.aspectText];
    [self refresh];
}
- (void)refresh {
    BOOL paced = [[self effective:@"TOTK_UNCAPPED"] isEqualToString:@"0"] || (launch_setting("TOTK_FPS60") && [[self effective:@"TOTK_FPS60"] isEqualToString:@"0"]);
    NSDictionary<NSString *, NSNumber *> *needs = @{@"TOTK_UNCAPPED_HZ": @(!paced), @"TOTK_UNCAPPED_INTERVAL": @(!paced),
                                                   @"TOTK_RUMBLE_GAIN": @(![[self effective:@"TOTK_RUMBLE"] isEqualToString:@"0"]),
                                                   @"TOTK_GYRO_AXES": @(![[self effective:@"TOTK_GYRO"] isEqualToString:@"0"])};
    for (NSString *name in needs) self.controls[name].enabled = needs[name].boolValue && ![self locked:name];
    if (self.aspectText) {
        NSPopUpButton *p = (NSPopUpButton *)self.controls[@"TOTK_ASPECT"];
        self.aspectText.hidden = ![p.selectedItem.representedObject isEqual:@"custom"];
        self.aspectText.enabled = ![self locked:@"TOTK_ASPECT"];
    }
    self.gainLabel.stringValue = [NSString stringWithFormat:@"%.2f×", self.controls[@"TOTK_RUMBLE_GAIN"].doubleValue];
    for (NSString *name in self.helps) {
        NSTextField *h = self.helps[name]; NSString *text = self.helpTexts[name]; NSColor *color = NSColor.secondaryLabelColor;
        if ([self locked:name]) {
            NSString *shown = [name isEqualToString:kPicture] ? @"TOTK_MFX_UPSCALE / TOTK_MFX_OUTPUT" : [NSString stringWithFormat:@"%@=%@", name, [self effective:name]];
            text = [NSString stringWithFormat:UI("由环境变量设置（open --env 或终端）：%@；不使用保存的值。", "Set by the environment (open --env or a shell): %@; the saved value is not used."), shown];
        } else if ([self.invalid containsObject:name]) {
            const LaunchSetting *s = launch_setting(name.UTF8String);
            NSControl *field = [name isEqualToString:@"TOTK_ASPECT"] ? self.aspectText : self.controls[name];
            const char *why = s ? s->check(field.stringValue.UTF8String) : NULL;
            text = [NSString stringWithFormat:@"%@ %s", name, why ?: ""]; color = NSColor.systemRedColor;
        } else if (self.highlight && [name isEqualToString:self.highlight]) color = NSColor.systemRedColor;
        h.stringValue = text; h.textColor = color;
    }
    self.save.enabled = self.once.enabled = self.invalid.count == 0;
}

/* ---- actions ---- */
- (void)boolChanged:(NSButton *)b { [self set:b.identifier to:b.state ? @"1" : @"0"]; [self refresh]; }
- (void)popupChanged:(NSPopUpButton *)p {
    NSString *v = p.selectedItem.representedObject;
    if ([p.identifier isEqualToString:kPicture]) { [self set:@"TOTK_MFX_UPSCALE" to:v]; [self set:@"TOTK_MFX_OUTPUT" to:v]; }
    else if ([v isEqualToString:@"custom"]) { [self.invalid removeObject:@"TOTK_ASPECT"]; [self textChanged:self.aspectText]; return; }
    else { [self.invalid removeObject:p.identifier]; [self set:p.identifier to:v]; }
    [self refresh];
}
- (void)gainChanged:(NSSlider *)s {
    double v = round(s.doubleValue * 4) / 4;
    [self set:@"TOTK_RUMBLE_GAIN" to:fabs(v - 1) < 0.001 ? nil : [NSString stringWithFormat:@"%g", v]];
    [self refresh];
}
- (void)textChanged:(NSTextField *)f {
    NSString *name = f.identifier, *v = [f.stringValue stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
    if (!v.length) { [self.invalid removeObject:name]; [self set:name to:nil]; }
    else if (launch_setting_check(name.UTF8String, v.UTF8String)) [self.invalid addObject:name];
    else { [self.invalid removeObject:name]; [self set:name to:v]; }
    [self refresh];
}
- (void)controlTextDidChange:(NSNotification *)n { [self textChanged:n.object]; }
- (void)reset:(id)sender { (void)sender; [self.values removeAllObjects]; self.highlight = nil; [self load]; }
- (void)finish:(LaunchChoice)choice { self.choice = choice; [NSApp stopModalWithCode:choice]; [self.window orderOut:nil]; }
- (void)quit:(id)sender { (void)sender; [self finish:LaunchQuit]; }
- (void)launchOnce:(id)sender { (void)sender; [self.window makeFirstResponder:nil]; [self finish:LaunchOnce]; }
- (void)saveAndLaunch:(id)sender { (void)sender; [self.window makeFirstResponder:nil]; [self finish:LaunchSave]; }
- (BOOL)windowShouldClose:(NSWindow *)w { (void)w; [self finish:LaunchQuit]; return NO; }
@end

/* Quit and the Edit menu (copy and paste in the text fields) while the window is up. */
static void install_menu(void) {
    NSMenu *bar = [NSMenu new], *app = [NSMenu new], *edit = [[NSMenu alloc] initWithTitle:UI("编辑", "Edit")];
    [app addItemWithTitle:UI("退出", "Quit") action:@selector(terminate:) keyEquivalent:@"q"];
    [edit addItemWithTitle:UI("撤销", "Undo") action:@selector(undo:) keyEquivalent:@"z"];
    [edit addItem:NSMenuItem.separatorItem];
    [edit addItemWithTitle:UI("剪切", "Cut") action:@selector(cut:) keyEquivalent:@"x"];
    [edit addItemWithTitle:UI("拷贝", "Copy") action:@selector(copy:) keyEquivalent:@"c"];
    [edit addItemWithTitle:UI("粘贴", "Paste") action:@selector(paste:) keyEquivalent:@"v"];
    [edit addItemWithTitle:UI("全选", "Select All") action:@selector(selectAll:) keyEquivalent:@"a"];
    [bar addItemWithTitle:@"" action:nil keyEquivalent:@""].submenu = app;
    [bar addItemWithTitle:edit.title action:nil keyEquivalent:@""].submenu = edit;
    NSApp.mainMenu = bar;
}

LaunchChoice launch_settings_window(NSMutableDictionary<NSString *, NSString *> *values, NSSet<NSString *> *fromEnv,
                                    NSString *problem, NSString *highlight, BOOL *showAlways, BOOL editOnly) {
    [NSApplication sharedApplication];
    NSApp.activationPolicy = NSApplicationActivationPolicyRegular;
    install_menu();
    [NSApp finishLaunching];
    SettingsWindow *s = [SettingsWindow new];
    s.values = values; s.env = fromEnv; s.highlight = highlight; s.editOnly = editOnly;
    [s build:problem];
    s.always.state = *showAlways;
    [s load];
    NSNumber *tab = highlight ? s.tabOf[highlight] : nil;
    if (tab) [s.tabs selectTabViewItemAtIndex:tab.integerValue];
    [s.window center]; [s.window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    [NSApp runModalForWindow:s.window];
    *showAlways = s.always.state == NSControlStateValueOn;
    return s.choice;
}
