/* Persona 5 Royal.app launcher: points the runtime at the app's game files and the player's data folder, then runs it.
   The plumbing every port's launcher shares (the bundle, the data folder, backups, the common environment, logs, the run
   loop with the settings panel's "Restart now") is app_launcher.h, a link into shared/app/ (as games/acnh's and
   games/mj2013's launchers); this file keeps what is this game's own.

   App:    Contents/MacOS/P5R (this); Contents/Resources/game/{build/p5r-native, build/shader-service/totk_xlate,
           build/spirv-cross/spirv-cross, rewritten/local, romfs/{index.json, runtime-index.bin, romfs.bin}, fonts/}
           (scripts/package_app.py). The runtime finds its translator beside its build/ folder (totk_shader_rt.m root_dir).
   Data:   ~/Library/Application Support/MacNativeGames/P5R (runtime/game_version.h GAME_FOLDER): profile/ (the saves),
           shader-cache/, game-slot/, backups/, commands.txt, last-run.log. P5R_DATA_DIR=<absolute dir> uses another folder.
   The game asks for no system font (fonts/ stays empty) and runs at its own 30 fps. */
#include "app_launcher.h"

#define NAME "Persona 5 Royal"

int main(int argc, char **argv) {
    int check = launcher_check_asked(argc, argv, NAME);
    if (check < 0) return 2;
    struct launcher_bundle b;
    if (launcher_find_bundle(&b)) return 1;
    char runtime[PATH_MAX], modules[PATH_MAX], romfs[PATH_MAX], fonts[PATH_MAX], main_module[PATH_MAX], index[PATH_MAX];
    launcher_join(runtime, b.game, "build/p5r-native"); launcher_join(modules, b.game, "rewritten/local");
    launcher_join(romfs, b.game, "romfs"); launcher_join(fonts, b.game, "fonts");
    launcher_join(main_module, modules, "main.patched.bin"); launcher_join(index, romfs, "runtime-index.bin");
    if (access(runtime, X_OK)) { perror(NAME ": missing runtime"); return 1; }
    if (access(main_module, R_OK) || access(index, R_OK)) { perror(NAME ": missing game data"); return 1; }
    if (check) { puts(NAME " launcher: runtime and game data present; game was not launched"); return 0; }

    char folder[PATH_MAX]; struct launcher_data d;
    if (launcher_data_folder(folder, &b, NAME, "P5R_DATA_DIR", NULL, "P5R")) return 1;
    launcher_data_files(&d, folder);
    char cache[PATH_MAX], slot[PATH_MAX];
    launcher_join(cache, d.data, "shader-cache"); launcher_join(slot, d.data, "game-slot");
    mkdir(cache, 0755); mkdir(slot, 0755);

    /* The graphics switches its Mac test runs use (README.md, as games/rdr's and games/aocx's): GPU submission with the
       wider barriers, render-target blocking and mid-pass clears. No GPU keep-alive spin kernels: they kept a MacBook
       Air's GPU busy for nothing (2026-11-22). */
    setenv("TOTK_GPU_ALLOW_SUBMIT", "1", 1);
    setenv("TOTK_GPU_WIDEN", "1", 1);
    setenv("TOTK_GPU_BLOCKRT", "1", 1);
    setenv("TOTK_GPU_MIDCLEAR", "1", 1);
    setenv("TOTK_GPU_KEEPALIVE", "0", 0);
    launcher_common_env(&d, cache, romfs, fonts, slot, "Persona 5 Royal.app");

    struct launcher_play play = {.name = NAME, .data = &d, .runtime = runtime, .argv0 = "p5r-native",
                                 .modules = modules, .stamp_with_pid = 1};
    return launcher_play(&play);
}
