/* Controller button layout: each physical button of a controller (named by its position, the same on a DualSense, an Xbox
   and a Nintendo controller: runtime/gamepad_layout.h gamepad_face_positions) drives one Switch Pro Controller button, or
   none. The default is the Switch's own layout by position: east -> A, south -> B, north -> X, west -> Y (issue #10); the
   other buttons i < 16 are FullKey bit i. The touchpad click (DualSense, DualShock 4) is physical button 16 and drives
   nothing by default. The mouse's buttons (Mac, mouse mode on key 7: runtime/gamepad.m) are physical buttons 17-21:
   left attacks (Y), right draws the bow (ZR, "aim mouse_left": a left click while it is held shoots instead of
   attacking), middle targets (ZL), the two side buttons drive nothing.
   A layout file has one "physical = switch [mode]" line per change; '#' starts a comment; names are case-insensitive;
   "none" leaves a button unused; lines not given keep the default. The optional mode says how long the Switch button is
   held: "hold" (default) while the physical button is down, "tap" for 400 ms after each press (tap again to keep
   running), "toggle" on and off at each press (cleared when the stick returns to center), "aim <fire button>" while
   the button is down, except while the fire button is also down (hold L2 to draw the bow, press R2 to shoot it; the
   fire button's own line applies only while the aim button is up). A mode byte keeps the kind in its low 3 bits and,
   for aim, the fire button's index in its high 5 bits.
   A "reach <lstick|rstick> <up|down|left|right> = <0.3..1>" line says how far a worn stick actually gets in that
   direction (measured 24 October: an Xbox controller's left stick reached only 0.67 downward); that half-axis is scaled
   up so its full travel gives full input (controller_reach_parse).
   Pure C: app/launcher.m checks the file before the game starts (an alert names the bad line), runtime/gamepad.m
   applies it, tests/test_controller_map.c covers the parser. */
#ifndef TOTK_CONTROLLER_MAP_H
#define TOTK_CONTROLLER_MAP_H
#include <stdint.h>
#include <string.h>
#include <strings.h>

enum { CONTROLLER_BUTTONS = 22, CONTROLLER_SWITCH_BUTTONS = 16, CONTROLLER_MOUSE_FIRST = 17, CONTROLLER_MOUSE_BUTTONS = 5 };
static const char *const controller_physical_names[CONTROLLER_BUTTONS] = {
    "south", "east", "west", "north", "l3", "r3", "l1", "r1", "l2", "r2", "menu", "view", "left", "up", "right", "down",
    "touchpad", "mouse_left", "mouse_right", "mouse_middle", "mouse4", "mouse5"};
static const char *const controller_switch_names[CONTROLLER_SWITCH_BUTTONS] = {
    "a", "b", "x", "y", "lstick", "rstick", "l", "r", "zl", "zr", "plus", "minus", "left", "up", "right", "down"};

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
        if (strlen(names[i]) == n && !strncasecmp(names[i], s, n)) return i;
    return -1;
}

/* Applies a layout file's text to map and modes (start from controller_map_default). Returns 0, or the 1-based number of
   the first bad line with *error naming the problem; both arrays are then unchanged. */
static inline int controller_map_parse(const char *text, uint16_t map[CONTROLLER_BUTTONS],
                                       uint8_t modes[CONTROLLER_BUTTONS], const char **error) {
    uint16_t next[CONTROLLER_BUTTONS]; uint8_t nextMode[CONTROLLER_BUTTONS];
    memcpy(next, map, sizeof next); memcpy(nextMode, modes, sizeof nextMode);
    int line = 0;
    for (const char *p = text; p && *p;) {
        line++;
        const char *end = strchr(p, '\n'), *stop = end ? end : p + strlen(p), *hash = memchr(p, '#', (size_t)(stop - p));
        if (hash) stop = hash;
        while (p < stop && (*p == ' ' || *p == '\t' || *p == '\r')) p++;
        while (stop > p && (stop[-1] == ' ' || stop[-1] == '\t' || stop[-1] == '\r')) stop--;
        if (p < stop && !(stop - p > 6 && !strncasecmp(p, "reach", 5) && (p[5] == ' ' || p[5] == '\t'))) {   /* reach lines: controller_reach_parse */
            const char *eq = memchr(p, '=', (size_t)(stop - p));
            if (!eq) { *error = "expected \"button = Switch button [hold|tap|toggle]\""; return line; }
            const char *key_end = eq, *value = eq + 1;
            while (key_end > p && (key_end[-1] == ' ' || key_end[-1] == '\t')) key_end--;
            while (value < stop && (*value == ' ' || *value == '\t')) value++;
            const char *value_end = value, *mode = NULL, *mode_end = NULL;
            while (value_end < stop && *value_end != ' ' && *value_end != '\t') value_end++;
            for (const char *q = value_end; q < stop; q++) if (*q != ' ' && *q != '\t') { mode = q; break; }
            if (mode) { mode_end = mode; while (mode_end < stop && *mode_end != ' ' && *mode_end != '\t') mode_end++; }
            int physical = controller_name_index(controller_physical_names, CONTROLLER_BUTTONS, p, (size_t)(key_end - p));
            if (physical < 0) { *error = "unknown controller button (south east west north l1 r1 l2 r2 l3 r3 menu view up down left right touchpad mouse_left mouse_right mouse_middle mouse4 mouse5)"; return line; }
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
    memcpy(map, next, sizeof next); memcpy(modes, nextMode, sizeof nextMode);
    return 0;
}

/* Stick reach lines (see the top): reach[stick * 4 + direction], stick 0 lstick / 1 rstick, direction 0 up / 1 down /
   2 left / 3 right; 1 = full travel (the default). Returns 0, or the 1-based number of the first bad line with *error. */
static inline int controller_reach_parse(const char *text, float reach[8], const char **error) {
    float next[8]; for (int i = 0; i < 8; i++) next[i] = 1.0f;
    static const char *const sticks[] = {"lstick", "rstick"}, *const dirs[] = {"up", "down", "left", "right"};
    int line = 0;
    for (const char *p = text; p && *p;) {
        line++;
        const char *end = strchr(p, '\n'), *stop = end ? end : p + strlen(p), *hash = memchr(p, '#', (size_t)(stop - p));
        if (hash) stop = hash;
        while (p < stop && (*p == ' ' || *p == '\t' || *p == '\r')) p++;
        if (stop - p > 6 && !strncasecmp(p, "reach", 5) && (p[5] == ' ' || p[5] == '\t')) {
            char stick[16] = "", dir[16] = "", rest[64] = ""; float value = 0; char tail[8] = "";
            size_t n = (size_t)(stop - p); char buf[128]; if (n >= sizeof buf) { *error = "reach line too long"; return line; }
            memcpy(buf, p, n); buf[n] = 0;
            if (sscanf(buf, "%*s %15s %15[^= \t] = %63s %7s", stick, dir, rest, tail) != 3 || sscanf(rest, "%f", &value) != 1) {
                *error = "expected \"reach lstick down = 0.67\""; return line; }
            int s = controller_name_index(sticks, 2, stick, strlen(stick)), d = controller_name_index(dirs, 4, dir, strlen(dir));
            if (s < 0 || d < 0) { *error = "reach needs lstick or rstick, then up, down, left or right"; return line; }
            if (!(value >= 0.3f && value <= 1.0f)) { *error = "reach must be between 0.3 and 1"; return line; }
            next[s * 4 + d] = value;
        }
        p = end ? end + 1 : NULL;
    }
    memcpy(reach, next, sizeof next);
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
