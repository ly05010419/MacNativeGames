/* Save transactions of games/totk/runtime/fs_host.c without the game, keys or saves: every run of the game is a child
   process that mounts the save through the fs entry points the game uses; a run can be killed at a chosen step of a
   Commit (the fault hook ends the child there). Usage: test_save_transaction <empty scratch folder outside the project>. */
#include "../games/totk/runtime/fs_host.c"
#include <sys/wait.h>
#include <time.h>

_Noreturn void runtime_fail(const char *message) { fprintf(stderr, "runtime_fail: %s\n", message); exit(70); }
static uint64_t skew;
uint64_t runtime_now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + skew; }
void runtime_account_uid(uint64_t out[2]) { out[0] = 0x1111222233334444ull; out[1] = 0x5555666677778888ull; }
int runtime_romfs_owns(const char *path) { (void)path; return 0; }
int runtime_romfs_handle(uint64_t h) { (void)h; return 0; }
unsigned runtime_romfs_open(uint64_t *out, const char *path, int mode) { (void)out; (void)path; (void)mode; return R_PATH_NOT_FOUND; }
unsigned runtime_romfs_read(uint64_t *out, uint64_t h, int64_t offset, void *buffer, uint64_t size) { (void)out; (void)h; (void)offset; (void)buffer; (void)size; return R_INVALID_ARGUMENT; }
unsigned runtime_romfs_size(int64_t *out, uint64_t h) { (void)out; (void)h; return R_INVALID_ARGUMENT; }
void runtime_romfs_close(uint64_t h) { (void)h; }
unsigned runtime_romfs_entry_type(int *out, const char *path) { (void)out; (void)path; return R_PATH_NOT_FOUND; }
void runtime_autoload_save_file_opened(const char *path) { (void)path; }
void runtime_fs_log(const char *what, const char *path) { (void)what; (void)path; }
void dm_guest_write_prepare(void *addr, size_t n) { (void)addr; (void)n; }

enum { KILLED = 42 };
#define USER "55556666777788881111222233334444"
static char scratch[900], committed[1100], work[1100], fresh[1100], old[1100], expect[64];
static const char *kill_at, *fail_at; static int fail_errno, fail_times;
static int hook(const char *step) {
    if (kill_at && !strcmp(step, kill_at)) { fflush(NULL); _exit(KILLED); }   /* the process is killed here */
    if (fail_at && fail_times && !strcmp(step, fail_at)) { fail_times--; return fail_errno; }
    return 0;
}
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

/* ---------- the game's side (in a child) ---------- */
static void mount(void) { uint64_t uid[2]; runtime_account_uid(uid); CHECK(mount_save("save", uid) == 0); }
static int gread(const char *path, char *buf, size_t cap) {
    uint64_t h, n = 0; if (open_file(&h, path, OPEN_READ)) return -1;
    CHECK(read_file(&n, h, 0, buf, cap - 1) == 0); close_file(h); buf[n] = 0; return (int)n;
}
static int gis(const char *path, const char *text) { char b[256]; return gread(path, b, sizeof b) >= 0 && !strcmp(b, text); }
static unsigned gwrite(const char *path, const char *text) {
    uint64_t h; int type; unsigned r;
    if (entry_type(&type, path) == R_PATH_NOT_FOUND && (r = create_file(path, 0))) return r;
    if ((r = open_file(&h, path, OPEN_WRITE | OPEN_APPEND))) return r;
    r = set_file_size(h, 0); if (!r) r = write_file(h, 0, text, strlen(text), NULL);
    close_file(h); return r;
}
static int gabsent(const char *path) { int type; return entry_type(&type, path) == R_PATH_NOT_FOUND; }

/* ---------- the host folders (parent or child) ---------- */
static int host_is(const char *dir, const char *rel, const char *text) {
    char p[1300], b[256] = {0}; snprintf(p, sizeof p, "%s/%s", dir, rel); FILE *f = fopen(p, "rb"); if (!f) return 0;
    size_t n = fread(b, 1, sizeof b - 1, f); fclose(f); b[n] = 0; return !strcmp(b, text);
}
static int exists(const char *p) { struct stat st; return !lstat(p, &st); }
static void host_write(const char *dir, const char *rel, const char *text) {
    char p[1300]; snprintf(p, sizeof p, "%s/%s", dir, rel); FILE *f = fopen(p, "wb"); CHECK(f && fputs(text, f) >= 0 && !fclose(f));
}

