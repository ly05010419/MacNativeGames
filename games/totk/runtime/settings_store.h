/* The settings panel's picks, kept between runs (Mac): <data>/settings.txt, one key=value per line.
   The panel (settings_panel.h) saves every pick; the next start replays them. Before this file a 1.2.1 app put every
   pick back to its default at each start (1.4.3 kept only its resolution, in picture-mode.txt), so a player who wanted
   2K, 21:9 or a frame lock had to set it again after every launch. One file holds them all:

     resolution  off | metalfx | native | native-metalfx | 720p   the picture (game_patches.c restores it at start)
     aspect      16:9 | 16:10 | 21:9 | 32:9 | 2.39                the ratio (becomes TOTK_ASPECT at start)
     taa         0 | 1                                            edge smoothing (becomes TOTK_TAA at start)
     fpslock     0 | 30 | 40 | 60                                 the frame lock (applied once the window is up)
     mfx         off | 30 | 40 | 50 | max                         frame interpolation (applied with the window)
     hud         all | fps | off                                  the performance overlay (applied with the window)

   The file sits next to controls.txt (the launcher's TOTK_CONTROLS, which a start script can point into the player's
   folder), as rumble.txt and mouse.txt do; the launcher's data folder otherwise. An environment variable that is already
   set wins over the file, so a start script or a test can override it. A file that cannot be read or written never
   stops the game: one log line at most. */
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline const char *settings_file(void) {
    static char path[4096];
    if (path[0]) return path;
    /* Next to controls.txt (the launcher's TOTK_CONTROLS, which a start script can put in the player's folder), like
       rumble.txt and mouse.txt; the launcher's data folder otherwise. TOTK_ROMFS_PROOF_DIR is fixed to the system data
       folder by the launcher, so it is only the fallback. */
    char dir[4096] = "";
    const char *controls = getenv("TOTK_CONTROLS");
    if (controls && *controls) {
        snprintf(dir, sizeof dir, "%s", controls);
        char *slash = strrchr(dir, '/');
        if (slash) *slash = 0;
    }
    if (!dir[0]) {
        const char *proof = getenv("TOTK_ROMFS_PROOF_DIR");
        if (proof && *proof) snprintf(dir, sizeof dir, "%s", proof);
    }
    if (!dir[0]) return NULL;
    if (strlen(dir) + 14 >= sizeof path) return NULL;
    snprintf(path, sizeof path, "%s/settings.txt", dir);
    return path;
}

/* The value of key, 0 when the file or the key is not there. */
static inline int settings_read(const char *key, char *out, size_t cap) {
    const char *p = settings_file();
    if (!p || !cap) return 0;
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    char line[512];
    size_t k = strlen(key);
    int found = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, key, k) && line[k] == '=') {
            snprintf(out, cap, "%s", line + k + 1);
            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}

/* Replaces the key's line (or appends it) and writes the whole small file back through a temporary. */
static inline void settings_write(const char *key, const char *value) {
    const char *p = settings_file();
    if (!p) return;
    char lines[64][512];
    int n = 0;
    size_t k = strlen(key);
    FILE *f = fopen(p, "r");
    if (f) {
        while (n < 63 && fgets(lines[n], sizeof lines[n], f)) {
            lines[n][strcspn(lines[n], "\r\n")] = 0;
            if (!strncmp(lines[n], key, k) && lines[n][k] == '=') continue;
            if (lines[n][0]) n++;
        }
        fclose(f);
    }
    snprintf(lines[n], sizeof lines[n], "%s=%s", key, value);
    n++;
    char tmp[4096];
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    FILE *o = fopen(tmp, "w");
    if (!o) { fprintf(stderr, "[totk] settings: cannot write %s\n", p); return; }
    for (int i = 0; i < n; i++) fprintf(o, "%s\n", lines[i]);
    if (fclose(o) || rename(tmp, p)) { fprintf(stderr, "[totk] settings: cannot write %s\n", p); remove(tmp); return; }
    fprintf(stderr, "[totk] settings: %s=%s kept for the next start\n", key, value);
}

static inline void settings_write_int(const char *key, int v) {
    char b[16];
    snprintf(b, sizeof b, "%d", v);
    settings_write(key, b);
}

/* At the very start (loader.c main, before the modules are in): the aspect ratio and edge smoothing are read from the
   environment when the game is initialised, so the file's pick becomes that variable first. A variable already there is
   left alone. */
static inline void settings_apply_env(void) {
    char v[64];
    if (!getenv("TOTK_ASPECT") && settings_read("aspect", v, sizeof v)) setenv("TOTK_ASPECT", v, 1);
    if (!getenv("TOTK_TAA") && settings_read("taa", v, sizeof v)) setenv("TOTK_TAA", v, 1);
}
