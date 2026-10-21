/* nn::fs host implementation for the functions main imports. RomFS paths and handles go to the
   verified read-only backend (romfs.c); save, temporary and cache storage are host directories under
   the profile directory. Application save data is created on first start, as the system launcher does
   for titles that declare save data.
   Save data is transactional, as on Horizon: the game's changes become durable only at Commit, and a quit, crash or
   iOS kill before it leaves the last committed save as it was (see "save transactions" below). runtime_fs_save_busy
   still makes a window close wait for a save in progress (runtime/frontend.m), so that the save the player made lands. */
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
#include <sys/clonefile.h>
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
#define R_USABLE_SPACE_NOT_ENOUGH 0x3c02u   /* fs 2-0030: the disk is full */
#ifdef GAME_BOTW
#define TOTK_APPLICATION_ID 0x01007ef00011e000ULL   /* Breath of the Wild: its own save data is the one that exists */
#else
#define TOTK_APPLICATION_ID 0x0100f2c0115b6000ULL
#endif
enum { M_FREE, M_SAVE, M_TEMP, M_CACHE };
enum { OPEN_READ = 1, OPEN_WRITE = 2, OPEN_APPEND = 4 };
#define FILE_BASE 0x200000000ULL
#define DIR_BASE 0x300000000ULL
#define NFILES 1024
#define NDIRS 256

typedef struct { int kind; char name[32]; char root[1024]; } Mount;
typedef struct { int fd, mode, save; } File;   /* save: on a save-data mount (counts for runtime_fs_save_busy) */
typedef struct { DIR *dir; char path[1024]; int mode; } Dir;
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
static _Atomic int txn_changed;   /* save data changed since the last commit (save transactions below) */
static void save_touched(void) { atomic_store(&save_changed_ns, runtime_now_ns()); atomic_store(&save_dirty, 1); atomic_store(&txn_changed, 1); }
int runtime_fs_save_busy(void) {
    if (atomic_load(&save_writers) > 0) return 1;
    return atomic_load(&save_dirty) && runtime_now_ns() - atomic_load(&save_changed_ns) < 3000000000ull;
}
static void make_dir(const char *p) {
    if (!mkdir(p, 0700) || errno == EEXIST) return;
    if (errno != ENOSPC && errno != EDQUOT) runtime_fail("create profile storage directory");
    fprintf(stderr, "[totk] %s: the disk is full, not created\n", p);   /* the game starts; what needs the folder fails cleanly */
}
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

/* ---------- save transactions ----------
   Horizon journals save data: the game sees its writes, creates, deletes and renames at once, but they become durable
   only at Commit; an uncommitted change is lost at power-off or a crash, and the last committed state survives. The
   host does the same with whole folders (the idea follows nxnative's dm/host/hle_fs.c, MIT; this code is our own):
     <profile>/save/<user>           committed: the save as of the last Commit, the only folder that is the save
     <profile>/save/.<user>.work     working copy, what the save mount shows: cloned from the committed folder at the
                                     first save mount of a run; every read and change goes here
     <profile>/save/.<user>.new/.old only during a Commit
   Commit clones the working copy to .new and swaps it with the committed folder in one renamex_np(RENAME_SWAP) (a
   volume without RENAME_SWAP: committed -> .old, .new -> committed), then deletes the old version. Open files stay in
   the working copy, which never moves. Stopped at any moment, the committed folder holds the old or the new save, never
   a mix; the next run finishes or drops the half-done step (save_recover, save_open_txn) and clones a fresh working
   copy, so uncommitted changes are lost as on the Switch. The dot names keep the copies out of Finder and the Files app.
   On APFS (the Mac and iOS user folders) a clone shares the data blocks: no copy per write, and one clone per mount and
   per Commit (tests/test_save_transaction.c measures it); other volumes copy the folder (each file fsynced).
   A save folder from earlier builds, which wrote into <profile>/save/<user> directly, is a committed save as it is.
   A full disk: a write that cannot land answers ResultUsableSpaceNotEnough and refuses this transaction's Commit (the
   committed save stays; the working copy goes back to it, as Horizon rolls its journal back); when not even the working
   copy can be made, the committed save is mounted read-only, and changes answer the same result (retried every 5 s). */
