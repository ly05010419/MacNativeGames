// nn::fs: on the real device it goes through the fsp-srv service; the host connects directly to the Mac file system. Mount name → host directory:
//   rom   → $DM_ROMFS (default $DM_ROOT/romfs, read-only; the game parses the storage blocks in it itself)
//   save  → $DM_SAVE (must be set, pointing to this run's copy of the standard save save_ref; if unset, report a failure and abort)
//   temp  → $DM_TEMP (default $DM_ROOT/run/platform/temp)
// Paths look like "rom:/data/x". FileHandle / DirectoryHandle are 8 bytes passed by value, here they store the host fd / DIR*.
// Struct layout follows the public Horizon ABI: DirectoryEntry = {char name[0x301], pad[3], s8 type, pad[3], s64 size} (0x310 bytes).
#include "hle.h"
#include <mach/mach.h>
#include "front.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdatomic.h>
#include <sys/clonefile.h>
#include <sys/xattr.h>

// Every take-over in this file runs on the big host stack (hle_bigstack.c): path buffers are sized by PATH_MAX and one file open takes 6-7 KB of stack, while a game's save thread can have as little as 8 KB (Super Mario 3D World + Bowser's Fury)
#undef HLE_FORCE
#define HLE_FORCE HLE_FORCE_BIG

#define RES_PATH_NOT_FOUND 0x202u      // fs module 2, description 1
#define RES_PATH_EXISTS    0x402u      // fs module 2, description 2
enum { MODE_READ = 1, MODE_WRITE = 2, MODE_APPEND = 4 };
enum { ENT_DIR = 0, ENT_FILE = 1 };
enum { DIRMODE_DIR = 1, DIRMODE_FILE = 2 };

typedef struct { char name[0x301]; uint8_t pad0[3]; int8_t type; uint8_t pad1[3]; int64_t size; } DirectoryEntry;
_Static_assert(sizeof(DirectoryEntry) == 0x310, "DirectoryEntry layout");

#define MAX_MOUNT 512   // Monster Hunter Rise mounts every add-on content it owns (aoc0 … : 252 licences in the folder) right after the first-boot settings: 16 ended in "mount table full"
// Save transactions (HLE audit items 1–3): on the real device a save can be rolled back as a whole before commit, and after a crash / kill a remount sees only the version from the last Commit.
// Host: at mount, the committed directory commit (= $DM_SAVE) is clonefile'd as a whole tree into the working directory <commit>.work (APFS copy-on-write, nearly zero cost), and the mount points at the working directory,
// all game reads and writes happen in the working directory; Commit = clonefile(work → <commit>.new), then renamex_np(RENAME_SWAP) swaps it atomically with commit, then the swapped-out old version is deleted.
// Killed at any moment, commit is either the old version or the new version, never half-written / a mix of old and new; the next mount discards leftover .work / .new.
typedef struct { char name[32]; char dir[1024]; char overlay[4096]; char commit[1024]; int ro; } mount_t;
static mount_t g_mnt[MAX_MOUNT];   // commit non-empty = transactional save mount
static int g_nmnt;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_path_lock = PTHREAD_MUTEX_INITIALIZER;
static void path_unlock(pthread_mutex_t **m) { pthread_mutex_unlock(*m); }
#define PATH_LOCK() pthread_mutex_t *path_guard __attribute__((cleanup(path_unlock))) = &g_path_lock; pthread_mutex_lock(path_guard)
static uint8_t g_fmode[65536];              // fd → open mode
static uint8_t g_fsave[65536];              // fd → whether it is on a transactional save mount (the save write injection point only counts these)
static _Atomic int g_save_writes;           // number of writes on the save mount (shared by the injection point and the commit log)
static _Atomic uint64_t g_txn_first_ns;     // time of this transaction's first save write (used by the commit log to report timing; cleared to 0 at commit)
// Write failures are refused per transaction: on the real device save space is reserved when the save is created, overwrites cannot fail, and the game does not check the return values of WriteFile / SetFileSize either, it Commits as usual after a failure
// (after injecting a failure on the 1st write, the committed save file was 0 bytes). When a host write fails (disk full etc., really possible on iOS):
// if any save write / set-size failed between the last Commit and this Commit, this Commit is refused -- the committed version is unchanged, the working directory is discarded and re-cloned from the committed version,
// and the front end is notified (front_save_failed); the next transaction (the game's next full save rewrite) is attempted as usual and committed if it succeeds. g_save_poison = errno of the first failure in this transaction (0 = none).
static _Atomic int g_save_poison;
static void save_poison(int e) { int z = 0; atomic_compare_exchange_strong(&g_save_poison, &z, e ? e : EIO); }
// The default weak implementation only logs (Mac); the iOS front end provides a strong implementation of the same name that shows a native alert. Called on the game thread, the front end switches to the main thread itself
__attribute__((weak)) void front_save_failed(int err) { LOG("front_save_failed(errno=%d): no front end attached (default weak implementation)", err); }
// DM_FSLOG=<file>: each ReadFile logs one TSV line "seconds since boot\tguest path\toffset\tlength\tguest buffer address" (which bytes boot read, for offline reverse lookup against the manifest)
static FILE *g_fslog; static char *g_fdpath[4096]; static pthread_mutex_t g_fslog_lock = PTHREAD_MUTEX_INITIALIZER;
static void fslog_init(void) { static int done; if (done) return; done = 1; const char *e = getenv("DM_FSLOG"); if (e && !(g_fslog = fopen(e, "w"))) die("cannot open DM_FSLOG %s", e); if (g_fslog) setvbuf(g_fslog, NULL, _IOLBF, 0); }   // line-buffered: runs end at the time-limit alarm or a crash, which never flush stdio

static const char *envdir(const char *var, const char *rel, char *buf, size_t n) {
    const char *v = getenv(var);
    if (v) return v;
    snprintf(buf, n, "%s%s/%s", getenv("DM_ROOT") ?: getenv("HOME"), getenv("DM_ROOT") ? "" : "/dm-work", rel); return buf;
}

// Steps that end in a failed write: test injection DM_TEST_FS_FAILAT=<point>[:<count>][,...] -- in a save transaction, mountclone (clone the work directory at mount),
// reclone (reclone after a refused commit), guestname (write the guest name after a rename), renameback (rename rollback) fail with ENOSPC for the first <count> times (default 1),
// to check "no exit, committed version unchanged, next commit works" (tests/t_fs.c 6). Without the variable it costs one getenv
static int dm_fs_test_fail(const char *pt) {
    static char spec[256]; static int cnt[8]; const char *e = getenv("DM_TEST_FS_FAILAT"); if (!e || !e[0]) return 0;
    if (strcmp(spec, e)) { strlcpy(spec, e, sizeof spec); memset(cnt, 0, sizeof cnt); }   // a new set of injections starts counting from zero
    size_t L = strlen(pt); int k = 0;
    for (const char *q = spec; *q && k < 8; k++) { const char *end = strchr(q, ','); size_t n = end ? (size_t)(end - q) : strlen(q);
        if (n >= L && !strncmp(q, pt, L) && (n == L || q[L] == ':')) { int lim = n > L ? atoi(q + L + 1) : 1; if (cnt[k] < lim) { cnt[k]++; errno = ENOSPC; LOG("DM_TEST_FS_FAILAT: %s fails with ENOSPC (#%d)", pt, cnt[k]); return 1; } return 0; }
        q = end ? end + 1 : q + n; }
    return 0;
}
static int in_txn_dir(const char *hp) { for (int i = 0; i < g_nmnt; i++) { size_t n = strlen(g_mnt[i].dir); if (g_mnt[i].commit[0] && !strncmp(hp, g_mnt[i].dir, n) && (hp[n] == '/' || !hp[n])) return 1; } return 0; }
static int rm_tree(const char *p);
static Result mount_ex(const char *name, const char *dir, int ro, int txn) {
    PATH_LOCK(); pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_nmnt; i++)
        if (!strcmp(g_mnt[i].name, name)) { pthread_mutex_unlock(&g_lock); return RES_OK; }
    if (g_nmnt >= MAX_MOUNT) die("mount table full");
    strlcpy(g_mnt[g_nmnt].name, name, sizeof g_mnt[0].name);
    g_mnt[g_nmnt].ro = ro; g_mnt[g_nmnt].overlay[0] = 0; g_mnt[g_nmnt].commit[0] = 0;
    if (!strcmp(name, "rom") && getenv("DM_ROMFS_OVERLAY"))
        strlcpy(g_mnt[g_nmnt].overlay, getenv("DM_ROMFS_OVERLAY"), sizeof g_mnt[0].overlay);
    // The transaction directory cannot be set up (leftovers cannot be cleared / clone failed, which really happens on a full disk): do not exit; mount the committed directory read-only -- saves still read, every write fails, commits are no-ops, the committed version is untouched, and the front end is told
    // (each of these three failures used to call die: with the phone's storage full the game exited as soon as it mounted its saves)
    int txn_err = 0, k = 0;
    while (txn && k < g_nmnt && strcmp(g_mnt[k].commit, dir)) k++;
    if (txn && k < g_nmnt) {   // a second name for a save already mounted (account and device save data are one host save): it shares that transaction, whose pending writes a new clone would drop
        strlcpy(g_mnt[g_nmnt].commit, g_mnt[k].commit, sizeof g_mnt[0].commit); strlcpy(g_mnt[g_nmnt].dir, g_mnt[k].dir, sizeof g_mnt[0].dir);
    } else if (txn) {
        char work[1100], neu[1100]; snprintf(work, sizeof work, "%s.work", dir); snprintf(neu, sizeof neu, "%s.new", dir);
        mkdir(dir, 0755);
        const char *why = NULL;
        if (!access(work, F_OK) && rm_tree(work)) why = "cannot clear the work directory left over from last time";
        else if (!access(neu, F_OK) && rm_tree(neu)) why = "cannot clear the .new left over from last time";
        else if (dm_fs_test_fail("mountclone") || clonefile(dir, work, 0)) why = "cloning the work directory failed";
        if (why) { txn_err = errno ? errno : EIO; g_mnt[g_nmnt].ro = 1; strlcpy(g_mnt[g_nmnt].dir, dir, sizeof g_mnt[0].dir);
            LOG("[RED] save transaction: %s (%s, errno=%d) -- mounting the committed directory %s read-only instead: saves cannot be written this session, the committed version is unchanged", why, work, txn_err, dir); }
        else {
            strlcpy(g_mnt[g_nmnt].commit, dir, sizeof g_mnt[0].commit);
            atomic_store(&g_save_poison, 0);                                // the working directory was just cloned from the committed version, intact
            strlcpy(g_mnt[g_nmnt].dir, work, sizeof g_mnt[0].dir); }
    } else strlcpy(g_mnt[g_nmnt].dir, dir, sizeof g_mnt[0].dir);
    g_nmnt++;
    pthread_mutex_unlock(&g_lock);
    if (txn_err) front_save_failed(txn_err);
    if (!ro && !txn) mkdir(dir, 0755);
    LOG("fs mount %s: → %s%s%s", name, txn ? g_mnt[g_nmnt - 1].dir : dir, ro || txn_err ? " (read-only)" : txn ? " (transactional save: work directory, swapped in atomically on Commit)" : "", g_mnt[g_nmnt - 1].overlay[0] ? " (overlay first)" : "");
    return RES_OK;
}
static Result mount(const char *name, const char *dir, int ro) { return mount_ex(name, dir, ro, 0); }

