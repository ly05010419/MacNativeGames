#pragma once
#include "runtime.h"

#define LM3_CONTROLLER_SUPPORT_CANCELED (202u | (3101u << 9))
typedef struct { int minimum, maximum, single_mode; } Lm3ControllerRequest;
/* SDK 7.3.2 sends a 0x21c-byte argument. Only this seven-byte header is
   inspected: signed min/max, then five boolean options; text/colors stay guest-owned. */
static inline int lm3_controller_request(const uint8_t *header, Lm3ControllerRequest *out) {
    if (!header || !out) return 0;
    int minimum = (int8_t)header[0], maximum = (int8_t)header[1];
    if (minimum < 0 || maximum < 1 || maximum > 8 || minimum > maximum) return 0;
    for (unsigned i = 2; i < 7; i++) if (header[i] > 1) return 0;
    int single = header[5] != 0;
    if (single) {
        minimum = maximum = 1;
    }
    *out = (Lm3ControllerRequest){minimum, maximum, single}; return 1;
}
/* One-player requests retain keyboard player 1. A multiplayer request counts
   actual physical full-key slots, so a keyboard does not impersonate a second pad. */
static inline unsigned lm3_controller_eligible(int maximum, unsigned allowed, unsigned physical) {
    return allowed & (physical | (maximum == 1 ? 1u : 0u)) & 3u;
}
static inline int lm3_controller_result_valid(const Lm3ControllerRequest *request, unsigned allowed,
                                             unsigned physical, const RuntimeControllerSupportResult *result) {
    unsigned mask = lm3_controller_eligible(request->maximum, allowed, physical);
    unsigned count = (mask & 1) + ((mask >> 1) & 1);
    return result->players >= 1 && result->players >= request->minimum &&
        result->players <= request->maximum && (unsigned)result->players <= count &&
        result->selected < 2 && (mask & (1u << result->selected));
}