static char save_parent[1024], save_work[1100], save_new[1100], save_old[1100], save_cur[1100];   /* save_cur: the save mount's folder */
static pthread_mutex_t txn_lock = PTHREAD_MUTEX_INITIALIZER;   /* working-copy setup, Commit and reset */
static int txn_opened;                                       /* the working copy was set up (or tried) in this run */
static _Atomic int save_readonly;                            /* no working copy: the committed save, read-only */
static _Atomic int txn_failed;                               /* errno of the first change that failed since the last commit */
static _Atomic int txn_reset_pending;                        /* a refused Commit could not reset the working copy yet */
static uint64_t txn_retry_ns;
/* Tests only (tests/test_save_transaction.c): called at each named step; a nonzero errno fails the step with it, or the
   hook ends the process there to stop a Commit half-way. NULL in the game. */
static int (*txn_fault)(const char *step);
static int fault(const char *step) { int e = txn_fault ? txn_fault(step) : 0; if (e) errno = e; return e; }
static int space_errno(int e) { return e == ENOSPC || e == EDQUOT; }
static void txn_fail(int e) { int z = 0; atomic_compare_exchange_strong(&txn_failed, &z, e ? e : EIO); }
static void sync_dir(const char *p) { int fd = open(p, O_RDONLY | O_DIRECTORY); if (fd >= 0) { fcntl(fd, F_FULLFSYNC); close(fd); } }
static int copy_file(const char *from, const char *to, char *buf, size_t cap) {
    int in = open(from, O_RDONLY | O_NOFOLLOW); if (in < 0) return -1;
    int out = open(to, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600); if (out < 0) { int e = errno; close(in); errno = e; return -1; }
    int bad = 0;
    while (!bad) {
        ssize_t n = read(in, buf, cap); if (n < 0 && errno == EINTR) continue; if (n <= 0) { bad = n < 0; break; }
        for (ssize_t o = 0; o < n && !bad;) { ssize_t w = write(out, buf + o, (size_t)(n - o)); if (w < 0 && errno == EINTR) continue; if (w <= 0) { if (!w) errno = ENOSPC; bad = 1; } else o += w; }
    }
    if (!bad && fsync(out)) bad = 1;                       /* on disk before the rename that publishes it */
    int e = errno; close(in); if (close(out) && !bad) { bad = 1; e = errno; }
    errno = e; return bad ? -1 : 0;
}
static int copy_tree(const char *from, const char *to, char *buf, size_t cap) {
    if (mkdir(to, 0700)) return -1;
    DIR *d = opendir(from); if (!d) return -1;
    struct dirent *e; struct stat st; char a[2048], b[2048]; int bad = 0;
    while (!bad && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(a, sizeof a, "%s/%s", from, e->d_name); snprintf(b, sizeof b, "%s/%s", to, e->d_name);
        if (lstat(a, &st)) bad = 1; else if (S_ISDIR(st.st_mode)) bad = copy_tree(a, b, buf, cap); else if (S_ISREG(st.st_mode)) bad = copy_file(a, b, buf, cap);
    }
    int err = errno; closedir(d); errno = err; return bad ? -1 : 0;
}
/* from -> to (to does not exist): one APFS clone of the whole tree, or a copy on a volume that cannot clone. */
static int clone_tree(const char *from, const char *to) {
    int e = fault("clonefile");
    if (!e && !clonefile(from, to, CLONE_NOFOLLOW)) return 0;
    if (!e) e = errno;
    if (e == ENOTSUP) {
        char *buf = malloc(1 << 20); need(buf != NULL, "save copy buffer");
        e = fault("copy") || copy_tree(from, to, buf, 1 << 20) ? errno : 0; free(buf);
        if (!e) return 0;
    }
    remove_tree(to); errno = e; return -1;
}
/* At the start of a run, before EnsureSaveData: a Commit stopped between its two renames left no committed folder. Its
   .new is complete (the committed folder is only renamed away once .new is), so it is the save; .old is the one before. */
static void save_recover(void) {
    struct stat st; if (!lstat(save_root, &st)) return;
    const char *from = !lstat(save_new, &st) ? save_new : !lstat(save_old, &st) ? save_old : NULL;
    if (!from) return;
    if (rename(from, save_root)) runtime_fail("recover the save a commit left half swapped");
    sync_dir(save_parent);
    fprintf(stderr, "[totk] save: a commit was stopped half-way; finished it from %s\n", from);
}
/* The working copy for this run (txn_lock held): what a stopped run left is dropped (its uncommitted changes, a
   half-made .new), then the committed save is cloned. When that fails (a full disk), the committed save is mounted
   read-only instead. */
