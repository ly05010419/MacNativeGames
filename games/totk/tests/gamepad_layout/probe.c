// runtime/gamepad_layout.h: every controller's face buttons by GameController's names (the Xbox layout).
#include <stdio.h>
#include "gamepad_layout.h"

static int failures, checked;
static void check(int ok, const char *what) { checked++; if (!ok) { failures++; fprintf(stderr, "FAIL: %s\n", what); } }

int main(void) {
    // Xbox A / Cross (bottom) is A, Xbox B / Circle (right) is B, Xbox X / Square (left) is X, Xbox Y / Triangle (top) is Y
    check(gamepad_face_bits(1, 0, 0, 0) == GAMEPAD_SWITCH_A, "bottom button is Switch A");
    check(gamepad_face_bits(0, 1, 0, 0) == GAMEPAD_SWITCH_B, "right button is Switch B");
    check(gamepad_face_bits(0, 0, 1, 0) == GAMEPAD_SWITCH_X, "left button is Switch X");
    check(gamepad_face_bits(0, 0, 0, 1) == GAMEPAD_SWITCH_Y, "top button is Switch Y");
    check(gamepad_face_bits(1, 1, 1, 1) == 15 && gamepad_face_bits(0, 0, 0, 0) == 0, "all and none");
    // controls.txt names: buttonA south, buttonB east, buttonX west, buttonY north
    check(gamepad_face_positions(1, 0, 0, 0) == 1 && gamepad_face_positions(0, 1, 0, 0) == 2 &&
          gamepad_face_positions(0, 0, 1, 0) == 4 && gamepad_face_positions(0, 0, 0, 1) == 8, "positions as on an Xbox controller");
    printf("{\"status\":\"%s\",\"probe\":\"gamepad-layout\",\"checked\":%d,\"failures\":%d}\n", failures ? "fail" : "pass", checked, failures);
    return failures != 0;
}
