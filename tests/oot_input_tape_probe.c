/* Probe of games/oot/runtime/input_tape.h (tests/test_oot_input_tape.py): records a short route, then replays it with
   the world entered 50 frames later, and checks what the game would read. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "input_tape.h"
/* defined after the header, as in the game, where they live in other files (the header only declares them weakly) */
static uint64_t g_frame, g_world; static int g_closed;
uint64_t dm_nvn_frame_count(void) { return g_frame; }
uint64_t mtl_world_frame(void) { return g_world; }
void runtime_front_request_close(void) { g_closed = 1; }

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    if (argv[1][0] == 'r' && argv[1][2] == 'c') {   /* record: the user's route */
        for (g_frame = 1; g_frame <= 1000; g_frame++) {
            if (g_frame == 100) g_world = 100;
            uint64_t b = g_frame >= 10 && g_frame < 20 ? 1 : 0;   /* a button on the title, frames 10-19 */
            int32_t lx = g_world && g_frame >= 105 && g_frame < 300 ? 20000 : 0, ly = 0, rx = 0, ry = 0;   /* walking, W 5-199 */
            input_tape(&b, &lx, &ly, &rx, &ry);
        }
        return 0;
    }
    /* replay: the world comes at frame 150 this time */
    int checks[][3] = { {12, 1, 0}, {25, 0, 0}, {120, 0, 0}, {155, 0, 20000}, {349, 0, 20000}, {350, 0, 0} };
    int failed = 0;
    for (g_frame = 1; g_frame <= 1100; g_frame++) {
        if (g_frame == 150) g_world = 150;
        uint64_t b = 7; int32_t lx = 9, ly = 9, rx = 9, ry = 9;   /* the real input, to be replaced */
        input_tape(&b, &lx, &ly, &rx, &ry);
        for (size_t i = 0; i < sizeof checks / sizeof checks[0]; i++)
            if ((int)g_frame == checks[i][0] && ((int)b != checks[i][1] || lx != checks[i][2] || ly != 0 || rx != 0 || ry != 0)) {
                printf("frame %llu: buttons %llu lx %d, want %d %d\n", (unsigned long long)g_frame, (unsigned long long)b, lx, checks[i][1], checks[i][2]); failed = 1; }
        if (g_closed) { printf("closed at frame %llu\n", (unsigned long long)g_frame); break; }
    }
    if (!g_closed) { printf("never closed\n"); failed = 1; }
    return failed;
}
