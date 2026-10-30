// TOTK: NVN physical / virtual memory pools backed by Darwin VM aliasing.
#pragma once
#include <stdint.h>
#include "nvn_obj.h"

// Runtime-verified layout of NVNmappingRequest (0x28 bytes, main nvnMemoryPoolMapVirtual call).
typedef struct {
    const NVNmemoryPool *physical_pool;
    int64_t physical_offset;
    int64_t virtual_offset;
    uint64_t size;
    int32_t storage_class;
    uint32_t padding;
} totk_mapping_request;
_Static_assert(sizeof(totk_mapping_request) == 0x28, "NVNmappingRequest stride");

#define TOTK_VPOOL_PAGE 0x10000ull   // NVN virtual mapping granularity (64 KiB)

// Called by MemoryPoolInitialize for PHYSICAL or VIRTUAL pools. Returns 0 with a logged reason on failure.
int totk_vpool_init(pool_t *pool);
// Called by MemoryPoolFinalize. Returns 1 when it released the pool's host storage.
int totk_vpool_fini(pool_t *pool);
// All requests are validated before any mapping changes; returns 0 and leaves mappings intact on failure.
int totk_vpool_map(pool_t *virtual_pool, int count, const totk_mapping_request *requests);
// TOTK diagnostics: CPU address of the physical backing of a virtual-pool range (1 = mapped, *cpu set).
int totk_vpool_backing(uintptr_t va, uint64_t len, uintptr_t *cpu);
