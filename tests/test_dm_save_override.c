/* Optional DM_SAVE override across shared account.c and fs_host.c (TOTK/BotW/DKB/Pokopia and OoT):
   - when DM_SAVE is set, Account 1 uses fixed UID 53574e415449564578626c0000000001, MountSaveData mounts
     $DM_SAVE directly (no <uid>/ directory), MountDeviceSaveData mounts ${DM_SAVE}_device, and MNG's
     users.txt / cloud save sync are bypassed;
   - when DM_SAVE is unset or empty, default TOTK_PROFILE_DIR/save/<uid> behavior is unchanged. */
#include "runtime.h"
#include "fs_host.h"
#include "users.h"
#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int cloud_started, cloud_changed;

_Noreturn void runtime_fail(const char *message) {
    fprintf(stderr, "runtime_fail: %s\n", message);
    exit(1);
}
uint64_t runtime_now_ns(void) { return 1000000000ull; }
void dm_guest_write_prepare(void *addr, size_t n) { (void)addr; (void)n; }
void hle_bind_system_event(void *event, int autoclear) { (void)event; (void)autoclear; }
void hle_signal_system_event(void *event) { (void)event; }
int runtime_romfs_owns(const char *path) { (void)path; return 0; }
int runtime_romfs_handle(uint64_t h) { (void)h; return 0; }
unsigned runtime_romfs_open(uint64_t *out, const char *path, int mode) { (void)out; (void)path; (void)mode; return 1; }
unsigned runtime_romfs_read(uint64_t *out, uint64_t h, int64_t offset, void *buffer, uint64_t size) {
    (void)out; (void)h; (void)offset; (void)buffer; (void)size; return 1;
}
unsigned runtime_romfs_size(int64_t *out, uint64_t h) { (void)out; (void)h; return 1; }
void runtime_romfs_close(uint64_t h) { (void)h; }
unsigned runtime_romfs_entry_type(int *out, const char *path) { (void)out; (void)path; return 1; }
void runtime_autoload_save_file_opened(const char *path) { (void)path; }
void runtime_fs_log(const char *what, const char *path) { (void)what; (void)path; }
void runtime_cloud_save_start(const char *save_dir, const char *profile_dir) {
    (void)save_dir; (void)profile_dir; cloud_started++;
}
void runtime_cloud_save_changed(void) { cloud_changed++; }
void runtime_cloud_save_quit(void) {}
int runtime_core_away(void) { return 1; }
void runtime_core_back(int *away) { (void)away; }
#ifdef GAME_OOT
int runtime_romfs_directory_handle(uint64_t h) { (void)h; return 0; }
unsigned runtime_romfs_directory_open(uint64_t *out, const char *path, int mode) {
    (void)out; (void)path; (void)mode; return 1;
}
unsigned runtime_romfs_directory_read(int64_t *count, RuntimeDirectoryEntry *out, uint64_t h, int64_t max) {
    (void)count; (void)out; (void)h; (void)max; return 1;
}
unsigned runtime_romfs_directory_count(int64_t *out, uint64_t h) { (void)out; (void)h; return 1; }
void runtime_romfs_directory_close(uint64_t h) { (void)h; }
#endif
#if defined(GAME_BOTW) || defined(GAME_DKB)
int runtime_aoc_owns(const char *path) { (void)path; return 0; }
int runtime_aoc_owns_name(const char *name) { (void)name; return 0; }
void runtime_aoc_unmount(const char *name) { (void)name; }
unsigned runtime_aoc_open(uint64_t *out, const char *path, int mode) { (void)out; (void)path; (void)mode; return 1; }
int runtime_aoc_handle(uint64_t h) { (void)h; return 0; }
unsigned runtime_aoc_read(uint64_t *out, uint64_t h, int64_t offset, void *buffer, uint64_t size) {
    (void)out; (void)h; (void)offset; (void)buffer; (void)size; return 1;
}
unsigned runtime_aoc_size(int64_t *out, uint64_t h) { (void)out; (void)h; return 1; }
void runtime_aoc_close(uint64_t h) { (void)h; }
unsigned runtime_aoc_entry_type(int *out, const char *path) { (void)out; (void)path; return 1; }
unsigned runtime_aoc_mount(const char *name, int index, void *cache, uint64_t size) {
    (void)name; (void)index; (void)cache; (void)size; return 1;
}
unsigned runtime_aoc_cache_size(uint64_t *out, int index) { (void)out; (void)index; return 1; }
#endif

