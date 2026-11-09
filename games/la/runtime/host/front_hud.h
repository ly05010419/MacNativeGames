// The corner box: one box in the window's top-left corner, after Tears of the Kingdom's compact HUD, holding the lines the
// player picks (hud_settings.h): FPS, the on-demand shader / pipeline counters, frame timing, the frame-time graph and the
// shortcut list. AppKit thread only.
#pragma once
#import "front_settings.h"
// The statistics text of the given lines (HUD_FPS, HUD_SHADERS, HUD_TIMING); detail adds every counter (the settings window).
NSString *dm_stats_text(DmMacSettings s, int lines, BOOL detail);
@interface DmCornerHUD : NSView
- (instancetype)initWithSnapshot:(DmSettingsSnapshot)snapshot;
- (void)refresh;
@end
