// The corner box (front_hud.h). Its lines and their keys: hud_settings.h, front_shortcuts.h.
#import "front_hud.h"
#include "hud_settings.h"
#include "shader_activity.h"
#include "front_graph.h"
#include <math.h>
#include <time.h>

static NSTextField *hud_label(CGFloat size, NSColor *color) {
    NSTextField *v = [NSTextField wrappingLabelWithString:@""];
    v.font = [NSFont monospacedSystemFontOfSize:size weight:NSFontWeightMedium]; v.textColor = color; v.selectable = NO;
    return v;
}
NSString *dm_stats_text(DmMacSettings s, int lines, BOOL detail) {
    AocStats p = s.performance; AocShaderStats sh = p.shaders;
    NSMutableArray<NSString *> *out = [NSMutableArray array];
    if (lines & HUD_FPS) {
        NSString *game = p.active && p.samples ? [NSString stringWithFormat:@"%.1f", p.game_fps] : @"--";
        NSString *display = p.display_active ? [NSString stringWithFormat:@"%.1f", p.display_fps] : @"--";
        // No frame rate target: Skyward Sword HD paces itself (its own present interval)
        [out addObject:[NSString stringWithFormat:@"Game %@ FPS · Display %@ FPS · Screen %d Hz", game, display, s.screen_fps]];
    }
    if (lines & HUD_TIMING) {
        NSString *gpu = p.gpu_ms >= 0 ? [NSString stringWithFormat:@"%.2f", p.gpu_ms] : @"--";
        [out addObject:p.active && p.samples ? [NSString stringWithFormat:@"Frame %.2f ms · GPU %@ ms\np50 %.2f · p95 %.2f · p99 %.2f ms",
                                                   p.frame_ms, gpu, p.p50_ms, p.p95_ms, p.p99_ms] : @"Frame timing: waiting for samples"];
    }
    if (detail)
        [out addObject:[NSString stringWithFormat:@"Libraries %llu · Inventory %llu keys\nRender PSOs %llu · Compute PSOs %llu",
            (unsigned long long)sh.libraries, (unsigned long long)sh.inventory, (unsigned long long)sh.render_pipelines, (unsigned long long)sh.compute_pipelines]];
    if (lines & HUD_SHADERS) {
        // N: on-demand attempts, failed ones included; Ready: Metal libraries / render + compute pipelines held
        BOOL ms = detail || (lines & HUD_TIMING);
        [out addObject:[NSString stringWithFormat:@"On-demand library loads %llu%@ · Ready %llu\nOn-demand pipeline builds %llu%@ · Ready %llu",
            (unsigned long long)sh.library_loads, ms ? [NSString stringWithFormat:@" / %.1f ms", sh.library_ms] : @"", (unsigned long long)sh.libraries,
            (unsigned long long)sh.pipeline_builds, ms ? [NSString stringWithFormat:@" / %.1f ms", sh.pipeline_ms] : @"",
            (unsigned long long)(sh.render_pipelines + sh.compute_pipelines)]];
    }
    if (detail)
        [out addObject:[NSString stringWithFormat:@"Prewarm imported %llu lib / %llu render / %llu compute%@",
            (unsigned long long)sh.warm_libraries, (unsigned long long)sh.warm_render, (unsigned long long)sh.warm_compute, sh.warming ? @" (running)" : @""]];
    if (detail || ((lines & HUD_SHADERS) && (sh.load_errors || sh.pipeline_errors)))
        [out addObject:[NSString stringWithFormat:@"Load errors %llu · Render PSO errors %llu", (unsigned long long)sh.load_errors, (unsigned long long)sh.pipeline_errors]];
    return [out componentsJoinedByString:@"\n"];
}

@interface DmFrameGraph : NSView
@property AocStats sample;
@end
@implementation DmFrameGraph
- (void)drawRect:(NSRect)dirty {
    (void)dirty; AocStats p = self.sample;
    NSDictionary *style = @{NSFontAttributeName: [NSFont monospacedSystemFontOfSize:9 weight:NSFontWeightRegular], NSForegroundColorAttributeName: NSColor.lightGrayColor};
    [@"FPS · last 240 frames" drawAtPoint:NSMakePoint(0, self.bounds.size.height - 15) withAttributes:style];
    NSRect plot = NSMakeRect(34, 8, MAX(1, self.bounds.size.width - 40), self.bounds.size.height - 30);
    const double top = DM_GRAPH_TOP_FPS, guides[] = {0, 30, 60};   // frames per second, 0 at the bottom
    for (int i = 0; i < 3; i++) {
        CGFloat y = NSMinY(plot) + (guides[i] / top) * plot.size.height;
        NSBezierPath *line = [NSBezierPath bezierPath]; [line moveToPoint:NSMakePoint(NSMinX(plot), y)]; [line lineToPoint:NSMakePoint(NSMaxX(plot), y)];
        [[NSColor colorWithWhite:1 alpha:.2] setStroke]; [line stroke];
        [[NSString stringWithFormat:@"%.0f", guides[i]] drawAtPoint:NSMakePoint(0, y - 5) withAttributes:style];
    }
    NSBezierPath *line = [NSBezierPath bezierPath];
    for (unsigned i = 0; i < p.samples; i++) {
        NSPoint pt = NSMakePoint(NSMinX(plot) + i * plot.size.width / (AOC_STATS_SAMPLES - 1), NSMinY(plot) + (dm_graph_fps(p.history[i], top) / top) * plot.size.height);
        if (i) [line lineToPoint:pt]; else [line moveToPoint:pt];
    }
    [NSColor.systemGreenColor setStroke]; line.lineWidth = 1.5; [line stroke];
}
@end

