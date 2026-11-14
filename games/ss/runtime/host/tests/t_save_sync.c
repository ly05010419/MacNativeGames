// Cloud save sync unit test (takes seconds): dm/host/save_sync.c through hle_fs.c's own save mount and Commit, with a plain folder standing in for iCloud Drive.
// Each device's game session runs in a child process (the host's sync state is once per process, as it is once per start of the app); the parent checks what each left
// in its save, its backups and the cloud. All expected values are written by hand from the semantics in save_sync.h:
//   (1) packing: a save with folders, a guest name and a device save comes back the same (contents, guest name, dates); the hash ignores dates and .DS_Store and sees contents;
//       a file cut short, a flipped byte, a byte too many and a path out of the save are refused
//   (2) ss_decide's table
//   (3) Mac A has a save and the cloud none: A's session sends it; iPhone B has none: B's session brings it in; B saves: the cloud has B's
//   (4) A starts again: only the cloud's changed: it comes in, and A's old save is kept in A's backups ("local")
//   (5) both changed, the cloud's newer: B played without sync, A sent a newer save; B starts with sync: A's comes in, B's is kept
//   (6) both changed, this one newer: A played without sync later than B sent; A starts with sync: A's goes out, B's is kept in A's backups ("cloud")
//   (7) the Mac launcher's send after the host exited: a save the host committed and did not send goes out
//   (8) the game's own folder (<folder>/<title ID>) chosen as the folder
//   (9) a new backup removes the oldest of its side beyond 10, and leaves the other side's and the player's own folders
//   (10) the launcher's DM_SAVE with a trailing slash: the same <save>.sync as the host's
#include "../hle_fs.c"
#include <ctype.h>
#include <sys/wait.h>
#include <sys/time.h>

struct dm_flags g_flag; uint64_t g_t0_ns;
uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_MONOTONIC); }
void dm_guest_write_prepare(void *a, size_t n) { (void)a; (void)n; }

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("  ✗ "); } else printf("  ✓ "); printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)
static void put(const char *p, const char *s) { FILE *f = fopen(p, "w"); fputs(s, f); fclose(f); }
static int same(const char *p, const char *s) {
    char b[64] = {0}; FILE *f = fopen(p, "r"); if (!f) return 0; size_t k = fread(b, 1, sizeof b - 1, f); fclose(f);
    return k == strlen(s) && !memcmp(b, s, k);
}
static void mkd(const char *p) { char c[1200]; snprintf(c, sizeof c, "%s", p); for (char *s = c + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(c, 0755); *s = '/'; } mkdir(c, 0755); }
static void set_mtime(const char *p, time_t t) { struct timeval tv[2] = {{t, 0}, {t, 0}}; utimes(p, tv); }
static time_t mtime_of(const char *p) { struct stat st; return stat(p, &st) ? 0 : st.st_mtimespec.tv_sec; }
static int exists(const char *p) { return !access(p, F_OK); }

// The newest backup of a save whose name ends in " <side>", "" when there is none
static void backup_of(const char *save, const char *side, char *out, size_t n) {
    char dir[1200]; snprintf(dir, sizeof dir, "%s-backups", save); out[0] = 0;
    DIR *d = opendir(dir); struct dirent *e; char best[256] = "";
    size_t sl = strlen(side);
    while (d && (e = readdir(d))) { size_t l = strlen(e->d_name); if (l > sl && e->d_name[l - sl - 1] == ' ' && !strcmp(e->d_name + l - sl, side) && strcmp(e->d_name, best) > 0) strlcpy(best, e->d_name, sizeof best); }
    if (d) closedir(d);
    if (*best) snprintf(out, n, "%s/%s", dir, best);
}
// What the cloud holds of the game file, unpacked into a folder of its own
static int cloud_has(const char *cloud_file, const char *root, const char *file, const char *value) {
    static int k; char into[1200], p[1400]; ss_meta m;
    snprintf(into, sizeof into, "%s/peek%d", root, k++); mkd(into);
    if (ss_read(cloud_file, into, &m)) return 0;
    snprintf(p, sizeof p, "%s/save/%s", into, file); return same(p, value);
}

