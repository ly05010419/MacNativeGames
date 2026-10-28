/* Animal Crossing: New Horizons.app launcher
   Adapted from Odyssey/TotK/Kirby native app launchers for Apple Silicon. */
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
static int run(const char *const argv[]) {
    pid_t pid = fork();
    if (pid == 0) { execv(argv[0], (char *const *)argv); _exit(127); }
    int status = 0; if (pid < 0 || waitpid(pid, &status, 0) < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
static int newest_first(const void *a, const void *b) { return strcmp(*(char *const *)b, *(char *const *)a); }
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

int main(int argc, char **argv) {
    int check = argc == 2 && !strcmp(argv[1], "--check");
    if (argc != 1 && !check) { fputs("usage: Animal Crossing [--check]\n", stderr); return 2; }
    char exe[PATH_MAX], real[PATH_MAX]; uint32_t size = sizeof exe;
    if (_NSGetExecutablePath(exe, &size) || !realpath(exe, real)) return 1;
    char macos[PATH_MAX], contents[PATH_MAX], app[PATH_MAX], home[PATH_MAX];
    parent_of(macos, real); parent_of(contents, macos); parent_of(app, contents); parent_of(home, app);

    char game[PATH_MAX], runtime[PATH_MAX], modules[PATH_MAX], romfs[PATH_MAX], fonts[PATH_MAX], resources[PATH_MAX];
    join(resources, contents, "Resources"); join(game, resources, "game");
    join(runtime, macos, "acnh-native"); join(modules, game, "rewritten");
    join(romfs, game, "romfs"); join(fonts, game, "fonts");

    if (access(runtime, X_OK)) { perror("Animal Crossing: missing runtime"); return 1; }
    if (check) { puts("Animal Crossing launcher: runtime exists; game was not launched"); return 0; }

    char data[PATH_MAX], profile[PATH_MAX], cache[PATH_MAX], commands[PATH_MAX], log[PATH_MAX];
    const char *override = getenv("ACNH_DATA_DIR");
    if (override && *override) {
        if (override[0] != '/' || snprintf(data, sizeof data, "%s", override) >= (int)sizeof data) {
            fputs("ACNH_DATA_DIR must be an absolute directory path\n", stderr); return 1;
        }
    } else join(data, home, "Animal Crossing Data");
    join(profile, data, "profile");
    join(cache, data, "shader-cache");
    join(commands, data, "commands.txt"); join(log, data, "last-run.log");
    mkdir(data, 0755); mkdir(profile, 0700); mkdir(cache, 0755);

    FILE *cf = fopen(commands, "w"); if (cf) fclose(cf);
    setenv("TOTK_GPU_ALLOW_SUBMIT", "1", 1);
    setenv("TOTK_GPU_MIDCLEAR", "1", 1);
    setenv("TOTK_GPU_WIDEN", "1", 1);
    setenv("TOTK_GPU_BLOCKRT", "1", 1);
    setenv("DM_PRESENT_HZ", "60", 1);
    setenv("TOTK_UNCAPPED", "0", 1);
    setenv("DM_PRESERVE_INVARIANCE", "1", 1);
    setenv("DM_NVN_GPU_CODE_MAX_MINOR", "31", 1);
    setenv("TOTK_PROFILE_DIR", profile, 1);
    setenv("TOTK_SHADER_CACHE", cache, 1);
    setenv("TOTK_ROMFS", romfs, 1);
    setenv("TOTK_SHARED_FONTS", fonts, 1);
    setenv("TOTK_INPUT_FILE", commands, 1);
    setenv("TOTK_SLOT_OWNER", "user", 1);
    setenv("TOTK_SLOT_PURPOSE", "Animal Crossing New Horizons.app", 1);
    setenv("TOTK_MOVIE_PACE", "0", 1);
    setenv("TOTK_SHADER_ASYNC", "0", 0);
    setenv("TOTK_MFX_UPSCALE", "0", 0);
    setenv("TOTK_MFX_OUTPUT", "0", 0);

    for (const char *const *v = (const char *const[]){"TOTK_ASSUME_FOCUS", "TOTK_VERIFY_STARTUP", "TOTK_HEADLESS", NULL}; *v; v++) unsetenv(*v);
    if (chdir(data)) return 1;
    char stamp[64], date[32]; time_t launched = time(NULL); strftime(date, sizeof date, "%Y%m%d-%H%M%S", localtime(&launched));
    snprintf(stamp, sizeof stamp, "%s-%ld", date, (long)getpid());
    back_up_saves(profile, data, stamp);

    pid_t child = fork();
    if (child < 0) return 1;
    if (child == 0) {
        char previous[PATH_MAX]; join(previous, data, "previous-run.log");
        rename(log, previous);
        int fd = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); close(fd); }
        execl(runtime, "acnh-native", modules, "boot", (char *)NULL);
        perror("Animal Crossing: could not start the game"); _exit(127);
    }
    int status;
    waitpid(child, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
