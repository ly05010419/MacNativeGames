#ifndef TMNT_IMAGE_H
#define TMNT_IMAGE_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    size_t file_size;
    size_t image_size;
    size_t key_offset;
    size_t patch_offset;
    size_t patch_size;
    uint8_t sha256[32];
} TmntImageSpec;

typedef struct {
    uint8_t *base;
    size_t mapped_size;
    size_t key_offset;
    size_t patch_offset;
    size_t patch_size;
    int sealed;
} TmntImage;

typedef enum {
    TMNT_IMAGE_OK = 0,
    TMNT_IMAGE_ARGUMENT,
    TMNT_IMAGE_LAYOUT,
    TMNT_IMAGE_IO,
    TMNT_IMAGE_DIGEST,
    TMNT_IMAGE_MEMORY
} TmntImageStatus;

/* Maps an after-image patch layout into private, non-executable RW memory.
   The expected digest must come from a separately verified rewrite record.
   Leading patch areas and code execution are intentionally not implemented. */
TmntImageStatus tmnt_image_map(const char *path, const TmntImageSpec *spec, TmntImage *out);
/* After relocations, protect text/patch as RX, read-only pages as R and data
   as RW. This does not validate or execute code. A protection failure unmaps
   the image. Call only with independently verified segment boundaries. */
TmntImageStatus tmnt_image_seal(TmntImage *image, size_t text_end, size_t data_offset);
void tmnt_image_unmap(TmntImage *image);
#endif
