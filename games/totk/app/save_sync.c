/* The three-way merge of app/save_sync.h: plain C, so tests/test_save_sync.c runs it against plain folders. */
#include "save_sync.h"
#include <CommonCrypto/CommonDigest.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAX_REL 1024
#define SHA_HEX 65

typedef struct { char *path; char sha[SHA_HEX]; int action; } Entry;
typedef struct { Entry *e; int count, cap; } Map;

enum { A_NONE, A_TAKE, A_SEND, A_RESTORE, A_DELETE_REMOTE, A_CONFLICT_SEND, A_CONFLICT_DELETE };

static void say(void (*log)(void *, const char *), void *ud, const char *fmt, ...) {
    if (!log) return;
    char line[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof line, fmt, args);
    va_end(args);
    log(ud, line);
}

/* Server names and manifest lines become file names: no traversal, no separators of another kind, no control bytes. */
static int rel_ok(const char *rel) {
    if (!rel || !*rel || strlen(rel) > MAX_REL || rel[0] == '/' || strchr(rel, '\\')) return 0;
    for (const char *p = rel; *p; p++) if ((unsigned char)*p < 0x20) return 0;
    for (const char *q = rel;; ) {
        const char *end = strchr(q, '/');
        size_t len = end ? (size_t)(end - q) : strlen(q);
        if (!len || (len == 2 && q[0] == '.' && q[1] == '.')) return 0;
        if (!end) break;
        q = end + 1;
    }
    return 1;
}

static int sha256_file(const char *path, char out[SHA_HEX]) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    CC_SHA256_CTX ctx;
    CC_SHA256_Init(&ctx);
    unsigned char buf[1 << 16], digest[CC_SHA256_DIGEST_LENGTH];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) CC_SHA256_Update(&ctx, buf, (CC_LONG)n);
    int bad = ferror(f);
    fclose(f);
    if (bad) return -1;
    CC_SHA256_Final(digest, &ctx);
    for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; i++) snprintf(out + 2 * i, 3, "%02x", digest[i]);
    return 0;
}

static int map_cmp(const void *a, const void *b) { return strcmp(((const Entry *)a)->path, ((const Entry *)b)->path); }
static void map_sort(Map *m) { if (m->count > 1) qsort(m->e, (size_t)m->count, sizeof *m->e, map_cmp); }
static void map_free(Map *m) {
    for (int i = 0; i < m->count; i++) free(m->e[i].path);
    free(m->e);
    m->e = NULL; m->count = m->cap = 0;
}
static Entry *map_add(Map *m, const char *path) {
    if (m->count == m->cap) {
        m->cap = m->cap ? m->cap * 2 : 16;
        Entry *grown = realloc(m->e, (size_t)m->cap * sizeof *m->e);
        if (!grown) return NULL;
        m->e = grown;
    }
    m->e[m->count] = (Entry){strdup(path), "", A_NONE};
    return m->e[m->count++].path ? &m->e[m->count - 1] : NULL;
}
static int map_find(const Map *m, const char *path) {
    for (int i = 0; i < m->count; i++) if (!strcmp(m->e[i].path, path)) return i;
    return -1;
}
static const char *sha_of(const Map *m, const char *path) {
    int i = map_find(m, path);
    return i < 0 ? "" : m->e[i].sha;
}

static int mkdirs(const char *path) {
    char copy[MAX_REL + 512];
    if (snprintf(copy, sizeof copy, "%s", path) >= (int)sizeof copy) return -1;
    for (char *p = copy + 1; *p; p++)
        if (*p == '/') { *p = 0; if (mkdir(copy, 0700) && errno != EEXIST) return -1; *p = '/'; }
    return mkdir(copy, 0700) && errno != EEXIST ? -1 : 0;
}

/* Writes land through a .partial file and a rename, so a failed sync never leaves half a file where the game reads. */
static int copy_file(const char *from, const char *to) {
    FILE *in = fopen(from, "rb");
    if (!in) return -1;
    char temp[MAX_REL + 512];
    if (snprintf(temp, sizeof temp, "%s.partial-%d", to, getpid()) >= (int)sizeof temp) { fclose(in); return -1; }
    FILE *out = fopen(temp, "wb");
    if (!out) { fclose(in); return -1; }
    char buf[1 << 16];
    size_t n;
    int bad = 0;
    while (!bad && (n = fread(buf, 1, sizeof buf, in)) > 0) bad = fwrite(buf, 1, n, out) != n;
    bad |= fflush(out) != 0 || fsync(fileno(out)) != 0;
    fclose(in);
    fclose(out);
    if (bad || rename(temp, to)) { unlink(temp); return -1; }
    return 0;
}

