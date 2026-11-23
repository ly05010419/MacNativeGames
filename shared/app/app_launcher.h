/* The plumbing every app launcher of a MNG Launcher port shares (shared/README.md): Super Mario Odyssey
   (games/odyssey/scripts/odyssey_app_launcher.c), Animal Crossing (games/acnh/scripts/acnh_app_launcher.c) and Echoes of
   Wisdom (games/eow/app/launcher.c) include it through a link next to their launcher. Before 2026-11-18 each launcher
   carried its own copy of this code.

   Here: the bundle found by the real executable path, the player's data folder in ~/Library/Application Support/
   MacNativeGames/<folder> (moved once from an older folder next to the app), save backups (the newest five kept), the
   Mac's first preferred language, the environment that names the data folder's and the bundle's files, log rotation and
   the run loop (fork, exec, wait, and the start again that restart-requested.txt asks for).

   Not here, on purpose: each game's own tuning (its GPU and presentation variables, Animal Crossing's frame rates and
   add-ons, Odyssey's session limit, display, face buttons and MetalFX switch, Echoes of Wisdom's start by the runtime's
   real path), its checks and its arguments. They stay in the game's launcher, which reads as the list of what that game
   is given; this file only does what all three did the same way. Every function is static, so a launcher compiles only
   what it calls (the language helper needs -framework CoreFoundation only where it is called). */
#ifndef MNG_APP_LAUNCHER_H
#define MNG_APP_LAUNCHER_H

#include <CoreFoundation/CoreFoundation.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
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

#define LAUNCHER_UNUSED __attribute__((unused))
#define LAUNCHER_BACKUPS_KEPT 5
#define LAUNCHER_DATA_ROOT "Library/Application Support/MacNativeGames"   /* under HOME; MNG Launcher's Storage and Saves pages look there */