// APFS is case-insensitive by default, Horizon paths are case-sensitive: every component is looked up exactly by guest name.
// Ordinary host names are unchanged; only when a name collides on the host is a separate physical name chosen, with the guest name stored in an xattr.
// clonefile preserves xattr, so save Commit/remount needs no separate mapping table; enumeration must not leak physical aliases.
#define FS_GUEST_NAME "org.d2rmobile.guest-name"   // the name this host has always used: existing saves carry it, so it is kept
static int guest_name(const char *parent, const char *physical, char *out, size_t n) {
    // Only the physical names chosen on a case collision (.dm-case-N) carry the xattr (set_guest_name writes it only when the physical name differs from the guest name); any other physical name is the guest name,
    // so no per-entry getxattr is needed -- that was one system call per entry when listing, seconds for the 216,000 entries Engage walks in RomFS at boot
    if (strncmp(physical, ".dm-case-", 9)) { strlcpy(out, physical, n); return 1; }
    char p[1280]; snprintf(p, sizeof p, "%s/%s", parent, physical);
    ssize_t k = getxattr(p, FS_GUEST_NAME, out, n - 1, 0, XATTR_NOFOLLOW);
    if (k >= 0) { out[k] = 0; return 1; }
    if (errno != ENOATTR) return 0;
    strlcpy(out, physical, n); return 1;
}
// Name index of the directories of read-only mounts (RomFS, a save that fell back to read-only): guest name → physical name, sorted by guest name. The mount table is resident and a read-only mount does not change for the whole process,
// so each directory's index is built once at its first lookup and stays valid. The entry-by-entry comparison below calls getxattr on every entry of the parent directory for each path component:
// Engage's voice bank directories have 38,000 entries, and walking the whole RomFS at boot (9,680 directories) took about 17 s of black screen on the Mac and longer on the phone;
// in play every audio file Wwise opened also took tens of milliseconds, all while holding the global path lock. Callers all hold PATH_LOCK (like host_path_ex), so the index takes no lock of its own.
typedef struct { const char *guest, *phys; } name_ent;
typedef struct dir_index { struct dir_index *next; uint64_t hash; char *path; size_t n; name_ent *e; char *pool; } dir_index;
#define DIR_INDEX_SLOTS 8192
static dir_index *g_dir_index[DIR_INDEX_SLOTS];
static uint64_t fnv1a(const char *s) { uint64_t h = 1469598103934665603ull; while (*s) { h ^= (uint8_t)*s++; h *= 1099511628211ull; } return h; }
static int name_ent_cmp(const void *a, const void *b) { return strcmp(((const name_ent *)a)->guest, ((const name_ent *)b)->guest); }
// Returns NULL when it cannot be built (the directory cannot be opened, reading an entry's xattr fails); the caller then takes the original path
static const dir_index *dir_index_get(const char *dir) {
    uint64_t h = fnv1a(dir); dir_index **slot = &g_dir_index[h % DIR_INDEX_SLOTS];
    for (dir_index *x = *slot; x; x = x->next) if (x->hash == h && !strcmp(x->path, dir)) return x;
    DIR *d = opendir(dir); if (!d) return NULL;
    // First gather the names into one pool (a physical name is stored separately only when it differs from the guest name), with offsets in off[]; they become pointers once the pool stops growing
    size_t n = 0, cap = 64, plen = 0, pcap = 4096; size_t (*off)[2] = malloc(cap * sizeof *off); char *pool = malloc(pcap);
    if (!off || !pool) die("directory index allocation failed");
    struct dirent *e; char guest[0x301]; int ok = 1;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (!guest_name(dir, e->d_name, guest, sizeof guest)) { ok = 0; break; }
        size_t gl = strlen(guest) + 1, same = !strcmp(guest, e->d_name), pl = same ? 0 : strlen(e->d_name) + 1;
        if (n == cap && !(off = realloc(off, (cap *= 2) * sizeof *off))) die("directory index allocation failed");
        while (plen + gl + pl > pcap) if (!(pool = realloc(pool, pcap *= 2))) die("directory index allocation failed");
        off[n][0] = plen; memcpy(pool + plen, guest, gl); plen += gl;
        off[n][1] = same ? off[n][0] : plen; if (!same) { memcpy(pool + plen, e->d_name, pl); plen += pl; }
        n++;
    }
    closedir(d);
    if (!ok) { free(off); free(pool); return NULL; }
    dir_index *x = calloc(1, sizeof *x); if (!x || !(x->path = strdup(dir)) || !(x->e = malloc((n ? n : 1) * sizeof *x->e))) die("directory index allocation failed");
    for (size_t i = 0; i < n; i++) { x->e[i].guest = pool + off[i][0]; x->e[i].phys = pool + off[i][1]; }
    free(off); qsort(x->e, n, sizeof *x->e, name_ent_cmp);
    x->hash = h; x->n = n; x->pool = pool; x->next = *slot; *slot = x;
    return x;
}
static int path_component(char *parent, size_t n, const char *name, int create, int ro) {
    if (ro && !create) {
        const dir_index *x = dir_index_get(parent);
        if (x) {
            name_ent key = {name, NULL}; const name_ent *hit = bsearch(&key, x->e, x->n, sizeof *x->e, name_ent_cmp);
            if (!hit) { errno = ENOENT; return 0; }
            size_t len = strlen(parent), k = strlen(hit->phys);
            if (len + k + 2 > n) { errno = ENAMETOOLONG; return 0; }
            parent[len] = '/'; memcpy(parent + len + 1, hit->phys, k + 1); return 1;
        }
    }
    DIR *d = opendir(parent); if (!d) return 0;
    struct dirent *e; char physical[0x301] = "", guest[0x301];
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (!guest_name(parent, e->d_name, guest, sizeof guest)) { closedir(d); return 0; }
        if (!strcmp(guest, name)) { strlcpy(physical, e->d_name, sizeof physical); break; }
    }
    closedir(d);
    if (!physical[0]) {
        if (!create) { errno = ENOENT; return 0; }
        strlcpy(physical, name, sizeof physical);
        char p[1280]; struct stat st;
        snprintf(p, sizeof p, "%s/%s", parent, physical);
        if (!lstat(p, &st)) { // the exact guest name does not exist, but APFS matched the physical name to another entry
            unsigned i = 0;
            do { snprintf(physical, sizeof physical, ".dm-case-%u", ++i);
                 snprintf(p, sizeof p, "%s/%s", parent, physical); } while (!lstat(p, &st));
        }
        if (errno != ENOENT) return 0;
    }
    size_t len = strlen(parent), k = strlen(physical);
    if (len + k + 2 > n) { errno = ENAMETOOLONG; return 0; }
    parent[len] = '/'; memcpy(parent + len + 1, physical, k + 1); return 1;
}
static int root_path(const char *root, const char *rest, char *out, size_t n) {
    strlcpy(out, root, n);
    while (*rest) {
        while (*rest == '/') rest++;
        if (!*rest) break;
        const char *end = strchr(rest, '/'); if (!end) end = rest + strlen(rest);
        size_t k = end - rest; char part[0x301];
        if (k >= sizeof part) { errno = ENAMETOOLONG; return 0; }
        memcpy(part, rest, k); part[k] = 0;
        if (!strcmp(part, "..")) { errno = ENOENT; return 0; }
        if (strcmp(part, ".") && !path_component(out, n, part, 0, 1)) return 0;   // overlay roots are all RomFS, read-only
        rest = end;
    }
    return 1;
}
static int rom_overlay_path(const mount_t *m, const char *rest, char *out, size_t n) {
    char *roots = strdup(m->overlay);
    if (!roots) die("rom overlay path allocation failed");
    char *save = NULL;
    for (char *root = strtok_r(roots, ":", &save); root; root = strtok_r(NULL, ":", &save)) {
        char candidate[1024];
        if (root_path(root, rest, candidate, sizeof candidate) && !access(candidate, F_OK)) {
            strlcpy(out, candidate, n); free(roots); return 1;
        }
    }
    int found = root_path(m->dir, rest, out, n) && !access(out, F_OK);
    free(roots);
    if (found) return 1;
    errno = ENOENT; return 0;
}
// The path after the mount name is normalized lexically as Horizon's PathNormalizer does: "." is dropped, ".." goes up one level, and going above the root is an error (ResultDirectoryUnobtainable on hardware, reported here as not found).
// Metroid Dread's stream sound paths are "rom:/system/snd/../../sounds/streams/...": ".." used to be reported as not found, and Atk's stream load task got an unexpected error code and aborted (main+0x1225a8c)
static int norm_rest(const char *rest, char *out, size_t n) {
    size_t len = 0; out[0] = 0;
    while (*rest) {
        while (*rest == '/') rest++;
        if (!*rest) break;
        const char *end = strchr(rest, '/'); if (!end) end = rest + strlen(rest);
        size_t k = end - rest;
        if (k == 2 && rest[0] == '.' && rest[1] == '.') {
            if (!len) { errno = ENOENT; return 0; }
            while (len && out[len - 1] != '/') len--;
            if (len) len--;
            out[len] = 0;
        } else if (!(k == 1 && rest[0] == '.')) {
            if (len + 1 + k + 1 > n) { errno = ENAMETOOLONG; return 0; }
            out[len++] = '/'; memcpy(out + len, rest, k); len += k; out[len] = 0;
        }
        rest = end;
    }
    return 1;
}
// create only allows the last component to not exist yet; intermediate directories still need an exact match.
static int host_path_ex(const char *p, char *out, size_t n, int *ro, int create) {
    const char *c = strchr(p, ':');
    if (!c) { errno = ENOENT; return 0; }
    size_t ln = c - p;
    for (int i = 0; i < g_nmnt; i++) {
        if (strlen(g_mnt[i].name) != ln || strncmp(g_mnt[i].name, p, ln)) continue;
        if (ro) *ro = g_mnt[i].ro;
        char normed[1024]; const char *rest = c + 1;
        if (strstr(rest, "/.")) { if (!norm_rest(rest, normed, sizeof normed)) return 0; rest = normed; }   // only paths with "." components are normalized; the rest walk component by component as before
        if (g_mnt[i].overlay[0] && !create) return rom_overlay_path(&g_mnt[i], rest, out, n);
        strlcpy(out, g_mnt[i].dir, n);
        while (*rest) {
            while (*rest == '/') rest++;
            if (!*rest) break;
            const char *end = strchr(rest, '/'); if (!end) end = rest + strlen(rest);
            size_t k = end - rest; char part[0x301];
            if (k >= sizeof part) { errno = ENAMETOOLONG; return 0; }
            memcpy(part, rest, k); part[k] = 0;
            if (!strcmp(part, "..")) { errno = ENOENT; return 0; }
            const char *next = end; while (*next == '/') next++;
            if (strcmp(part, ".") && !path_component(out, n, part, create && !*next, g_mnt[i].ro)) return 0;
            rest = end;
        }
        return 1;
    }
    errno = ENOENT; return 0;
}
static int host_path(const char *p, char *out, size_t n, int *ro) { return host_path_ex(p, out, n, ro, 0); }
static int set_guest_name(const char *hp, const char *path) {
    const char *end = path + strlen(path); while (end > path && end[-1] == '/') end--;
    const char *start = end; while (start > path && start[-1] != '/' && start[-1] != ':') start--;
    char guest[0x301]; size_t k = end - start;
    if (k >= sizeof guest) { errno = ENAMETOOLONG; return -1; }
    memcpy(guest, start, k); guest[k] = 0;
    const char *physical = strrchr(hp, '/') + 1;
    if (strcmp(physical, guest)) return setxattr(hp, FS_GUEST_NAME, guest, strlen(guest), 0, XATTR_NOFOLLOW);
    if (!removexattr(hp, FS_GUEST_NAME, XATTR_NOFOLLOW) || errno == ENOATTR) return 0;
    return -1;
}
static Result err_res(const char *op, const char *path) {
    if (errno == ENOENT || errno == ENOTDIR) { if (g_flag.fs) LOG("fs %s not found: %s", op, path); return RES_PATH_NOT_FOUND; }   // Logged under DM_FS=1 (the game often probes with "not found", not logged by default)
    if (errno == EEXIST) return RES_PATH_EXISTS;
    LOG("fs %s %s failed errno=%d", op, path, errno); return RES_FAIL;
}
// Name resolution shares one lock with create/rename/enumerate, aliases and xattr are published to the guest together.
#define HP_EX(path, create) PATH_LOCK(); char hp[1024]; int ro = 0; if (!host_path_ex(path, hp, sizeof hp, &ro, create)) return err_res("path", path)
#define HP(path) HP_EX(path, 0)
#define RW() if (ro) { LOG("fs write to a read-only mount: %s", hp); return RES_FAIL; }
// Create directory / create file: if the target exists, answer "already exists" as a writable mount would -- that is the only failure the game understands (the original save storage never fails to write); with a generic failure it carries on with a null handle
#define RW_CREATE() if (ro) { if (!access(hp, F_OK)) return RES_PATH_EXISTS; LOG("fs write to a read-only mount: %s", hp); return RES_FAIL; }

