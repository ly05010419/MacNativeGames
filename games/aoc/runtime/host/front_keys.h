// The game window's keyboard as controller input (front_mac.m on_key), kept pure so the settings UI test drives it: which keys
// hold which buttons, and letting go of all of them when the window stops receiving keys (the settings panel opened, focus lost),
// so that a key whose release reaches another window is not left held.
#pragma once
#include <stdint.h>
#include <string.h>
#include "dm_input.h"
typedef struct { uint64_t buttons; int w, a, s, d; } DmKeyboard;
static inline int dm_key_bit(unsigned short code) {
    switch (code) {
        case 38: return 0;  case 40: return 1;  case 32: return 2;  case 34: return 3;    // J K U I → A B X Y
        case 3: return 4;   case 5: return 5;                                            // F G → press stick
        case 12: return 6;  case 14: return 7;  case 6: return 8;   case 8: return 9;    // Q E Z C → L R ZL ZR
        case 36: return 10; case 48: return 11;                                          // Return Tab → + −
        case 123: return 12; case 126: return 13; case 124: return 14; case 125: return 15; // ← ↑ → ↓
    }
    return -1;
}
static inline void dm_keyboard_key(DmKeyboard *k, unsigned short code, int down) {
    switch (code) { case 13: k->w = down; break; case 0: k->a = down; break; case 1: k->s = down; break; case 2: k->d = down; break; }
    int b = dm_key_bit(code);
    uint64_t m = b >= 0 ? 1ull << b : code == 49 ? DM_BTN_SHAKE : 0;   // Space: shaken while held
    k->buttons = down ? (k->buttons | m) : (k->buttons & ~m);
}
static inline void dm_keyboard_release(DmKeyboard *k) { memset(k, 0, sizeof *k); }
static inline int dm_keyboard_idle(const DmKeyboard *k) { return !k->buttons && !k->w && !k->a && !k->s && !k->d; }
