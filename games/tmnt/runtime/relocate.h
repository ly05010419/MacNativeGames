#ifndef TMNT_RELOCATE_H
#define TMNT_RELOCATE_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t offset;
    uint32_t kind;
    uint32_t symbol;
    int64_t addend;
    int weak;
} TmntRelocation;

typedef int (*TmntResolveSymbol)(void *context, uint32_t symbol, uintptr_t *address);

typedef enum {
    TMNT_RELOC_OK = 0,
    TMNT_RELOC_ARGUMENT,
    TMNT_RELOC_NO_MEMORY,
    TMNT_RELOC_KIND,
    TMNT_RELOC_BOUNDS,
    TMNT_RELOC_OVERLAP,
    TMNT_RELOC_UNRESOLVED,
    TMNT_RELOC_OVERFLOW,
    TMNT_RELOC_RESOLUTION
} TmntRelocationStatus;

/* Resolve every entry before writing; on any error the image is unchanged.
   The resolver returns 1 when a symbol exists, 0 when it does not, or -1 on an
   ambiguous or invalid lookup. A missing
   weak symbol resolves to zero. Writable bounds are [data_start, data_end).
   image_size is the mapped module extent before its key page. */
TmntRelocationStatus tmnt_relocate(uint8_t *image, size_t image_size,
                                    size_t data_start, size_t data_end,
                                    const TmntRelocation *entries, size_t count,
                                    TmntResolveSymbol resolve, void *context);
#endif
