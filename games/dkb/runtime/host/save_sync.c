// Saves through a cloud folder (iCloud Drive): see save_sync.h. Plain C on system calls and CommonCrypto only, so the Mac launcher links it as the host does (dm/mac/pack_app.py).
// A packed save (<folder>/<title ID>/save.dmsync), little-endian:
//   header  "DMSYNC1\n", u32 version (1), u32 entries, 32 hex digits of the hash, i64 saved, char title[24], char device[64]
//   entry   u8 type (1 folder, 2 file), u8 0, u16 path length, u16 guest-name length, u16 0, u64 size, i64 mtime; the path (save/... or save_device/...), the guest name, the contents
//   end     "DMSYNCE\n", then the end of the file (a file cut off anywhere is refused)
// The hash runs over each entry's first 16 header bytes (not the date), its path, its guest name and its contents, in the order walk() visits them.
#include "save_sync.h"
#include <CommonCrypto/CommonDigest.h>
#include <TargetConditionals.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/clonefile.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/xattr.h>
#include <time.h>
#include <unistd.h>

#define SLOG(...) do { fprintf(stderr, "[dm] save sync: " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define SRED(...) do { fprintf(stderr, "[dm] [RED] save sync: " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define GUEST_NAME "org.d2rmobile.guest-name"   // hle_fs.c FS_GUEST_NAME: a file's guest name when its host name had to differ (a case collision); part of the save
#define QUIET_MS 3000    // a game often commits several times in a row: the sender waits until it has been quiet this long
#define FETCH_MS 10000   // the longest the game waits at its first save mount for the cloud's copy (a download); then it plays on with this device's save
#define KEEP_BACKUPS 10  // backups kept of each side (local, cloud); older ones are removed when a new one is made
enum { HDR = 8 + 4 + 4 + SS_HASH + 8 + 24 + 64, ENT = 24, CHUNK = 1 << 16 };   // header bytes, entry header bytes, the copy buffer
static const char MAGIC[8] __attribute__((nonstring)) = "DMSYNC1\n", END[8] __attribute__((nonstring)) = "DMSYNCE\n";
static const char *const FOLDERS[2] = {"save", "save_device"};   // their names in a packed save; on disk DM_SAVE and DM_SAVE_device (hle_fs.c h_fs_MountDeviceSave)

// ---------- files ----------
static void folder_path(const char *save, int i, char *out, size_t n) { snprintf(out, n, i ? "%s_device" : "%s", save); }

static void free_names(char **v, size_t n) { for (size_t i = 0; i < n; i++) free(v[i]); free(v); }
static int by_name(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
// A folder's names in byte order, without . and ..; -1 when it cannot be read
static int names_of(const char *dir, char ***out, size_t *count) {
    DIR *d = opendir(dir); if (!d) return -1;
    char **v = NULL; size_t n = 0, cap = 0; struct dirent *e; int r = 0;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (n == cap) { char **w = realloc(v, (cap = cap ? cap * 2 : 16) * sizeof *v); if (!w) { r = -1; break; } v = w; }
        if (!(v[n] = strdup(e->d_name))) { r = -1; break; }
        n++;
    }
    closedir(d);
    if (r) { free_names(v, n); return -1; }
    if (n) qsort(v, n, sizeof *v, by_name);
    *out = v; *count = n; return 0;
}

static int mkdirs(const char *path) {   // mkdir -p
    char p[PATH_MAX]; if (snprintf(p, sizeof p, "%s", path) >= (int)sizeof p) return -1;
    for (char *s = p + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(p, 0755); *s = '/'; }
    return mkdir(p, 0755) && errno != EEXIST ? -1 : 0;
}

static int rm_tree(const char *p) {   // rm -rf: 0 when nothing is left
    struct stat st; if (lstat(p, &st)) return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode)) return unlink(p);
    char **v; size_t n; if (names_of(p, &v, &n)) return -1;
    int r = 0;
    for (size_t i = 0; i < n; i++) { char c[PATH_MAX]; snprintf(c, sizeof c, "%s/%s", p, v[i]); if (rm_tree(c)) r = -1; }
    free_names(v, n);
    return rmdir(p) ? -1 : r;
}

// Anything in it but Finder's .DS_Store: an empty folder is no save (the host makes DM_SAVE at every mount, DM_SAVE_device at the device save's)
static int has_entries(const char *dir) {
    char **v; size_t n, k = 0; if (names_of(dir, &v, &n)) return 0;
    for (size_t i = 0; i < n; i++) k += strcmp(v[i], ".DS_Store") != 0;
    free_names(v, n); return k > 0;
}

