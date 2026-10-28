/* Mario Kart 8 Deluxe.app launcher (scripts/package_mk8d_app.py builds the bundle around it), adapted from Odyssey's.
   Everything the game reads is inside the bundle (Contents/Resources/game: runtime, shader tools, rewritten modules,
   RomFS, shared fonts); everything it writes is in the data folder (saves, shader cache, logs), so the app keeps working
   when the checkout is not there. The data folder is the one named in Contents/Resources/data-dir.txt (the work folder's
   "MK8D Data", written when the app is packaged), or "MK8D Data" next to the app. Contents/MacOS/mk8d-native is a
   symlink to the bundled runtime: started through it, the window belongs to this app (name and icon), while the runtime
   still finds its shader tools next to the real file. The environment is `make run`'s, plus full screen and logs.
   Odyssey's settings window, render-size tables and session limit are not carried over: none applies to this game yet. */
#include <CoreFoundation/CoreFoundation.h>
#include <errno.h>
#include <fcntl.h>
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
static int run(const char *const argv[]) {   /* a system tool, waited for */
    pid_t pid = fork();
    if (pid == 0) { execv(argv[0], (char *const *)argv); _exit(127); }
    int status = 0; if (pid < 0 || waitpid(pid, &status, 0) < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
static int newest_first(const void *a, const void *b) { return strcmp(*(char *const *)b, *(char *const *)a); }
/* Saves are rewritten in place (runtime/fs_host.c), so a crash during a write could leave the only copy damaged: keep the
   saves as they were before each of the last five sessions in "<data>/backups/<date-time>". */
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
    qsort(names, (size_t)n, sizeof names[0], newest_first);   /* stamps sort by time */
    for (int i = 0; i < n; i++) {
        if (i >= 5) { char old[PATH_MAX]; join(old, backups, names[i]); run((const char *const[]){"/bin/rm", "-rf", old, NULL}); }
        free(names[i]);
    }
}
/* The first line of a small text file in the bundle, or "" */
static void read_line(const char *path, char *out, size_t size) {
    *out = 0; FILE *f = fopen(path, "r");
    if (f) { if (fgets(out, (int)size, f)) out[strcspn(out, "\r\n")] = 0; fclose(f); }
}

int main(void) {
    char exe[PATH_MAX], real[PATH_MAX]; uint32_t size = sizeof exe;
    if (_NSGetExecutablePath(exe, &size) || !realpath(exe, real)) return 1;
    char macos[PATH_MAX], contents[PATH_MAX], app[PATH_MAX], home[PATH_MAX];
    parent_of(macos, real); parent_of(contents, macos); parent_of(app, contents); parent_of(home, app);

    char game[PATH_MAX], runtime[PATH_MAX], modules[PATH_MAX], romfs[PATH_MAX], fonts[PATH_MAX], resources[PATH_MAX];
    join(resources, contents, "Resources"); join(game, resources, "game");
    join(runtime, macos, "mk8d-native"); join(modules, game, "rewritten/local");
    join(romfs, game, "romfs"); join(fonts, game, "fonts");

    char data[PATH_MAX], path[PATH_MAX];
    join(path, resources, "data-dir.txt"); read_line(path, data, sizeof data);
    if (!*data) join(data, home, "MK8D Data");
    char profile[PATH_MAX], cache[PATH_MAX], commands[PATH_MAX], log[PATH_MAX], slot[PATH_MAX];
    mkdir(data, 0755); join(profile, data, "profile"); mkdir(profile, 0700);
    join(cache, data, "shader-cache/rt"); join(commands, data, "commands.txt"); join(log, data, "mk8d.log");
    join(slot, data, "game-slot"); mkdir(slot, 0755);
    struct stat st;

    FILE *cf = fopen(commands, "w"); if (cf) fclose(cf);
    /* As `make run` (../Makefile): the graphics switches, presents without the display link with the fallback, and the
       diagnostic command file (runtime/input.c) */
    setenv("TOTK_GPU_ALLOW_SUBMIT", "1", 1); setenv("TOTK_GPU_WIDEN", "1", 1); setenv("TOTK_GPU_BLOCKRT", "1", 1);
    setenv("TOTK_GPU_MIDCLEAR", "1", 1); setenv("TOTK_SHADER_ASYNC", "1", 0);
    setenv("DM_PRESENT_LINK", "0", 0); setenv("DM_PRESENT_LINK_FALLBACK", "1", 0);
    /* Warm up only the shaders this Mac has used (pso_user.tsv, pso_collected.tsv). With the whole-game seed in the cache,
       warming all of it at every start would load about 30,000 libraries (6,691 took 4.6 s and 460 MB on an M5); the
       rest load at first use, from the precompiled libraries. TOTK_WARM_SCOPE=all warms everything as before. */
    setenv("TOTK_WARM_SCOPE", "observed", 0);
    setenv("TOTK_PROFILE_DIR", profile, 1); setenv("TOTK_SHADER_CACHE", cache, 1);
    setenv("TOTK_ROMFS", romfs, 1); setenv("TOTK_SHARED_FONTS", fonts, 1); setenv("TOTK_INPUT_FILE", commands, 1);
    setenv("TOTK_GAME_SLOT_DIR", slot, 1); setenv("TOTK_SLOT_OWNER", "user", 1); setenv("TOTK_SLOT_PURPOSE", "mk8d-app", 1);
    setenv("TOTK_ROMFS_PATCHES", "0", 1);
    /* The update's version the app was built from (package_mk8d_app.py), for the window title */
    { char version[32]; join(path, resources, "game-version.txt"); read_line(path, version, sizeof version);
      if (*version) setenv("MK8D_GAME_VERSION", version, 1); }
    /* The game's language (runtime/services.c): the first preferred language, which follows a per-app language too. Inside
       an app bundle without localizations, the current locale the runtime reads otherwise is the bundle's (English). */
    { CFArrayRef langs = CFLocaleCopyPreferredLanguages(); char tag[32] = "";
      if (langs && CFArrayGetCount(langs) > 0) CFStringGetCString(CFArrayGetValueAtIndex(langs, 0), tag, sizeof tag, kCFStringEncodingUTF8);
      if (langs) CFRelease(langs);
      if (*tag) setenv("TOTK_LOCALE", tag, 0); }
    /* The RomFS check's proof file (runtime/romfs.c) goes next to the data, not into the bundle: written beside the
       bundled RomFS it broke Odyssey.app's code signature after the first launch. */
    setenv("TOTK_ROMFS_PROOF_DIR", data, 1);
    /* The player's button layout (runtime/controller_map.h), when "<data>/controls.txt" exists. */
    char controls[PATH_MAX]; join(controls, data, "controls.txt");
    if (!stat(controls, &st)) setenv("TOTK_CONTROLS", controls, 0);
    /* How GameController's A/B/X/Y map to positions (runtime/gamepad_layout.h): "position" or "label" in
       "<data>/face-buttons.txt". */
    char face_path[PATH_MAX]; join(face_path, data, "face-buttons.txt");
    FILE *ff = fopen(face_path, "r");
    if (ff) { char mode[16] = ""; if (fscanf(ff, "%15s", mode) == 1 && (!strcmp(mode, "position") || !strcmp(mode, "label"))) setenv("TOTK_FACE_BUTTONS", mode, 0); fclose(ff); }
    /* A freeze (no frame for 15 s) logs which threads hold and wait for the virtual cores (runtime/freeze_watchdog.c), so a
       hang in play can be diagnosed afterwards; the dumps go to "<data>/freeze-dumps". */
    setenv("MK8D_FREEZE_WATCHDOG", "15", 0);
    /* Full screen from the start (Odyssey measured input-to-present latency p50 63 -> 30 ms against the window). ⌃⌘F or the
       green button leaves it; windowed when "<data>/windowed" exists. */
    char windowed[PATH_MAX]; join(windowed, data, "windowed");
    if (stat(windowed, &st)) setenv("BY3_FULLSCREEN", "1", 0);
    for (const char *const *v = (const char *const[]){"TOTK_ASSUME_FOCUS", "TOTK_VERIFY_STARTUP", "TOTK_HEADLESS", NULL}; *v; v++) unsetenv(*v);
    if (chdir(data)) return 1;
    char stamp[32]; time_t launched = time(NULL); strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", localtime(&launched));
    back_up_saves(profile, data, stamp);

    pid_t child = fork();
    if (child < 0) return 1;
    if (child == 0) {
        char previous[PATH_MAX]; join(previous, data, "previous-run.log");
        rename(log, previous);   /* keep the last session's log: a crash is often reported after relaunching */
        int fd = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); close(fd); }
        execl(runtime, "mk8d-native", modules, "boot", (char *)NULL);
        perror("Mario Kart 8 Deluxe: could not start the game"); _exit(127);
    }
    for (;;) {
        int status; pid_t r = waitpid(child, &status, 0);
        if (r == child) {
            int code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
            if (code) {   /* a crash: keep its log apart from the next session's */
                char crash[PATH_MAX], name[64]; snprintf(name, sizeof name, "crash-%s.log", stamp);
                join(crash, data, name); rename(log, crash);
            }
            return code;
        }
        if (r < 0 && errno != EINTR) return 1;
    }
}
