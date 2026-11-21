// NVN counter timestamps are 614.4 MHz ticks (384 ticks per 625 ns), not the CPU's 19.2 MHz system counter.
// Keep the guest clock's epoch in both directions. Wide products avoid overflow on long host uptimes.
#pragma once
#include <stdint.h>

static inline uint64_t nvn_timestamp_from_ns(uint64_t ns) { return (uint64_t)((__uint128_t)ns * 384 / 625); }
static inline uint64_t nvn_timestamp_to_ns(uint64_t ticks) { return (uint64_t)((__uint128_t)ticks * 625 / 384); }
