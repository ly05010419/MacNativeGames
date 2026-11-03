// runtime/gamepad_layout.h: which controllers map their face buttons by position, and the resulting Switch buttons.
#include <math.h>
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
    // HID pads GameController does not take: the order by name, then each usage alone gives one physical bit
    // (controller_map.h: 0 south 1 east 2 west 3 north 4 l3 5 r3 6 l1 7 r1 8 l2 9 r2 10 menu 11 view)
    check(gamepad_hid_order("Lic3 Pro Controller") == GAMEPAD_HID_SWITCH_PRO, "a PowerA for Switch 2 sends the Switch Pro order");
    check(gamepad_hid_order("8BitDo SN30 Pro") == GAMEPAD_HID_GENERIC && gamepad_hid_order(NULL) == GAMEPAD_HID_GENERIC,
          "other pads keep the generic order");
    // measured 2026-10-27 on the PowerA: A 2, B 1, X 4, Y 3, L 5, R 6, ZL 7, ZR 8, minus 9, plus 10
    const int pro_bits[15] = {-1, 0, 1, 2, 3, 6, 7, 8, 9, 11, 10, 4, 5, -1, -1};
    const int generic_bits[16] = {-1, 0, 1, -1, 2, 3, -1, 6, 7, 8, 9, 11, 10, -1, 4, 5};
    for (int u = 1; u <= 15; u++) {
        unsigned char b[16] = {0}; b[u] = 1;
        char what[96];
        int want = u < 15 ? pro_bits[u] : -1;
        snprintf(what, sizeof what, "Switch Pro order: usage %d", u);
        check(gamepad_hid_buttons(b, GAMEPAD_HID_SWITCH_PRO) == (want < 0 ? 0 : 1ULL << want), what);
        snprintf(what, sizeof what, "generic order unchanged: usage %d", u);
        check(gamepad_hid_buttons(b, GAMEPAD_HID_GENERIC) == (generic_bits[u] < 0 ? 0 : 1ULL << generic_bits[u]), what);
    }
    // a stick that stops at 0.72 / -0.74 of its range reads 1 once pushed there, and from 0.85 of that reach on;
    // below it the value grows in proportion
    GamepadHidReach r = {0};
    const float full_at = GAMEPAD_HID_FULL_AT;
    check(fabsf(gamepad_hid_reach(&r, 0.4f) - 0.4f / (0.65f * full_at)) < 1e-5f, "before a full push: scaled by the minimum reach");
    check(gamepad_hid_reach(&r, 0.72f) == 1.0f && gamepad_hid_reach(&r, -0.74f) == -1.0f, "a full push reads 1");
    check(gamepad_hid_reach(&r, 0.62f) == 1.0f && gamepad_hid_reach(&r, -0.64f) == -1.0f,
          "0.86 of the reach reads 1 (a flick gets there a report earlier)");
    check(gamepad_hid_reach(&r, 0.57f) < 1.0f && gamepad_hid_reach(&r, -0.59f) > -1.0f, "0.8 of the reach is not full yet");
    check(fabsf(gamepad_hid_reach(&r, 0.36f * full_at) - 0.5f) < 1e-5f && fabsf(gamepad_hid_reach(&r, -0.37f * full_at) + 0.5f) < 1e-5f,
          "half of the full point reads a half");
    check(gamepad_hid_reach(&r, 0.0f) == 0.0f, "centre stays centre");
    GamepadHidReach full = {0};
    check(gamepad_hid_reach(&full, 1.0f) == 1.0f && fabsf(gamepad_hid_reach(&full, 0.5f) - 0.5f / full_at) < 1e-5f,
          "a stick reaching the range's end: only the full point");
    printf("{\"status\":\"%s\",\"probe\":\"gamepad-layout\",\"checked\":%d,\"failures\":%d}\n", failures ? "fail" : "pass", checked, failures);
    return failures != 0;
}
