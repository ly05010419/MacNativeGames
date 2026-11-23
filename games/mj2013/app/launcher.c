/* 明星三缺一.app launcher: points the runtime at the app's game files and the player's data folder, then runs it. The
   plumbing every port's launcher shares (the bundle, the data folder, backups, logs, the run loop) is app_launcher.h, a
   link into shared/app/ (as games/eow/app/launcher.c); this file keeps what is this game's own.

   App:    Contents/MacOS/mj2013 (this); Contents/Resources/game/{build/mj2013-native, rewritten/v0/main.patched.bin,
           romfs/romfs.bin} (scripts/package_app.py). The game draws in software: no shader cache, fonts or game slot.
   Data:   ~/Library/Application Support/MacNativeGames/mj2013 (the runtime's own default): profile/sdmc (the SD card,
           the game's saves in sdmc/switch/mj2013/save), backups/ (the card as it was before each of the last five
           starts), commands.txt, last-run.log; the working directory of the game. MJ2013_DATA_DIR=<absolute dir> uses
           another folder (test runs). */
#include "app_launcher.h"

#define NAME "明星三缺一"

int main(int argc, char **argv) {
    int check = launcher_check_asked(argc, argv, NAME);
    if (check < 0) return 2;
    struct launcher_bundle b;
    if (launcher_find_bundle(&b)) return 1;
    char runtime[PATH_MAX], modules[PATH_MAX], romfs[PATH_MAX], main_module[PATH_MAX], image[PATH_MAX];
    launcher_join(runtime, b.game, "build/mj2013-native"); launcher_join(modules, b.game, "rewritten/v0");
    launcher_join(romfs, b.game, "romfs"); launcher_join(main_module, modules, "main.patched.bin");
    launcher_join(image, romfs, "romfs.bin");
    if (access(runtime, X_OK)) { perror(NAME ": missing runtime"); return 1; }
    if (access(main_module, R_OK) || access(image, R_OK)) { perror(NAME ": missing game data"); return 1; }
    if (check) { puts(NAME " launcher: runtime and game data present; game was not launched"); return 0; }

    char folder[PATH_MAX]; struct launcher_data d;
    if (launcher_data_folder(folder, &b, NAME, "MJ2013_DATA_DIR", NULL, "mj2013")) return 1;
    launcher_data_files(&d, folder);
    char card[PATH_MAX]; launcher_join(card, d.profile, "sdmc");

    /* Only what this runtime reads (runtime/hb_fs.c, input.c): launcher_common_env would also name a shader cache,
       fonts and a game slot this game does not have. A test run's switches are not passed on to the player's game. */
    setenv("TOTK_PROFILE_DIR", d.profile, 1);
    setenv("TOTK_ROMFS", romfs, 1);
    setenv("TOTK_INPUT_FILE", d.commands, 1);
    for (const char *const *v = (const char *const[]){"TOTK_HEADLESS", "HB_TRACE_IPC", "HB_IPC_STATS", "TOTK_TRACE_SVC", NULL}; *v; v++)
        unsetenv(*v);

    struct launcher_play play = {.name = NAME, .data = &d, .runtime = runtime, .argv0 = "mj2013-native",
                                 .modules = modules, .stamp_with_pid = 1, .saves = card};
    return launcher_play(&play);
}
