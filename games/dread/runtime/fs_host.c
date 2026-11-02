/* nn::fs host implementation for the functions main imports. RomFS paths and handles go to the
   verified read-only backend (romfs.c); save, temporary and cache storage are host directories under
   the profile directory. Application save data is created on first start, as the system launcher does
   for titles that declare save data. Writes reach the host file immediately; Commit flushes them.
   Because of that, quitting in the middle of a save (the game writes several files, then commits) would leave a slot half
   written: runtime_fs_save_busy tells the window close to wait until the save is done (runtime/frontend.m). */
#include "autoload.h"
#include "runtime.h"
#include "fs_host.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <unistd.h>

#define R_PATH_NOT_FOUND 0x202u
#define R_ALREADY_EXISTS 0x402u
#define R_NOT_EMPTY 0x1002u
#define R_TARGET_NOT_FOUND 0x7d402u
#define R_INVALID_ARGUMENT 0x2ee202u
#define R_INVALID_PATH 0x2ee602u
#define R_OPEN_MODE 0x307402u
#define R_APPEND_REQUIRED 0x2f5e02u
#include "game_version.h"
#define TOTK_APPLICATION_ID GAME_TITLE_ID
enum { M_FREE, M_SAVE, M_TEMP, M_CACHE };
enum { OPEN_READ = 1, OPEN_WRITE = 2, OPEN_APPEND = 4 };
#define FILE_BASE 0x200000000ULL
#define DIR_BASE 0x300000000ULL
#define NFILES 1024
#define NDIRS 256

typedef struct { int kind; char name[32]; char root[1024]; } Mount;
typedef struct { int fd, mode, save; } File;   /* save: on a save-data mount (counts for runtime_fs_save_busy) */
/* A RomFS directory is a snapshot of its children taken at open (rom != NULL; dir stays NULL). */
struct RomEntry; typedef struct { DIR *dir; char path[1024]; int mode; struct RomEntry *rom; int64_t nrom, pos; } Dir;
static Mount mounts[16];
static File files[NFILES];
static Dir dirs[NDIRS];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static char profile[1024], save_root[1024];
static pthread_once_t once = PTHREAD_ONCE_INIT;

static void need(int c, const char *m) { if (!c) runtime_fail(m); }

/* Save in progress: a save-data file open for writing, or save data changed in the last 3 s and not committed yet (TOTK
   writes a slot's files back to back and then commits; between the files nothing is open for a moment). */
static _Atomic int save_writers, save_dirty;
static _Atomic uint64_t save_changed_ns;
static void save_touched(void) { atomic_store(&save_changed_ns, runtime_now_ns()); atomic_store(&save_dirty, 1); }
int runtime_fs_save_busy(void) {
    if (atomic_load(&save_writers) > 0) return 1;
    return atomic_load(&save_dirty) && runtime_now_ns() - atomic_load(&save_changed_ns) < 3000000000ull;
}
static void make_dir(const char *p) { if (mkdir(p, 0700) && errno != EEXIST) runtime_fail("create profile storage directory"); }
static int remove_tree(const char *path) {
    DIR *d = opendir(path); if (!d) return errno == ENOENT ? 0 : -1;
    struct dirent *e; int ok = 0; char child[2048];
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(child, sizeof child, "%s/%s", path, e->d_name);
        ok |= e->d_type == DT_DIR ? remove_tree(child) : unlink(child);
    }
    closedir(d);
    return ok | rmdir(path);
}
static void init(void) {
    const char *dir = getenv("TOTK_PROFILE_DIR"); if (!dir) dir = "user-data";
    need(snprintf(profile, sizeof profile, "%s", dir) < (int)sizeof profile, "profile path");
    uint64_t uid[2]; runtime_account_uid(uid);
    char p[1100]; make_dir(profile);
    snprintf(p, sizeof p, "%s/save", profile); make_dir(p);
    need(snprintf(save_root, sizeof save_root, "%s/save/%016llx%016llx", profile, (unsigned long long)uid[1], (unsigned long long)uid[0]) < (int)sizeof save_root, "save path");
    make_dir(save_root);                                   /* launcher-style EnsureSaveData */
    snprintf(p, sizeof p, "%s/temp", profile); remove_tree(p); make_dir(p);
    snprintf(p, sizeof p, "%s/cache", profile); make_dir(p);
    fprintf(stderr, "[totk] host save storage: %s\n", save_root);
}
static void ready(void) { pthread_once(&once, init); }