static void save_open_txn(void) {
    uint64_t t0 = runtime_now_ns(); const char *why = NULL;
    if (remove_tree(save_new) || remove_tree(save_old)) why = "cannot remove what a stopped commit left";
    else if (remove_tree(save_work)) why = "cannot remove the last run's working copy";
    else if (clone_tree(save_root, save_work)) why = "cannot make the working copy";
    int e = errno;
    pthread_mutex_lock(&lock); snprintf(save_cur, sizeof save_cur, "%s", why ? save_root : save_work); pthread_mutex_unlock(&lock);
    atomic_store(&save_readonly, why != NULL); txn_retry_ns = runtime_now_ns();
    if (why) fprintf(stderr, "[totk] save: %s (%s); the committed save %s is mounted read-only, changes are refused until there is room\n", why, strerror(e), save_root);
    else fprintf(stderr, "[totk] save transaction: working copy %s (%.1f ms)\n", save_work, (runtime_now_ns() - t0) / 1e6);
}
static void save_mount_ready(void) { pthread_mutex_lock(&txn_lock); if (!txn_opened) { txn_opened = 1; save_open_txn(); } pthread_mutex_unlock(&txn_lock); }
static int save_reset_work(void);
/* A change to save data may go ahead: 0, or the full disk's result while mounted read-only (retried every 5 s) or while
   the working copy still holds a refused save (its reset is retried first, so the game's next save starts clean). */
static unsigned save_writable(void) {
    if (!atomic_load(&save_readonly) && !atomic_load(&txn_reset_pending)) return 0;
    pthread_mutex_lock(&txn_lock);
    if (atomic_load(&txn_reset_pending)) {
        if (!save_reset_work()) { atomic_store(&txn_reset_pending, 0); atomic_store(&txn_failed, 0); atomic_store(&txn_changed, 0); }
    } else if (atomic_load(&save_readonly) && runtime_now_ns() - txn_retry_ns >= 5000000000ull) save_open_txn();
    pthread_mutex_unlock(&txn_lock);
    if (!atomic_load(&save_readonly) && !atomic_load(&txn_reset_pending)) return 0;
    txn_fail(ENOSPC);                                      /* the game's next Commit answers that its save did not land */
    return R_USABLE_SPACE_NOT_ENOUGH;
}
/* Commit's publish step (txn_lock held): the working copy becomes the committed save in one atomic swap. */
static int save_publish(void) {
    if (remove_tree(save_new) || clone_tree(save_work, save_new)) return -1;
    int e = fault("swap");
    if (!e && !renamex_np(save_new, save_root, RENAME_SWAP)) { fault("swapped"); remove_tree(save_new); sync_dir(save_parent); return 0; }
    if (!e) e = errno;
    if (e != ENOTSUP && e != EINVAL) { remove_tree(save_new); errno = e; return -1; }
    /* No RENAME_SWAP on this volume: two renames; a stop between them is finished at the next run (save_recover). */
    if (remove_tree(save_old) || rename(save_root, save_old)) { e = errno; remove_tree(save_new); errno = e; return -1; }
    fault("renamed");
    if (rename(save_new, save_root)) { e = errno; if (!rename(save_old, save_root)) remove_tree(save_new); errno = e; return -1; }
    remove_tree(save_old); sync_dir(save_parent); return 0;
}
/* After a refused Commit (txn_lock held): the working copy goes back to the committed save, cloned beside it first so a
   failure leaves it as it was (then changes and Commits are refused until a retry at the next change or Commit works). */
