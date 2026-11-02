/* Face buttons of a controller as the Switch's A / B / X / Y (issue #10).
   GameController names the face buttons by the Xbox layout: buttonA at the bottom, buttonB on the right, buttonX on the
   left, buttonY at the top (PlayStation: Cross, Circle, Square, Triangle). A Switch controller has A on the right, B at
   the bottom, X at the top and Y on the left. PlayStation, Xbox and other controllers are mapped by position, so each
   button sits where the game's Switch prompt shows it; Nintendo controllers keep GameController's own mapping.
   TOTK_FACE_BUTTONS=label maps every controller by label (the earlier behaviour), =position every one by position.
   Plain C, shared by runtime/gamepad.m (Mac and iOS) and tests/gamepad_layout/probe.c. */
#pragma once
#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { GAMEPAD_SWITCH_A = 1, GAMEPAD_SWITCH_B = 2, GAMEPAD_SWITCH_X = 4, GAMEPAD_SWITCH_Y = 8 };

static inline int gamepad_names_any(const char *text, const char *const *words, size_t count) {   /* case-insensitive */
    if (!text) return 0;
    char lower[128]; size_t n = 0;
    for (; text[n] && n < sizeof lower - 1; n++) lower[n] = (char)tolower((unsigned char)text[n]);
    lower[n] = 0;
    for (size_t i = 0; i < count; i++) if (strstr(lower, words[i])) return 1;
    return 0;
}

static inline int gamepad_names_nintendo(const char *text) {   /* "Nintendo", "Switch", "Joy-Con" */
    static const char *const words[] = {"nintendo", "switch", "joy-con", "joycon"};
    return gamepad_names_any(text, words, sizeof words / sizeof *words);
}

static inline int gamepad_names_playstation(const char *text) {   /* "DualSense", "DualShock", "PlayStation" */
    static const char *const words[] = {"dualsense", "dualshock", "playstation"};
    return gamepad_names_any(text, words, sizeof words / sizeof *words);
}

/* 1: map this controller's face buttons by position. category / vendor: GCController productCategory / vendorName. */
static inline int gamepad_face_by_position(const char *category, const char *vendor) {
    const char *e = getenv("TOTK_FACE_BUTTONS");
    if (e && !strcmp(e, "label")) return 0;
    if (e && !strcmp(e, "position")) return 1;
    return !gamepad_names_nintendo(category) && !gamepad_names_nintendo(vendor);
}

/* Controller type, key 6 on the Mac (runtime/input.c -> runtime/gamepad.m): auto, as gamepad_face_by_position decides
   from the name; Xbox and PlayStation map the face buttons by position; Switch maps them by label, GameController's
   buttonA being Switch A (on a Nintendo controller the right button, on an Xbox controller the bottom one, Xbox A). Xbox
   and PlayStation map the same way; they differ in name only, for the player to say which controller it is. The pick
   applies to every GameController controller and is saved as one word (gamepad_type_name); the generic HID pads of
   runtime/gamepad.m are Switch-style and always read by position. */
enum { GAMEPAD_TYPE_AUTO, GAMEPAD_TYPE_XBOX, GAMEPAD_TYPE_PLAYSTATION, GAMEPAD_TYPE_SWITCH, GAMEPAD_TYPE_COUNT };

static inline const char *gamepad_type_name(int type) {
    static const char *const names[GAMEPAD_TYPE_COUNT] = {"auto", "xbox", "playstation", "switch"};
    return type >= 0 && type < GAMEPAD_TYPE_COUNT ? names[type] : "auto";
}

/* The type a saved word names (surrounding white space ignored), or -1. */
static inline int gamepad_type_parse(const char *text) {
    if (!text) return -1;
    while (isspace((unsigned char)*text)) text++;
    size_t n = strlen(text);
    while (n && isspace((unsigned char)text[n - 1])) n--;
    for (int t = 0; t < GAMEPAD_TYPE_COUNT; t++) {
        const char *name = gamepad_type_name(t);
        if (n && strlen(name) == n && !strncmp(text, name, n)) return t;
    }
    return -1;
}

static inline int gamepad_type_next(int type) {   /* auto -> Xbox -> PlayStation -> Switch -> auto */
    return type >= 0 && type < GAMEPAD_TYPE_COUNT - 1 ? type + 1 : GAMEPAD_TYPE_AUTO;
}

/* What auto takes a controller for (shown on the HUD): Nintendo names Switch, PlayStation names PlayStation, any other
   controller Xbox, as GameController names its buttons. */
static inline int gamepad_type_detect(const char *category, const char *vendor) {
    if (gamepad_names_nintendo(category) || gamepad_names_nintendo(vendor)) return GAMEPAD_TYPE_SWITCH;
    if (gamepad_names_playstation(category) || gamepad_names_playstation(vendor)) return GAMEPAD_TYPE_PLAYSTATION;
    return GAMEPAD_TYPE_XBOX;
}

/* 1: map the face buttons by position under the picked type; auto keeps gamepad_face_by_position (and TOTK_FACE_BUTTONS). */
static inline int gamepad_type_face_by_position(int type, const char *category, const char *vendor) {
    if (type == GAMEPAD_TYPE_XBOX || type == GAMEPAD_TYPE_PLAYSTATION) return 1;
    if (type == GAMEPAD_TYPE_SWITCH) return 0;
    return gamepad_face_by_position(category, vendor);
}

/* The physical positions of GameController's buttonA, buttonB, buttonX, buttonY (bits 0 south, 1 east, 2 west, 3 north:
   runtime/controller_map.h). By position they sit as on an Xbox controller (A bottom, B right, X left, Y top); by label
   they name a Nintendo layout (A right, B bottom, X top, Y left). controller_map.h's default layout turns positions into
   the Switch's (A right, B bottom, X top, Y left), which gives gamepad_face_bits. */
static inline uint64_t gamepad_face_positions(int a, int b, int x, int y, int by_position) {
    enum { SOUTH = 1, EAST = 2, WEST = 4, NORTH = 8 };
    if (by_position) return (a ? SOUTH : 0) | (b ? EAST : 0) | (x ? WEST : 0) | (y ? NORTH : 0);
    return (a ? EAST : 0) | (b ? SOUTH : 0) | (x ? NORTH : 0) | (y ? WEST : 0);
}

/* The Switch button bits for GameController's buttonA (bottom), buttonB (right), buttonX (left), buttonY (top). */
static inline uint64_t gamepad_face_bits(int bottom, int right, int left, int top, int by_position) {
    if (by_position)
        return (right ? GAMEPAD_SWITCH_A : 0) | (bottom ? GAMEPAD_SWITCH_B : 0) | (top ? GAMEPAD_SWITCH_X : 0) | (left ? GAMEPAD_SWITCH_Y : 0);
    return (bottom ? GAMEPAD_SWITCH_A : 0) | (right ? GAMEPAD_SWITCH_B : 0) | (left ? GAMEPAD_SWITCH_X : 0) | (top ? GAMEPAD_SWITCH_Y : 0);
}
