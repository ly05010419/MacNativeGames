// Native macOS settings and compact shortcut HUD. All callbacks run on the AppKit thread.
#pragma once
#import <Cocoa/Cocoa.h>
#include "../gfx/nvn/aoc_stats.h"
typedef struct {
    int face_layout, swap_ab, start_fullscreen, output_preset, show_hints, fullscreen;
    NSUInteger output_width, output_height, controllers;
    int fps_target, dynamic_fps, stats_mode, frame_graph, screen_fps;
    AocStats performance;
} DmMacSettings;
typedef NS_ENUM(NSInteger, DmMacSetting) {
    DmSettingFaceLayout, DmSettingSwapAB, DmSettingStartFullscreen,
    DmSettingOutput, DmSettingHints, DmSettingFullscreen, DmSettingResetController,
    DmSettingFpsTarget, DmSettingDynamicFps, DmSettingStats, DmSettingFrameGraph
};
typedef DmMacSettings (^DmSettingsSnapshot)(void);
typedef void (^DmSettingsChange)(DmMacSetting setting, NSInteger value);
@interface DmSettingsPanel : NSWindowController <NSWindowDelegate>
- (instancetype)initWithParent:(NSWindow *)parent snapshot:(DmSettingsSnapshot)snapshot change:(DmSettingsChange)change;
- (void)showPanel;
- (void)refresh;
@end
@interface DmShortcutHUD : NSView
- (instancetype)initWithSnapshot:(DmSettingsSnapshot)snapshot openSettings:(dispatch_block_t)open;
- (void)refresh;
@end
@interface DmPerformanceHUD : NSView
- (instancetype)initWithSnapshot:(DmSettingsSnapshot)snapshot;
- (void)refresh;
@end