// ---------- Mount ----------
HLE_FORCE("_ZN2nn2fs22QueryMountRomCacheSizeEPm", "fsp-srv; rom reads the host directory directly, the cache gets only the minimum size", Result, h_fs_QueryRomCache, size_t *out) { *out = 0x1000; return RES_OK; }
HLE_FORCE("_ZN2nn2fs8MountRomEPKcPvm", "fsp-srv; rom → host RomFS directory", Result, h_fs_MountRom, const char *name, void *cache, size_t sz) {
    (void)cache; (void)sz; char b[1024];
    Result r = mount(name, envdir("DM_ROMFS", "romfs", b, sizeof b), 1);
    if (r == RES_OK && getenv("DM_ROMFS_OVERLAY")) {
        PATH_LOCK();
        for (int i = 0; i < g_nmnt; i++) if (!strcmp(g_mnt[i].name, name)) {
            strlcpy(g_mnt[i].overlay, getenv("DM_ROMFS_OVERLAY"), sizeof g_mnt[i].overlay); break;
        }
    }
    return r;
}
// Add-on content (DLC) RomFS: DM_AOC_DIR/<index>/, extracted from the player's DLC NSPs by the packing script; mounted read-only, using the directory index like rom.
// The owned index list is reported by the aoc calls in hle_misc.c from the same directory; the game only mounts indices it was given, and a missing directory answers "target not found" (fs 2-1002, as on hardware without that DLC)
#define RES_TARGET_NOT_FOUND 0x7D402u   // fs module 2, description 1002
// The cache a Query…CacheSize asks for is, on Horizon, the RomFS's file table as RomFsFileSystem::GetRequiredWorkingMemorySize sizes it: the directory and file hash tables (4 bytes a bucket)
// and entries (0x18 / 0x20 bytes plus the name, padded to 4). The host reads the files directly and needs no cache, but games take it from heaps sized for the real answer:
// Mario Kart 8 Deluxe mounts its add-on content (two files) with a cache from its 0x3130-byte AocHeap, and with a 4 KB answer the decryption buffer that follows came back null (main+0x87376c).
static size_t romfs_buckets(size_t n) {   // the hash table size of the RomFS builder: 3 below 3 entries, odd below 19, else the next number with no prime factor up to 17
    if (n < 3) return 3;
    if (n < 19) return n | 1;
    while (!(n % 2 && n % 3 && n % 5 && n % 7 && n % 11 && n % 13 && n % 17)) n++;
    return n;
}
static void romfs_tables(const char *dir, size_t *ndir, size_t *dbytes, size_t *nfile, size_t *fbytes, int depth) {
    DIR *d = opendir(dir); struct dirent *e; char hp[1280], g[256];
    if (!d) return;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || !guest_name(dir, e->d_name, g, sizeof g)) continue;
        size_t name = (strlen(g) + 3) & ~(size_t)3;
        if (e->d_type == DT_DIR) {
            ++*ndir; *dbytes += 0x18 + name;
            if (depth < 64 && snprintf(hp, sizeof hp, "%s/%s", dir, e->d_name) < (int)sizeof hp) romfs_tables(hp, ndir, dbytes, nfile, fbytes, depth + 1);
        } else { ++*nfile; *fbytes += 0x20 + name; }
    }
    closedir(d);
}
static size_t romfs_work_size(const char *dir) {
    size_t ndir = 1, dbytes = 0x18, nfile = 0, fbytes = 0;   // the root directory has an entry with an empty name
    romfs_tables(dir, &ndir, &dbytes, &nfile, &fbytes, 0);
    return 4 * romfs_buckets(ndir) + dbytes + 4 * romfs_buckets(nfile) + fbytes;
}
static int aoc_dir(char *dir, size_t n, int index) {
    const char *base = getenv("DM_AOC_DIR"); struct stat st;
    return base && *base && snprintf(dir, n, "%s/%d", base, index) < (int)n && !stat(dir, &st) && S_ISDIR(st.st_mode);
}
HLE_FORCE("_ZN2nn2fs31QueryMountAddOnContentCacheSizeEPmi", "fsp-srv; add-on content reads the host directory directly, the cache is the size of its RomFS file table", Result, h_fs_QueryAocCache, size_t *out, int index) {
    char dir[1024];
    *out = aoc_dir(dir, sizeof dir, index) ? romfs_work_size(dir) : 0x1000;   // an index the host does not have: the earlier minimum; the mount then fails
    return RES_OK;
}
HLE_FORCE("_ZN2nn2fs17MountAddOnContentEPKciPvm", "fsp-srv; add-on content → DM_AOC_DIR/<index> (read-only)", Result, h_fs_MountAoc, const char *name, int index, void *cache, size_t sz) {
    (void)cache; (void)sz; char dir[1024];
    if (!aoc_dir(dir, sizeof dir, index)) {
        LOG("fs mount add-on content %s: index %d does not exist (DM_AOC_DIR=%s)", name, index, getenv("DM_AOC_DIR") ? getenv("DM_AOC_DIR") : "unset");
        return RES_TARGET_NOT_FOUND;
    }
    return mount(name, dir, 1);   // mount_ex logs it
}
// System data archives are firmware content, which the host does not have. The one a game has needed so far is 0100000000000802, MiiModel (Mario Kart 8 Deluxe mounts it as MiiResource: and
// builds Mii heads from it at boot, for the title screen's demo race): the host writes a stand-in with every part empty (hle_mii.c dm_mii_standin) into <DM_TEMP>_sysdata/<id> and mounts it
// read-only. Any other archive answers "target not found", as for a title that is not installed
#define SYSDATA_MIIMODEL 0x0100000000000802ull
int dm_mii_standin(const char *dir);
static int sysdata_dir(char *dir, size_t n, uint64_t id) {
    char t[1024];
    if (id != SYSDATA_MIIMODEL || snprintf(dir, n, "%s_sysdata/%016llx", envdir("DM_TEMP", "run/platform/temp", t, sizeof t), (unsigned long long)id) >= (int)n) return 0;
    return dm_mii_standin(dir);
}
HLE_FORCE("_ZN2nn2fs29QueryMountSystemDataCacheSizeEPmNS_3ncm12SystemDataIdE", "fsp-srv; system data reads the host directory directly, the cache is the size of its RomFS file table", Result, h_fs_QuerySysDataCache, size_t *out, uint64_t id) {
    char dir[1024];
    *out = sysdata_dir(dir, sizeof dir, id) ? romfs_work_size(dir) : 0x1000;
    return RES_OK;
}
HLE_FORCE("_ZN2nn2fs15MountSystemDataEPKcNS_3ncm12SystemDataIdEPvm", "fsp-srv; system data: the host's MiiModel stand-in, nothing else", Result, h_fs_MountSysData, const char *name, uint64_t id, void *cache, size_t sz) {
    (void)cache; (void)sz; char dir[1024];
    if (!sysdata_dir(dir, sizeof dir, id)) { LOG("[RED] fs mount system data %s: the host has no %016llx", name, (unsigned long long)id); return RES_TARGET_NOT_FOUND; }
    return mount(name, dir, 1);
}
// IsMounted: the original SDK checks its own mount table, which stays empty because the host takes over mounting. Engage (IL2CPP) uses it to check whether "rom:" is mounted before opening a file; a "no" fails with an invalid-parameter error (0x57) and global-metadata.dat cannot be opened
HLE_FORCE("_ZN2nn2fs9IsMountedEPKc", "fsp-srv; looks up the host mount table", int, h_fs_IsMounted, const char *name) {   // bool in w0: 1 / 0
    PATH_LOCK();
    for (int i = 0; name && i < g_nmnt; i++) if (!strcmp(g_mnt[i].name, name)) return 1;
    return 0;
}
// IsPathMounted / GetPathLengthMax: the SDK's C library (nnmusl) checks every path with them before it calls nn::fs (sdk+0x8ef50, under open / stat / opendir), and the original SDK answers from
// its own empty mount table. Ace Attorney Investigations Collection (Unity 2022 IL2CPP): File.Exists → SystemNative_Stat → stat → open failed with no nn::fs call at all, so Addressables
// never found StreamingAssets/aa/settings.json and the game crashed on a null asset (main+0x31100ec; found by armyja, GitHub issue #18). The mount name is the part before ':', as in FindFileSystem
HLE_FORCE("_ZN2nn2fs13IsPathMountedEPKc", "fsp-srv; looks up the host mount table", int, h_fs_IsPathMounted, const char *path) {   // bool in w0: 1 / 0
    const char *c = path ? strchr(path, ':') : NULL;
    PATH_LOCK();
    for (int i = 0; c && i < g_nmnt; i++) if (strlen(g_mnt[i].name) == (size_t)(c - path) && !strncmp(g_mnt[i].name, path, c - path)) return 1;
    if (g_flag.fs) LOG("fs path not mounted: %s", path ? path : "(null)");
    return 0;
}
// The length limits nnmusl checks next (sdk+0x8f17c..0x8f254): Horizon's 0x300 bytes for paths and entry names, and no separate UTF-16 limit (false skips that check)
HLE_FORCE("_ZN2nn2fs16GetPathLengthMaxEPmPKc", "fsp-srv; every host mount takes Horizon's 0x300-byte paths", Result, h_fs_GetPathLengthMax, uint64_t *out, const char *path) {
    (void)path; *out = 0x300; return RES_OK;
}
HLE_FORCE("_ZN2nn2fs21GetEntryNameLengthMaxEPmPKc", "fsp-srv; every host mount takes Horizon's 0x300-byte entry names", Result, h_fs_GetEntryNameLengthMax, uint64_t *out, const char *path) {
    (void)path; *out = 0x300; return RES_OK;
}
HLE_FORCE("_ZN2nn2fs21GetUtf16PathLengthMaxEPbPmPKc", "fsp-srv; host mounts have no UTF-16 path limit", Result, h_fs_GetUtf16PathLengthMax, uint8_t *limited, uint64_t *out, const char *path) {
    (void)path; *limited = 0; *out = 0x300; return RES_OK;
}
HLE_FORCE("_ZN2nn2fs26GetUtf16EntryNameLengthMaxEPbPmPKc", "fsp-srv; host mounts have no UTF-16 entry name limit", Result, h_fs_GetUtf16EntryNameLengthMax, uint8_t *limited, uint64_t *out, const char *path) {
    (void)path; *limited = 0; *out = 0x300; return RES_OK;
}
HLE_FORCE("_ZN2nn2fs21MountTemporaryStorageEPKc", "fsp-srv; temporary storage → host directory", Result, h_fs_MountTemp, const char *name) {
    char b[1024]; return mount(name, envdir("DM_TEMP", "run/platform/temp", b, sizeof b), 0);
}
// Cache storage (Tears of the Kingdom; its first list call opened fsp-srv and aborted in sm::Initialize): storage <index> is the host folder <temporary storage>_cache<index>,
// created when it is created or mounted. The list reports none (a fresh console), so the game creates its storage at every boot; creating an existing one succeeds
static const char *cache_dir(int index, char *b, size_t n) { char t[1024]; snprintf(b, n, "%s_cache%d", envdir("DM_TEMP", "run/platform/temp", t, sizeof t), index); return b; }
HLE_FORCE("_ZN2nn2fs20OpenCacheStorageListEPNS0_22CacheStorageListHandleE", "fsp-srv; cache storage list: reports none", Result, h_fs_OpenCacheList, void *h) { (void)h; return RES_OK; }
HLE_FORCE("_ZN2nn2fs20ReadCacheStorageListEPiPNS0_16CacheStorageInfoENS0_22CacheStorageListHandleEi", "fsp-srv; cache storage list: reports none", Result, h_fs_ReadCacheList,
          int *n, void *info, uint64_t h, int max) { (void)info; (void)h; (void)max; *n = 0; return RES_OK; }
