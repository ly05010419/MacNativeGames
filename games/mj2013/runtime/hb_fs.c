/* fsp-srv as a libnx homebrew reaches it (fs.c, fs_dev.c, romfs_dev.c): the SD card (sdmc:) as a folder of the profile
   (sdmc_path.h), and the game's own data (romfs:) as the RomFS image read from the player's game (TOTK_ROMFS, default
   extracted/v0/romfs, file romfs.bin). Commands follow switchbrew "Filesystem services"; results are fs's own codes
   (module 2), which libnx's fsdev turns into errno. */
#include "hb.h"
#include "sdmc_path.h"
#include "runtime_tables.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <TargetConditionals.h>

enum { R_PATH_NOT_FOUND = 0x202, R_PATH_EXISTS = 0x402, R_NOT_EMPTY = 0x7802 /* (2, 0x3c) DirectoryNotEmpty */,
       R_BAD_PATH = 0x2ee602 /* InvalidPathFormat */, R_OUT_OF_RANGE = 0x2ee02 /* OutOfRange */ };
#define FS_MAX_PATH 0x301

static char profile[1024];
const char *runtime_profile_dir(void) {
    if (profile[0]) return profile;
    const char *e = getenv("TOTK_PROFILE_DIR"), *home = getenv("HOME");
#if TARGET_OS_IPHONE
    if (e && *e) snprintf(profile, sizeof profile, "%s", e); else snprintf(profile, sizeof profile, "%s/Documents/profile", home ? home : ".");
#else
    if (e && *e) snprintf(profile, sizeof profile, "%s", e); else snprintf(profile, sizeof profile, "%s/Library/Application Support/MacNativeGames/mj2013/profile", home ? home : ".");
#endif
    return profile;
}
static uint32_t errno_result(int e) { return e == ENOENT || e == ENOTDIR ? R_PATH_NOT_FOUND : e == EEXIST ? R_PATH_EXISTS : e == ENOTEMPTY ? R_NOT_EMPTY : 0x3e802 /* (2, 500) */; }
static int mkdirs(const char *path) {   /* every parent of path (not path itself) */
    char tmp[2048]; snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) if (*p == '/') { *p = 0; if (mkdir(tmp, 0755) && errno != EEXIST) return -1; *p = '/'; }
    return 0;
}
/* The path the request sends (an X buffer of FS_MAX_PATH bytes) as a host path; 0 or a result code. */
static uint32_t request_path(const IpcRequest *rq, unsigned i, char *out, size_t cap) {
    char guest[FS_MAX_PATH + 1]; hb_in_string(rq, i, guest, sizeof guest);
    if (sdmc_host_path(runtime_profile_dir(), guest, out, cap)) { HB_LOG("fs: refused path \"%s\"", guest); return R_BAD_PATH; }
    return 0;
}

