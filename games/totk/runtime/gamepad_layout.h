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

/* The Switch button bits for GameController's buttonA (bottom), buttonB (right), buttonX (left), buttonY (top). */
static inline uint64_t gamepad_face_bits(int bottom, int right, int left, int top, int by_position) {
    if (by_position)
        return (right ? GAMEPAD_SWITCH_A : 0) | (bottom ? GAMEPAD_SWITCH_B : 0) | (top ? GAMEPAD_SWITCH_X : 0) | (left ? GAMEPAD_SWITCH_Y : 0);
    return (bottom ? GAMEPAD_SWITCH_A : 0) | (right ? GAMEPAD_SWITCH_B : 0) | (left ? GAMEPAD_SWITCH_X : 0) | (top ? GAMEPAD_SWITCH_Y : 0);
}
