/* Breath of the Wild add-on content (botw.md): the DLC's RomFS images, read-only volumes beside the game's (romfs.c), listed
   by nn::aoc and mounted as the game asks (nn::fs::MountAddOnContent("aoc", 1, ...)). The game lists two add-on contents
   (index 1: the expansion pass packs, 7,094 files; index 2: one file) and mounts index 1 only. Each volume is the image
   games/botw/scripts/extract_aoc_romfs.py extracted and indexed, verified against aoc-config.txt in the game data folder
   (game_data.h, scripts/prepare_romfs_backend.py --aoc: SHA-256 of the image
   once, then a proof record as romfs.c keeps one; the index on every start). Built into the Breath of the Wild runtime
   only; fs_host.c and services.c reach it through the GAME_BOTW macros of fs_host.h. */
#ifdef GAME_BOTW
#include "runtime.h"
#include "game_data.h"
#include "game_version.h"
#include "fs_host.h"
#include "romfs_path.h"
#include "totk_ui_lang.h"
#include <CommonCrypto/CommonDigest.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#define NOT_FOUND 0x202u
#define UNSUPPORTED 0x6402u
#define INVALID 0xe402u
#define AOC_MAX 2
#define AOC_NH 1024
#define AOC_HANDLE_BASE 0x400000000ULL   /* its own range: fs_host.c uses 0x200000000 (files) and 0x300000000 (directories), romfs.c 0x100000000 */
typedef struct { uint64_t offset, size; uint32_t name, len; } Entry;
typedef struct {
    int index; uint64_t image_size; const char *image_sha, *index_sha;
    uint8_t *index_bytes; size_t index_size; uint64_t file_count, strings_offset; int image_fd; char mount[32]; int loaded;
} Vol;
static Vol vols[AOC_MAX] = {   /* sizes and hashes: config_load */
    {1, 0, NULL, NULL, NULL, 0, 0, 0, -1, "", 0},
    {2, 0, NULL, NULL, NULL, 0, 0, 0, -1, "", 0},
};
static struct { uint8_t vol; uint64_t entry; } handles[AOC_NH];   /* entry = file index + 1; 0 is free */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static void need(int c, const char *m) { if (!c) runtime_fail(m); }
static pthread_once_t config_once = PTHREAD_ONCE_INIT;
static unsigned present_mask;   /* bit index - 1: the volume was extracted and indexed */
static void config_load(void) {
    present_mask = (unsigned)runtime_game_number("aoc-config.txt", "AOC_PRESENT_MASK");
    for (int i = 0; i < AOC_MAX; i++) {
        if (!((present_mask >> i) & 1)) continue;
        char key[32]; Vol *v = &vols[i];
        snprintf(key, sizeof key, "AOC%d_IMAGE_SIZE", v->index); v->image_size = runtime_game_number("aoc-config.txt", key);
        snprintf(key, sizeof key, "AOC%d_IMAGE_SHA256", v->index); v->image_sha = runtime_game_sha256("aoc-config.txt", key);
        snprintf(key, sizeof key, "AOC%d_INDEX_SHA256", v->index); v->index_sha = runtime_game_sha256("aoc-config.txt", key);
    }
}
static int present(const Vol *v) { pthread_once(&config_once, config_load); return (present_mask >> (v->index - 1)) & 1; }
static void vol_dir(const Vol *v, char *out, size_t cap) {
    const char *dir = getenv("TOTK_AOC");   /* the folder holding <index>/; default beside the game's RomFS */
    need(snprintf(out, cap, "%s/%d", dir && *dir ? dir : "extracted/" TOTK_GAME_DIR "/aoc", v->index) < (int)cap, "add-on content path");
}
static Entry entry(const Vol *v, uint64_t i) { Entry e; memcpy(&e, v->index_bytes + 24 + i * 24, 24); return e; }
static const char *entry_name(const Vol *v, Entry e) { return (const char *)v->index_bytes + v->strings_offset + e.name; }
static void sha_fd(int fd, const char *expected, uint64_t total, const char *what) {
    CC_SHA256_CTX c; CC_SHA256_Init(&c); uint8_t *buf = malloc(8 << 20); need(buf != NULL, "hash buffer"); uint64_t done = 0;
    for (;;) { ssize_t n = pread(fd, buf, 8 << 20, (off_t)done); if (n < 0) runtime_fail("add-on content read"); if (!n) break; CC_SHA256_Update(&c, buf, (CC_LONG)n); done += (uint64_t)n;
        if (total) runtime_boot_progress(what, done, total); }
    free(buf);
    uint8_t d[32]; CC_SHA256_Final(d, &c); char hex[65]; for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", d[i]);
    if (total && done != total) runtime_fail("add-on content image size changed while hashing");
    if (strcmp(hex, expected)) { fprintf(stderr, "[botw] add-on content %s: SHA-256 %s, expected %s\n", what, hex, expected); runtime_fail("add-on content image does not match the extraction record"); }
}
typedef struct { uint64_t size, dev, ino; int64_t mtime_s, mtime_ns, ctime_s, ctime_ns; char sha[65]; } Proof;
static Proof proof_for(const struct stat *s, const char *sha) {
    Proof p; memset(&p, 0, sizeof p); p.size = (uint64_t)s->st_size; p.dev = (uint64_t)s->st_dev; p.ino = (uint64_t)s->st_ino;
    p.mtime_s = s->st_mtimespec.tv_sec; p.mtime_ns = s->st_mtimespec.tv_nsec; p.ctime_s = s->st_ctimespec.tv_sec; p.ctime_ns = s->st_ctimespec.tv_nsec;
    snprintf(p.sha, sizeof p.sha, "%s", sha); return p;
}
/* The image's full SHA-256 once; afterwards a record of the file's identity (size, inode, times, expected hash) in the
   volume's folder (TOTK_ROMFS_PROOF_DIR elsewhere, as romfs.c); TOTK_ROMFS_VERIFY=full hashes again. */
