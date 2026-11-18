/* The one-word settings files of the controller code and the rest of the runtime (shared/README.md): each next to
   controls.txt (TOTK_CONTROLS; a run without it, such as a test, neither reads nor writes them), so they hold for the next
   launch, and the few both Zelda games share in the folder above. Included by runtime/gamepad_core.h. */
#ifndef GAMEPAD_SETTINGS_H
#define GAMEPAD_SETTINGS_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int setting_path(const char *name, char *out, size_t size) {
    const char *controls = getenv("TOTK_CONTROLS");
    if (!controls || !*controls) return 0;
    const char *slash = strrchr(controls, '/');
    int dir = slash ? (int)(slash - controls) : 0;
    int n = slash ? snprintf(out, size, "%.*s/%s", dir, controls, name) : snprintf(out, size, "%s", name);
    return n > 0 && (size_t)n < size;
}
/* A setting both games share (user 2026-10-29: the FPS graph, "改成两个游戏共用"): the folder above the game's own, on the
   Mac Application Support/MacNativeGames next to TOTK/ and BotW/. Without such a folder (controls.txt has no parent
   folder) there is none and the default holds. */
static int shared_setting_path(const char *name, char *out, size_t size) {
    const char *controls = getenv("TOTK_CONTROLS");
    if (!controls || !*controls) return 0;
    const char *slash = strrchr(controls, '/');
    if (!slash) return 0;
    const char *up = slash;
    while (up > controls && up[-1] != '/') up--;
    if (up <= controls) return 0;
    int n = snprintf(out, size, "%.*s%s", (int)(up - controls), controls, name);
    return n > 0 && (size_t)n < size;
}

static int setting_read(const char *name, char *word, size_t size, char *path, size_t path_size) {   /* 1: read */
    if (!setting_path(name, path, path_size)) return 0;
    FILE *f = fopen(path, "r");
    if (!f) return 0;   /* never changed: the default */
    size_t n = fread(word, 1, size - 1, f);
    fclose(f);
    word[n] = 0;
    return 1;
}

static void file_write(const char *path, const char *text, const char *what) {   /* whole or not at all */
    char temp[4200];
    snprintf(temp, sizeof temp, "%s.tmp", path);
    FILE *f = fopen(temp, "w");
    int ok = f && fprintf(f, "%s", text) >= 0;
    if (f && fclose(f)) ok = 0;
    if (ok && rename(temp, path)) ok = 0;
    if (!ok) { fprintf(stderr, "[totk-input] could not save the %s to %s; it holds until the game closes\n", what, path); unlink(temp); }
}

static void setting_write(const char *name, const char *word, const char *what) {
    char path[4096], text[16384];
    if (!setting_path(name, path, sizeof path)) return;
    size_t n = strlen(word);
    snprintf(text, sizeof text, "%s%s", word, n && word[n - 1] == '\n' ? "" : "\n");
    file_write(path, text, what);
}

/* The same one-word files for the rest of the runtime: 1 with the file's text in word as written (the caller drops the
   blanks around the word), 0 when there is no such file or no settings folder (a run without TOTK_CONTROLS). */
int runtime_setting_word(const char *name, char *word, size_t size) {
    char path[4096];
    return setting_read(name, word, size, path, sizeof path);
}
/* ... and saves one, as controller-type.txt is saved (the settings panel's controller rows). */
void runtime_setting_save(const char *name, const char *word, const char *what) { setting_write(name, word, what); }
/* The same for a setting both games share (shared_setting_path). */
int runtime_shared_setting_word(const char *name, char *word, size_t size) {
    char path[4096];
    if (!shared_setting_path(name, path, sizeof path)) return 0;
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    size_t n = fread(word, 1, size - 1, f);
    fclose(f);
    word[n] = 0;
    return 1;
}
void runtime_shared_setting_save(const char *name, const char *word, const char *what) {
    char path[4096], text[64];
    if (!shared_setting_path(name, path, sizeof path)) return;
    snprintf(text, sizeof text, "%s\n", word);
    file_write(path, text, what);
}
/* ... or removes it: the default again (the settings panel's resets). */
void runtime_setting_remove(const char *name) {
    char path[4096];
    if (setting_path(name, path, sizeof path)) unlink(path);
}
#endif