static int save_reset_work(void) {
    if (remove_tree(save_new) || clone_tree(save_root, save_new)) return -1;
    if (!renamex_np(save_new, save_work, RENAME_SWAP)) { remove_tree(save_new); return 0; }
    if (errno != ENOTSUP && errno != EINVAL) { int e = errno; remove_tree(save_new); errno = e; return -1; }
    return remove_tree(save_work) || rename(save_new, save_work) ? -1 : 0;
}
static unsigned save_commit(void) {
    pthread_mutex_lock(&txn_lock);
    uint64_t t0 = runtime_now_ns(); unsigned r = 0;
    int failed = atomic_exchange(&txn_failed, 0);
    if (failed) {
        r = space_errno(failed) ? R_USABLE_SPACE_NOT_ENOUGH : R_INVALID_ARGUMENT;
        int reset = atomic_load(&save_readonly) || !save_reset_work();
        atomic_store(&txn_reset_pending, !reset);
        if (reset) atomic_store(&txn_changed, 0); else txn_fail(failed);
        fprintf(stderr, "[totk] save commit refused: a change failed in this save (%s); the committed save is unchanged, the working copy %s%s\n",
                strerror(failed), atomic_load(&save_readonly) ? "is not there (read-only)" : reset ? "matches it again" : "could not be reset yet (retried at the next change)",
                atomic_load(&save_writers) ? " [save files are still open]" : "");
    } else if (atomic_exchange(&txn_changed, 0)) {
        if (save_publish()) {
            int e = errno; atomic_store(&txn_changed, 1); r = space_errno(e) ? R_USABLE_SPACE_NOT_ENOUGH : R_INVALID_ARGUMENT;
            fprintf(stderr, "[totk] save commit failed (%s); the committed save is unchanged, the next commit retries\n", strerror(e));
        } else fprintf(stderr, "[totk] save committed (%.1f ms)\n", (runtime_now_ns() - t0) / 1e6);
    }
    pthread_mutex_unlock(&txn_lock);
    return r;
}

static void init(void) {
    const char *dir = getenv("TOTK_PROFILE_DIR"); if (!dir) dir = "user-data";
    need(snprintf(profile, sizeof profile, "%s", dir) < (int)sizeof profile, "profile path");
    uint64_t uid[2]; runtime_account_uid(uid);
    char p[1100], user[33]; make_dir(profile);
    snprintf(p, sizeof p, "%s/save", profile); make_dir(p);
    snprintf(user, sizeof user, "%016llx%016llx", (unsigned long long)uid[1], (unsigned long long)uid[0]);
    need(snprintf(save_root, sizeof save_root, "%s/save/%s", profile, user) < (int)sizeof save_root, "save path");
    snprintf(save_parent, sizeof save_parent, "%s/save", profile); snprintf(save_cur, sizeof save_cur, "%s", save_root);
    snprintf(save_work, sizeof save_work, "%s/.%s.work", save_parent, user);
    snprintf(save_new, sizeof save_new, "%s/.%s.new", save_parent, user); snprintf(save_old, sizeof save_old, "%s/.%s.old", save_parent, user);
    save_recover();
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
    char root[1100]; if (m) strcpy(root, m->kind == M_SAVE ? save_cur : m->root);   /* save: the working copy (or read-only, the committed save) */
    int k = m ? m->kind : M_FREE;
    pthread_mutex_unlock(&lock);
    if (!m) return R_PATH_NOT_FOUND;
    if (kind) *kind = k;
    const char *rest = colon + 1;
    for (const char *q = rest + 1; *q;) { const char *e = strchr(q, '/'); size_t len = e ? (size_t)(e - q) : strlen(q); if (!e && !len) break; if (!component_ok(q, len) || memchr(q, '\\', len)) return R_INVALID_PATH; q = e ? e + 1 : q + len; }
    if (snprintf(out, cap, "%s%s", root, rest) >= (int)cap) return R_INVALID_PATH;
    size_t L = strlen(out); while (L > strlen(root) && out[L - 1] == '/') out[--L] = 0;
    return 0;
}
static unsigned host_path(const char *path, char *out, size_t cap) { return host_path_kind(path, out, cap, NULL); }
/* A change through a path (create, delete, rename, directories, opening for writing): on save data it must be writable
   (save_writable), is resolved again after that (a retry can move the mount from the read-only committed save to a new
   working copy) and marks the save as in progress. */
static unsigned change_path(const char *path, char *out, size_t cap, int *kind) {
    unsigned r = host_path_kind(path, out, cap, kind);
    if (r || *kind != M_SAVE) return r;
    if ((r = save_writable()) || (r = host_path_kind(path, out, cap, kind))) return r;
    save_touched(); return 0;
}
static unsigned save_change_path(const char *path, char *out, size_t cap) { int kind = M_FREE; return change_path(path, out, cap, &kind); }
static unsigned errno_result(void) { return errno == ENOENT || errno == ENOTDIR ? R_PATH_NOT_FOUND : errno == EEXIST ? R_ALREADY_EXISTS : errno == ENOTEMPTY ? R_NOT_EMPTY : space_errno(errno) ? R_USABLE_SPACE_NOT_ENOUGH : R_INVALID_ARGUMENT; }