typedef int (*visit_fn)(void *ctx, const char *abs, const char *rel, const struct stat *st);
// Every entry under abs, depth first, a folder before what it holds, names in byte order: the order that gives the same save the same hash on every device.
// Finder's .DS_Store is left out (a save folder someone opened in Finder is still the same save), and so are links, which the host never makes in a save
static int walk(const char *abs, const char *rel, visit_fn fn, void *ctx) {
    char **v; size_t n; if (names_of(abs, &v, &n)) return -1;
    int r = 0;
    for (size_t i = 0; i < n && !r; i++) {
        if (!strcmp(v[i], ".DS_Store")) continue;
        char a[PATH_MAX], p[PATH_MAX]; struct stat st;
        if (snprintf(a, sizeof a, "%s/%s", abs, v[i]) >= (int)sizeof a || snprintf(p, sizeof p, "%s/%s", rel, v[i]) >= (int)sizeof p || lstat(a, &st)) r = -1;
        else if (S_ISDIR(st.st_mode)) { if (!(r = fn(ctx, a, p, &st))) r = walk(a, p, fn, ctx); }
        else if (S_ISREG(st.st_mode)) r = fn(ctx, a, p, &st);
    }
    free_names(v, n); return r;
}

static int write_all(int fd, const void *b, size_t n) {
    for (const char *p = b; n; ) { ssize_t k = write(fd, p, n); if (k < 0 && errno == EINTR) continue; if (k <= 0) return -1; p += k; n -= (size_t)k; }
    return 0;
}

static int copy_fd(int in, int out) {
    char *b = malloc(CHUNK); if (!b) return -1;
    ssize_t k; int r = 0;
    while ((k = read(in, b, CHUNK)) != 0) { if (k < 0) { if (errno == EINTR) continue; r = -1; break; } if (write_all(out, b, (size_t)k)) { r = -1; break; } }
    free(b); return r;
}

static void hex(const uint8_t *d, char *out) { for (int i = 0; i < SS_HASH / 2; i++) snprintf(out + 2 * i, 3, "%02x", d[i]); }

static void when(int64_t t, char *out, size_t n) {   // a save's time in the log
    time_t tt = (time_t)t; struct tm tm;
    if (!t || !localtime_r(&tt, &tm) || !strftime(out, n, "%Y-%m-%d %H:%M:%S", &tm)) snprintf(out, n, "unknown");
}

static void device_name(char *out, size_t n) {   // the model ("Mac16,1", "iPhone17,1"): which device a save came from, for the log
    char model[64] = ""; size_t len = sizeof model;
    if (sysctlbyname(TARGET_OS_OSX ? "hw.model" : "hw.machine", model, &len, NULL, 0)) model[0] = 0;
    snprintf(out, n, "%s", model[0] ? model : "unknown");
}

static void upper_title(const char *title, char *out, size_t n) {   // one spelling of the title ID in the cloud, whoever packed the app
    size_t i = 0; for (; title[i] && i + 1 < n; i++) out[i] = (char)toupper((unsigned char)title[i]);
    out[i] = 0;
}

// ---------- packing ----------
typedef struct { CC_SHA256_CTX sha; FILE *out; ss_meta *m; uint32_t entries; uint8_t buf[CHUNK]; } packer;

static int pack_entry(void *c, const char *abs, const char *rel, const struct stat *st) {
    packer *k = c;
    char guest[1024]; ssize_t gl = getxattr(abs, GUEST_NAME, guest, sizeof guest, 0, XATTR_NOFOLLOW);
    if (gl < 0) gl = 0;   // ENOATTR: the host name is the guest's
    size_t pl = strlen(rel); int file = S_ISREG(st->st_mode);
    if (pl > UINT16_MAX) return -1;
    uint8_t h[ENT] = {(uint8_t)(file ? 2 : 1)}; uint16_t pl16 = (uint16_t)pl, gl16 = (uint16_t)gl;
    uint64_t size = file ? (uint64_t)st->st_size : 0; int64_t mt = st->st_mtimespec.tv_sec;
    memcpy(h + 2, &pl16, 2); memcpy(h + 4, &gl16, 2); memcpy(h + 8, &size, 8); memcpy(h + 16, &mt, 8);
    CC_SHA256_Update(&k->sha, h, 16); CC_SHA256_Update(&k->sha, rel, (CC_LONG)pl); CC_SHA256_Update(&k->sha, guest, (CC_LONG)gl);
    if (k->out && (fwrite(h, 1, ENT, k->out) != ENT || fwrite(rel, 1, pl, k->out) != pl || fwrite(guest, 1, (size_t)gl, k->out) != (size_t)gl)) return -1;
    k->entries++;
    if (!file) return 0;
    int fd = open(abs, O_RDONLY); if (fd < 0) return -1;
    uint64_t left = size; ssize_t got = 0;
    while (left && (got = read(fd, k->buf, left < CHUNK ? (size_t)left : CHUNK)) != 0) {
        if (got < 0) { if (errno == EINTR) continue; break; }
        CC_SHA256_Update(&k->sha, k->buf, (CC_LONG)got);
        if (k->out && fwrite(k->buf, 1, (size_t)got, k->out) != (size_t)got) break;
        left -= (uint64_t)got;
    }
    close(fd);
    if (left) return -1;   // a failed read, or a file shorter than its size (the save is a clone no Commit touches, so not one being written)
    k->m->files++;
    if (mt > k->m->saved) k->m->saved = mt;
    return 0;
}