HLE_FORCE("_ZN2nn2fs21CloseCacheStorageListENS0_22CacheStorageListHandleE", "fsp-srv; cache storage list", void, h_fs_CloseCacheList, uint64_t h) { (void)h; }
HLE_FORCE("_ZN2nn2fs18CreateCacheStorageEill", "fsp-srv; cache storage → host directory", Result, h_fs_CreateCache, int index, int64_t size, int64_t journal) {
    char b[1100]; LOG("fs create cache storage %d (%lld bytes, journal %lld) → %s", index, (long long)size, (long long)journal, cache_dir(index, b, sizeof b)); mkdir(b, 0755); return RES_OK;
}
HLE_FORCE("_ZN2nn2fs17MountCacheStorageEPKci", "fsp-srv; cache storage → host directory", Result, h_fs_MountCache, const char *name, int index) {
    char b[1100]; cache_dir(index, b, sizeof b); mkdir(b, 0755); return mount(name, b, 0);
}
HLE_FORCE("_ZN2nn2fs18DeleteCacheStorageEi", "fsp-srv; cache storage → deletes the host directory", Result, h_fs_DeleteCache, int index) {
    char b[1100]; rm_tree(cache_dir(index, b, sizeof b)); return RES_OK;
}
HLE_FORCE("_ZN2nn2fs17MountCacheStorageEPKc", "fsp-srv; cache storage 0 → host directory (SDK 11 one-argument form, Monster Hunter Rise)", Result, h_fs_MountCache0, const char *name) { return h_fs_MountCache(name, 0); }
// Save data size (Monster Hunter Rise, SDK 11): the save directory has no size of its own, so it reports 256 MB of data and journal, more than a game asks for; extending succeeds without doing anything
HLE_FORCE("_ZN2nn2fs15GetSaveDataSizeEPlS1_RKNS_7account3UidE", "fsp-srv; save data size: 256 MB data + 256 MB journal", Result, h_fs_SaveSize, int64_t *size, int64_t *journal, const void *uid) {
    (void)uid; if (size) *size = 256 << 20; if (journal) *journal = 256 << 20; return RES_OK;
}
// The largest save data the game may extend to (Octopath Traveler 0, SDK 19: Unreal's save system asks before its first save, about 13 minutes in; the SDK's version read the fsp-srv
// object the host never made, SIGSEGV at GetSaveDataSizeMax+0xbc), the size of a cache storage and the free space of a mount: the same 256 MB the save data size reports, more than a game asks for
HLE_FORCE("_ZN2nn2fs18GetSaveDataSizeMaxEPlS1_", "fsp-srv; largest save data size: 256 MB data + 256 MB journal", Result, h_fs_SaveSizeMax, int64_t *size, int64_t *journal) {
    if (size) *size = 256 << 20; if (journal) *journal = 256 << 20; return RES_OK;
}
HLE_FORCE("_ZN2nn2fs19GetCacheStorageSizeEPlS1_i", "fsp-srv; cache storage size: 256 MB data + 256 MB journal", Result, h_fs_CacheSize, int64_t *size, int64_t *journal, int index) {
    (void)index; if (size) *size = 256 << 20; if (journal) *journal = 256 << 20; return RES_OK;
}
HLE_FORCE("_ZN2nn2fs16GetFreeSpaceSizeEPlPKc", "fsp-srv; free space of a mount: 256 MB (the host directory has no size limit of its own)", Result, h_fs_FreeSpace, int64_t *out, const char *path) {
    (void)path; if (out) *out = 256 << 20; return RES_OK;
}
HLE_FORCE("_ZN2nn2fs14ExtendSaveDataERKNS_7account3UidEll", "fsp-srv; extend save data: the host directory has no size limit", Result, h_fs_ExtendSave, const void *uid, int64_t size, int64_t journal) {
    (void)uid; LOG("fs extend save data to %lld bytes (journal %lld): nothing to do", (long long)size, (long long)journal); return RES_OK;
}
HLE_FORCE("_ZN2nn2fs14EnsureSaveDataERKNS_7account3UidE", "fsp-srv; the save directory is created at mount time", Result, h_fs_EnsureSave, const void *uid) { (void)uid; return RES_OK; }
// Another application's save data (Tears of the Kingdom asks by application ID and would mount it read-only): the host keeps only this game's own save, which exists once mounted
HLE_FORCE("_ZN2nn2fs18IsSaveDataExistingENS_13ApplicationIdERKNS_7account3UidE", "fsp-srv; only this game's own save data exists", int, h_fs_SaveExisting, uint64_t app, const void *uid) {
    (void)uid; LOG("fs asks whether save data exists: program %016llx → %s", (unsigned long long)app, app == DM_GAME_PROGRAM_ID ? "this game, yes" : "another program, no"); return app == DM_GAME_PROGRAM_ID;
}
HLE_FORCE("_ZN2nn2fs21MountSaveDataReadOnlyEPKcNS_13ApplicationIdERKNS_7account3UidE", "fsp-srv; another program's save data: none", Result, h_fs_MountSaveRO, const char *name, uint64_t app, const void *uid) {
    (void)uid; LOG("[RED] fs read-only mount of another program's save data %s: program %016llx, the host has none", name, (unsigned long long)app); return 0x7d402u;   // fs ResultTargetNotFound
}
HLE_FORCE("_ZN2nn2fs13MountSaveDataEPKcRKNS_7account3UidE", "fsp-srv; save data → host directory (single user)", Result, h_fs_MountSave, const char *name, const void *uid) {
    (void)uid;
    // No default directory: silently landing on a directory with no characters distorts the "character select" measurement (this once caused a misjudgment). Runs always use a fresh copy of save_ref
    const char *d = getenv("DM_SAVE");
    if (!d) die("DM_SAVE is not set: point it at a writable copy of a save directory");
    char dd[1024]; strlcpy(dd, d, sizeof dd); size_t L = strlen(dd); while (L > 1 && dd[L - 1] == '/') dd[--L] = 0;   // strip trailing slashes (.work / .new are appended at the same level)
    return mount_ex(name, dd, 0, 1);
}
// Device save data: one save for the console instead of one per user. Animal Crossing: New Horizons keeps its island there (3.0.3's main mounts no account save data at all); the host
// has one player, so it is the host save directory with its transaction, as the account save. Its size and extension follow the account save's.
// A game that mounts both is given two saves, as on Horizon: the device save is <DM_SAVE>_device, a transaction of its own. Mario Kart 8 Deluxe mounts CommonSave (device) and then
// UserSave (account, main+0x8b0c84), and at boot its save manager deletes CommonSave:/userdata.dat (main+0x8b1910, through its file device's DeleteFile); in one shared directory that
// was UserSave's own userdata.dat, so every boot started a new save. Decided by main's imports: of the games on hand only this one imports both, and ACNH's islands stay where they are
HLE_FORCE("_ZN2nn2fs19MountDeviceSaveDataEPKc", "fsp-srv; device save data → the host save directory (one player, as the account save), or a save of its own when main mounts account save data too", Result, h_fs_MountDeviceSave, const char *name) {
    if (!dm_main_imports("_ZN2nn2fs13MountSaveData")) return h_fs_MountSave(name, NULL);
    const char *d = getenv("DM_SAVE");
    if (!d) die("DM_SAVE is not set: point it at a writable copy of a save directory");
    char dd[1024]; strlcpy(dd, d, sizeof dd); size_t L = strlen(dd); while (L > 1 && dd[L - 1] == '/') dd[--L] = 0;
    strlcat(dd, "_device", sizeof dd);
    return mount_ex(name, dd, 0, 1);
}
HLE_FORCE("_ZN2nn2fs21GetDeviceSaveDataSizeEPlS1_", "fsp-srv; device save data size: 256 MB data + 256 MB journal", Result, h_fs_DeviceSaveSize, int64_t *size, int64_t *journal) { return h_fs_SaveSize(size, journal, NULL); }
HLE_FORCE("_ZN2nn2fs20ExtendDeviceSaveDataEll", "fsp-srv; extend device save data: the host directory has no size limit", Result, h_fs_ExtendDeviceSave, int64_t size, int64_t journal) { return h_fs_ExtendSave(NULL, size, journal); }
// The restore flag marks save data the system restored from a backup (cloud save data backup), which a game may check once and then clear with the commit option below; the host never restores
HLE_FORCE("_ZN2nn2fs22GetSaveDataRestoreFlagEPbPKc", "fsp-srv; save data restored from a backup: never", Result, h_fs_RestoreFlag, uint8_t *out, const char *name) { (void)name; *out = 0; return RES_OK; }
static _Atomic uint64_t g_save_commits;   // number of successful commits (used by the front end when going to background to wait for "the game's current save has been committed")
uint64_t dm_fs_commit_count(void) { return atomic_load(&g_save_commits); }
// Both Commit overloads share this transaction engine. Caller holds the path
// and mount locks; front-end failure notification is delivered after unlocking.
static Result commit_mount_locked(int i, int *notify_error) {
    uint64_t tc1 = clock_gettime_nsec_np(CLOCK_MONOTONIC), tw0 = atomic_exchange(&g_txn_first_ns, 0);
    int perr = atomic_exchange(&g_save_poison, 0);
    if (perr) {                                                         // a write failed in this transaction: refuse the commit, discard the working directory
        char torn[512] = ""; size_t tl = 0; DIR *d0 = opendir(g_mnt[i].dir); struct dirent *e0;   // record files truncated to 0 bytes (the damage that would enter the save if committed as usual)
        while (d0 && (e0 = readdir(d0))) {
            if (e0->d_name[0] == '.') continue;
            char sub[1300]; snprintf(sub, sizeof sub, "%s/%s", g_mnt[i].dir, e0->d_name); DIR *d1 = opendir(sub); struct dirent *e1;
            while (d1 && (e1 = readdir(d1))) { char f[1600]; struct stat st; snprintf(f, sizeof f, "%s/%s", sub, e1->d_name);
                if (e1->d_name[0] != '.' && !stat(f, &st) && S_ISREG(st.st_mode) && !st.st_size && tl + strlen(e0->d_name) + strlen(e1->d_name) + 3 < sizeof torn)
                    tl += snprintf(torn + tl, sizeof torn - tl, " %s/%s", e0->d_name, e1->d_name); }
            if (d1) closedir(d1);
        }
        if (d0) closedir(d0);
        int nopen = 0; for (int k = 0; k < 65536; k++) nopen += g_fsave[k];   // save files still open: after re-cloning, these handles point into the deleted old working directory
        // First clone the committed version into a sibling temporary directory; once that succeeds, atomically swap it with the tainted work directory and delete the swapped-out one. If any step fails, do not exit: the work directory stays and the "tainted" mark is put back
        // (the next commit is refused again and recloning is retried); the committed version is unchanged (before, the work directory was deleted first and then recloned, and a failed clone called die, so with a full disk the process exited and the work directory was gone)
        char re[1100]; snprintf(re, sizeof re, "%s.re", g_mnt[i].dir); int rerr = 0;
        if (!access(re, F_OK) && rm_tree(re)) rerr = errno ? errno : EIO;
        else if (dm_fs_test_fail("reclone") || clonefile(g_mnt[i].commit, re, 0)) { rerr = errno ? errno : EIO; rm_tree(re); }
        else if (renamex_np(re, g_mnt[i].dir, RENAME_SWAP)) { rerr = errno ? errno : EIO; rm_tree(re); }
        else rm_tree(re);                                               // the swapped-out tainted work directory
        if (rerr) { save_poison(perr); LOG("[RED] save transaction: recloning the work directory failed errno=%d -- the work directory is left as it is and stays tainted (the next commit is refused again and retries), the committed version is unchanged", rerr); }
        LOG("[RED] save commit refused: a save write failed in this transaction errno=%d (0-byte files in the work directory: %s); the committed version is unchanged, the work directory %s%s", perr, tl ? torn + 1 : "none", rerr ? "could not be recloned (see the previous line)" : "was recloned from the committed version", nopen ? "; [RED] save files are still open" : "");
        *notify_error = perr;
        return RES_FAIL;
    }
    char neu[1100]; snprintf(neu, sizeof neu, "%s.new", g_mnt[i].commit);
    Result r = RES_OK;
    if (!access(neu, F_OK)) rm_tree(neu);
    if (clonefile(g_mnt[i].dir, neu, 0)) { LOG("[RED] save commit: clonefile %s failed errno=%d (committed version unchanged)", neu, errno); r = RES_FAIL; }
    else if (renamex_np(neu, g_mnt[i].commit, RENAME_SWAP)) { LOG("[RED] save commit: atomic swap failed errno=%d (committed version unchanged)", errno); rm_tree(neu); r = RES_FAIL; }
    else rm_tree(neu);                                                  // the old version that was swapped out
    // Timing readouts (integer microseconds, no floating point): how long the clone + atomic swap took; how long in total from this transaction's first save write to the end of the commit (the game writing files + the commit; 0 = no save write in this transaction)
    static int n; if (r == RES_OK) { uint64_t tc2 = clock_gettime_nsec_np(CLOCK_MONOTONIC);
        LOG("save commit #%d: %s swapped atomically into %s (%d save writes so far) (clone + swap %llu us, since this transaction's first write %llu us)", ++n, g_mnt[i].dir, g_mnt[i].commit, atomic_load(&g_save_writes),
            (unsigned long long)((tc2 - tc1) / 1000), (unsigned long long)(tw0 ? (tc2 - tw0) / 1000 : 0)); atomic_fetch_add(&g_save_commits, 1);
    }
    return r;
}
HLE_FORCE("_ZN2nn2fs6CommitEPKc", "fsp-srv; transactional save: the whole work directory tree is cloned, then swapped atomically with the committed directory (renamex_np RENAME_SWAP)", Result, h_fs_Commit, const char *name) {
    PATH_LOCK(); pthread_mutex_lock(&g_lock);
    int i = 0; while (i < g_nmnt && strcmp(g_mnt[i].name, name)) i++;
    if (i == g_nmnt || !g_mnt[i].commit[0]) { pthread_mutex_unlock(&g_lock); return RES_OK; }   // non-transactional mount: nothing to commit
    int notify_error = 0; Result r = commit_mount_locked(i, &notify_error);
    pthread_mutex_unlock(&g_lock);
    if (notify_error) front_save_failed(notify_error);
    return r;
}
// A mount list is atomic only when every name addresses the same host backing
// transaction. Validate the whole list first. Independent save trees cannot be
// swapped atomically together, so refuse them without publishing either tree.
// Writable mounts without a transaction (cache / temporary storage, journaled on
// Horizon) are already on disk: skipped, as the single-name Commit returns OK.
HLE_FORCE("_ZN2nn2fs6CommitEPKPKci", "fsp-srv; commit a validated mount list sharing one host save transaction; independent multi-save commits are unsupported", Result, h_fs_CommitList, const char *const *names, int count) {
    if (count < 0 || count > MAX_MOUNT || (count && !names)) return RES_FAIL;
    if (!count) return RES_OK;
    PATH_LOCK(); pthread_mutex_lock(&g_lock);
    int target = -1, notify_error = 0; Result r = RES_FAIL;
    for (int k = 0; k < count; k++) {
        if (!names[k] || strnlen(names[k], sizeof g_mnt[0].name) == sizeof g_mnt[0].name) goto out;
        int i = 0; while (i < g_nmnt && strcmp(g_mnt[i].name, names[k])) i++;
        if (i == g_nmnt || g_mnt[i].ro) goto out;
        if (!g_mnt[i].commit[0]) continue;
        if (target >= 0 && (strcmp(g_mnt[target].commit, g_mnt[i].commit) || strcmp(g_mnt[target].dir, g_mnt[i].dir))) {
            LOG("[RED] Commit mount list spans independent host saves; refused without a partial commit");
            goto out;
        }
        if (target < 0) target = i;
    }
    r = target < 0 ? RES_OK : commit_mount_locked(target, &notify_error);
out:
    pthread_mutex_unlock(&g_lock);
    if (notify_error) front_save_failed(notify_error);
    return r;
}
// The overload with a CommitOption (Animal Crossing: New Horizons 3.0.3): its only flag clears the restore flag, which the host never sets
HLE_FORCE("_ZN2nn2fs6CommitEPKcRKNS0_12CommitOptionE", "fsp-srv; same as Commit (the option only clears the restore flag)", Result, h_fs_CommitOpt, const char *name, const void *opt) { (void)opt; return h_fs_Commit(name); }
// SDK 11's old name: CommitSaveData(mount name) is the later Commit (Metroid Dread)
HLE_FORCE("_ZN2nn2fs14CommitSaveDataEPKc", "fsp-srv; same as Commit (SDK 11's old name)", Result, h_fs_CommitSaveData, const char *name) { return h_fs_Commit(name); }
HLE_FORCE("_ZN2nn2fs7UnmountEPKc", "fsp-srv; the mount table is permanent, unmounting is a no-op", void, h_fs_Unmount, const char *name) { (void)name; }