/* ---------- storage mounts ---------- */
static unsigned mount_save(const char *name, const uint64_t *uid) {
    ready(); uint64_t me[2]; runtime_account_uid(me);
    if (!uid || uid[0] != me[0] || uid[1] != me[1]) return R_TARGET_NOT_FOUND;
    save_mount_ready(); return mount_add(name, M_SAVE, save_cur);
}
static unsigned char save_exists(uint64_t application, const uint64_t *uid) {
    ready(); uint64_t me[2]; runtime_account_uid(me);
    return application == TOTK_APPLICATION_ID && uid && uid[0] == me[0] && uid[1] == me[1];
}
/* Other titles' save data (e.g. data-link bonuses) is not present on this host. */
static unsigned mount_save_ro(const char *name, uint64_t application, const uint64_t *uid) {
    if (!save_exists(application, uid)) return R_TARGET_NOT_FOUND;
    save_mount_ready(); return mount_add(name, M_SAVE, save_cur);   /* the same working copy as the save mount */
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
    BOTW_AOC_UNMOUNT(name) if (!runtime_romfs_owns(name)) runtime_fail("unmount of unknown fs mount");
}
static unsigned commit(const char *name) {
    pthread_mutex_lock(&lock); int found = 0, save = 0;
    for (int i = 0; i < 16; i++) if (mounts[i].kind && !strcmp(mounts[i].name, name)) { found = 1; save = mounts[i].kind == M_SAVE; }
    pthread_mutex_unlock(&lock);
    if (!found) return R_PATH_NOT_FOUND;
    for (int i = 0; i < NFILES; i++) { pthread_mutex_lock(&lock); int fd = files[i].fd; pthread_mutex_unlock(&lock); if (fd > 0) fcntl(fd, F_FULLFSYNC); }
    unsigned r = save ? save_commit() : 0;               /* save data: the transaction lands (or is refused) */
    if (save && !(getenv("TOTK_PROBE_NEGATIVE") && atoi(getenv("TOTK_PROBE_NEGATIVE")) == 23)) atomic_store(&save_dirty, 0);   /* 23: probe-save's negative control */
    return r;
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
    if (runtime_romfs_owns(path)) return runtime_romfs_open(out, path, mode); BOTW_AOC_OPEN(out, path, mode)
    if (!out || !(mode & (OPEN_READ | OPEN_WRITE)) || (mode & ~7)) return R_INVALID_ARGUMENT;
    char p[1100]; int kind = M_FREE; unsigned r = host_path_kind(path, p, sizeof p, &kind); if (r) return r;
    struct stat st; if (stat(p, &st)) return R_PATH_NOT_FOUND; if (!S_ISREG(st.st_mode)) return R_PATH_NOT_FOUND;
    int save_writer = kind == M_SAVE && (mode & OPEN_WRITE);
    if (save_writer && (r = change_path(path, p, sizeof p, &kind))) return r;   /* writable save data */
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
    if (runtime_romfs_handle(h)) return runtime_romfs_read(out, h, offset, buffer, size); BOTW_AOC_READ(out, h, offset, buffer, size)
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
    while (done < size) {
        ssize_t n = f->save && fault("write") ? -1 : pwrite(fd, (const uint8_t *)buffer + done, (size_t)(size - done), offset + (off_t)done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {                                      /* a full disk answers as on the Switch; on save data the file may be half written, */
            int e = n < 0 ? errno : ENOSPC;                /* so this transaction's Commit is refused and the committed save stays */
            if (!space_errno(e)) runtime_fail("host save write failure");
            if (f->save) txn_fail(e);
            return R_USABLE_SPACE_NOT_ENOUGH;
        }
        done += (uint64_t)n;
    }
    if (option && (*option & 1)) fcntl(fd, F_FULLFSYNC);
    return 0;
}
static unsigned file_size(int64_t *out, uint64_t h) {
    if (runtime_romfs_handle(h)) return runtime_romfs_size(out, h); BOTW_AOC_SIZE(out, h)
    int fd; struct stat st; if (!out || !file_of(h, &fd, NULL) || fstat(fd, &st)) return R_INVALID_ARGUMENT;
    *out = st.st_size; return 0;
}
static unsigned set_file_size(uint64_t h, int64_t size) {
    int fd, mode; if (size < 0 || !file_of(h, &fd, &mode)) return R_INVALID_ARGUMENT;
    if (!(mode & OPEN_WRITE)) return R_OPEN_MODE;
    int save = files[h - FILE_BASE].save; if (save) save_touched();
    if (!ftruncate(fd, size)) return 0;
    if (save && space_errno(errno)) txn_fail(errno);       /* a file that could not grow: the Commit is refused (see write_file) */
    return errno_result();
}
static unsigned flush_file(uint64_t h) { int fd; if (!file_of(h, &fd, NULL)) return R_INVALID_ARGUMENT; fcntl(fd, F_FULLFSYNC); return 0; }
static void close_file(uint64_t h) {
    if (runtime_romfs_handle(h)) { runtime_romfs_close(h); return; } BOTW_AOC_CLOSE(h)
    if (!file_of(h, NULL, NULL)) runtime_fail("close invalid file handle");
    pthread_mutex_lock(&lock); int fd = files[h - FILE_BASE].fd, save = files[h - FILE_BASE].save; files[h - FILE_BASE].fd = 0; pthread_mutex_unlock(&lock);
    close(fd);
    if (save) { save_touched(); atomic_fetch_sub(&save_writers, 1); }
}
static unsigned create_file(const char *path, int64_t size) {
    char p[1100]; unsigned r = save_change_path(path, p, sizeof p); if (r) return r; if (size < 0) return R_INVALID_ARGUMENT;
    int fd = open(p, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600); if (fd < 0) return errno_result();
    if (!ftruncate(fd, size)) { close(fd); return 0; }
    int e = errno; close(fd); unlink(p); errno = e; return errno_result();   /* no size: no file (a full disk) */
}
static unsigned delete_file(const char *path) { char p[1100]; unsigned r = save_change_path(path, p, sizeof p); if (r) return r; return unlink(p) ? errno_result() : 0; }
static unsigned rename_file(const char *from, const char *to) {
    char a[1100], b[1100]; unsigned r = save_change_path(from, a, sizeof a); if (!r) r = host_path(to, b, sizeof b); if (r) return r;
    struct stat st; if (!stat(b, &st)) return R_ALREADY_EXISTS;
    return rename(a, b) ? errno_result() : 0;
}
static unsigned entry_type(int *out, const char *path) {
    if (runtime_romfs_owns(path)) return runtime_romfs_entry_type(out, path); BOTW_AOC_TYPE(out, path)
    char p[1100]; unsigned r = host_path(path, p, sizeof p); if (r) return r; struct stat st;
    if (!out) return R_INVALID_ARGUMENT; if (stat(p, &st)) return R_PATH_NOT_FOUND;
    *out = S_ISDIR(st.st_mode) ? 0 : 1; return 0;
}

/* ---------- directories ---------- */
typedef struct { char name[769]; uint8_t pad0[3]; int8_t type; uint8_t pad1[3]; int64_t size; } DirectoryEntry;
_Static_assert(sizeof(DirectoryEntry) == 0x310, "nn::fs::DirectoryEntry");
static unsigned create_directory(const char *path) { char p[1100]; unsigned r = save_change_path(path, p, sizeof p); if (r) return r; return mkdir(p, 0700) ? errno_result() : 0; }
static unsigned delete_recursive(const char *path) { char p[1100]; unsigned r = save_change_path(path, p, sizeof p); if (r) return r; struct stat st; if (stat(p, &st)) return R_PATH_NOT_FOUND; return remove_tree(p) ? R_INVALID_ARGUMENT : 0; }
static unsigned open_directory(uint64_t *out, const char *path, int mode) {
    if (runtime_romfs_owns(path)) { fprintf(stderr, "[totk] OpenDirectory on RomFS %s\n", path); runtime_fail("RomFS directory listing not implemented"); }
    char p[1100]; unsigned r = host_path(path, p, sizeof p); if (r) return r; if (!out || mode < 1 || mode > 3) return R_INVALID_ARGUMENT;
    DIR *d = opendir(p); if (!d) return errno_result();
    pthread_mutex_lock(&lock);
    for (int i = 0; i < NDIRS; i++) if (!dirs[i].dir) { dirs[i].dir = d; dirs[i].mode = mode; strcpy(dirs[i].path, p); *out = DIR_BASE + (uint64_t)i; pthread_mutex_unlock(&lock); return 0; }
    pthread_mutex_unlock(&lock); runtime_fail("host directory handle table full");
}
static unsigned read_directory(int64_t *count, DirectoryEntry *out, uint64_t h, int64_t max) {
    if (h < DIR_BASE || h >= DIR_BASE + NDIRS || !count || max < 0 || (!out && max)) return R_INVALID_ARGUMENT;
    Dir *d = &dirs[h - DIR_BASE]; if (!d->dir) return R_INVALID_ARGUMENT;
    int64_t n = 0; struct dirent *e;
    while (n < max && (e = readdir(d->dir))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || strlen(e->d_name) > 768) continue;
        char child[2048]; snprintf(child, sizeof child, "%s/%s", d->path, e->d_name); struct stat st; if (stat(child, &st)) continue;
        int is_dir = S_ISDIR(st.st_mode); if (!(d->mode & (is_dir ? 1 : 2))) continue;
        memset(&out[n], 0, sizeof *out); strcpy(out[n].name, e->d_name); out[n].type = is_dir ? 0 : 1; out[n].size = is_dir ? 0 : st.st_size; n++;
    }
    *count = n; return 0;
}
static void close_directory(uint64_t h) {
    if (h < DIR_BASE || h >= DIR_BASE + NDIRS || !dirs[h - DIR_BASE].dir) runtime_fail("close invalid directory handle");
    pthread_mutex_lock(&lock); closedir(dirs[h - DIR_BASE].dir); dirs[h - DIR_BASE].dir = NULL; pthread_mutex_unlock(&lock);
}
/* The fs priority only orders I/O inside the system fs process; host I/O is unordered. */
static void set_priority(int priority) { (void)priority; }

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