static int pack_save(const char *save, packer *k) {
    CC_SHA256_Init(&k->sha);
    for (int i = 0; i < 2; i++) {
        char dir[PATH_MAX]; folder_path(save, i, dir, sizeof dir);
        if (has_entries(dir) && walk(dir, FOLDERS[i], pack_entry, k)) return -1;
    }
    uint8_t d[CC_SHA256_DIGEST_LENGTH]; CC_SHA256_Final(d, &k->sha); hex(d, k->m->hash);
    if (!k->m->files) k->m->hash[0] = 0;   // no save: no hash, so that "nothing here" compares as nothing
    return 0;
}

int ss_scan(const char *save, ss_meta *m) {
    memset(m, 0, sizeof *m);
    packer *k = calloc(1, sizeof *k); if (!k) return -1;
    k->m = m; int r = pack_save(save, k);
    free(k); return r;
}

int ss_pack(const char *save, const char *title, const char *out, ss_meta *m) {
    memset(m, 0, sizeof *m);
    upper_title(title, m->title, sizeof m->title); device_name(m->device, sizeof m->device);
    packer *k = calloc(1, sizeof *k); if (!k) return -1;
    k->m = m; int r = -1;
    uint8_t hdr[HDR] = {0}; uint32_t version = 1;
    if (!(k->out = fopen(out, "wb")) || fwrite(hdr, 1, HDR, k->out) != HDR || pack_save(save, k) || fwrite(END, 1, sizeof END, k->out) != sizeof END) goto done;
    memcpy(hdr, MAGIC, 8); memcpy(hdr + 8, &version, 4); memcpy(hdr + 12, &k->entries, 4); memcpy(hdr + 16, m->hash, strlen(m->hash));
    memcpy(hdr + 48, &m->saved, 8); memcpy(hdr + 56, m->title, strlen(m->title)); memcpy(hdr + 80, m->device, strlen(m->device));
    if (fseeko(k->out, 0, SEEK_SET) || fwrite(hdr, 1, HDR, k->out) != HDR || fflush(k->out) || fsync(fileno(k->out))) goto done;
    r = 0;
done:
    if (k->out && fclose(k->out)) r = -1;
    if (r) unlink(out);
    free(k); return r;
}

// A path in a packed save: under save/ or save_device/, with no empty, . or .. part (a damaged file must not write outside `into`)
static int path_ok(const char *p) {
    if (strncmp(p, "save/", 5) && strncmp(p, "save_device/", 12)) return 0;
    for (const char *s = p;; ) {
        const char *e = strchr(s, '/'); size_t n = e ? (size_t)(e - s) : strlen(s);
        if (!n || (n == 1 && s[0] == '.') || (n == 2 && s[0] == '.' && s[1] == '.')) return 0;
        if (!e) return 1;
        s = e + 1;
    }
}