/* ---------- mounts and paths ---------- */
static unsigned mount_add(const char *name, int kind, const char *root) {
    if (!name || !*name || strlen(name) >= 32 || strchr(name, ':') || strchr(name, '/')) return R_INVALID_ARGUMENT;
    pthread_mutex_lock(&lock);
    int slot = -1;
    for (int i = 0; i < 16; i++) { if (mounts[i].kind && !strcmp(mounts[i].name, name)) { pthread_mutex_unlock(&lock); return R_ALREADY_EXISTS; } if (!mounts[i].kind && slot < 0) slot = i; }
    if (slot < 0) runtime_fail("fs mount table full");
    mounts[slot].kind = kind; strcpy(mounts[slot].name, name); snprintf(mounts[slot].root, sizeof mounts[slot].root, "%s", root);
    pthread_mutex_unlock(&lock);
    fprintf(stderr, "[totk] fs mount %s: -> %s\n", name, root);
    return 0;
}
static int component_ok(const char *c, size_t n) { return n && !(n == 1 && c[0] == '.') && !(n == 2 && c[0] == '.' && c[1] == '.'); }
/* "name:/a/b" -> host path; 0 on success; *kind = the mount's kind (M_SAVE ...) when kind is given */
static unsigned host_path_kind(const char *path, char *out, size_t cap, int *kind) {
    if (!path) return R_INVALID_PATH;
    const char *colon = strchr(path, ':');
    if (!colon || colon[1] != '/') return R_INVALID_PATH;
    size_t n = (size_t)(colon - path);
    pthread_mutex_lock(&lock);
    const Mount *m = NULL;
    for (int i = 0; i < 16; i++) if (mounts[i].kind && strlen(mounts[i].name) == n && !strncmp(mounts[i].name, path, n)) m = &mounts[i];
    char root[1024]; if (m) strcpy(root, m->root);
    int k = m ? m->kind : M_FREE;
    pthread_mutex_unlock(&lock);
    if (!m) return R_PATH_NOT_FOUND;
    if (kind) *kind = k;
    /* Repeated separators collapse, as Horizon's fs normalizes them (Metroid Dread opens savedata://userdata/); "." and ".."
       stay invalid here, since this becomes a host path. */
    size_t L = (size_t)snprintf(out, cap, "%s", root); if (L >= cap) return R_INVALID_PATH;
    for (const char *q = colon + 1; *q;) {
        while (*q == '/') q++;
        if (!*q) break;
        const char *e = strchr(q, '/'); size_t len = e ? (size_t)(e - q) : strlen(q);
        if (!component_ok(q, len) || memchr(q, '\\', len)) return R_INVALID_PATH;
        if (L + 1 + len >= cap) return R_INVALID_PATH;
        out[L++] = '/'; memcpy(out + L, q, len); L += len; out[L] = 0;
        q += len;
    }
    return 0;
}
static unsigned host_path(const char *path, char *out, size_t cap) { return host_path_kind(path, out, cap, NULL); }
/* A change to save data through a path (create, delete, rename, directories): marks the save as in progress. */
static unsigned save_change_path(const char *path, char *out, size_t cap) {
    int kind = M_FREE; unsigned r = host_path_kind(path, out, cap, &kind);
    if (!r && kind == M_SAVE) save_touched();
    return r;
}
static unsigned errno_result(void) { return errno == ENOENT || errno == ENOTDIR ? R_PATH_NOT_FOUND : errno == EEXIST ? R_ALREADY_EXISTS : errno == ENOTEMPTY ? R_NOT_EMPTY : R_INVALID_ARGUMENT; }