static Result write_save(const char *path, const char *s) {           // create -> open -> set size -> write -> close, as a game does
    uint64_t h; h_fs_CreateFile(path, 0); Result r = h_fs_OpenFile(&h, path, MODE_WRITE); if (r) return r;
    h_fs_SetFileSize(h, (int64_t)strlen(s));
    r = h_fs_WriteFile(h, 0, s, strlen(s), NULL); h_fs_CloseFile(h); return r;
}

// One start of the game on a device: the save mount (the cloud's save comes in), then, with a file, the game writing it and committing, then a flush (the iPhone app
// going to the background). cloud NULL: sync off. Returns the child's failed steps
static int session(const char *save, const char *cloud, const char *file, const char *value) {
    fflush(stdout); fflush(stderr);
    pid_t p = fork();
    if (!p) {
        setenv("DM_SAVE", save, 1);
        if (cloud) setenv("DM_SAVE_SYNC", cloud, 1); else unsetenv("DM_SAVE_SYNC");
        int bad = h_fs_MountSave("save", NULL) != RES_OK;
        if (file) { char gp[256]; snprintf(gp, sizeof gp, "save:/%s", file); bad += write_save(gp, value) != RES_OK; bad += h_fs_Commit("save") != RES_OK; }
        if (cloud) bad += save_sync_flush(10000) != 1;
        fflush(stdout); fflush(stderr); _exit(bad);
    }
    int st; waitpid(p, &st, 0); return WIFEXITED(st) ? WEXITSTATUS(st) : 99;
}

