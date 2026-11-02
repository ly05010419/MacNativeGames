/* Keyboard and mouse layout (Mac; adapted from games/totk/runtime/keyboard_map.h): each game action -- a Switch button, or
   one direction of the left stick (moving) or the right stick (the camera) -- is driven by up to two inputs. An input is
   a Mac key (virtual key code 0-127: the key's physical position, named after a US layout) or a mouse button (128 left,
   129 right, 130 middle (the wheel click), 131 and 132 the side buttons; runtime/frontend.m sends them while the game
   window has the keyboard and the settings panel is closed). One input drives at most one action.
   The default (reported in play, PR #1305: the layout must suit keyboard and mouse players) keeps the left hand on the keyboard:
   W A S D walk, Shift (held) runs and Esc backs out (B), Space or the left button talks, uses the tool and confirms (A),
   E or the right button picks up (Y), Tab or I opens the pockets (X), Q the NookPhone (ZL), R the reactions (ZR), Z and C
   are L and R, Control and V the stick presses, M or Return the + button, Backspace the - button, 2 3 4 5 the d-pad
   (left, up, right, down; the wheel click is d-pad up, the tool ring, and the side buttons d-pad left and right), the
   arrow keys the camera. Animal Crossing has no aiming and no camera to turn with a mouse, so the mouse only clicks.
   Key 1 opens the settings panel (runtime/input.c) and is never part of a layout; Command, Caps Lock and fn are not
   offered either (Command-Q and Command-Tab stay the system's).
   A layout file (keyboard.txt next to controls.txt, written by the settings panel) has one "action = input [input]"
   line per action; "none" leaves an action without input; '#' starts a comment; names are case-insensitive; actions not
   given keep the default, minus the inputs the file gives to other actions.
   Pure C: runtime/input.c applies it, runtime/settings_panel.h edits it, tests/test_acnh_input.py covers it. */
#ifndef ACNH_KEYBOARD_MAP_H
#define ACNH_KEYBOARD_MAP_H
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

enum {
    KEYMAP_KEYS = 128, KEYMAP_MOUSE_FIRST = 128, KEYMAP_MOUSE_BUTTONS = 5, KEYMAP_INPUTS = 133, KEYMAP_NONE = 255,
    KEYMAP_SLOTS = 2, KEYMAP_SETTINGS_KEY = 18,
    /* Actions 0-15 are FullKey button bits; the rest are stick directions. */
    KEYMAP_A = 0, KEYMAP_B, KEYMAP_X, KEYMAP_Y, KEYMAP_LSTICK, KEYMAP_RSTICK, KEYMAP_L, KEYMAP_R, KEYMAP_ZL, KEYMAP_ZR,
    KEYMAP_PLUS, KEYMAP_MINUS, KEYMAP_DLEFT, KEYMAP_DUP, KEYMAP_DRIGHT, KEYMAP_DDOWN, KEYMAP_SWITCH_BUTTONS,
    KEYMAP_MOVE_UP = KEYMAP_SWITCH_BUTTONS, KEYMAP_MOVE_DOWN, KEYMAP_MOVE_LEFT, KEYMAP_MOVE_RIGHT,
    KEYMAP_CAMERA_UP, KEYMAP_CAMERA_DOWN, KEYMAP_CAMERA_LEFT, KEYMAP_CAMERA_RIGHT, KEYMAP_ACTIONS
};

typedef struct KeyMap { uint8_t slot[KEYMAP_ACTIONS][KEYMAP_SLOTS]; } KeyMap;

static inline const char *keymap_action_name(int action) {
    static const char *const names[KEYMAP_ACTIONS] = {
        "a", "b", "x", "y", "lstick", "rstick", "l", "r", "zl", "zr", "plus", "minus", "left", "up", "right", "down",
        "move_up", "move_down", "move_left", "move_right", "camera_up", "camera_down", "camera_left", "camera_right"};
    return action >= 0 && action < KEYMAP_ACTIONS ? names[action] : NULL;
}

