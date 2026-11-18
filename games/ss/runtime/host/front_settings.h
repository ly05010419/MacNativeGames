// Native macOS settings window and the types the corner box (front_hud.h) shares with it. All callbacks run on the AppKit thread.
#pragma once
#import <Cocoa/Cocoa.h>
#include "../gfx/nvn/aoc_stats.h"
typedef struct {
    int face_layout, swap_ab, start_fullscreen, output_preset, fullscreen;
    NSUInteger output_width, output_height, controllers;
    int fps_target, dynamic_fps, screen_fps;
    int hud_on, hud_lines;   // the corner box as shown this run (hud_settings.h dm_hud_effective)
    AocStats performance;
} DmMacSettings;
typedef NS_ENUM(NSInteger, DmMacSetting) {
    DmSettingFaceLayout, DmSettingSwapAB, DmSettingStartFullscreen,
    DmSettingOutput, DmSettingFullscreen, DmSettingResetController,
    DmSettingFpsTarget, DmSettingDynamicFps,
    DmSettingHudOn, DmSettingHudKey, DmSettingHudLine   // value: 0/1; a key's line bit (0: the box); line bit << 1 | 0/1 (hud_settings.h)
};
typedef DmMacSettings (^DmSettingsSnapshot)(void);
typedef void (^DmSettingsChange)(DmMacSetting setting, NSInteger value);
@interface DmSettingsPanel : NSWindowController <NSWindowDelegate>
- (instancetype)initWithParent:(NSWindow *)parent snapshot:(DmSettingsSnapshot)snapshot change:(DmSettingsChange)change;
- (void)showPanel;
- (void)refresh;
@property (copy) NSArray<NSString *> *shaderLists;   // pso_user.tsv, then the bundled pso_manifest.tsv (the export, pso_export.h)
- (NSString *)finishExport:(NSModalResponse)response url:(NSURL *)url;   // the save panel's answer: nil when cancelled (nothing written), else the message shown
@end
