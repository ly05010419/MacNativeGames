/* The button icon plan: for each glyph of the game's icon font that make_button_icons.py's keyboard style redraws, what
   to draw for a keyboard layout (keyboard_map.h). The settings panel's Apply button icons (runtime/icons_apply.m) writes
   it as JSON and has make_button_icons.py --style plan draw it; the default layout's plan is the keyboard style's own
   table (tests/test_keyboard_icons.py), so the shipped keyboard icons and a plan of the default are the same drawing.
     {"version":1,"glyphs":{"E040":["single","E"],"E041":["wide","Shift"],"E049":["cluster","3","2","5","4"],...}}
   A glyph naming one action draws that action's first input (the second when the first is none, "none" when it has
   neither): "single" for a one-character label, else "wide". A glyph naming a stick's or the d-pad's directions draws
   their first inputs side by side ("pair") or as the arrow-key cluster (top, left, middle, right), an empty key for a
   direction without input. Mouse buttons are drawn whether or not the mouse plays (the keyboard style does the same).
   Pure C: tests/test_keyboard_icons.py covers it. */
#ifndef TOTK_KEYBOARD_ICONS_H
#define TOTK_KEYBOARD_ICONS_H
#include <stdarg.h>
#include "keyboard_map.h"

typedef struct KeyboardIconsGlyph {
    unsigned code;
    const char *shape;      /* "single" (one action: single or wide by its label), "pair", "cluster" */
    int actions[4];
} KeyboardIconsGlyph;

/* In make_button_icons.py's keyboard style order. */
static inline const KeyboardIconsGlyph *keyboard_icons_glyphs(int *count) {
    static const KeyboardIconsGlyph glyphs[] = {
        {0xE040, "single", {KEYMAP_A}}, {0xE041, "single", {KEYMAP_B}}, {0xE042, "single", {KEYMAP_X}},
        {0xE043, "single", {KEYMAP_Y}}, {0xE044, "single", {KEYMAP_L}}, {0xE045, "single", {KEYMAP_R}},
        {0xE046, "single", {KEYMAP_ZL}}, {0xE047, "single", {KEYMAP_ZR}},
        {0xE049, "cluster", {KEYMAP_DUP, KEYMAP_DLEFT, KEYMAP_DDOWN, KEYMAP_DRIGHT}},
        {0xE04B, "single", {KEYMAP_PLUS}}, {0xE04C, "single", {KEYMAP_MINUS}},
        /* left stick: moving */
        {0xE050, "single", {KEYMAP_MOVE_DOWN}}, {0xE052, "single", {KEYMAP_MOVE_UP}},
        {0xE054, "pair", {KEYMAP_MOVE_LEFT, KEYMAP_MOVE_RIGHT}}, {0xE058, "single", {KEYMAP_MOVE_RIGHT}},
        {0xE05A, "single", {KEYMAP_MOVE_LEFT}}, {0xE05C, "pair", {KEYMAP_MOVE_UP, KEYMAP_MOVE_DOWN}},
        {0xE05E, "cluster", {KEYMAP_MOVE_UP, KEYMAP_MOVE_LEFT, KEYMAP_MOVE_DOWN, KEYMAP_MOVE_RIGHT}},
        {0xE056, "single", {KEYMAP_LSTICK}},
        /* right stick: the camera */
        {0xE051, "single", {KEYMAP_CAMERA_DOWN}}, {0xE053, "single", {KEYMAP_CAMERA_UP}},
        {0xE055, "pair", {KEYMAP_CAMERA_LEFT, KEYMAP_CAMERA_RIGHT}}, {0xE059, "single", {KEYMAP_CAMERA_RIGHT}},
        {0xE05B, "single", {KEYMAP_CAMERA_LEFT}}, {0xE05D, "pair", {KEYMAP_CAMERA_UP, KEYMAP_CAMERA_DOWN}},
        {0xE05F, "cluster", {KEYMAP_CAMERA_UP, KEYMAP_CAMERA_LEFT, KEYMAP_CAMERA_DOWN, KEYMAP_CAMERA_RIGHT}},
        {0xE068, "cluster", {KEYMAP_CAMERA_UP, KEYMAP_CAMERA_LEFT, KEYMAP_CAMERA_DOWN, KEYMAP_CAMERA_RIGHT}},
        {0xE057, "single", {KEYMAP_RSTICK}},
        /* d-pad */
        {0xE060, "single", {KEYMAP_DUP}}, {0xE061, "single", {KEYMAP_DDOWN}}, {0xE062, "single", {KEYMAP_DLEFT}},
        {0xE063, "single", {KEYMAP_DRIGHT}}, {0xE064, "pair", {KEYMAP_DUP, KEYMAP_DDOWN}},
        {0xE065, "pair", {KEYMAP_DLEFT, KEYMAP_DRIGHT}}};
    *count = (int)(sizeof glyphs / sizeof *glyphs);
    return glyphs;
}

