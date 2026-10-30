/* Enumerate and mount pinned PublicData RomFS images without modifying the base/update image. */
#include "runtime.h"
#include "dlc.h"
#include "romfs_path.h"
#include "fs_host.h"
#include <CommonCrypto/CommonDigest.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const char *path; uint64_t offset, size; } DlcFile;
typedef struct { int index; const char *title; uint64_t size; const char *sha256; const DlcFile *files; size_t count; } DlcImage;
#include "dlc_config.h"
#if !ACNH_DLC_COUNT
static const DlcImage dlc_images[1];
#endif
__attribute__((used)) static const char dlc_marker[] = ACNH_DLC_MARKER;
#define NSLOTS (ACNH_DLC_COUNT ? ACNH_DLC_COUNT : 1)
#define INVALID 0x2ee202u
#define NOT_FOUND 0x202u
#define NOT_INSTALLED 0x7d402u
#define READ_ONLY 0x307402u
static int image_fds[NSLOTS];
static struct stat image_stats[NSLOTS];
static struct { char name[32]; int slot; } mounts[16];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static void need(int condition, const char *message) { if (!condition) runtime_fail(message); }
static int same_file(const struct stat *a, const struct stat *b) {
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_size == b->st_size
        && a->st_mtimespec.tv_sec == b->st_mtimespec.tv_sec && a->st_mtimespec.tv_nsec == b->st_mtimespec.tv_nsec
        && a->st_ctimespec.tv_sec == b->st_ctimespec.tv_sec && a->st_ctimespec.tv_nsec == b->st_ctimespec.tv_nsec;
}
#if ACNH_DLC_COUNT
/* As for the base RomFS (romfs.c): a full hash is reused only while the same regular
 * file keeps its identity and modification metadata. TOTK_ROMFS_VERIFY=full forces one. */
typedef struct { char magic[16], sha256[65]; uint64_t dev, ino, size; struct timespec mtime, ctime, birth; } DlcProof;
static DlcProof proof_for(const struct stat *s, const char *sha256) {
    DlcProof p; memset(&p, 0, sizeof p); memcpy(p.magic, "ACNHDLC-PROOF-1", 16); snprintf(p.sha256, sizeof p.sha256, "%s", sha256);
    p.dev = (uint64_t)s->st_dev; p.ino = (uint64_t)s->st_ino; p.size = (uint64_t)s->st_size;
    p.mtime = s->st_mtimespec; p.ctime = s->st_ctimespec; p.birth = s->st_birthtimespec;
    return p;
}
static int proof_path(char path[4096], const char *title) {
    const char *dir = getenv("TOTK_ROMFS_PROOF_DIR");
    return dir && *dir && snprintf(path, 4096, "%s/.dlc-verified-%s-v1", dir, title) < 4096;
}
static int proof_reusable(const char *title, const DlcProof *want) {
    const char *force = getenv("TOTK_ROMFS_VERIFY"); char path[4096]; DlcProof saved; struct stat st;
    if ((force && !strcmp(force, "full")) || !proof_path(path, title)) return 0;
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC); if (fd < 0) return 0;
    int ok = !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size == sizeof saved && read(fd, &saved, sizeof saved) == sizeof saved;
    close(fd); return ok && !memcmp(&saved, want, sizeof saved);
}
static void proof_save(const char *title, const DlcProof *proof) {
    char path[4096], tmp[4200]; if (!proof_path(path, title)) return;
    if (snprintf(tmp, sizeof tmp, "%s.%ld.tmp", path, (long)getpid()) >= (int)sizeof tmp) return;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600); if (fd < 0) return;
    int ok = write(fd, proof, sizeof *proof) == (ssize_t)sizeof *proof && !fsync(fd); close(fd);
    if (!ok || rename(tmp, path)) unlink(tmp);
}
#endif
static void init(void) {
#if ACNH_DLC_COUNT
    const char *dir = getenv("ACNH_DLC_DIR"); if (!dir) dir = ACNH_DLC_DEFAULT_DIR;
    uint8_t *buffer = malloc(8 << 20); need(buffer != NULL, "DLC hash buffer");
    for (int i = 0; i < ACNH_DLC_COUNT; i++) {
        const DlcImage *image = &dlc_images[i]; char path[4096];
        need(snprintf(path, sizeof path, "%s/%s.romfs", dir, image->title) < (int)sizeof path, "DLC image path");
        int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC); struct stat before, after;
        if (fd < 0) { fprintf(stderr, "[acnh] missing DLC image: %s\n", path); runtime_fail("DLC image missing"); }
        need(!fstat(fd, &before) && S_ISREG(before.st_mode) && (uint64_t)before.st_size == image->size, "DLC image size/type");
        DlcProof want = proof_for(&before, image->sha256);
        if (proof_reusable(image->title, &want)) {
            image_fds[i] = fd; image_stats[i] = before;
            fprintf(stderr, "[acnh] DLC unchanged; reused full SHA256 verification record: %s index=%d files=%zu\n", image->title, image->index, image->count);
            continue;
        }
        CC_SHA256_CTX hash; CC_SHA256_Init(&hash);
        for (uint64_t pos = 0; pos < image->size;) {
            size_t size = image->size - pos > (8u << 20) ? (8u << 20) : (size_t)(image->size - pos);
            ssize_t n = pread(fd, buffer, size, (off_t)pos); need(n > 0, "DLC image hash read");
            CC_SHA256_Update(&hash, buffer, (CC_LONG)n); pos += (uint64_t)n;
        }
        uint8_t digest[32]; char hex[65]; CC_SHA256_Final(digest, &hash);
        for (unsigned j = 0; j < 32; j++) snprintf(hex + j * 2, 3, "%02x", digest[j]);
        need(!strcmp(hex, image->sha256), "DLC image SHA-256 mismatch");
        need(!fstat(fd, &after) && same_file(&before, &after), "DLC image changed during verification");
        image_fds[i] = fd; image_stats[i] = after; proof_save(image->title, &want);
        fprintf(stderr, "[acnh] DLC verified: %s index=%d files=%zu\n", image->title, image->index, image->count);
    }
    free(buffer);
