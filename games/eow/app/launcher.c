/* The Legend of Zelda: Echoes of Wisdom.app launcher: points the runtime at the app's game files and the player's data
   folder, then runs it. Adapted from games/acnh/scripts/acnh_app_launcher.c (third_party/COPIES.md).

   App:    Contents/MacOS/Echoes of Wisdom (this); Contents/Resources/game/{build/, rewritten/v110/, romfs/, fonts/}.
           build/ holds the runtime, and next to it the translator, the shader seed and the compiled shader libraries. The
           runtime is started by its real path: the graphics layer finds shader-libs/ beside the executable path without
           resolving links (nvn_mtl.m exe_rel), so started through a link in MacOS/ it found none of the 9,233 libraries,
           compiled every shader while the intro movie waited, and the window stayed black (2026-11-17).
   Data:   ~/Library/Application Support/MacNativeGames/EoW: profile/ (saves), shader-cache/, game-slot/, controls.txt,
           last-run.log; the working directory of the game. EOW_DATA_DIR=<absolute dir> uses another folder. */
#include <CoreFoundation/CoreFoundation.h>
#include <dirent.h>
#include <libgen.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void join(char *out, const char *dir, const char *name) {
    if (snprintf(out, PATH_MAX, "%s/%s", dir, name) >= PATH_MAX) { fputs("path too long\n", stderr); exit(1); }
}
static void parent_of(char *out, const char *path) {
    char copy[PATH_MAX]; snprintf(copy, sizeof copy, "%s", path); snprintf(out, PATH_MAX, "%s", dirname(copy));
}
static int run(const char *const argv[]) {
    pid_t pid = fork();
    if (pid == 0) { execv(argv[0], (char *const *)argv); _exit(127); }
    int status = 0; if (pid < 0 || waitpid(pid, &status, 0) < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
static int newest_first(const void *a, const void *b) { return strcmp(*(char *const *)b, *(char *const *)a); }

/* A copy of the saves before each start, the newest five kept. */
static void back_up_saves(const char *profile, const char *data, const char *stamp) {
    char saves[PATH_MAX], backups[PATH_MAX], target[PATH_MAX]; struct stat st;
    join(saves, profile, "save"); join(backups, data, "backups");
    if (stat(saves, &st) || !S_ISDIR(st.st_mode)) return;
    mkdir(backups, 0700); join(target, backups, stamp);
    if (run((const char *const[]){"/bin/cp", "-Rp", saves, target, NULL})) return;
    DIR *d = opendir(backups); if (!d) return;
    char *names[256]; int n = 0; struct dirent *e;
    while ((e = readdir(d)) && n < 256) if (e->d_name[0] != '.') names[n++] = strdup(e->d_name);
    closedir(d);
    qsort(names, (size_t)n, sizeof names[0], newest_first);
    for (int i = 0; i < n; i++) {
        if (i >= 5) { char old[PATH_MAX]; join(old, backups, names[i]); run((const char *const[]){"/bin/rm", "-rf", old, NULL}); }
        free(names[i]);
    }
}

/* The game's language follows the Mac's first preferred language (runtime/services.c reads TOTK_LOCALE). The runtime's
   own fallback, the current locale, is the app bundle's localization inside an app, so a Mac set to Chinese would get
   English (seen with Animal Crossing, 2026-11-17). */
static void set_locale(void) {
    if (getenv("TOTK_LOCALE") || getenv("TOTK_LANG")) return;
    CFArrayRef list = CFLocaleCopyPreferredLanguages(); char tag[64] = {0};
    if (list) {
        if (CFArrayGetCount(list) > 0)
            CFStringGetCString(CFArrayGetValueAtIndex(list, 0), tag, sizeof tag, kCFStringEncodingUTF8);
        CFRelease(list);
    }
    if (tag[0]) setenv("TOTK_LOCALE", tag, 1);
}

int main(int argc, char **argv) {
    int check = argc == 2 && !strcmp(argv[1], "--check");
    if (argc != 1 && !check) { fputs("usage: Echoes of Wisdom [--check]\n", stderr); return 2; }
    char exe[PATH_MAX], real[PATH_MAX]; uint32_t size = sizeof exe;
    if (_NSGetExecutablePath(exe, &size) || !realpath(exe, real)) return 1;
    char macos[PATH_MAX], contents[PATH_MAX], resources[PATH_MAX], game[PATH_MAX];
    parent_of(macos, real); parent_of(contents, macos);
    join(resources, contents, "Resources"); join(game, resources, "game");

    char runtime[PATH_MAX], modules[PATH_MAX], romfs[PATH_MAX], fonts[PATH_MAX], index[PATH_MAX];
    join(runtime, game, "build/eow-native"); join(modules, game, "rewritten/v110");
    join(romfs, game, "romfs"); join(fonts, game, "fonts"); join(index, romfs, "runtime-index.bin");
    if (access(runtime, X_OK)) { perror("Echoes of Wisdom: missing runtime"); return 1; }
    if (access(index, R_OK)) { perror("Echoes of Wisdom: missing game data"); return 1; }
    if (check) { puts("Echoes of Wisdom launcher: runtime and game data present; game was not launched"); return 0; }

    char data[PATH_MAX];
    const char *override = getenv("EOW_DATA_DIR");
    if (override && *override) {
        if (override[0] != '/' || snprintf(data, sizeof data, "%s", override) >= (int)sizeof data) {
            fputs("EOW_DATA_DIR must be an absolute directory path\n", stderr); return 1;
        }
    } else {
        const char *home = getenv("HOME");
        if (!home || !*home) { fputs("Echoes of Wisdom: HOME is not set\n", stderr); return 1; }
        join(data, home, "Library/Application Support/MacNativeGames/EoW");
    }
    char profile[PATH_MAX], cache[PATH_MAX], slot[PATH_MAX], commands[PATH_MAX], controls[PATH_MAX], log[PATH_MAX];
    join(profile, data, "profile"); join(cache, data, "shader-cache"); join(slot, data, "game-slot");
    join(commands, data, "commands.txt"); join(controls, data, "controls.txt"); join(log, data, "last-run.log");
    run((const char *const[]){"/bin/mkdir", "-p", data, NULL});
    mkdir(profile, 0700); mkdir(cache, 0755); mkdir(slot, 0755);
    FILE *cf = fopen(commands, "w"); if (cf) fclose(cf);

    /* The README's play settings (games/eow/README.md, Play). */
    setenv("TOTK_GPU_ALLOW_SUBMIT", "1", 1);
    setenv("TOTK_GPU_BLOCKRT", "1", 1);
    setenv("TOTK_GPU_MIDCLEAR", "1", 1);
    setenv("TOTK_GPU_WIDEN", "1", 1);
    setenv("TOTK_GPU_BUDGET_MB", "5000", 0);
    setenv("DM_PRESENT_LINK_FALLBACK", "1", 1);
    setenv("TOTK_PROFILE_DIR", profile, 1);
    setenv("TOTK_SHADER_CACHE", cache, 1);
    setenv("TOTK_ROMFS", romfs, 1);
    setenv("TOTK_ROMFS_PROOF_DIR", data, 1);
    setenv("TOTK_SHARED_FONTS", fonts, 1);
    setenv("TOTK_GAME_SLOT_DIR", slot, 1);
    setenv("TOTK_INPUT_FILE", commands, 1);
    setenv("TOTK_CONTROLS", controls, 1);
    setenv("TOTK_SLOT_OWNER", "user", 1);
    setenv("TOTK_SLOT_PURPOSE", "Echoes of Wisdom.app", 1);
    set_locale();
    for (const char *const *v = (const char *const[]){"TOTK_ASSUME_FOCUS", "TOTK_VERIFY_STARTUP", "TOTK_HEADLESS", NULL}; *v; v++)
        unsetenv(*v);
    if (chdir(data)) return 1;

    char stamp[64], date[32]; time_t launched = time(NULL);
    strftime(date, sizeof date, "%Y%m%d-%H%M%S", localtime(&launched));
    snprintf(stamp, sizeof stamp, "%s-%ld", date, (long)getpid());
    back_up_saves(profile, data, stamp);

    pid_t child = fork();
    if (child < 0) return 1;
    if (child == 0) {
        char previous[PATH_MAX]; join(previous, data, "previous-run.log");
        rename(log, previous);
        FILE *out = fopen(log, "w");
        if (out) { dup2(fileno(out), 1); dup2(fileno(out), 2); fclose(out); }
        execl(runtime, "eow-native", modules, "boot", (char *)NULL);
        perror("Echoes of Wisdom: could not start the game"); _exit(127);
    }
    int status;
    waitpid(child, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
