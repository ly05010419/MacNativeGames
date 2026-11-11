/* Ocarina of Time's runtime/game_slot.c, its pure parts: which processes are games, and the slot folder used when
   TOTK_GAME_SLOT_DIR is unset (the one the App's launcher passes). Built and run by tests/test_oot_game_slot.py. */
#include "../games/oot/runtime/game_slot.c"

_Noreturn void runtime_fail(const char *message) { fprintf(stderr, "runtime_fail: %s\n", message); exit(3); }

static int failures;
static void expect(int ok, const char *what) { if (!ok) { fprintf(stderr, "FAIL: %s\n", what); failures++; } }

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: oot-game-slot-probe <home>\n"); return 2; }
    expect(slot_path_is_game("/Applications/Ocarina of Time.app/Contents/MacOS/OcarinaOfTime"), "the App's game");
    expect(slot_path_is_game("/w/scratch/build/oot/build/oot-native"), "the built runtime");
    expect(slot_path_is_game("/w/build/totk-native.base"), "a renamed totk-native copy");
    expect(slot_path_is_game("/A/Tears of the Kingdom.app/Contents/MacOS/TOTK"), "Tears of the Kingdom's App");
    expect(slot_path_is_game("/A/Breath of the Wild.app/Contents/MacOS/BotW"), "Breath of the Wild's App");
    expect(!slot_path_is_game("/tmp/OcarinaOfTime"), "the App's name outside an App");
    expect(!slot_path_is_game("/A/X.app/Contents/MacOS/OcarinaOfTimeHelper"), "another program in an App");
    expect(!slot_path_is_game("/usr/bin/python3"), "a script");
    expect(!slot_path_is_game(""), "no path");

    char want[PATH_MAX], got[PATH_MAX] = "";
    snprintf(want, sizeof want, "%s/Library/Application Support/MacNativeGames/TOTK/game-slot", argv[1]);
    setenv("HOME", argv[1], 1);
    expect(slot_default_dir(got, sizeof got) && !strcmp(got, want), "the default folder is the launcher's");
    unsetenv("HOME");
    expect(!slot_default_dir(got, sizeof got), "no HOME: no default folder");

    if (failures) return 1;
    printf("pass: game processes and the default slot folder\n");
    return 0;
}
