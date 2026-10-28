// Shared by the game-window event handler and native UI tests.
#pragma once
#import "front_settings.h"
typedef NS_ENUM(NSInteger, DmShortcutAction) {
    DmShortcutNone, DmShortcutSettings, DmShortcutChange
};
static inline int dm_mac_is_shortcut(unsigned short key) {
    switch(key) { case 122: case 18: case 19: case 20: case 21: case 23: case 22: case 26: return 1; default:return 0; }
}
static inline DmShortcutAction dm_mac_shortcut(unsigned short key, DmMacSettings s,
                                              DmMacSetting *setting, NSInteger *value) {
    if (key == 122) return DmShortcutSettings; // F1
    switch (key) {
        case 18: *setting=DmSettingHints; *value=!s.show_hints; break; // 1
        case 19: *setting=DmSettingFpsTarget; // 2: Original 30 <-> 60
            *value=s.fps_target==60?0:60; break;
        case 20: *setting=DmSettingOutput; *value=(s.output_preset+1)%5; break; // 3
        case 21: *setting=DmSettingDynamicFps; *value=!s.dynamic_fps; break; // 4
        case 23: *setting=DmSettingStats; *value=(s.stats_mode+1)%3; break; // 5
        case 22: *setting=DmSettingFaceLayout; *value=(s.face_layout+1)%3; break; // 6
        case 26: *setting=DmSettingFrameGraph; *value=!s.frame_graph; break; // 7
        default: return DmShortcutNone;
    }
    return DmShortcutChange;
}
