#include "image.h"

#include <CommonCrypto/CommonDigest.h>
#include <fcntl.h>
#include <limits.h>
#include <libkern/OSCacheControl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define TMNT_KEY_PAGE_SIZE ((size_t)0x4000)

TmntImageStatus tmnt_image_map(const char *path, const TmntImageSpec *spec, TmntImage *out) {
    if (!path || !spec || !out) return TMNT_IMAGE_ARGUMENT;
    memset(out, 0, sizeof *out);
    if (spec->key_offset % TMNT_KEY_PAGE_SIZE ||
        spec->key_offset > SIZE_MAX - TMNT_KEY_PAGE_SIZE ||
        spec->image_size != spec->key_offset + TMNT_KEY_PAGE_SIZE ||
        spec->patch_offset < spec->image_size ||
        !spec->patch_size || spec->patch_offset > SIZE_MAX - spec->patch_size ||
        spec->file_size != spec->patch_offset + spec->patch_size)
        return TMNT_IMAGE_LAYOUT;
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0 || spec->file_size > SIZE_MAX - ((size_t)page - 1)) return TMNT_IMAGE_LAYOUT;
    size_t mapped_size = ((spec->file_size + (size_t)page - 1) / (size_t)page) * (size_t)page;
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return TMNT_IMAGE_IO;
    struct stat info;
    if (fstat(fd, &info) || !S_ISREG(info.st_mode) || info.st_size < 0 || (uintmax_t)info.st_size != spec->file_size) {
        close(fd);
        return TMNT_IMAGE_IO;
    }
    uint8_t *base = mmap(NULL, mapped_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (base == MAP_FAILED) { close(fd); return TMNT_IMAGE_MEMORY; }
    CC_SHA256_CTX hash;
    CC_SHA256_Init(&hash);
    size_t offset = 0;
    while (offset < spec->file_size) {
        size_t amount = spec->file_size - offset;
        if (amount > 1024 * 1024) amount = 1024 * 1024;
        ssize_t got = read(fd, base + offset, amount);
        if (got <= 0) { munmap(base, mapped_size); close(fd); return TMNT_IMAGE_IO; }
        CC_SHA256_Update(&hash, base + offset, (CC_LONG)got);
        offset += (size_t)got;
    }
    close(fd);
    uint8_t digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_Final(digest, &hash);
    if (memcmp(digest, spec->sha256, sizeof digest)) {
        munmap(base, mapped_size);
        return TMNT_IMAGE_DIGEST;
    }
    out->base = base;
    out->mapped_size = mapped_size;
    out->key_offset = spec->key_offset;
    out->patch_offset = spec->patch_offset;
    out->patch_size = spec->patch_size;
    return TMNT_IMAGE_OK;
}

TmntImageStatus tmnt_image_seal(TmntImage *image, size_t text_end, size_t data_offset) {
    if (!image || !image->base || image->sealed) return TMNT_IMAGE_ARGUMENT;
    size_t page = TMNT_KEY_PAGE_SIZE;
    if (sysconf(_SC_PAGESIZE) != (long)page ||
        (uintptr_t)image->base % page || image->mapped_size % page ||
        !text_end || text_end > SIZE_MAX - (page - 1) ||
        image->key_offset % page || image->patch_offset % page ||
        image->key_offset > SIZE_MAX - page ||
        image->patch_offset < image->key_offset + page ||
        !image->patch_size || image->patch_offset > SIZE_MAX - image->patch_size ||
        image->patch_offset + image->patch_size > image->mapped_size ||
        data_offset > image->key_offset) return TMNT_IMAGE_LAYOUT;
    size_t text_protect = (text_end + page - 1) & ~(page - 1);
    size_t ro_end = data_offset & ~(page - 1);
    if (text_protect > ro_end || image->patch_offset >= image->mapped_size)
        return TMNT_IMAGE_LAYOUT;
    sys_icache_invalidate(image->base, text_protect);
    sys_icache_invalidate(image->base + image->patch_offset,
                          image->mapped_size - image->patch_offset);
    if (mprotect(image->base, text_protect, PROT_READ | PROT_EXEC) ||
        (ro_end > text_protect && mprotect(image->base + text_protect, ro_end - text_protect, PROT_READ)) ||
        mprotect(image->base + image->key_offset, page, PROT_READ) ||
        mprotect(image->base + image->patch_offset, image->mapped_size - image->patch_offset, PROT_READ | PROT_EXEC)) {
        tmnt_image_unmap(image);
        return TMNT_IMAGE_MEMORY;
    }
    image->sealed = 1;
    return TMNT_IMAGE_OK;
}

void tmnt_image_unmap(TmntImage *image) {
    if (image && image->base) {
        munmap(image->base, image->mapped_size);
        memset(image, 0, sizeof *image);
    }
}
