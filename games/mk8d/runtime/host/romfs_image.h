// DMROMFS1's index loader, shared by the host mount and iOS startup. Startup must validate the same index before replacing a working folder.
#pragma once
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct { uint64_t a, b; uint32_t name; uint16_t nlen; uint8_t type, pad; } __attribute__((packed)) img_ent;   // directory: a = first child, b = children; file: a = offset, b = size
_Static_assert(sizeof(img_ent) == 24, "RomFS image entry layout");
typedef struct { int fd; uint64_t n; img_ent *e; const char *names; } romfs_img;

static void romfs_image_close(romfs_img *m) {
    if (m) { close(m->fd); free(m->e); free(m); }
}
static int romfs_image_read(int fd, void *buf, size_t size, off_t off) {
    size_t got = 0;
    while (got < size) {
        ssize_t n = pread(fd, (char *)buf + got, size - got, off + (off_t)got);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { if (!n) errno = EINVAL; return 0; }
        got += (size_t)n;
    }
    return 1;
}
// NULL for an incomplete or invalid image (errno EINVAL), an I/O error, or allocation failure. The caller owns the descriptor and index on success.
static romfs_img *romfs_image_open(const char *path) {
    int fd = open(path, O_RDONLY); struct stat st; uint8_t h[32]; romfs_img *m = NULL; uint64_t ioff, isz, n;
    if (fd < 0) return NULL;
    if (fstat(fd, &st) || !romfs_image_read(fd, h, sizeof h, 0)) goto bad;
    errno = EINVAL;
    if (memcmp(h, "DMROMFS1", 8) || st.st_size < (off_t)sizeof h) goto bad;
    memcpy(&ioff, h + 8, 8); memcpy(&isz, h + 16, 8); memcpy(&n, h + 24, 8);
    if (!n || n > UINT32_MAX || ioff < sizeof h || ioff > (uint64_t)st.st_size || isz != (uint64_t)st.st_size - ioff || isz > SIZE_MAX || n > isz / sizeof(img_ent)) goto bad;
    if (!(m = calloc(1, sizeof *m))) goto bad;
    m->fd = fd;
    if (!(m->e = malloc((size_t)isz)) || !romfs_image_read(fd, m->e, (size_t)isz, (off_t)ioff)) goto bad;
    m->n = n; m->names = (const char *)m->e + n * sizeof(img_ent);
    uint64_t pool = isz - n * sizeof(img_ent), next_child = 1;
    errno = EINVAL;
    if (m->e[0].type || m->e[0].nlen) goto bad;
    for (uint64_t k = 0; k < n; k++) {
        const img_ent *x = &m->e[k];
        if ((uint64_t)x->name + x->nlen > pool || x->type > 1 || x->pad) goto bad;
        const char *name = m->names + x->name;
        if (k && (!x->nlen || x->nlen > 0x300 || memchr(name, 0, x->nlen) || memchr(name, '/', x->nlen) || (x->nlen == 1 && name[0] == '.') || (x->nlen == 2 && !memcmp(name, "..", 2)))) goto bad;
        if (x->type) { if (x->a < sizeof h || x->a > ioff || x->b > ioff - x->a) goto bad; }
        else {
            // Breadth-first child runs partition entries 1..n-1; reject cycles, aliases and unreachable entries.
            if (x->a != next_child || x->a <= k || x->a > n || x->b > n - x->a) goto bad;
            next_child += x->b;
        }
    }
    if (next_child != n) goto bad;
    for (uint64_t k = 0; k < n; k++) {
        const img_ent *d = &m->e[k]; if (d->type) continue;
        for (uint64_t j = 1; j < d->b; j++) {
            const img_ent *a = &m->e[d->a + j - 1], *b = a + 1;
            size_t len = a->nlen < b->nlen ? a->nlen : b->nlen;
            int c = memcmp(m->names + a->name, m->names + b->name, len);
            if (c > 0 || (!c && a->nlen >= b->nlen)) goto bad;
        }
    }
    return m;
bad: {
    int e = errno ? errno : EINVAL;
    if (m) romfs_image_close(m); else close(fd);
    errno = e; return NULL;
    }
}