// ---------- Files ----------
HLE_FORCE("_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci", "fsp-srv → open(2)", Result, h_fs_OpenFile, uint64_t *out, const char *path, int mode) {
    HP(path);
    if (mode & (MODE_WRITE | MODE_APPEND)) RW();
    int fd = open(hp, (mode & (MODE_WRITE | MODE_APPEND)) ? ((mode & MODE_READ) ? O_RDWR : O_WRONLY) : O_RDONLY);
    if (fd < 0) return err_res("open", hp);
    struct stat st; fstat(fd, &st);
    if (S_ISDIR(st.st_mode)) { close(fd); return RES_PATH_NOT_FOUND; }
    if (fd >= (int)sizeof g_fmode) die("fd beyond the table");
    g_fmode[fd] = (uint8_t)mode; *out = (uint64_t)fd;
    { const char *c = strchr(path, ':'); size_t ln = c ? (size_t)(c - path) : 0; g_fsave[fd] = 0;
      for (int i = 0; c && i < g_nmnt; i++) if (strlen(g_mnt[i].name) == ln && !strncmp(g_mnt[i].name, path, ln)) g_fsave[fd] = g_mnt[i].commit[0] != 0; }
    fslog_init();
    if (g_fslog && fd < 4096) { pthread_mutex_lock(&g_fslog_lock); free(g_fdpath[fd]); g_fdpath[fd] = strdup(path); pthread_mutex_unlock(&g_fslog_lock); }
    if (g_flag.fs) LOG("fs open [%.2fs] %s mode=%d → fd %d", (now_ns() - g_t0_ns) / 1e9, path, mode, fd);
    return RES_OK;
}
HLE_FORCE("_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm", "fsp-srv → pread(2)", Result, h_fs_ReadFile, size_t *out, uint64_t h, int64_t off, void *buf, size_t sz) {
    if (!h) { *out = 0; return RES_FAIL; }   // a file handle is an fd; 0 is stdin and never a file the game opened: the null handle the game keeps using after a failed open
    size_t got = 0; int reprep = 0;
    dm_guest_write_prepare(buf, sz);   // The destination may be a texture pool page write-protected by the graphics layer: a kernel write to a read-only page returns EFAULT instead of page-faulting, so unprotect and mark dirty first
    while (got < sz) {
        ssize_t r = pread((int)h, (uint8_t *)buf + got, sz - got, off + got);
        if (r < 0) { if (errno == EINTR) continue;
            // The graphics layer write-protects tracked pages again after each upload (nvn_dirty.c), so a page made writable above can be read-only again by the time pread
            // writes it: a plain write would fault into the tracker's handler, a system call gets EFAULT. Prepare the rest again (unprotect, mark dirty) and read on. Monster Train 2
            // at 60 fps lost one read this way in two of three runs on the Mac (0x2c000 bytes of pool pages, protection read-only), never at 30 fps, where uploads come half as often
            if (errno == EFAULT && reprep++ < 8) {
                static _Atomic int once; if (!once++) LOG("[fs] ReadFile into %p+%#zx: the destination was write-protected again during the read, prepared again at +%#zx (first case)", buf, sz, got);
                dm_guest_write_prepare((uint8_t *)buf + got, sz - got); continue; }
            if (errno == EFAULT) {   // the destination is not writable host memory: say where, with the protection of its VM region
                int e = errno; vm_address_t ra = (vm_address_t)((uint8_t *)buf + got); vm_size_t rs = 0; vm_region_basic_info_data_64_t vi; mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t obj = MACH_PORT_NULL;
                kern_return_t kr = vm_region_64(mach_task_self(), &ra, &rs, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&vi, &cnt, &obj);
                LOG("[RED] fs ReadFile into %p+%#zx failed at +%#zx (EFAULT): VM region %#lx+%#lx protection %d%s", buf, sz, got, (unsigned long)ra, (unsigned long)rs, kr == KERN_SUCCESS ? vi.protection : -1, kr == KERN_SUCCESS ? "" : " (none)");
                errno = e; }
            return err_res("pread", "fd"); }
        if (r == 0) break;
        got += r;
    }
    *out = got;
    if (g_fslog) { pthread_mutex_lock(&g_fslog_lock);
        static int hash_on = -1; if (hash_on < 0) hash_on = getenv("DM_FSLOG_HASH") != NULL;   // DM_FSLOG_HASH=1: append a column with the FNV-1a 64 of the content read (to compare whether the bytes read from two resource packs match)
        uint64_t fh = 0xcbf29ce484222325ull; if (hash_on) for (size_t i = 0; i < got; i++) fh = (fh ^ ((uint8_t *)buf)[i]) * 0x100000001b3ull;
        if (hash_on) fprintf(g_fslog, "%.4f\t%s\t%lld\t%zu\t%p\t%016llx\n", (now_ns() - g_t0_ns) / 1e9, (int)h < 4096 && g_fdpath[(int)h] ? g_fdpath[(int)h] : "?", (long long)off, got, buf, (unsigned long long)fh);
        else fprintf(g_fslog, "%.4f\t%s\t%lld\t%zu\t%p\n", (now_ns() - g_t0_ns) / 1e9, (int)h < 4096 && g_fdpath[(int)h] ? g_fdpath[(int)h] : "?", (long long)off, got, buf);
        pthread_mutex_unlock(&g_fslog_lock); }
    if (g_flag.fs) {   // Read metering: print a line every 64 MiB accumulated or every 1000 reads
        static uint64_t n, bytes, next_b = 64 << 20; static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
        pthread_mutex_lock(&m); n++; bytes += got;
        if (bytes >= next_b || n % 1000 == 0) { LOG("fs read total: %llu reads %.1f MiB (this one: fd %d offset %#llx length %#zx)", n, bytes / 1048576.0, (int)h, off, got); if (bytes >= next_b) next_b += 64 << 20; }
        pthread_mutex_unlock(&m);
    }
    return RES_OK;
}
HLE_FORCE("_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE", "fsp-srv → pwrite(2)", Result, h_fs_WriteFile, uint64_t h, int64_t off, const void *buf, size_t sz, const void *opt) {
    if (!h) return RES_FAIL;
    (void)opt; size_t put = 0;
    if (g_fsave[(int)h & 0xffff]) {                                     // Save write injection point (for deterministic regression; only writes on the save mount are counted)
        static int cfg = -1, kill_n, fail_n, fail_e;
        if (cfg < 0) { const char *k = getenv("DM_TEST_KILL_AT"), *f = getenv("DM_TEST_FS_FAIL");
            kill_n = k && !strncmp(k, "write:", 6) ? atoi(k + 6) : 0;
            if (f && !strncmp(f, "write:", 6)) { fail_n = atoi(f + 6); const char *c = strchr(f + 6, ':'); fail_e = c ? atoi(c + 1) : ENOSPC; }
            cfg = 1; }
        int n = atomic_fetch_add(&g_save_writes, 1) + 1;
        { uint64_t z = 0; atomic_compare_exchange_strong(&g_txn_first_ns, &z, clock_gettime_nsec_np(CLOCK_MONOTONIC)); }
        if (kill_n && n == kill_n) { LOG("DM_TEST_KILL_AT: save write #%d, calling _exit (simulates being killed mid-write)", n); _exit(9); }
        if (fail_n && n == fail_n) { LOG("DM_TEST_FS_FAIL: save write #%d returns failure errno=%d", n, fail_e); errno = fail_e; save_poison(fail_e); return err_res("pwrite (injected)", "fd"); }
    }
    while (put < sz) {
        ssize_t r = pwrite((int)h, (const uint8_t *)buf + put, sz - put, off + put);
        if (r < 0) { if (errno == EINTR) continue; if (g_fsave[(int)h & 0xffff]) save_poison(errno); return err_res("pwrite", "fd"); }
        if (r == 0) { errno = ENOSPC; if (g_fsave[(int)h & 0xffff]) save_poison(ENOSPC); return err_res("pwrite (wrote 0 bytes)", "fd"); }   // This used to loop forever here
        put += r;
    }
    return RES_OK;
}
HLE_FORCE("_ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE", "fsp-srv → fstat(2)", Result, h_fs_GetFileSize, int64_t *out, uint64_t h) {
    if (!h) return RES_FAIL;
    struct stat st; if (fstat((int)h, &st)) return err_res("fstat", "fd");
    *out = st.st_size; return RES_OK;
}
HLE_FORCE("_ZN2nn2fs11SetFileSizeENS0_10FileHandleEl", "fsp-srv → ftruncate(2)", Result, h_fs_SetFileSize, uint64_t h, int64_t sz) {
    if (!h) return RES_FAIL;
    if (!ftruncate((int)h, sz)) return RES_OK;
    if (g_fsave[(int)h & 0xffff]) save_poison(errno);
    return err_res("ftruncate", "fd");
}
HLE_FORCE("_ZN2nn2fs9FlushFileENS0_10FileHandleE", "fsp-srv; host writes are already in the page cache, flushing is a no-op", Result, h_fs_FlushFile, uint64_t h) { (void)h; return RES_OK; }
HLE_FORCE("_ZN2nn2fs9CloseFileENS0_10FileHandleE", "fsp-srv → close(2)", void, h_fs_CloseFile, uint64_t h) {
    if (!h) return;   // the null handle after a failed open (see ReadFile): do not touch fd 0
    g_fmode[(int)h & 0xffff] = 0; g_fsave[(int)h & 0xffff] = 0;
    close((int)h); }
