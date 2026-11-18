/* Animal Crossing: New Horizons.app launcher
   Adapted from Odyssey/TotK/Kirby native app launchers for Apple Silicon.

   The plumbing every port's launcher shares (the bundle, the data folder and its move, backups, the common environment,
   logs, the run loop with the settings panel's "Restart now") is app_launcher.h, a link into shared/app/; what follows is
   this game's own: its data folder per version, its GPU and presentation settings, its frame-rate and add-on files. */
#include "app_launcher.h"

#ifndef ACNH_DATA_FOLDER
#error "ACNH_DATA_FOLDER: the packager passes the version's data folder name"
#endif

int main(int argc, char **argv) {
    int check = launcher_check_asked(argc, argv, "Animal Crossing");
    if (check < 0) return 2;
    struct launcher_bundle b;
    if (launcher_find_bundle(&b)) return 1;
    char runtime[PATH_MAX], modules[PATH_MAX], romfs[PATH_MAX], fonts[PATH_MAX];
    launcher_join(runtime, b.macos, "acnh-native"); launcher_join(modules, b.game, "rewritten");
    launcher_join(romfs, b.game, "romfs"); launcher_join(fonts, b.game, "fonts");

    if (access(runtime, X_OK)) { perror("Animal Crossing: missing runtime"); return 1; }
    if (check) { puts("Animal Crossing launcher: runtime exists; game was not launched"); return 0; }

    /* ~/Library/Application Support/MacNativeGames/<ACNH_DATA_FOLDER> (one folder per game version, as the other
       ports keep theirs; MNG Launcher's Storage and Saves pages look there). Until 2026-11-17 the data sat next
       to the app as "Animal Crossing Data": a folder found there is moved once, never copied over newer data.
       ACNH_DATA_DIR=<absolute dir> uses another folder. */
    char folder[PATH_MAX]; struct launcher_data d;
    if (launcher_data_folder(folder, &b, "Animal Crossing", "ACNH_DATA_DIR", "Animal Crossing Data", ACNH_DATA_FOLDER)) return 1;
    launcher_data_files(&d, folder);
    char cache[PATH_MAX], slot[PATH_MAX];
    launcher_join(cache, d.data, "shader-cache"); launcher_join(slot, d.data, "game-slot");
    mkdir(cache, 0755); mkdir(slot, 0755);

    setenv("TOTK_GPU_ALLOW_SUBMIT", "1", 1);
    setenv("TOTK_GPU_MIDCLEAR", "1", 1);
    setenv("TOTK_GPU_WIDEN", "1", 1);
    setenv("TOTK_GPU_BLOCKRT", "1", 1);
    setenv("DM_PRESENT_HZ", "60", 1);
    setenv("TOTK_UNCAPPED", "0", 1);
    unsetenv("DM_PRESENT_INTERVAL"); /* The saved 30/60 mode owns presentation cadence. */
    char fps_assets[PATH_MAX], fps_manifest[PATH_MAX];
    launcher_join(fps_assets, b.game, "fps60"); launcher_join(fps_manifest, fps_assets, "manifest.json");
    if (!access(fps_manifest, R_OK)) setenv("ACNH_FPS_ASSETS", fps_assets, 1);
    else unsetenv("ACNH_FPS_ASSETS");
    launcher_join(fps_assets, b.game, "fps120"); launcher_join(fps_manifest, fps_assets, "manifest.json");
    if (!access(fps_manifest, R_OK)) setenv("ACNH_FPS120_ASSETS", fps_assets, 1);
    else unsetenv("ACNH_FPS120_ASSETS");
    setenv("DM_PRESERVE_INVARIANCE", "1", 1);
    setenv("DM_NVN_GPU_CODE_MAX_MINOR", "31", 1);
    launcher_common_env(&d, cache, romfs, fonts, slot, "Animal Crossing New Horizons.app");
    char dlc[PATH_MAX]; launcher_join(dlc, b.game, "dlc");
    setenv("ACNH_DLC_DIR", dlc, 1);
    setenv("TOTK_CONTROLS", d.controls, 1);
    setenv("TOTK_MOVIE_PACE", "0", 1);
    setenv("TOTK_SHADER_ASYNC", "0", 0);
    /* MetalFX upscaling stays off unless the settings panel's Picture row (graphics-output.txt) turns it on;
       TOTK_MFX_UPSCALE / TOTK_MFX_OUTPUT given to the app still decide first. */
    /* The game's language: runtime/services.c reads the Mac's first preferred language itself when no TOTK_LOCALE is
       given, so this launcher sets none (launcher_set_locale would give the same). */

    /* The settings panel's "Restart now" (a new frame rate goes into the game as it starts): the game writes
       restart-requested.txt and closes; launcher_play removes it and starts the game again, saves backed up again. */
    struct launcher_play play = {.name = "Animal Crossing", .data = &d, .runtime = runtime, .argv0 = "acnh-native",
                                 .modules = modules, .stamp_with_pid = 1};
    return launcher_play(&play);
}
