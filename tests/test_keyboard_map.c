/* Test the keyboard and mouse layout (games/totk/runtime/keyboard_map.h) with synthetic files: the default, the parser,
   the writer, binding with swaps, and what held inputs drive (the bow's shot; R with the arrows in mouse mode). */
#include "../games/totk/runtime/keyboard_map.h"
#include <stdio.h>

static int failures;
static void expect(int ok, const char *what) { if (!ok) { fprintf(stderr, "FAIL: %s\n", what); failures++; } }

enum { KEY_A = 0, KEY_S = 1, KEY_D = 2, KEY_F = 3, KEY_G = 5, KEY_C = 8, KEY_V = 9, KEY_Q = 12, KEY_W = 13, KEY_E = 14,
       KEY_R = 15, KEY_1 = 18, KEY_2 = 19, KEY_3 = 20, KEY_4 = 21, KEY_5 = 23, KEY_RETURN = 36, KEY_J = 38, KEY_K = 40,
       KEY_M = 46, KEY_TAB = 48, KEY_SPACE = 49, KEY_BACKSPACE = 51, KEY_ESCAPE = 53, KEY_SHIFT = 56, KEY_OPTION = 58,
       KEY_CONTROL = 59, KEY_LEFT = 123, KEY_RIGHT = 124, KEY_DOWN = 125, KEY_UP = 126,
       MOUSE_LEFT = KEYMAP_MOUSE_FIRST, MOUSE_RIGHT, MOUSE_MIDDLE };

static KeyMapOutput hold_in(const KeyMap *m, const int *inputs, int n, int mouse_camera) {
    unsigned char held[KEYMAP_INPUTS] = {0};
    for (int i = 0; i < n; i++) held[inputs[i]] = 1;
    return keymap_apply(m, held, mouse_camera);
}
static KeyMapOutput hold(const KeyMap *m, const int *inputs, int n) { return hold_in(m, inputs, n, 0); }
static uint64_t buttons_of(const KeyMap *m, int input) { return hold(m, &input, 1).buttons; }
static int parse(const char *text, KeyMap *m, int *line) {
    const char *error = NULL; keymap_default(m);
    *line = keymap_parse(text, m, &error);
    expect(!*line || error, "a bad line names its problem");
    return *line;
}

