/* Face buttons of a controller as the Switch's A / B / X / Y (issue #10).
   GameController names the face buttons by the Xbox layout: buttonA at the bottom, buttonB on the right, buttonX on the
   left, buttonY at the top (PlayStation: Cross, Circle, Square, Triangle; a Nintendo controller reports its own A, B, X,
   Y labels). This fork keeps the Xbox layout (the user, 2026-10-20): each of them drives the Switch button of its name,
   so Xbox A / Cross (bottom) is A and Xbox B / Circle (right) is B. Upstream maps PlayStation and Xbox controllers by
   position instead (the right button is A, as on a Switch); a layout file gives that (runtime/controller_map.h).
   Plain C, shared by runtime/gamepad.m (Mac and iOS) and tests/gamepad_layout/probe.c. */
#pragma once
#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { GAMEPAD_SWITCH_A = 1, GAMEPAD_SWITCH_B = 2, GAMEPAD_SWITCH_X = 4, GAMEPAD_SWITCH_Y = 8 };

/* The positions of GameController's buttonA, buttonB, buttonX, buttonY as runtime/controller_map.h names them (bits 0
   south, 1 east, 2 west, 3 north): where they sit on an Xbox controller (A bottom, B right, X left, Y top), and on a
   PlayStation one; a Nintendo controller's are named by its labels the same way (south = its A). controller_map.h's
   default layout turns them into the Switch button of the same label, which gives gamepad_face_bits. */
static inline uint64_t gamepad_face_positions(int a, int b, int x, int y) {
    enum { SOUTH = 1, EAST = 2, WEST = 4, NORTH = 8 };
    return (a ? SOUTH : 0) | (b ? EAST : 0) | (x ? WEST : 0) | (y ? NORTH : 0);
}

/* The Switch button bits for GameController's buttonA (bottom), buttonB (right), buttonX (left), buttonY (top). */
static inline uint64_t gamepad_face_bits(int bottom, int right, int left, int top) {
    return (bottom ? GAMEPAD_SWITCH_A : 0) | (right ? GAMEPAD_SWITCH_B : 0) | (left ? GAMEPAD_SWITCH_X : 0) | (top ? GAMEPAD_SWITCH_Y : 0);
}