#endif
    fprintf(stderr, "[acnh] add-on content: %d DLC (%s)\n", ACNH_DLC_COUNT, dlc_marker);
}
void runtime_dlc_init(void) { pthread_once(&once, init); }
int runtime_dlc_count(void) { runtime_dlc_init(); return ACNH_DLC_COUNT; }
int runtime_dlc_list(int *out, int offset, int count) {
    runtime_dlc_init();
    need(offset >= 0 && count >= 0 && (out || !count), "invalid add-on content list range/buffer");
    if (offset >= ACNH_DLC_COUNT) return 0;
    int n = ACNH_DLC_COUNT - offset; if (n > count) n = count;
    for (int i = 0; i < n; i++) out[i] = dlc_images[offset + i].index;
    return n;
}
static int slot_for(int index) {
    runtime_dlc_init();
    for (int i = 0; i < ACNH_DLC_COUNT; i++) if (dlc_images[i].index == index) return i;
    return -1;
}
int runtime_dlc_has(int index) { return slot_for(index) >= 0; }
unsigned runtime_dlc_mount(const char *name, int index) {
    int slot = slot_for(index); if (slot < 0) return NOT_INSTALLED;
    if (!romfs_path_valid_relative(name) || strlen(name) >= sizeof mounts[0].name || strchr(name, '/') || strchr(name, ':')) return INVALID;
    pthread_mutex_lock(&lock); int free_slot = -1;
    for (unsigned i = 0; i < 16; i++) {
        if (*mounts[i].name && !strcmp(name, mounts[i].name)) { pthread_mutex_unlock(&lock); return 0x402u; }
        if (!*mounts[i].name && free_slot < 0) free_slot = (int)i;
    }
    if (free_slot < 0) { pthread_mutex_unlock(&lock); return INVALID; }
    strcpy(mounts[free_slot].name, name); mounts[free_slot].slot = slot; pthread_mutex_unlock(&lock);
    fprintf(stderr, "[acnh] DLC mount %s: -> %s (index %d)\n", name, dlc_images[slot].title, index);
    return 0;
}
int runtime_dlc_unmount(const char *name) {
    if (!name) return 0;
    pthread_mutex_lock(&lock);
    for (unsigned i = 0; i < 16; i++) if (*mounts[i].name && !strcmp(name, mounts[i].name)) {
        *mounts[i].name = 0; pthread_mutex_unlock(&lock); return 1;
    }
    pthread_mutex_unlock(&lock); return 0;
}
static int path_slot(const char *path) {
    if (!path) return -1;
    const char *colon = strchr(path, ':'); if (!colon) return -1;
    size_t n = (size_t)(colon - path); int slot = -1;
    pthread_mutex_lock(&lock);
    for (unsigned i = 0; i < 16; i++) if (*mounts[i].name && strlen(mounts[i].name) == n && !strncmp(path, mounts[i].name, n)) slot = mounts[i].slot;
    pthread_mutex_unlock(&lock); return slot;
}
int runtime_dlc_owns(const char *path) { return path_slot(path) >= 0; }
static int resolve(const char *path, char relative[4096], int directory) {
    int slot = path_slot(path); if (slot < 0) return -1;
    const char *p = strchr(path, ':') + 1; if (*p != '/') return -1;
    while (*p == '/') p++;
    if (snprintf(relative, 4096, "%s", p) >= 4096) return -1;
    if (directory) { size_t n = strlen(relative); while (n && relative[n - 1] == '/') relative[--n] = 0; }
    return (!*relative && directory) || romfs_path_valid_relative(relative) ? slot : -1;
}
static const DlcFile *find_file(int slot, const char *path) {
    const DlcImage *image = &dlc_images[slot]; size_t lo = 0, hi = image->count;
    while (lo < hi) { size_t mid = lo + (hi - lo) / 2; if (strcmp(image->files[mid].path, path) < 0) lo = mid + 1; else hi = mid; }
    return lo < image->count && !strcmp(image->files[lo].path, path) ? &image->files[lo] : NULL;
}
unsigned runtime_dlc_open(int *fd, uint64_t *base, uint64_t *size, const char *path, int mode) {
    if (mode != 1) return READ_ONLY;
    char relative[4096]; int slot = resolve(path, relative, 0); if (slot < 0) return NOT_FOUND;
    const DlcFile *file = find_file(slot, relative); if (!file) return NOT_FOUND;
    struct stat now;
    need(!fstat(image_fds[slot], &now) && same_file(&now, &image_stats[slot]), "DLC image changed after verification");
    *fd = fcntl(image_fds[slot], F_DUPFD_CLOEXEC, 3); need(*fd >= 0, "DLC duplicate descriptor");
    *base = file->offset; *size = file->size; return 0;
}
unsigned runtime_dlc_entry_type(int *out, const char *path) {
    if (!out) return INVALID;
    char relative[4096]; int slot = resolve(path, relative, 1); if (slot < 0) return NOT_FOUND;
    if (!*relative) { *out = 0; return 0; }
    if (find_file(slot, relative)) { *out = 1; return 0; }
    size_t n = strlen(relative);
    for (size_t i = 0; i < dlc_images[slot].count; i++)
        if (!strncmp(dlc_images[slot].files[i].path, relative, n) && dlc_images[slot].files[i].path[n] == '/') { *out = 0; return 0; }
    return NOT_FOUND;
}
unsigned runtime_dlc_directory(const char *path, int mode, void (*add)(void *, const char *, int, int64_t), void *ctx) {
    if (mode < 1 || mode > 3 || !add) return INVALID;
    int type; unsigned result = runtime_dlc_entry_type(&type, path); if (result || type != 0) return result ? result : NOT_FOUND;
    char relative[4096]; int slot = resolve(path, relative, 1); if (slot < 0) return NOT_FOUND;
    size_t n = strlen(relative); char last[769] = "";
    for (size_t i = 0; i < dlc_images[slot].count; i++) {
        const DlcFile *file = &dlc_images[slot].files[i]; const char *p = file->path;
        if (n) { if (strncmp(p, relative, n) || p[n] != '/') continue; p += n + 1; }
        const char *slash = strchr(p, '/');
        if (!slash) { if (mode & 2) add(ctx, p, 0, (int64_t)file->size); continue; }
        size_t len = (size_t)(slash - p); need(len < sizeof last, "DLC directory name length");
        if (strlen(last) == len && !strncmp(p, last, len)) continue;
        memcpy(last, p, len); last[len] = 0; if (mode & 1) add(ctx, last, 1, 0);
    }
    return 0;
}

