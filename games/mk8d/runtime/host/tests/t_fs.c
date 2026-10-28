// Save write and save transaction unit test (takes seconds): includes hle_fs.c directly and calls its static implementation; pwrite is replaced by a scriptable fake.
// All expected values are hand-written from the semantics, not computed by calling the implementation under test:
//   (1) pwrite returns 0 (can happen when the disk is full) -> WriteFile returns failure after one call (it used to loop forever here);
//   (2) pwrite writes in pieces (3 bytes first, then the rest) -> success and the file content is complete;
//   (3) transaction: the failure in (1) causes this transaction's Commit to be rejected and the working directory to return to the committed version; the next transaction writes in the working directory, the committed directory is still the old version before Commit and the new version after Commit;
//   (4) DM_TEST_FS_FAIL=write:N:errno makes only the Nth save write fail, with writes before and after proceeding normally; that transaction's Commit is rejected, the committed version is unchanged, and the front end receives one notification;
//      the next transaction commits normally, and the final save = the content of the next transaction;
//   (5) remounting discards uncommitted content.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
static ssize_t fake_pwrite(int fd, const void *buf, size_t n, off_t off);
#define pwrite fake_pwrite
#include "../hle_fs.c"
#undef pwrite
ssize_t pwrite(int, const void *, size_t, off_t);

struct dm_flags g_flag; uint64_t g_t0_ns; uint64_t now_ns(void) { return 0; }   // stubs for log timestamps and the read path (not used by the write path)
void dm_guest_write_prepare(void *a, size_t n) { (void)a; (void)n; }
extern int g_nfail, g_lasterr;                                        // front-end notification count (the strong implementation in t_fs_stub.c replaces the weak one in hle_fs.c)
static int g_mode, g_calls;                   // 0 write normally; 1 always return 0; 2 write only 3 bytes the first time
static ssize_t fake_pwrite(int fd, const void *buf, size_t n, off_t off) {
    g_calls++;
    if (g_mode == 1) { if (g_calls > 1000) { printf("  ✗ pwrite retried over 1000 times after returning 0 (endless loop)\n"); exit(1); } return 0; }
    if (g_mode == 2 && g_calls == 1 && n > 3) n = 3;
    return pwrite(fd, buf, n, off);
}
static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("  ✗ "); } else printf("  ✓ "); printf(__VA_ARGS__); printf("\n"); } while (0)
static void put(const char *p, const char *s) { FILE *f = fopen(p, "w"); fputs(s, f); fclose(f); }
static int same(const char *p, const char *s) {
    char b[64] = {0}; FILE *f = fopen(p, "r"); if (!f) return 0; size_t k = fread(b, 1, sizeof b - 1, f); fclose(f);
    return k == strlen(s) && !memcmp(b, s, k);
}
static Result write_save(const char *path, const char *s) {           // open -> set size -> write -> close
    uint64_t h; Result r = h_fs_OpenFile(&h, path, MODE_WRITE); if (r) return r;
    h_fs_SetFileSize(h, (int64_t)strlen(s));
    r = h_fs_WriteFile(h, 0, s, strlen(s), NULL); h_fs_CloseFile(h); return r;
}
static int read_fs(const char *path, char *out, size_t n) { uint64_t h; size_t got = 0; if (h_fs_OpenFile(&h, path, MODE_READ)) return 0; Result r = h_fs_ReadFile(&got, h, 0, out, n - 1); h_fs_CloseFile(h); out[got] = 0; return !r; }

