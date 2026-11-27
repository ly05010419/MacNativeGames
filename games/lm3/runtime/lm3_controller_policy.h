#pragma once
#include <stdint.h>

typedef struct { unsigned mask; unsigned count; uint32_t selected; } LM3ControllerChoice;
/* 1: a real selection is ready; 0: wait for controllers; -1: unsupported request.
   Single-player retains the existing keyboard-backed 1P. A multiplayer request
   counts physical devices only, so the keyboard cannot fabricate a second pad. */
static inline int lm3_controller_choose(int minimum, int maximum, unsigned allowed,
                                        unsigned physical, int single_mode, LM3ControllerChoice *out) {
    if (!out) return -1;
    *out = (LM3ControllerChoice){0};
    if (minimum < 0 || maximum < 1 || minimum > maximum || maximum > 8) return -1;
    if (single_mode) minimum = maximum = 1;
    if (minimum > 2) return -1;
    unsigned candidates = allowed & physical & 3u;
    if (maximum == 1 && (allowed & 1u)) candidates |= 1u;
    unsigned count = 0, mask = 0;
    for (unsigned i = 0; i < 2 && count < (unsigned)maximum; i++)
        if (candidates & (1u << i)) { mask |= 1u << i; count++; }
    if (!count || count < (unsigned)minimum) return 0;
    *out = (LM3ControllerChoice){.mask = mask, .count = count, .selected = mask & 1u ? 0u : 1u};
    return 1;
}