void *runtime_fs_host_override(const char *name) {
    static const struct { const char *name; void *fn; } map[] = {
        {"_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci", open_file}, {"_ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE", file_size}, BOTW_AOC_TABLE
        {"_ZN2nn2fs9CloseFileENS0_10FileHandleE", close_file}, {"_ZN2nn2fs12GetEntryTypeEPNS0_18DirectoryEntryTypeEPKc", entry_type},
        {"_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm", read_file}, {"_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvmRKNS0_10ReadOptionE", read_options},
        {"_ZN2nn2fs8ReadFileENS0_10FileHandleElPvm", read_exact},
        {"_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE", write_file},
        {"_ZN2nn2fs11SetFileSizeENS0_10FileHandleEl", set_file_size}, {"_ZN2nn2fs9FlushFileENS0_10FileHandleE", flush_file},
        {"_ZN2nn2fs10CreateFileEPKcl", create_file}, {"_ZN2nn2fs10DeleteFileEPKc", delete_file}, {"_ZN2nn2fs10RenameFileEPKcS2_", rename_file},
        {"_ZN2nn2fs15CreateDirectoryEPKc", create_directory}, {"_ZN2nn2fs26DeleteDirectoryRecursivelyEPKc", delete_recursive},
        {"_ZN2nn2fs13OpenDirectoryEPNS0_15DirectoryHandleEPKci", open_directory},
        {"_ZN2nn2fs13ReadDirectoryEPlPNS0_14DirectoryEntryENS0_15DirectoryHandleEl", read_directory},
        {"_ZN2nn2fs14CloseDirectoryENS0_15DirectoryHandleE", close_directory},
        {"_ZN2nn2fs13MountSaveDataEPKcRKNS_7account3UidE", mount_save},
        {"_ZN2nn2fs21MountSaveDataReadOnlyEPKcNS_13ApplicationIdERKNS_7account3UidE", mount_save_ro},
        {"_ZN2nn2fs18IsSaveDataExistingENS_13ApplicationIdERKNS_7account3UidE", save_exists},
        {"_ZN2nn2fs21MountTemporaryStorageEPKc", mount_temp}, {"_ZN2nn2fs7UnmountEPKc", unmount}, {"_ZN2nn2fs6CommitEPKc", commit},
#ifdef GAME_BOTW
        {"_ZN2nn2fs14CommitSaveDataEPKc", commit},   /* SDK 7 (Breath of the Wild 1.6.0) */
#endif
       
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
