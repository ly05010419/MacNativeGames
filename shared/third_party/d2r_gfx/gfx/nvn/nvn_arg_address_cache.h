#ifndef NVN_ARG_ADDRESS_CACHE_H
#define NVN_ARG_ADDRESS_CACHE_H

#include <stdint.h>
#include <string.h>

/* The Metal argument tables have 31 buffer slots, separately for vertex/fragment.
   This caches API state only: callers still resolve and retain all resources and
   materialize uniforms before supplying an address. Reset on every new encoder. */
typedef struct {
    uint64_t address[2][31];
    uint32_t valid[2];
} nvn_arg_address_cache;

static inline void nvn_arg_address_reset(nvn_arg_address_cache *c) {
    c->valid[0] = c->valid[1] = 0;
}

static inline int nvn_arg_address_repeated(nvn_arg_address_cache *c,
                                          unsigned stage, unsigned slot, uint64_t address) {
    if (stage >= 2 || slot >= 31) return 0;
    uint32_t bit = UINT32_C(1) << slot;
    if ((c->valid[stage] & bit) && c->address[stage][slot] == address) return 1;
    c->address[stage][slot] = address;
    c->valid[stage] |= bit;
    return 0;
}

#endif