int main(void) {
    char root[1024], save[1100], work[1200], p[1200];
    const char *sp = getenv("TMPDIR"); snprintf(root, sizeof root, "%s/t_fs.XXXXXX", sp ? sp : "/private/tmp");
    if (!mkdtemp(root)) { perror("mkdtemp"); return 1; }
    char rom[1100], overlay[1100], rdata[1200], odata[1200], fn[1300], got[64];
    snprintf(rom, sizeof rom, "%s/rom", root); snprintf(overlay, sizeof overlay, "%s/overlay", root); snprintf(rdata, sizeof rdata, "%s/data", rom); snprintf(odata, sizeof odata, "%s/data", overlay);
    mkdir(rom, 0755); mkdir(overlay, 0755); mkdir(rdata, 0755); mkdir(odata, 0755);
    snprintf(fn, sizeof fn, "%s/same", rdata); put(fn, "base"); snprintf(fn, sizeof fn, "%s/same", odata); put(fn, "overlay"); snprintf(fn, sizeof fn, "%s/base", rdata); put(fn, "only-base"); snprintf(fn, sizeof fn, "%s/over", odata); put(fn, "only-overlay");
    setenv("DM_ROMFS", rom, 1); setenv("DM_ROMFS_OVERLAY", overlay, 1); CHECK(h_fs_MountRom("rom", NULL, 0) == RES_OK, "mount RomFS with two layers (base + overlay)");
    CHECK(read_fs("rom:/data/same", got, sizeof got) && !strcmp(got, "overlay"), "the overlay file wins"); CHECK(read_fs("rom:/data/base", got, sizeof got) && !strcmp(got, "only-base"), "falls back to the base when the overlay lacks the file");
    uint64_t rh; CHECK(h_fs_OpenFile(&rh, "rom:/data/same", MODE_WRITE) == RES_FAIL, "the two-layer mount refuses writes");
    CHECK(read_fs("rom:\\data\\over", got, sizeof got) && !strcmp(got, "only-overlay") && read_fs("rom:\\data\\base", got, sizeof got) && !strcmp(got, "only-base"), "backslashes separate as slashes do, in both layers");
    CHECK(h_fs_OpenFile(&rh, "rom:/data/base", MODE_READ) == RES_OK && rh && !(rh & 15), "a folder file's handle is 16-byte aligned, as a Horizon FileAccessor pointer is (%#llx)", (unsigned long long)rh); h_fs_CloseFile(rh);
    g_nmnt = 0;

    printf("RomFS image\n");   // written by hand in romfs_image.py's layout: /data/a = "hello", /data/b empty directory, /data/c = "xy"
    {   uint8_t img[32 + 7 + 5 * 24 + 7] = {0}; uint64_t hd[3] = {39, 5 * 24 + 7, 5};
        img_ent e[5] = {{1, 1, 0, 0, 0, 0}, {2, 3, 0, 4, 0, 0}, {32, 5, 4, 1, 1, 0}, {5, 0, 5, 1, 0, 0}, {37, 2, 6, 1, 1, 0}};
        memcpy(img, "DMROMFS1", 8); memcpy(img + 8, hd, sizeof hd); memcpy(img + 32, "helloxy", 7); memcpy(img + 39, e, sizeof e); memcpy(img + 39 + sizeof e, "dataabc", 7);
        char ip[1100]; snprintf(ip, sizeof ip, "%s/rom.img", root); FILE *f = fopen(ip, "wb"); fwrite(img, 1, sizeof img, f); fclose(f);
        setenv("DM_ROMFS", ip, 1); unsetenv("DM_ROMFS_OVERLAY"); CHECK(h_fs_MountRom("rom", NULL, 0) == RES_OK, "mount the image");
        CHECK(read_fs("rom:/data/a", got, sizeof got) && !strcmp(got, "hello"), "reads a file"); CHECK(read_fs("rom:/data/../data/c", got, sizeof got) && !strcmp(got, "xy"), "reads the next file, through ..");
        uint64_t h; size_t n = 0; int64_t sz = 0; char b[16] = {0};
        CHECK(h_fs_OpenFile(&h, "rom:/data/a", MODE_READ) == RES_OK && h_fs_ReadFile(&n, h, 3, b, 10) == RES_OK && n == 2 && !memcmp(b, "lo", 2), "a read stops at the end of the file (got %zu)", n);
        CHECK(h_fs_GetFileSize(&sz, h) == RES_OK && sz == 5 && h_fs_GetFileOpenMode(h) == MODE_READ, "size 5, open for reading"); h_fs_CloseFile(h);
        // entry 4: its index has low bits set, which Metroid Prime 4's file device masks off (handle & ~7) as it may of a Horizon pointer
        CHECK(h_fs_OpenFile(&h, "rom:/data/c", MODE_READ) == RES_OK && !(h & 15) && h_fs_ReadFile(&n, h & ~7ull, 0, b, 10) == RES_OK && n == 2 && !memcmp(b, "xy", 2), "entry 4 still reads after its handle's low bits are masked off (%#llx)", (unsigned long long)h);
        CHECK(h_fs_GetFileSize(&sz, h & ~7ull) == RES_OK && sz == 2, "and is 2 bytes"); h_fs_CloseFile(h & ~7ull);
        CHECK(read_fs("rom:\\data\\c", got, sizeof got) && !strcmp(got, "xy") && read_fs("rom:\\data\\..\\data\\a", got, sizeof got) && !strcmp(got, "hello"), "backslash paths, also through ..");
        CHECK(h_fs_OpenFile(&h, "rom:/data/x", MODE_READ) == RES_PATH_NOT_FOUND, "a missing file is not found");
        CHECK(h_fs_OpenFile(&h, "rom:/data/A", MODE_READ) == RES_PATH_NOT_FOUND, "names are case-sensitive");
        CHECK(h_fs_OpenFile(&h, "rom:/data/b", MODE_READ) == RES_PATH_NOT_FOUND, "a directory does not open as a file");
        CHECK(h_fs_OpenFile(&h, "rom:/data/a", MODE_WRITE) == RES_FAIL && h_fs_CreateFile("rom:/data/a", 1) == RES_PATH_EXISTS, "refuses writes; create answers \"exists\"");
        int32_t t = -1; CHECK(h_fs_GetEntryType(&t, "rom:/data/b") == RES_OK && t == ENT_DIR && h_fs_GetEntryType(&t, "rom:/data/c") == RES_OK && t == ENT_FILE, "entry types");
        DirectoryEntry de[4]; int64_t cnt = 0;
        CHECK(h_fs_OpenDirectory(&h, "rom:/data", DIRMODE_DIR | DIRMODE_FILE) == RES_OK && h_fs_DirCount(&cnt, h) == RES_OK && cnt == 3, "a directory of 3 entries (%lld)", (long long)cnt);
        CHECK(h_fs_ReadDirectory(&cnt, de, h, 4) == RES_OK && cnt == 3 && !strcmp(de[0].name, "a") && de[0].size == 5 && de[1].type == ENT_DIR && !strcmp(de[2].name, "c"), "lists a, b, c"); h_fs_CloseDirectory(h);
        CHECK(h_fs_OpenDirectory(&h, "rom:/", DIRMODE_FILE) == RES_OK && h_fs_ReadDirectory(&cnt, de, h, 4) == RES_OK && cnt == 0, "the file filter hides directories"); h_fs_CloseDirectory(h);
        CHECK(h_fs_OpenDirectory(&h, "rom:/data/b", DIRMODE_DIR | DIRMODE_FILE) == RES_OK && h_fs_DirCount(&cnt, h) == RES_OK && cnt == 0, "an empty directory"); h_fs_CloseDirectory(h);
        g_nmnt = 0;
        char ov[1100], ovd[1200]; snprintf(ov, sizeof ov, "%s/imgov", root); snprintf(ovd, sizeof ovd, "%s/data", ov); mkdir(ov, 0755); mkdir(ovd, 0755);
        snprintf(fn, sizeof fn, "%s/a", ovd); put(fn, "mod"); snprintf(fn, sizeof fn, "%s/new", ovd); put(fn, "added");
        setenv("DM_ROMFS_OVERLAY", ov, 1); CHECK(h_fs_MountRom("rom", NULL, 0) == RES_OK, "mount the image with an overlay folder");
        CHECK(read_fs("rom:/data/a", got, sizeof got) && !strcmp(got, "mod"), "the overlay file wins over the image");
        CHECK(read_fs("rom:/data/c", got, sizeof got) && !strcmp(got, "xy") && read_fs("rom:/data/new", got, sizeof got) && !strcmp(got, "added"), "image files and overlay-only files both read");
        CHECK(h_fs_OpenDirectory(&h, "rom:/data", DIRMODE_DIR | DIRMODE_FILE) == RES_OK && h_fs_ReadDirectory(&cnt, de, h, 4) == RES_OK && cnt == 4, "the listing merges both, once each (%lld)", (long long)cnt);
        int seen = 0; for (int i = 0; i < cnt; i++) { if (!strcmp(de[i].name, "a")) seen += de[i].size == 3; if (!strcmp(de[i].name, "b")) seen += de[i].type == ENT_DIR; if (!strcmp(de[i].name, "c")) seen += de[i].size == 2; if (!strcmp(de[i].name, "new")) seen += de[i].size == 5; }
        CHECK(seen == 4, "with the right types and sizes"); h_fs_CloseDirectory(h);
        CHECK(read_fs("rom:\\data\\a", got, sizeof got) && !strcmp(got, "mod") && read_fs("rom:\\data\\c", got, sizeof got) && !strcmp(got, "xy"), "backslash paths reach the overlay folder and the image");
        h = 0; cnt = -1; CHECK(h_fs_OpenDirectory(&h, "rom:\\data", DIRMODE_DIR | DIRMODE_FILE) == RES_OK && h_fs_DirCount(&cnt, h) == RES_OK && cnt == 4, "and list the merged directory (%lld)", (long long)cnt); h_fs_CloseDirectory(h);
        unsetenv("DM_ROMFS_OVERLAY"); g_nmnt = 0;
        snprintf(fn, sizeof fn, "%s/2.img", root); CHECK(!link(ip, fn), "stage a DLC image"); setenv("DM_AOC_DIR", root, 1);
        size_t cache = 0;
        CHECK(h_fs_QueryAocCache(&cache, 2) == RES_OK && cache == 176, "DLC image reports Horizon table size (176, got %zu)", cache);
        CHECK(h_fs_MountAoc("aoc", 2, NULL, 0) == RES_OK && read_fs("aoc:/data/a", got, sizeof got) && !strcmp(got, "hello"), "DLC mounts and reads directly from its image");
        unsetenv("DM_AOC_DIR"); g_nmnt = 0;
        f = fopen(ip, "wb"); fwrite(img, 1, 39 + 50, f); fclose(f);   // cut off inside the index, as a copy stopped partway
        CHECK(!img_open(ip), "a cut-off image is refused");
    }
    snprintf(save, sizeof save, "%s/save", root); snprintf(work, sizeof work, "%s.work", save);
    mkdir(save, 0755); snprintf(p, sizeof p, "%s/a", save); put(p, "old");
    setenv("DM_SAVE", save, 1); setenv("DM_TEST_FS_FAIL", "write:5:28", 1);   // the 5th save write fails, errno 28 = ENOSPC
    CHECK(h_fs_MountSave("save", NULL) == RES_OK, "mount the save");

    // Save write count: (1)=1st (2)=2nd (3)=3rd (4)=4th/5th/6th/7th (5)=8th
    printf("① pwrite returns 0\n");
    g_mode = 1; g_calls = 0;
    CHECK(write_save("save:/a", "xyz") == RES_FAIL, "WriteFile returns failure 0x2A2");
    CHECK(g_calls == 1, "pwrite called only once (actually %d times; it used to retry forever)", g_calls);

    printf("② pwrite writes in pieces\n");
    g_mode = 2; g_calls = 0;
    CHECK(write_save("save:/a", "abcdefg") == RES_OK, "WriteFile succeeds");
    CHECK(g_calls == 2, "pwrite called twice (3 bytes + 4 bytes; actually %d)", g_calls);
    snprintf(p, sizeof p, "%s/a", work); CHECK(same(p, "abcdefg"), "the work directory holds the complete abcdefg");
    g_mode = 0;

    printf("③ transaction\n");
    CHECK(h_fs_Commit("save") == RES_FAIL && g_nfail == 1 && g_lasterr == ENOSPC, "the Commit of the transaction holding ①② is refused, front end notified once errno=ENOSPC (actually %d times errno=%d)", g_nfail, g_lasterr);
    snprintf(p, sizeof p, "%s/a", work); CHECK(same(p, "old"), "work directory discarded, back to the committed version old");
    CHECK(write_save("save:/a", "new") == RES_OK, "next transaction: write new");
    snprintf(p, sizeof p, "%s/a", save); CHECK(same(p, "old"), "before Commit the committed directory is still old");
    CHECK(h_fs_Commit("save") == RES_OK, "Commit");
    CHECK(same(p, "new"), "after Commit the committed directory is new");
    snprintf(p, sizeof p, "%s.new", save); CHECK(access(p, F_OK) != 0, "the swapped-out old version is deleted (no .new left)");

    printf("④ DM_TEST_FS_FAIL=write:5:28\n");
    CHECK(write_save("save:/b", "4") == RES_PATH_NOT_FOUND, "(b does not exist: open fails, not counted as a write)");
    h_fs_CreateFile("save:/b", 0);
    CHECK(write_save("save:/b", "4") == RES_OK, "4th write succeeds");
    errno = 0; CHECK(write_save("save:/b", "5") == RES_FAIL && errno == 28, "5th write fails errno=28 (actually errno=%d)", errno);
    CHECK(write_save("save:/b", "6") == RES_OK, "6th write succeeds");
    CHECK(h_fs_Commit("save") == RES_FAIL && g_nfail == 2 && g_lasterr == 28, "that transaction's Commit is refused, front end notified errno=28 (%d times in total)", g_nfail);
    snprintf(p, sizeof p, "%s/b", save); CHECK(access(p, F_OK) != 0, "committed directory unchanged (no b)");
    snprintf(p, sizeof p, "%s/b", work); CHECK(access(p, F_OK) != 0, "work directory discarded (no b)");
    snprintf(p, sizeof p, "%s/a", save); CHECK(same(p, "new"), "committed directory a is still new");
    h_fs_CreateFile("save:/b", 0);
    CHECK(write_save("save:/b", "7") == RES_OK && h_fs_Commit("save") == RES_OK, "next transaction: write b=7, Commit succeeds");
    snprintf(p, sizeof p, "%s/b", save); CHECK(same(p, "7"), "final committed b = 7 (the next transaction's content)");
    CHECK(g_nfail == 2, "a successful transaction sends no notification (still %d times in total)", g_nfail);

    printf("⑤ remounting discards uncommitted content\n");
    CHECK(write_save("save:/a", "v9") == RES_OK, "write a=v9 without committing");
    g_nmnt = 0;                                                        // simulate a process restart: clear the mount table
    CHECK(h_fs_MountSave("save", NULL) == RES_OK, "remount");
    snprintf(p, sizeof p, "%s/a", work); CHECK(same(p, "new"), "work directory a is back to the committed version new");
    snprintf(p, sizeof p, "%s/b", work); CHECK(same(p, "7"), "work directory b = the committed 7");

    printf("⑥ failed writes are handled without exiting (DM_TEST_FS_FAILAT)\n");
    unsetenv("DM_TEST_FS_FAIL");
    // 6a Recloning after a refused commit fails: do not exit, the work directory is untouched and the committed version unchanged; the next commit is refused again and the reclone succeeds; the transaction after that commits normally
    setenv("DM_TEST_FS_FAILAT", "reclone:1", 1);
    CHECK(write_save("save:/a", "bad") == RES_OK, "write a=bad");
    save_poison(ENOSPC); int nf0 = g_nfail;   // a save write of this transaction failed
    CHECK(h_fs_Commit("save") == RES_FAIL && g_nfail == nf0 + 1, "this transaction is refused (injected reclone failure), the process is still alive, front end notified once");
    snprintf(p, sizeof p, "%s/a", save); CHECK(same(p, "new"), "committed directory a is still new");
    snprintf(p, sizeof p, "%s/a", work); CHECK(same(p, "bad"), "work directory not deleted (a is still bad)");
    CHECK(h_fs_Commit("save") == RES_FAIL && g_nfail == nf0 + 2, "still tainted: the next commit is refused too");
    snprintf(p, sizeof p, "%s/a", work); CHECK(same(p, "new"), "this time the reclone works: work directory a is back to new");
    snprintf(p, sizeof p, "%s.re", work); CHECK(access(p, F_OK) != 0, "no .re left");
    CHECK(write_save("save:/a", "good") == RES_OK && h_fs_Commit("save") == RES_OK, "next transaction writes a=good, commit succeeds");
    snprintf(p, sizeof p, "%s/a", save); CHECK(same(p, "good"), "committed a = good");
    // 6b Writing the guest name after a rename fails and the rollback fails too: do not exit; the file is in a transactional save, so this transaction's commit is refused and the committed version is unchanged; after recloning everything works
    setenv("DM_TEST_FS_FAILAT", "guestname:1,renameback:1", 1);
    h_fs_CreateFile("save:/c", 0); CHECK(write_save("save:/c", "c") == RES_OK, "write c");
    CHECK(h_fs_RenameFile("save:/c", "save:/d") != RES_OK, "rename c → d returns failure (both steps injected to fail), the process is still alive");
    nf0 = g_nfail; CHECK(h_fs_Commit("save") == RES_FAIL && g_nfail == nf0 + 1, "this transaction's commit is refused");
    snprintf(p, sizeof p, "%s/c", save); CHECK(access(p, F_OK) != 0, "committed directory has no c");
    snprintf(p, sizeof p, "%s/d", save); CHECK(access(p, F_OK) != 0, "committed directory has no d");
    snprintf(p, sizeof p, "%s/a", save); CHECK(same(p, "good"), "committed a is still good");
    CHECK(write_save("save:/a", "ok2") == RES_OK && h_fs_Commit("save") == RES_OK, "the next transaction commits as usual");
    // 6c Cloning the work directory at mount fails: do not exit; mount the committed directory read-only instead. Reads work, writes fail, commits are no-ops, the committed version is unchanged, and the front end is notified once; the next normal mount works as before
    setenv("DM_TEST_FS_FAILAT", "mountclone:1", 1); g_nmnt = 0; nf0 = g_nfail;
    snprintf(p, sizeof p, "rm -rf '%s'", work); system(p);
    CHECK(h_fs_MountSave("save", NULL) == RES_OK && g_nfail == nf0 + 1, "mount returns success, front end notified once (actually %d times)", g_nfail - nf0);
    CHECK(read_fs("save:/a", got, sizeof got) && !strcmp(got, "ok2"), "the read-only mount reads the committed a=ok2 (read %s)", got);
    CHECK(write_save("save:/a", "zzz") != RES_OK, "writes fail");
    CHECK(h_fs_Commit("save") == RES_OK, "commit is a no-op");
    snprintf(p, sizeof p, "%s/a", save); CHECK(same(p, "ok2"), "committed a is still ok2");
    unsetenv("DM_TEST_FS_FAILAT"); g_nmnt = 0;
    CHECK(h_fs_MountSave("save", NULL) == RES_OK && write_save("save:/a", "ok3") == RES_OK && h_fs_Commit("save") == RES_OK, "remount without injection: transactional as usual, writing a=ok3 and committing succeed");
    snprintf(p, sizeof p, "%s/a", save); CHECK(same(p, "ok3"), "committed a = ok3");

    printf("⑦ backslash paths on a save\n");
    {   uint64_t h; DirectoryEntry de[4]; int64_t cnt = 0; int32_t t = -1;
        CHECK(h_fs_CreateDirectory("save:\\dir") == RES_OK && h_fs_CreateFile("save:\\dir\\f", 0) == RES_OK && write_save("save:\\dir\\f", "bs") == RES_OK, "create and write save:\\dir\\f");
        CHECK(read_fs("save:/dir/f", got, sizeof got) && !strcmp(got, "bs"), "it is save:/dir/f");
        CHECK(h_fs_RenameFile("save:\\dir\\f", "save:\\dir\\g") == RES_OK && h_fs_GetEntryType(&t, "save:/dir/g") == RES_OK && t == ENT_FILE, "rename by backslash path");
        h = 0; CHECK(h_fs_OpenDirectory(&h, "save:\\dir", DIRMODE_FILE) == RES_OK && h_fs_ReadDirectory(&cnt, de, h, 4) == RES_OK && cnt == 1 && !strcmp(de[0].name, "g"), "the directory lists g, not a name with backslashes (%s)", cnt ? de[0].name : "-"); h_fs_CloseDirectory(h);
    }


    char cmd[1200]; snprintf(cmd, sizeof cmd, "rm -rf '%s'", root); system(cmd);
    printf("== %d checks, %d failed ==\n", checks, fails);
    return fails != 0;
}