/* ---------- IFile ---------- */
typedef struct { int fd; unsigned mode; } File;
static void file_close(void *p) { File *f = p; close(f->fd); free(f); }
static void file(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    File *f = s->state;
    switch (rq->cmd) {
    case 0: {                                                    /* Read(u32 option, s64 offset, u64 size) -> u64 bytes */
        int64_t off = IPC_ARG(rq, int64_t, 8); uint64_t size = IPC_ARG(rq, uint64_t, 16);
        const IpcBuffer *b = hb_out_buffer(rq, 0); if (!b || size > b->size) size = b ? b->size : 0;
        ssize_t got = size ? pread(f->fd, ipc_guest(b->addr, size), size, off) : 0;
        static int trace = -1; if (trace < 0) trace = getenv("HB_TRACE_IPC") != NULL;
        if (trace) HB_LOG("fs: read %lld bytes at %lld -> %zd", (long long)size, (long long)off, got);
        if (got < 0) { rs->result = errno_result(errno); return; }
        HB_OUT_U64(rs, (uint64_t)got); return;
    }
    case 1: {                                                    /* Write(u32 option, s64 offset, u64 size) */
        int64_t off = IPC_ARG(rq, int64_t, 8); uint64_t size = IPC_ARG(rq, uint64_t, 16);
        const IpcBuffer *b = hb_in_buffer(rq, 0); if (!b || size > b->size) size = b ? b->size : 0;
        if (size && pwrite(f->fd, ipc_guest(b->addr, size), size, off) != (ssize_t)size) rs->result = errno_result(errno);
        return;
    }
    case 2: fsync(f->fd); return;                                /* Flush */
    case 3: if (ftruncate(f->fd, IPC_ARG(rq, int64_t, 0))) rs->result = errno_result(errno); return;   /* SetSize */
    case 4: { struct stat st; if (fstat(f->fd, &st)) { rs->result = errno_result(errno); return; } HB_OUT_U64(rs, (uint64_t)st.st_size); return; }   /* GetSize */
    default: HB_UNKNOWN(s, rq);
    }
}
/* ---------- IDirectory ---------- */
typedef struct { char path[2048]; unsigned mode; long pos; } Dir;
typedef struct { char name[FS_MAX_PATH]; uint8_t pad[3]; int8_t type; uint8_t pad2[3]; int64_t size; } DirEntry;
_Static_assert(sizeof(DirEntry) == 0x310, "FsDirectoryEntry");
/* The next entry of the listing the mode asks for (1 directories, 2 files), skipping "." and ".."; 0 at the end. */
static int dir_next(Dir *d, DirEntry *e) {
    DIR *dp = opendir(d->path); if (!dp) return 0;
    long index = 0; struct dirent *de; int found = 0;
    while ((de = readdir(dp))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char full[4096]; snprintf(full, sizeof full, "%s/%s", d->path, de->d_name);
        struct stat st; if (stat(full, &st)) continue;
        int is_dir = S_ISDIR(st.st_mode);
        if (!(d->mode & (is_dir ? 1 : 2))) continue;
        if (index++ < d->pos) continue;
        memset(e, 0, sizeof *e); snprintf(e->name, sizeof e->name, "%s", de->d_name); e->type = is_dir ? 0 : 1; e->size = is_dir ? 0 : st.st_size;
        found = 1; d->pos++; break;
    }
    closedir(dp); return found;
}
static void directory(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    Dir *d = s->state;
    switch (rq->cmd) {
    case 0: {                                                    /* Read -> s64 count, entries in the B buffer */
        const IpcBuffer *b = hb_out_buffer(rq, 0); int64_t n = 0;
        if (b) { DirEntry *out = ipc_guest(b->addr, b->size); uint64_t max = b->size / sizeof(DirEntry); while ((uint64_t)n < max && dir_next(d, &out[n])) n++; }
        IPC_OUT(rs, n); return;
    }
    case 1: { Dir copy = *d; copy.pos = 0; DirEntry e; int64_t n = 0; while (dir_next(&copy, &e)) n++; IPC_OUT(rs, n); return; }   /* GetEntryCount */
    default: HB_UNKNOWN(s, rq);
    }
}
/* ---------- IFileSystem (the SD card) ---------- */
static void filesystem(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    char path[2048], other[2048]; uint32_t r;
    switch (rq->cmd) {
    case 0: {                                                    /* CreateFile(u32 option, s64 size) */
        if ((r = request_path(rq, 0, path, sizeof path))) { rs->result = r; return; }
        int64_t size = IPC_ARG(rq, int64_t, 8);
        if (mkdirs(path)) { rs->result = errno_result(errno); return; }
        int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0644); if (fd < 0) { rs->result = errno_result(errno); return; }
        if (size > 0 && ftruncate(fd, size)) rs->result = errno_result(errno);
        close(fd); return;
    }
    case 1: if ((r = request_path(rq, 0, path, sizeof path))) { rs->result = r; return; } if (unlink(path)) rs->result = errno_result(errno); return;   /* DeleteFile */
    case 2: if ((r = request_path(rq, 0, path, sizeof path))) { rs->result = r; return; } if (mkdirs(path) || mkdir(path, 0755)) rs->result = errno_result(errno); return;   /* CreateDirectory */
    case 3: if ((r = request_path(rq, 0, path, sizeof path))) { rs->result = r; return; } if (rmdir(path)) rs->result = errno_result(errno); return;   /* DeleteDirectory */
    case 5: case 6:                                              /* RenameFile / RenameDirectory */
        if ((r = request_path(rq, 0, path, sizeof path)) || (r = request_path(rq, 1, other, sizeof other))) { rs->result = r; return; }
        if (rename(path, other)) rs->result = errno_result(errno);
        return;
    case 7: {                                                    /* GetEntryType -> u32 (0 directory, 1 file) */
        if ((r = request_path(rq, 0, path, sizeof path))) { rs->result = r; return; }
        struct stat st; if (stat(path, &st)) { if (!strcmp(path + strlen(path) - 5, "/sdmc")) { HB_OUT_U32(rs, 0); return; } rs->result = errno_result(errno); return; }
        HB_OUT_U32(rs, S_ISDIR(st.st_mode) ? 0 : 1); return;
    }
    case 8: {                                                    /* OpenFile(u32 mode: 1 read, 2 write, 4 append) */
        if ((r = request_path(rq, 0, path, sizeof path))) { rs->result = r; return; }
        unsigned mode = IPC_ARG(rq, uint32_t, 0);
        int fd = open(path, (mode & 2) ? O_RDWR : O_RDONLY);
        static int trace = -1; if (trace < 0) trace = getenv("HB_TRACE_IPC") != NULL;
        if (trace) HB_LOG("fs: open %s mode %u -> %s", path + strlen(runtime_profile_dir()), mode, fd < 0 ? strerror(errno) : "ok");
        if (fd < 0) { rs->result = errno_result(errno); return; }
        struct stat st; if (!fstat(fd, &st) && S_ISDIR(st.st_mode)) { close(fd); rs->result = R_PATH_NOT_FOUND; return; }
        File *f = calloc(1, sizeof *f); f->fd = fd; f->mode = mode;
        hb_session(rs, "IFile", file, f, file_close); return;
    }
    case 9: {                                                    /* OpenDirectory(u32 mode: 1 directories, 2 files) */
        if ((r = request_path(rq, 0, path, sizeof path))) { rs->result = r; return; }
        struct stat st; if (!strcmp(path + strlen(path) - 5, "/sdmc")) mkdir(path, 0755);
        if (stat(path, &st) || !S_ISDIR(st.st_mode)) { rs->result = R_PATH_NOT_FOUND; return; }
        Dir *d = calloc(1, sizeof *d); snprintf(d->path, sizeof d->path, "%s", path); d->mode = IPC_ARG(rq, uint32_t, 0);
        hb_session(rs, "IDirectory", directory, d, free); return;
    }
    case 10: sync(); return;                                     /* Commit */
    case 11: HB_OUT_U64(rs, 16ULL << 30); return;                /* GetFreeSpaceSize */
    case 12: HB_OUT_U64(rs, 32ULL << 30); return;                /* GetTotalSpaceSize */
    case 14: {                                                   /* GetFileTimeStampRaw */
        if ((r = request_path(rq, 0, path, sizeof path))) { rs->result = r; return; }
        struct stat st; if (stat(path, &st)) { rs->result = errno_result(errno); return; }
        struct { uint64_t created, modified, accessed; uint8_t valid, pad[7]; } t = {(uint64_t)st.st_ctime, (uint64_t)st.st_mtime, (uint64_t)st.st_atime, 1, {0}};
        IPC_OUT(rs, t); return;
    }
    default: HB_UNKNOWN(s, rq);
    }
}
/* ---------- IStorage: the game's RomFS image ---------- */
static int romfs_fd = -1;
static uint64_t romfs_size;
static void romfs_open(void) {
    if (romfs_fd >= 0) return;
    const char *dir = getenv("TOTK_ROMFS"); char path[2048];
    snprintf(path, sizeof path, "%s/romfs.bin", dir && *dir ? dir : "extracted/v0/romfs");
    romfs_fd = open(path, O_RDONLY);
    if (romfs_fd < 0) { HB_LOG("fs: RomFS image %s: %s", path, strerror(errno)); runtime_fail("RomFS image missing (TOTK_ROMFS)"); }
    struct stat st; fstat(romfs_fd, &st); romfs_size = (uint64_t)st.st_size;
    if (romfs_size != ROMFS_SIZE) { HB_LOG("fs: RomFS image %s is %llu bytes, the player's game has %llu", path, (unsigned long long)romfs_size, (unsigned long long)ROMFS_SIZE); runtime_fail("RomFS image differs from the prepared game"); }
    HB_LOG("fs: RomFS %s (%llu bytes)", path, (unsigned long long)romfs_size);
}
static void storage(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 0: {                                                    /* Read(s64 offset, u64 size) */
        int64_t off = IPC_ARG(rq, int64_t, 0); uint64_t size = IPC_ARG(rq, uint64_t, 8);
        const IpcBuffer *b = hb_out_buffer(rq, 0);
        if (off < 0 || (uint64_t)off > romfs_size || !b || size > b->size) { rs->result = R_OUT_OF_RANGE; return; }
        if ((uint64_t)off + size > romfs_size) size = romfs_size - (uint64_t)off;
        static int trace = -1; if (trace < 0) trace = getenv("HB_TRACE_IPC") != NULL;
        if (trace) HB_LOG("fs: romfs read %llu bytes at %lld", (unsigned long long)size, (long long)off);
        if (size && pread(romfs_fd, ipc_guest(b->addr, size), size, off) != (ssize_t)size) rs->result = errno_result(errno);
        return;
    }
    case 4: HB_OUT_U64(rs, romfs_size); return;                  /* GetSize */
    default: HB_UNKNOWN(s, rq);
    }
}
static void fsp_srv(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 1: return;                                              /* SetCurrentProcess */
    case 18: {                                                   /* OpenSdCardFileSystem */
        char root[2048]; snprintf(root, sizeof root, "%s/sdmc/", runtime_profile_dir()); mkdirs(root);
        hb_session(rs, "IFileSystem:sdmc", filesystem, NULL, NULL); return;
    }
    case 200: romfs_open(); hb_session(rs, "IStorage:romfs", storage, NULL, NULL); return;   /* OpenDataStorageByCurrentProcess */
    case 1005: HB_OUT_U32(rs, 0); return;                        /* GetGlobalAccessLogMode */
    default: HB_UNKNOWN(s, rq);
    }
}
void hb_fs_register(void) { runtime_ipc_register("fsp-srv", fsp_srv, NULL, NULL); }
