#include "relocate.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static int compare_offsets(const void *left, const void *right) {
    uint64_t a = *(const uint64_t *)left, b = *(const uint64_t *)right;
    return (a > b) - (a < b);
}

static int add_signed(uintptr_t base, int64_t addend, uintptr_t *result) {
    if (addend >= 0) {
        uint64_t amount = (uint64_t)addend;
        if (amount > UINTPTR_MAX - base) return 0;
        *result = base + (uintptr_t)amount;
    } else {
        uint64_t amount = (uint64_t)(-(addend + 1)) + 1;
        if (amount > base) return 0;
        *result = base - (uintptr_t)amount;
    }
    return 1;
}

TmntRelocationStatus tmnt_relocate(uint8_t *image, size_t image_size,
                                    size_t data_start, size_t data_end,
                                    const TmntRelocation *entries, size_t count,
                                    TmntResolveSymbol resolve, void *context) {
    if (!image || data_start > data_end || data_end > image_size ||
        (count && !entries) || count > SIZE_MAX / sizeof(uint64_t))
        return TMNT_RELOC_ARGUMENT;
    if (!count) return TMNT_RELOC_OK;
    uint64_t *offsets = malloc(count * sizeof(*offsets));
    uint64_t *values = malloc(count * sizeof(*values));
    if (!offsets || !values) { free(offsets); free(values); return TMNT_RELOC_NO_MEMORY; }
    TmntRelocationStatus status = TMNT_RELOC_OK;
    for (size_t i = 0; i < count; i++) {
        const TmntRelocation *entry = &entries[i];
        if (entry->offset < data_start || entry->offset > data_end || data_end - entry->offset < 8) {
            status = TMNT_RELOC_BOUNDS; break;
        }
        uintptr_t base = 0, value = 0;
        if (entry->kind == 1027) {
            if (entry->addend < 0 || (uint64_t)entry->addend >= image_size) {
                status = TMNT_RELOC_BOUNDS; break;
            }
            base = (uintptr_t)image;
        } else if (entry->kind == 257 || entry->kind == 1025 || entry->kind == 1026) {
            if (entry->kind == 1026 && entry->addend != 0) { status = TMNT_RELOC_KIND; break; }
            int found = resolve ? resolve(context, entry->symbol, &base) : 0;
            if (found < 0 || found > 1) { status = TMNT_RELOC_RESOLUTION; break; }
            if (!found) {
                if (!entry->weak) { status = TMNT_RELOC_UNRESOLVED; break; }
                base = 0;
            }
        } else { status = TMNT_RELOC_KIND; break; }
        if (!add_signed(base, entry->addend, &value)) { status = TMNT_RELOC_OVERFLOW; break; }
        offsets[i] = entry->offset;
        values[i] = (uint64_t)value;
    }
    if (status == TMNT_RELOC_OK) {
        qsort(offsets, count, sizeof(*offsets), compare_offsets);
        for (size_t i = 1; i < count; i++) {
            if (offsets[i] - offsets[i - 1] < 8) { status = TMNT_RELOC_OVERLAP; break; }
        }
    }
    if (status == TMNT_RELOC_OK)
        for (size_t i = 0; i < count; i++) memcpy(image + entries[i].offset, &values[i], sizeof(values[i]));
    free(offsets); free(values);
    return status;
}