/* ---------- storage mounts ---------- */
static unsigned mount_save(const char *name, const uint64_t *uid) {
    ready(); uint64_t me[2]; runtime_account_uid(me);
    if (!uid || uid[0] != me[0] || uid[1] != me[1]) return R_TARGET_NOT_FOUND;
    return mount_add(name, M_SAVE, save_root);
}
static unsigned char save_exists(uint64_t application, const uint64_t *uid) {
    ready(); uint64_t me[2]; runtime_account_uid(me);
    return application == TOTK_APPLICATION_ID && uid && uid[0] == me[0] && uid[1] == me[1];
}
/* Other titles' save data (e.g. data-link bonuses) is not present on this host. */
static unsigned mount_save_ro(const char *name, uint64_t application, const uint64_t *uid) {
    if (!save_exists(application, uid)) return R_TARGET_NOT_FOUND;
    return mount_add(name, M_SAVE, save_root);
}
static unsigned mount_temp(const char *name) { ready(); char p[1100]; snprintf(p, sizeof p, "%s/temp", profile); return mount_add(name, M_TEMP, p); }
static void cache_dir(int index, char *out, size_t cap) { snprintf(out, cap, "%s/cache/%d", profile, index); }
static unsigned create_cache(int index, int64_t size, int64_t journal) {
    ready(); if (index < 0 || size < 0 || journal < 0) return R_INVALID_ARGUMENT;
    char p[1100]; cache_dir(index, p, sizeof p);
    if (mkdir(p, 0700)) return errno == EEXIST ? R_ALREADY_EXISTS : errno_result();
    return 0;
}
static unsigned delete_cache(int index) { ready(); char p[1100]; cache_dir(index, p, sizeof p); return remove_tree(p) ? R_TARGET_NOT_FOUND : 0; }
static unsigned mount_cache(const char *name, int index) {
    ready(); char p[1100]; cache_dir(index, p, sizeof p); struct stat st;
    if (stat(p, &st) || !S_ISDIR(st.st_mode)) return R_TARGET_NOT_FOUND;
    return mount_add(name, M_CACHE, p);
}
typedef struct { int32_t index; uint8_t reserved[0x1c]; } CacheStorageInfo;
typedef struct { int list[64]; int count, next; } CacheList;
static unsigned open_cache_list(uint64_t *out) {
    ready(); if (!out) return R_INVALID_ARGUMENT;
    CacheList *l = calloc(1, sizeof *l); need(l != NULL, "cache list allocation");
    char p[1100]; snprintf(p, sizeof p, "%s/cache", profile); DIR *d = opendir(p); struct dirent *e;
    while (d && (e = readdir(d)) && l->count < 64) { char *end; long v = strtol(e->d_name, &end, 10); if (*e->d_name && !*end && v >= 0) l->list[l->count++] = (int)v; }
    if (d) closedir(d);
    *out = (uint64_t)(uintptr_t)l; return 0;
}
static unsigned read_cache_list(int *count, CacheStorageInfo *out, uint64_t h, int max) {
    CacheList *l = (CacheList *)(uintptr_t)h; if (!l || !count || max < 0 || (!out && max)) return R_INVALID_ARGUMENT;
    int n = 0; while (n < max && l->next < l->count) { memset(&out[n], 0, sizeof *out); out[n++].index = l->list[l->next++]; }
    *count = n; return 0;
}
static void close_cache_list(uint64_t h) { free((void *)(uintptr_t)h); }
static void unmount(const char *name) {
    pthread_mutex_lock(&lock);
    for (int i = 0; i < 16; i++) if (mounts[i].kind && !strcmp(mounts[i].name, name)) { mounts[i].kind = M_FREE; pthread_mutex_unlock(&lock); return; }
    pthread_mutex_unlock(&lock);
    if (!runtime_romfs_unmount(name)) { fprintf(stderr, "[totk] Unmount(%s): not mounted\n", name ? name : "(null)"); runtime_fail("unmount of unknown fs mount"); }
}
/* EnsureSaveData(uid) (Bayonetta 3): the profile's save folder is made when storage starts; only the local account owns one. */
static unsigned ensure_save(const uint64_t *uid) {
    ready(); uint64_t me[2]; runtime_account_uid(me);
    if (!uid || uid[0] != me[0] || uid[1] != me[1]) runtime_fail("EnsureSaveData for an unknown account");
    return 0;
}
static unsigned commit(const char *name) {
    pthread_mutex_lock(&lock); int found = 0, save = 0;
    for (int i = 0; i < 16; i++) if (mounts[i].kind && !strcmp(mounts[i].name, name)) { found = 1; save = mounts[i].kind == M_SAVE; }
    pthread_mutex_unlock(&lock);
    if (!found) return R_PATH_NOT_FOUND;
    for (int i = 0; i < NFILES; i++) { pthread_mutex_lock(&lock); int fd = files[i].fd; pthread_mutex_unlock(&lock); if (fd > 0) fcntl(fd, F_FULLFSYNC); }
    if (save && !(getenv("TOTK_PROBE_NEGATIVE") && atoi(getenv("TOTK_PROBE_NEGATIVE")) == 23)) atomic_store(&save_dirty, 0);   /* 23: probe-save's negative control */
    return 0;
}

