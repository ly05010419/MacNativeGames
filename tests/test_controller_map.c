/* Test the controller layout parser and mapping (games/totk/runtime/controller_map.h) with synthetic files. */
#include "../games/totk/runtime/controller_map.h"
#include "../games/totk/runtime/gamepad_layout.h"
#include <stdio.h>

enum { SOUTH = 1 << 0, EAST = 1 << 1, WEST = 1 << 2, NORTH = 1 << 3, MENU = 1 << 10 };
enum { A = 1 << 0, B = 1 << 1, X = 1 << 2, Y = 1 << 3, ZR = 1 << 9, PLUS = 1 << 10 };

static int failures;
static void expect(int ok, const char *what) { if (!ok) { fprintf(stderr, "FAIL: %s\n", what); failures++; } }

static uint64_t press(const char *text, uint64_t physical, int *line) {
    uint16_t map[CONTROLLER_BUTTONS]; uint8_t modes[CONTROLLER_BUTTONS]; const char *error = NULL;
    controller_map_default(map, modes);
    *line = controller_map_parse(text, map, modes, &error);
    expect(!*line || error, "a bad line names its problem");
    return controller_map_apply(map, physical);
}

/* The mode and target of one physical button after parsing text, or -1 with *line set. */
static int mode_of(const char *text, int physical, int *line) {
    uint16_t map[CONTROLLER_BUTTONS]; uint8_t modes[CONTROLLER_BUTTONS]; const char *error = NULL;
    controller_map_default(map, modes);
    *line = controller_map_parse(text, map, modes, &error);
    return *line ? -1 : modes[physical];
}

