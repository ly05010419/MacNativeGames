/* Test Metroid Dread's RomFS path normalizer with synthetic names; no game files/keys. */
#include "../games/dread/runtime/romfs_path.h"
#include <stdio.h>

typedef struct { const char *mount, *path, *expected; } Case;
int main(void) {
    const Case cases[] = {
        {"rom", "rom:/system/snd/../../sounds/streams/title.bfstm", "sounds/streams/title.bfstm"},
        {"rom", "rom://packs/players/samus.pkg", "packs/players/samus.pkg"},
        {"rom", "rom:/dir/./file", "dir/file"},
        {"rom", "rom:/dir//file", "dir/file"},
        {"rom", "rom:/a/b/../c", "a/c"},
        {"rom", "rom:/dir/a..b", "dir/a..b"},
        {"rom", "rom:/dir/.hidden", "dir/.hidden"},
        {"rom", "rom:/dir/file/", "dir/file"},
        {"rom", "rom:/dir/文件.bin", "dir/文件.bin"},
        {"rom", "rom:/../file", NULL},
        {"rom", "rom:/dir/../../file", NULL},
        {"rom", "rom:/dir/..", NULL},
        {"rom", "rom:/", NULL},
        {"rom", "rom:////", NULL},
        {"rom", "rom://dir\\file", NULL},
        {"rom", "rom:file", NULL},
        {"rom", "other:/file", NULL},
        {"rom", "rom2:/file", NULL},
        {NULL, "rom:/file", NULL},
        {"rom", NULL, NULL},
    };
    char out[64];
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        const char *result = romfs_path_normalize(cases[i].mount, cases[i].path, out, sizeof out);
        if ((!result != !cases[i].expected) || (result && strcmp(result, cases[i].expected))) {
            fprintf(stderr, "case %zu failed: %s -> %s (expected %s)\n", i, cases[i].path ? cases[i].path : "(null)",
                    result ? result : "(null)", cases[i].expected ? cases[i].expected : "(null)");
            return 1;
        }
    }
    char small[8];   /* a result longer than the buffer is refused, not truncated */
    if (romfs_path_normalize("rom", "rom:/longer/than/eight", small, sizeof small)) { fputs("overflow case failed\n", stderr); return 1; }
    printf("PASS: %zu Dread RomFS path cases\n", sizeof cases / sizeof cases[0] + 1);
    return 0;
}
