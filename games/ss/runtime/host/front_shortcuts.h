// Shared by the game-window event handler and native UI tests.
#pragma once
#import "front_settings.h"
#include "hud_settings.h"
typedef NS_ENUM(NSInteger, DmShortcutAction) {
    DmShortcutNone, DmShortcutSettings, DmShortcutChange
};
// F1 and the number keys 1-6 (the corner box); 7 and AoC's 2 / 4 meanings are gone. Taken from the game while pressed, repeats included.
static inline int dm_mac_is_shortcut(unsigned short key) {
    switch(key) { case 122: case 18: case 19: case 20: case 21: case 23: case 22: return 1; default:return 0; }
}
// repeat: a held key's auto-repeat, which never acts (one press, one change).
static inline DmShortcutAction dm_mac_shortcut(unsigned short key, int repeat, DmMacSettings s,
                                              DmMacSetting *setting, NSInteger *value) {
    (void)s;
    if (repeat) return DmShortcutNone;
    if (key == 122) return DmShortcutSettings; // F1
    int bit;
    switch (key) {
        case 18: bit = 0; break;            // 1  the box
        case 19: bit = HUD_FPS; break;      // 2
        case 20: bit = HUD_SHADERS; break;  // 3
        case 21: bit = HUD_TIMING; break;   // 4
        case 23: bit = HUD_GRAPH; break;    // 5
        case 22: bit = HUD_KEYS; break;     // 6
        default: return DmShortcutNone;
    }
    *setting = DmSettingHudKey; *value = bit;
    return DmShortcutChange;
}
