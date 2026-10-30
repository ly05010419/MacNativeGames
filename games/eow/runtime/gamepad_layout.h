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

static inline int gamepad_names_nintendo(const char *text) {   /* case-insensitive: "Nintendo", "Switch", "Joy-Con" */
    static const char *const words[] = {"nintendo", "switch", "joy-con", "joycon"};
    if (!text) return 0;
    char lower[128]; size_t n = 0;
    for (; text[n] && n < sizeof lower - 1; n++) lower[n] = (char)tolower((unsigned char)text[n]);
    lower[n] = 0;
    for (size_t i = 0; i < sizeof words / sizeof *words; i++) if (strstr(lower, words[i])) return 1;
    return 0;
}

/* 1: map this controller's face buttons by position. category / vendor: GCController productCategory / vendorName. */
static inline int gamepad_face_by_position(const char *category, const char *vendor) {
    const char *e = getenv("TOTK_FACE_BUTTONS");
    if (e && !strcmp(e, "label")) return 0;
    if (e && !strcmp(e, "position")) return 1;
    return !gamepad_names_nintendo(category) && !gamepad_names_nintendo(vendor);
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
