/* Odyssey.app launcher (scripts/package_native_app.py builds the bundle around it).
   Everything the game reads is inside the bundle (Contents/Resources/game: runtime, shader tools, rewritten modules,
   RomFS, shared fonts); everything it writes is in "Odyssey Data" next to the app (saves, shader cache, log), so the
   app keeps working when the project checkout is not there. Contents/MacOS/odyssey-native is a symlink to the bundled
   runtime: started through it, the window belongs to this app (name and icon), while the runtime still finds its shader
   tools next to the real file. The game runs as the user's (TOTK_SLOT_OWNER=user), so it takes the one-game slot ahead
   of test runs. "Odyssey Data/session-minutes.txt" sets a session limit (default 20, 0 = none): the copied renderer
   has caused GPU driver panics in long sessions, so sessions stay bounded until longer runs are verified. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <signal.h>
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
   saves as they were before each of the last five sessions in "Odyssey Data/backups/<date-time>". */
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
static void append(const char *path, const char *line) {
    FILE *f = fopen(path, "a"); if (f) { fputs(line, f); fclose(f); }
}

int main(void) {
    char exe[PATH_MAX], real[PATH_MAX]; uint32_t size = sizeof exe;
    if (_NSGetExecutablePath(exe, &size) || !realpath(exe, real)) return 1;
    char macos[PATH_MAX], contents[PATH_MAX], app[PATH_MAX], home[PATH_MAX];
    parent_of(macos, real); parent_of(contents, macos); parent_of(app, contents); parent_of(home, app);

    char game[PATH_MAX], runtime[PATH_MAX], modules[PATH_MAX], romfs[PATH_MAX], fonts[PATH_MAX], resources[PATH_MAX];
    join(resources, contents, "Resources"); join(game, resources, "game");
    join(runtime, macos, "odyssey-native"); join(modules, game, "rewritten/local");
    join(romfs, game, "romfs"); join(fonts, game, "fonts");

    char data[PATH_MAX], profile[PATH_MAX], cache[PATH_MAX], commands[PATH_MAX], log[PATH_MAX], minutes_path[PATH_MAX];
    join(data, home, "Odyssey Data"); join(profile, data, "profile");
    join(cache, data, "shader-cache/rt-totk-native-v4-unverified");
    join(commands, data, "commands.txt"); join(log, data, "last-run.log"); join(minutes_path, data, "session-minutes.txt");
    mkdir(data, 0755); mkdir(profile, 0700);

    /* The project's slot directory when the app was packaged there (so test runs and the app never overlap),
       otherwise one next to the data. */
    char slot[PATH_MAX] = "", slot_file[PATH_MAX]; join(slot_file, game, "slot-dir.txt");
    FILE *sf = fopen(slot_file, "r");
    if (sf) { if (fgets(slot, sizeof slot, sf)) slot[strcspn(slot, "\n")] = 0; fclose(sf); }
    struct stat st;
    if (!*slot || stat(slot, &st) || !S_ISDIR(st.st_mode)) { join(slot, data, "game-slot"); mkdir(slot, 0755); }

    double minutes = 20;
    FILE *mf = fopen(minutes_path, "r");
    if (mf) { if (fscanf(mf, "%lf", &minutes) != 1 || minutes < 0) minutes = 20; fclose(mf); }

    FILE *cf = fopen(commands, "w"); if (cf) fclose(cf);
    setenv("TOTK_GPU_ALLOW_SUBMIT", "1", 1); setenv("TOTK_GPU_WIDEN", "1", 1); setenv("TOTK_GPU_BLOCKRT", "1", 1);
    setenv("TOTK_PROFILE_DIR", profile, 1); setenv("TOTK_SHADER_CACHE", cache, 1);
    setenv("TOTK_ROMFS", romfs, 1); setenv("TOTK_SHARED_FONTS", fonts, 1); setenv("TOTK_INPUT_FILE", commands, 1);
    setenv("TOTK_GAME_SLOT_DIR", slot, 1); setenv("TOTK_SLOT_OWNER", "user", 1); setenv("TOTK_SLOT_PURPOSE", "Odyssey.app", 1);
    setenv("TOTK_SHADER_ASYNC", "1", 0);
    /* The RomFS check's proof file (runtime/romfs.c) goes next to the data, not into the bundle: written beside the
       bundled RomFS it broke the app's code signature after the first launch (23 October). */
    setenv("TOTK_ROMFS_PROOF_DIR", data, 1);
    /* The player's button layout (runtime/controller_map.h), when "Odyssey Data/controls.txt" exists. */
    char controls[PATH_MAX]; join(controls, data, "controls.txt");
    if (!stat(controls, &st)) setenv("TOTK_CONTROLS", controls, 0);
    /* How GameController's A/B/X/Y map to positions (runtime/gamepad_layout.h, TOTK_FACE_BUTTONS): "position" or "label"
       in "Odyssey Data/face-buttons.txt". "position" makes an Xbox-style controls.txt (south = a ...) match the printed
       letters on a Nintendo controller too (24 October: a Pro Controller had A and B swapped with that file). */
    char face_path[PATH_MAX]; join(face_path, data, "face-buttons.txt");
    FILE *ff = fopen(face_path, "r");
    if (ff) { char mode[16] = ""; if (fscanf(ff, "%15s", mode) == 1 && (!strcmp(mode, "position") || !strcmp(mode, "label"))) setenv("TOTK_FACE_BUTTONS", mode, 0); fclose(ff); }
    /* A freeze (no frame for 15 s) logs which threads hold and wait for the virtual cores (runtime/freeze_watchdog.c),
       so a hang in play can be diagnosed afterwards (23 October: a world-travel hang left no such record). */
    setenv("ODYSSEY_FREEZE_WATCHDOG", "15", 0);
    /* Shader variants and pipelines the game met that the app's list lacks (docs/shader-list.md), for the player to send
       in a shader-list issue: identifiers only. Without this the runtime writes no contribution file at all. */
    char contrib[PATH_MAX]; join(contrib, data, "shader-list-contribution.txt");
    setenv("TOTK_SHADER_CONTRIB", contrib, 0);
    /* MetalFX spatial upscale of the 1600x900 game output to the window (Applier's A/B, 22 October: no artifacts, no
       measurable cost, slightly crisper edges). On unless "Odyssey Data/no-metalfx" exists. */
    char no_mfx[PATH_MAX]; join(no_mfx, data, "no-metalfx");
    if (stat(no_mfx, &st)) { setenv("TOTK_MFX_UPSCALE", "1", 0); setenv("TOTK_MFX_OUTPUT", "1", 0); }
    /* Full screen from the start (Applier's 0c A/B, 22 October: input-to-present latency p50 63 -> 30 ms, because the
       windowed compositor path adds ~34 ms of display-link lead). ⌃⌘F or the green button leaves it. Windowed when
       "Odyssey Data/windowed" exists. */
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
        execl(runtime, "odyssey-native", modules, "boot", (char *)NULL);
        perror("Odyssey: could not start the game"); _exit(127);
    }
    time_t start = time(NULL), asked = 0;
    for (;;) {
        int status; pid_t r = waitpid(child, &status, WNOHANG);
        if (r == child) {
            int code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
            if (code && !asked) {   /* a crash, not the session limit: keep its log apart from the next session's */
                char crash[PATH_MAX], name[64]; snprintf(name, sizeof name, "crash-%s.log", stamp);
                join(crash, data, name); rename(log, crash);
            }
            return code;
        }
        if (r < 0 && errno != EINTR) return 1;
        time_t now = time(NULL);
        if (minutes > 0 && !asked && difftime(now, start) >= minutes * 60) {
            char note[160]; snprintf(note, sizeof note, "[Odyssey.app] %g-minute session limit: closing the game\n", minutes);
            append(log, note); append(commands, "close\n"); asked = now;
        }
        /* The game finishes a save in progress before quitting (up to 15 s, frontend.m windowWillClose); only a game
           that has not quit well after that is stopped. */
        if (asked && difftime(now, asked) >= 45) kill(child, SIGTERM);
        if (asked && difftime(now, asked) >= 60) kill(child, SIGKILL);
        sleep(1);
    }
}
