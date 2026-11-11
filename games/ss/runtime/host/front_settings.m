// The settings window (F1 / ⌘,); the corner box is front_hud.m.
#import "front_settings.h"
#import "front_hud.h"
#include "hud_settings.h"
#import "pso_export.h"

static NSArray<NSString *> *output_names(void) {
    return @[@"Automatic (window size)", @"720p · 1280 × 720", @"1080p · 1920 × 1080",
             @"1440p · 2560 × 1440", @"2160p · 3840 × 2160"];
}
static NSString *face_name(int layout) {
    return layout == 2 ? @"Nintendo labels" : layout == 1 ? @"Xbox / PlayStation positions" : @"Automatic";
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
@property NSButton *swap, *startup, *fullscreen, *hud;
@property NSArray<NSButton *> *hudLines;   // tag: the line bit (hud_settings.h)
@property NSTextField *dimensions, *connected, *performance, *shaderCount;
@property NSButton *exportShaders;
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
    window.title = @"The Legend of Zelda: Skyward Sword HD · Settings";
    window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    window.releasedWhenClosed = NO; window.hidesOnDeactivate = YES; window.delegate = self;
    window.collectionBehavior = NSWindowCollectionBehaviorFullScreenAuxiliary;
    window.titlebarAppearsTransparent = YES; window.backgroundColor = [NSColor colorWithWhite:0.075 alpha:1];
    NSTextField *heading = label(@"Settings & shortcuts", 23, NSColor.labelColor);
    heading.font = [NSFont systemFontOfSize:23 weight:NSFontWeightSemibold];
    NSTextField *subtitle = label(@"Display and button changes apply immediately; settings save automatically", 12, NSColor.secondaryLabelColor);
    self.hud = [self check:@"Show the corner box (key 1)" setting:DmSettingHudOn identifier:@"settings-hud"];
    NSMutableArray<NSButton *> *lines = [NSMutableArray array];
    NSArray<NSString *> *names = @[@"FPS (2)", @"On-demand shaders and pipelines (3)", @"Frame timing (4)", @"FPS graph (5)", @"Shortcut list (6)"];
    for (int i = 0; i < 5; i++) {
        NSButton *b = [NSButton checkboxWithTitle:names[i] target:self action:@selector(hudLine:)];
        b.tag = 1 << i; b.identifier = b.accessibilityIdentifier = [NSString stringWithFormat:@"settings-hud-%s", dm_hud_names[i]]; [lines addObject:b];
    }
    self.hudLines = lines;
    self.performance = label(@"", 10, NSColor.secondaryLabelColor);
    self.performance.font = [NSFont monospacedSystemFontOfSize:10 weight:NSFontWeightRegular];
    self.performance.identifier = @"settings-performance";
    self.output = [self popup:output_names() tags:@[@0,@1,@2,@3,@4] setting:DmSettingOutput identifier:@"settings-output"];
    self.dimensions = label(@"", 11, NSColor.secondaryLabelColor);
    self.fullscreen = [self check:@"Full screen" setting:DmSettingFullscreen identifier:@"settings-fullscreen"];
    self.startup = [self check:@"Start in full screen" setting:DmSettingStartFullscreen identifier:@"settings-start-fullscreen"];
    self.face = [self popup:@[face_name(0), face_name(2), face_name(1)] tags:@[@0,@2,@1] setting:DmSettingFaceLayout identifier:@"settings-controller"];
    self.swap = [self check:@"Swap A / B" setting:DmSettingSwapAB identifier:@"settings-swap-ab"];
    self.connected = label(@"", 11, NSColor.secondaryLabelColor);
    self.shaderCount = label(@"", 11, NSColor.secondaryLabelColor);
    self.exportShaders = [NSButton buttonWithTitle:@"Export shader list…" target:self action:@selector(exportShaderList:)];
    self.exportShaders.bezelStyle = NSBezelStyleRounded; self.exportShaders.identifier = @"settings-export-shaders";
    NSTextField *share = label(@"Saves the shaders and pipelines this Mac built that the app's list lacks, to send by hand. Nothing is sent.", 11, NSColor.secondaryLabelColor);
    NSButton *reset = [NSButton buttonWithTitle:@"Reset controller layout" target:self action:@selector(resetController:)];
    reset.bezelStyle = NSBezelStyleRounded;
    NSTextField *quality = label(@"Picture quality: original game settings", 12, NSColor.labelColor);
    NSButton *taa = [NSButton checkboxWithTitle:@"MetalFX temporal anti-aliasing (not yet supported)" target:nil action:nil];
    taa.enabled = NO; taa.identifier = @"settings-metalfx-unavailable"; taa.accessibilityIdentifier = @"settings-metalfx-unavailable";
    NSTextField *note = label(@"Output presets cap the display size, not the game's internal render resolution. MetalFX and internal quality enhancements require game-specific support.", 11, NSColor.secondaryLabelColor);
    NSTextField *keys = label(@"F1 / ⌘,  Settings · ⌃⌘F  Full screen\n1  Corner box · 2  FPS · 3  Shaders\n4  Timing · 5  Graph · 6  Shortcut list\nWASD  Move · Arrow keys  D-pad\nJ / K / U / I  A / B / X / Y\nQ / E  L / R · Z / C  ZL / ZR\nF / G  Stick clicks · Return / Tab  + / −\nSpace  Shake · Left mouse  Touch screen", 11, NSColor.secondaryLabelColor);
    NSStackView *body = column(@[heading, subtitle,
        card(@"Display", @[row(@"Output cap", self.output), self.dimensions, self.fullscreen, self.startup]),
        card(@"Corner box", [[@[self.hud] arrayByAddingObjectsFromArray:self.hudLines] arrayByAddingObject:self.performance]),
        card(@"Controller", @[row(@"Button layout", self.face), self.swap, self.connected, reset]),
        card(@"Shader list", @[self.shaderCount, self.exportShaders, share]),
        card(@"Picture quality", @[quality, taa, note]),
        card(@"Shortcuts", @[keys])], 12);
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
- (void)exportShaderList:(id)sender {
    (void)sender;
    NSDateFormatter *f = [NSDateFormatter new]; f.dateFormat = @"yyyy-MM-dd";
    NSSavePanel *save = [NSSavePanel savePanel];   // asks before replacing a file, as every save panel does
    save.nameFieldStringValue = [NSString stringWithFormat:@"ss-shaders-%@.tsv", [f stringFromDate:NSDate.date]];
    save.directoryURL = [NSFileManager.defaultManager URLsForDirectory:NSDesktopDirectory inDomains:NSUserDomainMask].firstObject;
    [save beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse response) {
        NSString *message = [self finishExport:response url:save.URL];
        if (!message) return;
        NSAlert *alert = [NSAlert new]; alert.messageText = message; [alert beginSheetModalForWindow:self.window completionHandler:nil];
        [self refresh];
    }];
}
- (NSString *)finishExport:(NSModalResponse)response url:(NSURL *)url {
    if (response != NSModalResponseOK || !url) return nil;   // cancelled: nothing written
    DmPsoExport *e = [DmPsoExport fromUserPath:self.shaderLists.firstObject seedPath:self.shaderLists.lastObject];   // counted again now, from one read
    NSString *problem = [e writeTo:url date:NSDate.date];
    if (problem) return [@"Not exported: " stringByAppendingString:problem];
    return [NSString stringWithFormat:@"Saved %lu rows to %@%@. Nothing was sent: send the file by hand.", (unsigned long)e.rows, url.lastPathComponent,
            e.partialTail ? @" (a row still being written was left out)" : @""];
}
- (void)hudLine:(NSButton *)b { self.change(DmSettingHudLine, b.tag << 1 | (b.state == NSControlStateValueOn)); [self refresh]; }
- (void)resetController:(id)sender { (void)sender; self.change(DmSettingResetController, 0); [self refresh]; }
- (void)refresh {
    DmMacSettings s = self.snapshot();
    [self.output selectItemWithTag:s.output_preset]; [self.face selectItemWithTag:s.face_layout];
    self.hud.state = dm_hud_visible((DmHud){s.hud_on, s.hud_lines});
    for (NSButton *b in self.hudLines) b.state = (s.hud_lines & b.tag) != 0;
    self.performance.stringValue = dm_stats_text(s, HUD_FPS | HUD_SHADERS | HUD_TIMING, YES);
    self.swap.state = s.swap_ab; self.startup.state = s.start_fullscreen;
    self.fullscreen.state = s.fullscreen;
    self.dimensions.stringValue = [NSString stringWithFormat:@"Current output %lu × %lu · window aspect preserved", s.output_width, s.output_height];
    self.connected.stringValue = [NSString stringWithFormat:@"%lu controllers connected · changes button mapping, not Joy-Con emulation", s.controllers];
    DmPsoExport *e = self.shaderLists.count == 2 ? [DmPsoExport fromUserPath:self.shaderLists[0] seedPath:self.shaderLists[1]] : nil;
    self.shaderCount.stringValue = !e ? @"No shader list on this run" : e.error ? [@"Shader list unreadable: " stringByAppendingString:e.error] :
        [NSString stringWithFormat:@"%lu new rows not in this app's list%@", (unsigned long)e.rows, e.partialTail ? @" (a row still being written is left out)" : @""];
    self.exportShaders.enabled = e && !e.error && e.rows;
}
@end
