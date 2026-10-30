// CPU-only test of controller_map.h: "reach" lines (a worn stick direction scaled to full input) and that the button
// parser skips them. build: xcrun clang -std=c11 -Wall -Wextra -Werror -Iruntime tests/controller_reach/test.c -o <out> && <out>
#include <stdio.h>
#include <assert.h>
#include "controller_map.h"
int main(void) {
    uint16_t map[CONTROLLER_BUTTONS]; uint8_t modes[CONTROLLER_BUTTONS]; float reach[8]; const char *err = NULL;
    const char *ok = "# comment\nsouth = a\neast = b\nreach lstick down = 0.67   # worn\nREACH rstick Left=0.9\n";
    controller_map_default(map, modes);
    assert(controller_map_parse(ok, map, modes, &err) == 0);
    assert(map[0] == 1u << 0 && map[1] == 1u << 1);
    assert(controller_reach_parse(ok, reach, &err) == 0);
    assert(reach[1] > 0.669f && reach[1] < 0.671f && reach[6] > 0.89f && reach[0] == 1.0f && reach[3] == 1.0f);
    assert(controller_reach_parse("reach lstick sideways = 0.5\n", reach, &err) == 1); printf("bad dir: %s\n", err);
    assert(controller_reach_parse("reach lstick down = 0.1\n", reach, &err) == 1); printf("bad value: %s\n", err);
    assert(controller_reach_parse("reach lstick down\n", reach, &err) == 1); printf("no value: %s\n", err);
    assert(controller_reach_parse("reach lstick down = 0.7 extra\n", reach, &err) == 1);
    assert(controller_map_parse("reach lstick down = 0.67\nnorth = x\n", map, modes, &err) == 0);
    printf("all reach tests pass\n"); return 0;
}