int ss_read(const char *bundle, const char *into, ss_meta *m) {
    memset(m, 0, sizeof *m);
    FILE *f = fopen(bundle, "rb"); if (!f) return -1;
    uint8_t *buf = malloc(CHUNK), hdr[HDR], h[ENT], end[sizeof END], d[CC_SHA256_DIGEST_LENGTH];
    char want[SS_HASH + 1] = "", path[PATH_MAX], guest[1024], out[PATH_MAX];
    uint32_t version, n; int r = -1, fd = -1;
    CC_SHA256_CTX sha; CC_SHA256_Init(&sha);
    if (!buf || fread(hdr, 1, HDR, f) != HDR || memcmp(hdr, MAGIC, 8)) goto done;
    memcpy(&version, hdr + 8, 4); memcpy(&n, hdr + 12, 4);
    if (version != 1) goto done;
    memcpy(want, hdr + 16, SS_HASH); memcpy(&m->saved, hdr + 48, 8); memcpy(m->title, hdr + 56, 23); memcpy(m->device, hdr + 80, 63);
    for (uint32_t i = 0; i < n; i++) {
        uint16_t pl, gl; uint64_t size; int64_t mt;
        if (fread(h, 1, ENT, f) != ENT) goto done;
        memcpy(&pl, h + 2, 2); memcpy(&gl, h + 4, 2); memcpy(&size, h + 8, 8); memcpy(&mt, h + 16, 8);
        if ((h[0] != 1 && h[0] != 2) || !pl || pl >= sizeof path || gl > sizeof guest || (h[0] == 1 && size)) goto done;
        if (fread(path, 1, pl, f) != pl || fread(guest, 1, gl, f) != gl) goto done;
        path[pl] = 0;
        if (strlen(path) != pl || !path_ok(path)) goto done;
        CC_SHA256_Update(&sha, h, 16); CC_SHA256_Update(&sha, path, pl); CC_SHA256_Update(&sha, guest, gl);
        if (into) {
            if (snprintf(out, sizeof out, "%s/%s", into, path) >= (int)sizeof out) goto done;
            if (h[0] == 1) { if (mkdirs(out)) goto done; }
            else {
                *strrchr(out, '/') = 0; int made = mkdirs(out); out[strlen(out)] = '/';
                if (made || (fd = open(out, O_WRONLY | O_CREAT | O_EXCL, 0644)) < 0) goto done;
            }
            if (gl && setxattr(out, GUEST_NAME, guest, gl, 0, XATTR_NOFOLLOW)) goto done;
        }
        for (uint64_t left = size; left; ) {
            size_t k = left < CHUNK ? (size_t)left : CHUNK;
            if (fread(buf, 1, k, f) != k) goto done;
            CC_SHA256_Update(&sha, buf, (CC_LONG)k);
            if (fd >= 0 && write_all(fd, buf, k)) goto done;
            left -= k;
        }
        if (fd >= 0) {   // the file's own date back: the newest one is the save's age when two changed saves are compared (ss_decide), not the time of the copy
            struct timespec t[2] = {{(time_t)mt, 0}, {(time_t)mt, 0}}; futimens(fd, t);
            int bad = close(fd); fd = -1; if (bad) goto done;
        }
        if (h[0] == 2) m->files++;
    }
    if (fread(end, 1, sizeof end, f) != sizeof end || memcmp(end, END, sizeof end) || fgetc(f) != EOF) goto done;
    CC_SHA256_Final(d, &sha); hex(d, m->hash);
    if (!m->files) m->hash[0] = 0;
    if (strcmp(m->hash, want)) goto done;
    r = 0;
done:
    if (fd >= 0) close(fd);
    free(buf); fclose(f);
    if (r) m->files = 0, m->hash[0] = 0;
    return r;
}

ss_way ss_decide(const char *local, const char *base, const char *remote, int64_t local_saved, int64_t remote_saved, int *both) {
    *both = 0;
    if (!*remote) return *local ? SS_PUSH : SS_KEEP;           // nothing in the cloud
    if (!strcmp(local, remote)) return SS_KEEP;                 // already the same
    if (!*local) return SS_PULL;                                // nothing here
    if (*base && !strcmp(local, base)) return SS_PULL;          // only the cloud's changed
    if (*base && !strcmp(remote, base)) return SS_PUSH;         // only this one changed
    *both = 1;                                                  // both changed, or this device's first sync with a save of its own
    return remote_saved > local_saved ? SS_PULL : SS_PUSH;
}

// ---------- the sync state, backups, the cloud's file ----------
static void read_base(const char *save, char *out) {   // <save>.sync: the hash of the save last known to be the same here and in the cloud ("" for none)
    char p[PATH_MAX], ln[128]; out[0] = 0;
    snprintf(p, sizeof p, "%s.sync", save);
    FILE *f = fopen(p, "r"); if (!f) return;
    while (fgets(ln, sizeof ln, f)) if (!strncmp(ln, "base\t", 5) && strspn(ln + 5, "0123456789abcdef") == SS_HASH) { memcpy(out, ln + 5, SS_HASH); out[SS_HASH] = 0; }
    fclose(f);
}

static void write_base(const char *save, const char *hash) {   // written beside it and renamed over it: a cut-off write must not read as another base
    char p[PATH_MAX], t[PATH_MAX + 8];
    snprintf(p, sizeof p, "%s.sync", save); snprintf(t, sizeof t, "%s.tmp", p);
    FILE *f = fopen(t, "w");
    int err = f ? 0 : errno;
    if (f) {
        if (fprintf(f, "# save sync (dm/host/save_sync.h): the save last known to be the same here and in the cloud\nbase\t%s\n", hash) < 0) err = errno;
        // Close even after a write error, and keep that error if close or cleanup fails too.
        if (fclose(f) && !err) err = errno;
        if (!err && rename(t, p)) err = errno;
        if (err) unlink(t);
    }
    if (err) SRED("cannot write %s (errno=%d): the next start compares with the sync before", p, err);
}

// A backup this file made for `side`: "YYYY-MM-DD HH.MM.SS <side>", or with " <k>" after it (backup_dir); anything else in <save>-backups is the player's
static int is_backup(const char *name, const char *side) {
    static const char form[] = "0000-00-00 00.00.00 ";
    for (int i = 0; form[i]; i++) if (form[i] == '0' ? !isdigit((unsigned char)name[i]) : name[i] != form[i]) return 0;
    const char *s = name + sizeof form - 1; size_t sl = strlen(side);
    if (strncmp(s, side, sl)) return 0;
    if (!s[sl]) return 1;
    if (s[sl] != ' ' || !s[sl + 1]) return 0;
    for (s += sl + 1; *s; s++) if (!isdigit((unsigned char)*s)) return 0;
    return 1;
}