HLE_FORCE("_ZN2nn2fs15GetFileOpenModeENS0_10FileHandleE", "fsp-srv; the open mode is recorded by the host", int, h_fs_GetFileOpenMode, uint64_t h) { return g_fmode[(int)h & 0xffff]; }

// ---------- Path operations ----------
HLE_FORCE("_ZN2nn2fs10CreateFileEPKcl", "fsp-srv → open(O_CREAT|O_EXCL)+ftruncate", Result, h_fs_CreateFile, const char *path, int64_t sz) {
    HP_EX(path, 1); RW_CREATE();
    int fd = open(hp, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) return err_res("create", hp);
    int rc = ftruncate(fd, sz); int e = errno; close(fd);
    if (!rc && set_guest_name(hp, path)) { rc = -1; e = errno; }
    if (rc) unlink(hp);
    errno = e; return rc ? err_res("create", hp) : RES_OK;
}
HLE_FORCE("_ZN2nn2fs10DeleteFileEPKc", "fsp-srv → unlink(2)", Result, h_fs_DeleteFile, const char *path) { HP(path); RW(); return unlink(hp) ? err_res("unlink", hp) : RES_OK; }
HLE_FORCE("_ZN2nn2fs15CreateDirectoryEPKc", "fsp-srv → mkdir(2)", Result, h_fs_CreateDirectory, const char *path) { HP_EX(path, 1); RW_CREATE();
    if (mkdir(hp, 0755)) return err_res("mkdir", hp);
    if (set_guest_name(hp, path)) { int e = errno; rmdir(hp); errno = e; return err_res("mkdir name", hp); }
    return RES_OK; }
