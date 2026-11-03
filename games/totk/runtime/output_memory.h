/* Keep the virtual heap large enough for a resolution-expanded application pool.
   These are address-space reservations, not an eager physical-memory allocation. */
#pragma once
#include <stdint.h>
static inline uint64_t output_heap_capacity(uint64_t legacy_cap, uint64_t application_pool) {
    uint64_t rounded = (application_pool + 0x1fffffULL) & ~0x1fffffULL;
    return rounded > legacy_cap ? rounded : legacy_cap;
}
static inline uint64_t output_low_size(uint64_t legacy_size, uint64_t heap_offset, uint64_t heap_cap) {
    uint64_t needed = heap_offset + heap_cap;
    return needed > legacy_size ? needed : legacy_size;
}