// Only the newest KEEP_BACKUPS of a side stay: each switch between devices keeps a whole save, and on a phone they add up. `made` (just made) is one of them whatever its date says
static void prune_backups(const char *save, const char *side, const char *made) {
    char dir[PATH_MAX], **v; size_t n, k = 0, gone = 0; snprintf(dir, sizeof dir, "%s-backups", save);
    if (names_of(dir, &v, &n)) return;
    const char *mine = strrchr(made, '/'); mine = mine ? mine + 1 : made;
    for (size_t i = 0; i < n; i++) if (is_backup(v[i], side) && strcmp(v[i], mine)) v[k++] = v[i]; else free(v[i]);   // in date order (names_of sorts them)
    for (size_t i = 0; i + (KEEP_BACKUPS - 1) < k; i++) {
        char p[PATH_MAX]; snprintf(p, sizeof p, "%s/%s", dir, v[i]);
        if (rm_tree(p)) SRED("cannot remove the old backup %s (errno=%d)", p, errno); else gone++;
    }
    if (gone) SLOG("removed %zu old %s backup%s (the newest %d stay)", gone, side, gone == 1 ? "" : "s", KEEP_BACKUPS);
    free_names(v, k);
}

// A folder for a save about to be replaced: <save>-backups/<date> <side> (local: this device's, cloud: the cloud's); the oldest of that side beyond KEEP_BACKUPS go
static int backup_dir(const char *save, const char *side, char *out, size_t n) {
    char stamp[32]; time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm); strftime(stamp, sizeof stamp, "%Y-%m-%d %H.%M.%S", &tm);
    snprintf(out, n, "%s-backups/%s %s", save, stamp, side);
    for (int k = 2; !access(out, F_OK); k++) snprintf(out, n, "%s-backups/%s %s %d", save, stamp, side, k);
    if (mkdirs(out)) return -1;
    prune_backups(save, side, out);
    return 0;
}

// DM_SAVE_SYNC → <folder>/<title ID>/save.dmsync; 0 when sync is off
static int cloud_file(const char *title, char *out, size_t n) {
    const char *v = getenv("DM_SAVE_SYNC"); char dir[PATH_MAX], id[24];
    if (!v || !*v || !strcmp(v, "0")) return 0;
    if (!strcmp(v, "icloud")) {
#if TARGET_OS_OSX
        const char *home = getenv("HOME"); char drive[PATH_MAX]; struct stat st;
        snprintf(drive, sizeof drive, "%s/Library/Mobile Documents/com~apple~CloudDocs", home ? home : "");
        if (!home || stat(drive, &st)) {
            if (errno == EPERM || errno == EACCES) SLOG("off: this app may not open iCloud Drive (System Settings > Privacy & Security > Files and Folders)");
            else SLOG("off: iCloud Drive is not on for this Mac (no %s)", drive);
            return 0;
        }
        snprintf(dir, sizeof dir, "%s/nxnative Saves", drive);
#else
        SLOG("off: DM_SAVE_SYNC=icloud is the Mac's iCloud Drive folder; on this device, choose the folder in the launcher (Cloud saves)");
        return 0;
#endif
    } else snprintf(dir, sizeof dir, "%s", v);
    size_t L = strlen(dir); while (L > 1 && dir[L - 1] == '/') dir[--L] = 0;
    upper_title(title, id, sizeof id);
    if (!*id || strchr(id, '/')) return 0;
    // The game's own folder chosen instead of the one above it (a picker shows <folder>/<title ID> once another device has synced): taken as it is, not given a second <title ID>
    const char *last = strrchr(dir, '/');
    if (!strcasecmp(last ? last + 1 : dir, id)) return snprintf(out, n, "%s/save.dmsync", dir) < (int)n;
    return snprintf(out, n, "%s/%s/save.dmsync", dir, id) < (int)n;
}

