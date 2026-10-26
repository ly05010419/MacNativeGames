/* Controller button layout: each physical button of a controller (named by its position, the same on a DualSense, an Xbox
   and a Nintendo controller: runtime/gamepad_layout.h gamepad_face_positions) drives one Switch Pro Controller button, or
   none. The default is the Switch's own layout by position: east -> A, south -> B, north -> X, west -> Y (issue #10); the
   other buttons i < 16 are FullKey bit i. The touchpad click (DualSense, DualShock 4) is physical button 16 and drives
   nothing by default. The mouse's buttons (Mac, Keyboard + Mouse mode: runtime/gamepad.m) are physical buttons 17-21:
   left attacks (Y), right draws the bow (ZR: a left click while it is held shoots instead of
   attacking), middle targets (ZL), the two side buttons drive nothing.
   Keyboard and mouse bindings use "Switch action = key_<name> mouse_<button>"; a blank value unbinds the action.
   Controller remapping uses "physical = switch [mode]"; '#' starts a comment; names are case-insensitive;
   "none" leaves a button unused; lines not given keep the default. The optional mode says how long the Switch button is
   held: "hold" (default) while the physical button is down, "tap" for 400 ms after each press (tap again to keep
   running), "toggle" on and off at each press (cleared when the stick returns to center), "aim <fire button>" while
   the button is down, except while the fire button is also down (hold L2 to draw the bow, press R2 to shoot it; the
   fire button's own line applies only while the aim button is up). A mode byte keeps the kind in its low 3 bits and,
   for aim, the fire button's index in its high 5 bits.
   Pure C: app/launcher.m checks the file before the game starts (an alert names the bad line), runtime/gamepad.m
   applies it, tests/test_controller_map.c covers the parser. */
#ifndef TOTK_CONTROLLER_MAP_H
#define TOTK_CONTROLLER_MAP_H
#include <stdint.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

enum { CONTROLLER_BUTTONS = 22, CONTROLLER_SWITCH_BUTTONS = 16, CONTROLLER_MOUSE_FIRST = 17, CONTROLLER_MOUSE_BUTTONS = 5 };
static const char *const controller_physical_names[CONTROLLER_BUTTONS] = {
    "south", "east", "west", "north", "l3", "r3", "l1", "r1", "l2", "r2", "menu", "view", "left", "up", "right", "down",
    "touchpad", "mouse_left", "mouse_right", "mouse_middle", "mouse4", "mouse5"};
static const char *const controller_switch_names[CONTROLLER_SWITCH_BUTTONS] = {
    "a", "b", "x", "y", "lstick", "rstick", "l", "r", "zl", "zr", "plus", "minus", "left", "up", "right", "down"};

enum { KEYBOARD_KEYS = 128, KEYBOARD_STICK_FIRST = 16 };
static const char *const keyboard_names[KEYBOARD_KEYS] = {
    [0]="a", [1]="s", [2]="d", [3]="f", [4]="h", [5]="g", [6]="z", [7]="x",
    [8]="c", [9]="v", [11]="b", [12]="q", [13]="w", [14]="e", [15]="r", [16]="y", [17]="t",
    [18]="1", [19]="2", [20]="3", [21]="4", [22]="6", [23]="5", [24]="equal", [25]="9",
    [26]="7", [27]="minus", [28]="8", [29]="0", [30]="right_bracket", [31]="o", [32]="u",
    [33]="left_bracket", [34]="i", [35]="p", [36]="return", [37]="l", [38]="j", [39]="quote",
    [40]="k", [41]="semicolon", [42]="backslash", [43]="comma", [44]="slash", [45]="n", [46]="m",
    [47]="period", [48]="tab", [49]="space", [50]="grave", [51]="backspace", [53]="escape",
    [56]="shift", [58]="option", [123]="left", [124]="right", [125]="down", [126]="up"
};
static const char *const keyboard_stick_names[8] = {
    "left_stick_left", "left_stick_right", "left_stick_up", "left_stick_down",
    "right_stick_left", "right_stick_right", "right_stick_up", "right_stick_down"
};
static inline void keyboard_map_default(uint32_t map[KEYBOARD_KEYS]) {
    memset(map, 0, KEYBOARD_KEYS * sizeof *map);
    map[38]=1; map[40]=map[49]=2; map[34]=4; map[32]=8; map[6]=16; map[8]=32;
    map[12]=64; map[14]=128; map[58]=256; map[56]=512; map[36]=1024; map[48]=map[53]=2048;
    map[3]=4096; map[17]=8192; map[4]=16384; map[5]=32768;
    map[0]=1u<<16; map[2]=1u<<17; map[13]=1u<<18; map[1]=1u<<19;
    map[123]=1u<<20; map[124]=1u<<21; map[126]=1u<<22; map[125]=1u<<23;
}

