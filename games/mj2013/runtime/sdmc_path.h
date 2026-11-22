/* Where a path on the game's SD card lives on the Mac (hb_fs.c; tests/sdmc_path_probe.c). The card is a folder of the
   profile: sdmc:/switch/mj2013/save/x -> <profile>/sdmc/switch/mj2013/save/x. A guest path is absolute, uses '/', and
   may not climb out of the card: "..", "." and empty components (other than a trailing '/') are refused, as are
   backslashes and control characters. Returns 0 and the host path, or -1. */
#pragma once
#include <stdio.h>
#include <string.h>

static inline int sdmc_host_path(const char *root, const char *guest, char *out, size_t cap) {
    if (!root || !*root || !guest || guest[0] != '/') return -1;
    size_t n = strlen(guest);
    if (n > 0x300) return -1;                                    /* FS_MAX_PATH - 1 */
    for (size_t i = 0; i < n; i++) if ((unsigned char)guest[i] < 0x20 || guest[i] == '\\') return -1;
    const char *p = guest;
    while (*p) {                                                  /* each component between slashes */
        while (*p == '/') { if (p[1] == '/') return -1; p++; }
        if (!*p) break;
        const char *e = strchr(p, '/'); size_t len = e ? (size_t)(e - p) : strlen(p);
        if ((len == 1 && p[0] == '.') || (len == 2 && p[0] == '.' && p[1] == '.')) return -1;
        p += len;
    }
    size_t used = (size_t)snprintf(out, cap, "%s/sdmc%s", root, guest);
    if (used >= cap) return -1;
    if (used > 1 && out[used - 1] == '/' && strcmp(guest, "/")) out[used - 1] = 0;   /* "dir/" names the directory */
    return 0;
}
