// Identity of a RomFS image (DMROMFS1, romfs_image.h), compared at iOS startup: a build made with DM_COPY_ROMFS=NO carries <App>/romfs.id, and only a Documents/romfs.img with the same identity is used.
// The identity is cheap to recompute (the image's size and a hash of its 32-byte header and index, never of the file data), so it can be checked at every start even for a 14 GB image.
// File format (dm/tools/romfs_image.py --identity writes it): one ASCII line ending in a newline,
//   DMROMFSID1 title=<16 hex digits> version=<text> build=<text> size=<decimal bytes> index=<16 lower-case hex digits>
// size and index decide the match; title, version and build (the packed game's title ID, display version and package version) only name the image in messages.
// index = FNV-1a 64 (offset 0xcbf29ce484222325, prime 0x100000001b3) over the little-endian 64-bit words of header (32 bytes) followed by the index (zero-padded to a multiple of 8 bytes).
#pragma once
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct { uint64_t size, index; char title[24], version[40], build[40]; } romfs_id;
enum { ROMFS_ID_OK, ROMFS_ID_NONE /* no expected identity: not a slim build */, ROMFS_ID_BAD /* the expected identity cannot be read */,
       ROMFS_ID_MISSING /* no image */, ROMFS_ID_UNREADABLE /* an image that is cut off or invalid */, ROMFS_ID_DIFFERENT };