/* The label an input gets on a key: keyboard_map.h's label, the long ones shortened to fit a key. */
static inline const char *keyboard_icons_label(int input) {
    static const struct { const char *label, *icon; } shorter[] = {
        {"Left click", "LMB"}, {"Right click", "RMB"}, {"Wheel click", "MMB"}, {"Mouse 4", "M4"}, {"Mouse 5", "M5"},
        {"Control", "Ctrl"}, {"Right Control", "RCtrl"}, {"Option", "Opt"}, {"Right Option", "ROpt"},
        {"Right Shift", "RShift"}, {"Backspace", "Bksp"}, {"Delete", "Del"}, {"Page Up", "PgUp"}, {"Page Down", "PgDn"},
        {"Keypad Enter", "KEnt"}};
    const char *label = keymap_input_label(input);
    if (!label) return NULL;
    for (size_t i = 0; i < sizeof shorter / sizeof *shorter; i++) if (!strcmp(label, shorter[i].label)) return shorter[i].icon;
    if (!strncmp(label, "Keypad ", 7)) {   /* "Keypad 1" -> "K1" */
        static char keypad[8];
        snprintf(keypad, sizeof keypad, "K%s", label + 7);
        return keypad;
    }
    return label;
}

/* The first input of an action, the second when the first is none, else KEYMAP_NONE. */
static inline int keyboard_icons_input(const KeyMap *m, int action) {
    return m->slot[action][0] != KEYMAP_NONE ? m->slot[action][0] : m->slot[action][1];
}

/* 1: label is one character (UTF-8), drawn on a square key. */
static inline int keyboard_icons_one_character(const char *label) {
    const unsigned char *s = (const unsigned char *)label;
    if (!*s) return 0;
    size_t n = *s < 0x80 ? 1 : *s < 0xE0 ? 2 : *s < 0xF0 ? 3 : 4;
    return strlen(label) == n;
}

typedef struct { char *at; size_t left; int full; } KeyboardIconsOut;
static inline void keyboard_icons_put(KeyboardIconsOut *o, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int n = vsnprintf(o->at, o->left, format, args);
    va_end(args);
    if (n < 0 || (size_t)n >= o->left) { o->full = 1; o->left = 0; return; }
    o->at += n; o->left -= (size_t)n;
}
/* A label as a JSON string (the labels hold no control characters; quote and backslash are escaped). */
static inline void keyboard_icons_put_label(KeyboardIconsOut *o, const char *label) {
    keyboard_icons_put(o, ",\"");
    for (const char *c = label; *c; c++) keyboard_icons_put(o, *c == '"' || *c == '\\' ? "\\%c" : "%c", *c);
    keyboard_icons_put(o, "\"");
}

/* The plan of layout m into json (NUL-terminated): the bytes written, or -1 when size is too small. */
static inline int keyboard_icons_plan(const KeyMap *m, char *json, size_t size) {
    if (!size) return -1;
    KeyboardIconsOut o = {json, size, 0};
    int count; const KeyboardIconsGlyph *glyphs = keyboard_icons_glyphs(&count);
    keyboard_icons_put(&o, "{\"version\":1,\"glyphs\":{");
    for (int g = 0; g < count; g++) {
        keyboard_icons_put(&o, "%s\"%04X\":[", g ? "," : "", glyphs[g].code);
        if (!strcmp(glyphs[g].shape, "single")) {
            int input = keyboard_icons_input(m, glyphs[g].actions[0]);
            const char *label = input == KEYMAP_NONE ? NULL : keyboard_icons_label(input);
            if (!label) keyboard_icons_put(&o, "\"none\"");
            else {
                keyboard_icons_put(&o, keyboard_icons_one_character(label) ? "\"single\"" : "\"wide\"");
                keyboard_icons_put_label(&o, label);
            }
        } else {
            int n = !strcmp(glyphs[g].shape, "pair") ? 2 : 4;
            keyboard_icons_put(&o, "\"%s\"", glyphs[g].shape);
            for (int i = 0; i < n; i++) {
                int input = keyboard_icons_input(m, glyphs[g].actions[i]);
                const char *label = input == KEYMAP_NONE ? NULL : keyboard_icons_label(input);
                keyboard_icons_put_label(&o, label ? label : "");
            }
        }
        keyboard_icons_put(&o, "]");
    }
    keyboard_icons_put(&o, "}}");
    if (o.full) { json[0] = 0; return -1; }
    return (int)(o.at - json);
}
#endif
