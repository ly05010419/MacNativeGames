// Jamboree's own UI: independent of the other games' front ends.
#import "front_settings.h"
#include <stdlib.h>
#include <string.h>

NSString *dm_settings_text(NSString *chinese, NSString *english) {
    const char *language = getenv("DM_LANG");
    return language && !strncmp(language, "zh", 2) ? chinese : english;
}
#define UI(C, E) dm_settings_text(@C, @E)
static NSArray<NSString *> *output_names(void) {
    return @[UI("自动（随窗口）", "Automatic (window size)"), @"720p · 1280 × 720", @"1080p · 1920 × 1080",
             @"1440p · 2560 × 1440", @"2160p · 3840 × 2160"];
}
static NSString *face_name(int layout) {
    return layout == 2 ? UI("Nintendo · 按标签", "Nintendo labels") : layout == 1 ? UI("Xbox / PlayStation · 按位置", "Xbox / PlayStation positions") : UI("自动识别", "Automatic");
}
static NSTextField *label(NSString *text, CGFloat size, NSColor *color) {
    NSTextField *v = [NSTextField wrappingLabelWithString:text];
    v.font = [NSFont systemFontOfSize:size]; v.textColor = color;
    v.selectable = NO; return v;
}
static NSStackView *column(NSArray<NSView *> *views, CGFloat spacing) {
    NSStackView *s = [NSStackView stackViewWithViews:views]; s.orientation = NSUserInterfaceLayoutOrientationVertical;
    s.alignment = NSLayoutAttributeLeading; s.distribution = NSStackViewDistributionFill; s.spacing = spacing;
    return s;
}
static NSView *card(NSString *title, NSArray<NSView *> *views) {
    NSTextField *heading = label(title, 13, NSColor.labelColor); heading.font = [NSFont boldSystemFontOfSize:13];
    NSStackView *s = column([@[heading] arrayByAddingObjectsFromArray:views], 9);
    s.edgeInsets = NSEdgeInsetsMake(13, 14, 13, 14); s.wantsLayer = YES;
    s.layer.backgroundColor = [NSColor colorWithWhite:1 alpha:0.055].CGColor;
    s.layer.cornerRadius = 10;
    for (NSView *v in views) [v.widthAnchor constraintEqualToAnchor:s.widthAnchor constant:-28].active = YES;
    return s;
}
static NSView *row(NSString *title, NSView *control) {
    NSTextField *name = label(title, 12, NSColor.labelColor);
    NSStackView *s = [NSStackView stackViewWithViews:@[name, control]];
    s.orientation = NSUserInterfaceLayoutOrientationHorizontal; s.spacing = 12; s.alignment = NSLayoutAttributeCenterY;
    [name.widthAnchor constraintEqualToConstant:116].active = YES;
    [control.widthAnchor constraintGreaterThanOrEqualToConstant:242].active = YES;
    [s.heightAnchor constraintEqualToConstant:26].active = YES;
    return s;
}
@interface DmPreferencesDocument : NSView
@end
@implementation DmPreferencesDocument
- (BOOL)isFlipped { return YES; }
@end
@interface DmPreferencesWindow : NSPanel
@end
@implementation DmPreferencesWindow
- (void)cancelOperation:(id)sender { [self performClose:sender]; }
@end
@interface DmSettingsPanel ()
@property (weak) NSWindow *parent;
@property (copy) DmSettingsSnapshot snapshot;
@property (copy) DmSettingsChange change;
@property NSPopUpButton *output, *face;
@property NSButton *swap, *startup, *fullscreen, *hints;
@property NSTextField *dimensions, *connected;
@property NSTimer *timer;
@end
@implementation DmSettingsPanel
- (NSButton *)check:(NSString *)title setting:(DmMacSetting)setting identifier:(NSString *)identifier {
    NSButton *b = [NSButton checkboxWithTitle:title target:self action:@selector(changed:)];
    b.tag = setting; b.identifier = identifier; b.accessibilityIdentifier = identifier;
    return b;
}
- (NSPopUpButton *)popup:(NSArray<NSString *> *)titles tags:(NSArray<NSNumber *> *)tags setting:(DmMacSetting)setting identifier:(NSString *)identifier {
    NSPopUpButton *p = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    for (NSUInteger i = 0; i < titles.count; i++) { [p addItemWithTitle:titles[i]]; p.lastItem.tag = tags[i].integerValue; }
    p.tag = setting; p.target = self; p.action = @selector(changed:);
    p.identifier = identifier; p.accessibilityIdentifier = identifier;
    return p;
}
- (instancetype)initWithParent:(NSWindow *)parent snapshot:(DmSettingsSnapshot)snapshot change:(DmSettingsChange)change {
    DmPreferencesWindow *window = [[DmPreferencesWindow alloc] initWithContentRect:NSMakeRect(0, 0, 490, 760)
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable backing:NSBackingStoreBuffered defer:NO];
    if (!(self = [super initWithWindow:window])) return nil;
    self.parent = parent; self.snapshot = snapshot; self.change = change;
    window.title = UI("超级马里奥派对 · 设置", "Jamboree · Settings");
    window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    window.releasedWhenClosed = NO; window.hidesOnDeactivate = YES; window.delegate = self;
    window.collectionBehavior = NSWindowCollectionBehaviorFullScreenAuxiliary;
    window.titlebarAppearsTransparent = YES; window.backgroundColor = [NSColor colorWithWhite:0.075 alpha:1];
    NSTextField *heading = label(UI("设置与快捷键", "Settings & shortcuts"), 23, NSColor.labelColor);
    heading.font = [NSFont systemFontOfSize:23 weight:NSFontWeightSemibold];
    NSTextField *subtitle = label(UI("显示与按键即时生效，设置自动保存", "Display and button changes apply immediately; settings save automatically"), 12, NSColor.secondaryLabelColor);
    self.output = [self popup:output_names() tags:@[@0,@1,@2,@3,@4] setting:DmSettingOutput identifier:@"settings-output"];
    self.dimensions = label(@"", 11, NSColor.secondaryLabelColor);
    self.fullscreen = [self check:UI("全屏显示", "Full screen") setting:DmSettingFullscreen identifier:@"settings-fullscreen"];
    self.startup = [self check:UI("下次启动时全屏", "Start in full screen") setting:DmSettingStartFullscreen identifier:@"settings-start-fullscreen"];
    self.face = [self popup:@[face_name(0), face_name(2), face_name(1)] tags:@[@0,@2,@1] setting:DmSettingFaceLayout identifier:@"settings-controller"];
    self.swap = [self check:UI("交换 A / B", "Swap A / B") setting:DmSettingSwapAB identifier:@"settings-swap-ab"];
    self.connected = label(@"", 11, NSColor.secondaryLabelColor);
    NSButton *reset = [NSButton buttonWithTitle:UI("恢复控制器默认设置", "Reset controller layout") target:self action:@selector(resetController:)];
    reset.bezelStyle = NSBezelStyleRounded;
    NSTextField *quality = label(UI("画质：游戏原始设置", "Picture quality: original game settings"), 12, NSColor.labelColor);
    NSButton *taa = [NSButton checkboxWithTitle:UI("MetalFX 时间性边缘平滑（尚未适配）", "MetalFX temporal anti-aliasing (not yet supported)") target:nil action:nil];
    taa.enabled = NO; taa.identifier = @"settings-metalfx-unavailable"; taa.accessibilityIdentifier = @"settings-metalfx-unavailable";
    NSTextField *note = label(UI("输出选项仅控制显示尺寸上限，不改变游戏内部渲染分辨率。MetalFX 与内部画质增强需要单独适配。",
        "Output presets cap the display size, not the game's internal render resolution. MetalFX and internal quality enhancements require game-specific support."), 11, NSColor.secondaryLabelColor);
    self.hints = [self check:UI("显示游戏内快捷键提示", "Show in-game shortcut hints") setting:DmSettingHints identifier:@"settings-hints"];
    NSTextField *keys = label(UI("F1 / ⌘,  设置    ·    1  提示    ·    3  输出    ·    6  控制器\nWASD  移动    J / K / U / I  A / B / X / Y    Return  ＋",
        "F1 / ⌘,  Settings  ·  1  Hints  ·  3  Output  ·  6  Controller\nWASD  Move  ·  J / K / U / I  A / B / X / Y  ·  Return  +"), 11, NSColor.secondaryLabelColor);
    NSStackView *body = column(@[heading, subtitle,
        card(UI("显示", "Display"), @[row(UI("输出上限", "Output cap"), self.output), self.dimensions, self.fullscreen, self.startup]),
        card(UI("控制器", "Controller"), @[row(UI("按键布局", "Button layout"), self.face), self.swap, self.connected, reset]),
        card(UI("画质", "Picture quality"), @[quality, taa, note]),
        card(UI("快捷键", "Shortcuts"), @[self.hints, keys])], 12);
    body.translatesAutoresizingMaskIntoConstraints = NO;
    [window.contentView addSubview:body];
    [NSLayoutConstraint activateConstraints:@[
        [body.leadingAnchor constraintEqualToAnchor:window.contentView.leadingAnchor constant:20],
        [body.trailingAnchor constraintEqualToAnchor:window.contentView.trailingAnchor constant:-20],
        [body.topAnchor constraintEqualToAnchor:window.contentView.topAnchor constant:16]]];
    for (NSView *v in body.arrangedSubviews) [v.widthAnchor constraintEqualToAnchor:body.widthAnchor].active = YES;
    [window.contentView layoutSubtreeIfNeeded];
    CGFloat height = body.fittingSize.height;
    [body removeFromSuperview]; body.translatesAutoresizingMaskIntoConstraints = YES;
    body.frame = NSMakeRect(20, 16, 450, height);
    DmPreferencesDocument *document = [[DmPreferencesDocument alloc] initWithFrame:NSMakeRect(0, 0, 490, height + 34)];
    [document addSubview:body];
    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 490, height + 34)];
    scroll.drawsBackground = NO; scroll.hasVerticalScroller = YES; scroll.autohidesScrollers = YES;
    scroll.scrollerStyle = NSScrollerStyleOverlay; scroll.documentView = document;
    window.contentView = scroll;
    CGFloat available = (parent.screen ?: NSScreen.mainScreen).visibleFrame.size.height - 70;
    [window setContentSize:NSMakeSize(490, MIN(height + 34, MAX(300, available)))];
    [self refresh];
    __weak DmSettingsPanel *weak = self;
    self.timer = [NSTimer scheduledTimerWithTimeInterval:0.5 repeats:YES block:^(NSTimer *timer) { (void)timer; if (weak.window.visible) [weak refresh]; }];
    return self;
}
- (void)showPanel {
    [self refresh];
    if (!self.window.visible) {
        NSRect parent = self.parent.frame, frame = self.window.frame;
        NSRect screen = (self.parent.screen ?: NSScreen.mainScreen).visibleFrame;
        CGFloat x = NSMidX(parent) - frame.size.width / 2, y = NSMidY(parent) - frame.size.height / 2;
        x = MAX(NSMinX(screen), MIN(x, NSMaxX(screen) - frame.size.width));
        y = MAX(NSMinY(screen), MIN(y, NSMaxY(screen) - frame.size.height));
        [self.window setFrameOrigin:NSMakePoint(x, y)];
        [self.parent addChildWindow:self.window ordered:NSWindowAbove];
    }
    [self.window makeKeyAndOrderFront:nil];
}
- (void)windowWillClose:(NSNotification *)notification {
    (void)notification; [self.parent removeChildWindow:self.window];
    if (NSApp.active) [self.parent makeKeyAndOrderFront:nil];
}
- (void)changed:(NSControl *)control {
    NSInteger value = [control isKindOfClass:NSPopUpButton.class] ? ((NSPopUpButton *)control).selectedItem.tag : ((NSButton *)control).state == NSControlStateValueOn;
    self.change((DmMacSetting)control.tag, value); [self refresh];
}
- (void)resetController:(id)sender { (void)sender; self.change(DmSettingResetController, 0); [self refresh]; }
- (void)refresh {
    DmMacSettings s = self.snapshot();
    [self.output selectItemWithTag:s.output_preset]; [self.face selectItemWithTag:s.face_layout];
    self.swap.state = s.swap_ab; self.startup.state = s.start_fullscreen;
    self.fullscreen.state = s.fullscreen; self.hints.state = s.show_hints;
    self.dimensions.stringValue = [NSString stringWithFormat:UI("当前输出 %lu × %lu · 自动保持窗口比例", "Current output %lu × %lu · window aspect preserved"), s.output_width, s.output_height];
    self.connected.stringValue = [NSString stringWithFormat:UI("已连接 %lu 个控制器 · 此项调整按键映射，不模拟 Joy-Con 类型", "%lu controllers connected · changes button mapping, not Joy-Con emulation"), s.controllers];
}
@end