static const char controls_default_text[] =
    "a = key_j\n"
    "b = key_k key_space\n"
    "x = key_i\n"
    "y = key_u mouse_left\n"
    "lstick = key_z\n"
    "rstick = key_c\n"
    "l = key_q\n"
    "r = key_e\n"
    "zl = key_option mouse_middle\n"
    "zr = key_shift mouse_right\n"
    "plus = key_return\n"
    "minus = key_tab key_escape\n"
    "left = key_f\n"
    "up = key_t\n"
    "right = key_h\n"
    "down = key_g\n"
    "left_stick_left = key_a\n"
    "left_stick_right = key_d\n"
    "left_stick_up = key_w\n"
    "left_stick_down = key_s\n"
    "right_stick_left = key_left\n"
    "right_stick_right = key_right\n"
    "right_stick_up = key_up\n"
    "right_stick_down = key_down\n";

static inline int controls_file_create(const char *path) {
    FILE *f = fopen(path, "wx");
    if (!f) return errno == EEXIST ? 0 : -1;
    int written = fputs(controls_default_text, f) != EOF;
    int closed = fclose(f) == 0;
    return written && closed ? 0 : -1;
}

static inline int keyboard_key_reserved(int key) {
    return (key >= 18 && key <= 23) || key == 26;
}

enum { CONTROLLER_HOLD = 0, CONTROLLER_TAP = 1, CONTROLLER_TOGGLE = 2, CONTROLLER_AIM = 3 };
static inline int controller_mode_kind(uint8_t mode) { return mode & 7; }
static inline int controller_mode_fire(uint8_t mode) { return mode >> 3; }

static inline void controller_map_default(uint16_t map[CONTROLLER_BUTTONS], uint8_t modes[CONTROLLER_BUTTONS]) {
    for (int i = 0; i < CONTROLLER_BUTTONS; i++) { map[i] = i < CONTROLLER_SWITCH_BUTTONS ? (uint16_t)(1u << i) : 0; modes[i] = CONTROLLER_HOLD; }
    map[0] = 1u << 1; map[1] = 1u << 0; map[2] = 1u << 3; map[3] = 1u << 2;   /* south B, east A, west Y, north X */
    enum { LEFT = CONTROLLER_MOUSE_FIRST, RIGHT, MIDDLE };
    map[LEFT] = 1u << 3;                                                    /* attack (Y) */
    map[RIGHT] = 1u << 9; modes[RIGHT] = (uint8_t)(CONTROLLER_AIM | LEFT << 3);   /* draw the bow (ZR); a left click shoots */
    map[MIDDLE] = 1u << 8;                                                  /* target (ZL) */
}

/* Index of name [s, s + n) in the count names, or -1. */
static inline int controller_name_index(const char *const names[], int count, const char *s, size_t n) {
    for (int i = 0; i < count; i++)
        if (names[i] && strlen(names[i]) == n && !strncasecmp(names[i], s, n)) return i;
    return -1;
}

/* Applies a layout file's text to map and modes (start from controller_map_default). Returns 0, or the 1-based number of
   the first bad line with *error naming the problem; all arrays are then unchanged. */
