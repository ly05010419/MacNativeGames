/* C stdio on RomFS files for main (Echoes of Wisdom). Its resource loader reads files through libc++'s std::ifstream,
   whose basic_filebuf calls fopen / fread / fseeko / ftello / fclose; TotK and Bayonetta 3 only used nn::fs. The SDK's
   own fopen cannot open "rom:/..." here, because the host answers the RomFS mount itself (romfs.c), so every load
   failed quietly and the game later dereferenced an empty resource.

   These replacements are bound for main's imports only (runtime_module_override, loader.c): a "rom:" path opens the
   host RomFS read-only and returns a host stream; any other path, and every FILE* the host did not make (stdout,
   stderr), goes to the SDK's own function unchanged. The SDK and the other modules keep their stdio. */
#include "runtime.h"
#include "fs_host.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint64_t magic, handle; int64_t pos, size; int eof; } HostFile;
#define HOST_FILE_MAGIC 0x4f49445453574f45ULL   /* "EOWSTDIO" */
#define MAX_FILES 256
static HostFile *files[MAX_FILES];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static HostFile *host_file(void *f) {
    pthread_mutex_lock(&lock);
    HostFile *found = NULL;
    for (unsigned i = 0; i < MAX_FILES; i++) if (files[i] && files[i] == f) { found = files[i]; break; }
    pthread_mutex_unlock(&lock);
    return found;
}
static void *sdk(const char *name) {
    void *p = runtime_symbol(name);
    if (!p) { fprintf(stderr, "[eow-stdio] SDK %s missing\n", name); runtime_fail("SDK stdio function missing"); }
    return p;
}
static void guest_errno(int value) {   /* the SDK's errno (Horizon numbering follows newlib/musl: ENOENT 2, EBADF 9, EINVAL 22) */
    int *(*location)(void) = sdk("__errno_location");
    *location() = value;
}

static void *eow_fopen(const char *path, const char *mode) {
    if (!path || !mode || !runtime_romfs_owns(path)) return ((void *(*)(const char *, const char *))sdk("fopen"))(path, mode);
    if (mode[0] != 'r' || strchr(mode, '+')) { fprintf(stderr, "[eow-stdio] fopen(%s, %s): RomFS is read-only\n", path, mode); guest_errno(30); return NULL; }   /* EROFS */
    uint64_t h = 0;
    void runtime_fs_log(const char *what, const char *path); runtime_fs_log("fopen", path);   /* TOTK_FS_LOG */
    if (runtime_romfs_open(&h, path, 1)) { guest_errno(2); return NULL; }
    int64_t size = 0;
    if (runtime_romfs_size(&size, h)) { runtime_romfs_close(h); guest_errno(5); return NULL; }
    HostFile *f = calloc(1, sizeof *f);
    if (!f) runtime_fail("stdio stream allocation");
    *f = (HostFile){HOST_FILE_MAGIC, h, 0, size, 0};
    pthread_mutex_lock(&lock);
    unsigned i = 0;
    while (i < MAX_FILES && files[i]) i++;
    if (i == MAX_FILES) { pthread_mutex_unlock(&lock); runtime_fail("stdio stream table full"); }
    files[i] = f;
    pthread_mutex_unlock(&lock);
    return f;
}
static size_t eow_fread(void *buffer, size_t size, size_t count, void *stream) {
    HostFile *f = host_file(stream);
    if (!f) return ((size_t (*)(void *, size_t, size_t, void *))sdk("fread"))(buffer, size, count, stream);
    if (!size || !count) return 0;
    if (count > UINT64_MAX / size || !buffer) { guest_errno(22); return 0; }
    uint64_t want = (uint64_t)size * count, got = 0;
    if (f->pos >= f->size) { f->eof = 1; return 0; }
    if (runtime_romfs_read(&got, f->handle, f->pos, buffer, want)) { guest_errno(5); return 0; }
    f->pos += (int64_t)got;
    if (got < want) f->eof = 1;
    return (size_t)(got / size);
}
static size_t eow_fwrite(const void *buffer, size_t size, size_t count, void *stream) {
    if (!host_file(stream)) return ((size_t (*)(const void *, size_t, size_t, void *))sdk("fwrite"))(buffer, size, count, stream);
    guest_errno(9); return 0;   /* opened read-only */
}
static int eow_fseeko(void *stream, int64_t offset, int whence) {
    HostFile *f = host_file(stream);
    if (!f) return ((int (*)(void *, int64_t, int))sdk("fseeko"))(stream, offset, whence);
    int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? f->pos : whence == SEEK_END ? f->size : -1;
    if (base < 0 || base + offset < 0) { guest_errno(22); return -1; }
    f->pos = base + offset; f->eof = 0;
    return 0;
}
static int64_t eow_ftello(void *stream) {
    HostFile *f = host_file(stream);
    if (!f) return ((int64_t (*)(void *))sdk("ftello"))(stream);
    return f->pos;
}
static int eow_fflush(void *stream) {
    if (!stream || !host_file(stream)) return ((int (*)(void *))sdk("fflush"))(stream);
    return 0;
}
static int eow_fclose(void *stream) {
    HostFile *f = host_file(stream);
    if (!f) return ((int (*)(void *))sdk("fclose"))(stream);
    pthread_mutex_lock(&lock);
    for (unsigned i = 0; i < MAX_FILES; i++) if (files[i] == f) files[i] = NULL;
    pthread_mutex_unlock(&lock);
    runtime_romfs_close(f->handle);
    f->magic = 0; free(f);
    return 0;
}

void *runtime_module_override(const char *module, const char *name) {
    if (strcmp(module, "main")) return NULL;
    static const struct { const char *name; void *fn; } entries[] = {
        {"fopen", eow_fopen}, {"fread", eow_fread}, {"fwrite", eow_fwrite}, {"fseeko", eow_fseeko},
        {"ftello", eow_ftello}, {"fflush", eow_fflush}, {"fclose", eow_fclose},
    };
    for (size_t i = 0; i < sizeof entries / sizeof *entries; i++) if (!strcmp(name, entries[i].name)) return entries[i].fn;
    return NULL;
}