static int walk_local(const char *root, const char *prefix, Map *m, void (*log)(void *, const char *), void *ud) {
    DIR *d = opendir(root);
    if (!d) return errno == ENOENT ? 0 : -1;
    struct dirent *e;
    int bad = 0;
    while (!bad && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char path[MAX_REL + 512], rel[MAX_REL + 256];
        if (snprintf(path, sizeof path, "%s/%s", root, e->d_name) >= (int)sizeof path ||
            snprintf(rel, sizeof rel, "%s%s", prefix, e->d_name) >= (int)sizeof rel) { bad = -1; break; }
        struct stat st;
        if (lstat(path, &st)) continue;   /* vanished between readdir and stat: not part of this sync */
        if (S_ISDIR(st.st_mode)) {
            char next[MAX_REL + 256];
            if (snprintf(next, sizeof next, "%s%s/", prefix, e->d_name) >= (int)sizeof next) { bad = -1; break; }
            bad = walk_local(path, next, m, log, ud);
        } else if (S_ISREG(st.st_mode)) {
            Entry *entry = map_add(m, rel);
            bad = !entry || sha256_file(path, entry->sha);
        } else {
            say(log, ud, "not a regular file, left out of the sync: %s", rel);
        }
    }
    closedir(d);
    return bad;
}

static int read_manifest(const char *path, Map *m, void (*log)(void *, const char *), void *ud) {
    FILE *f = fopen(path, "r");
    if (!f) return errno == ENOENT ? 0 : -1;
    char line[MAX_REL + 128];
    while (fgets(line, sizeof line, f)) {
        char sha[SHA_HEX], rel[MAX_REL + 8];
        if (sscanf(line, "%64s %1023s", sha, rel) != 2) continue;   // a blank line
        if (strlen(sha) != 64 || strspn(sha, "0123456789abcdef") != 64 || !rel_ok(rel)) {
            say(log, ud, "manifest line ignored (not a sha and a plain path): %s", line);
            continue;
        }
        Entry *e = map_add(m, rel);
        if (!e) { fclose(f); return -1; }
        strcpy(e->sha, sha);
    }
    fclose(f);
    return 0;
}

static int write_manifest(const char *path, const Map *m) {
    char temp[MAX_REL + 512];
    if (snprintf(temp, sizeof temp, "%s.partial-%d", path, getpid()) >= (int)sizeof temp) return -1;
    FILE *f = fopen(temp, "w");
    if (!f) return -1;
    for (int i = 0; i < m->count; i++)
        if (*m->e[i].sha && fprintf(f, "%s %s\n", m->e[i].sha, m->e[i].path) < 0) { fclose(f); unlink(temp); return -1; }
    int bad = fflush(f) != 0 || fsync(fileno(f)) != 0;
    fclose(f);
    if (bad || rename(temp, path)) { unlink(temp); return -1; }
    return 0;
}

/* A copy of the whole save root (a few MB) taken before the first local change of this sync. */
static int backup_tree(const char *src, const char *dst) {
    if (mkdir(dst, 0700) && errno != EEXIST) return -1;
    DIR *d = opendir(src);
    if (!d) return errno == ENOENT ? 0 : -1;
    struct dirent *e;
    int bad = 0;
    while (!bad && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char from[MAX_REL + 512], to[MAX_REL + 512];
        if (snprintf(from, sizeof from, "%s/%s", src, e->d_name) >= (int)sizeof from ||
            snprintf(to, sizeof to, "%s/%s", dst, e->d_name) >= (int)sizeof to) { bad = -1; break; }
        struct stat st;
        if (lstat(from, &st)) continue;
        if (S_ISDIR(st.st_mode)) bad = backup_tree(from, to);
        else if (S_ISREG(st.st_mode)) bad = copy_file(from, to);
    }
    closedir(d);
    return bad;
}

static void utc_stamp(char out[24]) {
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(out, 24, "%Y-%m-%dT%H-%M-%SZ", &tm);
}

/* What one path needs, from the three versions: the file's sha on this device (local), on the server (remote) and at
   the last sync (manifest); an empty sha means the file is not there. */
static int decide(const char *lm, const char *rm, const char *mm) {
    if (!strcmp(lm, rm)) return A_NONE;        // the sides agree, including both missing
    if (!strcmp(lm, mm))                       // this device is as it was at the last sync
        return *rm ? A_TAKE : A_RESTORE;       // the server changed it: take it; the server lost it: send ours back
    if (!strcmp(rm, mm))                       // the server is as it was at the last sync
        return *lm ? A_SEND : A_DELETE_REMOTE; // changed here: send it; deleted here: the deletion propagates
    return *lm ? A_CONFLICT_SEND : A_CONFLICT_DELETE;   // changed on both sides: this device's file stays the live save
}

