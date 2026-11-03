/* Verified, optional ACNH add-on images. File descriptors are read-only image slices. */
#pragma once
#include <stdint.h>
#include <stddef.h>

void runtime_dlc_init(void);
int runtime_dlc_count(void);
int runtime_dlc_list(int *out, int offset, int count);
int runtime_dlc_has(int index);
unsigned runtime_dlc_mount(const char *name, int index);
int runtime_dlc_unmount(const char *name);
int runtime_dlc_owns(const char *path);
unsigned runtime_dlc_open(int *fd, uint64_t *base, uint64_t *size, const char *path, int mode);
unsigned runtime_dlc_entry_type(int *out, const char *path);
unsigned runtime_dlc_directory(const char *path, int mode,
    void (*add)(void *, const char *, int, int64_t), void *ctx);
void runtime_dlc_probe(void);