int main(void) {
    int line;
    expect(press("", SOUTH | EAST | WEST | NORTH | MENU, &line) == (A | B | X | Y | PLUS) && !line, "no lines: default layout");
    expect(press("", EAST, &line) == A && press("", SOUTH, &line) == B && press("", NORTH, &line) == X && press("", WEST, &line) == Y,
           "the default is the Switch's layout by position: east A, south B, north X, west Y");
    /* Without a layout file every controller gets what gamepad_face_bits gave before (issue #10), by position or label. */
    for (int by_position = 0; by_position < 2; by_position++)
        for (int held = 0; held < 16; held++) {
            int a = held & 1, b = held >> 1 & 1, x = held >> 2 & 1, y = held >> 3 & 1;
            expect(press("", gamepad_face_positions(a, b, x, y, by_position), &line) == gamepad_face_bits(a, b, x, y, by_position),
                   "default layout over face positions = gamepad_face_bits");
        }
    uint64_t nintendo = press("# buttons by position, as on a Switch\neast = a\nsouth = b\nnorth = x\nwest = y\n", SOUTH, &line);
    expect(nintendo == B && !line, "south drives B in the Nintendo layout");
    expect(press("east = a\nsouth = b\nnorth = x\nwest = y", EAST | NORTH, &line) == (A | X) && !line, "last line without a newline");
    expect(press("  SOUTH\t=  ZR  \r\n\r\n", SOUTH, &line) == ZR && !line, "spaces, tabs, CRLF and upper case");
    expect(press("south = a # trailing comment\neast = a\n", SOUTH | EAST, &line) == A && !line, "two buttons may drive one Switch button");
    expect(press("menu = none\n", MENU | SOUTH, &line) == B && !line, "none leaves a button unused");
    expect(press("north = a\nnorth = b\n", NORTH, &line) == B && !line, "a later line for the same button wins");
    expect(press("east = a\nsout = b\n", SOUTH, &line) == B && line == 2, "unknown controller button: line 2, layout unchanged");
    expect(press("south = c\n", SOUTH, &line) == B && line == 1, "unknown Switch button");
    expect(press("south =\n", SOUTH, &line) == B && line == 1, "missing Switch button");
    expect(press("\n# note\nsouth b\n", SOUTH, &line) == B && line == 3, "missing equals sign");
    expect(press("= a\n", SOUTH, &line) == B && line == 1, "missing controller button");
    expect(mode_of("south = x tap\n", 0, &line) == CONTROLLER_TAP && !line, "tap mode");
    expect(mode_of("l3 = b TOGGLE\n", 4, &line) == CONTROLLER_TOGGLE && !line, "toggle mode, upper case");
    expect(mode_of("south = b hold\n", 0, &line) == CONTROLLER_HOLD && !line, "hold mode");
    expect(mode_of("south = b\n", 0, &line) == CONTROLLER_HOLD && !line, "no mode: hold");
    expect(mode_of("south = b tap extra\n", 0, &line) == -1 && line == 1, "two modes");
    expect(mode_of("south = b turbo\n", 0, &line) == -1 && line == 1, "unknown mode");
    expect(mode_of("menu = none tap\n", 10, &line) == -1 && line == 1, "none takes no mode");
    expect(mode_of("l2 = zr aim r2\n", 8, &line) == (CONTROLLER_AIM | 9 << 3) && !line, "aim with its fire button");
    expect(mode_of("l2 = zr aim touchpad\n", 8, &line) == (CONTROLLER_AIM | 16 << 3) && controller_mode_fire((uint8_t)(CONTROLLER_AIM | 16 << 3)) == 16 && !line, "the touchpad as a fire button");
    expect(press("", 1ULL << 16, &line) == 0 && !line, "the touchpad drives nothing by default");
    expect(press("touchpad = minus\n", 1ULL << 16, &line) == (1ULL << 11) && !line, "the touchpad opens the Purah Pad");
    /* The share button (Xbox Share, Switch Capture, DualSense Create) is physical button 22, the other one the settings
       panel opens on: also nothing until the layout file gives it a Switch button. */
    expect(press("", 1ULL << CONTROLLER_SHARE, &line) == 0 && !line, "the share button drives nothing by default");
    expect(press("share = minus\n", 1ULL << CONTROLLER_SHARE, &line) == (1ULL << 11) && !line, "the share button remapped");
    expect(mode_of("l2 = zr aim\n", 8, &line) == -1 && line == 1, "aim without a fire button");
    expect(mode_of("l2 = zr aim l2\n", 8, &line) == -1 && line == 1, "aim fired by itself");
    expect(mode_of("l2 = zr aim r2 extra\n", 8, &line) == -1 && line == 1, "aim with an extra word");
    /* Mouse buttons (runtime/gamepad.m, key 7 on the Mac) are physical buttons 17-21. Default: left attacks (Y), right
       draws the bow (ZR, released by a left click: the shot), middle targets (ZL), the side buttons drive nothing. */
    enum { ML = 17, MR = 18, MM = 19, M4 = 20, M5 = 21 };
    expect(CONTROLLER_BUTTONS == 23 && !strcmp(controller_physical_names[ML], "mouse_left") && !strcmp(controller_physical_names[MR], "mouse_right") &&
           !strcmp(controller_physical_names[MM], "mouse_middle") && !strcmp(controller_physical_names[M4], "mouse4") && !strcmp(controller_physical_names[M5], "mouse5") &&
           !strcmp(controller_physical_names[CONTROLLER_SHARE], "share"),
           "mouse button names");
    expect(press("", 1ULL << ML, &line) == Y && !line, "left click attacks (Y)");
    expect(press("", 1ULL << MR, &line) == ZR && mode_of("", MR, &line) == (CONTROLLER_AIM | ML << 3), "right button draws the bow, fired by the left");
    expect(press("", 1ULL << MM, &line) == (1 << 8), "middle button targets (ZL)");
    expect(press("", 1ULL << M4 | 1ULL << M5, &line) == 0, "side buttons drive nothing by default");
    expect(press("mouse_right = zl\nmouse4 = minus\n", 1ULL << MR | 1ULL << M4, &line) == ((1 << 8) | (1 << 11)) && !line, "mouse buttons remapped");
    expect(mode_of("mouse_right = zl\n", MR, &line) == CONTROLLER_HOLD, "a mouse line without a mode holds");
    expect(mode_of("mouse_middle = zr aim mouse5\n", MM, &line) == (CONTROLLER_AIM | M5 << 3), "a side button as a fire button");
    expect(mode_of("mouse_left = y tap\n", ML, &line) == CONTROLLER_TAP, "modes work on mouse buttons");
    if (failures) return 1;
    printf("PASS: controller layout cases\n");
    return 0;
}