/* Exercise the guest-facing fs symbol map against every image slice and its exact bytes. */
void runtime_dlc_probe(void) {
    typedef unsigned (*Open)(uint64_t *, const char *, int);
    typedef unsigned (*Read)(uint64_t *, uint64_t, int64_t, void *, uint64_t);
    typedef unsigned (*Size)(int64_t *, uint64_t);
    typedef void (*Close)(uint64_t);
    typedef int (*Count)(void);
    typedef int (*List)(int *, int, int);
    typedef unsigned (*Mount)(const char *, int, void *, size_t);
    typedef unsigned (*Query)(size_t *, int);
    typedef void (*Unmount)(const char *);
    Open open_file = (Open)runtime_fs_host_override("_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci");
    Read read_file = (Read)runtime_fs_host_override("_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm");
    Size file_size = (Size)runtime_fs_host_override("_ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE");
    Close close_file = (Close)runtime_fs_host_override("_ZN2nn2fs9CloseFileENS0_10FileHandleE");
    Count count = (Count)runtime_override("_ZN2nn3aoc17CountAddOnContentEv");
    List list = (List)runtime_override("_ZN2nn3aoc16ListAddOnContentEPiii");
    Mount mount = (Mount)runtime_fs_host_override("_ZN2nn2fs17MountAddOnContentEPKciPvm");
    Query query = (Query)runtime_fs_host_override("_ZN2nn2fs31QueryMountAddOnContentCacheSizeEPmi");
    Unmount unmount = (Unmount)runtime_fs_host_override("_ZN2nn2fs7UnmountEPKc");
    need(open_file && read_file && file_size && close_file && count && list && mount && query && unmount, "DLC probe fs imports");
    int ids[NSLOTS + 1]; ids[NSLOTS] = -99;
    need(count() == ACNH_DLC_COUNT && list(ids, 0, NSLOTS) == ACNH_DLC_COUNT && ids[NSLOTS] == -99, "DLC enumerate bounds");
    need(list(NULL, 0, 0) == 0 && list(ids, ACNH_DLC_COUNT + 1, 1) == 0, "DLC list empty range");
    size_t checked = 0; uint64_t bytes = 0;
    for (int i = 0; i < ACNH_DLC_COUNT; i++) {
        size_t cache_size = 999;
        need(!query(&cache_size, ids[i]) && cache_size == 0, "DLC mount cache query");
        need(ids[i] == dlc_images[i].index && !mount("dlcprobe", ids[i], NULL, 0), "DLC mount/list index");
        for (size_t j = 0; j < dlc_images[i].count; j++) {
            const DlcFile *f = &dlc_images[i].files[j]; char path[4096]; uint64_t h, n; int64_t size;
            need(snprintf(path, sizeof path, "dlcprobe:/%s", f->path) < (int)sizeof path, "DLC probe path");
            need(!open_file(&h, path, 1) && !file_size(&size, h) && (uint64_t)size == f->size, "DLC open/size");
            uint8_t actual[257], want[257]; uint64_t length = f->size < sizeof actual ? f->size : sizeof actual;
            need(!read_file(&n, h, 0, actual, length) && n == length && pread(image_fds[i], want, (size_t)length, (off_t)f->offset) == (ssize_t)length && !memcmp(actual, want, (size_t)length), "DLC slice bytes");
            need(!read_file(&n, h, (int64_t)f->size, actual, sizeof actual) && n == 0, "DLC EOF clamp");
            close_file(h); need(open_file(&h, path, 2) == READ_ONLY, "DLC reject writes"); checked++; bytes += f->size;
        }
        uint64_t h; need(open_file(&h, "dlcprobe:/../escape", 1) == NOT_FOUND, "DLC reject traversal");
        unmount("dlcprobe"); need(!runtime_dlc_owns("dlcprobe:/"), "DLC unmount");
    }
    need(runtime_dlc_mount("unknown", -1) == NOT_INSTALLED, "DLC reject uninstalled index");
    printf("{\"status\":\"pass\",\"dlc_count\":%d,\"files\":%zu,\"file_bytes\":%llu,\"scope\":\"verified DLC enumeration, mount, file slices, EOF, read-only and traversal; no gameplay\"}\n", ACNH_DLC_COUNT, checked, (unsigned long long)bytes);
}
