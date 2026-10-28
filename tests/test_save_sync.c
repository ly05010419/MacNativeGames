/* The save-sync merge (games/totk/app/save_sync.c) against plain folders: one device's save root, a folder standing in
   for the server, and the manifest of the last sync. No game files, no network. */
#include "../games/totk/app/save_sync.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int checks = 0, failures = 0;
#define CHECK(cond, what) do { checks++; if (!(cond)) { fprintf(stderr, "save-sync test: %s\n", what); failures++; } } while (0)

static char test_root[512], local_root[512], server_root[512], state[512], staging[512], backups[512];

static void mkdirs(const char *path) {
    char copy[512];
    snprintf(copy, sizeof copy, "%s", path);
    for (char *p = copy + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(copy, 0700); *p = '/'; }
    mkdir(copy, 0700);
}

static void put(const char *base, const char *rel, const char *text) {
    char path[1024], parent[1024];
    snprintf(path, sizeof path, "%s/%s", base, rel);
    snprintf(parent, sizeof parent, "%s", path);
    *strrchr(parent, '/') = 0;
    mkdirs(parent);
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); exit(1); }
    fputs(text, f);
    fclose(f);
}

static int same(const char *base, const char *rel, const char *text) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", base, rel);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char buf[4096] = "";
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    return strlen(text) == n && !memcmp(buf, text, n);
}

static void unlink_in(const char *base, const char *rel) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", base, rel);
    unlink(path);
}

/* The backend: a plain folder standing in for the server. */
typedef struct { char **files; long long *sizes; int count, cap; } Listing;
static int list_walk(const char *dir, const char *prefix, Listing *out) {
    DIR *d = opendir(dir);
    if (!d) return errno == ENOENT ? 0 : -1;
    struct dirent *e;
    int bad = 0;
    while (!bad && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char path[1024], rel[1024];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        snprintf(rel, sizeof rel, "%s%s", prefix, e->d_name);
        struct stat st;
        if (stat(path, &st)) continue;
        if (S_ISDIR(st.st_mode)) {
            char next[1024];
            snprintf(next, sizeof next, "%s/", rel);
            bad = list_walk(path, next, out);
            continue;
        }
        if (out->count == out->cap) {
            out->cap = out->cap ? out->cap * 2 : 16;
            out->files = realloc(out->files, (size_t)out->cap * sizeof *out->files);
            out->sizes = realloc(out->sizes, (size_t)out->cap * sizeof *out->sizes);
        }
        out->files[out->count] = strdup(rel);
        out->sizes[out->count] = st.st_size;
        out->count++;
    }
    closedir(d);
    return bad;
}
static int dir_list(void *ctx, char ***files, long long **sizes, int *count) {
    Listing out = {0};
    if (list_walk(ctx, "", &out)) return -1;
    *files = out.files;
    *sizes = out.sizes;
    *count = out.count;
    return 0;
}
static int dir_fetch(void *ctx, const char *rel, const char *to) {
    char from[1024];
    snprintf(from, sizeof from, "%s/%s", (const char *)ctx, rel);
    FILE *in = fopen(from, "rb");
    if (!in) return -1;
    FILE *out = fopen(to, "wb");
    if (!out) { fclose(in); return -1; }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, n, out);
    fclose(in);
    return fclose(out) ? -1 : 0;
}
static int dir_store(void *ctx, const char *rel, const char *from) {
    char to[1024], parent[1024];
    snprintf(to, sizeof to, "%s/%s", (const char *)ctx, rel);
    snprintf(parent, sizeof parent, "%s", to);
    *strrchr(parent, '/') = 0;
    mkdirs(parent);
    FILE *in = fopen(from, "rb");
    if (!in) return -1;
    FILE *out = fopen(to, "wb");
    if (!out) { fclose(in); return -1; }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, n, out);
    fclose(in);
    return fclose(out) ? -1 : 0;
}
static int dir_remove(void *ctx, const char *rel) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", (const char *)ctx, rel);
    return unlink(path) && errno != ENOENT ? -1 : 0;
}
static void dir_release(void *ctx, char **files, long long *sizes, int count) {
    (void)ctx;
    for (int i = 0; i < count; i++) free(files[i]);
    free(files);
    free(sizes);
}

static int run_sync(int *downloaded, int *uploaded, int *deleted_remote, int *conflicts) {
    mng_sync_backend be = {server_root, dir_list, dir_fetch, dir_store, dir_remove, dir_release};
    return mng_save_sync(local_root, state, staging, backups, &be, NULL, NULL,
                         downloaded, uploaded, deleted_remote, conflicts);
}

static void fresh(void) {
    snprintf(test_root, sizeof test_root, "/tmp/test-save-sync-%d", getpid());
    char cmd[1152];
    snprintf(cmd, sizeof cmd, "rm -rf %s && mkdir -p %s", test_root, test_root);
    if (system(cmd)) exit(1);
    snprintf(local_root, sizeof local_root, "%s/local", test_root);
    snprintf(server_root, sizeof server_root, "%s/server", test_root);
    snprintf(state, sizeof state, "%s/local-manifest.txt", test_root);
    snprintf(staging, sizeof staging, "%s/staging", test_root);
    snprintf(backups, sizeof backups, "%s/backups", test_root);
    mkdirs(local_root);
    mkdirs(server_root);
}

static int backup_holds(const char *prefix, const char *rel, const char *text) {
    DIR *d = opendir(backups);
    if (!d) return 0;
    int found = 0;
    for (struct dirent *e; !found && (e = readdir(d));)
        if (!strncmp(e->d_name, prefix, strlen(prefix))) {
            char path[1024];
            snprintf(path, sizeof path, "%s/%s/%s", backups, e->d_name, rel);
            FILE *f = fopen(path, "r");
            char buf[4096] = "";
            size_t n = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
            if (f) fclose(f);
            found = strlen(text) == n && !memcmp(buf, text, n);
        }
    closedir(d);
    return found;
}