// The platform's fetch, given at most `ms`: a download iCloud never finishes must not hold the game at its first save mount. One that comes too late is dropped
typedef struct { pthread_mutex_t mu; pthread_cond_t cv; int done, abandoned, result; char cloud[PATH_MAX], local[PATH_MAX]; } fetch_job;
static void *fetch_thread(void *p) {
    fetch_job *j = p; int r = front_save_sync_fetch(j->cloud, j->local);
    pthread_mutex_lock(&j->mu); j->done = 1; j->result = r; int late = j->abandoned; pthread_cond_signal(&j->cv); pthread_mutex_unlock(&j->mu);
    if (late) { unlink(j->local); pthread_mutex_destroy(&j->mu); pthread_cond_destroy(&j->cv); free(j); }
    return NULL;
}
static void deadline(int ms, struct timespec *ts) {
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += ms / 1000; ts->tv_nsec += (long)(ms % 1000) * 1000000;
    if (ts->tv_nsec >= 1000000000) { ts->tv_sec++; ts->tv_nsec -= 1000000000; }
}
static int fetch_timed(const char *cloud, const char *local, int ms) {   // front_save_sync_fetch's answer, or -1 at the time limit (or with no thread to wait on)
    fetch_job *j = calloc(1, sizeof *j); pthread_t t;
    if (!j) return -1;
    pthread_mutex_init(&j->mu, NULL); pthread_cond_init(&j->cv, NULL);
    snprintf(j->cloud, sizeof j->cloud, "%s", cloud); snprintf(j->local, sizeof j->local, "%s", local);
    int e = pthread_create(&t, NULL, fetch_thread, j);
    if (e) { pthread_mutex_destroy(&j->mu); pthread_cond_destroy(&j->cv); free(j); SRED("no thread to fetch the cloud's save (error %d)", e); return -1; }   // not fetched on this thread: a download that never finishes would hold the game
    pthread_detach(t);
    struct timespec ts; deadline(ms, &ts);
    pthread_mutex_lock(&j->mu);
    while (!j->done && pthread_cond_timedwait(&j->cv, &j->mu, &ts) != ETIMEDOUT) {}
    int r = j->done ? j->result : -1, mine = j->done;
    if (!mine) j->abandoned = 1;   // the thread frees it when it ends
    pthread_mutex_unlock(&j->mu);
    if (mine) { pthread_mutex_destroy(&j->mu); pthread_cond_destroy(&j->cv); free(j); }
    else SLOG("the cloud's save did not come within %d s", ms / 1000);
    return r;
}

// ---------- the two directions ----------
// The cloud's save replaces this one. It is unpacked beside it first, so a pull that fails leaves this save as it was; then each folder is swapped in at once
// (renamex_np, as a Commit swaps), and the one it replaced goes to the backups. kept: that backup, "" when this device had no save
static int pull(const char *save, const char *bundle, ss_meta *r, char *kept, size_t kn) {
    char tmp[PATH_MAX]; snprintf(tmp, sizeof tmp, "%s.sync-tmp/in", save);
    kept[0] = 0;
    if (rm_tree(tmp) || mkdirs(tmp) || ss_read(bundle, tmp, r)) { rm_tree(tmp); return -1; }
    for (int i = 0; i < 2; i++) {
        char here[PATH_MAX], incoming[PATH_MAX], old[PATH_MAX];
        folder_path(save, i, here, sizeof here); snprintf(incoming, sizeof incoming, "%s/%s", tmp, FOLDERS[i]);
        int fresh = !access(incoming, F_OK), had = has_entries(here);
        if (had && !*kept && backup_dir(save, "local", kept, kn)) return -1;
        snprintf(old, sizeof old, "%s/%s", kept, FOLDERS[i]);
        if (fresh && !access(here, F_OK)) {   // swapped: here holds the cloud's, incoming this device's
            if (renamex_np(incoming, here, RENAME_SWAP) || (had ? rename(incoming, old) : rm_tree(incoming))) return -1;
        } else if (fresh) { if (rename(incoming, here)) return -1; }
        else if (had && rename(here, old)) return -1;   // a folder the cloud's save does not have: this device's goes, so both hold the same
    }
    rm_tree(tmp);
    return 0;
}

