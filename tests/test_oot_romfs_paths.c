/* Test Ocarina of Time's RomFS path parser with synthetic names; no game files/keys. */
#include "../games/oot/runtime/romfs_path.h"
#include <stdio.h>

typedef struct { const char *mount, *path, *expected; } Case;
int main(void) {
    const Case cases[] = {
        {"content", "content://Voice/Resource//Stream/line.bwav", "Voice/Resource/Stream/line.bwav"},
        {"content", "content:/Sound/Resource/Stream/opening.bwav", "Sound/Resource/Stream/opening.bwav"},
        {"content", "content:///Sound/Resource/Stream/opening.bwav", "Sound/Resource/Stream/opening.bwav"},
        {"rom", "rom:/dir///file", "dir/file"},
        {"rom", "rom:/dir/a..b", "dir/a..b"},
        {"rom", "rom:/dir/.hidden", "dir/.hidden"},
        {"rom", "rom:/dir//../file", NULL},
        {"rom", "rom:/dir//./file", NULL},
        {"rom", "rom://../file", NULL},
        {"rom", "rom:/dir//a\\b", NULL},
        {"rom", "rom:////", NULL},
        {"rom", "rom:file", NULL},
        {"rom", "other:/dir//file", NULL},
        {"rom", "rom2:/file", NULL},
        {NULL, "rom:/file", NULL},
        {"rom", NULL, NULL},
    };
    char out[64];
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        const char *result = romfs_path_relative_collapsed(cases[i].mount, cases[i].path, out, sizeof out);
        if ((!result != !cases[i].expected) || (result && strcmp(result, cases[i].expected))) {
            fprintf(stderr, "case %zu failed: %s -> %s (expected %s)\n", i,
                    cases[i].path ? cases[i].path : "(null)", result ? result : "(null)",
                    cases[i].expected ? cases[i].expected : "(null)");
            return 1;
        }
    }
    char tiny[4];
    if (romfs_path_relative_collapsed("rom", "rom:/abcd", tiny, sizeof tiny)) { fputs("overflow case failed\n", stderr); return 1; }
    if (romfs_path_relative("rom", "rom:/dir//file")) { fputs("strict parser accepted an empty component\n", stderr); return 1; }
    printf("PASS: %zu Ocarina of Time RomFS path cases\n", sizeof cases / sizeof cases[0] + 2);
    return 0;
}
