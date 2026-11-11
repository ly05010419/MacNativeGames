// Settings an environment variable can decide for one run (front_mac.m), kept pure so the settings UI test drives them:
//   stats pane   DM_AOC_STATS        0 Off · 1 FPS only · 2 Full     saved as stats=       default 0
//   frame graph  DM_AOC_FRAME_GRAPH  0 Off · 1 On                   saved as frame_graph= default 0
// A valid value decides the setting for the run and locks its row in the panel (greyed, naming the variable); the preference in
// controller-layout.txt stays as it was, is the one written back, and applies again on a run without the variable. Any other
// value is ignored, with a log line, and the saved preference applies. A key missing from the file, or an out-of-range saved
// value, is the default.
#pragma once
#include <string.h>
// The aspect ratio row needs the camera stubs patches.py writes only into 1.4.1's main (build e5f275b5...): on another build the
// panel leaves the row out and the run keeps the game's 16:9, while a saved 16:10 stays in the file for a 1.4.1 app.
static inline int dm_aspect_supported(const char *main_build) {
    return main_build && !strcmp(main_build, "e5f275b5d0889899e55de1a164eef060");
}
typedef struct { int saved, value, locked; } DmPref;
// One decimal digit from 0 to max, nothing else.
static inline int dm_pref_parse(const char *text, int max, int *out) {
    if (!text || text[0] < '0' || text[0] > '0' + max || text[1]) return 0;
    *out = text[0] - '0'; return 1;
}
// saved: the file's value, or -1 when the key is missing. *rejected: the variable was set but not valid.
static inline DmPref dm_pref_resolve(int saved, const char *env, int max, int fallback, int *rejected) {
    DmPref p = { saved >= 0 && saved <= max ? saved : fallback, 0, 0 };
    int v; *rejected = 0;
    if (env && dm_pref_parse(env, max, &v)) { p.value = v; p.locked = 1; }
    else { p.value = p.saved; *rejected = env != 0; }
    return p;
}
// A change from the panel: refused while the variable decides the setting, so the saved preference is never overwritten by it.
static inline int dm_pref_set(DmPref *p, int value) {
    if (p->locked) return 0;
    p->saved = p->value = value; return 1;
}
