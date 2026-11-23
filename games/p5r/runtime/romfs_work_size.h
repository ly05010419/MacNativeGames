/* Horizon sizes a RomFS mount's cache as the RomFS's file table, RomFsFileSystem::GetRequiredWorkingMemorySize: both
   hash tables (4 bytes a bucket, the RomFS builder's bucket count for the entries) plus every directory and file entry
   (0x18 / 0x20 bytes and the name, padded to 4). Mario Kart 8 Deluxe decrypts its add-on content in a 0x3130-byte heap
   that also holds this cache, so a bigger answer leaves the decryption buffer no room (found in nxnative's notes on this
   game: a null destination in crypto::detail::CbcModeAes128Helper::DecryptBlocks; its two files give 0x88).
   runtime/fs_host.c QueryMountAddOnContentCacheSize; tests/test_mk8dx.py. */
#pragma once
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static inline uint64_t romfs_buckets(uint64_t n) {
    if (n < 3) return 3;
    if (n < 19) return n | 1;
    while (n % 2 == 0 || n % 3 == 0 || n % 5 == 0 || n % 7 == 0 || n % 11 == 0 || n % 13 == 0 || n % 17 == 0) n++;
    return n;
}
static inline void romfs_walk(const char *path, uint64_t *dirs_n, uint64_t *files_n, uint64_t *bytes) {
    DIR *d = opendir(path); if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char child[2048]; snprintf(child, sizeof child, "%s/%s", path, e->d_name); struct stat st; if (stat(child, &st)) continue;
        uint64_t name = (strlen(e->d_name) + 3) & ~3ULL;
        if (S_ISDIR(st.st_mode)) { (*dirs_n)++; *bytes += 0x18 + name; romfs_walk(child, dirs_n, files_n, bytes); }
        else { (*files_n)++; *bytes += 0x20 + name; }
    }
    closedir(d);
}
/* The working memory of the folder's files as a RomFS (the root an entry with an empty name). */
static inline uint64_t romfs_work_size(const char *path) {
    uint64_t dirs_n = 1, files_n = 0, bytes = 0x18;
    romfs_walk(path, &dirs_n, &files_n, &bytes);
    return bytes + 4 * romfs_buckets(dirs_n) + 4 * romfs_buckets(files_n);
}