int mng_save_sync(const char *local_root, const char *local_state, const char *staging, const char *backups,
                  const mng_sync_backend *be,
                  void (*log)(void *, const char *), void *ud,
                  int *out_downloaded, int *out_uploaded, int *out_deleted_remote, int *out_conflicts) {
    if (out_downloaded) *out_downloaded = 0;
    if (out_uploaded) *out_uploaded = 0;
    if (out_deleted_remote) *out_deleted_remote = 0;
    if (out_conflicts) *out_conflicts = 0;
    if (!local_root || !local_state || !staging || !backups || !be ||
        !be->list || !be->fetch || !be->store || !be->remove || !be->release) return -1;

    Map local = {0}, remote = {0}, manifest = {0}, next = {0};
    char **files = NULL;
    long long *sizes = NULL;
    int count = 0, bad = 0, take_any = 0, conflicts = 0, downloaded = 0, uploaded = 0, deleted_remote = 0;

    /* Nothing is applied before every remote file is in the staging folder: a failed server read leaves everything as
       it was. The local manifest is this device's memory of the last sync; the server keeps the same one. */
    bad = read_manifest(local_state, &manifest, log, ud) ||
          mkdirs(staging) || mkdirs(backups) || mkdirs(local_root) ||
          be->list(be->ctx, &files, &sizes, &count);
    if (bad) say(log, ud, "the server could not be reached; nothing was changed");
    for (int i = 0; !bad && i < count; i++) {
        if (!strcmp(files[i], MNG_SYNC_STATE_NAME)) continue;   // the manifests are the sync's own, not a save
        if (!rel_ok(files[i])) { say(log, ud, "server path refused, left out of the sync: %s", files[i]); continue; }
        char to[MAX_REL + 512];
        if (snprintf(to, sizeof to, "%s/%s", staging, files[i]) >= (int)sizeof to) { bad = -1; break; }
        char parent[MAX_REL + 512];
        snprintf(parent, sizeof parent, "%s", to);
        char *slash = strrchr(parent, '/');
        if (slash) { *slash = 0; if (mkdirs(parent)) { bad = -1; break; } }
        Entry *e = map_add(&remote, files[i]);
        if (e && !be->fetch(be->ctx, files[i], to) && !sha256_file(to, e->sha)) continue;
        say(log, ud, "reading %s from the server failed; nothing was changed", files[i]);
        bad = -1;
    }
    if (files) be->release(be->ctx, files, sizes, count);
    if (!bad) bad = walk_local(local_root, "", &local, log, ud);

    /* The merge, one path at a time: every path of the union of the three sides gets its action. */
    if (!bad) {
        for (int i = 0; i < local.count; i++) if (map_find(&next, local.e[i].path) < 0) map_add(&next, local.e[i].path);
        for (int i = 0; i < remote.count; i++) if (map_find(&next, remote.e[i].path) < 0) map_add(&next, remote.e[i].path);
        for (int i = 0; i < manifest.count; i++) if (map_find(&next, manifest.e[i].path) < 0) map_add(&next, manifest.e[i].path);
        for (int i = 0; i < next.count; i++) {
            const char *rel = next.e[i].path, *lm = sha_of(&local, rel), *rm = sha_of(&remote, rel), *mm = sha_of(&manifest, rel);
            next.e[i].action = decide(lm, rm, mm);
            if (next.e[i].action == A_TAKE) { take_any = 1; strcpy(next.e[i].sha, rm); }
            else if (next.e[i].action == A_SEND || next.e[i].action == A_RESTORE || next.e[i].action == A_CONFLICT_SEND)
                strcpy(next.e[i].sha, lm);
            if (next.e[i].action == A_CONFLICT_SEND || next.e[i].action == A_CONFLICT_DELETE) conflicts++;
        }
    }

    /* Downloads overwrite local files: the whole save root goes to the backups once, before the first of them. */
    if (!bad && take_any) {
        char stamp[24], folder[MAX_REL + 512];
        utc_stamp(stamp);
        if (snprintf(folder, sizeof folder, "%s/before-sync-%s", backups, stamp) >= (int)sizeof folder ||
            backup_tree(local_root, folder)) {
            say(log, ud, "taking the before-sync snapshot failed; nothing was changed");
            bad = -1;
        }
    }

    if (!bad) {
        char stamp[24], conflict[MAX_REL + 512];
        utc_stamp(stamp);
        if (snprintf(conflict, sizeof conflict, "%s/sync-conflict-%s", backups, stamp) >= (int)sizeof conflict) bad = -1;
        for (int i = 0; !bad && i < next.count; i++) {
            const char *rel = next.e[i].path;
            int action = next.e[i].action;
            if (action == A_NONE) continue;
            char from[MAX_REL + 512], to[MAX_REL + 512];
            /* A conflict keeps the server's copy in the backups before anything is sent or deleted, so no device's
               save can be lost by agreeing the sides again. */
            if ((action == A_CONFLICT_SEND || action == A_CONFLICT_DELETE) && *sha_of(&remote, rel)) {
                if (snprintf(from, sizeof from, "%s/%s", staging, rel) >= (int)sizeof from ||
                    snprintf(to, sizeof to, "%s/%s", conflict, rel) >= (int)sizeof to) { bad = -1; break; }
                char parent[MAX_REL + 512];
                snprintf(parent, sizeof parent, "%s", to);
                char *slash = strrchr(parent, '/');
                if (slash) { *slash = 0; if (mkdirs(parent)) { bad = -1; break; } }
                if (copy_file(from, to)) {
                    say(log, ud, "keeping the server's copy of %s failed; nothing was changed", rel);
                    bad = -1;
                    break;
                }
                say(log, ud, "the server's %s is kept in %s", rel, conflict);
            }
            if (action == A_TAKE) {
                if (snprintf(from, sizeof from, "%s/%s", staging, rel) >= (int)sizeof from ||
                    snprintf(to, sizeof to, "%s/%s", local_root, rel) >= (int)sizeof to) { bad = -1; break; }
                char parent[MAX_REL + 512];
                snprintf(parent, sizeof parent, "%s", to);
                char *slash = strrchr(parent, '/');
                if (slash) { *slash = 0; if (mkdirs(parent)) { bad = -1; break; } }
                if (copy_file(from, to)) { say(log, ud, "writing %s failed; the manifests record nothing of this sync", rel); bad = -1; break; }
                downloaded++;
            } else if (action == A_SEND || action == A_RESTORE || action == A_CONFLICT_SEND) {
                if (snprintf(from, sizeof from, "%s/%s", local_root, rel) >= (int)sizeof from) { bad = -1; break; }
                if (action == A_SEND) say(log, ud, "sending %s", rel);
                else if (action == A_RESTORE) say(log, ud, "restoring %s on the server (deleted there, unchanged here)", rel);
                else say(log, ud, "sending this device's %s (both sides had changed it)", rel);
                if (be->store(be->ctx, rel, from)) { say(log, ud, "sending %s failed; the manifests record nothing of this sync", rel); bad = -1; break; }
                uploaded++;
            } else {   // A_DELETE_REMOTE or A_CONFLICT_DELETE
                if (action == A_DELETE_REMOTE) say(log, ud, "deleting %s on the server (deleted here)", rel);
                if (be->remove(be->ctx, rel)) { say(log, ud, "deleting %s on the server failed; the manifests record nothing of this sync", rel); bad = -1; break; }
                deleted_remote++;
            }
        }
    }

    /* The manifests record what the sync leaves on both sides; only written when every change succeeded, so a failed
       run is simply redone whole. */
    if (!bad) {
        Map written = {0};
        for (int i = 0; i < next.count && !bad; i++)
            if (*next.e[i].sha) {
                Entry *e = map_add(&written, next.e[i].path);
                if (e) strcpy(e->sha, next.e[i].sha);
                else bad = -1;
            }
        if (!bad) {
            map_sort(&written);
            /* The server's manifest goes first: if only this device's were written, the next sync would take the
               server's older files for changes made elsewhere and take them over the local ones. The other order is
               safe: with the server's manifest new and this device's old, the next sync merely sends again. */
            char state[MAX_REL + 512];
            if (snprintf(state, sizeof state, "%s/%s", staging, MNG_SYNC_STATE_NAME) >= (int)sizeof state ||
                write_manifest(state, &written) ||
                be->store(be->ctx, MNG_SYNC_STATE_NAME, state)) {
                say(log, ud, "the server's manifest could not be written; the manifests record nothing of this sync");
                bad = -1;
            } else if (copy_file(state, local_state)) {
                say(log, ud, "this device's manifest could not be written; the next sync sends and takes the same files again");
                bad = -1;
            }
        }
        map_free(&written);
    }

    map_free(&local); map_free(&remote); map_free(&manifest); map_free(&next);
    if (bad) return -1;
    if (out_downloaded) *out_downloaded = downloaded;
    if (out_uploaded) *out_uploaded = uploaded;
    if (out_deleted_remote) *out_deleted_remote = deleted_remote;
    if (out_conflicts) *out_conflicts = conflicts;
    say(log, ud, "done: %d taken from the server, %d sent, %d deleted there, %d conflicts", downloaded, uploaded, deleted_remote, conflicts);
    return 0;
}
