// Age of Calamity's cutscenes advance a fixed step per frame, so above 30 fps they run fast (FPSLocker's notes for this game say the same, and it has no fix). While one plays the
// frame step goes back to the game's own 30 fps; the chosen target comes back when it ends. The state read is a count the game keeps itself, at main+0x2783ad0 in 1.4.1: the
// holds on a shared resource, taken through main+0x8db820 (called from main+0x8db4b4 at start-up and from main+0xb10314) and given back at main+0x8ae58c and main+0xb09b80,
// which clear the held pointer at main+0x2783ac8 when the count reaches 0. It is 1 from start-up and 2 while an in-engine cutscene plays: in 1.4.1 memory captures, 55 taken every
// 5 s along one scripted route (title, map, mission menus, a loading screen, a narration screen, the mission's opening cutscene, battle) and 12 from a mission played by hand,
// every cutscene capture read 2 and every other one 1, and it fell back to 1 on the black frame that ends the cutscene. Title, menus, loading and battle are never held.
// Pre-rendered movies keep their own clock. A build not listed keeps its target through cutscenes.
#include "dm.h"
#include "../gfx/nvn/aoc_timing.h"
#include <pthread.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
enum { POLL_US = 10000, RELEASE_POLLS = 10 };   // released 100 ms after the count drops, so a flicker between two cutscenes does not switch twice
static const struct { const char *build; uint32_t holds; } k_builds[] = {
    { "e5f275b5d0889899e55de1a164eef060", 0x2783ad0 },   // 1.4.1
};
static const uint8_t *g_holds;
static int cutscene(void) { uint32_t n; memcpy(&n, g_holds, 4); return n >= 2; }
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
            g_holds = g_mod[i].base + k_builds[k].holds;
            pthread_t th; if (!pthread_create(&th, NULL, watch, NULL)) pthread_detach(th);
            return;
        }
}