/* ---------- files ---------- */
static File *file_of(uint64_t h, int *fd, int *mode) {
    if (h < FILE_BASE || h >= FILE_BASE + NFILES) return NULL;
    pthread_mutex_lock(&lock); File *f = &files[h - FILE_BASE]; int v = f->fd; if (fd) *fd = f->fd; if (mode) *mode = f->mode; pthread_mutex_unlock(&lock);
    return v > 0 ? f : NULL;
}
void runtime_fs_log(const char *what, const char *path);
static unsigned open_file(uint64_t *out, const char *path, int mode) {
    if (path && !runtime_romfs_owns(path)) runtime_fs_log(mode & 2 ? "write" : "read", path);
    if (runtime_romfs_owns(path)) return runtime_romfs_open(out, path, mode);
    if (!out || !(mode & (OPEN_READ | OPEN_WRITE)) || (mode & ~7)) return R_INVALID_ARGUMENT;
    char p[1100]; int kind = M_FREE; unsigned r = host_path_kind(path, p, sizeof p, &kind); if (r) return r;
    struct stat st; if (stat(p, &st)) return R_PATH_NOT_FOUND; if (!S_ISREG(st.st_mode)) return R_PATH_NOT_FOUND;
    int save_writer = kind == M_SAVE && (mode & OPEN_WRITE);
    int fd = open(p, (mode & OPEN_WRITE ? (mode & OPEN_READ ? O_RDWR : O_WRONLY) : O_RDONLY) | O_NOFOLLOW);
    if (fd < 0) return errno_result();
    runtime_autoload_save_file_opened(path);   /* TOTK_AUTOLOAD: a slot's caption.sav starts the presses, its progress.sav ends them */
    pthread_mutex_lock(&lock);
    for (int i = 0; i < NFILES; i++) if (files[i].fd <= 0) {
        files[i] = (File){fd, mode, save_writer}; *out = FILE_BASE + (uint64_t)i; pthread_mutex_unlock(&lock);
        if (save_writer) { atomic_fetch_add(&save_writers, 1); save_touched(); }
        return 0;
    }
    pthread_mutex_unlock(&lock); runtime_fail("host file handle table full");
}
static unsigned read_file(uint64_t *out, uint64_t h, int64_t offset, void *buffer, uint64_t size) {
    if (runtime_romfs_handle(h)) return runtime_romfs_read(out, h, offset, buffer, size);
    int fd, mode; if (!out || offset < 0 || (!buffer && size) || !file_of(h, &fd, &mode)) return R_INVALID_ARGUMENT;
    if (!(mode & OPEN_READ)) return R_OPEN_MODE;
    dm_guest_write_prepare(buffer, (size_t)size);
    uint64_t done = 0;
    while (done < size) { ssize_t n = pread(fd, (uint8_t *)buffer + done, (size_t)(size - done), offset + (off_t)done); if (n < 0) runtime_fail("host save read failure"); if (!n) break; done += (uint64_t)n; }
    *out = done; return 0;
}
static unsigned read_options(uint64_t *out, uint64_t h, int64_t off, void *buf, uint64_t size, const void *options) { (void)options; return read_file(out, h, off, buf, size); }
static unsigned read_exact(uint64_t h, int64_t off, void *buf, uint64_t size) { uint64_t n = 0; unsigned r = read_file(&n, h, off, buf, size); return r ? r : n == size ? 0 : R_INVALID_ARGUMENT; }
static unsigned write_file(uint64_t h, int64_t offset, const void *buffer, uint64_t size, const int32_t *option) {
    int fd, mode; File *f = offset < 0 || (!buffer && size) ? NULL : file_of(h, &fd, &mode); if (!f) return R_INVALID_ARGUMENT;
    if (!(mode & OPEN_WRITE)) return R_OPEN_MODE;
    struct stat st; if (fstat(fd, &st)) runtime_fail("host save stat");
    if ((uint64_t)offset + size > (uint64_t)st.st_size && !(mode & OPEN_APPEND)) return R_APPEND_REQUIRED;
    uint64_t done = 0;
    if (f->save) {
        save_touched();
        static int delay_ms = -1; if (delay_ms < 0) { const char *e = getenv("TOTK_SAVE_WRITE_DELAY_MS"); delay_ms = e ? atoi(e) : 0; }
        if (delay_ms > 0) usleep((useconds_t)delay_ms * 1000);   /* tests only: slow saves down so a window close lands in the middle of one */
    }
    while (done < size) { ssize_t n = pwrite(fd, (const uint8_t *)buffer + done, (size_t)(size - done), offset + (off_t)done); if (n <= 0) runtime_fail("host save write failure"); done += (uint64_t)n; }
    if (option && (*option & 1)) fcntl(fd, F_FULLFSYNC);
    return 0;
}
static unsigned file_size(int64_t *out, uint64_t h) {
    if (runtime_romfs_handle(h)) return runtime_romfs_size(out, h);
    int fd; struct stat st; if (!out || !file_of(h, &fd, NULL) || fstat(fd, &st)) return R_INVALID_ARGUMENT;
    *out = st.st_size; return 0;
}
static unsigned set_file_size(uint64_t h, int64_t size) {
    int fd, mode; if (size < 0 || !file_of(h, &fd, &mode)) return R_INVALID_ARGUMENT;
    if (!(mode & OPEN_WRITE)) return R_OPEN_MODE;
    if (files[h - FILE_BASE].save) save_touched();
    return ftruncate(fd, size) ? errno_result() : 0;
}
static unsigned flush_file(uint64_t h) { int fd; if (!file_of(h, &fd, NULL)) return R_INVALID_ARGUMENT; fcntl(fd, F_FULLFSYNC); return 0; }
static void close_file(uint64_t h) {
    if (runtime_romfs_handle(h)) { runtime_romfs_close(h); return; }
    if (!file_of(h, NULL, NULL)) runtime_fail("close invalid file handle");
    pthread_mutex_lock(&lock); int fd = files[h - FILE_BASE].fd, save = files[h - FILE_BASE].save; files[h - FILE_BASE].fd = 0; pthread_mutex_unlock(&lock);
    close(fd);
    if (save) { save_touched(); atomic_fetch_sub(&save_writers, 1); }
}
static unsigned create_file(const char *path, int64_t size) {
    char p[1100]; unsigned r = save_change_path(path, p, sizeof p); if (r) return r; if (size < 0) return R_INVALID_ARGUMENT;
    int fd = open(p, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600); if (fd < 0) return errno_result();
    int bad = ftruncate(fd, size); close(fd); return bad ? errno_result() : 0;
}
static unsigned delete_file(const char *path) { char p[1100]; unsigned r = save_change_path(path, p, sizeof p); if (r) return r; return unlink(p) ? errno_result() : 0; }
static unsigned rename_file(const char *from, const char *to) {
    char a[1100], b[1100]; unsigned r = save_change_path(from, a, sizeof a); if (!r) r = host_path(to, b, sizeof b); if (r) return r;
    struct stat st; if (!stat(b, &st)) return R_ALREADY_EXISTS;
    return rename(a, b) ? errno_result() : 0;
}
static unsigned entry_type(int *out, const char *path) {
    if (runtime_romfs_owns(path)) return runtime_romfs_entry_type(out, path);
    char p[1100]; unsigned r = host_path(path, p, sizeof p); if (r) return r; struct stat st;
    if (!out) return R_INVALID_ARGUMENT; if (stat(p, &st)) return R_PATH_NOT_FOUND;
    *out = S_ISDIR(st.st_mode) ? 0 : 1; return 0;
}

