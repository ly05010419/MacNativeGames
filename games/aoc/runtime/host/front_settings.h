// Native macOS settings panel and stats pane. All callbacks run on the AppKit thread.
#pragma once
#import <Cocoa/Cocoa.h>
#include "../gfx/nvn/aoc_stats.h"
typedef struct {
    int face_layout, swap_ab, start_fullscreen, output_preset, fullscreen, aspect;   // aspect: 0 = the game's 16:9, 1 = 16:10
    int aspect_available;   // the game build has the camera stubs 16:10 needs (front_prefs.h dm_aspect_supported); 0: no Aspect ratio row
    NSUInteger output_width, output_height, controllers;
    int fps_target, dynamic_fps, stats_mode, frame_graph, screen_fps;
    int stats_locked, graph_locked;
    int scene_width, scene_height, game_width, game_height;   // the game's picture (scene target) and its output (presented texture), as last observed
    double scene_age_s, game_age_s;   // seconds since each was observed; -1: never (shown as --)   // DM_AOC_STATS / DM_AOC_FRAME_GRAPH decide the row for this run (front_prefs.h)
    AocStats performance;
} DmMacSettings;
typedef NS_ENUM(NSInteger, DmMacSetting) {
    DmSettingFaceLayout, DmSettingSwapAB, DmSettingStartFullscreen,
    DmSettingOutput, DmSettingFullscreen, DmSettingResetController,
    DmSettingFpsTarget, DmSettingDynamicFps, DmSettingStats, DmSettingFrameGraph, DmSettingAspect
};
typedef DmMacSettings (^DmSettingsSnapshot)(void);
typedef void (^DmSettingsChange)(DmMacSetting setting, NSInteger value);
@interface DmSettingsPanel : NSWindowController <NSWindowDelegate>
- (instancetype)initWithParent:(NSWindow *)parent snapshot:(DmSettingsSnapshot)snapshot change:(DmSettingsChange)change;
- (void)showPanel;
- (void)togglePanel;   // key 1 / F1 on the game window (front_shortcuts.h)
- (void)refresh;
@end
@interface DmPerformanceHUD : NSView
- (instancetype)initWithSnapshot:(DmSettingsSnapshot)snapshot;
- (void)refresh;
@end
