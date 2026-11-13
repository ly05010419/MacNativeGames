/* BOTW's heap must cover the application pool, without reserving unused GiB
   from the small address map of an iOS process without extended addressing. */
#pragma once
#include <stdint.h>

static inline uint64_t ios_botw_heap_capacity(uint64_t application_pool) {
    return (application_pool + 0x1fffffULL) & ~0x1fffffULL;
}

static inline uint64_t ios_botw190_alias_default_gib(uint64_t maximum_address) {
    const uint64_t horizon_window = 0x7000000000ULL;
    /* iPad mini 6 / iPadOS 27.2.1: only six GiB of this window are mappable.
       A larger alias leaves too little room for the host's Metal/ObjC objects.
       Keep the ordinary default when the limit is unknown or less restricted. */
    return maximum_address > horizon_window &&
           maximum_address - horizon_window <= (6ULL << 30) ? 1 : 16;
}