static void verify_image(Vol *v, const char *dir) {
    const char *pdir = getenv("TOTK_ROMFS_PROOF_DIR"); if (pdir && *pdir) dir = pdir;
    struct stat st; need(fstat(v->image_fd, &st) == 0 && S_ISREG(st.st_mode) && (uint64_t)st.st_size == v->image_size, "add-on content image size");
    Proof want = proof_for(&st, v->image_sha), saved;
    char path[4096], tmp[4096]; need(snprintf(path, sizeof path, "%s/.aoc%d-verified-v1", dir, v->index) < (int)sizeof path, "add-on content proof path");
    int force = getenv("TOTK_ROMFS_VERIFY") && !strcmp(getenv("TOTK_ROMFS_VERIFY"), "full");
    int pf = open(path, O_RDONLY | O_NOFOLLOW);
    if (pf >= 0) { int ok = read(pf, &saved, sizeof saved) == (ssize_t)sizeof saved; close(pf);
        if (ok && !force && !memcmp(&saved, &want, sizeof want)) { fprintf(stderr, "[botw] add-on content %d unchanged; reused the SHA-256 verification record\n", v->index); return; } }
    char what[64]; snprintf(what, sizeof what, "%s %d", TOTK_UI("校验追加内容", "Verifying add-on content"), v->index);
    fprintf(stderr, "[botw] verifying add-on content %d SHA-256 (%llu bytes)%s\n", v->index, (unsigned long long)v->image_size, force ? " [forced]" : "");
    sha_fd(v->image_fd, v->image_sha, v->image_size, what);
    need(fstat(v->image_fd, &st) == 0 && (uint64_t)st.st_size == v->image_size, "add-on content image stat");
    Proof after = proof_for(&st, v->image_sha); need(!memcmp(&after, &want, sizeof want), "add-on content changed during verification");
    if (snprintf(tmp, sizeof tmp, "%s/.aoc%d-verified-v1.%ld.tmp", dir, v->index, (long)getpid()) >= (int)sizeof tmp) return;
    int out = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (out >= 0) { int ok = write(out, &want, sizeof want) == (ssize_t)sizeof want && fsync(out) == 0; close(out); if (!ok || rename(tmp, path) != 0) unlink(tmp); }
}
static void vol_load(Vol *v) {
    if (v->loaded) return;
    need(present(v), "add-on content not extracted");
    char dir[4096], path[4200]; vol_dir(v, dir, sizeof dir);
    need(snprintf(path, sizeof path, "%s/runtime-index.bin", dir) < (int)sizeof path, "add-on content index path");
    int fd = open(path, O_RDONLY); need(fd >= 0, "add-on content index missing (games/botw/scripts/extract_aoc_romfs.py, then make)");
    struct stat st; need(fstat(fd, &st) == 0 && st.st_size >= 24 && st.st_size < 64 * 1024 * 1024, "add-on content index size");
    sha_fd(fd, v->index_sha, 0, "index");
    v->index_size = (size_t)st.st_size; v->index_bytes = mmap(NULL, v->index_size, PROT_READ, MAP_PRIVATE, fd, 0); close(fd); need(v->index_bytes != MAP_FAILED, "map add-on content index");
    need(!memcmp(v->index_bytes, "TOTKRFS1", 8), "add-on content index format"); memcpy(&v->file_count, v->index_bytes + 8, 8); memcpy(&v->strings_offset, v->index_bytes + 16, 8);
    need(v->file_count < 1000000 && v->strings_offset == 24 + v->file_count * 24 && v->strings_offset < v->index_size, "add-on content index layout");
    const char *previous = NULL;
    for (uint64_t i = 0; i < v->file_count; i++) { Entry e = entry(v, i);
        need(e.offset <= v->image_size && e.size <= v->image_size - e.offset, "add-on content file bounds");
        need(e.name < v->index_size - v->strings_offset && e.len > 1 && e.len <= v->index_size - v->strings_offset - e.name, "add-on content name bounds");
        const char *n = entry_name(v, e); need(n[e.len - 1] == 0 && strlen(n) == e.len - 1 && romfs_path_valid_relative(n), "add-on content file name");
        need(!previous || strcmp(previous, n) < 0, "add-on content index order"); previous = n; }
    need(snprintf(path, sizeof path, "%s/romfs.bin", dir) < (int)sizeof path, "add-on content image path");
    v->image_fd = open(path, O_RDONLY); need(v->image_fd >= 0, "add-on content image missing");
    verify_image(v, dir);
    v->loaded = 1;
    fprintf(stderr, "[botw] add-on content %d ready: %llu files, %llu bytes\n", v->index, (unsigned long long)v->file_count, (unsigned long long)v->image_size);
}
/* nn::aoc */
void runtime_aoc_announce(void) {
    int n = 0; for (int i = 0; i < AOC_MAX; i++) n += present(&vols[i]);
    fprintf(stderr, "[botw] add-on content: %d of %d packs extracted (TOTK_AOC: the folder holding <index>/romfs.bin)\n", n, AOC_MAX);
}
int runtime_aoc_count(void) { int n = 0; for (int i = 0; i < AOC_MAX; i++) n += present(&vols[i]); return n; }
int runtime_aoc_list(int *out, int offset, int count) {
    int n = 0, seen = 0;
    for (int i = 0; i < AOC_MAX && n < count; i++) { if (!present(&vols[i])) continue; if (seen++ < offset) continue; if (out) out[n] = vols[i].index; n++; }
    return n;
}
/* nn::fs */
unsigned runtime_aoc_cache_size(uint64_t *out, int index) { if (!out || index < 1 || index > AOC_MAX) return INVALID; *out = 0; return 0; }
unsigned runtime_aoc_mount(const char *name, int index, void *cache, uint64_t size) {
    (void)cache; (void)size;
    if (!name || !*name || strlen(name) >= sizeof vols[0].mount || strchr(name, '/') || strchr(name, ':') || index < 1 || index > AOC_MAX) return INVALID;
    Vol *v = &vols[index - 1]; if (!present(v)) return NOT_FOUND;
    vol_load(v);
    pthread_mutex_lock(&lock); snprintf(v->mount, sizeof v->mount, "%s", name); pthread_mutex_unlock(&lock);
    fprintf(stderr, "[botw] fs mount %s: -> add-on content %d\n", name, index);
    return 0;
}
static Vol *vol_of_path(const char *path, const char **rel) {
    if (!path) return NULL;
    for (int i = 0; i < AOC_MAX; i++) { Vol *v = &vols[i]; if (!v->loaded || !*v->mount) continue;
        size_t n = strlen(v->mount); if (strncmp(path, v->mount, n) || path[n] != ':') continue;
        if (rel) *rel = romfs_path_relative(v->mount, path); return v; }
    return NULL;
}
int runtime_aoc_owns(const char *path) { return vol_of_path(path, NULL) != NULL; }
int runtime_aoc_owns_name(const char *name) {
    if (!name) return 0;
    for (int i = 0; i < AOC_MAX; i++) if (vols[i].loaded && *vols[i].mount && !strcmp(vols[i].mount, name)) return 1;
    return 0;
}
void runtime_aoc_unmount(const char *name) {
    for (int i = 0; i < AOC_MAX; i++) if (vols[i].loaded && !strcmp(vols[i].mount, name)) { pthread_mutex_lock(&lock); vols[i].mount[0] = 0; pthread_mutex_unlock(&lock); fprintf(stderr, "[botw] fs unmount %s (add-on content %d)\n", name, vols[i].index); }
}
static uint64_t lower(const Vol *v, const char *name) { uint64_t lo = 0, hi = v->file_count; while (lo < hi) { uint64_t m = lo + (hi - lo) / 2; if (strcmp(entry_name(v, entry(v, m)), name) < 0) lo = m + 1; else hi = m; } return lo; }
unsigned runtime_aoc_open(uint64_t *out, const char *path, int mode) {
    const char *p = NULL; Vol *v = vol_of_path(path, &p);
    if (!out || mode != 1) return UNSUPPORTED;
    if (!v || !p) return NOT_FOUND;
    uint64_t i = lower(v, p); if (i >= v->file_count || strcmp(p, entry_name(v, entry(v, i)))) return NOT_FOUND;
    pthread_mutex_lock(&lock);
    for (unsigned j = 1; j < AOC_NH; j++) if (!handles[j].entry) { handles[j].vol = (uint8_t)(v - vols); handles[j].entry = i + 1; *out = AOC_HANDLE_BASE + j; pthread_mutex_unlock(&lock); return 0; }
    pthread_mutex_unlock(&lock); runtime_fail("add-on content file handle table full"); return INVALID;
}
int runtime_aoc_handle(uint64_t h) { return h > AOC_HANDLE_BASE && h < AOC_HANDLE_BASE + AOC_NH; }
static int handle_entry(uint64_t h, Vol **v, Entry *e) {
    if (!runtime_aoc_handle(h)) return 0;
    pthread_mutex_lock(&lock); uint64_t i = handles[h - AOC_HANDLE_BASE].entry; uint8_t vi = handles[h - AOC_HANDLE_BASE].vol; pthread_mutex_unlock(&lock);
    if (!i) return 0; *v = &vols[vi]; *e = entry(*v, i - 1); return 1;
}
unsigned runtime_aoc_read(uint64_t *out, uint64_t h, int64_t offset, void *buffer, uint64_t size) {
    Vol *v; Entry e; if (!out || offset < 0 || !handle_entry(h, &v, &e)) return INVALID;
    if ((uint64_t)offset > e.size || (!buffer && size)) return INVALID;
    uint64_t n = size; if (n > e.size - (uint64_t)offset) n = e.size - (uint64_t)offset; uint64_t done = 0;
    dm_guest_write_prepare(buffer, (size_t)n);
    while (done < n) { size_t chunk = (size_t)(n - done); if (chunk > 8 << 20) chunk = 8 << 20;
        ssize_t r = pread(v->image_fd, (uint8_t *)buffer + done, chunk, (off_t)(e.offset + (uint64_t)offset + done)); if (r <= 0) runtime_fail("add-on content read failure"); done += (uint64_t)r; }
    *out = done; return 0;
}
unsigned runtime_aoc_size(int64_t *out, uint64_t h) { Vol *v; Entry e; if (!out || !handle_entry(h, &v, &e)) return INVALID; *out = (int64_t)e.size; return 0; }
void runtime_aoc_close(uint64_t h) { Vol *v; Entry e; if (!handle_entry(h, &v, &e)) runtime_fail("close invalid add-on content handle"); pthread_mutex_lock(&lock); handles[h - AOC_HANDLE_BASE].entry = 0; pthread_mutex_unlock(&lock); }
unsigned runtime_aoc_entry_type(int *out, const char *path) {
    const char *p = NULL; Vol *v = vol_of_path(path, &p); if (!out) return INVALID; if (!v || !p) return NOT_FOUND;
    uint64_t i = lower(v, p); if (i < v->file_count && !strcmp(entry_name(v, entry(v, i)), p)) { *out = 1; return 0; }
    char dir[4096]; if (snprintf(dir, sizeof dir, "%s/", p) >= (int)sizeof dir) return INVALID;
    i = lower(v, dir); if (i < v->file_count && !strncmp(entry_name(v, entry(v, i)), dir, strlen(dir))) { *out = 0; return 0; }
    return NOT_FOUND;
}
#endif