int main(void) {
    int got = 0, up = 0, del = 0, con = 0;

    /* First sync, both sides empty: nothing to do, the local manifest appears. */
    fresh();
    CHECK(!run_sync(&got, &up, &del, &con), "empty sync runs");
    CHECK(!got && !up && !del && !con, "empty sync changes nothing");
    CHECK(access(state, F_OK) == 0, "the local manifest exists");

    /* Local saves, no server files, no manifest yet: everything is sent. */
    fresh();
    put(local_root, "slot_00/progress.sav", "progress one");
    put(local_root, "slot_00/caption.sav", "caption one");
    CHECK(!run_sync(&got, &up, &del, &con), "first sync runs");
    CHECK(up == 2 && !got && !del && !con, "two files sent");
    CHECK(same(server_root, "slot_00/progress.sav", "progress one"), "the server has the save");
    CHECK(access(state, F_OK) == 0, "the local manifest exists");
    CHECK(!run_sync(&got, &up, &del, &con) && !got && !up && !del && !con, "the second sync is a no-op");

    /* A new file on the server is taken; a local change is sent. */
    fresh();
    put(local_root, "slot_00/progress.sav", "progress one");
    CHECK(!run_sync(&got, &up, &del, &con), "seed sync runs");
    put(local_root, "slot_00/progress.sav", "progress two");
    put(server_root, "slot_01/caption.sav", "caption from the other device");
    CHECK(!run_sync(&got, &up, &del, &con), "merge sync runs");
    CHECK(got == 1 && up == 1 && !del && !con, "one taken, one sent");
    CHECK(same(local_root, "slot_00/progress.sav", "progress two"), "the local change stays");
    CHECK(same(server_root, "slot_00/progress.sav", "progress two"), "the local change reached the server");
    CHECK(same(local_root, "slot_01/caption.sav", "caption from the other device"), "the server's file arrived");
    CHECK(!run_sync(&got, &up, &del, &con) && !got && !up && !del && !con, "merged state is a no-op");

    /* Both sides changed the same file: the local file stays, the server's copy goes to the backups, both agree after. */
    fresh();
    put(local_root, "slot_00/progress.sav", "version one");
    CHECK(!run_sync(&got, &up, &del, &con), "conflict seed runs");
    put(local_root, "slot_00/progress.sav", "edited on this device");
    put(server_root, "slot_00/progress.sav", "edited on the other device");
    CHECK(!run_sync(&got, &up, &del, &con), "conflict sync runs");
    CHECK(con == 1 && up == 1 && !got && !del, "one conflict, resolved by sending");
    CHECK(same(local_root, "slot_00/progress.sav", "edited on this device"), "the local file stays the live save");
    CHECK(same(server_root, "slot_00/progress.sav", "edited on this device"), "the sides agree again");
    CHECK(backup_holds("sync-conflict-", "slot_00/progress.sav", "edited on the other device"),
          "the backup holds the server's version");
    CHECK(!run_sync(&got, &up, &del, &con) && !got && !up && !del && !con, "the resolved conflict is a no-op");

    /* Deleted here propagates to the server; deleted on the server is answered by sending the file again. */
    fresh();
    put(local_root, "slot_00/progress.sav", "kept");
    put(local_root, "slot_01/progress.sav", "to delete");
    CHECK(!run_sync(&got, &up, &del, &con), "deletion seed runs");
    unlink_in(local_root, "slot_01/progress.sav");
    unlink_in(server_root, "slot_00/progress.sav");
    CHECK(!run_sync(&got, &up, &del, &con), "deletion sync runs");
    CHECK(del == 1 && up == 1 && !got && !con, "one deletion propagated, one answered");
    CHECK(!same(server_root, "slot_01/progress.sav", "to delete"), "the server copy of the deleted file is gone");
    CHECK(same(server_root, "slot_00/progress.sav", "kept"), "the server's deletion was not honored");
    CHECK(same(local_root, "slot_00/progress.sav", "kept"), "the local file was never deleted");

    /* A manifest naming paths outside the save root is ignored, and nothing lands outside it. */
    fresh();
    put(local_root, "slot_00/progress.sav", "kept");
    FILE *m = fopen(state, "w");
    if (!m) exit(1);
    fprintf(m, "deadbeef ../evil.txt\n");
    fclose(m);
    CHECK(!run_sync(&got, &up, &del, &con), "a manifest with a traversal line does not stop the sync");
    char outside[1024];
    snprintf(outside, sizeof outside, "%s/evil.txt", test_root);
    CHECK(access(outside, F_OK) != 0, "nothing was written outside the save root");
    CHECK(up == 1, "the sync still sent the local save");
    CHECK(!run_sync(&got, &up, &del, &con) && !up, "the next sync is a no-op");

    /* The manifest of the sync itself is not a save: on the server, it is neither merged nor taken. */
    fresh();
    put(local_root, "slot_00/progress.sav", "kept");
    put(server_root, "sync-manifest.txt", "not a save");
    CHECK(!run_sync(&got, &up, &del, &con), "sync with a server manifest file runs");
    CHECK(!same(local_root, "sync-manifest.txt", "not a save"), "the sync's manifest is not taken as a save");

    printf("{\"status\":\"%s\",\"probe\":\"test_save_sync\",\"checks\":%d,\"failures\":%d,"
           "\"scope\":\"three-way merge of app/save_sync.c: send, take, conflict backup, deletion propagation, server-deletion restore, manifest safety\"}\n",
           failures ? "fail" : "pass", checks, failures);
    return failures ? 1 : 0;
}
