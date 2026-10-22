/* Video preferences: the frame-rate ceiling (key 6: 30 -> 60 -> 120 -> unlimited -> 30) and
   vertical sync (key 7), kept in <profile>/video.txt ("fps=30|60|120|0", "vsync=0|1"; a missing
   file caps at 60 with vertical sync off). The cap is a floor on the present period enforced in
   hle_nvn.c (present_period), so every choice applies immediately: the game's own frame
   controller still steps down in heavy scenes, only the ceiling is ours. 60 is the default
   because the game's audio driver has only been reliable at or below 60 fps: with the 60 FPS
   patch (interval 1) the uncapped clock let the loop run faster on a ProMotion display and the
   game stopped feeding audio within seconds of boot (2026-10-23). Vertical sync off paces the
   game by its own clock and shows the newest finished frame at every display refresh (the
   app's previous behaviour); on it holds the game to the display, Ryujinx's "Switch" mode.
   The file is read once (loader.c calls video_prefs_boot before anything else); keys 6 and 7
   rewrite it, update the cached values and invalidate the pacing configuration. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int pref_fps = 60, pref_vsync = 0, loaded;

static void load(void) {
    if (loaded) return;
    loaded = 1;
    const char *dir = getenv("TOTK_PROFILE_DIR");
    char path[4096];
    if (!dir || snprintf(path, sizeof path, "%s/video.txt", dir) >= (int)sizeof path) return;
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[64];
    while (fgets(line, sizeof line, f)) {
        int v;
        if (sscanf(line, " fps=%d", &v) == 1 && (v == 0 || v == 30 || v == 60 || v == 120)) pref_fps = v;
        else if (sscanf(line, " vsync=%d", &v) == 1 && (v == 0 || v == 1)) pref_vsync = v;
    }
    fclose(f);
}

int video_pref_fps(void) { load(); return pref_fps; }
int video_pref_vsync(void) { load(); return pref_vsync; }

/* The shortest present period the cap allows, in nanoseconds; 0 = unlimited. */
long video_pref_period_floor_ns(void) {
    load();
    return pref_fps == 30 ? 1000000000l / 30 : pref_fps == 60 ? 1000000000l / 60 :
           pref_fps == 120 ? 1000000000l / 120 : 0;
}

static void save(void) {
    const char *dir = getenv("TOTK_PROFILE_DIR");
    char path[4096];
    if (!dir || snprintf(path, sizeof path, "%s/video.txt", dir) >= (int)sizeof path) return;
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "[totk] video preferences: cannot write %s\n", path); return; }
    fprintf(f, "fps=%d\nvsync=%d\n", pref_fps, pref_vsync);
    fclose(f);
}

/* Vertical sync on holds the game to the display (TOTK_UNCAPPED=0); off keeps the own clock.
   force: a key press overrides variables the start left alone. */
static void apply_pacing_env(int force) {
    if (force || !getenv("TOTK_UNCAPPED")) {
        if (pref_vsync) setenv("TOTK_UNCAPPED", "0", 1);
        else unsetenv("TOTK_UNCAPPED");
    }
}

static void pacing_changed(const char *what) {
    extern void dm_nvn_present_invalidate(void);
    dm_nvn_present_invalidate();
    fprintf(stderr, "[totk] %s: %d fps ceiling, vertical sync %s\n", what, pref_fps,
            pref_vsync ? "on" : "off");
}

void video_prefs_boot(void) {
    load();
    apply_pacing_env(0);
    if (pref_fps) fprintf(stderr, "[totk] video preferences: %d fps ceiling, vertical sync %s (key 6 / key 7)\n",
                          pref_fps, pref_vsync ? "on" : "off");
    else fprintf(stderr, "[totk] video preferences: unlimited, vertical sync %s (key 6 / key 7)\n",
                 pref_vsync ? "on" : "off");
}

/* Key 6: 30 -> 60 -> 120 -> unlimited -> 30. The cap is a floor on the present period, so the
   change applies from the next frame on. Frame interpolation (key 2) and a ceiling are two
   pacing controls that must not both steer the game: choosing a ceiling turns interpolation
   off, and starting interpolation clears the ceiling (video_pref_cap_clear). */
void video_pref_fps_cycle(void) {
    load();
    pref_fps = pref_fps == 30 ? 60 : pref_fps == 60 ? 120 : pref_fps == 120 ? 0 : 30;
    save();
    if (pref_fps) { extern void dm_gfx_set_flag(const char *name, int v); dm_gfx_set_flag("mfx_interp", 0); }
    pacing_changed("frame-rate preference changed");
}

/* Key 2 leaving "off": the ceiling gives way to the interpolation's own pacing. */
void video_pref_cap_clear(void) {
    load();
    if (!pref_fps) return;
    pref_fps = 0;
    save();
    pacing_changed("frame-rate preference cleared for interpolation");
}

/* Key 7: vertical sync on/off. Applied at the next start, not live: switching the acquire path
   to the display link during the boot and title window stopped the game's audio at once
   (2026-10-23: rms fell to zero within seconds of the toggle, only when it happened early;
   the same switch deep in play was harmless), while a start that already has it has not been
   seen to fail. The frame-rate cap keeps switching live. */
void video_pref_vsync_toggle(void) {
    load();
    pref_vsync = !pref_vsync;
    save();
    fprintf(stderr, "[totk] vertical-sync preference changed: %s at the next start\n", pref_vsync ? "on" : "off");
}