/* Every input a layout can name: its file name and the short English label the settings panel shows. */
typedef struct { uint8_t input; const char *name, *label; } KeyMapInput;
static inline const KeyMapInput *keymap_inputs(int *count) {
    static const KeyMapInput inputs[] = {
        {0, "a", "A"}, {11, "b", "B"}, {8, "c", "C"}, {2, "d", "D"}, {14, "e", "E"}, {3, "f", "F"}, {5, "g", "G"},
        {4, "h", "H"}, {34, "i", "I"}, {38, "j", "J"}, {40, "k", "K"}, {37, "l", "L"}, {46, "m", "M"}, {45, "n", "N"},
        {31, "o", "O"}, {35, "p", "P"}, {12, "q", "Q"}, {15, "r", "R"}, {1, "s", "S"}, {17, "t", "T"}, {32, "u", "U"},
        {9, "v", "V"}, {13, "w", "W"}, {7, "x", "X"}, {16, "y", "Y"}, {6, "z", "Z"},
        {29, "0", "0"}, {18, "1", "1"}, {19, "2", "2"}, {20, "3", "3"}, {21, "4", "4"}, {23, "5", "5"}, {22, "6", "6"},
        {26, "7", "7"}, {28, "8", "8"}, {25, "9", "9"},
        {49, "space", "Space"}, {36, "return", "Return"}, {53, "escape", "Esc"}, {48, "tab", "Tab"},
        {51, "backspace", "Backspace"}, {117, "forward_delete", "Delete"},
        {56, "shift", "Shift"}, {60, "right_shift", "Right Shift"}, {59, "control", "Control"},
        {62, "right_control", "Right Control"}, {58, "option", "Option"}, {61, "right_option", "Right Option"},
        {126, "arrow_up", "\xE2\x86\x91"}, {125, "arrow_down", "\xE2\x86\x93"}, {123, "arrow_left", "\xE2\x86\x90"},
        {124, "arrow_right", "\xE2\x86\x92"},
        {50, "grave", "`"}, {27, "minus", "-"}, {24, "equal", "="}, {33, "left_bracket", "["}, {30, "right_bracket", "]"},
        {42, "backslash", "\\"}, {41, "semicolon", ";"}, {39, "quote", "'"}, {43, "comma", ","}, {47, "period", "."},
        {44, "slash", "/"},
        {115, "home", "Home"}, {119, "end", "End"}, {116, "page_up", "Page Up"}, {121, "page_down", "Page Down"},
        {122, "f1", "F1"}, {120, "f2", "F2"}, {99, "f3", "F3"}, {118, "f4", "F4"}, {96, "f5", "F5"}, {97, "f6", "F6"},
        {98, "f7", "F7"}, {100, "f8", "F8"}, {101, "f9", "F9"}, {109, "f10", "F10"}, {103, "f11", "F11"}, {111, "f12", "F12"},
        {82, "keypad_0", "Keypad 0"}, {83, "keypad_1", "Keypad 1"}, {84, "keypad_2", "Keypad 2"}, {85, "keypad_3", "Keypad 3"},
        {86, "keypad_4", "Keypad 4"}, {87, "keypad_5", "Keypad 5"}, {88, "keypad_6", "Keypad 6"}, {89, "keypad_7", "Keypad 7"},
        {91, "keypad_8", "Keypad 8"}, {92, "keypad_9", "Keypad 9"}, {65, "keypad_period", "Keypad ."},
        {76, "keypad_enter", "Keypad Enter"}, {69, "keypad_plus", "Keypad +"}, {78, "keypad_minus", "Keypad -"},
        {67, "keypad_multiply", "Keypad *"}, {75, "keypad_divide", "Keypad /"},
        {128, "mouse_left", "Left click"}, {129, "mouse_right", "Right click"}, {130, "mouse_middle", "Wheel click"},
        {131, "mouse4", "Mouse 4"}, {132, "mouse5", "Mouse 5"}};
    *count = (int)(sizeof inputs / sizeof *inputs);
    return inputs;
}

static inline const KeyMapInput *keymap_input_find(int input) {
    int count; const KeyMapInput *all = keymap_inputs(&count);
    for (int i = 0; i < count; i++) if (all[i].input == input) return &all[i];
    return NULL;
}
static inline const char *keymap_input_name(int input) { const KeyMapInput *k = keymap_input_find(input); return k ? k->name : NULL; }
static inline const char *keymap_input_label(int input) { const KeyMapInput *k = keymap_input_find(input); return k ? k->label : NULL; }
/* 1: a layout may use this input (a named key other than key 1, or a mouse button). */
static inline int keymap_bindable(int input) { return input != KEYMAP_SETTINGS_KEY && keymap_input_find(input) != NULL; }

