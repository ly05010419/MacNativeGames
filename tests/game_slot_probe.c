/* A game's runtime/game_slot.c, its pure parts: which processes are games, and the slot folder used when
   TOTK_GAME_SLOT_DIR is unset (the one Tears of the Kingdom's and Ocarina of Time's launchers pass). Built with
   -DGAME_SLOT_C='"../games/<game>/runtime/game_slot.c"' and run by tests/test_game_slot.py. "watch": an agent's game closes
   itself when idle or over its time (agent_slot_limit.h), on the real slot code with a test slot folder. */
#include GAME_SLOT_C

_Noreturn void runtime_fail(const char *message) { fprintf(stderr, "runtime_fail: %s\n", message); exit(3); }

/* The agent game watch (agent_slot_limit.h): the game's first presented frame and its close are the probe's. */
static _Atomic uint64_t probe_frames;
static uint64_t probe_t0;
uint64_t dm_nvn_frame_count(void) { return atomic_load(&probe_frames); }
static double probe_elapsed(void) { return (double)(clock_gettime_nsec_np(CLOCK_MONOTONIC) - probe_t0) / 1e9; }
void runtime_front_request_close(void) {
    printf("close requested at %.1f s\n", probe_elapsed());
    fflush(stdout);
    exit(0);   /* the window's close ends the game; at_exit writes the slot history's exit line */
}

/* watch: take the slot (TOTK_GAME_SLOT_DIR, TOTK_SLOT_OWNER, TOTK_AGENT_* from the test), then play a timeline: the first
   frame at <frame_s> (negative: never), an input-file command at each <command_s>, a real user input at each <user_s>;
   wait until <end_s> and say "no close" unless the watch closed the game first. Times in seconds from the slot taken. */
static int watch(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: game-slot-probe watch <frame_s> <end_s> [c<command_s>|u<user_s>]...\n"); return 2; }
    runtime_game_slot_acquire("boot");
    probe_t0 = clock_gettime_nsec_np(CLOCK_MONOTONIC);
    double frame = atof(argv[2]), end = atof(argv[3]);
    int n = argc - 4, done[64] = {0}, framed = frame < 0;
    if (n > 64) return 2;
    while (probe_elapsed() < end) {
        double t = probe_elapsed();
        if (!framed && t >= frame) { atomic_store(&probe_frames, 1); framed = 1; }
        for (int i = 0; i < n; i++)
            if (!done[i] && t >= atof(argv[4 + i] + 1)) { runtime_slot_activity(argv[4 + i][0] == 'u'); done[i] = 1; }
        usleep(20000);
    }
    printf("no close by %.1f s\n", probe_elapsed());
    return 0;
}

static int failures;
static void expect(int ok, const char *what) { if (!ok) { fprintf(stderr, "FAIL: %s\n", what); failures++; } }

int main(int argc, char **argv) {
    if (argc >= 2 && !strcmp(argv[1], "watch")) return watch(argc, argv);
    if (argc != 2) { fprintf(stderr, "usage: game-slot-probe <home>\n"); return 2; }
    expect(slot_path_is_game("/Applications/Ocarina of Time.app/Contents/MacOS/OcarinaOfTime"), "Ocarina of Time's App");
    expect(slot_path_is_game("/w/scratch/build/oot/build/oot-native"), "the built Ocarina of Time runtime");
    expect(slot_path_is_game("/w/build/totk-native.base"), "a renamed totk-native copy");
    expect(slot_path_is_game("/A/Tears of the Kingdom.app/Contents/MacOS/TOTK"), "Tears of the Kingdom's App");
    expect(slot_path_is_game("/A/Breath of the Wild.app/Contents/MacOS/BotW"), "Breath of the Wild's App");
    expect(slot_path_is_game("/A/Pokémon Pokopia.app/Contents/MacOS/Pokopia"), "Pokopia's App");
    expect(slot_path_is_game("/A/Donkey Kong Bananza.app/Contents/MacOS/Bananza"), "Bananza's App");
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
