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
#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
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
/* <folder>.img.000, .001, ... as long as they are there; 0 parts when there is none (a folder of files, as before). A folder
   that is a link (the Mac app's build/shader-seed and build/shader-libs point into its ShaderData bundle) has its image
   beside the folder it points to, inside the bundle (games/oot/build.py shader_bundle, 2026-11-17). */
static void seed_image_open(seed_image *img, NSString *folder) {
    img->parts = 0;
    for (int pass = 0; pass < 2 && !img->parts; pass++) {
        NSString *base = pass ? folder.stringByResolvingSymlinksInPath : folder;
        if (pass && [base isEqualToString:folder]) break;
        for (int i = 0; i < SEED_IMAGE_PARTS; i++) {
            NSString *path = [base stringByAppendingFormat:@".img.%03d", i];
            if (!seed_image_part_open(path.fileSystemRepresentation, &img->p[img->parts])) break;
            img->parts++;
        }
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
/* Small files read on the replay thread, copied into memory up front (2026-11-19, Ocarina of Time on the user's play).
   A shader's variant index (<key>.variants, 300-900 bytes) is read the first time the game uses the shader in a session,
   on the thread replaying the game's commands; mapped from the image on the external drive, its first touch waited for a
   disk read, and while the game streamed an area in (0.8-0.9 GB/s of page-ins) that read held the replay 200-411 ms. All
   of them, about 13 MB for 42,848 shaders, are copied into one buffer by a background thread after the image opens; a
   file found there is served from memory, any other as before. */
typedef struct { uint8_t *buf; size_t len, files; uint32_t *at[SEED_IMAGE_PARTS]; } seed_image_preloaded;   // at: offset + 1 in buf, 0 for a file not copied
static int seed_image_name_ends(const seed_image_part *p, const seed_image_ent *x, const char *suffix, size_t slen) {
    return x->nlen >= slen && !memcmp(p->names + x->name + x->nlen - slen, suffix, slen);
}
/* Every file whose name ends with suffix, copied with pread on several threads. NULL when there is none, when they do not
   fit in 4 GiB, or when a read comes short (then nothing is preloaded: every read stays the mapped one). */
__attribute__((unused)) static seed_image_preloaded *seed_image_preload(const seed_image *img, const char *suffix) {   // unused: the launcher and runtime include this header for seed_image_data alone (-Werror)
    size_t slen = strlen(suffix), total = 0, files = 0;
    for (int i = 0; i < img->parts; i++)
        for (uint32_t k = 0; k < img->p[i].count; k++)
            if (seed_image_name_ends(&img->p[i], &img->p[i].e[k], suffix, slen)) { total += (size_t)img->p[i].e[k].size; files++; }
    if (!files || total >= UINT32_MAX) return NULL;
    seed_image_preloaded *pre = (seed_image_preloaded *)calloc(1, sizeof *pre);
    if (!pre || !(pre->buf = (uint8_t *)malloc(total ? total : 1))) { free(pre); return NULL; }
    pre->len = total; pre->files = files;
    typedef struct { int part; uint32_t k; size_t at; } job;
    job *jobs = (job *)malloc(files * sizeof *jobs);
    int ok = jobs != NULL; size_t o = 0, n = 0;
    for (int i = 0; ok && i < img->parts; i++) {
        if (!(pre->at[i] = (uint32_t *)calloc(img->p[i].count ? img->p[i].count : 1, sizeof(uint32_t)))) { ok = 0; break; }
        for (uint32_t k = 0; k < img->p[i].count; k++)
            if (seed_image_name_ends(&img->p[i], &img->p[i].e[k], suffix, slen)) { jobs[n++] = (job){i, k, o}; pre->at[i][k] = (uint32_t)o + 1; o += (size_t)img->p[i].e[k].size; }
    }
    __block int bad = !ok;
    if (ok) dispatch_apply(n, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^(size_t j) {
        const seed_image_part *p = &img->p[jobs[j].part]; const seed_image_ent *x = &p->e[jobs[j].k];
        if (x->size && pread(p->fd, pre->buf + jobs[j].at, (size_t)x->size, (off_t)x->off) != (ssize_t)x->size) __atomic_store_n(&bad, 1, __ATOMIC_RELAXED);
    });
    free(jobs);
    if (bad) { for (int i = 0; i < img->parts; i++) free(pre->at[i]); free(pre->buf); free(pre); return NULL; }
    return pre;
}
/* The file's bytes from the preloaded buffer (not copied again; the buffer lives as long as the process), nil when it was
   not preloaded. */
__attribute__((unused)) static NSData *seed_image_preloaded_data(const seed_image_preloaded *pre, const seed_image *img, NSString *name) {
    const seed_image_part *p; const seed_image_ent *x;
    if (!pre || !seed_image_find(img, name, &p, &x)) return nil;
    uint32_t a = pre->at[p - img->p][x - p->e];
    return a ? [NSData dataWithBytesNoCopy:pre->buf + a - 1 length:(NSUInteger)x->size freeWhenDone:NO] : nil;
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
