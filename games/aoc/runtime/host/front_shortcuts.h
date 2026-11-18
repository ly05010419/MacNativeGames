// Shared by the game-window event handler and native UI tests.
// Key 1 (and F1) opens and closes the settings panel, as in Tears of the Kingdom; the panel holds every setting the number keys
// 2 to 7 used to change, so those keys are no longer shortcuts. A held key repeats: only the first press acts.
#pragma once
#import "front_settings.h"
typedef NS_ENUM(NSInteger, DmShortcutAction) {
    DmShortcutNone, DmShortcutSettings
};
enum { DM_KEY_1 = 18, DM_KEY_F1 = 122, DM_KEY_ESC = 53 };
static inline int dm_mac_is_shortcut(unsigned short key) { return key == DM_KEY_1 || key == DM_KEY_F1; }
static inline DmShortcutAction dm_mac_shortcut(unsigned short key) {
    return dm_mac_is_shortcut(key) ? DmShortcutSettings : DmShortcutNone;
}
// Where a key event on the game window goes. ⌘ presses go to the menu (a release while ⌘ is held still reaches the game, or the
// key would stay pressed). A shortcut key never reaches the game: its first press opens or closes the panel, its repeats and
// release are swallowed. Other repeats are swallowed too (the game sees one press until the release).
typedef NS_ENUM(NSInteger, DmKeyRoute) { DmKeyToGame, DmKeyToSystem, DmKeySwallow, DmKeyTogglePanel };
static inline DmKeyRoute dm_mac_game_key(unsigned short key, NSEventModifierFlags flags, BOOL down, BOOL repeat) {
    if (down && (flags & NSEventModifierFlagCommand)) return DmKeyToSystem;
    if (!(flags & (NSEventModifierFlagCommand | NSEventModifierFlagControl | NSEventModifierFlagOption)) && dm_mac_is_shortcut(key))
        return down && !repeat ? DmKeyTogglePanel : DmKeySwallow;
    return repeat ? DmKeySwallow : DmKeyToGame;
}
// A key press on the settings panel's window: 1 or F1 closes it (its repeats do nothing), anything else goes to the panel's
// controls (Esc closes it through cancelOperation:).
typedef NS_ENUM(NSInteger, DmPanelKey) { DmPanelKeyPass, DmPanelKeySwallow, DmPanelKeyClose };
static inline DmPanelKey dm_mac_panel_key(unsigned short key, NSEventModifierFlags flags, BOOL repeat) {
    if ((flags & (NSEventModifierFlagCommand | NSEventModifierFlagControl | NSEventModifierFlagOption)) || !dm_mac_is_shortcut(key))
        return DmPanelKeyPass;
    return repeat ? DmPanelKeySwallow : DmPanelKeyClose;
}
