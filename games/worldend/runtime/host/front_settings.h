// Native macOS settings and compact shortcut HUD. All callbacks run on the AppKit thread.
#pragma once
#import <Cocoa/Cocoa.h>
typedef struct {
    int face_layout, swap_ab, start_fullscreen, output_preset, show_hints, fullscreen;
    NSUInteger output_width, output_height, controllers;
} DmMacSettings;
typedef NS_ENUM(NSInteger, DmMacSetting) {
    DmSettingFaceLayout, DmSettingSwapAB, DmSettingStartFullscreen,
    DmSettingOutput, DmSettingHints, DmSettingFullscreen, DmSettingResetController
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
NSString *dm_settings_text(NSString *chinese, NSString *english);
