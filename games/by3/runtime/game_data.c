/* The game data folder (game_data.h): small "KEY VALUE" files the build writes from the player's game files. Each file is
   read once, on first use, and kept: the values are pointers into it. */
#include "game_data.h"
#include "runtime.h"
#include "game_version.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct Config { struct Config *next; char *file; const char **keys, **values; size_t count; } Config;
static Config *configs;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static void need(int c, const char *m) { if (!c) runtime_fail(m); }
void runtime_game_data_path(const char *name, char *out, size_t cap) {
    const char *dir = getenv("TOTK_GAME_DATA");
    int n = dir && *dir ? snprintf(out, cap, "%s/%s", dir, name) : snprintf(out, cap, "build/game-data/" TOTK_GAME_DIR "/%s", name);
    need(n > 0 && (size_t)n < cap, "game data path too long");
}
static Config *config_load(const char *file) {
    char path[4096]; runtime_game_data_path(file, path, sizeof path);
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "[totk] %s: %s\n", path, strerror(errno)); runtime_fail("game data missing (build_app.py writes it into the app; update_runtime.py --work adds it to an older app)"); }
    char *text = malloc((1u << 20) + 1); need(text != NULL, "game data memory");
    size_t n = fread(text, 1, 1u << 20, f); need(!ferror(f) && fgetc(f) == EOF, "game data file too large"); fclose(f); text[n] = 0;
    Config *c = calloc(1, sizeof *c); need(c && (c->file = strdup(file)), "game data memory");
    size_t lines = 1; for (size_t i = 0; i < n; i++) lines += text[i] == '\n';
    c->keys = calloc(lines, sizeof *c->keys); c->values = calloc(lines, sizeof *c->values); need(c->keys && c->values, "game data memory");
    for (char *line = text, *next; *line; line = next) {
        char *end = strchr(line, '\n'); next = end ? end + 1 : line + strlen(line); if (end) *end = 0;
        if (!*line || *line == '#') continue;
        char *space = strchr(line, ' '); need(space && space > line && space[1], "game data line is not KEY VALUE");
        *space = 0; c->keys[c->count] = line; c->values[c->count++] = space + 1;
    }
    return c;
}
const char *runtime_game_text(const char *file, const char *key) {
    pthread_mutex_lock(&lock);
    Config *c = configs; while (c && strcmp(c->file, file)) c = c->next;
    if (!c) { c = config_load(file); c->next = configs; configs = c; }
    pthread_mutex_unlock(&lock);
    for (size_t i = 0; i < c->count; i++) if (!strcmp(c->keys[i], key)) return c->values[i];
    fprintf(stderr, "[totk] game data %s has no %s\n", file, key); runtime_fail("game data value missing");
}
uint64_t runtime_game_number(const char *file, const char *key) {
    const char *text = runtime_game_text(file, key); char *end = NULL; errno = 0;
    unsigned long long v = strtoull(text, &end, 0); need(!errno && end != text && !*end && *text != '-', "game data number");
    return v;
}
const char *runtime_game_sha256(const char *file, const char *key) {
    const char *text = runtime_game_text(file, key);
    need(strlen(text) == 64 && strspn(text, "0123456789abcdef") == 64, "game data SHA-256");
    return text;
}
