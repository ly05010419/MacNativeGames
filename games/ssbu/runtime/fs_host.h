/* Unified nn::fs host layer: RomFS (romfs.c) plus writable save, temporary and cache storage. */
#pragma once
#include <stdint.h>

void *runtime_fs_host_override(const char *name);

/* romfs.c */
int runtime_romfs_owns(const char *path);
int runtime_romfs_handle(uint64_t h);
unsigned runtime_romfs_list(const char *path, int mode, void (*add)(void *ctx, const char *name, int is_dir, int64_t size), void *ctx);
unsigned runtime_romfs_open(uint64_t *out, const char *path, int mode);
unsigned runtime_romfs_read(uint64_t *out, uint64_t h, int64_t offset, void *buffer, uint64_t size);
unsigned runtime_romfs_size(int64_t *out, uint64_t h);
void runtime_romfs_close(uint64_t h);
unsigned runtime_romfs_entry_type(int *out, const char *path);

/* account.c: the local user identity */
void runtime_account_uid(uint64_t out[2]);

#define BOTW_AOC_OPEN(out, path, mode)
#define BOTW_AOC_READ(out, h, off, buf, size)
#define BOTW_AOC_SIZE(out, h)
#define BOTW_AOC_CLOSE(h)
#define BOTW_AOC_TYPE(out, path)
#define BOTW_AOC_UNMOUNT(name)
#define BOTW_AOC_TABLE
