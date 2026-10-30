/* Checks runtime/keyboard_map.h and runtime/controller_map.h (tests/test_acnh_input.py compiles and runs it). With
   "dump" it prints the default keyboard layout instead: action, then the labels of its inputs, tab separated. */
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

static void keyboard(void) {
    KeyMap m; keymap_default(&m);
    /* Every action has an input, every input is bindable and used once, and key 1 is never one. */
    int used[KEYMAP_INPUTS] = {0};
    for (int a = 0; a < KEYMAP_ACTIONS; a++) {
        CHECK(m.slot[a][0] != KEYMAP_NONE);
        for (int s = 0; s < KEYMAP_SLOTS; s++) {
            int in = m.slot[a][s]; if (in == KEYMAP_NONE) continue;
            CHECK(keymap_bindable(in)); CHECK(!used[in]); used[in] = 1;
        }
    }
    CHECK(!used[KEYMAP_SETTINGS_KEY]);
    /* Space and the left button are A; Shift and Esc are B; W A S D move; the arrows turn the camera. */
    unsigned char held[KEYMAP_INPUTS] = {0};
    held[49] = 1; KeyMapOutput o = keymap_apply(&m, held); CHECK(o.buttons == 1); held[49] = 0;
    held[KEYMAP_MOUSE_FIRST] = 1; o = keymap_apply(&m, held); CHECK(o.buttons == 1); held[KEYMAP_MOUSE_FIRST] = 0;
    held[56] = 1; held[53] = 1; o = keymap_apply(&m, held); CHECK(o.buttons == 2); held[56] = held[53] = 0;
    held[13] = 1; held[2] = 1; o = keymap_apply(&m, held); CHECK(o.ly == 32767 && o.lx == 32767 && !o.buttons); held[13] = held[2] = 0;
    held[123] = 1; o = keymap_apply(&m, held); CHECK(o.rx == -32767 && !o.ry); held[123] = 0;
    /* Unlike Tears of the Kingdom, A and Y together are both held (no bow rule), and R with an arrow stays R and camera. */
    held[49] = 1; held[14] = 1; o = keymap_apply(&m, held); CHECK(o.buttons == (1u | 8u)); held[49] = held[14] = 0;
    held[8] = 1; held[126] = 1; o = keymap_apply(&m, held); CHECK(o.buttons == 1u << KEYMAP_R && o.ry == 32767); held[8] = held[126] = 0;
    /* The file round trip: format, parse back, the same layout. */
    char text[8192]; CHECK(keymap_format(&m, text, sizeof text) > 0);
    KeyMap back; keymap_default(&back); memset(&back, KEYMAP_NONE, sizeof back);
    const char *error = NULL; CHECK(keymap_parse(text, &back, &error) == 0); CHECK(!memcmp(&back, &m, sizeof m));
    /* Binding a key another action holds swaps them; key 1 cannot be bound; a file line naming key 1 is refused. */
    KeyMap b = m; CHECK(keymap_bind(&b, KEYMAP_X, 0, 49));
    CHECK(b.slot[KEYMAP_X][0] == 49 && b.slot[KEYMAP_A][0] == 48);
    CHECK(!keymap_bind(&b, KEYMAP_A, 0, KEYMAP_SETTINGS_KEY));
    KeyMap c; keymap_default(&c); CHECK(keymap_parse("a = 1\n", &c, &error) == 1);
    /* A file that gives Space to X leaves A with its other input. */
    keymap_default(&c); CHECK(keymap_parse("x = space\n", &c, &error) == 0);
    CHECK(c.slot[KEYMAP_X][0] == 49 && c.slot[KEYMAP_A][0] == KEYMAP_MOUSE_FIRST && c.slot[KEYMAP_A][1] == KEYMAP_NONE);
    /* The drawing's assignment moves a key from one action to another without swapping. */
    KeyMap d = m; CHECK(keymap_assign(&d, 14, KEYMAP_ZL));
    CHECK(d.slot[KEYMAP_Y][0] == KEYMAP_MOUSE_FIRST + 1 && d.slot[KEYMAP_ZL][1] == 14);
}

static void controller(void) {
    uint16_t map[CONTROLLER_BUTTONS]; uint8_t modes[CONTROLLER_BUTTONS];
    controller_map_default(map, modes);
    CHECK(controller_map_target(map[1]) == 0 && controller_map_target(map[0]) == 1);   /* east A, south B */
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
    CHECK(controller_map_target(0) == -1);
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "dump")) { dump(); return 0; }
    keyboard(); controller();
    if (failures) { printf("%d failed\n", failures); return 1; }
    printf("PASS\n");
    return 0;
}
