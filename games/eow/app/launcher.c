/* The Legend of Zelda: Echoes of Wisdom.app launcher: points the runtime at the app's game files and the player's data
   folder, then runs it. Adapted from games/acnh/scripts/acnh_app_launcher.c (third_party/COPIES.md); since 2026-11-18 the
   plumbing every port's launcher shares (the bundle, the data folder, backups, the common environment, the language,
   logs, the run loop) is app_launcher.h, a link into shared/app/, and this file keeps what is this game's own.

   App:    Contents/MacOS/Echoes of Wisdom (this); Contents/Resources/game/{build/, rewritten/v110/, romfs/, fonts/}.
           build/ holds the runtime, and next to it the translator, the shader seed and the compiled shader libraries. The
           runtime is started by its real path: the graphics layer finds shader-libs/ beside the executable path without
           resolving links (nvn_mtl.m exe_rel), so started through a link in MacOS/ it found none of the 9,233 libraries,
           compiled every shader while the intro movie waited, and the window stayed black (2026-11-17).
   Data:   ~/Library/Application Support/MacNativeGames/EoW: profile/ (saves), shader-cache/, game-slot/, controls.txt,
           last-run.log; the working directory of the game. EOW_DATA_DIR=<absolute dir> uses another folder. */
#include "app_launcher.h"

int main(int argc, char **argv) {
    int check = launcher_check_asked(argc, argv, "Echoes of Wisdom");
    if (check < 0) return 2;
    struct launcher_bundle b;
    if (launcher_find_bundle(&b)) return 1;
    char runtime[PATH_MAX], modules[PATH_MAX], romfs[PATH_MAX], fonts[PATH_MAX], index[PATH_MAX];
    launcher_join(runtime, b.game, "build/eow-native"); launcher_join(modules, b.game, "rewritten/v110");
    launcher_join(romfs, b.game, "romfs"); launcher_join(fonts, b.game, "fonts"); launcher_join(index, romfs, "runtime-index.bin");
    if (access(runtime, X_OK)) { perror("Echoes of Wisdom: missing runtime"); return 1; }
    if (access(index, R_OK)) { perror("Echoes of Wisdom: missing game data"); return 1; }
    if (check) { puts("Echoes of Wisdom launcher: runtime and game data present; game was not launched"); return 0; }

    char folder[PATH_MAX]; struct launcher_data d;
    if (launcher_data_folder(folder, &b, "Echoes of Wisdom", "EOW_DATA_DIR", NULL, "EoW")) return 1;
    launcher_data_files(&d, folder);
    char cache[PATH_MAX], slot[PATH_MAX];
    launcher_join(cache, d.data, "shader-cache"); launcher_join(slot, d.data, "game-slot");
    mkdir(cache, 0755); mkdir(slot, 0755);

    /* The README's play settings (games/eow/README.md, Play). */
    setenv("TOTK_GPU_ALLOW_SUBMIT", "1", 1);
    setenv("TOTK_GPU_BLOCKRT", "1", 1);
    setenv("TOTK_GPU_MIDCLEAR", "1", 1);
    setenv("TOTK_GPU_WIDEN", "1", 1);
    setenv("TOTK_GPU_BUDGET_MB", "5000", 0);
    setenv("DM_PRESENT_LINK_FALLBACK", "1", 1);
    /* No recording of runtime-compiled shader sources: nvn_mtl.m keeps them next to the executable for
       scripts/build_shader_libs.py, which inside the app is the signed bundle (2026-11-18: two .metal files appeared in
       game/build/shader-libs/src after play). The work folder's own runs keep recording. */
    setenv("DM_SHADER_LIBS_RECORD", "0", 0);
    launcher_common_env(&d, cache, romfs, fonts, slot, "Echoes of Wisdom.app");
    setenv("TOTK_CONTROLS", d.controls, 1);
    launcher_set_locale();

    struct launcher_play play = {.name = "Echoes of Wisdom", .data = &d, .runtime = runtime, .argv0 = "eow-native",
                                 .modules = modules, .stamp_with_pid = 1};
    return launcher_play(&play);
}