static int rm_tree(const char *p) {
    DIR *d = opendir(p); if (!d) return -1;
    struct dirent *e; char c[1024]; int rc = 0;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(c, sizeof c, "%s/%s", p, e->d_name);
        rc |= e->d_type == DT_DIR ? rm_tree(c) : unlink(c);
    }
    closedir(d);
    return rc | rmdir(p);
}
HLE_FORCE("_ZN2nn2fs26DeleteDirectoryRecursivelyEPKc", "fsp-srv → recursive delete (only inside writable mounts)", Result, h_fs_DeleteDirRec, const char *path) {
    HP(path); RW(); return rm_tree(hp) ? err_res("rmtree", hp) : RES_OK;
}
// empty a directory and keep the directory itself (13 Sentinels)
HLE_FORCE("_ZN2nn2fs25CleanDirectoryRecursivelyEPKc", "fsp-srv → recursively deletes the directory's contents and keeps the directory (only inside writable mounts)", Result, h_fs_CleanDirRec, const char *path) {
    HP(path); RW();
    DIR *d = opendir(hp); if (!d) return err_res("cleandir", hp);
    struct dirent *e; char c[1024]; int rc = 0;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(c, sizeof c, "%s/%s", hp, e->d_name);
        rc |= e->d_type == DT_DIR ? rm_tree(c) : unlink(c);
    }
    closedir(d);
    return rc ? err_res("cleandir", hp) : RES_OK;
}
// file data cache (a global read cache in the fs process): the host reads through the page cache and leaves the buffer unused
HLE_FORCE("_ZN2nn2fs25EnableGlobalFileDataCacheEPvm", "fsp-srv global read cache; unused by the host", void, h_fs_EnableGlobalCache, void *buf, size_t sz) { (void)buf; (void)sz; }
// the same cache for one file (SDK 11, Big Brain Academy: Brain vs. Brain; the game does not read the result)
HLE_FORCE("_ZN2nn2fs29EnableIndividualFileDataCacheEPKcPvm", "fsp-srv read cache for one file; unused by the host", Result, h_fs_EnableFileCache, const char *path, void *buf, size_t sz) {
    (void)path; (void)buf; (void)sz; return RES_OK;
}
HLE_FORCE("_ZN2nn2fs30DisableIndividualFileDataCacheEPKc", "fsp-srv read cache for one file; unused by the host", void, h_fs_DisableFileCache, const char *path) { (void)path; }
HLE_FORCE("_ZN2nn2fs26DisableGlobalFileDataCacheEv", "fsp-srv global read cache; unused by the host (DRAGON QUEST XI S's nninitStartup turns it off again after its start-up thread)", void, h_fs_DisableGlobalCache, void) {}
// SDK 11 (Metroid Dread): delete an empty directory
HLE_FORCE("_ZN2nn2fs15DeleteDirectoryEPKc", "fsp-srv → rmdir(2) (the directory must be empty)", Result, h_fs_DeleteDir, const char *path) {
    HP(path); RW(); return rmdir(hp) ? err_res("rmdir", hp) : RES_OK;
}
static Result rename_entry(const char *from, const char *to) {
    HP(from); RW(); char hp2[1024];
    if (!host_path_ex(to, hp2, sizeof hp2, NULL, 1)) return err_res("rename path", to);
    if (!access(hp2, F_OK)) return RES_PATH_EXISTS;
    if (rename(hp, hp2)) return err_res("rename", hp);
    if (dm_fs_test_fail("guestname") || set_guest_name(hp2, to)) { int e = errno ? errno : EIO;
        if (dm_fs_test_fail("renameback") || rename(hp2, hp)) {   // the rollback fails too: the file stays at the new path with wrong guest-name metadata; do not exit -- inside a transactional save, this transaction's commit is refused (committed version unchanged; recloning restores it)
            LOG("[RED] fs rename: writing the name metadata failed and so did the rollback errno=%d: %s stays at the new path%s", errno, hp2, in_txn_dir(hp2) ? ", this transaction's save commit will be refused" : ""); if (in_txn_dir(hp2)) save_poison(e); }
        errno = e; return err_res("rename name", hp2); }
    return RES_OK;
}
HLE_FORCE("_ZN2nn2fs10RenameFileEPKcS2_", "fsp-srv → rename(2)", Result, h_fs_RenameFile, const char *from, const char *to) { return rename_entry(from, to); }
// The Great Ace Attorney Chronicles (SDK 10) imports it; a directory carries its guest name the same way as a file
HLE_FORCE("_ZN2nn2fs15RenameDirectoryEPKcS2_", "fsp-srv → rename(2) of a directory", Result, h_fs_RenameDir, const char *from, const char *to) { return rename_entry(from, to); }
HLE_FORCE("_ZN2nn2fs12GetEntryTypeEPNS0_18DirectoryEntryTypeEPKc", "fsp-srv → stat(2)", Result, h_fs_GetEntryType, int32_t *out, const char *path) {
    HP(path); struct stat st;
    if (stat(hp, &st)) return err_res("stat", hp);
    *out = S_ISDIR(st.st_mode) ? ENT_DIR : ENT_FILE; return RES_OK;
}

