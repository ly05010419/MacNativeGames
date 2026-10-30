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
    // the settings panel's controller type (auto, Xbox, PlayStation, Switch), saved as one word
    unsetenv("TOTK_FACE_BUTTONS");
    for (int t = 0; t < GAMEPAD_TYPE_COUNT; t++) check(gamepad_type_parse(gamepad_type_name(t)) == t, "every type name parses back");
    check(gamepad_type_parse("  switch\n") == GAMEPAD_TYPE_SWITCH && gamepad_type_parse("xbox\r\n") == GAMEPAD_TYPE_XBOX, "the saved word may end in a newline");
    check(gamepad_type_parse("ps5") < 0 && gamepad_type_parse("") < 0 && gamepad_type_parse(NULL) < 0 && gamepad_type_parse("switchx") < 0,
          "anything else is not a type");
    check(gamepad_type_detect("DualSense", "DualSense Wireless Controller") == GAMEPAD_TYPE_PLAYSTATION, "DualSense is detected as PlayStation");
    check(gamepad_type_detect("DualShock 4", "Wireless Controller") == GAMEPAD_TYPE_PLAYSTATION, "DualShock 4 is detected as PlayStation");
    check(gamepad_type_detect("Switch Pro Controller", "Pro Controller") == GAMEPAD_TYPE_SWITCH, "Switch Pro is detected as Switch");
    check(gamepad_type_detect("Xbox One", "Xbox Wireless Controller") == GAMEPAD_TYPE_XBOX, "Xbox is detected as Xbox");
    check(gamepad_type_detect("MFi", "8BitDo SN30 Pro") == GAMEPAD_TYPE_XBOX && gamepad_type_detect(NULL, NULL) == GAMEPAD_TYPE_XBOX,
          "other controllers count as Xbox (GameController names their buttons that way)");
    check(gamepad_type_face_by_position(GAMEPAD_TYPE_XBOX, "Switch Pro Controller", "Pro Controller"), "type Xbox: by position, whatever the name");
    check(gamepad_type_face_by_position(GAMEPAD_TYPE_PLAYSTATION, "Switch Pro Controller", "Pro Controller"), "type PlayStation: by position");
    check(!gamepad_type_face_by_position(GAMEPAD_TYPE_SWITCH, "Xbox One", "Xbox Wireless Controller"), "type Switch: by label, whatever the name");
    check(gamepad_type_face_by_position(GAMEPAD_TYPE_AUTO, "Xbox One", "Xbox Wireless Controller") &&
          !gamepad_type_face_by_position(GAMEPAD_TYPE_AUTO, "Switch Pro Controller", "Pro Controller"), "type auto: as detected");
    setenv("TOTK_FACE_BUTTONS", "label", 1);
    check(!gamepad_type_face_by_position(GAMEPAD_TYPE_AUTO, "Xbox One", "Xbox Wireless Controller"), "type auto keeps TOTK_FACE_BUTTONS");
    check(gamepad_type_face_by_position(GAMEPAD_TYPE_XBOX, "Xbox One", "Xbox Wireless Controller"), "a picked type wins over TOTK_FACE_BUTTONS");
    unsetenv("TOTK_FACE_BUTTONS");
    printf("{\"status\":\"%s\",\"probe\":\"gamepad-layout\",\"checked\":%d,\"failures\":%d}\n", failures ? "fail" : "pass", checked, failures);
    return failures != 0;
}