/* ---------- directories ---------- */
typedef struct { char name[769]; uint8_t pad0[3]; int8_t type; uint8_t pad1[3]; int64_t size; } DirectoryEntry;
_Static_assert(sizeof(DirectoryEntry) == 0x310, "nn::fs::DirectoryEntry");
static unsigned create_directory(const char *path) { char p[1100]; unsigned r = save_change_path(path, p, sizeof p); if (r) return r; return mkdir(p, 0700) ? errno_result() : 0; }
static unsigned delete_directory(const char *path) { char p[1100]; unsigned r = save_change_path(path, p, sizeof p); if (r) return r; return rmdir(p) ? errno_result() : 0; }   /* empty folders only, as Horizon */
static unsigned delete_recursive(const char *path) { char p[1100]; unsigned r = save_change_path(path, p, sizeof p); if (r) return r; struct stat st; if (stat(p, &st)) return R_PATH_NOT_FOUND; return remove_tree(p) ? R_INVALID_ARGUMENT : 0; }
struct RomEntry { char name[769]; int is_dir; int64_t size; };
typedef struct { struct RomEntry *list; int64_t n, cap; } RomList;
static void rom_add(void *ctx, const char *name, int is_dir, int64_t size) {
    RomList *l = ctx;
    if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 64; l->list = realloc(l->list, (size_t)l->cap * sizeof *l->list); need(l->list != NULL, "RomFS directory listing"); }
    struct RomEntry *e = &l->list[l->n++]; snprintf(e->name, sizeof e->name, "%s", name); e->is_dir = is_dir; e->size = size;
}
static unsigned open_rom_directory(uint64_t *out, const char *path, int mode) {
    if (!out || mode < 1 || mode > 3) return R_INVALID_ARGUMENT;
    RomList l = {0}; unsigned r = runtime_romfs_list(path, mode, rom_add, &l);
    if (r) { fprintf(stderr, "[totk] RomFS directory %s: result %#x\n", path, r); free(l.list); return r; }
    fprintf(stderr, "[totk] RomFS directory %s: %lld entries\n", path, (long long)l.n);
    pthread_mutex_lock(&lock);
    for (int i = 0; i < NDIRS; i++) if (!dirs[i].dir && !dirs[i].rom) {
        dirs[i] = (Dir){.mode = mode, .rom = l.list ? l.list : calloc(1, sizeof *l.list), .nrom = l.n};
        snprintf(dirs[i].path, sizeof dirs[i].path, "%s", path); *out = DIR_BASE + (uint64_t)i; pthread_mutex_unlock(&lock); return 0;
    }
    pthread_mutex_unlock(&lock); runtime_fail("host directory handle table full");
}
static unsigned open_directory(uint64_t *out, const char *path, int mode) {
    if (getenv("TOTK_FS_LOG")) fprintf(stderr, "[totk-fs] OpenDirectory %s mode %d\n", path ? path : "(null)", mode);
    if (runtime_romfs_owns(path)) return open_rom_directory(out, path, mode);
    char p[1100]; unsigned r = host_path(path, p, sizeof p); if (r) return r; if (!out || mode < 1 || mode > 3) return R_INVALID_ARGUMENT;
    DIR *d = opendir(p); if (!d) return errno_result();
    pthread_mutex_lock(&lock);
    for (int i = 0; i < NDIRS; i++) if (!dirs[i].dir && !dirs[i].rom) { dirs[i].dir = d; dirs[i].mode = mode; strcpy(dirs[i].path, p); *out = DIR_BASE + (uint64_t)i; pthread_mutex_unlock(&lock); return 0; }
    pthread_mutex_unlock(&lock); runtime_fail("host directory handle table full");
}
static unsigned read_directory(int64_t *count, DirectoryEntry *out, uint64_t h, int64_t max) {
    if (h < DIR_BASE || h >= DIR_BASE + NDIRS || !count || max < 0 || (!out && max)) return R_INVALID_ARGUMENT;
    Dir *d = &dirs[h - DIR_BASE]; if (!d->dir && !d->rom) return R_INVALID_ARGUMENT;
    int64_t n = 0; struct dirent *e;
    if (d->rom) {
        for (; n < max && d->pos < d->nrom; n++, d->pos++) {
            const struct RomEntry *r = &d->rom[d->pos];
            memset(&out[n], 0, sizeof *out); strcpy(out[n].name, r->name); out[n].type = r->is_dir ? 0 : 1; out[n].size = r->size;
        }
        *count = n; return 0;
    }
    while (n < max && (e = readdir(d->dir))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || strlen(e->d_name) > 768) continue;
        char child[2048]; snprintf(child, sizeof child, "%s/%s", d->path, e->d_name); struct stat st; if (stat(child, &st)) continue;
        int is_dir = S_ISDIR(st.st_mode); if (!(d->mode & (is_dir ? 1 : 2))) continue;
        memset(&out[n], 0, sizeof *out); strcpy(out[n].name, e->d_name); out[n].type = is_dir ? 0 : 1; out[n].size = is_dir ? 0 : st.st_size; n++;
    }
    *count = n; return 0;
}
static void close_directory(uint64_t h) {
    if (h < DIR_BASE || h >= DIR_BASE + NDIRS || (!dirs[h - DIR_BASE].dir && !dirs[h - DIR_BASE].rom)) runtime_fail("close invalid directory handle");
    pthread_mutex_lock(&lock); Dir *d = &dirs[h - DIR_BASE];
    if (d->rom) { free(d->rom); d->rom = NULL; } else { closedir(d->dir); d->dir = NULL; }
    pthread_mutex_unlock(&lock);
}
/* The fs priority only orders I/O inside the system fs process; host I/O is unordered. */
static void set_priority(int priority) { (void)priority; }
/* SetAllocator (Metroid Dread calls it first thing): the allocator of the SDK's own fs library. The host answers fs
   itself and never allocates guest memory for it, so there is nothing to keep. */