// ---------- Directories ----------
typedef struct { DIR *d; char path[1024]; DIR *od; char opath[1024]; DirectoryEntry *snap; int64_t nsnap, psnap; int mode; } dirh_t;
static void dir_snapshot(dirh_t *h, const mount_t *m, const char *rest) {
    char *roots = strdup(m->overlay);
    char (*names)[0x301] = calloc(2048, sizeof *names);
    const char **all = calloc(65, sizeof *all);
    char *parent = malloc(1024), *guest = malloc(0x301), *rel = malloc(2048), *resolved = malloc(1024);
    if (!roots || !names || !all || !parent || !guest || !rel || !resolved) die("rom overlay directory snapshot allocation failed");
    int nn = 0, nr = 0;
    all[nr++] = m->dir;
    char *save = NULL;
    for (char *root = strtok_r(roots, ":", &save); root && nr < 65; root = strtok_r(NULL, ":", &save)) all[nr++] = root;
    for (int ri = nr - 1; ri >= 0; ri--) {
        if (!root_path(all[ri], rest, parent, 1024)) continue;
        DIR *x = opendir(parent); if (!x) continue;
        struct dirent *e;
        while ((e = readdir(x))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            if (!guest_name(parent, e->d_name, guest, 0x301)) continue;
            int dup = 0; for (int j = 0; j < nn; j++) if (!strcmp(names[j], guest)) { dup = 1; break; }
            if (!dup && nn < 2048) strlcpy(names[nn++], guest, sizeof names[0]);
        }
        closedir(x);
    }
    h->snap = calloc(nn, sizeof *h->snap); h->nsnap = nn;
    if (nn && !h->snap) die("rom overlay directory snapshot allocation failed");
    for (int j = 0; j < nn; j++) {
        strlcpy(h->snap[j].name, names[j], sizeof h->snap[j].name);
        snprintf(rel, 2048, "%s/%s", rest, names[j]);
        if (!rom_overlay_path(m, rel, resolved, 1024)) continue;
        struct stat st;
        if (!stat(resolved, &st)) { h->snap[j].type = S_ISDIR(st.st_mode) ? ENT_DIR : ENT_FILE; h->snap[j].size = S_ISDIR(st.st_mode) ? 0 : st.st_size; }
    }
    free(resolved); free(rel); free(guest); free(parent); free(all); free(names); free(roots);
}
HLE_FORCE("_ZN2nn2fs13OpenDirectoryEPNS0_15DirectoryHandleEPKci", "fsp-srv → opendir(3)", Result, h_fs_OpenDirectory, uint64_t *out, const char *path, int mode) {
    HP(path); DIR *d = opendir(hp);
    if (!d) return err_res("opendir", hp);
    dirh_t *h = calloc(1, sizeof *h); h->d = d; strlcpy(h->path, hp, sizeof h->path); h->mode = mode;
    const char *c = strchr(path, ':');
    if (c) for (int i = 0; i < g_nmnt; i++) if (g_mnt[i].overlay[0] && strlen(g_mnt[i].name) == (size_t)(c - path) && !strncmp(g_mnt[i].name, path, (size_t)(c - path))) {
        dir_snapshot(h, &g_mnt[i], c + 1); break;
    }
    *out = (uint64_t)h; return RES_OK;
}
HLE_FORCE("_ZN2nn2fs13ReadDirectoryEPlPNS0_14DirectoryEntryENS0_15DirectoryHandleEl", "fsp-srv → readdir(3)", Result, h_fs_ReadDirectory, int64_t *count, DirectoryEntry *ents, uint64_t hh, int64_t max) {
    if (!hh) { *count = 0; return RES_FAIL; }
    PATH_LOCK(); dirh_t *h = (dirh_t *)hh; int64_t n = 0; struct dirent *e;
    if (h->snap) { while (n < max && h->psnap < h->nsnap) { DirectoryEntry *src = &h->snap[h->psnap++]; if (!(h->mode & (src->type == ENT_DIR ? DIRMODE_DIR : DIRMODE_FILE))) continue; ents[n++] = *src; } *count = n; return RES_OK; }
    int dfd = dirfd(h->d);
    while (n < max && (e = readdir(h->d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        // a directory's type comes from d_type without stat; files need a size, taken with fstatat relative to the directory fd (following links, like the old stat)
        struct stat st; int isdir;
        if (e->d_type == DT_DIR) isdir = 1;
        else { if (fstatat(dfd, e->d_name, &st, 0)) continue; isdir = S_ISDIR(st.st_mode); }
        if (!(h->mode & (isdir ? DIRMODE_DIR : DIRMODE_FILE))) continue;
        DirectoryEntry *o = &ents[n++]; memset(o, 0, sizeof *o);
        if (!guest_name(h->path, e->d_name, o->name, sizeof o->name)) { char c[1280]; snprintf(c, sizeof c, "%s/%s", h->path, e->d_name); *count = n - 1; return err_res("directory name", c); }
        o->type = isdir ? ENT_DIR : ENT_FILE; o->size = isdir ? 0 : st.st_size;
    }
    *count = n; return RES_OK;
}
HLE_FORCE("_ZN2nn2fs14CloseDirectoryENS0_15DirectoryHandleE", "fsp-srv → closedir(3)", void, h_fs_CloseDirectory, uint64_t hh) {
    if (!hh) return;   // the game also closes after a failed open (handle 0): nothing to do in the original either
    dirh_t *h = (dirh_t *)hh; closedir(h->d); if (h->od) closedir(h->od); free(h->snap); free(h);
}

// ---------- Overloads from newer SDKs (SDK 16: MountRom without a cache, ReadFile without the bytes-read output / with read options, directory entry count) ----------
HLE_FORCE("_ZN2nn2fs8MountRomEPKc", "fsp-srv; rom → host RomFS directory (overload without the cache arguments)", Result, h_fs_MountRom1, const char *name) { return h_fs_MountRom(name, NULL, 0); }
// ReadFile without the bytes-read output: the read must be complete; a short read (past the end of file) is returned as a failure
HLE_FORCE("_ZN2nn2fs8ReadFileENS0_10FileHandleElPvm", "fsp-srv → pread(2), must read in full", Result, h_fs_ReadFileExact, uint64_t h, int64_t off, void *buf, size_t sz) {
    size_t got = 0; Result r = h_fs_ReadFile(&got, h, off, buf, sz);
    if (r != RES_OK) return r;
    if (got != sz) { LOG("fs ReadFile short read: fd %d offset %lld wanted %zu got %zu", (int)h, (long long)off, sz, got); return RES_FAIL; }
    return RES_OK;
}
HLE_FORCE("_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvmRKNS0_10ReadOptionE", "fsp-srv → pread(2) (overload with read options, which the host ignores)", Result, h_fs_ReadFileOpt,
          size_t *out, uint64_t h, int64_t off, void *buf, size_t sz, const void *opt) { (void)opt; return h_fs_ReadFile(out, h, off, buf, sz); }
HLE_FORCE("_ZN2nn2fs22GetDirectoryEntryCountEPlNS0_15DirectoryHandleE", "fsp-srv; directory entry count (filtered by the mode given at open, read position untouched)", Result, h_fs_DirCount, int64_t *out, uint64_t hh) {
    PATH_LOCK(); dirh_t *h = (dirh_t *)hh; int64_t n = 0;
    if (h->snap) { for (int64_t i = 0; i < h->nsnap; i++) if (h->mode & (h->snap[i].type == ENT_DIR ? DIRMODE_DIR : DIRMODE_FILE)) n++; *out = n; return RES_OK; }
    DIR *d = opendir(h->path); if (!d) return err_res("opendir", h->path);
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char c[1280]; snprintf(c, sizeof c, "%s/%s", h->path, e->d_name);
        struct stat st; if (stat(c, &st)) continue;
        if (h->mode & (S_ISDIR(st.st_mode) ? DIRMODE_DIR : DIRMODE_FILE)) n++;
    }
    closedir(d); *out = n; return RES_OK;
}
// Whether save data exists: the host's save directory is created at mount time, so from the game's point of view it always exists (EnsureSaveData always succeeds too)
HLE_FORCE("_ZN2nn2fs18IsSaveDataExistingERKNS_7account3UidE", "fsp-srv; save data always exists (returns bool)", unsigned, h_fs_SaveExists, const void *uid) { (void)uid; return 1; }
// File system access log and thread priority: on the real device both ask fsp-srv (the access log switch lives in the service); the host has no access log and priority does not affect pread
HLE_FORCE("_ZN2nn2fs26SetPriorityOnCurrentThreadENS0_8PriorityE", "fsp-srv; file read priority, ignored by the host", void, h_fs_SetPrio, int prio) { (void)prio; }
HLE_FORCE("_ZN2nn2fs6detail18IsEnabledAccessLogEj", "fsp-srv; access log always off (returns bool)", unsigned, h_fs_AccessLogMask, unsigned mask) { (void)mask; return 0; }
HLE_FORCE("_ZN2nn2fs6detail18IsEnabledAccessLogEv", "fsp-srv; access log always off (returns bool)", unsigned, h_fs_AccessLog, void) { return 0; }
