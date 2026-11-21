// Real folder-backed OpenDirectory: allocation failure must return an error,
// leave the output untouched, and close the DIR already opened by the handler.
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>

static void *directory_calloc(size_t count, size_t size);
static DIR *directory_opendir(const char *path);
static int directory_closedir(DIR *d);
#define calloc directory_calloc
#define opendir directory_opendir
#define closedir directory_closedir
#include "../hle_fs.c"
#undef closedir
#undef opendir
#undef calloc

struct dm_flags g_flag;
uint64_t g_t0_ns;
uint64_t now_ns(void) { return 0; }
void dm_guest_write_prepare(void *a, size_t n) { (void)a; (void)n; }

static int fail_handle, failed_allocations, opened_fd = -1;
static void *directory_calloc(size_t count, size_t size) {
    if (fail_handle && count == 1 && size == sizeof(dirh_t)) {
        failed_allocations++;
        errno = ENOMEM;
        return NULL;
    }
    return calloc(count, size);
}
static DIR *directory_opendir(const char *path) {
    DIR *d = opendir(path);
    if (d) opened_fd = dirfd(d);
    return d;
}

static int directory_closedir(DIR *d) {
    int r = closedir(d);
    // Successful cleanup may change errno; it must not hide the allocation error.
    if (fail_handle) errno = ENOENT;
    return r;
}

static int checks, fails;
#define CHECK(c, text) do { checks++; int ok = !!(c); fails += !ok; printf("%s %s\n", ok ? "PASS" : "FAIL", text); } while (0)

int main(void) {
    char root[1024], file[1100];
    const char *tmp = getenv("TMPDIR");
    snprintf(root, sizeof root, "%s/t_fs_directory.XXXXXX", tmp ? tmp : "/private/tmp");
    if (!mkdtemp(root)) { perror("mkdtemp"); return 2; }
    snprintf(file, sizeof file, "%s/entry", root);
    FILE *f = fopen(file, "w");
    if (!f) { perror("fopen"); rmdir(root); return 2; }
    fputs("hello", f); fclose(f);
    CHECK(mount("test", root, 0) == RES_OK, "mount the synthetic folder");

    const uint64_t initial[] = {0, 0x1234};
    for (size_t i = 0; i < sizeof initial / sizeof initial[0]; i++) {
        uint64_t h = initial[i];
        opened_fd = -1; failed_allocations = 0; fail_handle = 1;
        Result r = h_fs_OpenDirectory(&h, "test:/", DIRMODE_DIR | DIRMODE_FILE);
        int error = errno;
        fail_handle = 0;
        CHECK(failed_allocations == 1, "inject failure into the directory-handle allocation");
        CHECK(r == RES_FAIL, "failed allocation returns the filesystem resource failure");
        CHECK(error == ENOMEM, "allocation error survives directory cleanup");
        CHECK(h == initial[i], "failed open leaves the caller's handle untouched");
        CHECK(opened_fd >= 0, "the handler opened a real directory before allocation failed");
        errno = 0;
        CHECK(fcntl(opened_fd, F_GETFD) == -1 && errno == EBADF, "failed open closes the real directory descriptor");
    }
    h_fs_CloseDirectory(0);

    uint64_t h = 0;
    Result r = h_fs_OpenDirectory(&h, "test:/", DIRMODE_DIR | DIRMODE_FILE);
    int fd = opened_fd;
    CHECK(r == RES_OK && h != 0, "a later successful open publishes a handle");
    if (r == RES_OK && h) {
        CHECK(fcntl(fd, F_GETFD) >= 0, "successful open keeps its directory descriptor alive");
        int64_t count = -1;
        CHECK(h_fs_DirCount(&count, h) == RES_OK && count == 1, "the successful handle counts the one synthetic file");
        DirectoryEntry entry[2] = {0};
        CHECK(h_fs_ReadDirectory(&count, entry, h, 2) == RES_OK && count == 1 &&
              !strcmp(entry[0].name, "entry") && entry[0].type == ENT_FILE && entry[0].size == 5,
              "the successful handle reads the file name, type and size");
        h_fs_CloseDirectory(h);
        errno = 0;
        CHECK(fcntl(fd, F_GETFD) == -1 && errno == EBADF, "CloseDirectory releases the successful handle's descriptor");
    }
    CHECK(!unlink(file) && !rmdir(root), "remove the synthetic fixture");
    printf("== fs directory: %d checks, %d failed ==\n", checks, fails);
    return fails != 0;
}