static uint64_t romfs_id_mix(uint64_t h, const uint8_t *p, size_t n) {   // n multiple of 8
    for (size_t i = 0; i < n; i += 8) {
        uint64_t w = 0; for (int k = 7; k >= 0; k--) w = w << 8 | p[i + (size_t)k];
        h = (h ^ w) * 0x100000001b3ull;
    }
    return h;
}
// Size and index hash of the image at path (title / version / build empty). 0 with errno set for a missing, cut off or invalid image.
static int romfs_id_of_image(const char *path, romfs_id *out) {
    memset(out, 0, sizeof *out);
    int fd = open(path, O_RDONLY); if (fd < 0) return 0;
    struct stat st; uint8_t h[32]; uint64_t ioff, isz, hash = 0xcbf29ce484222325ull;
    if (fstat(fd, &st) || pread(fd, h, sizeof h, 0) != (ssize_t)sizeof h) { int e = errno ? errno : EINVAL; close(fd); errno = e; return 0; }
    memcpy(&ioff, h + 8, 8); memcpy(&isz, h + 16, 8);
    if (memcmp(h, "DMROMFS1", 8) || ioff < sizeof h || ioff > (uint64_t)st.st_size || isz != (uint64_t)st.st_size - ioff) { close(fd); errno = EINVAL; return 0; }
    hash = romfs_id_mix(hash, h, sizeof h);
    enum { CHUNK = 1 << 20 };
    uint8_t *buf = malloc(CHUNK + 8); if (!buf) { close(fd); errno = ENOMEM; return 0; }
    for (uint64_t at = 0; at < isz;) {
        size_t n = isz - at < CHUNK ? (size_t)(isz - at) : (size_t)CHUNK, got = 0;
        while (got < n) {
            ssize_t r = pread(fd, buf + got, n - got, (off_t)(ioff + at + got));
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) { free(buf); close(fd); errno = EINVAL; return 0; }
            got += (size_t)r;
        }
        size_t padded = (n + 7) & ~(size_t)7; memset(buf + n, 0, padded - n);
        hash = romfs_id_mix(hash, buf, padded); at += n;
    }
    free(buf); close(fd);
    out->size = (uint64_t)st.st_size; out->index = hash; return 1;
}
static void romfs_id_field(char *dst, size_t cap, const char *v, size_t n) {
    if (n >= cap) n = cap - 1;
    memcpy(dst, v, n); dst[n] = 0;
}
// Parses the identity line in text (NUL-terminated). 1 when it has the magic, a size and an index.
static int romfs_id_parse(const char *text, romfs_id *out) {
    memset(out, 0, sizeof *out);
    if (strncmp(text, "DMROMFSID1", 10) || (text[10] != ' ' && text[10] != '\n' && text[10])) return 0;
    int have = 0; const char *p = text + 10;
    while (*p && *p != '\n') {
        while (*p == ' ') p++;
        const char *e = p; while (*e && *e != ' ' && *e != '\n') e++;
        const char *eq = memchr(p, '=', (size_t)(e - p));
        if (eq) {
            size_t kn = (size_t)(eq - p), vn = (size_t)(e - eq - 1); const char *v = eq + 1; char num[24];
            if (kn == 5 && !strncmp(p, "title", 5)) romfs_id_field(out->title, sizeof out->title, v, vn);
            else if (kn == 7 && !strncmp(p, "version", 7)) romfs_id_field(out->version, sizeof out->version, v, vn);
            else if (kn == 5 && !strncmp(p, "build", 5)) romfs_id_field(out->build, sizeof out->build, v, vn);
            else if ((kn == 4 && !strncmp(p, "size", 4)) || (kn == 5 && !strncmp(p, "index", 5))) {
                romfs_id_field(num, sizeof num, v, vn); char *end; int hex = kn == 5; errno = 0;
                unsigned long long x = strtoull(num, &end, hex ? 16 : 10);
                if (!vn || *end || errno) return 0;
                if (hex) { out->index = x; have |= 2; } else { out->size = x; have |= 1; }
            }
        }
        p = e;
    }
    return have == 3;
}
static int romfs_id_same(const romfs_id *a, const romfs_id *b) { return a->size == b->size && a->index == b->index; }
__attribute__((unused)) static void romfs_id_text(const romfs_id *x, char *buf, size_t cap) {   // the line romfs_id_parse reads (without the newline)
    snprintf(buf, cap, "DMROMFSID1 title=%s version=%s build=%s size=%" PRIu64 " index=%016" PRIx64, x->title, x->version, x->build, x->size, x->index);
}
// Checks the image against the identity file expected_path: want / got are filled as far as they could be read.
static int romfs_id_check(const char *expected_path, const char *image_path, romfs_id *want, romfs_id *got) {
    char text[512]; memset(want, 0, sizeof *want); memset(got, 0, sizeof *got);
    FILE *f = fopen(expected_path, "r");
    if (!f) return errno == ENOENT ? ROMFS_ID_NONE : ROMFS_ID_BAD;
    size_t n = fread(text, 1, sizeof text - 1, f); fclose(f); text[n] = 0;
    if (!romfs_id_parse(text, want)) return ROMFS_ID_BAD;
    if (!romfs_id_of_image(image_path, got)) return errno == ENOENT ? ROMFS_ID_MISSING : ROMFS_ID_UNREADABLE;
    return romfs_id_same(want, got) ? ROMFS_ID_OK : ROMFS_ID_DIFFERENT;
}
// The player-facing text for a verdict other than OK / NONE. bundle_id names the app in the command that puts the right image on the phone.
__attribute__((unused)) static void romfs_id_message(int rc, const romfs_id *want, const romfs_id *got, const char *bundle_id, char *buf, size_t cap) {
    char w[160]; snprintf(w, sizeof w, "%s %s (build %s, %" PRIu64 " bytes, index %016" PRIx64 ")", want->title, want->version, want->build, want->size, want->index);
    const char *what = rc == ROMFS_ID_MISSING ? "There is no game file image on this device." : rc == ROMFS_ID_UNREADABLE ? "The game file image on this device is incomplete or invalid."
                     : rc == ROMFS_ID_DIFFERENT ? "The game file image on this device is for another version." : "This app's game file identity (romfs.id) cannot be read; install the app again.";
    int n = snprintf(buf, cap, "%s Expected: %s.", what, w);
    if (rc == ROMFS_ID_DIFFERENT && n > 0 && (size_t)n < cap) n += snprintf(buf + n, cap - (size_t)n, " Found: %" PRIu64 " bytes, index %016" PRIx64 ".", got->size, got->index);
    if (rc != ROMFS_ID_BAD && n > 0 && (size_t)n < cap)
        snprintf(buf + n, cap - (size_t)n, " On the Mac run: scripts/push_romfs.sh <device UDID> %s <RomFS.img> (build_ipa_from_nsp.py prints the whole command).", bundle_id);
}