// What this device committed goes to the cloud, unless the cloud already has it. A cloud save changed since this device's last sync (another device sent it) is kept first.
// 1 = in step with the cloud, 0 = not: the cloud cannot be read or written now (the next Commit, a flush or the Mac launcher tries again)
static int send_save(const char *save, const char *title, const char *cloud) {
    char tmp[PATH_MAX], snap[PATH_MAX], packed[PATH_MAX], theirs[PATH_MAX], base[SS_HASH + 1], dir[PATH_MAX], kept[PATH_MAX], t[32];
    ss_meta mine, other; int ok = 0, got;
    snprintf(tmp, sizeof tmp, "%s.sync-tmp/out", save); snprintf(snap, sizeof snap, "%s/s", tmp);
    snprintf(packed, sizeof packed, "%s/save.dmsync", tmp); snprintf(theirs, sizeof theirs, "%s/cloud.dmsync", tmp);
    if (rm_tree(tmp) || mkdirs(tmp)) { SRED("cannot make %s (errno=%d)", tmp, errno); goto done; }
    // A clone of the committed folders, taken while no Commit can swap them: the game may commit again while this packs
    for (int i = 0; i < 2; i++) {
        char from[PATH_MAX], to[PATH_MAX]; folder_path(save, i, from, sizeof from); folder_path(snap, i, to, sizeof to);
        if (has_entries(from) && dm_fs_clone_committed(from, to)) { SRED("cloning %s failed (errno=%d)", from, errno); goto done; }
    }
    if (ss_pack(snap, title, packed, &mine)) { SRED("packing %s failed (errno=%d)", save, errno); goto done; }
    read_base(save, base);
    if (!mine.files || !strcmp(mine.hash, base)) { ok = 1; goto done; }   // no save yet, or the one already in step
    if ((got = fetch_timed(cloud, theirs, FETCH_MS)) < 0) { SLOG("the cloud's save %s cannot be read now; sending later", cloud); goto done; }
    if (got && ss_read(theirs, NULL, &other)) SRED("the cloud's save %s is damaged; this device's replaces it", cloud);
    else if (got && !strcmp(other.hash, mine.hash)) { write_base(save, mine.hash); ok = 1; goto done; }
    else if (got && other.files && strcmp(other.hash, base)) {
        if (backup_dir(save, "cloud", kept, sizeof kept) || ss_read(theirs, kept, &other)) { SRED("cannot keep the cloud's save before replacing it (errno=%d); not sent", errno); goto done; }
        when(other.saved, t, sizeof t);
        SLOG("the cloud's save (from %s, saved %s) changed since this device's last sync; this device's newer one replaces it, and it is kept in %s", other.device, t, kept);
    }
    snprintf(dir, sizeof dir, "%s", cloud); *strrchr(dir, '/') = 0;
    if (mkdirs(dir) || front_save_sync_store(packed, cloud) != 1) { SRED("sending to %s failed (errno=%d)", cloud, errno); goto done; }
    write_base(save, mine.hash); ok = 1;
    when(mine.saved, t, sizeof t);
    SLOG("sent: %d files, saved %s, to %s", mine.files, t, cloud);
done:
    rm_tree(tmp); *strrchr(tmp, '/') = 0; rmdir(tmp);   // <save>.sync-tmp too, once nothing else is in it (a fetch that came too late may still be)
    return ok;
}

// ---------- the host: the first mount, the sender, flushes ----------
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER, g_mount_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static _Atomic int g_on;
static int g_mounted, g_want, g_busy, g_failed;   // the mount ran; a send asked for without a Commit (the mount found this save newer, a flush); a send running; the last one failed
static uint64_t g_commits, g_sent;                // Commits seen; the Commits the last send covered
static char g_save[PATH_MAX], g_title[24], g_cloud[PATH_MAX];

static void *sender(void *arg) {
    (void)arg;
    pthread_mutex_lock(&g_mu);
    for (;;) {
        while (g_commits == g_sent && !g_want) pthread_cond_wait(&g_cv, &g_mu);
        for (uint64_t seen = g_commits; !g_want; ) {   // quiet for QUIET_MS since the last Commit (a flush does not wait)
            struct timespec ts; deadline(QUIET_MS, &ts);
            int r = 0;
            while (!g_want && g_commits == seen && r != ETIMEDOUT) r = pthread_cond_timedwait(&g_cv, &g_mu, &ts);
            if (g_commits == seen) break;
            seen = g_commits;
        }
        uint64_t upto = g_commits; g_want = 0; g_busy = 1;
        pthread_mutex_unlock(&g_mu);
        int ok = send_save(g_save, g_title, g_cloud);
        pthread_mutex_lock(&g_mu);
        g_busy = 0; g_sent = upto; g_failed = !ok;
        pthread_cond_broadcast(&g_cv);
    }
    return NULL;
}

