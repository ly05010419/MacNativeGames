// Age of Calamity's cutscenes advance a fixed step per frame, so above 30 fps they run fast (FPSLocker's notes for this game say the same, and it has no fix). While one plays the
// frame step goes back to the game's own 30 fps; the chosen target comes back when it ends. Nothing in the game says "cutscene" outright. What marks it is the battle screen's
// state: main+ui points at an object whose +0x2ea and +0x2eb bytes are 1 while the battle interface is up, and both 0 when a cutscene has closed it (written by main+0x861700 in
// 1.4.1; the title screen has +0x2eb at 1). Compared in memory captures of 1.4.1, 6 in play against 4 in a cutscene. Loading screens may read the same, and 30 fps does no harm there.
// A build not listed keeps its target through cutscenes.
#include "dm.h"
#include "../gfx/nvn/aoc_timing.h"
#include <pthread.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
int dm_guest_heap_readable(uint64_t a, size_t n);
enum { UI_OPEN = 0x2ea, UI_SIZE = 0x300, POLL_US = 10000, RELEASE_POLLS = 10 };   // released 100 ms after the mark goes, so a flicker between two screens does not switch twice
static const struct { const char *build; uint32_t ui; } k_builds[] = {
    { "c3cf52bf2b05d7310884971b50bcccb8", 0x277f8c0 },   // 1.3.0: the same writer at the same offset, checked by disassembly; not played yet
    { "e5f275b5d0889899e55de1a164eef060", 0x27838c0 },   // 1.4.1
};
static const uint8_t *g_ui_slot;
static int cutscene(void) {
    uint64_t ui; memcpy(&ui, g_ui_slot, 8);
    if (!ui || !dm_guest_heap_readable(ui, UI_SIZE)) return 0;
    const uint8_t *p = (const uint8_t *)(uintptr_t)ui;
    return !p[UI_OPEN] && !p[UI_OPEN + 1];
}
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static void *watch(void *arg) {
    (void)arg; int held = 0, clear = 0; double since = 0;
    for (;; usleep(POLL_US)) {
        if (cutscene()) { clear = 0; if (!held) { held = 1; since = now(); aoc_timing_hold(1); LOG("AoC cutscene: held at the game's 30 fps"); } }
        else if (held && ++clear >= RELEASE_POLLS) { held = 0; aoc_timing_hold(0); LOG("AoC cutscene over after %.1f s: back to the chosen target", now() - since); }
    }
    return NULL;
}
void aoc_scene_start(void) {
    const char *build = dm_main_build();
    for (unsigned k = 0; k < sizeof k_builds / sizeof *k_builds; k++) if (!strcmp(build, k_builds[k].build))
        for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) {
            g_ui_slot = g_mod[i].base + k_builds[k].ui;
            pthread_t th; if (!pthread_create(&th, NULL, watch, NULL)) pthread_detach(th);
            return;
        }
}