static void new_dir(char out[256]) {
    const char *tmp = getenv("TMPDIR");
    size_t n = tmp && *tmp ? strlen(tmp) : 0;
    snprintf(out, 256, "%.*s%smng-dmsave-XXXXXX", n ? (int)n : 5, n ? tmp : "/tmp/", n && tmp[n - 1] != '/' ? "/" : "");
    assert(mkdtemp(out));
}

static void read_exact_file(const char *path, char *buf, size_t cap) {
    FILE *f = fopen(path, "rb");
    assert(f);
    size_t n = fread(buf, 1, cap - 1, f);
    buf[n] = 0;
    assert(fclose(f) == 0);
}

static void write_mount_file(const char *mount_path, const char *payload) {
    unsigned (*create_dir)(const char *) = runtime_fs_host_override("_ZN2nn2fs15CreateDirectoryEPKc");
    unsigned (*create_file)(const char *, int64_t) = runtime_fs_host_override("_ZN2nn2fs10CreateFileEPKcl");
    unsigned (*open_file)(uint64_t *, const char *, int) = runtime_fs_host_override("_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci");
    unsigned (*write_file)(uint64_t, int64_t, const void *, uint64_t, const int32_t *) =
        runtime_fs_host_override("_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE");
    void (*close_file)(uint64_t) = runtime_fs_host_override("_ZN2nn2fs9CloseFileENS0_10FileHandleE");
    unsigned (*commit)(const char *) = runtime_fs_host_override("_ZN2nn2fs6CommitEPKc");
    assert(create_dir && create_file && open_file && write_file && close_file && commit);

    char dir[256];
    const char *slash = strrchr(mount_path, '/');
    assert(slash && (size_t)(slash - mount_path) < sizeof dir);
    const char *colon = strchr(mount_path, ':');
    if (colon && slash > colon + 1) {
        memcpy(dir, mount_path, (size_t)(slash - mount_path));
        dir[slash - mount_path] = 0;
        assert(create_dir(dir) == 0);
    }
    size_t len = strlen(payload);
    assert(create_file(mount_path, (int64_t)len) == 0);
    uint64_t h = 0;
    assert(open_file(&h, mount_path, 3) == 0);
    assert(write_file(h, 0, payload, (uint64_t)len, NULL) == 0);
    close_file(h);
    char mname[32];
    assert(colon && (size_t)(colon - mount_path) < sizeof mname);
    memcpy(mname, mount_path, (size_t)(colon - mount_path));
    mname[colon - mount_path] = 0;
    assert(commit(mname) == 0);
}

