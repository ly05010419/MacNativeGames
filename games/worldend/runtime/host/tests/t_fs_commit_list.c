// Exercise the exact mount-list Commit ABI with synthetic files, no guest SDK.
#include <sys/types.h>
static ssize_t fake_pwrite(int fd, const void *buf, size_t n, off_t off);
#define pwrite fake_pwrite
#include "../hle_fs.c"
#undef pwrite
#include <mach-o/getsect.h>
ssize_t pwrite(int, const void *, size_t, off_t);

struct dm_flags g_flag; uint64_t g_t0_ns;
uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_MONOTONIC); }
void dm_prof_add(int id, uint64_t ns, uint64_t b) { (void)id; (void)ns; (void)b; }
void dm_guest_write_prepare(void *a, size_t n) { (void)a; (void)n; }
extern int g_nfail;
static int fail_write, write_calls;
static ssize_t fake_pwrite(int fd, const void *buf, size_t n, off_t off) {
    if (fail_write) { if (++write_calls > 10) { fputs("FAIL: retrying a zero-byte write\n", stderr); exit(1); } return 0; }
    return pwrite(fd, buf, n, off);
}
static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)
static void put(const char *path, const char *s) {
    FILE *f = fopen(path, "w"); if (!f) { perror(path); exit(2); } fputs(s, f); fclose(f);
}
static int same(const char *path, const char *s) {
    char b[64] = {0}; FILE *f = fopen(path, "r"); if (!f) return 0;
    size_t n = fread(b, 1, sizeof b - 1, f); fclose(f);
    return n == strlen(s) && !memcmp(b, s, n);
}
static Result write_save(const char *path, const char *s) {
    uint64_t h; Result r = h_fs_OpenFile(&h, path, MODE_WRITE); if (r) return r;
    r = h_fs_SetFileSize(h, (int64_t)strlen(s));
    if (!r) r = h_fs_WriteFile(h, 0, s, strlen(s), NULL);
    h_fs_CloseFile(h); return r;
}
static int read_save(const char *path, char *out, size_t n) {
    uint64_t h; size_t got = 0; if (h_fs_OpenFile(&h, path, MODE_READ)) return 0;
    Result r = h_fs_ReadFile(&got, h, 0, out, n - 1); h_fs_CloseFile(h); out[got] = 0; return !r;
}
extern const struct mach_header_64 _mh_execute_header;
static Result (*list_hook(void))(const char *const *, int) {
    unsigned long bytes = 0;
    const hle_force_ent *e = (const hle_force_ent *)getsectiondata(&_mh_execute_header, "__DATA", "swforce", &bytes);
    for (size_t i = 0; e && i < bytes / sizeof *e; i++)
        if (!strcmp(e[i].name, "_ZN2nn2fs6CommitEPKPKci")) return e[i].fn;
    return NULL;
}
int main(void) {
    unsetenv("DM_TEST_FS_FAIL"); unsetenv("DM_TEST_FS_FAILAT");
    Result (*commit)(const char *const *, int) = list_hook();
    CHECK(commit != NULL, "exact mount-list Commit symbol is registered");
    if (!commit) return 1;
    const char *one[] = {"save"};
    char root[1024], save[1100], other[1100], path[1200], bpath[1200];
    snprintf(root, sizeof root, "%s/list-commit.XXXXXX", getenv("TMPDIR") ?: "/private/tmp");
    if (!mkdtemp(root)) return 2;
    snprintf(save, sizeof save, "%s/save", root); mkdir(save, 0755);
    snprintf(path, sizeof path, "%s/a", save); put(path, "old"); setenv("DM_SAVE", save, 1);
    CHECK(h_fs_MountSave("save", NULL) == RES_OK, "mount transactional output");
    CHECK(write_save("save:/a", "new") == RES_OK && same(path, "old"), "writes remain uncommitted");
    uint64_t n = dm_fs_commit_count();
    CHECK(commit(one, 1) == RES_OK && same(path, "new"), "one-element list commits actual file bytes");
    CHECK(dm_fs_commit_count() == n + 1, "one transaction");
    g_nmnt = 0; // Simulate a fresh mount after process restart.
    CHECK(h_fs_MountSave("save", NULL) == RES_OK, "remount committed data");
    char got[64]; CHECK(read_save("save:/a", got, sizeof got) && !strcmp(got, "new"), "committed bytes survive remount");
    fail_write = 1; write_calls = 0;
    CHECK(write_save("save:/a", "bad") == RES_FAIL, "inject a failed write"); fail_write = 0;
    n = dm_fs_commit_count(); int nf = g_nfail;
    CHECK(commit(one, 1) != RES_OK && same(path, "new"), "poisoned list commit fails without publishing bad bytes");
    CHECK(dm_fs_commit_count() == n && g_nfail == nf + 1, "refused transaction not counted; front end notified");
    CHECK(write_save("save:/a", "good") == RES_OK && commit(one, 1) == RES_OK && same(path, "good"), "next clean transaction succeeds");
    CHECK(write_save("save:/a", "pending0") == RES_OK, "stage a write before the alias is mounted");
    CHECK(h_fs_MountSave("alias", NULL) == RES_OK, "mount an alias of the same host transaction");
    CHECK(read_save("alias:/a", got, sizeof got) && !strcmp(got, "pending0") && same(path, "good"), "the alias shares the pending write instead of recloning over it");
    const char *aliases[] = {"save", "alias", "save"};
    CHECK(write_save("alias:/a", "alias-new") == RES_OK, "write through alias"); n = dm_fs_commit_count();
    CHECK(commit(aliases, 3) == RES_OK && same(path, "alias-new"), "aliases sharing one backing transaction commit together");
    CHECK(dm_fs_commit_count() == n + 1, "duplicates do not commit twice");
    CHECK(write_save("save:/a", "pending") == RES_OK, "stage bytes before validation tests"); n = dm_fs_commit_count();
    const char *unknown[] = {"save", "absent"}, *null_name[] = {"save", NULL};
    CHECK(commit(unknown, 2) != RES_OK && same(path, "alias-new"), "unknown name rejects the whole request before any commit");
    CHECK(commit(null_name, 2) != RES_OK, "null name rejected");
    CHECK(commit(NULL, 1) != RES_OK && commit(one, -1) != RES_OK && commit(one, MAX_MOUNT + 1) != RES_OK, "invalid pointer/count combinations rejected");
    CHECK(commit(NULL, 0) == RES_OK, "empty list is a no-op");
    CHECK(mount("rom", root, 1) == RES_OK, "mount non-save input"); const char *non_save[] = {"save", "rom"};
    CHECK(commit(non_save, 2) != RES_OK && same(path, "alias-new"), "read-only mount cannot produce partial success");
    CHECK(dm_fs_commit_count() == n, "rejected/empty lists do not publish");
    if (geteuid() != 0) {
        CHECK(chmod(root, 0500) == 0, "refuse creation of commit snapshot");
        Result denied = commit(one, 1); chmod(root, 0700);
        CHECK(denied != RES_OK && same(path, "alias-new") && dm_fs_commit_count() == n, "clone failure propagates; committed data unchanged");
        CHECK(commit(one, 1) == RES_OK && same(path, "pending"), "retry after snapshot failure commits the same staged bytes");
    } else puts("SKIP permission failure injection when running as root");
    snprintf(bpath, sizeof bpath, "%s/temp", root); setenv("DM_TEMP", bpath, 1);
    CHECK(h_fs_MountCache("cache", 0) == RES_OK, "mount cache storage (writable, no host transaction)");
    const char *with_cache[] = {"save", "cache"}, *cache_only[] = {"cache"};
    CHECK(write_save("save:/a", "with-cache") == RES_OK, "stage bytes beside cache storage"); n = dm_fs_commit_count();
    CHECK(commit(with_cache, 2) == RES_OK && same(path, "with-cache") && dm_fs_commit_count() == n + 1, "save + cache storage commits the save once");
    CHECK(commit(cache_only, 1) == RES_OK && dm_fs_commit_count() == n + 1, "cache storage alone is a no-op, as the single-name Commit");
    snprintf(other, sizeof other, "%s/other", root); mkdir(other, 0755);
    snprintf(bpath, sizeof bpath, "%s/a", other); put(bpath, "old-B"); setenv("DM_SAVE", other, 1);
    CHECK(h_fs_MountSave("other", NULL) == RES_OK, "mount independent transaction");
    CHECK(write_save("save:/a", "new-A") == RES_OK && write_save("other:/a", "new-B") == RES_OK, "stage independent changes");
    const char *multi[] = {"save", "other"}; n = dm_fs_commit_count();
    CHECK(commit(multi, 2) != RES_OK, "independent multi-save atomic commit explicitly unsupported");
    CHECK(same(path, "with-cache") && same(bpath, "old-B") && dm_fs_commit_count() == n, "unsupported request publishes neither transaction");
    rm_tree(root);
    printf("list Commit: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
