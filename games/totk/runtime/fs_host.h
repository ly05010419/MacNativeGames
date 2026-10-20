/* Unified nn::fs host layer: RomFS (romfs.c) plus writable save, temporary and cache storage. */
#pragma once
#include <stdint.h>

void *runtime_fs_host_override(const char *name);

/* romfs.c */
int runtime_romfs_owns(const char *path);
int runtime_romfs_handle(uint64_t h);
unsigned runtime_romfs_open(uint64_t *out, const char *path, int mode);
unsigned runtime_romfs_read(uint64_t *out, uint64_t h, int64_t offset, void *buffer, uint64_t size);
unsigned runtime_romfs_size(int64_t *out, uint64_t h);
void runtime_romfs_close(uint64_t h);
unsigned runtime_romfs_entry_type(int *out, const char *path);

/* account.c: the local user identity */
void runtime_account_uid(uint64_t out[2]);

#ifdef GAME_BOTW
/* aoc_romfs.c (Breath of the Wild only): the add-on content volumes. fs_host.c and services.c call them through these
   macros, appended to their existing lines, so the Tears of the Kingdom builds preprocess to the same text. */
void runtime_aoc_announce(void);
int runtime_aoc_count(void);
int runtime_aoc_list(int *out, int offset, int count);
unsigned runtime_aoc_cache_size(uint64_t *out, int index);
unsigned runtime_aoc_mount(const char *name, int index, void *cache, uint64_t size);
int runtime_aoc_owns(const char *path);
int runtime_aoc_owns_name(const char *name);
void runtime_aoc_unmount(const char *name);
unsigned runtime_aoc_open(uint64_t *out, const char *path, int mode);
int runtime_aoc_handle(uint64_t h);
unsigned runtime_aoc_read(uint64_t *out, uint64_t h, int64_t offset, void *buffer, uint64_t size);
unsigned runtime_aoc_size(int64_t *out, uint64_t h);
void runtime_aoc_close(uint64_t h);
unsigned runtime_aoc_entry_type(int *out, const char *path);
#define BOTW_AOC_OPEN(out, path, mode) if (runtime_aoc_owns(path)) return runtime_aoc_open((out), (path), (mode));
#define BOTW_AOC_READ(out, h, off, buf, size) if (runtime_aoc_handle(h)) return runtime_aoc_read((out), (h), (off), (buf), (size));
#define BOTW_AOC_SIZE(out, h) if (runtime_aoc_handle(h)) return runtime_aoc_size((out), (h));
#define BOTW_AOC_CLOSE(h) if (runtime_aoc_handle(h)) { runtime_aoc_close(h); return; }
#define BOTW_AOC_TYPE(out, path) if (runtime_aoc_owns(path)) return runtime_aoc_entry_type((out), (path));
#define BOTW_AOC_UNMOUNT(name) if (runtime_aoc_owns_name(name)) { runtime_aoc_unmount(name); return; }
#define BOTW_AOC_TABLE {"_ZN2nn2fs17MountAddOnContentEPKciPvm", runtime_aoc_mount}, {"_ZN2nn2fs31QueryMountAddOnContentCacheSizeEPmi", runtime_aoc_cache_size},
#else
#define BOTW_AOC_OPEN(out, path, mode)
#define BOTW_AOC_READ(out, h, off, buf, size)
#define BOTW_AOC_SIZE(out, h)
#define BOTW_AOC_CLOSE(h)
#define BOTW_AOC_TYPE(out, path)
#define BOTW_AOC_UNMOUNT(name)
#define BOTW_AOC_TABLE
#endif
