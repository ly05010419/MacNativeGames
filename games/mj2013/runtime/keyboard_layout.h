/* A held Mac key as the console sees it (input.c; tests/keyboard_layout_probe.c): its USB HID usage on the console's
   keyboard (the game polls a Windows keyboard, which its port takes from hid's keyboard), and, for the keys of the
   Zelda-line layout, a controller button or stick (J/K/I/U = A/B/X/Y, Q/E = L/R, Option/Shift = ZL/ZR, Return = +,
   Tab = -, arrows = d-pad, WASD = left stick). */
#pragma once
#include <stdint.h>
#include <string.h>

typedef struct {
    uint64_t buttons;               /* HidNpadButton bits */
    int32_t lx, ly, rx, ry;         /* sticks, -32767..32767 */
    uint64_t keys[4];               /* HID usages held (bit usage) */
    uint64_t modifiers;             /* HidKeyboardModifier bits */
    int touch; float touch_x, touch_y;
    float mouse_x, mouse_y; int mouse_buttons, mouse_connected;
} HbInput;

enum { PAD_A = 1 << 0, PAD_B = 1 << 1, PAD_X = 1 << 2, PAD_Y = 1 << 3, PAD_L = 1 << 6, PAD_R = 1 << 7, PAD_ZL = 1 << 8,
       PAD_ZR = 1 << 9, PAD_PLUS = 1 << 10, PAD_MINUS = 1 << 11, PAD_LEFT = 1 << 12, PAD_UP = 1 << 13, PAD_RIGHT = 1 << 14,
       PAD_DOWN = 1 << 15 };
enum { MOD_CONTROL = 1 << 0, MOD_SHIFT = 1 << 1, MOD_LEFT_ALT = 1 << 2, MOD_RIGHT_ALT = 1 << 3, MOD_GUI = 1 << 4 };
#define STICK_MAX 32767

/* Mac virtual key code -> USB HID usage (0 = none). */
static inline unsigned keyboard_hid_usage(unsigned code) {
    static const uint8_t table[128] = {
        [0] = 4, [11] = 5, [8] = 6, [2] = 7, [14] = 8, [3] = 9, [5] = 10, [4] = 11, [34] = 12, [38] = 13, [40] = 14, [37] = 15,
        [46] = 16, [45] = 17, [31] = 18, [35] = 19, [12] = 20, [15] = 21, [1] = 22, [17] = 23, [32] = 24, [9] = 25, [13] = 26,
        [7] = 27, [16] = 28, [6] = 29,                                               /* a..z */
        [18] = 30, [19] = 31, [20] = 32, [21] = 33, [23] = 34, [22] = 35, [26] = 36, [28] = 37, [25] = 38, [29] = 39,   /* 1..9, 0 */
        [36] = 40, [53] = 41, [51] = 42, [48] = 43, [49] = 44, [27] = 45, [24] = 46, [33] = 47, [30] = 48, [42] = 49,
        [41] = 51, [39] = 52, [50] = 53, [43] = 54, [47] = 55, [44] = 56, [57] = 57,
        [122] = 58, [120] = 59, [99] = 60, [118] = 61, [96] = 62, [97] = 63, [98] = 64, [100] = 65, [101] = 66, [109] = 67,
        [103] = 68, [111] = 69,                                                      /* F1..F12 */
        [115] = 74, [116] = 75, [117] = 76, [119] = 77, [121] = 78, [124] = 79, [123] = 80, [125] = 81, [126] = 82,
        [59] = 224, [56] = 225, [58] = 226, [55] = 227, [62] = 228, [60] = 229, [61] = 230,
    };
    return code < 128 ? table[code] : 0;
}
static inline void keyboard_layout_apply(unsigned code, HbInput *o) {
    unsigned usage = keyboard_hid_usage(code);
    if (usage) o->keys[usage / 64] |= 1ull << (usage % 64);
    switch (code) {
    case 59: case 62: o->modifiers |= MOD_CONTROL; break;
    case 56: case 60: o->modifiers |= MOD_SHIFT; o->buttons |= PAD_ZR; break;
    case 58: o->modifiers |= MOD_LEFT_ALT; o->buttons |= PAD_ZL; break;
    case 61: o->modifiers |= MOD_RIGHT_ALT; break;
    case 55: o->modifiers |= MOD_GUI; break;
    case 38: o->buttons |= PAD_A; break;   case 40: o->buttons |= PAD_B; break;
    case 34: o->buttons |= PAD_X; break;   case 32: o->buttons |= PAD_Y; break;
    case 12: o->buttons |= PAD_L; break;   case 14: o->buttons |= PAD_R; break;
    case 36: o->buttons |= PAD_PLUS; break; case 48: o->buttons |= PAD_MINUS; break;
    case 123: o->buttons |= PAD_LEFT; break; case 124: o->buttons |= PAD_RIGHT; break;
    case 125: o->buttons |= PAD_DOWN; break; case 126: o->buttons |= PAD_UP; break;
    case 13: o->ly = STICK_MAX; break;     case 1: o->ly = -STICK_MAX; break;
    case 0: o->lx = -STICK_MAX; break;     case 2: o->lx = STICK_MAX; break;
    }
}
void hb_input_snapshot(HbInput *o);   /* input.c */
void runtime_input_mouse(float x, float y, int buttons);
