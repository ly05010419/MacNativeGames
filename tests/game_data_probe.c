/* The runtime's game data reader (games/totk/runtime/game_data.c) with synthetic files; no game files or keys.
   game_data_probe <file> <key> [number|sha256|text]: prints the value; a refused file or value exits 3 (runtime_fail). */
#include "../games/totk/runtime/game_data.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Noreturn void runtime_fail(const char *message) { fprintf(stderr, "FAIL: %s\n", message); exit(3); }

int main(int argc, char **argv) {
    if (argc != 4) return 2;
    if (!strcmp(argv[3], "number")) printf("%llu\n", (unsigned long long)runtime_game_number(argv[1], argv[2]));
    else if (!strcmp(argv[3], "sha256")) printf("%s\n", runtime_game_sha256(argv[1], argv[2]));
    else printf("%s\n", runtime_game_text(argv[1], argv[2]));
    return 0;
}