static void set_allocator(void *(*alloc)(uint64_t), void (*release)(void *, uint64_t)) { (void)alloc; (void)release; }

/* make probe-save: the "save in progress" state the window close waits on, through the fs entry points the game uses
   (a fresh profile in build/probe-save-profile; no game). */
void runtime_save_probe(void) {
    const char *dir = "build/probe-save-profile";   /* a fresh profile each run (a failed run leaves it for a look) */
    remove_tree(dir); setenv("TOTK_PROFILE_DIR", dir, 1);
    unsigned checks = 0; uint64_t uid[2]; runtime_account_uid(uid); uint64_t h = 0;
#define CHECK(c, what) do { checks++; if (!(c)) { fprintf(stderr, "save probe: %s\n", what); runtime_fail("save probe failed"); } } while (0)
    CHECK(mount_save("save", uid) == 0 && mount_temp("temp") == 0, "mount save and temporary storage");
    CHECK(!runtime_fs_save_busy(), "idle at start");
    CHECK(create_file("save:/a.sav", 16) == 0 && runtime_fs_save_busy(), "creating a save file starts a save");
    CHECK(commit("save") == 0 && !runtime_fs_save_busy(), "commit ends it");
    CHECK(open_file(&h, "save:/a.sav", OPEN_WRITE) == 0 && runtime_fs_save_busy(), "a save file open for writing");
    static const char data[16] = "progress";
    CHECK(write_file(h, 0, data, sizeof data, NULL) == 0 && runtime_fs_save_busy(), "writing");
    atomic_store(&save_changed_ns, runtime_now_ns() - 10000000000ull);
    CHECK(runtime_fs_save_busy(), "a file open for writing is busy however long ago it was written");
    close_file(h);
    CHECK(runtime_fs_save_busy(), "closed but not committed yet (the next file of the save follows)");
    CHECK(commit("save") == 0 && !runtime_fs_save_busy(), "committed");
    CHECK(open_file(&h, "save:/a.sav", OPEN_READ) == 0 && !runtime_fs_save_busy(), "reading is not saving");
    close_file(h);
    CHECK(create_file("save:/b.sav", 4) == 0 && runtime_fs_save_busy(), "second change");
    atomic_store(&save_changed_ns, runtime_now_ns() - 4000000000ull);
    CHECK(!runtime_fs_save_busy(), "uncommitted changes older than 3 s are not a save in progress");
    CHECK(create_file("temp:/t.bin", 4) == 0 && !runtime_fs_save_busy(), "temporary storage does not count");
    CHECK(rename_file("save:/b.sav", "save:/c.sav") == 0 && runtime_fs_save_busy(), "renaming save data");
    CHECK(commit("save") == 0 && !runtime_fs_save_busy() && delete_file("save:/c.sav") == 0 && runtime_fs_save_busy() && commit("save") == 0, "delete and commit");
#undef CHECK
    remove_tree(dir);
    printf("{\"status\":\"pass\",\"probe\":\"probe-save\",\"checks\":%u,\"scope\":\"save-in-progress state (runtime_fs_save_busy) through open / write / close / create / rename / delete / commit on a save mount; reads and temporary storage do not count; stale uncommitted changes do not hold a close\"}\n", checks);
}