@interface DmShortcutHUD ()
@property (copy) DmSettingsSnapshot snapshot;
@property (copy) dispatch_block_t open;
@property NSTextField *text;
@property NSTimer *timer;
@end
@implementation DmShortcutHUD
- (instancetype)initWithSnapshot:(DmSettingsSnapshot)snapshot openSettings:(dispatch_block_t)open {
    if (!(self = [super initWithFrame:NSMakeRect(0, 0, 300, 126)])) return nil;
    self.snapshot = snapshot; self.open = open; self.wantsLayer = YES;
    self.layer.backgroundColor = [NSColor colorWithWhite:0.035 alpha:0.86].CGColor;
    self.layer.cornerRadius = 10; self.layer.borderWidth = 1;
    self.layer.borderColor = [NSColor colorWithWhite:1 alpha:0.16].CGColor;
    NSButton *button = [NSButton buttonWithTitle:UI("设置与快捷键   F1 / ⌘,", "Settings & shortcuts   F1 / ⌘,") target:self action:@selector(openPanel:)];
    button.bordered = NO; button.font = [NSFont boldSystemFontOfSize:12]; button.contentTintColor = NSColor.whiteColor;
    button.frame = NSMakeRect(10, 94, 280, 24); button.accessibilityIdentifier = @"settings-open";
    [self addSubview:button];
    self.text = label(@"", 11, [NSColor colorWithWhite:0.85 alpha:1]); self.text.frame = NSMakeRect(14, 10, 272, 80);
    [self addSubview:self.text]; self.accessibilityIdentifier = @"settings-shortcuts";
    [self refresh];
    __weak DmShortcutHUD *weak = self;
    self.timer = [NSTimer scheduledTimerWithTimeInterval:0.5 repeats:YES block:^(NSTimer *timer) { (void)timer; [weak refresh]; }];
    return self;
}
- (void)openPanel:(id)sender { (void)sender; self.open(); }
- (void)refresh {
    DmMacSettings s = self.snapshot(); self.hidden = !s.show_hints;
    if (self.superview) [self setFrameOrigin:NSMakePoint(MAX(10, self.superview.bounds.size.width - self.frame.size.width - 12), MAX(10, self.superview.bounds.size.height - self.frame.size.height - 12))];
    self.text.stringValue = [NSString stringWithFormat:UI("1  提示：显示 / 隐藏\n3  输出：%@\n6  控制器：%@%@\n⌃⌘F  全屏    ·    %lu × %lu", "1  Hints: show / hide\n3  Output: %@\n6  Controller: %@%@\n⌃⌘F  Full screen  ·  %lu × %lu"),
        output_names()[MAX(0, MIN(4, s.output_preset))], face_name(s.face_layout), s.swap_ab ? UI(" · A/B 已交换", " · A/B swapped") : @"", s.output_width, s.output_height];
}
@end
