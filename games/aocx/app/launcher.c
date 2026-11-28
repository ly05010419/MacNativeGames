/* Hyrule Warriors: Age of Calamity.app launcher.
   The game runtime and indexes live in the app; the large RomFS image and
   all writable data live outside the app in Application Support. */
#include "app_launcher.h"

#define NAME "Hyrule Warriors: Age of Calamity"
#define ROMFS_IMAGE_SIZE 11719235972ULL

static int check_game_data(const char *home) {
    char data[PATH_MAX], game_data[PATH_MAX], image[PATH_MAX];
    struct stat st;

    launcher_join(data, home, "Library/Application Support/MacNativeGames/AOCX");
    launcher_join(game_data, data, "GameData");
    launcher_join(image, game_data, "romfs.bin");

    if (stat(image, &st) || !S_ISREG(st.st_mode) ||
        (uint64_t)st.st_size != ROMFS_IMAGE_SIZE) {
        fprintf(stderr, NAME ": missing or invalid external RomFS: %s\n", image);
        return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    int check = launcher_check_asked(argc, argv, NAME);
    if (check < 0) return 2;

    struct launcher_bundle b;
    if (launcher_find_bundle(&b)) return 1;

    char runtime[PATH_MAX], modules[PATH_MAX], romfs[PATH_MAX], fonts[PATH_MAX];
    char main_module[PATH_MAX], subsdk[PATH_MAX], sdk[PATH_MAX];
    char index[PATH_MAX], json_index[PATH_MAX];

    launcher_join(runtime, b.game, "build/aocx-native");
    launcher_join(modules, b.game, "rewritten/local");
    launcher_join(romfs, b.game, "romfs");
    launcher_join(fonts, b.game, "fonts");

    launcher_join(main_module, modules, "main.patched.bin");
    launcher_join(subsdk, modules, "subsdk0.patched.bin");
    launcher_join(sdk, modules, "sdk.patched.bin");
    launcher_join(index, romfs, "runtime-index.bin");
    launcher_join(json_index, romfs, "index.json");

    if (access(runtime, X_OK)) {
        perror(NAME ": missing runtime");
        return 1;
    }

    if (access(main_module, R_OK) || access(subsdk, R_OK) ||
        access(sdk, R_OK) || access(index, R_OK) ||
        access(json_index, R_OK)) {
        fprintf(stderr, NAME ": app game resources are incomplete\n");
        return 1;
    }

    const char *home = getenv("HOME");
    if (!home || !*home || check_game_data(home)) return 1;

    if (check) {
        puts(NAME " launcher: runtime, modules, indexes and external RomFS present; game was not launched");
        return 0;
    }

    char folder[PATH_MAX];
    struct launcher_data d;

    if (launcher_data_folder(folder, &b, NAME, NULL, NULL, "AOCX")) return 1;
    launcher_data_files(&d, folder);

    char cache[PATH_MAX], slot[PATH_MAX], game_data[PATH_MAX];
    char patches_out[PATH_MAX], aoc_dir[PATH_MAX];

    launcher_join(cache, d.data, "shader-cache/rt-aocx-native-v6-shared-memory");
    launcher_join(slot, d.data, "game-slot");
    launcher_join(game_data, d.data, "GameData");
    launcher_join(patches_out, d.data, "romfs-patches-out");
    launcher_join(aoc_dir, d.data, "AddOnContent");

    mkdir(d.data, 0755);
    mkdir(cache, 0755);
    mkdir(slot, 0755);
    mkdir(game_data, 0755);
    mkdir(patches_out, 0755);
    mkdir(aoc_dir, 0755);

    setenv("TOTK_GPU_ALLOW_SUBMIT", "1", 1);
    setenv("TOTK_GPU_WIDEN", "1", 1);
    setenv("TOTK_GPU_BLOCKRT", "1", 1);
    setenv("TOTK_GPU_MIDCLEAR", "1", 1);
    setenv("TOTK_GPU_KEEPALIVE", "0", 0);

    launcher_common_env(&d, cache, romfs, fonts, slot, "Hyrule Warriors: Age of Calamity.app");

    setenv("TOTK_ROMFS_DATA", game_data, 1);
    setenv("TOTK_ROMFS_PATCHES", "0", 1);
    setenv("TOTK_ROMFS_PATCHES_OUT", patches_out, 1);
    setenv("TOTK_ROMFS_MODS", "0", 1);
    setenv("TOTK_AOC_DIR", aoc_dir, 1);
    setenv("TOTK_SHADER_SEED", "off", 1);

    launcher_set_locale();

    struct launcher_play play = {
        .name = NAME,
        .data = &d,
        .runtime = runtime,
        .argv0 = "aocx-native",
        .modules = modules,
        .stamp_with_pid = 1
    };

    return launcher_play(&play);
}