/* ---------- runs ---------- */
static void run_old_layout(void) {   /* a save written by earlier builds, straight into <profile>/save/<user> */
    mount();
    CHECK(strstr(save_cur, "/." USER ".work"));
    CHECK(gis("save:/slot_00/progress.sav", "old-build") && gis("save:/option.sav", "opt"));
    uint64_t d; int64_t n = 0; DirectoryEntry e[8];
    CHECK(open_directory(&d, "save:/", 3) == 0 && read_directory(&n, e, d, 8) == 0); close_directory(d);
    CHECK(n == 2);                                         /* the game sees only its own files */
    CHECK(host_is(committed, "slot_00/progress.sav", "old-build"));
}
static void run_commit(void) {
    mount();
    uint64_t h; CHECK(open_file(&h, "save:/slot_00/progress.sav", OPEN_WRITE | OPEN_APPEND) == 0);
    CHECK(set_file_size(h, 0) == 0 && write_file(h, 0, "A1", 2, NULL) == 0);
    CHECK(host_is(committed, "slot_00/progress.sav", "old-build"));   /* not durable before Commit */
    CHECK(commit("save") == 0 && host_is(committed, "slot_00/progress.sav", "A1"));
    CHECK(write_file(h, 0, "B2", 2, NULL) == 0); close_file(h);          /* a file open across a Commit stays in the working copy */
    CHECK(host_is(committed, "slot_00/progress.sav", "A1") && commit("save") == 0 && host_is(committed, "slot_00/progress.sav", "B2"));
    CHECK(gwrite("save:/slot_00/progress.sav", "v1") == 0 && create_directory("save:/slot_01") == 0 && gwrite("save:/slot_01/progress.sav", "s1") == 0);
    CHECK(commit("save") == 0 && commit("save") == 0);    /* a second Commit without changes publishes nothing new */
    CHECK(host_is(committed, "slot_00/progress.sav", "v1") && host_is(committed, "slot_01/progress.sav", "s1") && !exists(fresh) && !exists(old));
}
static void run_uncommitted(void) {   /* the game changes everything, then the app is killed before Commit */
    mount();
    CHECK(gwrite("save:/slot_00/progress.sav", "lost") == 0 && create_file("save:/extra.sav", 4) == 0 && delete_file("save:/option.sav") == 0);
    CHECK(rename_file("save:/slot_01/progress.sav", "save:/slot_01/renamed.sav") == 0 && delete_recursive("save:/slot_00") == 0 && create_directory("save:/slot_09") == 0);
    CHECK(gabsent("save:/slot_00/progress.sav") && !gabsent("save:/extra.sav"));   /* the game sees its own changes */
    CHECK(host_is(committed, "slot_00/progress.sav", "v1") && host_is(committed, "option.sav", "opt"));
    fflush(NULL); _exit(KILLED);
}
static void run_expect(void) {   /* the next run: the save as of the last Commit */
    mount();
    CHECK(gis("save:/slot_00/progress.sav", expect) && gis("save:/slot_01/progress.sav", "s1") && gis("save:/option.sav", "opt"));
    CHECK(gabsent("save:/extra.sav") && gabsent("save:/slot_01/renamed.sav") && gabsent("save:/slot_09"));
    CHECK(exists(committed) && !exists(fresh) && !exists(old) && host_is(committed, "slot_00/progress.sav", expect));
}
static void run_commit_value(void) { mount(); CHECK(gwrite("save:/slot_00/progress.sav", expect) == 0); CHECK(commit("save") == 0 && host_is(committed, "slot_00/progress.sav", expect)); }
static void run_disk_full_write(void) {
    mount();
    fail_at = "write"; fail_errno = ENOSPC; fail_times = 1;
    CHECK(gwrite("save:/slot_00/progress.sav", "v7") == R_USABLE_SPACE_NOT_ENOUGH && gwrite("save:/slot_01/progress.sav", "s7") == 0);
    CHECK(commit("save") == R_USABLE_SPACE_NOT_ENOUGH);  /* half a save does not land */
    CHECK(host_is(committed, "slot_00/progress.sav", "v6") && host_is(committed, "slot_01/progress.sav", "s1"));
    CHECK(gis("save:/slot_00/progress.sav", "v6") && gis("save:/slot_01/progress.sav", "s1"));   /* rolled back, as Horizon's journal */
    CHECK(gwrite("save:/slot_00/progress.sav", "v8") == 0 && commit("save") == 0 && host_is(committed, "slot_00/progress.sav", "v8"));
}
static void run_reset_retry(void) {   /* the disk stays full at the refused Commit: the working copy cannot be reset yet */
    mount();
    fail_at = "write"; fail_errno = ENOSPC; fail_times = 1;
    CHECK(gwrite("save:/slot_00/progress.sav", "lost") == R_USABLE_SPACE_NOT_ENOUGH);
    fail_at = "clonefile"; fail_times = 1;
    CHECK(commit("save") == R_USABLE_SPACE_NOT_ENOUGH && txn_reset_pending && host_is(committed, "slot_00/progress.sav", "v8"));
    fail_times = 1;                                        /* still full at the next change: refused, nothing written */
    CHECK(gwrite("save:/slot_00/progress.sav", "lost") == R_USABLE_SPACE_NOT_ENOUGH && txn_reset_pending);
    CHECK(gwrite("save:/slot_01/progress.sav", "s1") == 0 && !txn_reset_pending);   /* room again: the next save starts clean */
    CHECK(gis("save:/slot_00/progress.sav", "v8") && commit("save") == 0 && host_is(committed, "slot_00/progress.sav", "v8"));
}
static void run_disk_full_mount(void) {
    fail_at = "clonefile"; fail_errno = ENOSPC; fail_times = 1;
    mount();
    CHECK(save_readonly && !strcmp(save_cur, committed) && gis("save:/slot_00/progress.sav", "v8"));   /* read-only: the committed save */
    uint64_t h;
    CHECK(gwrite("save:/slot_00/progress.sav", "v9") == R_USABLE_SPACE_NOT_ENOUGH && open_file(&h, "save:/option.sav", OPEN_WRITE) == R_USABLE_SPACE_NOT_ENOUGH);
    CHECK(create_directory("save:/slot_05") == R_USABLE_SPACE_NOT_ENOUGH && delete_file("save:/option.sav") == R_USABLE_SPACE_NOT_ENOUGH);
    CHECK(commit("save") == R_USABLE_SPACE_NOT_ENOUGH && host_is(committed, "slot_00/progress.sav", "v8") && host_is(committed, "option.sav", "opt"));
    skew += 6000000000ull;                                 /* room again, 6 s later: the next change makes the working copy */
    CHECK(gwrite("save:/slot_00/progress.sav", "v9") == 0 && !save_readonly && strstr(save_cur, ".work"));
    CHECK(host_is(committed, "slot_00/progress.sav", "v8") && commit("save") == 0 && host_is(committed, "slot_00/progress.sav", "v9"));
}
static void run_disk_full_commit(void) {
    mount(); CHECK(gwrite("save:/slot_00/progress.sav", "v10") == 0);
    fail_at = "clonefile"; fail_errno = ENOSPC; fail_times = 1;
    CHECK(commit("save") == R_USABLE_SPACE_NOT_ENOUGH && host_is(committed, "slot_00/progress.sav", "v9") && !exists(fresh));
    CHECK(commit("save") == 0 && host_is(committed, "slot_00/progress.sav", "v10"));   /* the next Commit lands it */
}
static void run_runtime_probe(void) {   /* make probe-save's checks of runtime_fs_save_busy, on the transactional save */
    char p[1000]; snprintf(p, sizeof p, "%s/probe-cwd", scratch); CHECK(!mkdir(p, 0700) && !chdir(p) && !mkdir("build", 0700));
    runtime_save_probe();
}
static double ms(uint64_t a, uint64_t b) { return (b - a) / 1e6; }
static void run_timing(void) {   /* a save the size of a TOTK one: 20 files, 18 MB */
    mount();
    uint64_t t0 = runtime_now_ns(); CHECK(gwrite("save:/slot_00/caption.sav", "x") == 0 && commit("save") == 0);
    uint64_t t1 = runtime_now_ns();
    fail_at = "clonefile"; fail_errno = ENOTSUP; fail_times = 1;   /* a volume that cannot clone: a full copy */
    CHECK(gwrite("save:/slot_00/caption.sav", "y") == 0 && commit("save") == 0);
    uint64_t t2 = runtime_now_ns();
    uint64_t t3 = runtime_now_ns(); CHECK(!clone_tree(save_work, save_new)); uint64_t t4 = runtime_now_ns(); sync_dir(save_parent); uint64_t t5 = runtime_now_ns();
    CHECK(!remove_tree(save_new));
    printf("timing for 20 files / 18 MB: Commit %.2f ms on APFS (of it the clone %.2f ms, the F_FULLFSYNC %.2f ms), %.2f ms copying (no clonefile)\n",
           ms(t0, t1), ms(t3, t4), ms(t4, t5), ms(t1, t2));
    CHECK(host_is(committed, "slot_00/caption.sav", "y"));
}

