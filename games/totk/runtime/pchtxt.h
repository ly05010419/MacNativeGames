/* TOTK_PCHTXT=<file>: an emulator ExeFS patch for main in pchtxt form, as community mods ship them (for example
   TOTK-Ultrawide's aspect-ratio patch), for trying them here. One reader for both games (game_patches.c and
   game_patches_botw.c include it). Mac only: the phone's code is patched at build. Its @nsobid must be main's build id,
   read from the extracted ExeFS (extracted/<TOTK_GAME_DIR>/exefs/main at 0x40); the lines after @enabled are
   "<main address> <bytes>", @disabled switches the lines that follow off again (a section the author keeps without
   applying it) and @stop ends the file. "@flag offset_shift 0x100" must come before the first patch line: it says the
   addresses leave out the NSO header only the IPS form counts, which is how they are applied here; a file without it,
   or with another shift, would land 0x100 bytes off and stops the start. "@flag print_values" (a display option of the
   patching tools) is accepted, any other flag stops the start. The same reading as the build's --mod
   (scripts/mod_exefs.py). A line outside main or with a byte that is not two hex digits stops the start, and so does a
   line on one of the caller's reserved sites. It is applied before the runtime's own patches, whose checks of the
   original words then refuse a file that overlaps them. */
#ifndef PCHTXT_H
#define PCHTXT_H
#include "game_patches.h"
#include "game_version.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct { uint32_t off, size; } PchtxtSite;

static int pchtxt_hex_digit(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* tag: the log prefix ("totk", "botw"); reserved: the sites a line may not touch (none: NULL, 0). */
static void pchtxt_apply(Module *m, const char *tag, const PchtxtSite *reserved, size_t reserved_count) {
    const char *path = getenv("TOTK_PCHTXT");
    if (!path || !*path) return;
    FILE *f = fopen(path, "r");
    if (!f) runtime_fail("TOTK_PCHTXT: cannot open the file");
    char nso[256], want[41] = {0}; uint8_t id[20];
    snprintf(nso, sizeof nso, "extracted/%s/exefs/main", TOTK_GAME_DIR);
    FILE *n = fopen(nso, "rb");
    if (!n || fseek(n, 0x40, SEEK_SET) || fread(id, 1, sizeof id, n) != sizeof id) runtime_fail("TOTK_PCHTXT: cannot read main's build id from extracted/<version>/exefs/main");
    fclose(n);
    for (int i = 0; i < 20; i++) snprintf(want + 2 * i, 3, "%02X", id[i]);
    char line[1024]; int enabled = 0, matched = 0, shifted = 0; unsigned count = 0, bytes = 0;
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "@nsobid-", 8)) {
            if (strncasecmp(line + 8, want, 40)) runtime_fail("TOTK_PCHTXT is for another build of main");
            matched = 1; continue;
        }
        if (!strncmp(line, "@enabled", 8)) { enabled = 1; continue; }
        if (!strncmp(line, "@disabled", 9)) { enabled = 0; continue; }
        if (!strncmp(line, "@flag", 5)) {
            char name[64] = "", value[64] = "";
            sscanf(line + 5, "%63s %63s", name, value);
            if (!strcmp(name, "print_values")) continue;
            if (strcmp(name, "offset_shift")) runtime_fail("TOTK_PCHTXT: only @flag offset_shift 0x100 and print_values are supported");
            if (strtoull(value, NULL, 16) != 0x100) runtime_fail("TOTK_PCHTXT: @flag offset_shift must be 0x100 (the addresses are main addresses)");
            shifted = 1; continue;
        }
        if (!strncmp(line, "@stop", 5)) break;
        unsigned long long at; char hex[512];
        if (!enabled || line[0] == '@' || sscanf(line, "%llx %511s", &at, hex) != 2) continue;
        size_t len = strlen(hex);
        if (!matched) runtime_fail("TOTK_PCHTXT: patches before its @nsobid line");
        if (!shifted) runtime_fail("TOTK_PCHTXT: no @flag offset_shift 0x100 before the first patch line");
        if (len % 2 || at > m->file_size || len / 2 > m->file_size - at) runtime_fail("TOTK_PCHTXT: a patch line is malformed or outside main");
        for (size_t i = 0; i < len; i++)
            if (pchtxt_hex_digit(hex[i]) < 0) runtime_fail("TOTK_PCHTXT: a patch line's bytes are not hex digits");
        for (size_t i = 0; i < reserved_count; i++)
            if (at < reserved[i].off + reserved[i].size && reserved[i].off < at + len / 2) {
                fprintf(stderr, "[%s] TOTK_PCHTXT: main+%#llx touches main+%#x, which the app patches or checks itself\n", tag, at, reserved[i].off);
                runtime_fail("TOTK_PCHTXT: a patch line touches a site the app patches or checks itself");
            }
        for (size_t i = 0; i < len / 2; i++) m->base[at + i] = (uint8_t)(pchtxt_hex_digit(hex[2 * i]) << 4 | pchtxt_hex_digit(hex[2 * i + 1]));
        count++; bytes += (unsigned)(len / 2);
    }
    fclose(f);
    if (!matched) runtime_fail("TOTK_PCHTXT has no @nsobid line");
    fprintf(stderr, "[%s] TOTK_PCHTXT %s: %u patches, %u bytes in main (build id %s)\n", tag, path, count, bytes, want);
}
#endif