/* TOTK_FS_LOG: the result of each save-side path operation as well (a game may quit on one it cannot handle). */
static int fs_result_log(void) { static int on = -1; if (on < 0) on = getenv("TOTK_FS_LOG") != NULL; return on; }
static unsigned logged(const char *op, const char *path, unsigned r) { if (fs_result_log() && path && !runtime_romfs_owns(path)) fprintf(stderr, "[totk-fs] %s %s -> %#x\n", op, path, r); return r; }
static unsigned log_open_file(uint64_t *out, const char *path, int mode) { return logged("OpenFile", path, open_file(out, path, mode)); }
static unsigned log_create_file(const char *path, int64_t size) { return logged("CreateFile", path, create_file(path, size)); }
static unsigned log_delete_file(const char *path) { return logged("DeleteFile", path, delete_file(path)); }
static unsigned log_rename_file(const char *from, const char *to) { return logged("RenameFile", from, rename_file(from, to)); }
static unsigned log_create_directory(const char *path) { return logged("CreateDirectory", path, create_directory(path)); }
static unsigned log_open_directory(uint64_t *out, const char *path, int mode) { return logged("OpenDirectory", path, open_directory(out, path, mode)); }
static unsigned log_commit(const char *name) { return logged("Commit", name, commit(name)); }
void *runtime_fs_host_override(const char *name) {
    static const struct { const char *name; void *fn; } map[] = {
        {"_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci", log_open_file}, {"_ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE", file_size},
        {"_ZN2nn2fs9CloseFileENS0_10FileHandleE", close_file}, {"_ZN2nn2fs12GetEntryTypeEPNS0_18DirectoryEntryTypeEPKc", entry_type},
        {"_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm", read_file}, {"_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvmRKNS0_10ReadOptionE", read_options},
        {"_ZN2nn2fs8ReadFileENS0_10FileHandleElPvm", read_exact},
        {"_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE", write_file},
        {"_ZN2nn2fs11SetFileSizeENS0_10FileHandleEl", set_file_size}, {"_ZN2nn2fs9FlushFileENS0_10FileHandleE", flush_file},
        {"_ZN2nn2fs10CreateFileEPKcl", log_create_file}, {"_ZN2nn2fs10DeleteFileEPKc", log_delete_file}, {"_ZN2nn2fs10RenameFileEPKcS2_", log_rename_file},
        {"_ZN2nn2fs15CreateDirectoryEPKc", log_create_directory}, {"_ZN2nn2fs26DeleteDirectoryRecursivelyEPKc", delete_recursive},
        {"_ZN2nn2fs15DeleteDirectoryEPKc", delete_directory}, {"_ZN2nn2fs12SetAllocatorEPFPvmEPFvS1_mE", set_allocator},   /* Metroid Dread */
        {"_ZN2nn2fs13OpenDirectoryEPNS0_15DirectoryHandleEPKci", log_open_directory},
        {"_ZN2nn2fs13ReadDirectoryEPlPNS0_14DirectoryEntryENS0_15DirectoryHandleEl", read_directory},
        {"_ZN2nn2fs14CloseDirectoryENS0_15DirectoryHandleE", close_directory},
        {"_ZN2nn2fs13MountSaveDataEPKcRKNS_7account3UidE", mount_save},
        {"_ZN2nn2fs21MountSaveDataReadOnlyEPKcNS_13ApplicationIdERKNS_7account3UidE", mount_save_ro},
        {"_ZN2nn2fs18IsSaveDataExistingENS_13ApplicationIdERKNS_7account3UidE", save_exists},
        {"_ZN2nn2fs21MountTemporaryStorageEPKc", mount_temp}, {"_ZN2nn2fs7UnmountEPKc", unmount}, {"_ZN2nn2fs6CommitEPKc", log_commit},
        {"_ZN2nn2fs14CommitSaveDataEPKc", log_commit}, {"_ZN2nn2fs14EnsureSaveDataERKNS_7account3UidE", ensure_save},   /* Bayonetta 3 */
        {"_ZN2nn2fs18CreateCacheStorageEill", create_cache}, {"_ZN2nn2fs18DeleteCacheStorageEi", delete_cache},
        {"_ZN2nn2fs17MountCacheStorageEPKci", mount_cache},
        {"_ZN2nn2fs20OpenCacheStorageListEPNS0_22CacheStorageListHandleE", open_cache_list},
        {"_ZN2nn2fs20ReadCacheStorageListEPiPNS0_16CacheStorageInfoENS0_22CacheStorageListHandleEi", read_cache_list},
        {"_ZN2nn2fs21CloseCacheStorageListENS0_22CacheStorageListHandleE", close_cache_list},
        {"_ZN2nn2fs26SetPriorityOnCurrentThreadENS0_8PriorityE", set_priority},
    };
    for (size_t i = 0; i < sizeof map / sizeof *map; i++) if (!strcmp(name, map[i].name)) return map[i].fn;
    return NULL;
}
