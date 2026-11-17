/* Checks runtime/keyboard_map.h and runtime/controller_map.h (tests/test_eow_input.py compiles and runs it, as games/acnh's
   tests/input_probe.c). With "dump" it prints the default keyboard layout instead: action, then the labels of its inputs,
   tab separated; with "keys" what each Mac key code drives under the default: code, buttons, lx, ly, rx, ry. */
#include "keyboard_map.h"
#include "controller_map.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static void dump(void) {
    KeyMap m; keymap_default(&m);
    for (int a = 0; a < KEYMAP_ACTIONS; a++) {
        printf("%s", keymap_action_name(a));
        for (int s = 0; s < KEYMAP_SLOTS; s++) if (m.slot[a][s] != KEYMAP_NONE) printf("\t%s", keymap_input_label(m.slot[a][s]));
        printf("\n");
    }
}

static void keys(void) {
    KeyMap m; keymap_default(&m);
    for (int code = 0; code < KEYMAP_KEYS; code++) {
        unsigned char held[KEYMAP_INPUTS] = {0}; held[code] = 1;
        KeyMapOutput o = keymap_apply(&m, held);
        printf("%d\t%llu\t%d\t%d\t%d\t%d\n", code, (unsigned long long)o.buttons, o.lx, o.ly, o.rx, o.ry);
    }
}

static void keyboard(void) {
    KeyMap m; keymap_default(&m);
    /* Every action has an input, every input is bindable and used once, key 1 is never one, and the mouse drives nothing. */
    int used[KEYMAP_INPUTS] = {0};
    for (int a = 0; a < KEYMAP_ACTIONS; a++) {
        CHECK(m.slot[a][0] != KEYMAP_NONE);
        for (int s = 0; s < KEYMAP_SLOTS; s++) {
            int in = m.slot[a][s]; if (in == KEYMAP_NONE) continue;
            CHECK(keymap_bindable(in)); CHECK(!used[in]); used[in] = 1; CHECK(in < KEYMAP_MOUSE_FIRST);
        }
    }
    CHECK(!used[KEYMAP_SETTINGS_KEY] && !used[19] && !used[20] && !used[21]);   /* keys 1-4: the panel, and no shortcuts */
    /* J is A, K and Space B, W A S D move, the arrows the right stick, P the screenshot (which drives nothing in the game). */
    unsigned char held[KEYMAP_INPUTS] = {0};
    held[38] = 1; KeyMapOutput o = keymap_apply(&m, held); CHECK(o.buttons == 1); held[38] = 0;
    held[40] = 1; held[49] = 1; o = keymap_apply(&m, held); CHECK(o.buttons == 2); held[40] = held[49] = 0;
    held[13] = 1; held[2] = 1; o = keymap_apply(&m, held); CHECK(o.ly == 32767 && o.lx == 32767 && !o.buttons); held[13] = held[2] = 0;
    held[123] = 1; o = keymap_apply(&m, held); CHECK(o.rx == -32767 && !o.ry); held[123] = 0;
    held[35] = 1; o = keymap_apply(&m, held); CHECK(!o.buttons && !o.lx && !o.ly && !o.rx && !o.ry); held[35] = 0;
    CHECK(keymap_action_of(&m, 35, NULL) == KEYMAP_SCREENSHOT);
    /* The file round trip: format, parse back, the same layout. */
    char text[8192]; CHECK(keymap_format(&m, text, sizeof text) > 0);
    KeyMap back; memset(&back, KEYMAP_NONE, sizeof back);
    const char *error = NULL; CHECK(keymap_parse(text, &back, &error) == 0); CHECK(!memcmp(&back, &m, sizeof m));
    /* Binding a key another action holds swaps them; key 1 cannot be bound; a file line naming key 1 is refused. */
    KeyMap b = m; CHECK(keymap_bind(&b, KEYMAP_X, 0, 38));
    CHECK(b.slot[KEYMAP_X][0] == 38 && b.slot[KEYMAP_A][0] == 34);
    CHECK(!keymap_bind(&b, KEYMAP_A, 0, KEYMAP_SETTINGS_KEY));
    KeyMap c; keymap_default(&c); CHECK(keymap_parse("a = 1\n", &c, &error) == 1);
    /* A file that gives Space to A leaves B with K; the left button can be given an action. */
    keymap_default(&c); CHECK(keymap_parse("a = space mouse_left\n", &c, &error) == 0);
    CHECK(c.slot[KEYMAP_A][0] == 49 && c.slot[KEYMAP_A][1] == KEYMAP_MOUSE_FIRST && c.slot[KEYMAP_B][0] == 40 && c.slot[KEYMAP_B][1] == KEYMAP_NONE);
}

static void controller(void) {
    uint16_t map[CONTROLLER_BUTTONS]; uint8_t modes[CONTROLLER_BUTTONS];
    controller_map_default(map, modes);
    CHECK(controller_map_target(map[1]) == 0 && controller_map_target(map[0]) == 1);   /* east A, south B */
    CHECK(controller_map_target(map[16]) == -1);   /* the touchpad drives nothing (it takes the screenshot when held) */
    char text[4096]; int n = controller_map_format(map, modes, text, sizeof text);
    CHECK(n > 0);
    for (const char *line = text; *line; line = strchr(line, '\n') + 1) CHECK(line[0] == '#');   /* the default: comments only */
    controller_map_assign(map, modes, 2, 0);      /* west drives A */
    controller_map_assign(map, modes, 16, 7);     /* the touchpad drives R */
    controller_map_assign(map, modes, 6, -1);     /* L1 drives nothing */
    modes[3] = CONTROLLER_TOGGLE;
    CHECK(controller_map_format(map, modes, text, sizeof text) > 0);
    CHECK(strstr(text, "west = a\n") && strstr(text, "touchpad = r\n") && strstr(text, "l1 = none\n") && strstr(text, "north = x toggle\n"));
    uint16_t back[CONTROLLER_BUTTONS]; uint8_t backModes[CONTROLLER_BUTTONS]; controller_map_default(back, backModes);
    const char *error = NULL; CHECK(controller_map_parse(text, back, backModes, &error) == 0);
    CHECK(!memcmp(back, map, sizeof map) && !memcmp(backModes, modes, sizeof modes));
    /* A hold of the screenshot button fires once, after CONTROLLER_SCREENSHOT_HOLD_MS. */
    int fire = 0; uint64_t need = CONTROLLER_SCREENSHOT_HOLD_MS * 1000000ull;
    ControllerHold h = controller_hold_next((ControllerHold){0}, 1, 1000, need, &fire); CHECK(!fire && h.down);
    h = controller_hold_next(h, 1, 1000 + need, need, &fire); CHECK(fire);
    h = controller_hold_next(h, 1, 2000 + need, need, &fire); CHECK(!fire);
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "dump")) { dump(); return 0; }
    if (argc == 2 && !strcmp(argv[1], "keys")) { keys(); return 0; }
    keyboard(); controller();
    if (failures) { printf("%d failed\n", failures); return 1; }
    printf("PASS\n");
    return 0;
}
