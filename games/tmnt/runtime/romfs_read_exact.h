#ifndef TMNT_ROMFS_READ_EXACT_H
#define TMNT_ROMFS_READ_EXACT_H

#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

typedef ssize_t (*tmnt_romfs_pread_fn)(int, void *, size_t, off_t);

/* Preserve the synchronous guest read contract across short host reads and
   interrupted syscalls. The caller owns the virtual-core handoff. */
static inline int tmnt_romfs_read_exact(int fd, void *buffer, uint64_t size,
                                        uint64_t offset, size_t max_chunk,
                                        tmnt_romfs_pread_fn read_fn,
                                        uint64_t *done) {
    *done = 0;
    if (!max_chunk || !read_fn) return -1;
    while (*done < size) {
        size_t chunk = (size_t)(size - *done);
        if (chunk > max_chunk) chunk = max_chunk;
        ssize_t n = read_fn(fd, (uint8_t *)buffer + *done, chunk,
                            (off_t)(offset + *done));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        *done += (uint64_t)n;
    }
    return 0;
}

#endif
