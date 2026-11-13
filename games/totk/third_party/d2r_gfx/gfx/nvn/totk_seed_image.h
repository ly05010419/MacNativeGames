// The shader seed's per-shader files as a few image files beside its folder, <seed>.img.000, .001, ... (2026-11-13, after
// nxnative's b-<chain>.img, dm/gfx/nvn/msl_image.h): ios/tools/seed_image.py writes them. An iPad unpacked the seed into
// about 128,000 files at the first start after every update and removed the last seed's 128,000 afterwards; with the
// image the files stay where they arrived, and a shader's file is found by a binary search of its part's index and only
// its pages are mapped. Each part is a whole image of its own (no file spans two), 1 GiB at most.
//
// Format (little-endian), written by seed_image.py write_parts:
//   header, 64 bytes: "TOTKIMG1", u32 version (1), u32 count, u64 index offset, u64 names offset, u64 names length, zeros
//   the files' bytes, each starting at a multiple of 16
//   index: count entries of { u64 offset, u64 size, u32 name offset, u32 name length }, sorted by name (bytes)
//   names: the entries' names, UTF-8, one after another
// Read only, from any thread, once opened (seed_image_open, before the warm-up threads start).
#pragma once
#import <Foundation/Foundation.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

enum { SEED_IMAGE_PARTS = 64, SEED_IMAGE_HEADER = 64 };
typedef struct { uint64_t off, size; uint32_t name, nlen; } seed_image_ent;
typedef struct { int fd; uint32_t count; const seed_image_ent *e; const char *names; void *map; size_t map_len; } seed_image_part;
typedef struct { int parts; seed_image_part p[SEED_IMAGE_PARTS]; } seed_image;

static uint64_t seed_image_u64(const uint8_t *b) { uint64_t v; memcpy(&v, b, 8); return v; }
static uint32_t seed_image_u32(const uint8_t *b) { uint32_t v; memcpy(&v, b, 4); return v; }

/* One part: its header read, its index and names mapped (a part whose header or index does not fit its size is refused). */
static int seed_image_part_open(const char *path, seed_image_part *out) {
    int fd = open(path, O_RDONLY); struct stat st;
    if (fd < 0) return 0;
    uint8_t h[SEED_IMAGE_HEADER];
    if (fstat(fd, &st) || pread(fd, h, sizeof h, 0) != (ssize_t)sizeof h || memcmp(h, "TOTKIMG1", 8) || seed_image_u32(h + 8) != 1) { close(fd); return 0; }
    uint32_t count = seed_image_u32(h + 12); uint64_t index = seed_image_u64(h + 16), names = seed_image_u64(h + 24), nlen = seed_image_u64(h + 32);
    uint64_t size = (uint64_t)st.st_size;
    if (index < SEED_IMAGE_HEADER || names != index + (uint64_t)count * sizeof(seed_image_ent) || names + nlen != size) { close(fd); return 0; }
    uint64_t page = (uint64_t)getpagesize(), start = index - index % page;
    void *m = mmap(NULL, (size_t)(size - start), PROT_READ, MAP_SHARED, fd, (off_t)start);
    if (m == MAP_FAILED) { close(fd); return 0; }
    const seed_image_ent *e = (const seed_image_ent *)((const char *)m + (index - start));
    for (uint32_t i = 0; i < count; i++)
        if (e[i].off < SEED_IMAGE_HEADER || e[i].off + e[i].size > index || (uint64_t)e[i].name + e[i].nlen > nlen) { munmap(m, (size_t)(size - start)); close(fd); return 0; }
    *out = (seed_image_part){ fd, count, e, (const char *)m + (names - start), m, (size_t)(size - start) };
    return 1;
}
/* <folder>.img.000, .001, ... as long as they are there; 0 parts when there is none (a folder of files, as before). */
static void seed_image_open(seed_image *img, NSString *folder) {
    img->parts = 0;
    for (int i = 0; i < SEED_IMAGE_PARTS; i++) {
        NSString *path = [folder stringByAppendingFormat:@".img.%03d", i];
        if (!seed_image_part_open(path.fileSystemRepresentation, &img->p[img->parts])) break;
        img->parts++;
    }
}
static const seed_image_ent *seed_image_find_part(const seed_image_part *p, const char *name, size_t len) {
    uint32_t lo = 0, hi = p->count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2; const seed_image_ent *x = &p->e[mid];
        int c = memcmp(p->names + x->name, name, x->nlen < len ? x->nlen : len);
        if (!c) c = x->nlen < len ? -1 : x->nlen > len;
        if (!c) return x;
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return NULL;
}
/* The file's part and entry, or 0 when the image has no file of that name. */
static int seed_image_find(const seed_image *img, NSString *name, const seed_image_part **part, const seed_image_ent **ent) {
    const char *n = name.UTF8String; size_t len = n ? strlen(n) : 0;
    if (!len) return 0;
    for (int i = 0; i < img->parts; i++) {
        const seed_image_ent *x = seed_image_find_part(&img->p[i], n, len);
        if (x) { *part = &img->p[i]; *ent = x; return 1; }
    }
    return 0;
}
/* The file's bytes, mapped read only (only its pages); the mapping goes with the data. nil when the image has no such
   file or the mapping fails. An empty file is empty data. */
static NSData *seed_image_data(const seed_image *img, NSString *name) {
    const seed_image_part *p; const seed_image_ent *x;
    if (!seed_image_find(img, name, &p, &x)) return nil;
    if (!x->size) return [NSData data];
    uint64_t page = (uint64_t)getpagesize(), delta = x->off % page;
    size_t len = (size_t)(x->size + delta);
    void *m = mmap(NULL, len, PROT_READ, MAP_SHARED, p->fd, (off_t)(x->off - delta));
    if (m == MAP_FAILED) return nil;
    return [[NSData alloc] initWithBytesNoCopy:(char *)m + delta length:(NSUInteger)x->size deallocator:^(void *bytes, NSUInteger length) { (void)bytes; (void)length; munmap(m, len); }];
}
