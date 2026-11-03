// Horizon path case unit test: expectations are hand-written; runs real file operations on the default case-insensitive APFS.
// Names differing only in case must coexist, all path components match exactly, directory listing still gives the guest names, and Commit/remount preserve names and content.
#ifndef HLE_FS_SOURCE
#define HLE_FS_SOURCE "../hle_fs.c"
#endif
#include <sys/xattr.h>
#include <errno.h>
static int fail_name_write;
static int fake_setxattr(const char *p, const char *name, const void *v, size_t n, u_int32_t pos, int opt) {
    if (fail_name_write) { errno = ENOSPC; return -1; }
    return setxattr(p, name, v, n, pos, opt);
}
#define setxattr fake_setxattr
#include HLE_FS_SOURCE
#undef setxattr
struct dm_flags g_flag; uint64_t g_t0_ns;
uint64_t now_ns(void) { return 0; }
void dm_guest_write_prepare(void *a, size_t n) { (void)a; (void)n; }
static int checks, fails;
#define CHECK(c, text) do { checks++; int ok = !!(c); fails += !ok; printf("%s %s\n", ok ? "✓" : "✗", text); } while (0)
static int content(const char *path, const char *want) {
    uint64_t h; if (h_fs_OpenFile(&h, path, MODE_READ)) return 0;
    char b[64] = {0}; size_t n = 0; Result r = h_fs_ReadFile(&n, h, 0, b, sizeof b);
    h_fs_CloseFile(h); return !r && n == strlen(want) && !memcmp(b, want, n);
}
static int write_text(const char *path, const char *text) {
    uint64_t h; if (h_fs_OpenFile(&h, path, MODE_WRITE)) return 0;
    Result r = h_fs_WriteFile(h, 0, text, strlen(text), NULL); h_fs_CloseFile(h); return !r;
}
static int listed(const char *path, const char *name, int type) {
    uint64_t h; if (h_fs_OpenDirectory(&h, path, 3)) return 0;
    DirectoryEntry e[32]; int64_t n = 0; int found = 0;
    h_fs_ReadDirectory(&n, e, h, 32); h_fs_CloseDirectory(h);
    for (int i = 0; i < n; i++) if (!strcmp(e[i].name, name) && e[i].type == type) found++;
    return found == 1;
}
static int entry_count(void) {
    uint64_t h; if (h_fs_OpenDirectory(&h, "save:/", 3)) return -1;
    DirectoryEntry e[32]; int64_t n = 0;
    h_fs_ReadDirectory(&n, e, h, 32); h_fs_CloseDirectory(h); return (int)n;
}
static void *create_race(void *p) { return (void *)(uintptr_t)h_fs_CreateFile(p, 0); }
module_t *g_mod; int g_nmod;   // Module table (MountSave picks save fixes by main build ID): this unit test loads no modules
int main(void) {
    char root[1024], save[1024], p[1100];
    const char *tmp = getenv("TMPDIR");
    if (tmp) snprintf(root, sizeof root, "%s/case.XXXXXX", tmp);
    else snprintf(root, sizeof root, "%s%s/run/platform/case.XXXXXX", getenv("DM_ROOT") ?: getenv("HOME"), getenv("DM_ROOT") ? "" : "/dm-work");
    if (!mkdtemp(root)) { perror("mkdtemp"); return 2; }
    snprintf(save, sizeof save, "%s/save", root); mkdir(save, 0755);
    snprintf(p, sizeof p, "%s/Legacy", save); FILE *f = fopen(p, "w"); fputs("old", f); fclose(f);
    setenv("DM_SAVE", save, 1); CHECK(h_fs_MountSave("save", NULL) == RES_OK, "mount the existing save");
    uint64_t h = 0; int32_t type = -1;
    CHECK(content("save:/Legacy", "old"), "the existing name with its case is readable");
    Result r = h_fs_OpenFile(&h, "save:/legacy", 1); if (!r) h_fs_CloseFile(h);
    CHECK(r == RES_PATH_NOT_FOUND, "wrong case OpenFile → PathNotFound");
    CHECK(h_fs_GetEntryType(&type, "save:/legacy") == RES_PATH_NOT_FOUND, "wrong case GetEntryType → PathNotFound");
    CHECK(h_fs_DeleteFile("save:/legacy") == RES_PATH_NOT_FOUND, "wrong case DeleteFile → PathNotFound");
    CHECK(content("save:/Legacy", "old"), "deleting by the wrong name leaves the original file alone");
    CHECK(h_fs_CreateFile("save:/legacy", 0) == RES_OK, "Legacy and legacy can coexist");
    CHECK(write_text("save:/legacy", "lower"), "write legacy");
    CHECK(content("save:/Legacy", "old") && content("save:/legacy", "lower"), "the two names read back separate content old/lower");
    CHECK(h_fs_CreateFile("save:/legacy", 0) == RES_PATH_EXISTS, "creating the exact same name again → PathExists");
    CHECK(listed("save:/", "Legacy", ENT_FILE) && listed("save:/", "legacy", ENT_FILE), "directory listing keeps Legacy/legacy");
    CHECK(h_fs_CreateDirectory("save:/Dir") == RES_OK, "create Dir");
    CHECK(h_fs_CreateFile("save:/dir/x", 0) == RES_PATH_NOT_FOUND, "creation through an intermediate directory with the wrong case is refused");
    CHECK(h_fs_CreateDirectory("save:/dir") == RES_OK, "Dir and dir can coexist");
    CHECK(h_fs_CreateFile("save:/Dir/X", 0) == RES_OK && write_text("save:/Dir/X", "upper"), "Dir/X has its own content upper");
    CHECK(h_fs_CreateFile("save:/dir/X", 0) == RES_OK && write_text("save:/dir/X", "lower"), "dir/X has its own content lower");
    CHECK(content("save:/Dir/X", "upper") && content("save:/dir/X", "lower"), "reads do not mix up intermediate directories by case");
    r = h_fs_OpenDirectory(&h, "save:/DIR", 3); if (!r) h_fs_CloseDirectory(h);
    CHECK(r == RES_PATH_NOT_FOUND, "wrong case OpenDirectory → PathNotFound");
    CHECK(h_fs_DeleteDirRec("save:/DIR") == RES_PATH_NOT_FOUND, "wrong case recursive delete → PathNotFound");
    CHECK(listed("save:/", "Dir", ENT_DIR) && listed("save:/", "dir", ENT_DIR), "directory listing keeps Dir/dir");
    CHECK(h_fs_RenameFile("save:/Legacy", "save:/legacy") == RES_PATH_EXISTS, "rename onto an existing exact name → PathExists");
    CHECK(h_fs_RenameFile("save:/LEGACY", "save:/other") == RES_PATH_NOT_FOUND, "rename from a wrong source name → PathNotFound");
    CHECK(h_fs_RenameFile("save:/Legacy", "save:/LEGACY") == RES_OK, "a rename that only changes case succeeds");
    CHECK(content("save:/LEGACY", "old") && content("save:/legacy", "lower"), "the rename does not overwrite the other guest file");
    CHECK(h_fs_GetEntryType(&type, "save:/Legacy") == RES_PATH_NOT_FOUND, "the old guest name is gone");
    CHECK(h_fs_Commit("save") == RES_OK, "commit both case variants of the files and directories");
    g_nmnt = 0; CHECK(h_fs_MountSave("save", NULL) == RES_OK, "remount the committed save");
    CHECK(content("save:/LEGACY", "old") && content("save:/legacy", "lower"), "commit + remount keeps the separate names/content");
    CHECK(content("save:/Dir/X", "upper") && content("save:/dir/X", "lower"), "commit + remount keeps the separate directories");
    CHECK(listed("save:/", "LEGACY", ENT_FILE) && listed("save:/", "legacy", ENT_FILE), "after commit + remount the listing misses no mapped name");
    CHECK(h_fs_DeleteFile("save:/legacy") == RES_OK && content("save:/LEGACY", "old"), "deleting a mapped file leaves the other name alone");
    CHECK(h_fs_DeleteDirRec("save:/dir") == RES_OK && content("save:/Dir/X", "upper"), "deleting a mapped directory leaves the other directory alone");
    CHECK(h_fs_CreateFile("save:/legacy", 0) == RES_OK && write_text("save:/legacy", "again"), "a file of the same name can be created again after deletion");
    CHECK(h_fs_RenameFile("save:/legacy", "save:/plain") == RES_OK && content("save:/plain", "again"), "a mapped file renamed to an ordinary name is readable");
    CHECK(listed("save:/", "plain", ENT_FILE) && !listed("save:/", "legacy", ENT_FILE), "rename updates the listing, the old name is gone");
    CHECK(mount("rom", save, 1) == RES_OK && content("rom:/LEGACY", "old"), "a read-only mount reads mapped names");
    CHECK(h_fs_GetEntryType(&type, "rom:/Legacy") == RES_PATH_NOT_FOUND, "a read-only mount also looks up by exact case");
    CHECK(h_fs_CreateDirectory("save:/Trail/") == RES_OK && listed("save:/", "Trail", ENT_DIR), "a trailing slash on create directory does not become an empty name");
    CHECK(h_fs_CreateFile("save:/.dm-case-1", 0) == RES_OK && write_text("save:/.dm-case-1", "real"), "a real guest name with the alias prefix can be created");
    CHECK(content("save:/.dm-case-1", "real") && content("save:/LEGACY", "old"), "a physical alias does not pose as a real guest name");
    CHECK(listed("save:/", ".dm-case-1", ENT_FILE), "a real guest name with the alias prefix is listed under its own name");
    CHECK(h_fs_CreateFile("save:/plain", 0) == RES_PATH_EXISTS, "creating an ordinary name twice is still refused");
    int before = entry_count(); fail_name_write = 1;
    CHECK(h_fs_CreateFile("save:/PLAIN", 0) == RES_FAIL, "disk full on alias metadata: create file returns failure");
    CHECK(entry_count() == before && h_fs_GetEntryType(&type, "save:/PLAIN") == RES_PATH_NOT_FOUND, "a failed create leaves no file or guest name behind");
    CHECK(h_fs_CreateDirectory("save:/trail") == RES_FAIL && entry_count() == before, "alias directory metadata fails: the new directory is removed");
    CHECK(h_fs_RenameFile("save:/plain", "save:/PLAIN") == RES_FAIL, "rename metadata failure returns failure");
    CHECK(content("save:/plain", "again") && h_fs_GetEntryType(&type, "save:/PLAIN") == RES_PATH_NOT_FOUND && entry_count() == before, "a failed rename fully restores the original name and content");
    fail_name_write = 0;
    CHECK(h_fs_CreateFile("save:/PLAIN", -1) == RES_FAIL && entry_count() == before, "a failed size set on an alias file leaves no physical name without metadata");
    CHECK(h_fs_CreateFile("save:/Ä", 0) == RES_OK && h_fs_CreateFile("save:/ä", 0) == RES_OK, "non-ASCII case variants coexist too");
    CHECK(write_text("save:/Ä", "upper") && write_text("save:/ä", "lower") && content("save:/Ä", "upper") && content("save:/ä", "lower"), "non-ASCII names have separate content");
    pthread_t t[2]; void *r0, *r1;
    pthread_create(&t[0], NULL, create_race, "save:/Race"); pthread_create(&t[1], NULL, create_race, "save:/race");
    pthread_join(t[0], &r0); pthread_join(t[1], &r1);
    CHECK(!r0 && !r1 && listed("save:/", "Race", ENT_FILE) && listed("save:/", "race", ENT_FILE), "concurrent creates of both case variants each publish separately");
    rm_tree(root);
    printf("== case: %d checks, %d failed ==\n", checks, fails); return fails != 0;
}
// the guest address space (hle_svc.c): the unit test has none, so every stack is a host stack (hle_bigstack.c)
int dm_guest_as_contains(uintptr_t a) { (void)a; return 0; }
// main's imports (loader.c) and the MiiModel stand-in (hle_mii.c): no main module, no system data
int dm_main_imports(const char *prefix) { (void)prefix; return 0; }
int dm_mii_standin(const char *dir) { (void)dir; return 0; }