static LAUNCHER_UNUSED void launcher_join(char *out, const char *dir, const char *name) {
    if (snprintf(out, PATH_MAX, "%s/%s", dir, name) >= PATH_MAX) { fputs("path too long\n", stderr); exit(1); }
}
static LAUNCHER_UNUSED void launcher_parent_of(char *out, const char *path) {
    char copy[PATH_MAX]; snprintf(copy, sizeof copy, "%s", path); snprintf(out, PATH_MAX, "%s", dirname(copy));
}
/* A system tool, waited for: its exit code, -1 when it could not run. */
static LAUNCHER_UNUSED int launcher_run(const char *const argv[]) {
    pid_t pid = fork();
    if (pid == 0) { execv(argv[0], (char *const *)argv); _exit(127); }
    int status = 0; if (pid < 0 || waitpid(pid, &status, 0) < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* "" or --check (the packager's self-check: 1), else the usage line and -1. */
static LAUNCHER_UNUSED int launcher_check_asked(int argc, char **argv, const char *name) {
    int check = argc == 2 && !strcmp(argv[1], "--check");
    if (argc != 1 && !check) { fprintf(stderr, "usage: %s [--check]\n", name); return -1; }
    return check;
}

/* The bundle, found from the executable's real path (links resolved): Contents/MacOS/<launcher>. beside is the folder that
   holds the .app. */
struct launcher_bundle {
    char macos[PATH_MAX], contents[PATH_MAX], app[PATH_MAX], beside[PATH_MAX], resources[PATH_MAX], game[PATH_MAX];
};
static LAUNCHER_UNUSED int launcher_find_bundle(struct launcher_bundle *b) {
    char exe[PATH_MAX], real[PATH_MAX]; uint32_t size = sizeof exe;
    if (_NSGetExecutablePath(exe, &size) || !realpath(exe, real)) return -1;
    launcher_parent_of(b->macos, real); launcher_parent_of(b->contents, b->macos);
    launcher_parent_of(b->app, b->contents); launcher_parent_of(b->beside, b->app);
    launcher_join(b->resources, b->contents, "Resources"); launcher_join(b->game, b->resources, "game");
    return 0;
}

/* The player's data folder: $<override_env> when it is set (an absolute path; NULL: no override), else
   ~/Library/Application Support/MacNativeGames/<folder>, made when missing. A folder called <old_folder> next to the app
   (NULL: none; where the data was before 2026-11-17) is moved there once, never over data already in the new place; when
   it cannot be moved it is used where it is. */
static LAUNCHER_UNUSED int launcher_data_folder(char *data, const struct launcher_bundle *b, const char *name,
                                                const char *override_env, const char *old_folder, const char *folder) {
    const char *override = override_env ? getenv(override_env) : NULL;
    if (override && *override) {
        if (override[0] != '/' || snprintf(data, PATH_MAX, "%s", override) >= PATH_MAX) {
            fprintf(stderr, "%s must be an absolute directory path\n", override_env); return -1;
        }
    } else {
        const char *home = getenv("HOME"); char support[PATH_MAX]; struct stat st;
        if (!home || !*home) { fprintf(stderr, "%s: HOME is not set\n", name); return -1; }
        launcher_join(support, home, LAUNCHER_DATA_ROOT); launcher_join(data, support, folder);
        if (old_folder) {
            char old[PATH_MAX]; launcher_join(old, b->beside, old_folder);
            if (stat(data, &st) && !stat(old, &st) && S_ISDIR(st.st_mode)) {
                if (launcher_run((const char *const[]){"/bin/mkdir", "-p", support, NULL})
                    || launcher_run((const char *const[]){"/bin/mv", old, data, NULL})) {
                    fprintf(stderr, "%s: could not move %s to %s; using it where it is\n", name, old, data);
                    snprintf(data, PATH_MAX, "%s", old);
                }
            }
        }
    }
    launcher_run((const char *const[]){"/bin/mkdir", "-p", data, NULL});
    return 0;
}

/* The files every game keeps in its data folder. profile/ is made (0700), commands.txt emptied for this start. The shader
   cache and the game slot are named by each game (Odyssey's are not these folders). */
struct launcher_data {
    char data[PATH_MAX], profile[PATH_MAX], commands[PATH_MAX], controls[PATH_MAX], log[PATH_MAX];
};
static LAUNCHER_UNUSED void launcher_data_files(struct launcher_data *d, const char *data) {
    snprintf(d->data, PATH_MAX, "%s", data);
    launcher_join(d->profile, data, "profile"); launcher_join(d->commands, data, "commands.txt");
    /* The settings panel (key 1) keeps controls.txt, keyboard.txt and its other one-word files here. */
    launcher_join(d->controls, data, "controls.txt"); launcher_join(d->log, data, "last-run.log");
    mkdir(data, 0755); mkdir(d->profile, 0700);
    FILE *cf = fopen(d->commands, "w"); if (cf) fclose(cf);
}

/* What the runtime is told about the data folder and the bundle's files. purpose names the app holding the one-game slot
   (TOTK_SLOT_OWNER=user: the player's game goes ahead of test runs). TOTK_CONTROLS is each game's own line. */
static LAUNCHER_UNUSED void launcher_common_env(const struct launcher_data *d, const char *cache, const char *romfs,
                                                const char *fonts, const char *slot, const char *purpose) {
    setenv("TOTK_PROFILE_DIR", d->profile, 1);
    setenv("TOTK_SHADER_CACHE", cache, 1);
    setenv("TOTK_ROMFS", romfs, 1);
    setenv("TOTK_SHARED_FONTS", fonts, 1);
    /* The RomFS check's proof file (runtime/romfs.c) goes into the data folder, not into the bundle: written beside the
       bundled RomFS it broke the app's code signature after the first launch (Odyssey, 23 October). */
    setenv("TOTK_ROMFS_PROOF_DIR", d->data, 1);
    setenv("TOTK_GAME_SLOT_DIR", slot, 1);
    setenv("TOTK_INPUT_FILE", d->commands, 1);
    /* The leaderboard's and the shared screenshots' folder (runtime/board_upload.h, shot_upload.h): their switches and what
       waits to be sent; the board's random id is one folder up, MacNativeGames/leaderboard-id.txt, the Zelda games' too.
       Nothing is sent unless the player turns 我要上榜 or 分享我的截图 on in the settings panel (key 1). A test run keeps
       its own. */
    setenv("TOTK_BOARD_DIR", d->data, 0);
    setenv("TOTK_SLOT_OWNER", "user", 1);
    setenv("TOTK_SLOT_PURPOSE", purpose, 1);
    for (const char *const *v = (const char *const[]){"TOTK_ASSUME_FOCUS", "TOTK_VERIFY_STARTUP", "TOTK_HEADLESS", NULL}; *v; v++)
        unsetenv(*v);
}

/* The game's language follows the Mac's first preferred language (runtime/services.c reads TOTK_LOCALE). The runtime's
   own fallback, the current locale, is the app bundle's localization inside an app, so a Mac set to Chinese would get
   English (seen with Animal Crossing, 2026-11-17). TOTK_LOCALE or TOTK_LANG given to the app decide first. Needs
   -framework CoreFoundation. */
static LAUNCHER_UNUSED void launcher_set_locale(void) {
    if (getenv("TOTK_LOCALE") || getenv("TOTK_LANG")) return;
    CFArrayRef list = CFLocaleCopyPreferredLanguages(); char tag[64] = {0};
    if (list) {
        if (CFArrayGetCount(list) > 0)
            CFStringGetCString(CFArrayGetValueAtIndex(list, 0), tag, sizeof tag, kCFStringEncodingUTF8);
        CFRelease(list);
    }
    if (tag[0]) setenv("TOTK_LOCALE", tag, 1);
}

/* This start's time stamp, the backup folder's name: <date>-<time>, with the launcher's process id when with_pid (two
   starts in one second then get two folders). */
static LAUNCHER_UNUSED void launcher_stamp(char *out, size_t size, int with_pid) {
    char date[32]; time_t now = time(NULL); strftime(date, sizeof date, "%Y%m%d-%H%M%S", localtime(&now));
    if (with_pid) snprintf(out, size, "%s-%ld", date, (long)getpid());
    else snprintf(out, size, "%s", date);
}

static LAUNCHER_UNUSED int launcher_newest_first(const void *a, const void *b) { return strcmp(*(char *const *)b, *(char *const *)a); }
/* Saves are rewritten in place (runtime/fs_host.c), so a crash during a write could leave the only copy damaged: keep the
   saves folder as it was before each of the last five starts in <data>/backups/<stamp>. */
static LAUNCHER_UNUSED void launcher_back_up_folder(const char *saves, const char *data, const char *stamp) {
    char backups[PATH_MAX], target[PATH_MAX]; struct stat st;
    launcher_join(backups, data, "backups");
    if (stat(saves, &st) || !S_ISDIR(st.st_mode)) return;
    mkdir(backups, 0700); launcher_join(target, backups, stamp);
    if (launcher_run((const char *const[]){"/bin/cp", "-Rp", saves, target, NULL})) return;
    DIR *dir = opendir(backups); if (!dir) return;
    char *names[256]; int n = 0; struct dirent *e;
    while ((e = readdir(dir)) && n < 256) if (e->d_name[0] != '.') names[n++] = strdup(e->d_name);
    closedir(dir);
    qsort(names, (size_t)n, sizeof names[0], launcher_newest_first);   /* stamps sort by time */
    for (int i = 0; i < n; i++) {
        if (i >= LAUNCHER_BACKUPS_KEPT) {
            char old[PATH_MAX]; launcher_join(old, backups, names[i]);
            launcher_run((const char *const[]){"/bin/rm", "-rf", old, NULL});
        }
        free(names[i]);
    }
}
/* The Nintendo SDK games' saves: <profile>/save. */
static LAUNCHER_UNUSED void launcher_back_up_saves(const char *profile, const char *data, const char *stamp) {
    char saves[PATH_MAX]; launcher_join(saves, profile, "save"); launcher_back_up_folder(saves, data, stamp);
}

/* The game's exit code once it ends (a signal: 1). */
static LAUNCHER_UNUSED int launcher_wait(pid_t child, const char *stamp, void *context) {
    (void)stamp; (void)context;
    int status = 0;
    while (waitpid(child, &status, 0) < 0) if (errno != EINTR) return 1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

/* One play: saves backed up, the runtime started as <argv0> <modules> boot with its output in last-run.log (the last
   session's kept as previous-run.log: a crash is often reported after relaunching), and waited for by wait (NULL:
   launcher_wait), which gets this start's stamp. When the game closed because the settings panel's "Restart now" wrote
   restart-requested.txt (Animal Crossing's new frame rate goes into the game as it starts), the file is removed and the
   game started again, its saves backed up again. */
struct launcher_play {
    const char *name;               /* for messages: "Odyssey" */
    const struct launcher_data *data;
    const char *runtime, *argv0, *modules;
    int stamp_with_pid;
    int (*wait)(pid_t child, const char *stamp, void *context);
    void *context;
    const char *saves;              /* the folder backed up before each start; NULL: <profile>/save */
};
static LAUNCHER_UNUSED int launcher_play(const struct launcher_play *p) {
    const struct launcher_data *d = p->data;
    char restart[PATH_MAX]; launcher_join(restart, d->data, "restart-requested.txt"); unlink(restart);
    if (chdir(d->data)) return 1;
    for (;;) {
        char stamp[64]; launcher_stamp(stamp, sizeof stamp, p->stamp_with_pid);
        if (p->saves) launcher_back_up_folder(p->saves, d->data, stamp);
        else launcher_back_up_saves(d->profile, d->data, stamp);
        pid_t child = fork();
        if (child < 0) return 1;
        if (child == 0) {
            char previous[PATH_MAX]; launcher_join(previous, d->data, "previous-run.log");
            rename(d->log, previous);
            int fd = open(d->log, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); close(fd); }
            execl(p->runtime, p->argv0, p->modules, "boot", (char *)NULL);
            char message[128]; snprintf(message, sizeof message, "%s: could not start the game", p->name);
            perror(message); _exit(127);
        }
        int code = (p->wait ? p->wait : launcher_wait)(child, stamp, p->context);
        if (unlink(restart)) return code;   /* no restart asked: the game's own end */
    }
}

#endif
