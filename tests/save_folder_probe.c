/* Verify that the save-folder accessor uses the current profile and the same account ID as the host filesystem. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "runtime.h"

void runtime_account_uid(uint64_t out[2]) { out[0] = 0x1234; out[1] = 0xabcd; }
_Noreturn void runtime_fail(const char *message) { fprintf(stderr, "fail: %s\n", message); exit(1); }

int main(int argc, char **argv) {
    if (argc != 2 || setenv("TOTK_PROFILE_DIR", argv[1], 1)) return 2;
    const char *path = runtime_fs_save_root();
    char expected[4096];
    snprintf(expected, sizeof expected, "%s/save/000000000000abcd0000000000001234", argv[1]);
    struct stat info;
    if (strcmp(path, expected) || stat(path, &info) || !S_ISDIR(info.st_mode)) return 3;
    puts("save root override: ok");
    return 0;
}