static inline int controller_map_parse(const char *text, uint16_t map[CONTROLLER_BUTTONS],
                                       uint8_t modes[CONTROLLER_BUTTONS], uint32_t keyboard[KEYBOARD_KEYS], const char **error) {
    uint32_t nextKeyboard[KEYBOARD_KEYS]; memcpy(nextKeyboard, keyboard, sizeof nextKeyboard);
    uint16_t next[CONTROLLER_BUTTONS]; uint8_t nextMode[CONTROLLER_BUTTONS];
    memcpy(next, map, sizeof next); memcpy(nextMode, modes, sizeof nextMode);
    int line = 0, content = 0;
    for (const char *p = text; p && *p;) {
        line++;
        const char *end = strchr(p, '\n'), *stop = end ? end : p + strlen(p), *hash = memchr(p, '#', (size_t)(stop - p));
        if (hash) stop = hash;
        while (p < stop && (*p == ' ' || *p == '\t' || *p == '\r')) p++;
        while (stop > p && (stop[-1] == ' ' || stop[-1] == '\t' || stop[-1] == '\r')) stop--;
        if (p < stop) {
            content = 1;
            const char *eq = memchr(p, '=', (size_t)(stop - p));
            if (!eq) { *error = "expected \"button = Switch button [hold|tap|toggle]\""; return line; }
            const char *key_end = eq, *value = eq + 1;
            while (key_end > p && (key_end[-1] == ' ' || key_end[-1] == '\t')) key_end--;
            while (value < stop && (*value == ' ' || *value == '\t')) value++;
            const char *value_end = value, *mode = NULL, *mode_end = NULL;
            while (value_end < stop && *value_end != ' ' && *value_end != '\t') value_end++;
            for (const char *q = value_end; q < stop; q++) if (*q != ' ' && *q != '\t') { mode = q; break; }
            if (mode) { mode_end = mode; while (mode_end < stop && *mode_end != ' ' && *mode_end != '\t') mode_end++; }
            int physical = controller_name_index(controller_physical_names, CONTROLLER_MOUSE_FIRST, p, (size_t)(key_end - p));
            int action = controller_name_index(controller_switch_names, CONTROLLER_SWITCH_BUTTONS, p, (size_t)(key_end - p));
            int stick = controller_name_index(keyboard_stick_names, 8, p, (size_t)(key_end - p));
            int binding = value == stop || (stop - value >= 4 && !strncasecmp(value, "key_", 4)) ||
                          (stop - value >= 5 && !strncasecmp(value, "mouse", 5));
            if (binding || physical < 0) {
                if (action < 0 && stick < 0) { *error = "unknown Switch action"; return line; }
                uint32_t bit = 1u << (action >= 0 ? action : KEYBOARD_STICK_FIRST + stick);
                for (int i = 0; i < KEYBOARD_KEYS; i++) nextKeyboard[i] &= ~bit;
                if (action >= 0) for (int i = CONTROLLER_MOUSE_FIRST; i < CONTROLLER_BUTTONS; i++) {
                    if (next[i] & bit) { next[i] = 0; nextMode[i] = CONTROLLER_HOLD; }
                }
                for (const char *q = value; q < stop;) {
                    const char *word = q; while (q < stop && *q != ' ' && *q != '\t') q++;
                    size_t n = (size_t)(q - word);
                    if (n >= 4 && !strncasecmp(word, "key_", 4)) {
                        int key = controller_name_index(keyboard_names, KEYBOARD_KEYS, word + 4, n - 4);
                        if (key < 0) { *error = "unknown keyboard key"; return line; }
                        if (keyboard_key_reserved(key)) { *error = "keys 1-7 are reserved for game shortcuts"; return line; }
                        nextKeyboard[key] = bit;
                    } else {
                        int mouse = controller_name_index(controller_physical_names + CONTROLLER_MOUSE_FIRST,
                                                          CONTROLLER_MOUSE_BUTTONS, word, n);
                        if (mouse < 0) { *error = "expected key_<name> or mouse_left/right/middle/mouse4/mouse5"; return line; }
                        if (action < 0) { *error = "mouse buttons need a Switch button action"; return line; }
                        next[CONTROLLER_MOUSE_FIRST + mouse] = (uint16_t)bit;
                        nextMode[CONTROLLER_MOUSE_FIRST + mouse] = CONTROLLER_HOLD;
                    }
                    while (q < stop && (*q == ' ' || *q == '\t')) q++;
                }
                p = end ? end + 1 : NULL;
                continue;
            }
            size_t n = (size_t)(value_end - value);
            int target = n == 4 && !strncasecmp(value, "none", 4) ? CONTROLLER_SWITCH_BUTTONS : controller_name_index(controller_switch_names, CONTROLLER_SWITCH_BUTTONS, value, n);
            if (target < 0) { *error = "unknown Switch button (a b x y l r zl zr plus minus lstick rstick up down left right none)"; return line; }
            uint8_t how = CONTROLLER_HOLD;
            if (mode) {
                size_t m = (size_t)(mode_end - mode);
                if (m == 4 && !strncasecmp(mode, "hold", 4)) how = CONTROLLER_HOLD;
                else if (m == 3 && !strncasecmp(mode, "tap", 3)) how = CONTROLLER_TAP;
                else if (m == 6 && !strncasecmp(mode, "toggle", 6)) how = CONTROLLER_TOGGLE;
                else if (m == 3 && !strncasecmp(mode, "aim", 3)) how = CONTROLLER_AIM;
                else { *error = "unknown mode (hold tap toggle aim)"; return line; }
                const char *extra = mode_end; while (extra < stop && (*extra == ' ' || *extra == '\t')) extra++;
                if (how == CONTROLLER_AIM) {
                    const char *fire_end = extra; while (fire_end < stop && *fire_end != ' ' && *fire_end != '\t') fire_end++;
                    int fire = extra < stop ? controller_name_index(controller_physical_names, CONTROLLER_BUTTONS, extra, (size_t)(fire_end - extra)) : -1;
                    if (fire < 0) { *error = "aim needs a fire button, e.g. \"l2 = zr aim r2\""; return line; }
                    if (fire == physical) { *error = "the fire button must be another button"; return line; }
                    how = (uint8_t)(CONTROLLER_AIM | fire << 3);
                    extra = fire_end; while (extra < stop && (*extra == ' ' || *extra == '\t')) extra++;
                }
                if (extra < stop) { *error = "too many words after the Switch button"; return line; }
                if (target == CONTROLLER_SWITCH_BUTTONS && how != CONTROLLER_HOLD) { *error = "none takes no mode"; return line; }
            }
            next[physical] = target == CONTROLLER_SWITCH_BUTTONS ? 0 : (uint16_t)(1u << target);
            nextMode[physical] = how;
        }
        p = end ? end + 1 : NULL;
    }
    if (!content) memset(nextKeyboard, 0, sizeof nextKeyboard);
    if (next[CONTROLLER_MOUSE_FIRST] == (1u << 3) && next[CONTROLLER_MOUSE_FIRST + 1] == (1u << 9))
        nextMode[CONTROLLER_MOUSE_FIRST + 1] = (uint8_t)(CONTROLLER_AIM | CONTROLLER_MOUSE_FIRST << 3);
    memcpy(map, next, sizeof next); memcpy(modes, nextMode, sizeof nextMode);
    memcpy(keyboard, nextKeyboard, sizeof nextKeyboard);
    return 0;
}

/* The Switch buttons held, from the physical buttons held (bit i = physical button i). */
static inline uint64_t controller_map_apply(const uint16_t map[CONTROLLER_BUTTONS], uint64_t physical) {
    uint64_t buttons = 0;
    for (int i = 0; i < CONTROLLER_BUTTONS; i++)
        if (physical >> i & 1) buttons |= map[i];
    return buttons;
}
#endif