static void mount_once(const char *save, const char *title) {
    char cloud[PATH_MAX], tmp[PATH_MAX], theirs[PATH_MAX], base[SS_HASH + 1], kept[PATH_MAX], t[32];
    ss_meta mine, other; memset(&other, 0, sizeof other);
    if (!cloud_file(title, cloud, sizeof cloud)) return;
    SLOG("on: %s", cloud);
    snprintf(tmp, sizeof tmp, "%s.sync-tmp", save); snprintf(theirs, sizeof theirs, "%s/in.dmsync", tmp);
    if (rm_tree(tmp) || mkdirs(tmp)) { SRED("cannot make %s (errno=%d): off", tmp, errno); return; }   // rm_tree: what an earlier session left
    if (ss_scan(save, &mine)) { SRED("this device's save %s cannot be read (errno=%d): off", save, errno); return; }
    read_base(save, base);
    int got = fetch_timed(cloud, theirs, FETCH_MS), both = 0, failed = 0;
    if (got > 0 && ss_read(theirs, NULL, &other)) { SRED("the cloud's save %s is damaged: playing on with this device's, which replaces it", cloud); got = 0; }
    else if (got > 0 && strcasecmp(other.title, title)) { SRED("the cloud's save %s is %s's, not %s's: not used", cloud, other.title, title); got = -1; }
    if (got < 0) { SLOG("playing on with this device's save; it is sent after the game saves, unless the cloud's has changed meanwhile (then that one is kept)"); failed = 1; }
    switch (got < 0 ? SS_KEEP : ss_decide(mine.hash, base, got ? other.hash : "", mine.saved, other.saved, &both)) {
    case SS_PULL:
        when(other.saved, t, sizeof t);
        if (pull(save, theirs, &other, kept, sizeof kept)) { SRED("bringing the cloud's save in failed (errno=%d): playing on with this device's", errno); failed = 1; break; }
        write_base(save, other.hash);
        SLOG("brought in the cloud's save (from %s, saved %s%s)%s%s", other.device, t, both ? ", the newer of two that both changed" : "", *kept ? "; this device's is kept in " : "", kept);
        break;
    case SS_PUSH:
        when(mine.saved, t, sizeof t);
        SLOG("this device's save (saved %s) %s: it goes to the cloud", t, !got ? "is not in the cloud yet" : both ? "is the newer of two that both changed" : "changed since the last sync");
        g_want = 1;
        break;
    case SS_KEEP:
        if (got > 0 && mine.files) { write_base(save, mine.hash); SLOG("in step with the cloud"); }
        else if (!got) SLOG("no save here or in the cloud yet");
        break;
    }
    rm_tree(tmp);
    snprintf(g_save, sizeof g_save, "%s", save); upper_title(title, g_title, sizeof g_title); snprintf(g_cloud, sizeof g_cloud, "%s", cloud);
    g_failed = failed;
    pthread_t th;
    if (pthread_create(&th, NULL, sender, NULL)) { SRED("cannot start the sender: saves are not sent this session"); return; }
    pthread_detach(th);
    atomic_store(&g_on, 1);
}

void save_sync_mount(const char *save, const char *title) {   // the first mount does it; a second one (the device save) waits for it
    pthread_mutex_lock(&g_mount_mu);
    if (!g_mounted) { g_mounted = 1; mount_once(save, title); }
    pthread_mutex_unlock(&g_mount_mu);
}

void save_sync_committed(void) {
    if (!atomic_load(&g_on)) return;
    pthread_mutex_lock(&g_mu); g_commits++; pthread_cond_broadcast(&g_cv); pthread_mutex_unlock(&g_mu);
}

int save_sync_flush(int timeout_ms) {
    if (!atomic_load(&g_on)) return 1;
    struct timespec ts; deadline(timeout_ms, &ts);
    pthread_mutex_lock(&g_mu);
    if (g_commits != g_sent || g_failed || g_busy) { g_want = 1; pthread_cond_broadcast(&g_cv); }
    while ((g_want || g_busy || g_commits != g_sent) && pthread_cond_timedwait(&g_cv, &g_mu, &ts) != ETIMEDOUT) {}
    int ok = !g_want && !g_busy && g_commits == g_sent && !g_failed;
    pthread_mutex_unlock(&g_mu);
    return ok;
}

int save_sync_after_exit(const char *save, const char *title) {   // save: DM_SAVE, its trailing slashes stripped as h_fs_MountSave strips them (the same <save>.sync and <save>-backups)
    char cloud[PATH_MAX], dd[PATH_MAX];
    if (!save || !*save) return 1;
    snprintf(dd, sizeof dd, "%s", save); size_t L = strlen(dd); while (L > 1 && dd[L - 1] == '/') dd[--L] = 0;
    return !cloud_file(title, cloud, sizeof cloud) || send_save(dd, title, cloud);
}

// ---------- the platform's parts: these defaults are plain file copies, which the Mac's iCloud Drive takes (it downloads a file a read asks for) ----------
__attribute__((weak)) int front_save_sync_fetch(const char *cloud, const char *local) {
    int in = open(cloud, O_RDONLY); if (in < 0) return errno == ENOENT ? 0 : -1;
    int out = open(local, O_WRONLY | O_CREAT | O_TRUNC, 0644); if (out < 0) { close(in); return -1; }
    int r = copy_fd(in, out); close(in);
    if (close(out)) r = -1;
    if (r) unlink(local);
    return r ? -1 : 1;
}

// Written beside the cloud file under a .nosync name (iCloud Drive does not upload those) and renamed over it: iCloud sees one whole new file
__attribute__((weak)) int front_save_sync_store(const char *local, const char *cloud) {
    char part[PATH_MAX]; snprintf(part, sizeof part, "%s.%d.nosync", cloud, (int)getpid());
    int in = open(local, O_RDONLY); if (in < 0) return 0;
    int out = open(part, O_WRONLY | O_CREAT | O_TRUNC, 0644); if (out < 0) { close(in); return 0; }
    int r = copy_fd(in, out);
    if (!r && fsync(out)) r = -1;
    close(in);
    if (close(out)) r = -1;
    if (!r && rename(part, cloud)) r = -1;
    if (r) unlink(part);
    return !r;
}

__attribute__((weak)) int dm_fs_clone_committed(const char *dir, const char *to) { return clonefile(dir, to, 0) ? -1 : 0; }