int main(void) {
    char root[1024]; const char *sp = getenv("TMPDIR"); snprintf(root, sizeof root, "%s/t_save_sync.XXXXXX", sp ? sp : "/private/tmp");
    if (!mkdtemp(root)) { perror("mkdtemp"); return 1; }
    char p[1400], q[1400], u[1400], bundle[1400];
    ss_meta m, m2;

    printf("(1) packing\n");
    char P[1200]; snprintf(P, sizeof P, "%s/pack/save", root);
    snprintf(p, sizeof p, "%s/sub", P); mkd(p); snprintf(p, sizeof p, "%s/empty", P); mkd(p); snprintf(p, sizeof p, "%s_device", P); mkd(p);
    snprintf(p, sizeof p, "%s/a", P); put(p, "hello"); set_mtime(p, 1000000000);
    snprintf(p, sizeof p, "%s/sub/b", P); put(p, "xy"); setxattr(p, FS_GUEST_NAME, "B", 1, 0, XATTR_NOFOLLOW); set_mtime(p, 1000000100);
    snprintf(p, sizeof p, "%s_device/d", P); put(p, "dev"); set_mtime(p, 1000000050);
    CHECK(!ss_scan(P, &m) && m.files == 3 && strlen(m.hash) == SS_HASH && m.saved == 1000000100, "scan: 3 files, a 32-digit hash, saved = the newest file's date (%d files, saved %lld)", m.files, (long long)m.saved);
    snprintf(bundle, sizeof bundle, "%s/pack/save.dmsync", root);
    CHECK(!ss_pack(P, "0100abc", bundle, &m2) && !strcmp(m2.hash, m.hash) && !strcmp(m2.title, "0100ABC"), "pack: the same hash as the scan, the title ID in capitals (%s)", m2.title);
    CHECK(!ss_read(bundle, NULL, &m2) && !strcmp(m2.hash, m.hash) && m2.files == 3 && m2.saved == 1000000100 && !strcmp(m2.title, "0100ABC") && m2.device[0], "read: hash, files, date, title and device come back (from %s)", m2.device);
    snprintf(u, sizeof u, "%s/pack/out", root); mkd(u);
    CHECK(!ss_read(bundle, u, &m2), "unpack");
    snprintf(p, sizeof p, "%s/save/a", u); snprintf(q, sizeof q, "%s/save/sub/b", u);
    char g[8] = {0}; ssize_t gl = getxattr(q, FS_GUEST_NAME, g, sizeof g - 1, 0, XATTR_NOFOLLOW);
    CHECK(same(p, "hello") && same(q, "xy") && gl == 1 && g[0] == 'B', "files and the guest name (hle_fs.c's FS_GUEST_NAME) come back");
    CHECK(mtime_of(p) == 1000000000, "a file's date comes back (%lld)", (long long)mtime_of(p));
    snprintf(p, sizeof p, "%s/save/empty", u); snprintf(q, sizeof q, "%s/save_device/d", u);
    struct stat st; CHECK(!stat(p, &st) && S_ISDIR(st.st_mode) && same(q, "dev"), "an empty folder and the device save come back");
    snprintf(p, sizeof p, "%s/save", u); CHECK(!ss_scan(p, &m2) && !strcmp(m2.hash, m.hash), "the unpacked save has the original's hash");
    snprintf(p, sizeof p, "%s/a", P); set_mtime(p, 1200000000);
    CHECK(!ss_scan(P, &m2) && !strcmp(m2.hash, m.hash) && m2.saved == 1200000000, "another date, the same hash (the date still says which is newer)");
    snprintf(p, sizeof p, "%s/.DS_Store", P); put(p, "finder");
    CHECK(!ss_scan(P, &m2) && !strcmp(m2.hash, m.hash), "Finder's .DS_Store is not part of the save");
    snprintf(p, sizeof p, "%s/a", P); put(p, "hellO");
    CHECK(!ss_scan(P, &m2) && strcmp(m2.hash, m.hash), "one changed byte, another hash");
    snprintf(p, sizeof p, "%s/pack/none", root); mkd(p);
    CHECK(!ss_scan(p, &m2) && !m2.files && !m2.hash[0], "an empty folder is no save: no files, no hash");
    {   FILE *f = fopen(bundle, "rb"); fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f); char *b = malloc((size_t)n + 1); fread(b, 1, (size_t)n, f); fclose(f);
        snprintf(p, sizeof p, "%s/pack/bad.dmsync", root);
        f = fopen(p, "wb"); fwrite(b, 1, (size_t)n - 1, f); fclose(f); CHECK(ss_read(p, NULL, &m2) && !m2.files, "a file cut short by one byte is refused");
        f = fopen(p, "wb"); fwrite(b, 1, (size_t)n, f); fputc(0, f); fclose(f); CHECK(ss_read(p, NULL, &m2), "a byte too many is refused");
        b[n - 12] ^= 1; f = fopen(p, "wb"); fwrite(b, 1, (size_t)n, f); fclose(f); CHECK(ss_read(p, NULL, &m2), "a flipped byte in the contents is refused (the hash)");
        free(b);
    }
    {   // one entry, save/../escaped, in an otherwise well-formed file
        uint8_t hdr[144] = {0}, h[24] = {2}; uint32_t one = 1, n = 1; uint16_t pl = 15; uint64_t sz = 1;
        memcpy(hdr, "DMSYNC1\n", 8); memcpy(hdr + 8, &one, 4); memcpy(hdr + 12, &n, 4); memcpy(h + 2, &pl, 2); memcpy(h + 8, &sz, 8);
        snprintf(p, sizeof p, "%s/pack/escape.dmsync", root); FILE *f = fopen(p, "wb");
        fwrite(hdr, 1, sizeof hdr, f); fwrite(h, 1, sizeof h, f); fwrite("save/../escaped", 1, 15, f); fputc('z', f); fwrite("DMSYNCE\n", 1, 8, f); fclose(f);
        snprintf(u, sizeof u, "%s/pack/esc/in", root); mkd(u); snprintf(q, sizeof q, "%s/pack/esc/in/escaped", root);
        char q2[1400]; snprintf(q2, sizeof q2, "%s/pack/esc/escaped", root);
        CHECK(ss_read(p, u, &m2) && !exists(q) && !exists(q2), "a path that leaves the save is refused, and nothing is written");
    }

    printf("(2) which way\n");
    int both;
    CHECK(ss_decide("", "", "", 0, 0, &both) == SS_KEEP && !both, "no save anywhere: keep");
    CHECK(ss_decide("L", "", "", 0, 0, &both) == SS_PUSH && !both, "nothing in the cloud: send");
    CHECK(ss_decide("", "", "R", 0, 0, &both) == SS_PULL && !both, "nothing here: bring in");
    CHECK(ss_decide("X", "B", "X", 0, 0, &both) == SS_KEEP && !both, "the same here and in the cloud: keep");
    CHECK(ss_decide("B", "B", "R", 9, 1, &both) == SS_PULL && !both, "only the cloud's changed: bring in, even with an older date");
    CHECK(ss_decide("L", "B", "B", 1, 9, &both) == SS_PUSH && !both, "only this one changed: send, even with an older date");
    CHECK(ss_decide("L", "B", "R", 10, 20, &both) == SS_PULL && both, "both changed, the cloud's newer: bring in");
    CHECK(ss_decide("L", "B", "R", 20, 10, &both) == SS_PUSH && both, "both changed, this one newer: send");
    CHECK(ss_decide("L", "", "R", 20, 10, &both) == SS_PUSH && both, "a first sync with a save of its own counts as both changed");
    CHECK(ss_decide("L", "B", "R", 10, 10, &both) == SS_PUSH && both, "the same date: this device's is kept");

    char A[1200], B[1200], cloud[1200], cf[1400], id[24], back[1400];
    snprintf(A, sizeof A, "%s/mac/save", root); snprintf(B, sizeof B, "%s/iphone/save", root); snprintf(cloud, sizeof cloud, "%s/iCloud Drive/nxnative Saves", root);
    mkd(A); mkd(B); mkd(cloud);
    for (size_t i = 0; i <= strlen(DM_GAME_ID); i++) id[i] = (char)toupper((unsigned char)DM_GAME_ID[i]);
    snprintf(cf, sizeof cf, "%s/%s/save.dmsync", cloud, id);

    printf("(3) Mac -> cloud -> iPhone\n");
    snprintf(p, sizeof p, "%s/game.dat", A); put(p, "v1");
    CHECK(session(A, cloud, NULL, NULL) == 0 && cloud_has(cf, root, "game.dat", "v1"), "the Mac's first session with sync sends its save");
    snprintf(p, sizeof p, "%s.sync", A); CHECK(exists(p), "and remembers it as in step (%s)", p);
    snprintf(p, sizeof p, "%s.sync-tmp", A); CHECK(!exists(p), "nothing is left beside the save");
    snprintf(p, sizeof p, "%s/game.dat", B);
    CHECK(session(B, cloud, NULL, NULL) == 0 && same(p, "v1"), "the iPhone's first session brings it in");
    snprintf(q, sizeof q, "%s-backups", B); CHECK(!exists(q), "with no save of its own, the iPhone keeps no backup");
    CHECK(session(B, cloud, "game.dat", "v2") == 0 && cloud_has(cf, root, "game.dat", "v2"), "the iPhone saves: the cloud has its save");

    printf("(4) the cloud's changed\n");
    snprintf(p, sizeof p, "%s/game.dat", A);
    CHECK(session(A, cloud, NULL, NULL) == 0 && same(p, "v2"), "the Mac starts again: the iPhone's save comes in");
    backup_of(A, "local", back, sizeof back); snprintf(q, sizeof q, "%s/save/game.dat", back);
    CHECK(*back && same(q, "v1"), "the Mac's old save is kept (%s)", back);

    printf("(5) both changed, the cloud's newer\n");
    snprintf(p, sizeof p, "%s/game.dat", B);
    CHECK(session(B, NULL, "game.dat", "v4") == 0 && same(p, "v4"), "the iPhone plays with sync off");
    set_mtime(p, time(NULL) - 100);
    CHECK(session(A, cloud, "game.dat", "v5") == 0 && cloud_has(cf, root, "game.dat", "v5"), "then the Mac saves and sends");
    CHECK(session(B, cloud, NULL, NULL) == 0 && same(p, "v5"), "the iPhone starts with sync: the newer Mac save comes in");
    backup_of(B, "local", back, sizeof back); snprintf(q, sizeof q, "%s/save/game.dat", back);
    CHECK(*back && same(q, "v4"), "the iPhone's own is kept (%s)", back);

    printf("(6) both changed, this one newer\n");
    snprintf(p, sizeof p, "%s/game.dat", A);
    CHECK(session(A, NULL, "game.dat", "v6") == 0, "the Mac plays with sync off");
    set_mtime(p, time(NULL) + 100);
    CHECK(session(B, cloud, "game.dat", "v7") == 0 && cloud_has(cf, root, "game.dat", "v7"), "the iPhone saves and sends");
    CHECK(session(A, cloud, NULL, NULL) == 0 && same(p, "v6") && cloud_has(cf, root, "game.dat", "v6"), "the Mac starts with sync: its newer save stays and goes out");
    backup_of(A, "cloud", back, sizeof back); snprintf(q, sizeof q, "%s/save/game.dat", back);
    CHECK(*back && same(q, "v7"), "the iPhone's save it replaced in the cloud is kept on the Mac (%s)", back);

    printf("(7) the Mac launcher after the host\n");
    snprintf(p, sizeof p, "%s/game.dat", A); put(p, "v8");
    setenv("DM_SAVE_SYNC", cloud, 1);
    CHECK(save_sync_after_exit(A, DM_GAME_ID) == 1 && cloud_has(cf, root, "game.dat", "v8"), "a save the host left unsent goes out");
    time_t sent = mtime_of(cf);
    sleep(1);
    CHECK(save_sync_after_exit(A, DM_GAME_ID) == 1 && mtime_of(cf) == sent, "a second time there is nothing to send (the cloud's file is not rewritten)");
    unsetenv("DM_SAVE_SYNC");
    CHECK(save_sync_after_exit(A, DM_GAME_ID) == 1, "sync off: nothing to do");

    printf("(8) the game's own folder chosen\n");
    char C[1200], own[1300]; snprintf(C, sizeof C, "%s/mac2/save", root); mkd(C); snprintf(own, sizeof own, "%s/%s", cloud, id);
    snprintf(p, sizeof p, "%s/game.dat", C);
    CHECK(session(C, own, NULL, NULL) == 0 && same(p, "v8"), "<folder>/<title ID> itself as the folder: the save in it comes in");
    snprintf(q, sizeof q, "%s/%s", own, id); CHECK(!exists(q), "and no second <title ID> folder is made inside it");

    printf("(9) old backups go\n");
    snprintf(q, sizeof q, "%s-backups", C);
    for (int i = 0; i < 12; i++) { snprintf(p, sizeof p, "%s/2001-01-01 00.00.%02d local/save", q, i); mkd(p); }
    snprintf(p, sizeof p, "%s/2001-01-01 00.00.00 cloud", q); mkd(p); snprintf(p, sizeof p, "%s/my copy", q); mkd(p);
    snprintf(p, sizeof p, "%s/2001-01-01 00.00.00 local 2", q); mkd(p);
    CHECK(session(B, cloud, "game.dat", "v9") == 0 && cloud_has(cf, root, "game.dat", "v9"), "the iPhone saves again");
    snprintf(p, sizeof p, "%s/game.dat", C);
    CHECK(session(C, own, NULL, NULL) == 0 && same(p, "v9"), "the second Mac brings it in, keeping its own");
    int locals = 0; DIR *d = opendir(q); struct dirent *e;
    while (d && (e = readdir(d))) { size_t l = strlen(e->d_name); locals += l > 6 && (!strcmp(e->d_name + l - 6, " local") || strstr(e->d_name, " local ")); }
    if (d) closedir(d);
    backup_of(C, "local", back, sizeof back); snprintf(u, sizeof u, "%s/save/game.dat", back);
    CHECK(locals == 10 && same(u, "v8"), "10 local backups stay, the new one among them (%d)", locals);
    snprintf(p, sizeof p, "%s/2001-01-01 00.00.00 local 2", q); snprintf(u, sizeof u, "%s/2001-01-01 00.00.02 local", q);
    char u2[1400]; snprintf(u2, sizeof u2, "%s/2001-01-01 00.00.03 local", q);
    CHECK(!exists(p) && !exists(u) && exists(u2), "the four oldest went (00, 00 2, 01, 02)");
    snprintf(p, sizeof p, "%s/2001-01-01 00.00.00 cloud", q); snprintf(u, sizeof u, "%s/my copy", q);
    CHECK(exists(p) && exists(u), "the other side's backups and a folder of the player's stay");

    printf("(10) the Mac launcher's DM_SAVE with a slash after it\n");
    snprintf(p, sizeof p, "%s/game.dat", C); put(p, "v10");
    snprintf(u, sizeof u, "%s/", C); setenv("DM_SAVE_SYNC", own, 1);
    CHECK(save_sync_after_exit(u, DM_GAME_ID) == 1 && cloud_has(cf, root, "game.dat", "v10"), "it goes out");
    snprintf(p, sizeof p, "%s/.sync", C); CHECK(!exists(p), "and the sync state is beside the save, not inside it");
    unsetenv("DM_SAVE_SYNC");

    printf("== save sync: %d checks, %d failed ==\n", checks, fails);
    if (!fails) { char c[1200]; snprintf(c, sizeof c, "rm -rf '%s'", root); system(c); }
    return fails != 0;
}
