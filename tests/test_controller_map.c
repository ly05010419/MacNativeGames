/* Test the controller layout parser and mapping (games/totk/runtime/controller_map.h) with synthetic files. */
#include "../games/totk/runtime/controller_map.h"
#include "../games/totk/runtime/gamepad_layout.h"
#include <stdio.h>

enum { SOUTH = 1 << 0, EAST = 1 << 1, WEST = 1 << 2, NORTH = 1 << 3, MENU = 1 << 10 };
enum { A = 1 << 0, B = 1 << 1, X = 1 << 2, Y = 1 << 3, ZR = 1 << 9, PLUS = 1 << 10 };

static int failures;
static void expect(int ok, const char *what) { if (!ok) { fprintf(stderr, "FAIL: %s\n", what); failures++; } }

static uint64_t press(const char *text, uint64_t physical, int *line) {
    uint32_t keyboard[KEYBOARD_KEYS]; keyboard_map_default(keyboard);
    uint16_t map[CONTROLLER_BUTTONS]; uint8_t modes[CONTROLLER_BUTTONS]; const char *error = NULL;
    controller_map_default(map, modes);
    *line = controller_map_parse(text, map, modes, keyboard, &error);
    expect(!*line || error, "a bad line names its problem");
    return controller_map_apply(map, physical);
}

/* The mode and target of one physical button after parsing text, or -1 with *line set. */
static int mode_of(const char *text, int physical, int *line) {
    uint32_t keyboard[KEYBOARD_KEYS]; keyboard_map_default(keyboard);
    uint16_t map[CONTROLLER_BUTTONS]; uint8_t modes[CONTROLLER_BUTTONS]; const char *error = NULL;
    controller_map_default(map, modes);
    *line = controller_map_parse(text, map, modes, keyboard, &error);
    return *line ? -1 : modes[physical];
}

int main(int argc, char **argv) {
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
    expect(mode_of("l2 = zr aim\n", 8, &line) == -1 && line == 1, "aim without a fire button");
    expect(mode_of("l2 = zr aim l2\n", 8, &line) == -1 && line == 1, "aim fired by itself");
    expect(mode_of("l2 = zr aim r2 extra\n", 8, &line) == -1 && line == 1, "aim with an extra word");
    /* Mouse buttons (runtime/gamepad.m, key 7 on the Mac) are physical buttons 17-21. Default: left attacks (Y), right
       draws the bow (ZR, released by a left click: the shot), middle targets (ZL), the side buttons drive nothing. */
    enum { ML = 17, MR = 18, MM = 19, M4 = 20, M5 = 21 };
    expect(CONTROLLER_BUTTONS == 22 && !strcmp(controller_physical_names[ML], "mouse_left") && !strcmp(controller_physical_names[MR], "mouse_right") &&
           !strcmp(controller_physical_names[MM], "mouse_middle") && !strcmp(controller_physical_names[M4], "mouse4") && !strcmp(controller_physical_names[M5], "mouse5"),
           "mouse button names");
    expect(press("", 1ULL << ML, &line) == Y && !line, "left click attacks (Y)");
    expect(press("", 1ULL << MR, &line) == ZR && mode_of("", MR, &line) == (CONTROLLER_AIM | ML << 3), "right button draws the bow, fired by the left");
    expect(press("", 1ULL << MM, &line) == (1 << 8), "middle button targets (ZL)");
    expect(press("", 1ULL << M4 | 1ULL << M5, &line) == 0, "side buttons drive nothing by default");
    expect(press("zl = mouse_right\nminus = mouse4\n", 1ULL << MR | 1ULL << M4, &line) == ((1 << 8) | (1 << 11)) && !line, "mouse buttons remapped");
    expect(mode_of("zl = mouse_right\n", MR, &line) == CONTROLLER_HOLD, "a mouse line without a mode holds");
    uint32_t keyboard[KEYBOARD_KEYS]; keyboard_map_default(keyboard);
    uint16_t map[CONTROLLER_BUTTONS]; uint8_t modes[CONTROLLER_BUTTONS]; controller_map_default(map, modes);
    const char *error = NULL;
    expect(keyboard[49] == B && keyboard[13] == (1u << 18), "default keyboard buttons and movement");
    line = controller_map_parse("x = key_space\nb = key_k key_i mouse_left\na = key_e\nleft_stick_up = key_j\nminus = mouse4\n", map, modes, keyboard, &error);
    expect(!line && keyboard[49] == X && keyboard[40] == B && keyboard[34] == B && keyboard[14] == A &&
           keyboard[38] == (1u << 18) && !keyboard[13] && map[ML] == B && map[M4] == (1 << 11), "action-based keyboard and mouse layout");
    line = controller_map_parse("b =   # unbound\n", map, modes, keyboard, &error);
    expect(!line && !keyboard[40] && !keyboard[34] && !map[ML], "blank action removes every keyboard and mouse binding");
    line = controller_map_parse("x = key_k\na = key_typo\n", map, modes, keyboard, &error);
    expect(line == 2 && keyboard[49] == X && !keyboard[40], "invalid binding leaves all arrays unchanged");
    expect(controller_map_parse("x = key_1", map, modes, keyboard, &error) == 1, "settings key is reserved");
    expect(controller_map_parse("X = KEY_I KEY_SPACE", map, modes, keyboard, &error) == 0 && keyboard[34] == X && keyboard[49] == X, "multiple keys and case-insensitive names");
    expect(controller_map_parse("x = key_k\nx = key_space", map, modes, keyboard, &error) == 0 && !keyboard[40] && keyboard[49] == X, "later action replaces its earlier bindings");
    keyboard_map_default(keyboard);
    expect(!controller_map_parse("  \n# empty\n", map, modes, keyboard, &error), "blank layout parses");
    for (int i=0;i<KEYBOARD_KEYS;i++) expect(!keyboard[i], "blank file disables all keyboard gameplay");
    uint32_t defaultKeyboard[KEYBOARD_KEYS]; keyboard_map_default(defaultKeyboard);
    uint16_t defaultMap[CONTROLLER_BUTTONS]; uint8_t defaultModes[CONTROLLER_BUTTONS]; controller_map_default(defaultMap,defaultModes);
    controller_map_default(map,modes);keyboard_map_default(keyboard);
    expect(!controller_map_parse(controls_default_text,map,modes,keyboard,&error) &&
           !memcmp(keyboard,defaultKeyboard,sizeof keyboard) && !memcmp(map,defaultMap,sizeof map) &&
           !memcmp(modes,defaultModes,sizeof modes), "generated default file reproduces the previous bindings");
    for(int i=1;i<=7;i++) {
        char text[32];snprintf(text,sizeof text,"x = key_%d",i);
        expect(controller_map_parse(text,map,modes,keyboard,&error)==1,"keys 1-7 cannot be rebound");
    }
    if(argc==2) {
        expect(!controls_file_create(argv[1]),"missing controls file is created");
        char text[sizeof controls_default_text]={0};FILE *f=fopen(argv[1],"r");
        expect(f!=NULL,"created controls file opens");
        if(f){fread(text,1,sizeof text-1,f);fclose(f);}
        expect(!strcmp(text,controls_default_text),"created file contains default bindings");
        f=fopen(argv[1],"w");if(f)fclose(f);
        expect(!controls_file_create(argv[1]),"existing blank file is preserved");
        f=fopen(argv[1],"r");expect(f&&fgetc(f)==EOF,"existing blank file stays blank");if(f)fclose(f);
    }
    if (failures) return 1;
    printf("PASS: controller layout cases\n");
    return 0;
}
