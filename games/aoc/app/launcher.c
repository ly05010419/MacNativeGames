// Minimal app-bundle launcher for this Mac development build.
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>

static void make_dir(const char *path) {
    if (!mkdir(path, 0755) || errno == EEXIST) return;
    fprintf(stderr, "mkdir %s: %s\n", path, strerror(errno));
    exit(1);
}

static void join(char *out, size_t n, const char *base, const char *name) {
    if (snprintf(out, n, "%s/%s", base, name) >= (int)n) {
        fprintf(stderr, "path too long: %s/%s\n", base, name);
        exit(1);
    }
}

int main(void) {
    char exe[PATH_MAX], app[PATH_MAX], game[PATH_MAX], run[PATH_MAX], save[PATH_MAX], temp[PATH_MAX], log[PATH_MAX];
    uint32_t n = sizeof exe;
    if (_NSGetExecutablePath(exe, &n) || !realpath(exe, app)) {
        fprintf(stderr, "cannot locate app executable\n");
        return 1;
    }
    char *slash = strrchr(app, '/'); if (!slash) return 1;
    *slash = 0;                                   // Contents/MacOS
    slash = strrchr(app, '/'); if (!slash) return 1;
    *slash = 0;                                   // Contents
    join(game, sizeof game, app, "Resources/game");
    // The save and log live in Application Support: a newly signed app cannot write arbitrary paths on the external game volume until macOS grants removable-volume access,
    // and writing them inside Resources would invalidate the bundle signature.
    const char *home = getenv("HOME");
    if (!home) return 1;
    const char *profile = getenv("DM_AOC_PROFILE");
    if (profile && *profile) {
        // Package validation uses an existing disposable directory. Resolve
        // symlinks before rejecting a directory inside the signed app.
        char bundle[PATH_MAX];
        struct stat state_info;
        if (*profile != '/' || !realpath(profile, run) || stat(run, &state_info) || !S_ISDIR(state_info.st_mode)) {
            fprintf(stderr, "DM_AOC_PROFILE must name an existing absolute directory\n");
            return 1;
        }
        memcpy(bundle, app, strlen(app) + 1);
        char *contents = strrchr(bundle, '/'); if (!contents) return 1;
        *contents = 0;
        size_t len = strlen(bundle);
        if (!strncmp(run, bundle, len) && (!run[len] || run[len] == '/')) {
            fprintf(stderr, "profile must be outside the signed app\n");
            return 1;
        }
    } else {
        join(run, sizeof run, home, "Library/Application Support/Hyrule Warriors Age of Calamity");
    }
    join(save, sizeof save, run, "save");
    join(temp, sizeof temp, run, "temp");
    make_dir(run); make_dir(save); make_dir(temp);
    char lockpath[PATH_MAX];
    join(lockpath, sizeof lockpath, run, "session.lock");
    int lock = open(lockpath, O_RDWR | O_CREAT, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB)) {
        fprintf(stderr, "Hyrule Warriors: Age of Calamity is already running or its profile is unavailable\n");
        return 1;
    }
    // Keep the lock through exec so a second app cannot write this save profile.
    char filename[64];
    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now)) { perror("session time"); return 1; }
    snprintf(filename, sizeof filename, "session-%lld-%09ld-%ld.log", (long long)now.tv_sec, now.tv_nsec, (long)getpid());
    join(log, sizeof log, run, filename);
    int fd = open(log, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) { perror("session log"); return 1; }
    if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); if (fd > 2) close(fd); }

    setenv("DM_ROOT", game, 1);
    char romfs[PATH_MAX], msl[PATH_MAX], shmap[PATH_MAX], font[PATH_MAX];
    join(romfs, sizeof romfs, game, "RomFS");
    join(msl, sizeof msl, game, "shader-cache");
    join(shmap, sizeof shmap, game, "profile/shader_map.tsv");
    join(font, sizeof font, game, "font");
    setenv("DM_ROMFS", romfs, 1);
    setenv("DM_SAVE", save, 1);
    setenv("DM_TEMP", temp, 1);
    setenv("DM_MSL_DIR", msl, 1);
    setenv("DM_NVN_SHMAP", shmap, 1);
    setenv("DM_FONT_DIR", font, 1);
    setenv("DM_METAL_RUNTIME", "0", 1);
    char language_path[PATH_MAX], language[64] = "en-US";
    join(language_path, sizeof language_path, game, "language.txt");
    FILE *settings = fopen(language_path, "r");
    if (settings) {
        if (fgets(language, sizeof language, settings)) language[strcspn(language, "\n\r")] = 0;
        fclose(settings);
    }
    setenv("DM_LANG", language, 1);
    setenv("DM_WINDOW", "1", 1);
    setenv("DM_AUDIO", "1", 1);

    char host[PATH_MAX], patched[PATH_MAX];
    join(host, sizeof host, game, "dmhost");
    join(patched, sizeof patched, game, "patched");
    if (chdir(run)) { perror("chdir"); return 1; }
    execl(host, host, patched, (char *)NULL);
    perror("launch dmhost");
    return 1;
}
