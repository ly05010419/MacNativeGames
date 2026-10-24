/* Prints what the C layout parser (games/totk/runtime/controller_map.h) makes of a controls.txt, for tests/test_prompt_layout.py.
   usage: controls_dump <controls.txt or -> <position|label>
   Output: "error <line>", or one "phys <i> <name> <Switch bits> <mode kind> <fire>" line per physical button, then one
   "pad <a|b|x|y> <Switch bits>" line per controller face button, through gamepad_face_positions (by position or by label). */
#include "../games/totk/runtime/controller_map.h"
#include "../games/totk/runtime/gamepad_layout.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: controls_dump <controls.txt or -> <position|label>\n"); return 2; }
    char text[16384] = "";
    if (strcmp(argv[1], "-")) {
        FILE *f = fopen(argv[1], "rb");
        if (!f) { perror(argv[1]); return 2; }
        size_t n = fread(text, 1, sizeof text - 1, f); text[n] = 0; fclose(f);
    }
    int by_position = !strcmp(argv[2], "position");
    uint16_t map[CONTROLLER_BUTTONS]; uint8_t modes[CONTROLLER_BUTTONS]; const char *error = NULL;
    controller_map_default(map, modes);
    int line = controller_map_parse(text, map, modes, &error);
    if (line) { printf("error %d\n", line); return 0; }
    for (int i = 0; i < CONTROLLER_BUTTONS; i++)
        printf("phys %d %s %u %d %d\n", i, controller_physical_names[i], map[i], controller_mode_kind(modes[i]),
               controller_mode_kind(modes[i]) == CONTROLLER_AIM ? controller_mode_fire(modes[i]) : -1);
    static const char pads[] = "abxy";
    for (int p = 0; p < 4; p++)
        printf("pad %c %llu\n", pads[p], (unsigned long long)controller_map_apply(map, gamepad_face_positions(p == 0, p == 1, p == 2, p == 3, by_position)));
    return 0;
}