static inline int keymap_word_index(int (*name_of)(int, const char *, size_t), int count, const char *s, size_t n) {
    for (int i = 0; i < count; i++) if (name_of(i, s, n)) return i;
    return -1;
}
static inline int keymap_action_is(int action, const char *s, size_t n) {
    const char *name = keymap_action_name(action);
    return strlen(name) == n && !strncasecmp(name, s, n);
}
static inline int keymap_action_parse(const char *s, size_t n) { return keymap_word_index(keymap_action_is, KEYMAP_ACTIONS, s, n); }
/* The input [s, s + n) names, or -1. */
static inline int keymap_input_parse(const char *s, size_t n) {
    int count; const KeyMapInput *all = keymap_inputs(&count);
    for (int i = 0; i < count; i++) if (strlen(all[i].name) == n && !strncasecmp(all[i].name, s, n)) return all[i].input;
    return -1;
}

static inline void keymap_default(KeyMap *m) {
    memset(m, KEYMAP_NONE, sizeof *m);
#define KEYMAP_SET(action, first, second) do { m->slot[action][0] = (first); m->slot[action][1] = (second); } while (0)
    KEYMAP_SET(KEYMAP_A, 49, KEYMAP_MOUSE_FIRST + 0);   /* Space, left click: talk, use the tool, confirm */
    KEYMAP_SET(KEYMAP_B, 56, 53);                        /* Shift, Esc: run (held), cancel */
    KEYMAP_SET(KEYMAP_X, 48, 34);                        /* Tab, I: pockets */
    KEYMAP_SET(KEYMAP_Y, 14, KEYMAP_MOUSE_FIRST + 1);   /* E, right click: pick up */
    KEYMAP_SET(KEYMAP_LSTICK, 59, KEYMAP_NONE);          /* Control: left stick press */
    KEYMAP_SET(KEYMAP_RSTICK, 9, KEYMAP_NONE);           /* V: right stick press */
    KEYMAP_SET(KEYMAP_L, 6, KEYMAP_NONE);                /* Z: L */
    KEYMAP_SET(KEYMAP_R, 8, KEYMAP_NONE);                /* C: R */
    KEYMAP_SET(KEYMAP_ZL, 12, KEYMAP_NONE);              /* Q: NookPhone */
    KEYMAP_SET(KEYMAP_ZR, 15, KEYMAP_NONE);              /* R: reactions */
    KEYMAP_SET(KEYMAP_PLUS, 46, 36);                     /* M, Return: + */
    KEYMAP_SET(KEYMAP_MINUS, 51, KEYMAP_NONE);           /* Backspace: - */
    KEYMAP_SET(KEYMAP_DLEFT, 19, KEYMAP_MOUSE_FIRST + 3);   /* 2 3 4 5: d-pad left, up, right, down; mouse 4 left, */
    KEYMAP_SET(KEYMAP_DUP, 20, KEYMAP_MOUSE_FIRST + 2);     /* the wheel click up (the tool ring), mouse 5 right */
    KEYMAP_SET(KEYMAP_DRIGHT, 21, KEYMAP_MOUSE_FIRST + 4);
    KEYMAP_SET(KEYMAP_DDOWN, 23, KEYMAP_NONE);
    KEYMAP_SET(KEYMAP_MOVE_UP, 13, KEYMAP_NONE);         /* W A S D */
    KEYMAP_SET(KEYMAP_MOVE_DOWN, 1, KEYMAP_NONE);
    KEYMAP_SET(KEYMAP_MOVE_LEFT, 0, KEYMAP_NONE);
    KEYMAP_SET(KEYMAP_MOVE_RIGHT, 2, KEYMAP_NONE);
    KEYMAP_SET(KEYMAP_CAMERA_UP, 126, KEYMAP_NONE);      /* the arrow keys */
    KEYMAP_SET(KEYMAP_CAMERA_DOWN, 125, KEYMAP_NONE);
    KEYMAP_SET(KEYMAP_CAMERA_LEFT, 123, KEYMAP_NONE);
    KEYMAP_SET(KEYMAP_CAMERA_RIGHT, 124, KEYMAP_NONE);
#undef KEYMAP_SET
}

