// AoC's own UI: independent of the other games' front ends.
#import "front_settings.h"
#import "front_shortcuts.h"
#include "shader_activity.h"
#include "front_graph.h"
#include <math.h>
#include <time.h>

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
static NSString *size_text(long w, long h) { return w > 0 && h > 0 ? [NSString stringWithFormat:@"%ld × %ld", w, h] : @"--"; }
// A size as observed by the host: plain while observed within DM_SIZE_FRESH_S, "last seen N s ago" once older (the game stopped drawing the scene target, or
// stopped presenting), -- when never observed.
#define DM_SIZE_FRESH_S 1.0
static NSString *seen_text(long w, long h, double age_s) {
    if (age_s < 0 || w <= 0 || h <= 0) return @"--";
    if (age_s <= DM_SIZE_FRESH_S) return size_text(w, h);
    return [NSString stringWithFormat:@"%@ (last seen %.0f s ago)", size_text(w, h), floor(age_s)];
}
// The resolutions: the game's 3D picture (its scene target) and output (the texture it presents), as last observed, and the window it is shown in.
static NSString *resolution_text(DmMacSettings s) {
    return [NSString stringWithFormat:@"Game picture %@ · game output %@ · window %@", seen_text(s.scene_width, s.scene_height, s.scene_age_s),
        seen_text(s.game_width, s.game_height, s.game_age_s), size_text((long)s.output_width, (long)s.output_height)];
}
static NSString *performance_text(DmMacSettings s, BOOL full) {
    AocStats p = s.performance;
    NSString *game = p.active && p.samples ? [NSString stringWithFormat:@"%.1f",p.game_fps] : @"--";
    NSString *display = p.display_active ? [NSString stringWithFormat:@"%.1f",p.display_fps] : @"--";
    NSString *head = [NSString stringWithFormat:@"Game %@ FPS · Display %@ FPS\nTarget %d · Screen %d Hz · %@",
        game, display, s.fps_target ? s.fps_target : 30, s.screen_fps,
        s.fps_target && s.dynamic_fps ? @"Dynamic step" : @"Fixed step"];
    head = [NSString stringWithFormat:@"%@\nPicture %@ → output %@ → window %@", head, seen_text(s.scene_width, s.scene_height, s.scene_age_s),
        seen_text(s.game_width, s.game_height, s.game_age_s), size_text((long)s.output_width, (long)s.output_height)];
    AocShaderStats sh=p.shaders;   // on-demand attempts while playing, failed ones included; Ready: Metal libraries / render + compute pipelines held
    NSString *ms[2]={full?[NSString stringWithFormat:@" / %.1f ms",sh.library_ms]:@"",full?[NSString stringWithFormat:@" / %.1f ms",sh.pipeline_ms]:@""};
    NSString *demand=[NSString stringWithFormat:@"On-demand library loads %llu%@ · Ready %llu\nOn-demand pipeline builds %llu%@ · Ready %llu",(unsigned long long)sh.library_loads,ms[0],
        (unsigned long long)sh.libraries,(unsigned long long)sh.pipeline_builds,ms[1],(unsigned long long)(sh.render_pipelines+sh.compute_pipelines)];
    if (!full) return [NSString stringWithFormat:@"%@\n%@", head, demand];
    NSString *gpu=p.gpu_ms>=0?[NSString stringWithFormat:@"%.2f",p.gpu_ms]:@"--";
    NSString *timing = p.active && p.samples ? [NSString stringWithFormat:@"Frame %.2f ms · GPU %@ ms\np50 %.2f · p95 %.2f · p99 %.2f ms",p.frame_ms,gpu,p.p50_ms,p.p95_ms,p.p99_ms] : @"Frame timing: waiting for samples";
    return [NSString stringWithFormat:@"%@\n%@\nLibraries %llu · Inventory %llu keys\nRender PSOs %llu · Compute PSOs %llu\n%@\nPrewarm imported %llu lib / %llu render / %llu compute%@\nLoad errors %llu · Render PSO errors %llu",
        head,timing,(unsigned long long)sh.libraries,(unsigned long long)sh.inventory,
        (unsigned long long)sh.render_pipelines,(unsigned long long)sh.compute_pipelines,demand,
        (unsigned long long)sh.warm_libraries,(unsigned long long)sh.warm_render,(unsigned long long)sh.warm_compute,
        sh.warming?@" (running)":@"",(unsigned long long)sh.load_errors,(unsigned long long)sh.pipeline_errors];
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
// Key 1 or F1 closes the panel as it opened it; a held key's repeats do nothing, and its release then reaches the game window,
// which swallows it (front_shortcuts.h dm_mac_game_key).
- (void)keyDown:(NSEvent *)event {
    switch (dm_mac_panel_key(event.keyCode, event.modifierFlags, event.isARepeat)) {
        case DmPanelKeyClose: [self performClose:nil]; return;
        case DmPanelKeySwallow: return;
        case DmPanelKeyPass: [super keyDown:event]; return;
    }
}
@end
@interface DmSettingsPanel ()
@property (weak) NSWindow *parent;
@property (copy) DmSettingsSnapshot snapshot;
@property (copy) DmSettingsChange change;
@property NSPopUpButton *output, *face, *fps, *stats, *aspect;
@property NSButton *swap, *startup, *fullscreen, *dynamicFps, *frameGraph;
@property NSTextField *dimensions, *connected, *performance;
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
    window.title = @"Hyrule Warriors: Age of Calamity · Settings";
    window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    window.releasedWhenClosed = NO; window.hidesOnDeactivate = YES; window.delegate = self;
    window.collectionBehavior = NSWindowCollectionBehaviorFullScreenAuxiliary;
    window.titlebarAppearsTransparent = YES; window.backgroundColor = [NSColor colorWithWhite:0.075 alpha:1];
    NSTextField *heading = label(@"Settings & shortcuts", 23, NSColor.labelColor);
    heading.font = [NSFont systemFontOfSize:23 weight:NSFontWeightSemibold];
    NSTextField *subtitle = label(@"Display and button changes apply immediately; settings save automatically", 12, NSColor.secondaryLabelColor);
    self.fps = [self popup:@[@"Original · 30 FPS", @"30 FPS", @"60 FPS"] tags:@[@0,@30,@60] setting:DmSettingFpsTarget identifier:@"settings-fps"];
    self.dynamicFps = [self check:@"Dynamic step (follow actual update time)" setting:DmSettingDynamicFps identifier:@"settings-dynamic-fps"];
    self.stats = [self popup:@[@"Off", @"FPS only", @"Full stats"] tags:@[@0,@1,@2] setting:DmSettingStats identifier:@"settings-stats"];
    self.frameGraph = [self check:@"Show frame-time graph" setting:DmSettingFrameGraph identifier:@"settings-frame-graph"];
    self.performance = label(@"", 10, NSColor.secondaryLabelColor);
    self.performance.font = [NSFont monospacedSystemFontOfSize:10 weight:NSFontWeightRegular];
    self.performance.identifier = @"settings-performance";
    self.output = [self popup:output_names() tags:@[@0,@1,@2,@3,@4] setting:DmSettingOutput identifier:@"settings-output"];
    self.dimensions = label(@"", 11, NSColor.secondaryLabelColor); self.dimensions.identifier = @"settings-resolution";
    self.aspect = [self popup:@[@"16:9 · original", @"16:10 · fills a Mac screen"] tags:@[@0,@1] setting:DmSettingAspect identifier:@"settings-aspect"];
    self.aspect.toolTip = @"16:10 stretches the picture to the screen: the 3D view sees the wider shape and the HUD stays in the middle 16:9, as in Tears of the Kingdom";
    self.fullscreen = [self check:@"Full screen" setting:DmSettingFullscreen identifier:@"settings-fullscreen"];
    self.startup = [self check:@"Start in full screen" setting:DmSettingStartFullscreen identifier:@"settings-start-fullscreen"];
    self.face = [self popup:@[face_name(0), face_name(2), face_name(1)] tags:@[@0,@2,@1] setting:DmSettingFaceLayout identifier:@"settings-controller"];
    self.swap = [self check:@"Swap A / B" setting:DmSettingSwapAB identifier:@"settings-swap-ab"];
    self.connected = label(@"", 11, NSColor.secondaryLabelColor);
    NSButton *reset = [NSButton buttonWithTitle:@"Reset controller layout" target:self action:@selector(resetController:)];
    reset.bezelStyle = NSBezelStyleRounded;
    NSTextField *quality = label(@"Picture quality: original game settings", 12, NSColor.labelColor);
    NSButton *taa = [NSButton checkboxWithTitle:@"MetalFX temporal anti-aliasing (not yet supported)" target:nil action:nil];
    taa.enabled = NO; taa.identifier = @"settings-metalfx-unavailable"; taa.accessibilityIdentifier = @"settings-metalfx-unavailable";
    NSTextField *note = label(@"The window size cap only scales the shown picture; it does not change the game's own resolution (Display shows it). MetalFX and internal quality enhancements require game-specific support.", 11, NSColor.secondaryLabelColor);
    NSTextField *keys = label(@"1 / F1 / ⌘,  Settings (1, F1 or Esc closes) · ⌃⌘F  Full screen\nWASD  Move · Arrow keys  D-pad\nJ / K / U / I  A / B / X / Y\nQ / E  L / R · Z / C  ZL / ZR\nF / G  Stick clicks · Return / Tab  + / −\nSpace  Shake · Left mouse  Touch screen", 11, NSColor.secondaryLabelColor);
    NSStackView *body = column(@[heading, subtitle,
        card(@"Display", [(snapshot().aspect_available ? @[row(@"Aspect ratio", self.aspect)] : @[]) arrayByAddingObjectsFromArray:
            @[row(@"Window size cap", self.output), self.dimensions, self.fullscreen, self.startup]]),
        card(@"Frame rate & stats", @[row(@"FPS target", self.fps), self.dynamicFps,
            label(@"60 FPS is experimental. 120 FPS is disabled pending gameplay timing fixes.", 11, NSColor.secondaryLabelColor),
            row(@"Stats pane", self.stats), self.frameGraph, self.performance]),
        card(@"Controller", @[row(@"Button layout", self.face), self.swap, self.connected, reset]),
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
- (void)togglePanel { if (self.window.visible) [self.window performClose:nil]; else [self showPanel]; }
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
    [self.output selectItemWithTag:s.output_preset]; [self.face selectItemWithTag:s.face_layout]; [self.aspect selectItemWithTag:s.aspect];
    [self.fps selectItemWithTag:s.fps_target]; [self.stats selectItemWithTag:s.stats_mode];
    self.dynamicFps.state=s.dynamic_fps; self.dynamicFps.enabled=s.fps_target!=0;
    self.frameGraph.state=s.frame_graph;
    self.stats.enabled=!s.stats_locked; self.frameGraph.enabled=!s.graph_locked;   // the environment decides them for this run
    self.stats.toolTip=s.stats_locked?@"Set by DM_AOC_STATS for this run; the saved choice is kept":nil;
    self.frameGraph.toolTip=s.graph_locked?@"Set by DM_AOC_FRAME_GRAPH for this run; the saved choice is kept":nil;
    self.performance.stringValue=performance_text(s,YES);
    self.swap.state = s.swap_ab; self.startup.state = s.start_fullscreen;
    self.fullscreen.state = s.fullscreen;
    self.dimensions.stringValue = resolution_text(s);
    self.connected.stringValue = [NSString stringWithFormat:@"%lu controllers connected · changes button mapping, not Joy-Con emulation", s.controllers];
}
@end

@interface DmFrameGraph : NSView
@property AocStats sample;
@end
@implementation DmFrameGraph
- (void)drawRect:(NSRect)dirty {
    (void)dirty; AocStats p=self.sample;
    NSDictionary *style=@{NSFontAttributeName:[NSFont monospacedSystemFontOfSize:9 weight:NSFontWeightRegular],NSForegroundColorAttributeName:NSColor.lightGrayColor};
    [@"FPS · last 240 frames" drawAtPoint:NSMakePoint(0,self.bounds.size.height-15) withAttributes:style];
    NSRect plot=NSMakeRect(34,8,MAX(1,self.bounds.size.width-40),self.bounds.size.height-30);
    const double top=DM_GRAPH_TOP_FPS, guides[]={0,30,60};   // frames per second, 0 at the bottom
    for(int i=0;i<3;i++) {
        CGFloat y=NSMinY(plot)+(guides[i]/top)*plot.size.height;
        NSBezierPath *line=[NSBezierPath bezierPath];[line moveToPoint:NSMakePoint(NSMinX(plot),y)];[line lineToPoint:NSMakePoint(NSMaxX(plot),y)];
        [[NSColor colorWithWhite:1 alpha:.2] setStroke];[line stroke];
        [[NSString stringWithFormat:@"%.0f",guides[i]] drawAtPoint:NSMakePoint(0,y-5) withAttributes:style];
    }
    NSBezierPath *line=[NSBezierPath bezierPath];
    for(unsigned i=0;i<p.samples;i++) {
        NSPoint pt=NSMakePoint(NSMinX(plot)+i*plot.size.width/(AOC_STATS_SAMPLES-1),NSMinY(plot)+(dm_graph_fps(p.history[i],top)/top)*plot.size.height);
        if(i)[line lineToPoint:pt];else[line moveToPoint:pt];
    }
    [NSColor.systemGreenColor setStroke];line.lineWidth=1.5;[line stroke];
}
@end
@interface DmPerformanceHUD () { DmShaderActivity _activity; }
@property(copy) DmSettingsSnapshot snapshot;
@property NSTextField *text;
@property DmFrameGraph *graph;
@property NSTimer *timer;
@end
@implementation DmPerformanceHUD
- (instancetype)initWithSnapshot:(DmSettingsSnapshot)snapshot {
    if(!(self=[super initWithFrame:NSMakeRect(0,0,340,260)])) return nil;
    self.snapshot=snapshot;self.identifier=@"performance-hud";
    self.wantsLayer=YES;self.layer.cornerRadius=8;self.layer.backgroundColor=[NSColor colorWithWhite:.025 alpha:.82].CGColor;
    self.text=label(@"",10.5,NSColor.systemGreenColor);self.text.identifier=@"performance-text";
    self.text.font=[NSFont monospacedSystemFontOfSize:10.5 weight:NSFontWeightMedium];[self addSubview:self.text];
    self.graph=[DmFrameGraph new];[self addSubview:self.graph];
    __weak DmPerformanceHUD *weak=self;
    self.timer=[NSTimer scheduledTimerWithTimeInterval:.5 repeats:YES block:^(NSTimer *timer){(void)timer;[weak refresh];}];
    [self refresh];return self;
}
- (NSView *)hitTest:(NSPoint)point {(void)point;return nil;}
- (void)refresh {
    DmMacSettings s=self.snapshot();self.hidden=s.stats_mode==0; if(self.hidden){dm_activity_reset(&_activity);return;}
    self.text.stringValue=performance_text(s,s.stats_mode==2);
    AocShaderStats sh=s.performance.shaders;
    int c=dm_activity_step(&_activity,sh.library_loads,sh.pipeline_builds,sh.load_errors+sh.pipeline_errors,clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW)/1e9);
    self.text.textColor=c==DM_ACTIVITY_ORANGE?NSColor.systemOrangeColor:c==DM_ACTIVITY_YELLOW?NSColor.systemYellowColor:NSColor.systemGreenColor;
    NSSize text=[self.text sizeThatFits:NSMakeSize(330,CGFLOAT_MAX)];
    CGFloat graph=s.frame_graph?110:0,w=MAX(320,ceil(text.width)+20),h=ceil(text.height)+graph+20;
    self.frame=NSMakeRect(self.superview?MAX(8,self.superview.bounds.size.width-w-12):0,self.superview?MAX(8,self.superview.bounds.size.height-h-12):0,w,h);
    self.text.frame=NSMakeRect(10,graph+10,w-20,ceil(text.height));
    self.graph.hidden=!s.frame_graph;self.graph.frame=NSMakeRect(10,8,w-20,graph);self.graph.sample=s.performance;[self.graph setNeedsDisplay:YES];
}
@end
