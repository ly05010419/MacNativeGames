/* Keyboard-to-controller mapping. Codes are USB HID keyboard usages, also used by
   GCKeyboard; never pass AppKit virtual key codes here. Each player owns a map
   and held/edge state. Platform adapters handle device identity and persistence. */
#pragma once
#include <stdint.h>
#include <string.h>

enum { KEYBOARD_ACTIONS = 24, KEYBOARD_KEYS = 256 };
typedef struct { uint16_t key[KEYBOARD_ACTIONS]; } KeyboardMap;
typedef struct { uint8_t held[KEYBOARD_KEYS]; uint16_t pending; } KeyboardStateMap;
typedef struct { uint16_t buttons; int32_t lx, ly, rx, ry; } KeyboardOutput;
static inline void keyboard_map_default(KeyboardMap *map) {
    /* A B X Y L3 R3 L R ZL ZR + - dpad L U R D; left stick L R U D; right L R U D.
       Preserve the existing desktop keyboard layout, expressed as USB usages. */
    const uint16_t keys[KEYBOARD_ACTIONS] = {
        13,14,12,24,29,6,20,8,226,225,40,43,9,23,11,10,
        4,7,26,22,80,79,82,81
    };
    memcpy(map->key, keys, sizeof keys);
}
static inline int keyboard_map_set(KeyboardMap *map, int action, int key) {
    if (action < 0 || action >= KEYBOARD_ACTIONS || key < 0 || key >= KEYBOARD_KEYS) return 0;
    map->key[action] = (uint16_t)key; /* 0 = unbound; shared keys intentionally allowed */
    return 1;
}
static inline void keyboard_state_clear(KeyboardStateMap *state) { memset(state, 0, sizeof *state); }
static inline void keyboard_state_event(KeyboardStateMap *state, const KeyboardMap *map, int key, int down) {
    if (key <= 0 || key >= KEYBOARD_KEYS) return;
    if (down && !state->held[key])
        for (int i = 0; i < 16; i++) if (map->key[i] == key) state->pending |= (uint16_t)(1u << i);
    state->held[key] = !!down;
}
static inline KeyboardOutput keyboard_state_read(KeyboardStateMap *state, const KeyboardMap *map) {
    int active[KEYBOARD_ACTIONS];
    for (int i = 0; i < KEYBOARD_ACTIONS; i++)
        active[i] = map->key[i] > 0 && map->key[i] < KEYBOARD_KEYS && state->held[map->key[i]];
    KeyboardOutput out = { .buttons = state->pending };
    state->pending = 0;
    for (int i = 0; i < 16; i++) if (active[i]) out.buttons |= (uint16_t)(1u << i);
    out.lx = 32767 * (active[17] - active[16]); out.ly = 32767 * (active[18] - active[19]);
    out.rx = 32767 * (active[21] - active[20]); out.ry = 32767 * (active[22] - active[23]);
    return out;
}