static int run(void (*fn)(void)) {
    fflush(NULL); pid_t p = fork(); CHECK(p >= 0);
    if (!p) { txn_fault = hook; fn(); fflush(NULL); _exit(0); }
    int st; CHECK(waitpid(p, &st, 0) == p); kill_at = fail_at = NULL; fail_times = 0;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}
static int cases;
#define RUN(fn, want) do { int got = run(fn); if (got != (want)) { fprintf(stderr, "FAIL %s: exit %d, expected %d\n", #fn, got, want); return 1; } cases++; printf("PASS %s\n", #fn); } while (0)
static void use_profile(const char *name) {
    char p[1000]; snprintf(p, sizeof p, "%s/%s", scratch, name); CHECK(!mkdir(p, 0700)); setenv("TOTK_PROFILE_DIR", p, 1);
    snprintf(committed, sizeof committed, "%s/save/" USER, p); snprintf(work, sizeof work, "%s/save/." USER ".work", p);
    snprintf(fresh, sizeof fresh, "%s/save/." USER ".new", p); snprintf(old, sizeof old, "%s/save/." USER ".old", p);
}
int main(int argc, char **argv) {
    if (argc != 2 || snprintf(scratch, sizeof scratch, "%s", argv[1]) >= (int)sizeof scratch) { fprintf(stderr, "usage: %s <empty scratch folder>\n", argv[0]); return 2; }
    use_profile("profile");
    char p[1300]; snprintf(p, sizeof p, "%s/save", getenv("TOTK_PROFILE_DIR")); CHECK(!mkdir(p, 0700) && !mkdir(committed, 0700));
    snprintf(p, sizeof p, "%s/slot_00", committed); CHECK(!mkdir(p, 0700));
    host_write(committed, "slot_00/progress.sav", "old-build"); host_write(committed, "option.sav", "opt");

    RUN(run_old_layout, 0);
    CHECK(exists(work) && host_is(committed, "slot_00/progress.sav", "old-build"));
    RUN(run_commit, 0);
    strcpy(expect, "v1"); RUN(run_expect, 0);
    RUN(run_uncommitted, KILLED);
    CHECK(exists(work) && host_is(committed, "slot_00/progress.sav", "v1"));
    RUN(run_expect, 0);                                    /* uncommitted changes are gone, the last commit is there */

    strcpy(expect, "v2"); kill_at = "swapped"; RUN(run_commit_value, KILLED);   /* killed after the swap, before the old version is deleted */
    CHECK(host_is(committed, "slot_00/progress.sav", "v2") && host_is(fresh, "slot_00/progress.sav", "v1"));
    RUN(run_expect, 0);
    strcpy(expect, "v3"); kill_at = "swap"; RUN(run_commit_value, KILLED);      /* killed with .new complete, before the swap */
    CHECK(host_is(committed, "slot_00/progress.sav", "v2") && host_is(fresh, "slot_00/progress.sav", "v3"));
    strcpy(expect, "v2"); RUN(run_expect, 0);
    /* A volume without RENAME_SWAP, killed between its two renames: no committed folder until the next run finishes it */
    strcpy(expect, "v4"); fail_at = "swap"; fail_errno = ENOTSUP; fail_times = 1; kill_at = "renamed"; RUN(run_commit_value, KILLED);
    CHECK(!exists(committed) && host_is(fresh, "slot_00/progress.sav", "v4") && host_is(old, "slot_00/progress.sav", "v2"));
    RUN(run_expect, 0);
    strcpy(expect, "v5"); fail_at = "swap"; fail_errno = ENOTSUP; fail_times = 1; RUN(run_commit_value, 0);
    RUN(run_expect, 0);
    strcpy(expect, "v6"); fail_at = "clonefile"; fail_errno = ENOTSUP; fail_times = 9; RUN(run_commit_value, 0);   /* copies, no clones */
    RUN(run_expect, 0);
    RUN(run_disk_full_write, 0);
    RUN(run_reset_retry, 0);
    RUN(run_disk_full_mount, 0);
    RUN(run_disk_full_commit, 0);
    strcpy(expect, "v10"); RUN(run_expect, 0);
    RUN(run_runtime_probe, 0);

    use_profile("timing");
    snprintf(p, sizeof p, "%s/save", getenv("TOTK_PROFILE_DIR")); CHECK(!mkdir(p, 0700) && !mkdir(committed, 0700));
    char *block = malloc(900 << 10); CHECK(block != NULL);
    for (int i = 0; i < 20; i++) {
        snprintf(p, sizeof p, "%s/slot_%02d", committed, i / 4); mkdir(p, 0700);
        snprintf(p, sizeof p, "%s/slot_%02d/file%d.sav", committed, i / 4, i); FILE *f = fopen(p, "wb");
        for (size_t k = 0; k < (900 << 10); k++) block[k] = (char)(k * 2654435761u >> (i % 7));
        CHECK(f && fwrite(block, 1, 900 << 10, f) == (900 << 10) && !fclose(f));
    }
    free(block);
    RUN(run_timing, 0);
    printf("%d save transaction cases passed\n", cases);
    return 0;
}