/* The action and slot holding input, or -1. */
static inline int keymap_action_of(const KeyMap *m, int input, int *slot) {
    for (int a = 0; a < KEYMAP_ACTIONS; a++)
        for (int s = 0; s < KEYMAP_SLOTS; s++)
            if (m->slot[a][s] == input) { if (slot) *slot = s; return a; }
    return -1;
}

/* Puts input (or KEYMAP_NONE) into one slot. An input another slot holds moves here, and that slot takes what this one
   held, so binding never leaves two actions on one input. Returns 0 when the input cannot be bound. */
static inline int keymap_bind(KeyMap *m, int action, int slot, int input) {
    if (action < 0 || action >= KEYMAP_ACTIONS || slot < 0 || slot >= KEYMAP_SLOTS) return 0;
    if (input != KEYMAP_NONE && !keymap_bindable(input)) return 0;
    uint8_t was = m->slot[action][slot];
    int other_slot, other = input == KEYMAP_NONE ? -1 : keymap_action_of(m, input, &other_slot);
    if (other >= 0) m->slot[other][other_slot] = was;
    m->slot[action][slot] = (uint8_t)input;
    return 1;
}

/* The key bindings drawing (settings panel): input now drives action (-1: nothing). It leaves the action it drove (the
   other input of that action moves up), and takes a free slot of the new action, or its second one, whose input is then
   unused. Nothing swaps: the drawing shows every key, so a key left without an action is seen. 0 when not bindable. */
static inline int keymap_assign(KeyMap *m, int input, int action) {
    if (!keymap_bindable(input) || action < -1 || action >= KEYMAP_ACTIONS) return 0;
    int slot, was = keymap_action_of(m, input, &slot);
    if (was == action) return 1;
    if (was >= 0) {
        m->slot[was][slot] = KEYMAP_NONE;
        if (m->slot[was][0] == KEYMAP_NONE) { m->slot[was][0] = m->slot[was][1]; m->slot[was][1] = KEYMAP_NONE; }
    }
    if (action >= 0) m->slot[action][m->slot[action][0] == KEYMAP_NONE ? 0 : 1] = (uint8_t)input;
    return 1;
}

/* Applies a layout file's text to m (start from keymap_default). Returns 0, or the 1-based number of the first bad line
   with *error naming the problem; m is then unchanged. */
static inline int keymap_parse(const char *text, KeyMap *m, const char **error) {
    KeyMap next = *m; int given[KEYMAP_ACTIONS] = {0}, line = 0;
    for (const char *p = text; p && *p;) {
        line++;
        const char *end = strchr(p, '\n'), *stop = end ? end : p + strlen(p), *hash = memchr(p, '#', (size_t)(stop - p));
        if (hash) stop = hash;
        while (p < stop && (*p == ' ' || *p == '\t' || *p == '\r')) p++;
        while (stop > p && (stop[-1] == ' ' || stop[-1] == '\t' || stop[-1] == '\r')) stop--;
        if (p < stop) {
            const char *eq = memchr(p, '=', (size_t)(stop - p));
            if (!eq) { *error = "expected \"action = input [input]\""; return line; }
            const char *key_end = eq;
            while (key_end > p && (key_end[-1] == ' ' || key_end[-1] == '\t')) key_end--;
            int action = keymap_action_parse(p, (size_t)(key_end - p));
            if (action < 0) { *error = "unknown action (a b x y l r zl zr plus minus lstick rstick up down left right move_up move_down move_left move_right camera_up camera_down camera_left camera_right)"; return line; }
            if (given[action]) { *error = "this action has a line already"; return line; }
            uint8_t slots[KEYMAP_SLOTS] = {KEYMAP_NONE, KEYMAP_NONE}; int words = 0, none = 0;
            for (const char *q = eq + 1; q < stop;) {
                while (q < stop && (*q == ' ' || *q == '\t')) q++;
                if (q >= stop) break;
                const char *word = q; while (q < stop && *q != ' ' && *q != '\t') q++;
                size_t n = (size_t)(q - word);
                if (++words > KEYMAP_SLOTS) { *error = "at most two inputs an action"; return line; }
                if (n == 4 && !strncasecmp(word, "none", 4)) { none = 1; continue; }
                int input = keymap_input_parse(word, n);
                if (input < 0) { *error = "unknown key or mouse button (letters, digits, space, return, escape, tab, backspace, shift, control, option, arrow_up ..., f1 ..., mouse_left, mouse_right, mouse_middle, mouse4, mouse5)"; return line; }
                if (input == KEYMAP_SETTINGS_KEY) { *error = "key 1 opens the settings panel"; return line; }
                for (int a = 0; a < KEYMAP_ACTIONS; a++)
                    if (given[a] && (next.slot[a][0] == input || next.slot[a][1] == input)) { *error = "this input drives another action already"; return line; }
                if (slots[0] == input) { *error = "the same input twice"; return line; }
                slots[words - 1 - none] = (uint8_t)input;
            }
            if (!words) { *error = "missing input (or none)"; return line; }
            if (none && words > 1) { *error = "none stands alone"; return line; }
            next.slot[action][0] = slots[0]; next.slot[action][1] = slots[1]; given[action] = 1;
        }
        p = end ? end + 1 : NULL;
    }
    /* An input the file gives to an action leaves the default of an action the file does not name. */
    for (int a = 0; a < KEYMAP_ACTIONS; a++) {
        if (given[a]) continue;
        for (int s = 0; s < KEYMAP_SLOTS; s++)
            for (int g = 0; g < KEYMAP_ACTIONS; g++)
                if (given[g] && next.slot[a][s] != KEYMAP_NONE && (next.slot[g][0] == next.slot[a][s] || next.slot[g][1] == next.slot[a][s]))
                    next.slot[a][s] = KEYMAP_NONE;
        if (next.slot[a][0] == KEYMAP_NONE && next.slot[a][1] != KEYMAP_NONE) { next.slot[a][0] = next.slot[a][1]; next.slot[a][1] = KEYMAP_NONE; }
    }
    *m = next;
    return 0;
}

