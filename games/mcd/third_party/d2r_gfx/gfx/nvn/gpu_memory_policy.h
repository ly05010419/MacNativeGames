/* Mac unified-memory policy. MiB, not a reservation: leave room for guest RAM,
   CPU allocations and the OS. iOS uses its existing per-process pressure path. */
#pragma once
#include <stdint.h>
static inline uint64_t nvn_mac_gpu_budget(uint64_t physical, uint64_t recommended, uint64_t requested) {
    uint64_t cap = UINT64_C(12) << 30;
    uint64_t share = physical / 5 * 2;
    if (share < cap) cap = share;
    if (recommended && recommended < cap) cap = recommended;
    if (requested && requested < cap) cap = requested;
    return cap;
}
/* Start reclamation at 80%, target 70%. Byte count is a reclamation request,
   not a claim that the driver has already released the deferred resources. */
static inline uint64_t nvn_mac_gpu_reclaim(uint64_t allocated, uint64_t budget) {
    return budget && allocated > budget / 10 * 8 ? allocated - budget / 10 * 7 : 0;
}
