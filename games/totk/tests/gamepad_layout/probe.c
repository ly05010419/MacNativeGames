// runtime/gamepad_layout.h: which controllers map their face buttons by position, and the resulting Switch buttons.
#include <stdio.h>
#include "gamepad_layout.h"

static int failures, checked;
static void check(int ok, const char *what) { checked++; if (!ok) { failures++; fprintf(stderr, "FAIL: %s\n", what); } }

int main(void) {
    unsetenv("TOTK_FACE_BUTTONS");
    check(gamepad_face_by_position("DualSense", "DualSense Wireless Controller"), "DualSense by position");
    check(gamepad_face_by_position("DualShock 4", "Wireless Controller"), "DualShock 4 by position");
    check(gamepad_face_by_position("Xbox One", "Xbox Wireless Controller"), "Xbox by position");
    check(gamepad_face_by_position("MFi", "8BitDo SN30 Pro"), "an MFi (Xbox-style) controller by position");
    check(!gamepad_face_by_position("Switch Pro Controller", "Pro Controller"), "Switch Pro Controller by label");
    check(!gamepad_face_by_position("Nintendo Switch Joy-Con (L/R)", "Joy-Con (L/R)"), "Joy-Con by label");
    check(gamepad_face_by_position(NULL, NULL), "an unnamed controller by position");
    // by position: Circle / Xbox B (right) is A, Cross / Xbox A (bottom) is B, Triangle / Y (top) is X, Square / X (left) is Y
    check(gamepad_face_bits(0, 1, 0, 0, 1) == GAMEPAD_SWITCH_A, "right button is Switch A");
    check(gamepad_face_bits(1, 0, 0, 0, 1) == GAMEPAD_SWITCH_B, "bottom button is Switch B");
    check(gamepad_face_bits(0, 0, 0, 1, 1) == GAMEPAD_SWITCH_X, "top button is Switch X");
    check(gamepad_face_bits(0, 0, 1, 0, 1) == GAMEPAD_SWITCH_Y, "left button is Switch Y");
    check(gamepad_face_bits(1, 1, 1, 1, 1) == 15 && gamepad_face_bits(0, 0, 0, 0, 1) == 0, "all and none");
    // by label: GameController's buttonA..Y straight to A..Y
    check(gamepad_face_bits(1, 0, 0, 0, 0) == GAMEPAD_SWITCH_A && gamepad_face_bits(0, 1, 0, 0, 0) == GAMEPAD_SWITCH_B &&
          gamepad_face_bits(0, 0, 1, 0, 0) == GAMEPAD_SWITCH_X && gamepad_face_bits(0, 0, 0, 1, 0) == GAMEPAD_SWITCH_Y, "by label");
    setenv("TOTK_FACE_BUTTONS", "label", 1);
    check(!gamepad_face_by_position("DualSense", "DualSense Wireless Controller"), "TOTK_FACE_BUTTONS=label");
    setenv("TOTK_FACE_BUTTONS", "position", 1);
    check(gamepad_face_by_position("Switch Pro Controller", "Pro Controller"), "TOTK_FACE_BUTTONS=position");
    printf("{\"status\":\"%s\",\"probe\":\"gamepad-layout\",\"checked\":%d,\"failures\":%d}\n", failures ? "fail" : "pass", checked, failures);
    return failures != 0;
}
