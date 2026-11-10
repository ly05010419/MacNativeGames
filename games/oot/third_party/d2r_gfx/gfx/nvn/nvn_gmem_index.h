#ifndef NVN_GMEM_INDEX_H
#define NVN_GMEM_INDEX_H
#include <stdint.h>
#include <stddef.h>

/* Exact acceleration of the sorted raw-address table, not another mapping.
 * Header words 5..7 contain bucket count and the first 64-bit bucket number.
 * After capacity entries, each 4 MiB bucket holds {first, step, last}. Both
 * ends are predecessor indices in the original disjoint range table. Gaps
 * still fail its original exclusive-end check. An unusually sparse address
 * space uses the original full search instead of allocating a huge index. */
#define GMEM_INDEX_SHIFT 22u
/* Guest pools and host allocations can be hundreds of GiB apart. Cover a
 * 1 TiB span with at most 3 MiB of metadata; larger sparse spans still use
 * the exact full-table search. */
#define GMEM_INDEX_MAX_BUCKETS 262144u
static inline uint32_t gmem_index_count(uint64_t first, uint64_t end) {
    if (!end || end <= first) return 0;
    uint64_t n = ((end - 1) >> GMEM_INDEX_SHIFT) - (first >> GMEM_INDEX_SHIFT) + 1;
    return n <= GMEM_INDEX_MAX_BUCKETS ? (uint32_t)n : 0;
}
static inline void gmem_index_build(uint64_t *table, uint32_t used, uint32_t capacity, uint32_t count) {
    uint32_t *header = (uint32_t *)table;
    header[5] = count;
    if (!count || used <= 1) return;
    uint64_t first_bucket = table[4] >> GMEM_INDEX_SHIFT;
    header[6] = (uint32_t)first_bucket; header[7] = (uint32_t)(first_bucket >> 32);
    uint32_t *buckets = (uint32_t *)(table + (size_t)capacity * 4);
    uint32_t first = 0, last = 0;
    for (uint32_t b = 0; b < count; ++b) {
        uint64_t begin = (first_bucket + b) << GMEM_INDEX_SHIFT;
        uint64_t end = begin | ((UINT64_C(1) << GMEM_INDEX_SHIFT) - 1);
        while (first + 1 < used && table[(size_t)(first + 1) * 4] <= begin) ++first;
        if (last < first) last = first;
        while (last + 1 < used && table[(size_t)(last + 1) * 4] <= end) ++last;
        uint32_t step = 1;
        while (step <= last - first) step <<= 1;
        buckets[(size_t)b * 3] = first;
        buckets[(size_t)b * 3 + 1] = step >> 1;
        buckets[(size_t)b * 3 + 2] = last;
    }
}
#endif
