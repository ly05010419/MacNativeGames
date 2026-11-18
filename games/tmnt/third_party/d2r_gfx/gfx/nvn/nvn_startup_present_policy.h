#ifndef TMNT_NVN_STARTUP_PRESENT_POLICY_H
#define TMNT_NVN_STARTUP_PRESENT_POLICY_H

#include <stdint.h>

/* Only the first, as-yet-unwritten window image is withheld. A clear-only
   pass counts as GPU use, and later re-presents do not need another draw. */
static inline int nvn_startup_present_ready(uint64_t current_frame,
                                             int window_texture,
                                             int gpu_target_seen) {
    return current_frame || !window_texture || gpu_target_seen;
}

#endif
