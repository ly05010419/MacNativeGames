/* RomFS path validation shared by the runtime and game-data-free regressions. */
#pragma once
#include <stddef.h>
#include <string.h>

static inline int romfs_path_valid_relative(const char *path) {
    if (!path || !*path || *path == '/' || strchr(path, '\\')) return 0;
    for (const char *part = path; *part;) {
        const char *end = strchr(part, '/');
        size_t size = end ? (size_t)(end - part) : strlen(part);
        if (!size || (size == 1 && part[0] == '.') ||
            (size == 2 && part[0] == '.' && part[1] == '.')) return 0;
        if (!end) break;
        part = end + 1;
    }
    return 1;
}

static inline const char *romfs_path_relative(const char *mount, const char *path) {
    if (!mount || !*mount || !path) return NULL;
    size_t size = strlen(mount);
    if (strncmp(path, mount, size) || path[size] != ':' || path[size + 1] != '/')
        return NULL;
    path += size + 2;
    /* Audio stream requests join a mount root and a leading-slash resource,
       producing content://Sound/.... Normalize root separators only; keep
       rejecting traversal, backslashes and empty interior components. */
    while (*path == '/') ++path;
    return romfs_path_valid_relative(path) ? path : NULL;
}

/* A RomFS path as Horizon's fs normalizes it: separators collapsed, "." dropped, ".." resolved, never above the root
   (Metroid Dread's sound library opens rom:/system/snd/../../sounds/streams/...). Backslashes stay invalid. The result is
   only looked up in the verified RomFS index, never used as a host file path. Writes into out; NULL when invalid, empty,
   above the root or longer than cap. */
static inline const char *romfs_path_normalize(const char *mount, const char *path, char *out, size_t cap) {
    if (!mount || !*mount || !path || !out || !cap) return NULL;
    size_t size = strlen(mount);
    if (strncmp(path, mount, size) || path[size] != ':' || path[size + 1] != '/' || strchr(path, '\\')) return NULL;
    size_t n = 0;
    for (const char *part = path + size + 2; *part;) {
        const char *end = strchr(part, '/');
        size_t len = end ? (size_t)(end - part) : strlen(part);
        if (len == 2 && part[0] == '.' && part[1] == '.') {
            if (!n) return NULL;
            while (n && out[n - 1] != '/') --n;
            if (n) --n;
        } else if (len && !(len == 1 && part[0] == '.')) {
            if (n + (n ? 1 : 0) + len >= cap) return NULL;
            if (n) out[n++] = '/';
            memcpy(out + n, part, len); n += len;
        }
        if (!end) break;
        part = end + 1;
    }
    out[n] = 0;
    return n ? out : NULL;
}