static void check_external_mode(const char *profile, const char *dm_save_env, const char *expected_save_dir) {
    setenv("TOTK_PROFILE_DIR", profile, 1);
    setenv("DM_SAVE", dm_save_env, 1);

    uint64_t uid[2] = {0, 0};
    runtime_account_uid(uid);
    assert(uid[0] == 0x78626c0000000001ull && uid[1] == 0x53574e4154495645ull);
    char hex[33];
    snprintf(hex, sizeof hex, "%016llx%016llx", (unsigned long long)uid[1], (unsigned long long)uid[0]);
    assert(!strcmp(hex, "53574e415449564578626c0000000001"));

    unsigned (*mount_save)(const char *, const uint64_t *) =
        runtime_fs_host_override("_ZN2nn2fs13MountSaveDataEPKcRKNS_7account3UidE");
    unsigned (*mount_dev)(const char *) =
        runtime_fs_host_override("_ZN2nn2fs19MountDeviceSaveDataEPKc");
    assert(mount_save && mount_dev);

    uint64_t wrong_uid[2] = {1, 2};
    assert(mount_save("save", wrong_uid) == 0x7d402u);
    assert(mount_save("save", uid) == 0);
    assert(mount_dev("dev") == 0);

    write_mount_file("save:/slot_00/progress.sav", "account-payload");
    write_mount_file("dev:/device.sav", "device-payload");

    char buf[128], path[512];
    snprintf(path, sizeof path, "%s/slot_00/progress.sav", expected_save_dir);
    read_exact_file(path, buf, sizeof buf);
    assert(!strcmp(buf, "account-payload"));

    snprintf(path, sizeof path, "%s_device/device.sav", expected_save_dir);
    read_exact_file(path, buf, sizeof buf);
    assert(!strcmp(buf, "device-payload"));

    /* No <uid>/ nesting inside DM_SAVE and no users.txt or save/ in TOTK_PROFILE_DIR. */
    struct stat st;
    snprintf(path, sizeof path, "%s/%s", expected_save_dir, hex);
    assert(stat(path, &st) != 0);
    snprintf(path, sizeof path, "%s/users.txt", profile);
    assert(stat(path, &st) != 0);
    snprintf(path, sizeof path, "%s/save", profile);
    assert(stat(path, &st) != 0);
    assert(cloud_started == 0 && cloud_changed == 0);
}

static void check_default_mode(const char *profile, const char *empty_dm_save) {
    setenv("TOTK_PROFILE_DIR", profile, 1);
    if (empty_dm_save) setenv("DM_SAVE", empty_dm_save, 1);
    else unsetenv("DM_SAVE");

    uint64_t uid[2] = {0, 0};
    runtime_account_uid(uid);
    assert(uid[0] || uid[1]);
    char hex[33];
    snprintf(hex, sizeof hex, "%016llx%016llx", (unsigned long long)uid[1], (unsigned long long)uid[0]);

    unsigned (*mount_save)(const char *, const uint64_t *) =
        runtime_fs_host_override("_ZN2nn2fs13MountSaveDataEPKcRKNS_7account3UidE");
    assert(mount_save && mount_save("save", uid) == 0);
    /* device save data stays the game library's without DM_SAVE */
    assert(!runtime_fs_host_override("_ZN2nn2fs19MountDeviceSaveDataEPKc"));
    assert(!runtime_fs_host_override("_ZN2nn2fs21GetDeviceSaveDataSizeEPlS1_") && !runtime_fs_host_override("_ZN2nn2fs20ExtendDeviceSaveDataEll"));
    write_mount_file("save:/slot_00/progress.sav", "default-payload");

    char buf[128], path[512];
    snprintf(path, sizeof path, "%s/save/%s/slot_00/progress.sav", profile, hex);
    read_exact_file(path, buf, sizeof buf);
    assert(!strcmp(buf, "default-payload"));

    struct stat st;
    snprintf(path, sizeof path, "%s/users.txt", profile);
    assert(stat(path, &st) == 0 && S_ISREG(st.st_mode));
    assert(cloud_started == 1 && cloud_changed == 1);
}

int main(void) {
    char root[256];
    new_dir(root);

    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        char profile[512], ext_env[512], ext_dir[512];
        snprintf(profile, sizeof profile, "%s/p1", root);
        snprintf(ext_dir, sizeof ext_dir, "%s/ext/nested/save", root);
        snprintf(ext_env, sizeof ext_env, "%s///", ext_dir);
        check_external_mode(profile, ext_env, ext_dir);
        _exit(0);
    }
    int status = 0;
    assert(waitpid(pid, &status, 0) == pid && status == 0);

    pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        char profile[512];
        snprintf(profile, sizeof profile, "%s/p2", root);
        check_default_mode(profile, NULL);
        _exit(0);
    }
    assert(waitpid(pid, &status, 0) == pid && status == 0);

    pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        char profile[512];
        snprintf(profile, sizeof profile, "%s/p3", root);
        check_default_mode(profile, "");
        _exit(0);
    }
    assert(waitpid(pid, &status, 0) == pid && status == 0);

    puts("pass: dm_save_override (fixed UID, flat DM_SAVE, _device mount, default fallback)");
    return 0;
}
