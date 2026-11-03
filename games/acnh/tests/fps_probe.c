/* Synthetic module only: exercises the production FPS state and checked writes. */
#include "runtime.h"
#include "game_version.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned *observed;
_Noreturn void runtime_fail(const char *message) {
    fprintf(stderr, "%s\n", message);
    /* A late original-word mismatch must not leave an earlier site modified. */
    if (observed && observed[0] != 1) _Exit(78);
    _Exit(77);
}
static void setting_path(char *path, size_t size) {
    snprintf(path, size, "%s/frame-rate.txt", getenv("TEST_SETTINGS"));
}
int runtime_setting_word(const char *name, char *word, size_t size) {
    if (strcmp(name, "frame-rate.txt")) abort();
    char path[1024]; setting_path(path, sizeof path);
    FILE *f = fopen(path, "r"); if (!f) return 0;
    size_t n = fread(word, 1, size - 1, f); word[n] = 0; fclose(f); return 1;
}
void runtime_setting_save(const char *name, const char *word, const char *what) {
    if (strcmp(name, "frame-rate.txt") || strcmp(what, "frame rate")) abort();
    char path[1024]; setting_path(path, sizeof path);
    FILE *f = fopen(path, "w"); if (!f) abort(); fprintf(f, "%s\n", word); fclose(f);
}
unsigned runtime_present_interval(unsigned interval);
int main(int argc, char **argv) {
    if (argc != 3) return 1;
    int want = atoi(argv[1]);
    unsigned image[4] = {1, 2, 3, 4};
    if (!strcmp(argv[2], "mismatch")) { image[2] = 9; observed = image; }
    Module m = {.name="main", .file_size=sizeof image, .key_off=sizeof image, .base=(void *)image};
    if (runtime_frame_rate_active() != want || runtime_frame_rate_choice() != want) return 2;
    if (runtime_present_interval(2) != (want == 60 ? 1u : 2u)) return 3;
    /* Only a build offering the selector locks 30 FPS; others keep the game's interval. */
    unsigned one = want == 60 || !runtime_frame_rate_available() ? 1u : 2u;
    if (!strcmp(argv[2], "unlocked") && one != 1u) return 10;
    if (runtime_present_interval(1) != one || runtime_present_interval(3) != (want == 60 ? 1u : 3u)) return 9;
    runtime_frame_rate_install(&m);
    if (image[0] != (want == 60 ? 10u : 1u) || image[2] != (want == 60 ? 30u : 3u)) return 4;
    if (!strcmp(argv[2], "choose")) {
        if (!runtime_frame_rate_available() || runtime_frame_rate_choose(120)) return 5;
        if (!runtime_frame_rate_choose(want == 60 ? 30 : 60)) return 6;
        if (runtime_frame_rate_active() != want || runtime_frame_rate_choice() == want) return 7;
        if (runtime_present_interval(2) != (want == 60 ? 1u : 2u)) return 8;
    }
    return 0;
}