@interface DmCornerHUD () { DmShaderActivity _activity; }
@property (copy) DmSettingsSnapshot snapshot;
@property NSTextField *stats, *keys;
@property DmFrameGraph *graph;
@property NSTimer *timer;
@end
@implementation DmCornerHUD
- (instancetype)initWithSnapshot:(DmSettingsSnapshot)snapshot {
    if (!(self = [super initWithFrame:NSMakeRect(0, 0, 320, 120)])) return nil;
    self.snapshot = snapshot; self.identifier = @"corner-hud";
    self.wantsLayer = YES; self.layer.cornerRadius = 8; self.layer.backgroundColor = [NSColor colorWithWhite:.025 alpha:.82].CGColor;
    self.stats = hud_label(10.5, NSColor.systemGreenColor); self.stats.identifier = @"hud-stats"; [self addSubview:self.stats];
    self.keys = hud_label(10.5, [NSColor colorWithWhite:.85 alpha:1]); self.keys.identifier = @"hud-keys"; [self addSubview:self.keys];
    self.keys.stringValue = @"1  Box · 2  FPS · 3  Shaders · 4  Timing\n5  Graph · 6  Shortcuts · F1  Settings\n⌃⌘F  Full screen · ⌘Q  Quit";
    self.graph = [DmFrameGraph new]; [self addSubview:self.graph];
    __weak DmCornerHUD *weak = self;
    self.timer = [NSTimer scheduledTimerWithTimeInterval:.5 repeats:YES block:^(NSTimer *timer) { (void)timer; [weak refresh]; }];
    [self refresh]; return self;
}
- (NSView *)hitTest:(NSPoint)point { (void)point; return nil; }   // clicks go to the game (the touch screen)
- (void)refresh {
    DmMacSettings s = self.snapshot(); DmHud eff = {s.hud_on, s.hud_lines};
    self.hidden = !dm_hud_visible(eff);
    if (self.hidden || !(eff.lines & HUD_SHADERS)) dm_activity_reset(&_activity);   // shown again: the next sample is a new start, never a false yellow
    if (self.hidden) return;
    AocShaderStats sh = s.performance.shaders;
    int c = sh.load_errors || sh.pipeline_errors ? DM_ACTIVITY_ORANGE : DM_ACTIVITY_GREEN;
    if (eff.lines & HUD_SHADERS)   // yellow: an on-demand counter rose in the last 3 s (a hint where to look, not proof of a compile or of a hitch's cause)
        c = dm_activity_step(&_activity, sh.library_loads, sh.pipeline_builds, sh.load_errors + sh.pipeline_errors, clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) / 1e9);
    self.stats.textColor = c == DM_ACTIVITY_ORANGE ? NSColor.systemOrangeColor : c == DM_ACTIVITY_YELLOW ? NSColor.systemYellowColor : NSColor.systemGreenColor;
    int statLines = eff.lines & (HUD_FPS | HUD_SHADERS | HUD_TIMING);
    self.stats.stringValue = statLines ? dm_stats_text(s, statLines, NO) : @"";
    self.stats.hidden = !statLines; self.keys.hidden = !(eff.lines & HUD_KEYS); self.graph.hidden = !(eff.lines & HUD_GRAPH);
    NSSize st = self.stats.hidden ? NSZeroSize : [self.stats sizeThatFits:NSMakeSize(420, CGFLOAT_MAX)];
    NSSize ks = self.keys.hidden ? NSZeroSize : [self.keys sizeThatFits:NSMakeSize(420, CGFLOAT_MAX)];
    CGFloat graph = self.graph.hidden ? 0 : 110, gap = 6;
    CGFloat w = MAX(300, ceil(MAX(st.width, ks.width)) + 20);
    CGFloat h = 20 + ceil(st.height) + graph + ceil(ks.height) + gap * ((!self.stats.hidden) + (!self.graph.hidden) + (!self.keys.hidden) - 1);
    NSRect area = self.superview ? self.superview.bounds : NSMakeRect(0, 0, w + 24, h + 24);
    self.frame = NSMakeRect(NSMinX(area) + 12, MAX(8, NSMaxY(area) - h - 12), w, h);   // top-left corner
    CGFloat y = h - 10;
    if (!self.stats.hidden) { y -= ceil(st.height); self.stats.frame = NSMakeRect(10, y, w - 20, ceil(st.height)); y -= gap; }
    if (!self.graph.hidden) { y -= graph; self.graph.frame = NSMakeRect(10, y, w - 20, graph); self.graph.sample = s.performance; [self.graph setNeedsDisplay:YES]; y -= gap; }
    if (!self.keys.hidden) { y -= ceil(ks.height); self.keys.frame = NSMakeRect(10, y, w - 20, ceil(ks.height)); }
}
@end