int main(void) {
    KeyMap m, d; int line;
    keymap_default(&d);
    /* The default (PR #1210): left hand on the keyboard, right hand on the mouse. */
    expect(buttons_of(&d, KEY_E) == 1u << KEYMAP_A && buttons_of(&d, KEY_RETURN) == 1u << KEYMAP_A, "E and Return confirm (A)");
    expect(buttons_of(&d, KEY_SHIFT) == 1u << KEYMAP_B && buttons_of(&d, KEY_BACKSPACE) == 1u << KEYMAP_B, "Shift and Backspace cancel and sprint (B)");
    expect(buttons_of(&d, KEY_SPACE) == 1u << KEYMAP_X, "Space jumps (X)");
    expect(buttons_of(&d, MOUSE_LEFT) == 1u << KEYMAP_Y && buttons_of(&d, KEY_G) == 1u << KEYMAP_Y, "left click and G attack (Y)");
    expect(buttons_of(&d, MOUSE_RIGHT) == 1u << KEYMAP_ZR && buttons_of(&d, KEY_OPTION) == 1u << KEYMAP_ZR, "right click and Option draw the bow (ZR)");
    expect(buttons_of(&d, KEY_Q) == 1u << KEYMAP_ZL && buttons_of(&d, MOUSE_MIDDLE) == 1u << KEYMAP_ZL, "Q and the wheel click lock on (ZL)");
    expect(buttons_of(&d, KEY_ESCAPE) == 1u << KEYMAP_PLUS && buttons_of(&d, KEY_TAB) == 1u << KEYMAP_MINUS && buttons_of(&d, KEY_M) == 1u << KEYMAP_MINUS,
           "Esc opens the menu (+); Tab and M the map (-): no longer the same");
    expect(buttons_of(&d, KEY_F) == 1u << KEYMAP_L && buttons_of(&d, KEY_R) == 1u << KEYMAP_R, "F ability (L), R throw (R)");
    expect(buttons_of(&d, KEY_CONTROL) == 1u << KEYMAP_LSTICK && buttons_of(&d, KEY_C) == 1u << KEYMAP_LSTICK && buttons_of(&d, KEY_V) == 1u << KEYMAP_RSTICK,
           "Control / C crouch (L3), V looks (R3)");
    expect(buttons_of(&d, KEY_2) == 1u << KEYMAP_DLEFT && buttons_of(&d, KEY_3) == 1u << KEYMAP_DUP && buttons_of(&d, KEY_4) == 1u << KEYMAP_DRIGHT &&
           buttons_of(&d, KEY_5) == 1u << KEYMAP_DDOWN, "2 3 4 5: the d-pad");
    expect(!buttons_of(&d, KEY_J) && !buttons_of(&d, KEY_K) && !buttons_of(&d, KEY_1), "J, K and key 1 drive nothing");
    {
        int wd[] = {KEY_W, KEY_D, KEY_LEFT, KEY_DOWN}; KeyMapOutput o = hold(&d, wd, 4);
        expect(!o.buttons && o.lx == 32767 && o.ly == 32767 && o.rx == -32767 && o.ry == -32767, "W A S D move, the arrows turn the camera");
        int ad[] = {KEY_A, KEY_D, KEY_UP, KEY_RIGHT}; o = hold(&d, ad, 4);
        expect(!o.lx && !o.ly && o.rx == 32767 && o.ry == 32767, "opposite keys cancel");
    }
    /* The bow: while ZR is held, attack shoots (both let go). */
    {
        int both[] = {MOUSE_RIGHT, MOUSE_LEFT}, mixed[] = {KEY_OPTION, MOUSE_LEFT};
        expect(!hold(&d, both, 2).buttons && !hold(&d, mixed, 2).buttons, "drawing and attack: the shot");
        int jump[] = {MOUSE_RIGHT, KEY_SPACE};
        expect(hold(&d, jump, 2).buttons == (1u << KEYMAP_ZR | 1u << KEYMAP_X), "other buttons while drawing");
    }
    /* Rotating (Ultrahand: R with the d-pad): in mouse mode, R with the arrows drives the d-pad, not the camera. */
    {
        int left[] = {KEY_R, KEY_LEFT}, up[] = {KEY_R, KEY_UP}, two[] = {KEY_R, KEY_RIGHT, KEY_DOWN}, alone[] = {KEY_LEFT};
        KeyMapOutput o = hold_in(&d, left, 2, 1);
        expect(o.buttons == (1u << KEYMAP_R | 1u << KEYMAP_DLEFT) && !o.rx && !o.ry, "mouse mode: R + left arrow is R + d-pad left");
        expect(hold_in(&d, up, 2, 1).buttons == (1u << KEYMAP_R | 1u << KEYMAP_DUP), "mouse mode: R + up arrow is R + d-pad up");
        expect(hold_in(&d, two, 3, 1).buttons == (1u << KEYMAP_R | 1u << KEYMAP_DRIGHT | 1u << KEYMAP_DDOWN), "two arrows, two d-pad directions");
        o = hold_in(&d, alone, 1, 1);
        expect(!o.buttons && o.rx == -32767, "mouse mode, no R: the arrows still turn the camera");
        o = hold_in(&d, left, 2, 0);
        expect(o.buttons == 1u << KEYMAP_R && o.rx == -32767, "no mouse mode: R + arrows aims (the camera), as before");
        int moving[] = {KEY_R, KEY_W};
        o = hold_in(&d, moving, 2, 1);
        expect(o.buttons == 1u << KEYMAP_R && o.ly == 32767, "R does not change moving");
    }
    int seen[KEYMAP_INPUTS] = {0}, unique = 1;
    for (int a = 0; a < KEYMAP_ACTIONS; a++)
        for (int s = 0; s < KEYMAP_SLOTS; s++) {
            int input = d.slot[a][s];
            if (input == KEYMAP_NONE) continue;
            if (seen[input]++ || !keymap_bindable(input)) unique = 0;
        }
    expect(unique, "every default input is bindable and drives one action");
    expect(!keymap_bindable(KEY_1) && !keymap_bindable(55) && !keymap_bindable(57) && !keymap_bindable(140) && keymap_bindable(MOUSE_MIDDLE),
           "key 1, Command, Caps Lock and unknown inputs are not bindable");

    /* Parsing. */
    expect(!parse("", &m, &line) && !memcmp(&m, &d, sizeof m), "no lines: the default");
    expect(!parse("# my layout\n  ZL = e   # lock on with E\r\n", &m, &line) && m.slot[KEYMAP_ZL][0] == KEY_E && m.slot[KEYMAP_ZL][1] == KEYMAP_NONE,
           "one action, a comment, CRLF, upper case");
    expect(m.slot[KEYMAP_A][0] == KEY_RETURN && m.slot[KEYMAP_A][1] == KEYMAP_NONE, "an input given to another action leaves the default (Return moves up)");
    expect(m.slot[KEYMAP_PLUS][0] == KEY_ESCAPE, "actions not named keep the default");
    expect(!parse("a = j k\nb = none\nmove_up = arrow_up\ncamera_up = w", &m, &line) && m.slot[KEYMAP_A][0] == KEY_J && m.slot[KEYMAP_A][1] == KEY_K &&
           m.slot[KEYMAP_B][0] == KEYMAP_NONE && m.slot[KEYMAP_MOVE_UP][0] == KEY_UP && m.slot[KEYMAP_CAMERA_UP][0] == KEY_W, "two inputs, none, swapped sticks");
    expect(!parse("y = mouse_middle\nzl = q", &m, &line) && m.slot[KEYMAP_Y][0] == MOUSE_MIDDLE && m.slot[KEYMAP_ZL][0] == KEY_Q &&
           m.slot[KEYMAP_ZL][1] == KEYMAP_NONE, "mouse buttons by name");
    expect(parse("a = e\nfly = q\n", &m, &line) && line == 2 && !memcmp(&m, &d, sizeof m), "unknown action: line 2, layout unchanged");
    expect(parse("a = hyperspace\n", &m, &line) && line == 1, "unknown input");
    expect(parse("a = 1\n", &m, &line) && line == 1, "key 1 opens the settings panel");
    expect(parse("a = e\nb = e\n", &m, &line) && line == 2, "one input for two actions");
    expect(parse("a = e e\n", &m, &line) && line == 1, "the same input twice");
    expect(parse("a = e return space\n", &m, &line) && line == 1, "three inputs");
    expect(parse("a = none e\n", &m, &line) && line == 1, "none with an input");
    expect(parse("a =\n", &m, &line) && line == 1, "missing input");
    expect(parse("a e\n", &m, &line) && line == 1, "missing equals sign");
    expect(parse("a = e\na = q\n", &m, &line) && line == 2, "an action twice");

    /* Writing: every action, read back the same. */
    {
        char text[8192]; const char *error = NULL;
        m = d; keymap_bind(&m, KEYMAP_X, 1, KEY_J); keymap_bind(&m, KEYMAP_B, 1, KEYMAP_NONE);
        int n = keymap_format(&m, text, sizeof text);
        KeyMap back; keymap_default(&back);
        expect(n > 0 && !keymap_parse(text, &back, &error) && !memcmp(&back, &m, sizeof m), "written and read back");
        expect(strstr(text, "x = space j\n") && strstr(text, "b = shift\n") && strstr(text, "y = mouse_left g\n") && strstr(text, "move_up = w\n"), "line forms");
        KeyMap empty; memset(&empty, KEYMAP_NONE, sizeof empty);
        n = keymap_format(&empty, text, sizeof text); keymap_default(&back);
        expect(n > 0 && !keymap_parse(text, &back, &error) && !memcmp(&back, &empty, sizeof empty), "an empty layout too");
        expect(keymap_format(&d, text, 100) == -1, "too small a buffer: -1");
    }

    /* Binding: an input another slot holds swaps places with this one's. */
    m = d;
    expect(keymap_bind(&m, KEYMAP_X, 0, KEY_Q) && m.slot[KEYMAP_X][0] == KEY_Q && m.slot[KEYMAP_ZL][0] == KEY_SPACE, "Q to jump: lock on takes Space");
    expect(keymap_bind(&m, KEYMAP_X, 1, KEY_J) && m.slot[KEYMAP_X][1] == KEY_J, "a free input");
    expect(keymap_bind(&m, KEYMAP_X, 1, KEY_Q) && m.slot[KEYMAP_X][0] == KEY_J && m.slot[KEYMAP_X][1] == KEY_Q, "within one action: the slots swap");
    expect(keymap_bind(&m, KEYMAP_A, 1, KEYMAP_NONE) && m.slot[KEYMAP_A][1] == KEYMAP_NONE && !buttons_of(&m, KEY_RETURN), "cleared");
    expect(!keymap_bind(&m, KEYMAP_A, 1, KEY_1) && !keymap_bind(&m, KEYMAP_ACTIONS, 0, KEY_J) && !keymap_bind(&m, KEYMAP_A, 2, KEY_J), "refused binds");
    expect(keymap_action_of(&m, KEY_SPACE, &line) == KEYMAP_ZL && line == 0 && keymap_action_of(&m, KEY_RETURN, NULL) < 0, "the action of an input");
    expect(keymap_input_parse("MOUSE_LEFT", 10) == MOUSE_LEFT && keymap_input_parse("f12", 3) == 111 && keymap_input_parse("nope", 4) < 0, "input names");
    expect(!strcmp(keymap_input_label(KEY_ESCAPE), "Esc") && !strcmp(keymap_input_label(MOUSE_MIDDLE), "Wheel click") && !keymap_input_label(140), "labels");

    /* The drawing's assignment: a key now drives an action, nothing swaps. */
    m = d;
    expect(keymap_assign(&m, KEY_E, KEYMAP_X) && m.slot[KEYMAP_X][0] == KEY_SPACE && m.slot[KEYMAP_X][1] == KEY_E &&
           m.slot[KEYMAP_A][0] == KEY_RETURN && m.slot[KEYMAP_A][1] == KEYMAP_NONE, "E to jump: the free slot; confirm keeps Return, moved up");
    expect(keymap_assign(&m, KEY_J, KEYMAP_X) && m.slot[KEYMAP_X][0] == KEY_SPACE && m.slot[KEYMAP_X][1] == KEY_J && keymap_action_of(&m, KEY_E, NULL) < 0,
           "both slots taken: the second one is replaced, its key unused");
    expect(keymap_assign(&m, KEY_RETURN, -1) && m.slot[KEYMAP_A][0] == KEYMAP_NONE && !buttons_of(&m, KEY_RETURN), "none");
    expect(keymap_assign(&m, KEY_SPACE, KEYMAP_X) && m.slot[KEYMAP_X][0] == KEY_SPACE && m.slot[KEYMAP_X][1] == KEY_J, "the action it already drives: unchanged");
    expect(keymap_assign(&m, MOUSE_MIDDLE, KEYMAP_MOVE_UP) && m.slot[KEYMAP_MOVE_UP][1] == MOUSE_MIDDLE && m.slot[KEYMAP_ZL][0] == KEY_Q &&
           m.slot[KEYMAP_ZL][1] == KEYMAP_NONE, "a mouse button to a direction");
    expect(!keymap_assign(&m, KEY_1, KEYMAP_A) && !keymap_assign(&m, KEY_E, KEYMAP_ACTIONS) && !keymap_assign(&m, KEY_E, -2), "refused");

    if (failures) return 1;
    printf("PASS: keyboard layout cases\n");
    return 0;
}