/* The whole layout as a file keymap_parse reads back (every action, one line each); the length, or -1 when it does not fit. */
static inline int keymap_format(const KeyMap *m, char *out, size_t size) {
    size_t used = 0;
    int n = snprintf(out, size, "# Keyboard and mouse layout, written by the settings panel (key 1). Delete this file for the default.\n"
                                "# action = input [second input]; none leaves the action without input.\n");
    if (n < 0 || (size_t)n >= size) return -1;
    used = (size_t)n;
    for (int a = 0; a < KEYMAP_ACTIONS; a++) {
        const char *first = keymap_input_name(m->slot[a][0]), *second = keymap_input_name(m->slot[a][1]);
        if (!first) { first = second; second = NULL; }
        n = snprintf(out + used, size - used, "%s = %s%s%s\n", keymap_action_name(a), first ? first : "none", second ? " " : "", second ? second : "");
        if (n < 0 || (size_t)n >= size - used) return -1;
        used += (size_t)n;
    }
    return (int)used;
}

/* The FullKey buttons and the two sticks (-32767..32767, up positive) from the inputs held (held[input] nonzero). */
typedef struct { uint64_t buttons; int32_t lx, ly, rx, ry; } KeyMapOutput;
static inline KeyMapOutput keymap_apply(const KeyMap *m, const unsigned char held[KEYMAP_INPUTS]) {
    int on[KEYMAP_ACTIONS];
    for (int a = 0; a < KEYMAP_ACTIONS; a++) {
        on[a] = 0;
        for (int s = 0; s < KEYMAP_SLOTS; s++) if (m->slot[a][s] < KEYMAP_INPUTS && held[m->slot[a][s]]) on[a] = 1;
    }
    KeyMapOutput o = {0};
    for (int a = 0; a < KEYMAP_SWITCH_BUTTONS; a++) if (on[a]) o.buttons |= 1ull << a;
    o.lx = 32767 * (on[KEYMAP_MOVE_RIGHT] - on[KEYMAP_MOVE_LEFT]); o.ly = 32767 * (on[KEYMAP_MOVE_UP] - on[KEYMAP_MOVE_DOWN]);
    o.rx = 32767 * (on[KEYMAP_CAMERA_RIGHT] - on[KEYMAP_CAMERA_LEFT]); o.ry = 32767 * (on[KEYMAP_CAMERA_UP] - on[KEYMAP_CAMERA_DOWN]);
    return o;
}
#endif
